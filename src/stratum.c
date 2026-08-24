/* Stratum V1 server: TCP listener + thread-per-connection.
 *
 * Wire format: newline-delimited JSON-RPC 2.0. Methods handled:
 *   mining.subscribe, mining.authorize, mining.submit
 *
 * Concurrency: an rwlock guards `current_job`. set_job swaps the pointer
 * under a write lock and pushes the previous job into a small ring of
 * "recent jobs" kept alive ~60s for late submits. Connection threads take
 * read locks for notify/submit lookups.
 *
 * Vardiff adjusts each connection's difficulty toward cfg.vardiff_target_spm
 * shares/minute, clamped so the share target never exceeds the network
 * target (see vardiff_maybe_retarget).
 */

#define _POSIX_C_SOURCE 200809L
#include "stratum.h"
#include "coinbase.h"
#include "share.h"
#include "log.h"
#include "thunder.h"
#include "cjson/cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE_BYTES 16384
#define DEDUPE_RING    1024
/* Process-wide ring, so it has to cover every live connection's recent
 * submissions across every server rather than just one's. */
#define SHARE_DEDUPE_RING 16384
/* The retention ring is sized in stratum.h, because main.c's payout-plan ring
 * must be able to cover every job that is still solvable. */
#define RECENT_JOBS    STRATUM_RECENT_JOBS
#define RECENT_JOB_TTL_MS 60000
/* Per-connection job -> issued-difficulty ring. Must cover every job stratum
 * will still accept a submit for: the recent ring plus the current job,
 * doubled for headroom. See stratum_conn.job_diff. */
#define JOB_DIFF_RING  ((STRATUM_RECENT_JOBS + 1) * 2)
/* Upper bound on a single blocking send to one miner. See conn_socket_setup. */
#define SEND_TIMEOUT_SEC 10

/* BIP320 reserved version-rolling bits (ASICBoost). Advertised in
 * mining.configure; only these block-header version bits may be rolled by a
 * miner, and a per-connection mask (this ANDed with the client's request) is
 * applied to every submitted version. */
#define VERSION_ROLLING_MASK 0x1fffe000u

/* ========================================================== shared ====== */

static uint64_t now_ms(void);   /* defined below; used to seed the counter */

/* State every stratum server in the process must draw from in common. The
 * rationale for each field — and why a per-server copy is a double-credit bug
 * once a second port exists — is on stratum_shared_t in stratum.h. */
struct stratum_shared {
    /* Seeded from the clock at construction (so values differ across
     * restarts) and incremented per subscribe, which is what makes each
     * connection's extranonce1 distinct. Do not mix it with the clock again
     * at use. */
    atomic_uint extranonce1_seq;

    /* Share dedupe, keyed on the resulting block-header hash. The
     * per-connection ring in stratum_conn cannot catch a duplicate that
     * arrives on a *different* connection, and two connections handed the
     * same extranonce1 render identical coinbases — so the same nonce yields
     * the same hash on both, and it would be credited twice. Keying on the
     * final hash makes the check independent of how the submission was framed
     * (job id, extranonce2, version rolling). */
    pthread_mutex_t dedupe_lock;
    uint64_t        dedupe[SHARE_DEDUPE_RING];
    size_t          dedupe_head;
};

stratum_shared_t *stratum_shared_new(void) {
    stratum_shared_t *sh = calloc(1, sizeof(*sh));
    if (!sh) return NULL;
    pthread_mutex_init(&sh->dedupe_lock, NULL);
    atomic_init(&sh->extranonce1_seq, (unsigned)now_ms());
    return sh;
}

void stratum_shared_free(stratum_shared_t *sh) {
    if (!sh) return;
    pthread_mutex_destroy(&sh->dedupe_lock);
    free(sh);
}

/* ============================================================== job ===== */

struct stratum_job {
    char     job_id[32];
    int32_t  version;
    uint8_t  prev_hash_le[32];

    /* Template-level inputs for per-connection coinbase rendering. */
    int64_t  value_sats;
    char    *wc_hex;          /* witness commitment hex, owned, may be NULL */
    /* Server-provided coinbase (BIP22 "coinbasetxn"), owned, may be NULL. When
     * set, the per-connection coinbase is built from this rather than from
     * scratch; coinbase_has_witness says whether to re-attach the witness
     * reserved value when assembling a found block. */
    char    *coinbasetxn_hex;
    int      coinbase_has_witness;
    size_t   en1_size;
    size_t   en2_size;

    uint8_t (*merkle_branches)[32];
    size_t   branch_count;
    uint32_t nbits;
    uint32_t ntime;
    uint8_t  network_target_be[32];
    uint32_t height;

    char   **tx_hex_list;   /* owned */
    size_t   tx_count;

    /* pool_mode=proportional: the PPLNS payout set for this template, shared
     * by every connection (unlike solo, where each connection's coinbase pays
     * its own miner). Attached by main.c via stratum_job_set_payouts after it
     * has queried the window. NULL/0 means no window was available, and the
     * render path falls back to paying the connection's own address. The
     * address strings are owned by the job. */
    coinbase_payout_t *payouts;
    size_t             n_payouts;

    uint64_t created_ms;    /* for retention ring */

    /* Reference count. find_job() hands a job to a submit handler that then
     * works with it for a long time — coinbase render, merkle fold, and on a
     * solve a full block assembly that walks every template transaction —
     * while the tip watcher is free to retire and free that same job (TTL
     * sweep or ring wrap). Borrowing the pointer under a lock and using it
     * after unlocking was a use-after-free during block assembly, i.e. at the
     * exact moment a block is found. Owners: the current_job slot, each
     * recent[] slot, and every outstanding find_job() caller. */
    atomic_uint refs;
};

static void job_destroy(stratum_job_t *j);

stratum_job_t *stratum_job_new(
    const char *job_id,
    int32_t version,
    const uint8_t prev_hash_le[32],
    int64_t value_sats,
    const char *witness_commitment_hex,
    size_t en1_size, size_t en2_size,
    const uint8_t (*merkle_branches)[32], size_t branch_count,
    uint32_t nbits, uint32_t ntime,
    const uint8_t network_target_be[32],
    uint32_t height,
    const char *const *tx_hex_list, size_t tx_count,
    const char *coinbasetxn_hex, int coinbase_has_witness)
{
    stratum_job_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    atomic_init(&j->refs, 1u);
    snprintf(j->job_id, sizeof(j->job_id), "%s", job_id ? job_id : "");
    j->version = version;
    if (prev_hash_le) memcpy(j->prev_hash_le, prev_hash_le, 32);
    j->value_sats = value_sats;
    j->en1_size   = en1_size;
    j->en2_size   = en2_size;
    j->coinbase_has_witness = coinbase_has_witness;
    if (witness_commitment_hex && *witness_commitment_hex) {
        j->wc_hex = strdup(witness_commitment_hex);
        if (!j->wc_hex) goto fail;
    }
    if (coinbasetxn_hex && *coinbasetxn_hex) {
        j->coinbasetxn_hex = strdup(coinbasetxn_hex);
        if (!j->coinbasetxn_hex) goto fail;
    }
    if (branch_count) {
        j->merkle_branches = calloc(branch_count, sizeof(*j->merkle_branches));
        if (!j->merkle_branches) goto fail;
        memcpy(j->merkle_branches, merkle_branches, branch_count * 32);
        j->branch_count = branch_count;
    }
    j->nbits = nbits;
    j->ntime = ntime;
    if (network_target_be) memcpy(j->network_target_be, network_target_be, 32);
    j->height = height;
    if (tx_count && tx_hex_list) {
        j->tx_hex_list = calloc(tx_count, sizeof(char *));
        if (!j->tx_hex_list) goto fail;
        for (size_t i = 0; i < tx_count; ++i) {
            j->tx_hex_list[i] = tx_hex_list[i] ? strdup(tx_hex_list[i]) : strdup("");
            if (!j->tx_hex_list[i]) goto fail;
        }
        j->tx_count = tx_count;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    j->created_ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    return j;
fail:
    job_destroy(j);
    return NULL;
}

/* Actually deallocate. Only stratum_job_free(), at the last reference. */
static void job_destroy(stratum_job_t *j) {
    if (!j) return;
    free(j->wc_hex);
    free(j->coinbasetxn_hex);
    free(j->merkle_branches);
    if (j->tx_hex_list) {
        for (size_t i = 0; i < j->tx_count; ++i) free(j->tx_hex_list[i]);
        free(j->tx_hex_list);
    }
    if (j->payouts) {
        for (size_t i = 0; i < j->n_payouts; ++i) free((char *)j->payouts[i].address);
        free(j->payouts);
    }
    free(j);
}

/* Take one reference. See the header: set_job consumes one, so publishing the
 * same job to a second server needs one taken first. */
stratum_job_t *stratum_job_ref(stratum_job_t *j) {
    if (j) atomic_fetch_add(&j->refs, 1u);
    return j;
}

/* Drop one reference; deallocate at zero. Named "free" because that is what it
 * is to every caller outside this file — a job handed back is a job released. */
void stratum_job_free(stratum_job_t *j) {
    if (!j) return;
    if (atomic_fetch_sub(&j->refs, 1u) == 1u) job_destroy(j);
}

int stratum_job_set_payouts(stratum_job_t *j,
                            const coinbase_payout_t *payouts,
                            size_t n_payouts)
{
    if (!j) return -1;
    /* Replace any previous set outright — a job is only ever populated once,
     * but making this idempotent keeps a retry from leaking. */
    if (j->payouts) {
        for (size_t i = 0; i < j->n_payouts; ++i) free((char *)j->payouts[i].address);
        free(j->payouts);
        j->payouts = NULL;
        j->n_payouts = 0;
    }
    if (!payouts || n_payouts == 0) return 0;

    coinbase_payout_t *copy = calloc(n_payouts, sizeof(*copy));
    if (!copy) return -1;
    for (size_t i = 0; i < n_payouts; ++i) {
        copy[i].address = strdup(payouts[i].address ? payouts[i].address : "");
        if (!copy[i].address) {
            for (size_t k = 0; k < i; ++k) free((char *)copy[k].address);
            free(copy);
            return -1;
        }
        copy[i].sats = payouts[i].sats;
    }
    j->payouts   = copy;
    j->n_payouts = n_payouts;
    return 0;
}

/* ============================================================ server ==== */

struct stratum_server {
    stratum_cfg_t cfg;

    /* Derived once from cfg.pool_mode so the render path does not string-compare
     * per share. Mirrors cfg.pps_enabled, which main.c derives the same way. */
    int prop_enabled;

    int  listen_fd;
    atomic_int  stop;
    atomic_int  conn_count;
    /* Extranonce1 counter and share dedupe, shared with every other server in
     * the process (see stratum_shared_t in stratum.h). Never per-server: two
     * servers each with their own would hand out colliding extranonce1 values
     * and be unable to detect the duplicate shares that result. `shared_owned`
     * records whether this server allocated it and must free it. */
    stratum_shared_t *shared;
    int               shared_owned;

    pthread_t   listener_thr;
    int         listener_started;

    pthread_rwlock_t job_lock;
    stratum_job_t   *current_job;          /* protected by job_lock */
    stratum_job_t   *recent[RECENT_JOBS];  /* small retention ring */
    size_t           recent_head;
    pthread_mutex_t  recent_lock;

    /* List of live connections — for broadcasting notify on job swap. */
    pthread_mutex_t  conns_lock;
    struct stratum_conn *conns_head;
};

struct stratum_conn {
    stratum_server_t *server;
    int fd;                  /* -1 in tests */
    pthread_t thr;
    int thr_started;

    /* Peer address, filled at accept(). Recorded so a miner's complaint can
     * be tied to the socket it actually came from: worker names are chosen by
     * the miner and several connections routinely share one, so the name
     * alone cannot answer "did their proxy ever reach us?". */
    char     peer_ip[INET_ADDRSTRLEN];

    uint8_t  extranonce1[STRATUM_EXTRANONCE1_SIZE];
    double   difficulty;
    int      subscribed;
    int      authorized;
    uint32_t version_mask;         /* negotiated version-rolling bits; 0 = off */
    char     worker_name[129];     /* full stratum username (sanitized) */
    char     payout_address[128];  /* validated bech32/base58 */

    /* Per-connection coinbase, rendered against the current job using
     * payout_address (miner) + cfg.operator_address (fee). Refreshed any
     * time we hand out a new notify for a job id we haven't rendered
     * coinbase for yet. */
    uint8_t *cb1;
    size_t   cb1_len;
    uint8_t *cb2;
    size_t   cb2_len;
    char     cb_for_job_id[32];

    /* Dedupe ring. Each entry is a small hash of
     * (job_id|en2|ntime|nonce|version). */
    uint64_t dedupe[DEDUPE_RING];
    size_t   dedupe_head;

    /* Vardiff window state — counts accepted shares since vd_window_start_ms.
     * Every cfg.vardiff_window_sec the rate is compared to vardiff_target_spm
     * and `difficulty` is multiplied/divided to converge on the target. */
    uint64_t vd_window_start_ms;
    uint32_t vd_window_shares;

    /* The difficulty this connection was on when each recent job was SENT to
     * it — i.e. the difficulty the miner actually mined that job at.
     *
     * A submit names the job it solved, and that job may be several retargets
     * old: the server keeps STRATUM_RECENT_JOBS solvable, which is minutes of
     * history, while a vardiff window is seconds. Judging such a submit at the
     * connection's CURRENT difficulty rejects work the miner performed
     * correctly at the difficulty we asked it for. Marketplaces report that as
     * the single most common way a pool integration fails: it passes the
     * extranonce check, then collapses the first time difficulty moves.
     *
     * Sized to cover every job that can still be solved (the recent ring plus
     * the current job), doubled for headroom — the same reasoning as
     * PROP_PLAN_RING in main.c, and for the same reason: a ring shorter than
     * what stratum will still accept a submit for silently mis-judges the
     * oldest one. */
    struct {
        char   job_id[32];
        double difficulty;
    } job_diff[JOB_DIFF_RING];
    size_t   job_diff_head;

    /* The pre-retarget difficulty, honored for a grace period after a
     * set_difficulty. Still the fallback when job_diff has no record for a
     * submit, and when a miner applies a set_difficulty on a LATER job than we
     * recorded it against — stratum does not pin down which job a
     * set_difficulty first applies to, so miners differ. A back-to-back
     * retarget overwrites this — only the latest old value is honored. */
    double   prev_difficulty;
    uint64_t diff_changed_ms;

    /* Monotonic timestamp of the most recent recv() that got any bytes.
     * The conn thread checks this against cfg.idle_timeout_sec after each
     * SO_RCVTIMEO wake-up so silent connections are reaped. */
    uint64_t last_activity_ms;

    /* Guards every mutable field of this connection that is touched by more
     * than one thread: cb1/cb2/cb_for_job_id and the vardiff set
     * (difficulty, prev_difficulty, diff_changed_ms, vd_window_*).
     *
     * Two threads reach them. The connection's own thread does, via
     * handle_submit. The TIP WATCHER also does, because stratum_server_set_job
     * walks every connection and calls conn_render_coinbase (which frees and
     * replaces cb1/cb2) and vardiff_check_idle. Without this lock the watcher
     * frees the coinbase buffers out from under a submit that is memcpy'ing
     * them — a use-after-free that fires on any job switch racing a share.
     *
     * Distinct from write_lock, which serialises the socket only. Lock order
     * is conns_lock -> state_lock -> job_lock; never the reverse. */
    pthread_mutex_t state_lock;

    pthread_mutex_t write_lock;

    struct stratum_conn *next;  /* server->conns_head linked list */
};

static void conn_clear_coinbase(stratum_conn_t *c) {
    free(c->cb1); c->cb1 = NULL; c->cb1_len = 0;
    free(c->cb2); c->cb2 = NULL; c->cb2_len = 0;
    c->cb_for_job_id[0] = '\0';
}

/* ----------------------------------------------------- helpers ---------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Decode hex into out (exactly outlen bytes). Returns 0 on success. */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t outlen) {
    if (!hex) return -1;
    size_t hl = strlen(hex);
    if (hl != outlen * 2) return -1;
    for (size_t i = 0; i < outlen; ++i) {
        int hi = hex_nib(hex[2 * i]);
        int lo = hex_nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Decode an arbitrary-length hex string. Returns malloc'd buffer, *outlen
 * set, or NULL on error. */
static uint8_t *hex_to_bytes_alloc(const char *hex, size_t *outlen) {
    if (!hex) return NULL;
    size_t hl = strlen(hex);
    if (hl % 2) return NULL;
    size_t n = hl / 2;
    uint8_t *out = malloc(n ? n : 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; ++i) {
        int hi = hex_nib(hex[2 * i]);
        int lo = hex_nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) { free(out); return NULL; }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *outlen = n;
    return out;
}

static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = H[(bytes[i] >> 4) & 0xf];
        out[2 * i + 1] = H[bytes[i] & 0xf];
    }
    out[2 * n] = '\0';
}

/* Parse a hex u32, big-endian semantics: e.g. "5f5e1000" -> 0x5f5e1000. */
static int parse_u32_hex(const char *hex, uint32_t *out) {
    uint8_t b[4];
    if (hex_to_bytes(hex, b, 4) != 0) return -1;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
    return 0;
}

static void sanitize_worker(const char *in, char *out, size_t outlen) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < outlen; ++i) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t fnv1a_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- output buffer helpers ---- */

static int buf_append(char **buf, size_t *len, const char *s, size_t n) {
    char *nb = realloc(*buf, *len + n + 1);
    if (!nb) return -1;
    *buf = nb;
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int buf_append_json_line(char **buf, size_t *len, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return -1;
    int rc = buf_append(buf, len, s, strlen(s));
    if (rc == 0) rc = buf_append(buf, len, "\n", 1);
    free(s);
    return rc;
}

/* ---- job retention ring ---- */

static void retire_job(stratum_server_t *s, stratum_job_t *j) {
    if (!j) return;
    pthread_mutex_lock(&s->recent_lock);
    /* Sweep expired. */
    uint64_t cutoff = mono_ms();
    for (size_t i = 0; i < RECENT_JOBS; ++i) {
        if (s->recent[i] && cutoff - s->recent[i]->created_ms > RECENT_JOB_TTL_MS) {
            stratum_job_free(s->recent[i]);
            s->recent[i] = NULL;
        }
    }
    /* Free whatever is in the slot we are about to overwrite. */
    if (s->recent[s->recent_head]) {
        stratum_job_free(s->recent[s->recent_head]);
    }
    s->recent[s->recent_head] = j;
    s->recent_head = (s->recent_head + 1) % RECENT_JOBS;
    pthread_mutex_unlock(&s->recent_lock);
}

/* Find a job by id under read lock (current) or recent ring. Returned
 * pointer is borrowed — only valid while caller holds appropriate locks
 * (current_job: rdlock; recent: recent_lock). For simplicity we return
 * a reference that is safe so long as set_job hasn't replaced it; in this
 * design submit handlers complete quickly and shares for retired jobs are
 * rare. */
static stratum_job_t *find_job(stratum_server_t *s, const char *job_id) {
    if (!job_id) return NULL;
    /* current */
    pthread_rwlock_rdlock(&s->job_lock);
    stratum_job_t *cur = s->current_job;
    if (cur && strcmp(cur->job_id, job_id) == 0) {
        atomic_fetch_add(&cur->refs, 1u);   /* before unlocking, always */
        pthread_rwlock_unlock(&s->job_lock);
        return cur;
    }
    pthread_rwlock_unlock(&s->job_lock);
    /* recent */
    pthread_mutex_lock(&s->recent_lock);
    for (size_t i = 0; i < RECENT_JOBS; ++i) {
        if (s->recent[i] && strcmp(s->recent[i]->job_id, job_id) == 0) {
            stratum_job_t *r = s->recent[i];
            atomic_fetch_add(&r->refs, 1u);
            pthread_mutex_unlock(&s->recent_lock);
            return r;
        }
    }
    pthread_mutex_unlock(&s->recent_lock);
    return NULL;
}

/* ---- notify payload ---- */

/* Build a mining.notify params array. cb1/cb2 are supplied separately
 * because they are rendered per-connection (each miner's coinbase pays
 * that miner's payout_address). */
static cJSON *make_notify_params(const stratum_job_t *j,
                                 const uint8_t *cb1, size_t cb1_len,
                                 const uint8_t *cb2, size_t cb2_len,
                                 int clean_jobs) {
    cJSON *p = cJSON_CreateArray();
    cJSON_AddItemToArray(p, cJSON_CreateString(j->job_id));

    /* prev_hash: mining.notify uses the stratum convention where the 32-byte
     * hash is sent with each 4-byte word byte-reversed (word order preserved).
     * prev_hash_le holds the header-internal little-endian bytes, so we
     * word-swap before emitting; the miner word-swaps again to recover the
     * exact bytes that go into the block header. Sending the raw little-endian
     * bytes makes standard ASICs hash the wrong header (every share rejected
     * as "low difficulty"). */
    char hex[65];
    uint8_t prev_ws[32];
    for (int wi = 0; wi < 8; ++wi)
        for (int bi = 0; bi < 4; ++bi)
            prev_ws[wi * 4 + bi] = j->prev_hash_le[wi * 4 + 3 - bi];
    bytes_to_hex(prev_ws, 32, hex);
    cJSON_AddItemToArray(p, cJSON_CreateString(hex));

    char *cb1_hex = malloc(cb1_len * 2 + 1);
    char *cb2_hex = malloc(cb2_len * 2 + 1);
    if (!cb1_hex || !cb2_hex) {
        free(cb1_hex); free(cb2_hex); cJSON_Delete(p); return NULL;
    }
    bytes_to_hex(cb1, cb1_len, cb1_hex);
    bytes_to_hex(cb2, cb2_len, cb2_hex);
    cJSON_AddItemToArray(p, cJSON_CreateString(cb1_hex));
    cJSON_AddItemToArray(p, cJSON_CreateString(cb2_hex));
    free(cb1_hex); free(cb2_hex);

    cJSON *branches = cJSON_CreateArray();
    for (size_t i = 0; i < j->branch_count; ++i) {
        bytes_to_hex(j->merkle_branches[i], 32, hex);
        cJSON_AddItemToArray(branches, cJSON_CreateString(hex));
    }
    cJSON_AddItemToArray(p, branches);

    char vhex[9], thex[9], nhex[9];
    snprintf(vhex, sizeof(vhex), "%08x", (uint32_t)j->version);
    snprintf(thex, sizeof(thex), "%08x", j->nbits);
    snprintf(nhex, sizeof(nhex), "%08x", j->ntime);
    cJSON_AddItemToArray(p, cJSON_CreateString(vhex));
    cJSON_AddItemToArray(p, cJSON_CreateString(thex));
    cJSON_AddItemToArray(p, cJSON_CreateString(nhex));
    cJSON_AddItemToArray(p, cJSON_CreateBool(clean_jobs ? 1 : 0));
    return p;
}

/* Render a fresh coinbase for `c` against `job` using c->payout_address
 * and the server's operator_address / fee_bps / coinbase_tag. Caches into
 * c->cb1/cb2 keyed by job->job_id. Returns 0 ok, negative on error. */
/* CALLER MUST HOLD c->state_lock: this frees and replaces c->cb1/c->cb2, and
 * both the connection's own thread and the tip watcher get here. */
static int conn_render_coinbase(stratum_server_t *s, stratum_conn_t *c,
                                const stratum_job_t *job) {
    if (!c->authorized || c->payout_address[0] == '\0') return -1;
    if (c->cb_for_job_id[0] && strcmp(c->cb_for_job_id, job->job_id) == 0) {
        return 0; /* cached */
    }
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int rc;
    if (s->cfg.pps_enabled) {
        /* PPS-classic: every miner's coinbase is identical, paying the
         * pool's BTC wallet for the net-of-fee reward and the operator
         * address for the fee. The operator later moves accumulated BTC
         * into Thunder via the admin dashboard's deposit action; per-miner
         * accounting happens off-chain via pps_credits. */
        if (job->coinbasetxn_hex) {
            rc = coinbase_build_from_template(job->coinbasetxn_hex,
                                              s->cfg.pool_btc_address,
                                              s->cfg.operator_address, s->cfg.fee_bps,
                                              s->cfg.coinbase_tag,
                                              job->en1_size, job->en2_size,
                                              &parts, NULL, NULL, NULL, err, sizeof err);
        } else {
            rc = coinbase_build_split(job->height, job->value_sats,
                                      s->cfg.pool_btc_address,
                                      s->cfg.operator_address, s->cfg.fee_bps,
                                      job->wc_hex, s->cfg.coinbase_tag,
                                      job->en1_size, job->en2_size,
                                      &parts, NULL, NULL, err, sizeof err);
        }
    } else if (s->prop_enabled && job->payouts && job->n_payouts > 0 &&
               job->coinbasetxn_hex) {
        /* pool_mode=proportional: one coinbase per template, shared by every
         * connection, paying the PPLNS window's shareholders directly. Sessions
         * differ only by extranonce1, so this render is identical for all of
         * them — the per-connection cache below still holds because it is keyed
         * on job_id.
         *
         * main.c has already checked that sum(payouts) + fee == the template's
         * reward; the builder re-checks and refuses rather than emitting a
         * coinbase that pays the wrong total. */
        rc = coinbase_build_from_template_multi(job->coinbasetxn_hex,
                                          job->payouts, job->n_payouts,
                                          s->cfg.operator_address, s->cfg.fee_bps,
                                          s->cfg.coinbase_tag,
                                          job->en1_size, job->en2_size,
                                          &parts, NULL, NULL, NULL, err, sizeof err);
    } else if (job->coinbasetxn_hex) {
        /* Backend dictated the coinbase (e.g. CUSF enforcer): build from it,
         * redirecting the reward output to this miner and preserving the
         * mandatory commitment outputs. The witness commitment is already in
         * the server's coinbase, so job->wc_hex is not used here.
         *
         * In proportional mode this is also the fallback when no PPLNS window
         * exists yet (first block, or an empty shares table): paying the finder
         * directly is correct and non-custodial, just not yet proportional. */
        rc = coinbase_build_from_template(job->coinbasetxn_hex,
                                          c->payout_address,
                                          s->cfg.operator_address, s->cfg.fee_bps,
                                          s->cfg.coinbase_tag,
                                          job->en1_size, job->en2_size,
                                          &parts, NULL, NULL, NULL, err, sizeof err);
    } else {
        rc = coinbase_build_split(job->height, job->value_sats,
                                  c->payout_address,
                                  s->cfg.operator_address, s->cfg.fee_bps,
                                  job->wc_hex, s->cfg.coinbase_tag,
                                  job->en1_size, job->en2_size,
                                  &parts, NULL, NULL, err, sizeof err);
    }
    if (rc < 0) {
        LOG_WARN("stratum: coinbase render failed for %s: %s",
                 c->worker_name, err);
        return -1;
    }
    free(c->cb1); free(c->cb2);
    c->cb1 = parts.cb1; c->cb1_len = parts.cb1_len;
    c->cb2 = parts.cb2; c->cb2_len = parts.cb2_len;
    snprintf(c->cb_for_job_id, sizeof c->cb_for_job_id, "%s", job->job_id);
    return 0;
}

static int emit_notification(char **buf, size_t *len, const char *method, cJSON *params) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, "id", cJSON_CreateNull());
    cJSON_AddStringToObject(obj, "method", method);
    cJSON_AddItemToObject(obj, "params", params);
    int rc = buf_append_json_line(buf, len, obj);
    cJSON_Delete(obj);
    return rc;
}

static int emit_response(char **buf, size_t *len, cJSON *id, cJSON *result, cJSON *err) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(obj, "result", result ? result : cJSON_CreateNull());
    cJSON_AddItemToObject(obj, "error",  err    ? err    : cJSON_CreateNull());
    int rc = buf_append_json_line(buf, len, obj);
    cJSON_Delete(obj);
    return rc;
}

static cJSON *make_error(int code, const char *msg) {
    cJSON *e = cJSON_CreateArray();
    cJSON_AddItemToArray(e, cJSON_CreateNumber(code));
    cJSON_AddItemToArray(e, cJSON_CreateString(msg));
    cJSON_AddItemToArray(e, cJSON_CreateNull());
    return e;
}

/* ---- varint for block assembly ---- */

/* Return 0 on success, -1 on allocation failure. These used to return void and
 * silently do nothing when realloc failed, which meant a block could be
 * assembled SHORT and submitted — the node rejects it and the reward is gone,
 * with nothing in the log to explain why. Callers must check. */
static int varint_append(uint8_t **buf, size_t *cap, size_t *len, uint64_t n) {
    /* ensure 9 bytes */
    if (*len + 9 > *cap) {
        size_t nc = (*cap ? *cap * 2 : 64);
        while (nc < *len + 9) nc *= 2;
        uint8_t *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb; *cap = nc;
    }
    uint8_t *p = *buf + *len;
    if (n < 0xfd) { p[0] = (uint8_t)n; *len += 1; return 0; }
    if (n <= 0xffff) {
        p[0] = 0xfd; p[1] = (uint8_t)(n & 0xff); p[2] = (uint8_t)((n >> 8) & 0xff);
        *len += 3; return 0;
    }
    if (n <= 0xffffffffULL) {
        p[0] = 0xfe;
        for (int i = 0; i < 4; ++i) p[1 + i] = (uint8_t)((n >> (8 * i)) & 0xff);
        *len += 5; return 0;
    }
    p[0] = 0xff;
    for (int i = 0; i < 8; ++i) p[1 + i] = (uint8_t)((n >> (8 * i)) & 0xff);
    *len += 9;
    return 0;
}

static int bytes_append(uint8_t **buf, size_t *cap, size_t *len, const uint8_t *src, size_t n) {
    if (*len + n > *cap) {
        size_t nc = (*cap ? *cap * 2 : 64);
        while (nc < *len + n) nc *= 2;
        uint8_t *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return 0;
}

/* ---- core message handler --------------------------------------------- */

static void send_set_difficulty(char **buf, size_t *len, double diff) {
    cJSON *p = cJSON_CreateArray();
    cJSON_AddItemToArray(p, cJSON_CreateNumber(diff));
    emit_notification(buf, len, "mining.set_difficulty", p);
}

/* Network difficulty of the current job, or 0 when no job is set. */
static double current_net_diff(stratum_server_t *s) {
    double d = 0.0;
    pthread_rwlock_rdlock(&s->job_lock);
    if (s->current_job) d = target_to_diff(s->current_job->network_target_be);
    pthread_rwlock_unlock(&s->job_lock);
    return d;
}

/* How long shares at the pre-retarget difficulty stay acceptable. The miner
 * applies a set_difficulty on a later job notify, which on a slow chain can
 * lag well past the vardiff window. */
static uint64_t diff_grace_ms(const stratum_server_t *s) {
    uint64_t g = (uint64_t)s->cfg.vardiff_window_sec * 2000ULL;
    return g > 60000 ? g : 60000;
}

/* Vardiff: every cfg.vardiff_window_sec, look at how many shares the
 * connection submitted in that window and rescale its difficulty so the
 * rate converges on cfg.vardiff_target_spm shares/minute. Called from
 * handle_submit() after each accepted share.
 *
 * Conservative algorithm:
 *   ratio = observed_spm / target_spm
 *   if  ratio in [0.5, 2.0] → leave it (avoid jitter)
 *   else                    → new_diff = old_diff * ratio, clamped
 * Always emits a single mining.set_difficulty when diff changes. The
 * client picks it up for the next job notify; we don't force a re-notify
 * because handle_submit keeps accepting shares at the old difficulty for
 * a grace period (diff_grace_ms). */
/* CALLER MUST HOLD c->state_lock. */
static void vardiff_maybe_retarget(stratum_server_t *s, stratum_conn_t *c,
                                   uint64_t now,
                                   char **buf, size_t *len)
{
    if (!s->cfg.vardiff_enabled) return;
    if (c->vd_window_start_ms == 0) {
        c->vd_window_start_ms = now;
        c->vd_window_shares = 0;
        return;
    }
    uint64_t elapsed_ms = now - c->vd_window_start_ms;
    uint64_t window_ms  = (uint64_t)s->cfg.vardiff_window_sec * 1000ULL;
    if (elapsed_ms < window_ms) return;

    /* Observed shares per minute over this window. */
    double observed_spm = ((double)c->vd_window_shares * 60000.0) /
                          (double)elapsed_ms;
    double target_spm = s->cfg.vardiff_target_spm;
    double ratio = observed_spm / target_spm;

    double old_diff = c->difficulty;
    double new_diff = old_diff;
    if (ratio > 2.0 || ratio < 0.5) {
        new_diff = old_diff * ratio;
        /* Cap each adjustment to a 4x step to avoid wild swings on small
         * windows. */
        if (new_diff > old_diff * 4.0) new_diff = old_diff * 4.0;
        if (new_diff < old_diff / 4.0) new_diff = old_diff / 4.0;
        if (new_diff < s->cfg.vardiff_min) new_diff = s->cfg.vardiff_min;
        if (new_diff > s->cfg.vardiff_max) new_diff = s->cfg.vardiff_max;
        /* Never raise the share difficulty above the network difficulty:
         * the miner discards hashes above the stratum target locally, so a
         * share target harder than the network target throws away valid
         * blocks before the pool ever sees them. This clamp wins over
         * vardiff_min/max — it bites on low-difficulty networks where an
         * ASIC's vardiff otherwise climbs orders of magnitude past the
         * chain difficulty. */
        double net_diff = current_net_diff(s);
        if (net_diff > 0.0 && new_diff > net_diff) new_diff = net_diff;
    }

    /* Reset the window regardless of whether we changed diff. */
    c->vd_window_start_ms = now;
    c->vd_window_shares = 0;

    if (new_diff != old_diff) {
        c->difficulty = new_diff;
        c->prev_difficulty = old_diff;
        c->diff_changed_ms = now;
        LOG_INFO("stratum: vardiff %s: %.0f -> %.0f (%.1f spm observed, %.1f target)",
                 c->worker_name, old_diff, new_diff, observed_spm, target_spm);
        send_set_difficulty(buf, len, new_diff);
    }
}

/* Send mining.notify for the current job to a specific connection, using
 * that connection's rendered coinbase. Skips silently if the conn is not
 * yet authorized (we have no payout address to render against). */
/* Give vardiff a chance to act on a connection that is NOT submitting.
 *
 * vardiff_maybe_retarget only runs after an accepted share, so a miner whose
 * difficulty is set too high for its hashrate submits nothing, never
 * retargets, and stays stuck there forever. That is reachable now that
 * authorize can seed a difficulty from history: hardware gets swapped behind
 * the same worker name. Called on every job broadcast, so an idle connection
 * ratchets down 4x per vardiff window until it can produce shares again. */
static void vardiff_check_idle(stratum_server_t *s, stratum_conn_t *c,
                               char **buf, size_t *len) {
    if (!s->cfg.vardiff_enabled || !c->authorized) return;
    if (c->vd_window_shares > 0) return;      /* it is submitting; leave it */
    vardiff_maybe_retarget(s, c, now_ms(), buf, len);
}

/* Record the difficulty this connection is on as we hand it `job_id`.
 *
 * This is the difficulty the miner will mine that job at, which is what a
 * later submit naming that job has to be judged against. Re-recording an
 * existing job_id updates it in place rather than consuming a fresh slot, so a
 * re-notify of the same job (a reconnect, or a periodic refresh) cannot push
 * older still-solvable jobs out of the ring.
 *
 * CALLER MUST HOLD c->state_lock. */
static void conn_record_job_diff(stratum_conn_t *c, const char *job_id,
                                 double diff) {
    if (!job_id || !*job_id) return;
    for (size_t i = 0; i < JOB_DIFF_RING; ++i) {
        if (strcmp(c->job_diff[i].job_id, job_id) == 0) {
            c->job_diff[i].difficulty = diff;
            return;
        }
    }
    size_t slot = c->job_diff_head;
    snprintf(c->job_diff[slot].job_id, sizeof c->job_diff[slot].job_id,
             "%s", job_id);
    c->job_diff[slot].difficulty = diff;
    c->job_diff_head = (slot + 1) % JOB_DIFF_RING;
}

/* Difficulty `job_id` was issued to this connection at. Returns 1 and fills
 * *out when known, 0 when this connection was never sent that job (or the ring
 * has since wrapped past it) — in which case the caller falls back to the
 * current-difficulty-plus-grace behaviour.
 *
 * CALLER MUST HOLD c->state_lock. */
static int conn_job_diff_lookup(const stratum_conn_t *c, const char *job_id,
                                double *out) {
    if (!job_id || !*job_id) return 0;
    for (size_t i = 0; i < JOB_DIFF_RING; ++i) {
        if (c->job_diff[i].job_id[0] &&
            strcmp(c->job_diff[i].job_id, job_id) == 0) {
            if (c->job_diff[i].difficulty <= 0.0) return 0;
            *out = c->job_diff[i].difficulty;
            return 1;
        }
    }
    return 0;
}

static void send_current_notify(stratum_server_t *s, stratum_conn_t *c,
                                char **buf, size_t *len, int clean) {
    pthread_rwlock_rdlock(&s->job_lock);
    stratum_job_t *cur = s->current_job;
    if (cur && conn_render_coinbase(s, c, cur) == 0) {
        cJSON *p = make_notify_params(cur, c->cb1, c->cb1_len,
                                      c->cb2, c->cb2_len, clean);
        if (p) {
            emit_notification(buf, len, "mining.notify", p);
            /* Only once the notify is actually going out. Both call sites hold
             * state_lock, and both emit any pending mining.set_difficulty
             * BEFORE this, so c->difficulty here is the value the miner has
             * when it starts on this job. */
            conn_record_job_diff(c, cur->job_id, c->difficulty);
        }
    }
    pthread_rwlock_unlock(&s->job_lock);
}

static int handle_subscribe(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                            char **buf, size_t *len) {
    /* Take extranonce1 straight from the server counter, which is seeded
     * from the clock at startup and incremented once per subscribe.
     *
     * It must be unique across every live connection: identical extranonce1
     * means identical coinbases, so two connections mine the same header and
     * find the same hash from the same nonce — wasting half their hashrate
     * and double-crediting the share. XOR-ing the counter with the clock
     * again here (the previous approach) destroyed that guarantee: it
     * collides whenever the delta in the clock equals the delta in the
     * counter, e.g. an even seq at an even millisecond and the next seq one
     * millisecond later both yield the same value. A miner opening several
     * connections at once hits that case routinely. */
    /* The packing below writes exactly four bytes out of a uint32_t. Widening
     * extranonce1 means rewriting it, so fail the build rather than silently
     * leaving the high bytes of a wider field uninitialised. */
    _Static_assert(STRATUM_EXTRANONCE1_SIZE == 4,
                   "extranonce1 packing below assumes a 4-byte field");
    unsigned seq = atomic_fetch_add(&s->shared->extranonce1_seq, 1);
    uint32_t mix = (uint32_t)seq;
    c->extranonce1[0] = (uint8_t)(mix >> 24);
    c->extranonce1[1] = (uint8_t)(mix >> 16);
    c->extranonce1[2] = (uint8_t)(mix >> 8);
    c->extranonce1[3] = (uint8_t)mix;
    c->subscribed = 1;

    char ex1_hex[STRATUM_EXTRANONCE1_SIZE * 2 + 1];
    bytes_to_hex(c->extranonce1, STRATUM_EXTRANONCE1_SIZE, ex1_hex);

    cJSON *result = cJSON_CreateArray();
    cJSON *subs = cJSON_CreateArray();
    cJSON *sd = cJSON_CreateArray();
    cJSON_AddItemToArray(sd, cJSON_CreateString("mining.set_difficulty"));
    cJSON_AddItemToArray(sd, cJSON_CreateString("sd"));
    cJSON_AddItemToArray(subs, sd);
    cJSON *sn = cJSON_CreateArray();
    cJSON_AddItemToArray(sn, cJSON_CreateString("mining.notify"));
    cJSON_AddItemToArray(sn, cJSON_CreateString("sn"));
    cJSON_AddItemToArray(subs, sn);
    cJSON_AddItemToArray(result, subs);
    cJSON_AddItemToArray(result, cJSON_CreateString(ex1_hex));
    cJSON_AddItemToArray(result, cJSON_CreateNumber(STRATUM_EXTRANONCE2_SIZE));

    return emit_response(buf, len, id, result, NULL);
}

/* mining.configure (BIP310). Only the version-rolling extension is supported.
 * params = [ [extension names...], { extension parameters... } ]. We negotiate
 * the version-rolling mask as (client mask AND our BIP320 mask) and report it
 * back; other requested extensions are silently left unacknowledged. */
static int handle_configure(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                            cJSON *params, char **buf, size_t *len) {
    (void)s;
    cJSON *exts = NULL, *args = NULL;
    if (cJSON_IsArray(params)) {
        exts = cJSON_GetArrayItem(params, 0);
        args = cJSON_GetArrayItem(params, 1);
    }

    int wants_vr = 0;
    if (cJSON_IsArray(exts)) {
        int n = cJSON_GetArraySize(exts);
        for (int i = 0; i < n; ++i) {
            cJSON *e = cJSON_GetArrayItem(exts, i);
            if (cJSON_IsString(e) &&
                strcmp(e->valuestring, "version-rolling") == 0) {
                wants_vr = 1;
            }
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (wants_vr) {
        /* Client mask defaults to "roll everything" when omitted; we clamp it
         * to the bits we actually allow. */
        uint32_t client_mask = 0xffffffffu;
        if (cJSON_IsObject(args)) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(args,
                                                        "version-rolling.mask");
            uint32_t parsed;
            if (cJSON_IsString(m) && parse_u32_hex(m->valuestring, &parsed) == 0) {
                client_mask = parsed;
            }
        }
        c->version_mask = client_mask & VERSION_ROLLING_MASK;

        char mask_hex[9];
        snprintf(mask_hex, sizeof mask_hex, "%08x", c->version_mask);
        cJSON_AddItemToObject(result, "version-rolling", cJSON_CreateTrue());
        cJSON_AddStringToObject(result, "version-rolling.mask", mask_hex);
        LOG_INFO("stratum: version-rolling negotiated, mask=%s", mask_hex);
    }

    return emit_response(buf, len, id, result, NULL);
}

static int handle_authorize(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                            cJSON *params, char **buf, size_t *len) {
    const char *worker = NULL;
    if (cJSON_IsArray(params) && cJSON_GetArraySize(params) >= 1) {
        cJSON *w = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsString(w)) worker = w->valuestring;
    }
    if (!worker) {
        cJSON *err = make_error(24, "missing worker name");
        return emit_response(buf, len, id, NULL, err);
    }

    /* Username format: <address>[.<rig_label>]. The address part must be
     * a valid bech32 (P2WPKH) or base58check (P2PKH / P2SH) Bitcoin
     * address; the optional label is a free-form rig identifier. */
    const char *dot = strchr(worker, '.');
    size_t addr_len = dot ? (size_t)(dot - worker) : strlen(worker);
    if (addr_len == 0 || addr_len >= sizeof(c->payout_address)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, worker, now_ms(),
                             "stratum username must start with a bitcoin address");
        }
        cJSON *err = make_error(24,
            "stratum username must be <bitcoin_address>[.<rig_label>]");
        return emit_response(buf, len, id, NULL, err);
    }
    memcpy(c->payout_address, worker, addr_len);
    c->payout_address[addr_len] = '\0';

    char    derr[128] = {0};
    if (s->cfg.pps_enabled) {
        /* Thunder address: 20-byte hash160 in plain base58. The
         * 's<n>_<base58>_<hex6>' deposit-format wrapper is rejected (see
         * thunder.c). We don't need the decoded bytes here — the coinbase
         * pays the pool's BTC wallet, not the miner — but we validate so a
         * typo'd username can't accrue unpayable PPS. */
        uint8_t th[20];
        if (thunder_address_decode(c->payout_address, th, derr, sizeof derr) < 0) {
            if (s->cfg.on_reject) {
                char rmsg[192];
                snprintf(rmsg, sizeof rmsg, "invalid thunder address: %s", derr);
                s->cfg.on_reject(s->cfg.ctx, worker, now_ms(), rmsg);
            }
            c->payout_address[0] = '\0';
            char emsg[192];
            snprintf(emsg, sizeof emsg,
                     "invalid thunder address in stratum username: %s", derr);
            cJSON *err = make_error(24, emsg);
            return emit_response(buf, len, id, NULL, err);
        }
    } else {
        uint8_t spk[64];
        size_t  spk_len = 0;
        if (coinbase_address_to_script(c->payout_address, spk, sizeof spk,
                                       &spk_len, derr, sizeof derr) < 0) {
            if (s->cfg.on_reject) {
                char rmsg[192];
                snprintf(rmsg, sizeof rmsg, "invalid payout address: %s", derr);
                s->cfg.on_reject(s->cfg.ctx, worker, now_ms(), rmsg);
            }
            c->payout_address[0] = '\0';
            char emsg[192];
            snprintf(emsg, sizeof emsg,
                     "invalid payout address in stratum username: %s", derr);
            cJSON *err = make_error(24, emsg);
            return emit_response(buf, len, id, NULL, err);
        }
    }

    sanitize_worker(worker, c->worker_name, sizeof(c->worker_name));
    c->authorized = 1;
    /* Prefer what this worker was actually running at. A reconnect or a pool
     * restart otherwise drops it to initial_diff and makes vardiff climb again
     * at 4x per window — minutes of flooding and shed shares for a multi-TH/s
     * miner that was already converged.
     *
     * ⚠️ Do NOT gate this on `c->difficulty <= 0`: the connection constructor
     * already assigns initial_diff, so that test never holds and the hint is
     * silently never consulted. That shipped once and did nothing. */
    double hint = 0.0;
    if (s->cfg.on_difficulty_hint) {
        hint = s->cfg.on_difficulty_hint(s->cfg.ctx, c->worker_name);
        /* Floor the REPLAYED difficulty at vardiff_min. The hint comes from
         * this worker's own history, which is keyed on worker name and knows
         * nothing about which port it was earned on — so a miner that mined
         * the public port at difficulty 1 and then points a rented fleet at
         * the rental port would be seeded at 1 there, under the marketplace's
         * hard minimum, and the order fails for invalid shares.
         *
         * Deliberately NOT applied to cfg.initial_diff: a server whose
         * initial_diff sits below its vardiff_min is a valid configuration
         * (it starts easy and lets the first retarget lift it), and the
         * rental port sets initial_diff to the floor explicitly anyway. */
        if (hint > 0.0 && s->cfg.vardiff_min > 0.0 &&
            hint < s->cfg.vardiff_min) {
            LOG_INFO("stratum: %s hint %.0f is below the vardiff floor %.0f — "
                     "starting at the floor",
                     c->worker_name, hint, s->cfg.vardiff_min);
            hint = s->cfg.vardiff_min;
        }
    }
    /* From here to the initial notify we are mutating the same fields the tip
     * watcher touches on a job switch, so take the connection's state lock. */
    pthread_mutex_lock(&c->state_lock);
    if (hint > 0.0) {
        c->difficulty = hint;
        LOG_INFO("stratum: %s resumed at difficulty %.0f from its own history",
                 c->worker_name, hint);
    } else if (c->difficulty <= 0) {
        c->difficulty = s->cfg.initial_diff;
    }
    /* Same clamp as vardiff, and it deliberately wins over the hint floor
     * above: a starting difficulty above the network difficulty would make the
     * miner discard valid blocks locally. When the two disagree the chain
     * wins — see the minimum-difficulty-window note in the rental port's
     * config. */
    double net_diff = current_net_diff(s);
    if (net_diff > 0.0 && c->difficulty > net_diff) c->difficulty = net_diff;
    /* Arm vardiff window for this connection. */
    c->vd_window_start_ms = now_ms();
    c->vd_window_shares = 0;

    /* respond true */
    emit_response(buf, len, id, cJSON_CreateTrue(), NULL);
    LOG_INFO("stratum: authorized '%s' from %s (fd=%d) at difficulty %.0f",
             c->worker_name, c->peer_ip[0] ? c->peer_ip : "?", c->fd,
             c->difficulty);
    /* Then push initial set_difficulty + notify (renders this conn's
     * coinbase against the current job using its payout address). */
    send_set_difficulty(buf, len, c->difficulty);
    send_current_notify(s, c, buf, len, 1);
    pthread_mutex_unlock(&c->state_lock);
    return 0;
}

static int dedupe_check_and_add(stratum_conn_t *c, const char *jid,
                                const char *en2, const char *ntime,
                                const char *nonce, uint32_t version) {
    char key[256];
    snprintf(key, sizeof(key), "%s|%s|%s|%s|%08x",
             jid ? jid : "", en2 ? en2 : "", ntime ? ntime : "", nonce ? nonce : "",
             version);
    uint64_t h = fnv1a(key);
    for (size_t i = 0; i < DEDUPE_RING; ++i) {
        if (c->dedupe[i] == h) return 1;
    }
    c->dedupe[c->dedupe_head] = h;
    c->dedupe_head = (c->dedupe_head + 1) % DEDUPE_RING;
    return 0;
}

/* Server-wide dedupe on the assembled header hash. Returns 1 if this exact
 * hash has already been credited on any connection, else records it and
 * returns 0. Called after the header is built, so it catches duplicates the
 * per-connection ring structurally cannot: a resubmission on a reconnected
 * or parallel connection, or the same work reframed under a different job
 * id. Two identical hashes represent one solution and must be paid once. */
static int share_dedupe_check_and_add(stratum_server_t *s,
                                      const uint8_t hash_be[32]) {
    stratum_shared_t *sh = s->shared;
    uint64_t h = fnv1a_bytes(hash_be, 32);
    int dup = 0;
    pthread_mutex_lock(&sh->dedupe_lock);
    for (size_t i = 0; i < SHARE_DEDUPE_RING; ++i) {
        if (sh->dedupe[i] == h) { dup = 1; break; }
    }
    if (!dup) {
        sh->dedupe[sh->dedupe_head] = h;
        sh->dedupe_head = (sh->dedupe_head + 1) % SHARE_DEDUPE_RING;
    }
    pthread_mutex_unlock(&sh->dedupe_lock);
    return dup;
}

/* Build full block hex from job + coinbase + nonce/ntime. Returns malloc'd
 * NUL-terminated string, or NULL on OOM. */
static char *assemble_block_hex(const stratum_job_t *j,
                                const uint8_t *coinbase_tx, size_t cb_len,
                                const uint8_t header[80]) {
    /* header(80) | varint(1+tx_count) | coinbase | concat(template_txs raw) */
    size_t tx_count = j->tx_count + 1; /* +1 coinbase */
    uint8_t *block = NULL;
    size_t cap = 0, len = 0;
#define APPEND(call) do { if ((call) != 0) goto oom; } while (0)
    APPEND(bytes_append(&block, &cap, &len, header, 80));
    APPEND(varint_append(&block, &cap, &len, tx_count));
    if (j->coinbase_has_witness && cb_len >= 8) {
        /* coinbase_tx is the legacy serialization:
         *   version(4) | inputs | outputs | locktime(4)
         * The block's coinbase must carry its witness so the segwit
         * commitment validates. Re-serialize in segwit form: insert the
         * marker+flag after the version and the single-input witness (one
         * 32-byte reserved value, all zero — matching the commitment the
         * backend computed) just before the locktime. */
        static const uint8_t marker_flag[2] = { 0x00, 0x01 };
        static const uint8_t witness[34]    = { 0x01, 0x20 }; /* 1 item, 32 bytes, all zero */
        APPEND(bytes_append(&block, &cap, &len, coinbase_tx, 4));                 /* version */
        APPEND(bytes_append(&block, &cap, &len, marker_flag, 2));
        APPEND(bytes_append(&block, &cap, &len, coinbase_tx + 4, cb_len - 8));    /* inputs + outputs */
        APPEND(bytes_append(&block, &cap, &len, witness, sizeof witness));
        APPEND(bytes_append(&block, &cap, &len, coinbase_tx + cb_len - 4, 4));    /* locktime */
    } else {
        APPEND(bytes_append(&block, &cap, &len, coinbase_tx, cb_len));
    }
    for (size_t i = 0; i < j->tx_count; ++i) {
        size_t txn = 0;
        uint8_t *txb = hex_to_bytes_alloc(j->tx_hex_list[i], &txn);
        if (!txb) { free(block); return NULL; }
        int arc = bytes_append(&block, &cap, &len, txb, txn);
        free(txb);
        if (arc != 0) goto oom;
    }
    char *out = malloc(len * 2 + 1);
    if (!out) { free(block); return NULL; }
    bytes_to_hex(block, len, out);
    free(block);
    return out;
oom:
    free(block);
    return NULL;
#undef APPEND
}

static int handle_submit(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                         cJSON *params, char **buf, size_t *len) {
    /* Both refusals below answer the miner with an error, so they count as
     * rejects on ITS dashboard. Recording them keeps our reject table and the
     * miner's own numbers describing the same events — without this a miner
     * refused on every submit reads as simply absent here. */
    if (!c->authorized) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx,
                             c->worker_name[0] ? c->worker_name
                                               : "(unauthorized)",
                             now_ms(), "unauthorized");
        }
        cJSON *err = make_error(24, "unauthorized");
        return emit_response(buf, len, id, NULL, err);
    }
    if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) < 5) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "bad params");
        }
        cJSON *err = make_error(20, "bad params");
        return emit_response(buf, len, id, NULL, err);
    }
    const char *worker = cJSON_GetArrayItem(params, 0)->valuestring;
    const char *jid    = cJSON_GetArrayItem(params, 1)->valuestring;
    const char *en2    = cJSON_GetArrayItem(params, 2)->valuestring;
    const char *ntime  = cJSON_GetArrayItem(params, 3)->valuestring;
    const char *nonce  = cJSON_GetArrayItem(params, 4)->valuestring;
    (void)worker;

    stratum_job_t *job = find_job(s, jid);
    if (!job) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "stale or unknown job");
        }
        cJSON *err = make_error(21, "stale or unknown job");
        return emit_response(buf, len, id, NULL, err);
    }

    /* Version rolling (BIP310): the optional 6th submit param carries the
     * version the miner actually hashed. Keep the job's version bits outside
     * the negotiated mask and take the miner's bits inside it; with no param
     * (or no negotiation) this leaves job->version unchanged. We fall back to
     * the standard BIP320 mask if a version arrives without prior configure,
     * so miners that roll by default still verify correctly. */
    int32_t submit_version = job->version;
    if (cJSON_GetArraySize(params) >= 6) {
        cJSON *v = cJSON_GetArrayItem(params, 5);
        uint32_t rolled = 0;
        if (!cJSON_IsString(v) || parse_u32_hex(v->valuestring, &rolled) != 0) {
            cJSON *err = make_error(20, "bad version hex");
            stratum_job_free(job);   /* release find_job's reference */
            return emit_response(buf, len, id, NULL, err);
        }
        uint32_t mask = c->version_mask ? c->version_mask : VERSION_ROLLING_MASK;
        submit_version =
            (int32_t)(((uint32_t)job->version & ~mask) | (rolled & mask));
    }

    if (dedupe_check_and_add(c, jid, en2, ntime, nonce,
                             (uint32_t)submit_version)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "duplicate share");
        }
        cJSON *err = make_error(22, "duplicate share");
        stratum_job_free(job);   /* release find_job's reference */
        return emit_response(buf, len, id, NULL, err);
    }

    uint32_t ntime_v, nonce_v;
    if (parse_u32_hex(ntime, &ntime_v) != 0 || parse_u32_hex(nonce, &nonce_v) != 0) {
        cJSON *err = make_error(20, "bad ntime/nonce hex");
        stratum_job_free(job);   /* release find_job's reference */
        return emit_response(buf, len, id, NULL, err);
    }

    size_t en2_len = 0;
    uint8_t *en2_bytes = hex_to_bytes_alloc(en2, &en2_len);
    if (!en2_bytes) {
        cJSON *err = make_error(20, "bad extranonce2 hex");
        stratum_job_free(job);   /* release find_job's reference */
        return emit_response(buf, len, id, NULL, err);
    }

    /* The extranonce2 must be exactly the width we advertised at subscribe
     * and reserved in the coinbase scriptSig. Any other length still hashes
     * and can still beat the target, but it produces a coinbase whose
     * scriptSig length prefix disagrees with its contents — unserialisable.
     * Crediting such a share is bad; submitting the block it solves is worse,
     * because the node rejects the malformed transaction and the reward is
     * simply lost. Reject at the door instead. */
    if (en2_len != job->en2_size) {
        free(en2_bytes);
        LOG_INFO("stratum: reject from worker '%s' - Reason: extranonce2 is "
                 "%zu bytes, expected %zu", c->worker_name, en2_len,
                 job->en2_size);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "bad extranonce2 size");
        }
        cJSON *err = make_error(20, "bad extranonce2 size");
        stratum_job_free(job);   /* release find_job's reference */
        return emit_response(buf, len, id, NULL, err);
    }

    /* Render this connection's coinbase for `job` if not cached, and copy it
     * out — both under state_lock, in ONE critical section.
     *
     * They cannot be split. The tip watcher renders against this same
     * connection on every job switch, and rendering frees cb1/cb2; releasing
     * the lock between the render and the memcpy would put the free and the
     * read back in the same race this lock exists to close. Snapshot the
     * vardiff fields here too, so the rest of the validation works off stable
     * values instead of re-reading them as the watcher retargets.
     *
     * The cache is keyed on job_id; submits against an older job retired into
     * the recent ring will rebuild on demand. */
    pthread_mutex_lock(&c->state_lock);
    if (conn_render_coinbase(s, c, job) < 0) {
        pthread_mutex_unlock(&c->state_lock);
        free(en2_bytes);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "coinbase render failed");
        }
        cJSON *err = make_error(25, "coinbase render failed");
        stratum_job_free(job);   /* release find_job's reference */
        return emit_response(buf, len, id, NULL, err);
    }

    /* coinbase = cb1 || ex1 || ex2 || cb2 */
    size_t cb_len = c->cb1_len + STRATUM_EXTRANONCE1_SIZE + en2_len + c->cb2_len;
    uint8_t *cb = malloc(cb_len);
    if (!cb) {
        pthread_mutex_unlock(&c->state_lock);
        free(en2_bytes);
        stratum_job_free(job);   /* release find_job's reference */
        return -1;
    }
    size_t off = 0;
    memcpy(cb + off, c->cb1, c->cb1_len);   off += c->cb1_len;
    memcpy(cb + off, c->extranonce1, STRATUM_EXTRANONCE1_SIZE);
    off += STRATUM_EXTRANONCE1_SIZE;
    memcpy(cb + off, en2_bytes, en2_len);   off += en2_len;
    memcpy(cb + off, c->cb2, c->cb2_len);   off += c->cb2_len;
    const double cur_diff       = c->difficulty;
    const double prev_diff      = c->prev_difficulty;
    const uint64_t diff_changed = c->diff_changed_ms;
    /* The difficulty THIS job was issued to THIS connection at. Snapshotted
     * under the same lock as the rest of the vardiff set, so the tip watcher
     * cannot retarget between reading one and reading another. */
    double job_diff = 0.0;
    const int have_job_diff = conn_job_diff_lookup(c, jid, &job_diff);
    pthread_mutex_unlock(&c->state_lock);
    free(en2_bytes);

    uint8_t cb_txid_le[32];
    dsha256(cb, cb_len, cb_txid_le);

    uint8_t merkle_root_le[32];
    merkle_root_from_branches(cb_txid_le,
                              (const uint8_t (*)[32])job->merkle_branches,
                              job->branch_count, merkle_root_le);

    uint8_t header[80];
    build_header(submit_version, job->prev_hash_le, merkle_root_le,
                 ntime_v, job->nbits, nonce_v, header);

    uint8_t hash_be[32];
    hash_header(header, hash_be);

    /* Judge the share at the difficulty THIS job was issued to THIS connection
     * at, not at whatever the connection has retargeted to since. A submit may
     * name a job several retargets old — the server keeps STRATUM_RECENT_JOBS
     * solvable — and the miner mined it at the difficulty we asked for at the
     * time. Falls back to the current difficulty when this connection has no
     * record of the job. */
    const double judge_diff = have_job_diff ? job_diff : cur_diff;
    uint8_t worker_target[32];
    worker_diff_to_target(judge_diff, worker_target);

    char sent_hash_hex[65] = {0};
    char worker_target_hex[65] = {0};
    char network_target_hex[65] = {0};

    // sent_hash_hex is needed by the share record below, so it is always built.
    bytes_to_hex(hash_be, 32, sent_hash_hex);

    /* DEBUG, not INFO. Vardiff clamps the share target to the network target, so at
     * difficulty 1 every miner submits at its full hash rate and this fires tens of
     * thousands of times a second. At INFO that buries journald's rate limit (10k/30s
     * by default) and takes the pool's own WARN/ERROR lines down with it — the fault
     * signal is lost in the noise about ordinary shares. The two extra hex conversions
     * exist only for this line, so they are skipped with it. */
    if (log_enabled(LOG_LVL_DEBUG)) {
        bytes_to_hex(worker_target, 32, worker_target_hex);
        bytes_to_hex(job->network_target_be, 32, network_target_hex);
        LOG_DEBUG("stratum: [SUBMIT CHECK] Worker: %s\n"
                  "  -> Sent Hash:     %s\n"
                  "  -> Worker Target: %s\n"
                  "  -> Network Tgt:   %s\n"
                  "  -> Version:       job=%08x rolled=%08x mask=%08x",
                  c->worker_name, sent_hash_hex, worker_target_hex, network_target_hex,
                  (uint32_t)job->version, (uint32_t)submit_version, c->version_mask);
    }

    uint64_t ts_now   = now_ms();
    int is_block      = be32_cmp(hash_be, job->network_target_be) <= 0;
    int meets_worker  = be32_cmp(hash_be, worker_target) < 0;
    double share_diff = judge_diff;

    /* Second chance at the pre-retarget difficulty, within a grace period.
     *
     * Still needed even with judge_diff above, because stratum does not pin
     * down which job a mining.set_difficulty first applies to: we record it
     * against the notify it was sent with, but a miner that applies it only on
     * the FOLLOWING job mined this one easier than we recorded. Without this
     * that miner's honest work is rejected. Miners differ here, so the
     * tolerance is deliberate rather than a workaround for one of them. */
    if (!meets_worker && prev_diff > 0.0 &&
        ts_now - diff_changed < diff_grace_ms(s)) {
        uint8_t prev_target[32];
        worker_diff_to_target(prev_diff, prev_target);
        if (be32_cmp(hash_be, prev_target) < 0) {
            meets_worker = 1;
            share_diff = prev_diff;
        }
    }

    /* The network-target verdict must win over the share-difficulty reject:
     * when the share target is harder than the network target (low-difficulty
     * networks), a hash can be a valid block while failing the share check —
     * it has to be submitted, never rejected. */
    if (!is_block && !meets_worker) {
	LOG_INFO("stratum: reject from worker '%s' - Reason: low difficulty (Sent Hash > Worker Target)", c->worker_name);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "low difficulty");
        }
        free(cb);
        cJSON *err = make_error(23, "low difficulty");
        stratum_job_free(job);   /* release find_job's reference */
        return emit_response(buf, len, id, NULL, err);
    }

    /* Only NOW record the hash, once this share is known to be worth
     * something. The ring is server-wide and finite (SHARE_DEDUPE_RING), and
     * recording every submission put rejects in it too — a miner sending
     * garbage, or one whose difficulty is badly mismatched, could evict the
     * genuine recent hashes it exists to protect. Nothing below this point
     * rejects on difficulty, so a hash that reaches here is one that will be
     * credited, submitted, or both, which is exactly what must be deduplicated.
     *
     * Still before any crediting: the same hash is one solution however it was
     * framed, and paying it twice is the thing this prevents. */
    if (share_dedupe_check_and_add(s, hash_be)) {
        free(cb);
        LOG_INFO("stratum: reject from worker '%s' - Reason: duplicate share "
                 "(hash already credited)", c->worker_name);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "duplicate share");
        }
        cJSON *err = make_error(22, "duplicate share");
        stratum_job_free(job);   /* release find_job's reference */
        return emit_response(buf, len, id, NULL, err);
    }

    char block_hash_hex[65] = {0};
    /* Did the NODE take this block onto the best chain? Stays 0 unless
     * on_block says so, which also covers the case where the block could not
     * be assembled and was never submitted at all. */
    int  block_accepted = 0;
    if (is_block) {
        bytes_to_hex(hash_be, 32, block_hash_hex);
        if (!meets_worker) {
            /* Credit the share at the difficulty it provably met. */
            share_diff = target_to_diff(job->network_target_be);
            LOG_INFO("stratum: hash from '%s' beats the network target but not "
                     "the share target — submitting block", c->worker_name);
        }
        char *block_hex = assemble_block_hex(job, cb, cb_len, header);
        if (block_hex) {
            if (s->cfg.on_block)
                block_accepted = (s->cfg.on_block(s->cfg.ctx, block_hex) == 0);
            free(block_hex);
        } else {
            LOG_ERROR("stratum: could not assemble the block for '%s' at "
                      "height %u — nothing was submitted", c->worker_name,
                      job->height);
        }
    }
    free(cb);

    if (s->cfg.on_share) {
        /* Always pass the actual share hash so the dashboard can show the
         * hash of every share (and the user can eyeball its leading zeros
         * to gauge how lucky each share was). When is_block, this string
         * also IS the block hash; otherwise it's a 'just-a-share' hash. */
        s->cfg.on_share(s->cfg.ctx, c->worker_name, c->payout_address,
                        ts_now, share_diff, is_block, sent_hash_hex);
    }
    /* Tick vardiff: count this accepted share toward the window. May emit
     * a mining.set_difficulty notification if the window has elapsed. */
    pthread_mutex_lock(&c->state_lock);
    c->vd_window_shares++;
    vardiff_maybe_retarget(s, c, now_ms(), buf, len);
    pthread_mutex_unlock(&c->state_lock);
    /* ⛔ Gated on the NODE accepting the block, not on the pool solving it.
     *
     * Two miners can solve the same height milliseconds apart. submitblock
     * takes the first and answers the second "inconclusive": a valid block
     * that is not in the chain. Recording that as found writes a second
     * blocks_found row for one height and — far worse — settles the
     * proportional plan against a reward nobody was ever paid, which breaks
     * the zero-sum invariant the no-custody design rests on.
     *
     * This is rare at real difficulty and routine at minimum difficulty, which
     * is exactly the window this pool exists to mine. Found by
     * tests/test_burst_regtest.sh. */
    if (is_block && !block_accepted) {
        LOG_INFO("stratum: block %s from '%s' at height %u was NOT accepted by "
                 "the node — not recording it as found, not settling payouts. "
                 "The share itself still counts.",
                 block_hash_hex, c->worker_name, job->height);
    }
    if (is_block && block_accepted && s->cfg.on_block_found) {
        int64_t fee_sats = 0;
        if (s->cfg.fee_bps > 0 && s->cfg.operator_address[0]) {
            fee_sats = (job->value_sats * (int64_t)s->cfg.fee_bps) / 10000;
            if (fee_sats < 546) fee_sats = 0; /* matches coinbase dust rule */
        }
        int64_t reward_sats = job->value_sats - fee_sats;
        s->cfg.on_block_found(s->cfg.ctx, c->worker_name,
                              c->payout_address, ts_now, job->height,
                              job->job_id, block_hash_hex,
                              reward_sats, fee_sats);
    }
    stratum_job_free(job);   /* release find_job's reference */
    return emit_response(buf, len, id, cJSON_CreateTrue(), NULL);
}

int stratum_handle_message(stratum_server_t *s, stratum_conn_t *c,
                           const char *line, char **out_buf, size_t *out_len)
{
    if (!line) return -1;
    if (strlen(line) > MAX_LINE_BYTES) return -1;
    cJSON *root = cJSON_Parse(line);
    if (!root) return -1;
    cJSON *id     = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return -1;
    }
    int rc = 0;
    if (strcmp(method->valuestring, "mining.configure") == 0) {
        rc = handle_configure(s, c, id, params, out_buf, out_len);
    } else if (strcmp(method->valuestring, "mining.subscribe") == 0) {
        rc = handle_subscribe(s, c, id, out_buf, out_len);
    } else if (strcmp(method->valuestring, "mining.authorize") == 0) {
        rc = handle_authorize(s, c, id, params, out_buf, out_len);
    } else if (strcmp(method->valuestring, "mining.submit") == 0) {
        rc = handle_submit(s, c, id, params, out_buf, out_len);
    } else {
        cJSON *err = make_error(20, "unknown method");
        rc = emit_response(out_buf, out_len, id, NULL, err);
    }
    cJSON_Delete(root);
    return rc;
}

/* ---- conn lifecycle (test helpers + thread) --------------------------- */

stratum_conn_t *stratum_conn_new_for_test(stratum_server_t *s) {
    stratum_conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->server = s;
    c->fd = -1;
    c->difficulty = s ? s->cfg.initial_diff : 1.0;
    pthread_mutex_init(&c->state_lock, NULL);
    pthread_mutex_init(&c->write_lock, NULL);
    return c;
}

void stratum_conn_free_for_test(stratum_conn_t *c) {
    if (!c) return;
    conn_clear_coinbase(c);
    pthread_mutex_destroy(&c->state_lock);
    pthread_mutex_destroy(&c->write_lock);
    free(c);
}

const char *stratum_conn_worker_name_for_test(const stratum_conn_t *c) {
    return c ? c->worker_name : NULL;
}
const char *stratum_conn_payout_address_for_test(const stratum_conn_t *c) {
    return c ? c->payout_address : NULL;
}
int stratum_conn_authorized_for_test(const stratum_conn_t *c) {
    return c ? c->authorized : 0;
}

void stratum_conn_force_difficulty_for_test(stratum_conn_t *c,
                                            double cur, double prev) {
    if (!c) return;
    pthread_mutex_lock(&c->state_lock);
    c->difficulty = cur;
    c->prev_difficulty = prev;
    c->diff_changed_ms = now_ms();   /* grace window is OPEN, so a test that
                                      * passes is not passing because it
                                      * expired */
    pthread_mutex_unlock(&c->state_lock);
}
int stratum_conn_subscribed_for_test(const stratum_conn_t *c) {
    return c ? c->subscribed : 0;
}
int stratum_server_conn_count_for_test(const stratum_server_t *s) {
    return s ? atomic_load(&s->conn_count) : 0;
}

/* ---- real connection thread ------------------------------------------ */

/* Write the whole buffer or fail.
 *
 * The socket carries SO_SNDTIMEO (see conn_socket_setup), so a peer that has
 * stopped reading makes this return -1 with EAGAIN after SEND_TIMEOUT_SEC
 * rather than blocking forever. That bound is what keeps one stalled miner
 * from freezing job broadcast for every other miner — see the comment in
 * stratum_server_set_job. A timed-out write may have delivered a partial
 * line, so the caller has to drop the connection, not retry it. */
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)n;
    }
    return 0;
}

/* Configure an accepted socket so idle miners get reaped instead of
 * clogging fds/threads indefinitely. Two mechanisms, belt-and-suspenders:
 *
 *   1. Application-level: SO_RCVTIMEO gives recv() a bounded wake so we
 *      can compare last_activity_ms against cfg.idle_timeout_sec. Catches
 *      miners that hold the TCP open but never send anything (e.g. bad
 *      username, misconfigured worker).
 *   2. OS-level: SO_KEEPALIVE + tightened TCP_KEEPIDLE/INTVL/CNT so Linux
 *      drops the socket after ~5 min of unacked probes. Catches half-open
 *      TCPs where the miner box vanished from the network without FIN.
 *
 * idle_timeout_sec <= 0 disables the read-timeout path (legacy blocking
 * recv). Returns 0 on success, -1 on fatal setsockopt failure. */
static int conn_socket_setup(int fd, int idle_timeout_sec) {
    int one = 1;
    /* TCP_NODELAY: stratum is tiny latency-sensitive JSON, don't Nagle. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Kernel keepalive. Default kernel setting is ~2h before probes even
     * start, useless for our purposes — override with tight values. */
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 120;   /* start probing after 2 min of inactivity */
    int intvl = 30;   /* probe every 30s */
    int cnt = 3;      /* drop after 3 unacked probes ≈ 3.5 min total */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
#endif

    if (idle_timeout_sec > 0) {
        /* Poll interval: min(idle_timeout, 30s). Longer wastes the tail
         * of the timeout; shorter costs one recv wake per fd per interval
         * (500 conns × wake/30s = ~16/s, negligible). */
        int poll_s = idle_timeout_sec < 30 ? idle_timeout_sec : 30;
        struct timeval tv = { .tv_sec = poll_s, .tv_usec = 0 };
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            return -1;
        }
    }

    /* Bound every send. Without this a miner that simply stops reading fills
     * its socket buffer, write_all() blocks forever, and because job broadcast
     * writes to each connection in turn while holding conns_lock, NO miner
     * gets a new job until the kernel's keepalive finally kills the dead peer
     * (~3.5 min above). One connection could stall the whole pool on a stale
     * template — worst exactly during a fast-rotating min-difficulty window.
     * A miner with 10s of unread notify traffic is not mining for us anyway. */
    struct timeval sndtv = { .tv_sec = SEND_TIMEOUT_SEC, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndtv, sizeof(sndtv)) < 0) {
        return -1;
    }
    return 0;
}

int stratum_socket_setup_for_test(int fd, int idle_timeout_sec) {
    return conn_socket_setup(fd, idle_timeout_sec);
}

static void conn_register(stratum_server_t *s, stratum_conn_t *c);

void stratum_conn_register_for_test(stratum_server_t *s, stratum_conn_t *c,
                                    int fd) {
    if (!s || !c) return;
    c->fd = fd;
    conn_register(s, c);
}

static void conn_register(stratum_server_t *s, stratum_conn_t *c) {
    pthread_mutex_lock(&s->conns_lock);
    c->next = s->conns_head;
    s->conns_head = c;
    pthread_mutex_unlock(&s->conns_lock);
}

static void conn_unregister(stratum_server_t *s, stratum_conn_t *c) {
    pthread_mutex_lock(&s->conns_lock);
    struct stratum_conn **p = &s->conns_head;
    while (*p) {
        if (*p == c) { *p = c->next; break; }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&s->conns_lock);
}

static void *conn_thread(void *arg) {
    stratum_conn_t *c = arg;
    stratum_server_t *s = c->server;
    char buf[MAX_LINE_BYTES + 1];
    size_t blen = 0;

    /* Seed activity tracking at connect time — a client that never sends
     * a single byte is still governed by cfg.idle_timeout_sec. */
    c->last_activity_ms = mono_ms();
    const uint64_t idle_timeout_ms =
        s->cfg.idle_timeout_sec > 0
            ? (uint64_t)s->cfg.idle_timeout_sec * 1000u
            : 0;

    while (!atomic_load(&s->stop)) {
        ssize_t n = recv(c->fd, buf + blen, sizeof(buf) - 1 - blen, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* SO_RCVTIMEO wake. Drop iff we've been silent past the
                 * configured budget. Otherwise loop and try again. */
                if (idle_timeout_ms > 0 &&
                    mono_ms() - c->last_activity_ms > idle_timeout_ms) {
                    LOG_INFO("stratum: idle timeout after %us — closing fd=%d worker='%s'",
                             s->cfg.idle_timeout_sec, c->fd,
                             c->worker_name[0] ? c->worker_name : "(unauthorized)");
                    goto done;
                }
                continue;
            }
            break;  /* real socket error */
        }
        if (n == 0) break;  /* peer closed */
        c->last_activity_ms = mono_ms();
        blen += (size_t)n;
        buf[blen] = '\0';
        for (;;) {
            char *nl = memchr(buf, '\n', blen);
            if (!nl) {
                if (blen >= MAX_LINE_BYTES) { goto done; } /* oversize */
                break;
            }
            *nl = '\0';
            char *line = buf;
            char *out = NULL; size_t olen = 0;
            int rc = stratum_handle_message(s, c, line, &out, &olen);
            if (out && olen) {
                pthread_mutex_lock(&c->write_lock);
                write_all(c->fd, out, olen);
                pthread_mutex_unlock(&c->write_lock);
            }
            free(out);
            if (rc < 0) goto done;
            size_t consumed = (size_t)(nl - buf) + 1;
            memmove(buf, buf + consumed, blen - consumed);
            blen -= consumed;
        }
    }
done:
    /* Unregister BEFORE closing, never after.
     *
     * conn_unregister takes conns_lock, which is the lock job broadcast holds
     * while it writes to every linked connection. Closing first left this
     * connection linked with a dead fd number, and accept() on the listener
     * thread can hand that same number straight back out — so a mining.notify
     * meant for the departing miner lands in whatever the process opened next.
     * The listener's own error path below has always had this order right. */
    conn_unregister(s, c);
    close(c->fd);
    c->fd = -1;
    stratum_conn_free_for_test(c);
    /* Decrement LAST, after this thread has finished touching both the server
     * and its own connection. stratum_server_stop waits for this counter to
     * reach zero before the server is torn down, so it has to mean "no
     * connection thread will read any of this again" — not "the socket is
     * closed". Decrementing before the free left the counter at zero while
     * this thread was still inside stratum_conn_free_for_test. */
    atomic_fetch_sub(&s->conn_count, 1);
    return NULL;
}

static void *listener_thread(void *arg) {
    stratum_server_t *s = arg;
    while (!atomic_load(&s->stop)) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int fd = accept(s->listen_fd, (struct sockaddr *)&cli, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (atomic_load(&s->stop)) break;
            LOG_WARN("stratum: accept: %s", strerror(errno));
            continue;
        }
        if (atomic_load(&s->conn_count) >= s->cfg.max_conns) {
            close(fd);
            continue;
        }
        if (conn_socket_setup(fd, s->cfg.idle_timeout_sec) < 0) {
            LOG_WARN("stratum: socket setup failed for accepted fd: %s",
                     strerror(errno));
            close(fd);
            continue;
        }
        stratum_conn_t *c = stratum_conn_new_for_test(s);
        if (!c) { close(fd); continue; }
        c->fd = fd;
        if (!inet_ntop(AF_INET, &cli.sin_addr, c->peer_ip, sizeof c->peer_ip))
            snprintf(c->peer_ip, sizeof c->peer_ip, "?");
        atomic_fetch_add(&s->conn_count, 1);
        conn_register(s, c);
        if (pthread_create(&c->thr, NULL, conn_thread, c) != 0) {
            conn_unregister(s, c);
            atomic_fetch_sub(&s->conn_count, 1);
            close(fd);
            stratum_conn_free_for_test(c);
            continue;
        }
        pthread_detach(c->thr);
        c->thr_started = 1;
    }
    return NULL;
}

int stratum_server_start(const stratum_cfg_t *cfg, stratum_server_t **out) {
    if (!cfg || !out) return -1;
    stratum_server_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->cfg = *cfg;
    if (s->cfg.max_conns <= 0) s->cfg.max_conns = 500;
    if (s->cfg.initial_diff <= 0) s->cfg.initial_diff = 1.0;
    /* Negative → explicit disable. 0 → apply default (10 min). Positive kept. */
    if (s->cfg.idle_timeout_sec == 0) s->cfg.idle_timeout_sec = 600;
    else if (s->cfg.idle_timeout_sec < 0) s->cfg.idle_timeout_sec = 0;
    s->prop_enabled = (strcmp(s->cfg.pool_mode, "proportional") == 0);
    pthread_rwlock_init(&s->job_lock, NULL);
    pthread_mutex_init(&s->recent_lock, NULL);
    pthread_mutex_init(&s->conns_lock, NULL);
    atomic_init(&s->stop, 0);
    atomic_init(&s->conn_count, 0);

    /* A lone server (and every test) allocates its own; main.c passes one in
     * so the rental port draws extranonce1 from the same sequence and dedupes
     * against the same ring as the public port. */
    if (s->cfg.shared) {
        s->shared = s->cfg.shared;
        s->shared_owned = 0;
    } else {
        s->shared = stratum_shared_new();
        if (!s->shared) { free(s); return -1; }
        s->shared_owned = 1;
    }

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        if (s->shared_owned) stratum_shared_free(s->shared);
        free(s);
        return -1;
    }
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->bind_port);
    if (cfg->bind_addr[0] == '\0' || strcmp(cfg->bind_addr, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) != 1) {
            close(s->listen_fd); if (s->shared_owned) stratum_shared_free(s->shared); free(s); return -1;
        }
    }
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("stratum bind %s:%d: %s", cfg->bind_addr, cfg->bind_port, strerror(errno));
        close(s->listen_fd); if (s->shared_owned) stratum_shared_free(s->shared); free(s); return -1;
    }
    if (listen(s->listen_fd, 64) < 0) {
        close(s->listen_fd); if (s->shared_owned) stratum_shared_free(s->shared); free(s); return -1;
    }
    if (pthread_create(&s->listener_thr, NULL, listener_thread, s) != 0) {
        close(s->listen_fd); if (s->shared_owned) stratum_shared_free(s->shared); free(s); return -1;
    }
    s->listener_started = 1;
    *out = s;
    return 0;
}

void stratum_server_set_job(stratum_server_t *s, stratum_job_t *new_job,
                            int clean) {
    if (!s || !new_job) return;
    pthread_rwlock_wrlock(&s->job_lock);
    stratum_job_t *old = s->current_job;
    s->current_job = new_job;
    pthread_rwlock_unlock(&s->job_lock);
    if (old) retire_job(s, old);

    /* Broadcast the new job. Each conn renders its own coinbase against it
     * (paying its miner address).
     *
     * `clean` is the caller's answer to "must miners throw away work in
     * progress?", and it is only true when the previous block changed —
     * anything still being hashed against the old tip can now only produce an
     * orphan. It must be FALSE for a same-tip refresh that merely adds
     * transactions and moves ntime: the old job is still valid work, and
     * flushing it discards every miner's partial progress for nothing.
     *
     * This is not a cosmetic distinction. A hashrate marketplace's proxy
     * flushes its own fleet whenever an upstream sets the flag, and its
     * miners' in-flight shares then come back stale on ITS side — invisible
     * in our reject counters, which is exactly why a pool doing this looks
     * healthy from here while the marketplace measures a reject storm and
     * drops the upstream. */
    pthread_mutex_lock(&s->conns_lock);
    for (stratum_conn_t *c = s->conns_head; c; c = c->next) {
        if (!c->subscribed || c->fd < 0 || !c->authorized) continue;
        char *out = NULL; size_t olen = 0;
        pthread_mutex_lock(&c->state_lock);
        vardiff_check_idle(s, c, &out, &olen);
        send_current_notify(s, c, &out, &olen, clean);
        pthread_mutex_unlock(&c->state_lock);
        if (out) {
            pthread_mutex_lock(&c->write_lock);
            int wrc = write_all(c->fd, out, olen);
            pthread_mutex_unlock(&c->write_lock);
            free(out);
            /* A miner that has not drained its socket in SEND_TIMEOUT_SEC gets
             * dropped rather than allowed to hold up the broadcast. We cannot
             * close() the fd here — the connection thread owns it — but
             * shutdown() is safe from another thread and wakes that thread's
             * recv() so it runs its normal teardown. The partial line this may
             * have left on the wire is exactly why the connection has to go. */
            if (wrc < 0) {
                LOG_WARN("stratum: dropping '%s' — notify write failed (%s); "
                         "it was holding up the broadcast",
                         c->worker_name[0] ? c->worker_name : "(unauthorized)",
                         strerror(errno));
                shutdown(c->fd, SHUT_RDWR);
            }
        }
    }
    pthread_mutex_unlock(&s->conns_lock);
}

void stratum_server_stop(stratum_server_t *s) {
    if (!s) return;
    atomic_store(&s->stop, 1);
    /* shutdown() to break the listener out of accept(), then JOIN, and only
     * then close and clear the fd. Clearing it before the join raced the
     * listener's own read of s->listen_fd (TSan flagged it 7x in the job
     * rotation stress test) and could have closed the fd while accept() was
     * still using it. After the join nothing else touches the field. */
    int fd = s->listen_fd;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    if (s->listener_started) {
        pthread_join(s->listener_thr, NULL);
        s->listener_started = 0;
    }
    if (fd >= 0) {
        close(fd);
        s->listen_fd = -1;
    }

    /* Then drain the connection threads, and do NOT return until they are gone.
     *
     * They are detached and can be parked in recv() for a full SO_RCVTIMEO
     * (up to 30s), so simply returning here left them running while main.c went
     * on to free this server, close the store and destroy the server context.
     * A share arriving in that window was processed against freed memory:
     * conn_unregister takes a destroyed conns_lock, and on_share/on_reject call
     * into a closed store. Shutting each socket down wakes its thread
     * immediately, and conn_count reaching zero is the proof they are finished.
     *
     * conns_lock is released before waiting — the threads need it themselves to
     * unregister, so holding it here would deadlock the very exit we want. */
    pthread_mutex_lock(&s->conns_lock);
    for (stratum_conn_t *c = s->conns_head; c; c = c->next) {
        int cfd = c->fd;
        if (cfd >= 0) shutdown(cfd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&s->conns_lock);

    /* Bounded so a wedged thread cannot hang the process forever; the timeout
     * is reported rather than passed over, because continuing past it is the
     * use-after-free above and the operator needs to know it happened. */
    const int drain_ms = 5000, step_ms = 2;
    int waited_ms = 0;
    while (atomic_load(&s->conn_count) > 0 && waited_ms < drain_ms) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = step_ms * 1000000L };
        nanosleep(&ts, NULL);
        waited_ms += step_ms;
    }
    int left = atomic_load(&s->conn_count);
    if (left > 0) {
        LOG_ERROR("stratum: %d connection thread(s) still running after %dms — "
                  "continuing shutdown anyway; they may touch freed state",
                  left, drain_ms);
    }
}

void stratum_server_free(stratum_server_t *s) {
    if (!s) return;
    stratum_server_stop(s);
    pthread_rwlock_wrlock(&s->job_lock);
    stratum_job_free(s->current_job);
    s->current_job = NULL;
    pthread_rwlock_unlock(&s->job_lock);
    for (size_t i = 0; i < RECENT_JOBS; ++i) stratum_job_free(s->recent[i]);
    pthread_rwlock_destroy(&s->job_lock);
    pthread_mutex_destroy(&s->recent_lock);
    pthread_mutex_destroy(&s->conns_lock);
    if (s->shared_owned) stratum_shared_free(s->shared);
    free(s);
}
