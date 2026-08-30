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
 * Jobs are reference counted. Anything that reads a job only while holding
 * the lock that guards its slot (send_current_notify, current_net_diff) needs
 * nothing more; find_job hands out a counted reference because a submit
 * outlives the lock, and the tip watcher frees jobs from under it otherwise.
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
#include <math.h>
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
/* Matches store.c's REASON_MAX so a submitblock reason survives the trip to
 * the DB intact rather than being truncated twice. */
#define REASON_TEXT_MAX   128
/* The retention ring is sized in stratum.h, because main.c's payout-plan ring
 * must be able to cover every job that is still solvable. */
#define RECENT_JOBS    STRATUM_RECENT_JOBS
/* How long a retired job stays solvable. See STRATUM_RECENT_JOBS in stratum.h
 * — the effective window is the SMALLER of this and the ring's depth, and the
 * two must be read together.
 *
 * ⚠️ The sweep is LAZY: retire_job() is the only caller and it runs only when a
 * new job is pushed, so the real grace is this value plus the time to the next
 * job (~31 s at production cadence), and is UNBOUNDED if job production stalls.
 *
 * 60 s → 300 s on 2026-08-30, measured rather than chosen. Over 16 h of
 * instrumented rejects, raising the window to 300 s would have accepted 100%
 * of the stale shares from both complaining miners (199 and 4,686) and ~95% of
 * everyone else's. ckpool's equivalent cap is 600 s (stratifier.c, add_base),
 * so 60 s made us 10× stricter than the reference implementation — that, not
 * anything on the miners' side, is why rented hashrate bled here and not
 * elsewhere.
 *
 * ⚠️ A share accepted against a job issued before a tip change is credited but
 * can never win a block. That was already true at 60 s; this widens the window
 * in which it happens. ckpool instead marks such shares stale (workbase_id <
 * blockchange_id) while keeping every same-height job valid regardless of age.
 * Adopting that split is the better long-term shape and is deliberately NOT
 * bundled here: it would newly REFUSE shares we credit today, at block
 * boundaries, to exactly the miners this change is meant to help. Ship the
 * measured fix first; decide the fairness question on its own evidence. */
#define RECENT_JOB_TTL_MS 300000
/* 📌 READ vardiff_maybe_retarget BEFORE RAISING THIS. The vardiff retarget
 * ceiling is vardiff_window_sec * vardiff_max_window_mult (240s at defaults).
 * When this TTL exceeds that ceiling, a job can be old enough to starve a
 * retarget window and still young enough for its shares to be accepted --
 * which produced a downward difficulty runaway when the TTL went 60s -> 300s.
 * The drain guard there handles it at any values, so this is a pointer rather
 * than a constraint: no assert, because the code no longer depends on the
 * inequality. But the next person to raise this will be looking here. */
/* The two numbers above must be read together, so tie them together here: the
 * ring has to be deep enough to still hold a job the TTL considers live, or the
 * ring silently becomes the real window and the TTL is decoration. That is
 * exactly the state this pool shipped in until 2026-08-30, in the other
 * direction — 8 slots that never once bound because the TTL was 60 s.
 *
 * ⚠️ 30000 is `bitcoind_poll_interval_ms`'s DEFAULT (config.c:38), not a law:
 * the cadence is configurable, so this assert checks the shipped configuration,
 * not every possible one. Lower that key materially below 30 s and the ring
 * binds first again at runtime, with no build error to warn you. */
_Static_assert((uint64_t)STRATUM_RECENT_JOBS * 30000u >= RECENT_JOB_TTL_MS,
               "retention ring too shallow for RECENT_JOB_TTL_MS at the default "
               "job cadence: raise STRATUM_RECENT_JOBS or lower the TTL");
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

static uint64_t now_ms(void);   /* defined below; used to seed the counter */

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

    /* References held. The server holds one for current_job and one for each
     * ring slot; a submit handler holds one for as long as it is reading the
     * job. Destroyed when the last is dropped.
     *
     * Without this a submit read a job the tip watcher had already freed:
     * find_job() released its lock before returning the pointer, and
     * retire_job() frees on every new template. On a chain where templates
     * arrive several times a second the ring turns over in seconds, so the
     * window was wide open — it produced blocks_found rows carrying a freed
     * job's height and value (0, 2, 550 and rewards of 1.29M BTC on the
     * production pool), and a garbage network_target_be can make any hash
     * look like a solved block. */
    _Atomic int refs;
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
    /* Set before anything can `goto fail`: the failure path releases, and a
     * count of 0 there would decrement past zero and leak instead of free. */
    atomic_init(&j->refs, 1);
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

/* Take a reference. Callers must hold whichever lock protects the pointer
 * they are reading it from, so the job cannot be destroyed between the load
 * and the increment. */
static void stratum_job_retain(stratum_job_t *j) {
    if (j) atomic_fetch_add_explicit(&j->refs, 1, memory_order_relaxed);
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
    stratum_job_retain(j);
    return j;
}

/* Drop one reference; deallocate at zero. Named "free" because that is what it
 * is to every caller outside this file — a job handed back is a job released.
 *
 * acq_rel on the decrement and an acquire fence before destroying: every other
 * holder's writes must be visible to whichever thread happens to run the
 * destructor. Taken from upstream 440eb60. */
void stratum_job_free(stratum_job_t *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1) return;
    atomic_thread_fence(memory_order_acquire);
    job_destroy(j);
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

    /* Wall clock at startup. Job ids carry the wall-clock ms they were minted
     * at, and this is what separates "an id we issued this run and retired"
     * from "an id issued by the process that ran before this one" — the second
     * is not a retention problem and must not be counted as one. */
    uint64_t start_ms;

    /* Derived once from cfg.pool_mode so the render path does not string-compare
     * per share. Mirrors cfg.pps_enabled, which main.c derives the same way. */
    int prop_enabled;

    /* One entry per bound port. Each carries the difficulty policy the
     * connections it accepts inherit, so the port a miner dials decides what
     * difficulty it is served at. Slot 0 is always cfg.bind_port on the
     * server-wide defaults. */
    struct stratum_listener_slot {
        stratum_server_t  *srv;
        stratum_listener_t pol;
        int                fd;
        pthread_t          thr;
        int                thr_started;
    } listeners[STRATUM_MAX_LISTENERS];
    int  listener_count;

    atomic_int  stop;
    atomic_int  conn_count;

    /* Seeded from the clock at startup (so values differ across restarts) and
     * incremented per subscribe, which is what makes each connection's
     * extranonce1 distinct. Do not mix it with the clock again at use.
     *
     * Per-server again, and correct this time: there is exactly one server in
     * the process now. It was hoisted into a shared object only because two
     * servers each seeding from the clock handed out overlapping values. */
    atomic_uint extranonce1_seq;

    /* Share dedupe, keyed on the resulting block-header hash. The
     * per-connection ring in stratum_conn cannot catch a duplicate that
     * arrives on a *different* connection. Keying on the final hash makes the
     * check independent of how the submission was framed (job id,
     * extranonce2, version rolling). */
    pthread_mutex_t share_dedupe_lock;
    uint64_t        share_dedupe[SHARE_DEDUPE_RING];
    size_t          share_dedupe_head;

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
    /* ⛔ No thread handle here on purpose. The connection's thread is created
     * ALREADY DETACHED and owns this struct's lifetime — it frees it. A handle
     * stored here could only be written by the listener AFTER the struct was
     * published, which is a write into memory the thread may already have
     * freed (INC-002). Nothing joins a connection thread; only listener
     * threads are joined, and those keep their handles in stratum_listener_slot. */

    /* Peer address, filled at accept(). Recorded so a miner's complaint can
     * be tied to the socket it actually came from: worker names are chosen by
     * the miner and several connections routinely share one, so the name
     * alone cannot answer "did their proxy ever reach us?". */
    char     peer_ip[INET6_ADDRSTRLEN];

    uint8_t  extranonce1[STRATUM_EXTRANONCE1_SIZE];
    double   difficulty;

    /* The difficulty policy of the listener this connection arrived on,
     * copied at accept() so nothing downstream has to know which port it
     * came from. Zero means "use the server-wide default". */
    double   pol_initial_diff;
    double   pol_vardiff_min;
    double   pol_vardiff_max;
    /* ⛔ NO pol_min_diff. Upstream carries a per-listener floor that outranks
     * the network-difficulty ceiling, so a port keeps the difficulty it
     * advertised to a marketplace even when the chain is easier. We take the
     * field in the config (for parity) and deliberately do NOT honour it.
     *
     * The reason is arithmetic, not caution. If the share target D exceeds the
     * network target N, every share that arrives IS a block — but shares arrive
     * at H/(D·2^32) instead of H/(N·2^32), because the miner discards anything
     * above the stratum target locally and never submits it. Block discovery
     * falls by exactly N/D. On a chain whose difficulty is briefly in the
     * thousands — a minimum-difficulty window after a fork, which is precisely
     * when this pool most wants to be mining — a rental port pinned at 500,000
     * would throw away blocks by two or three orders of magnitude.
     *
     * The marketplace risk the promise avoids is real but bounded: an order
     * cannot be served until the chain ramps back up, which we say plainly.
     * Trading certain block loss for that is the wrong direction, and it is
     * worst in the window that matters most. The network clamp stays
     * authoritative. See conn_vardiff_min for the floor we DO honour. */
    int      pol_port;
    char     pol_label[32];
    /* 1 when this connection arrived on a solo listener. Read on the render
     * path and passed to the share observer. Like the other pol_ fields it is
     * copied from the listener ONCE at accept time and never re-read, so a
     * config reload cannot move a live connection between payout schemes
     * mid-session. */
    int      pol_solo;

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
     * and `difficulty` is multiplied/divided to converge on the target.
     *
     * vd_window_min_achieved is the smallest difficulty any share in the
     * window actually achieved (HUGE_VAL until the first share lands). It
     * detects a miner whose own local difficulty floor sits far above what
     * we assigned it — see vardiff_maybe_retarget. */
    uint64_t vd_window_start_ms;
    uint32_t vd_window_shares;
    double   vd_window_min_achieved;

    /* The highest difficulty any job in this window was NOTIFIED under, which
     * is not the same as the difficulty in force now: a retarget sends
     * set_difficulty without re-notifying, so shares for jobs the miner
     * already holds keep arriving mined against the older, higher number.
     * The floor detector has to judge them against what they were mined
     * under, or a legitimate downward retarget looks like a miner filtering
     * at a floor above its assignment. 0 until the first share lands. */
    double   vd_window_max_assigned;

    /* Shares in this window that were mined under a DIFFERENT difficulty than
     * the one now in force. Not counted as rate evidence (see handle_submit);
     * kept so the retarget log can say why a window was thin instead of
     * leaving it looking like a miner that stopped. */
    uint32_t vd_window_stale_diff_shares;

    /* Difficulty this miner asked for, via the stratum password `d=<n>` or
     * mining.suggest_difficulty. 0 = none requested.
     *
     * ⚠️ This is a FLOOR, never a pin. Vardiff may raise the connection
     * above it and the network-difficulty clamp still wins over it, but
     * nothing lowers the connection below it.
     *
     * A pin would be a denial-of-service hole: `d=1` from a 400 TH/s miner
     * is ~93,000 shares/sec aimed at the share pipeline. As a floor the
     * same request is inert — max(1, whatever vardiff chose) is just
     * vardiff's answer — while a miner asking to go HIGHER, which is the
     * real request, gets exactly what it asked for.
     *
     * The reason a miner needs this at all: vardiff tunes each CONNECTION
     * toward vardiff_target_spm, but a proxied fleet spreads one rig over
     * many connections, so the rig sees target_spm x N. At a uniform
     * difficulty the rig's total share rate is H/(D*2^32) — the connection
     * count cancels — so letting the miner name D is the one lever that
     * works regardless of how its hashrate is split. */
    double   requested_min_diff;

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

    /* Submit-rate ceiling state (max_submits_per_sec). Touched only by this
     * connection's own thread, inside handle_submit, so it needs no lock —
     * unlike the vardiff set above, the tip watcher never reads it. */
    uint64_t rl_window_start_ms;
    uint32_t rl_window_count;
    uint32_t rl_limited;         /* refused since the last report */
    uint64_t rl_limited_total;   /* refused on this connection, ever */
    uint64_t rl_reported_ms;

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

/* A job id stamped further ahead than this is not one of ours whatever it
 * parses as. Generous on purpose: the point is to exclude a foreign id, not
 * to police our own clock. */
#define JOB_ID_FUTURE_SLACK_MS 300000u

/* Documented in stratum.h. Pure, and public only so a test can reach it. */
const char *stratum_classify_job_id(uint64_t server_start_ms,
                                    uint64_t now_wall_ms,
                                    const char *job_id, int64_t *age_ms_out)
{
    if (age_ms_out) *age_ms_out = STRATUM_JOB_AGE_NONE;
    if (!job_id || !*job_id) return STRATUM_REJECT_KIND_NEVER_ISSUED;

    const char *dash = strchr(job_id, '-');
    if (!dash || dash == job_id || !dash[1])
        return STRATUM_REJECT_KIND_NEVER_ISSUED;

    uint64_t ms = 0;
    for (const char *q = job_id; q < dash; ++q) {
        int nib = hex_nib(*q);
        if (nib < 0) return STRATUM_REJECT_KIND_NEVER_ISSUED;
        /* An id long enough to overflow is not one we minted: refuse it
         * rather than wrap into a plausible-looking timestamp. */
        if (ms > (UINT64_MAX >> 4)) return STRATUM_REJECT_KIND_NEVER_ISSUED;
        ms = (ms << 4) | (uint64_t)nib;
    }
    for (const char *q = dash + 1; *q; ++q)
        if (hex_nib(*q) < 0) return STRATUM_REJECT_KIND_NEVER_ISSUED;

    if (ms > now_wall_ms + JOB_ID_FUTURE_SLACK_MS)
        return STRATUM_REJECT_KIND_NEVER_ISSUED;
    if (ms < server_start_ms) return STRATUM_REJECT_KIND_PRE_RESTART;

    if (age_ms_out) *age_ms_out = (int64_t)now_wall_ms - (int64_t)ms;
    return STRATUM_REJECT_KIND_EVICTED;
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

/* Is PPS accrual currently suspended? While it is, work handed to this pool
 * earns nothing, so the pool says so rather than banking it silently. */
static int pps_gated(const stratum_server_t *s) {
    return s->cfg.pps_enabled && s->cfg.pps_refuse_shares_below_min &&
           s->cfg.pps_gate &&
           atomic_load_explicit(s->cfg.pps_gate, memory_order_relaxed) != 0;
}

#define PPS_GATED_MSG \
    "pool is not crediting shares right now: network difficulty is below " \
    "the minimum this pool will pay PPS at. Point your miner elsewhere " \
    "until it retargets."

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

/* Find a job by id in the current slot or the recent ring.
 *
 * Returns a COUNTED reference: the caller owns it and must
 * stratum_job_free() it. The retain happens under the same lock that guards
 * the slot, so the job cannot be destroyed between finding it and claiming
 * it. Returning a borrowed pointer here — the previous behaviour — meant a
 * submit could still be reading a job that retire_job() had freed on the tip
 * watcher thread, which is a use-after-free in a network-facing path. */
static stratum_job_t *find_job(stratum_server_t *s, const char *job_id) {
    if (!job_id) return NULL;
    /* current */
    pthread_rwlock_rdlock(&s->job_lock);
    stratum_job_t *cur = s->current_job;
    if (cur && strcmp(cur->job_id, job_id) == 0) {
        stratum_job_retain(cur);
        pthread_rwlock_unlock(&s->job_lock);
        return cur;
    }
    pthread_rwlock_unlock(&s->job_lock);
    /* recent */
    pthread_mutex_lock(&s->recent_lock);
    for (size_t i = 0; i < RECENT_JOBS; ++i) {
        if (s->recent[i] && strcmp(s->recent[i]->job_id, job_id) == 0) {
            stratum_job_t *r = s->recent[i];
            stratum_job_retain(r);
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
    } else if (s->prop_enabled && !c->pol_solo &&
               job->payouts && job->n_payouts > 0 &&
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
         * directly is correct and non-custodial, just not yet proportional.
         *
         * It is ALSO the deliberate path for a SOLO connection (pol_solo),
         * which is why the proportional branch above excludes them: solo means
         * exactly "this miner's coinbase pays this miner", which is what this
         * branch already did. The operator fee still applies -- fee_bps is
         * passed here just as it is everywhere else -- so a solo block is
         * split miner + fee, and verify-split.py needs no new coinbase shape. */
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

/* How often a connection over its submit ceiling says so, rather than once
 * per refused share — one log line and one reject row per interval whatever
 * the rate, because the alternative writes the flood into the database that
 * exists to account for shares. INC-002 put ~1.95M reject rows in the live DB
 * in one hour and took every frontend query to ~6 s. */
#define RL_REPORT_INTERVAL_MS 10000

/* Has this connection used up its submits for the current second?
 *
 * Called before anything expensive, so a flood costs a JSON parse and a reply
 * instead of a coinbase render and four SHA256 passes. Returns non-zero when
 * the submit must be refused, and counts it for the periodic report.
 *
 * Ported by hand from upstream `959009d` rather than cherry-picked: our tree
 * already carried the config key (written, never read — the enforcement half
 * was lost in a merge, which is why the ceiling was inert during INC-002), and
 * the commit's other hunks collide with our per-job difficulty ring. */
static int submit_rate_exceeded(stratum_server_t *s, stratum_conn_t *c,
                                uint64_t now_mono) {
    int limit = s->cfg.max_submits_per_sec;
    if (limit <= 0) return 0;              /* 0 disables the ceiling */
    if (now_mono - c->rl_window_start_ms >= 1000) {
        c->rl_window_start_ms = now_mono;
        c->rl_window_count = 0;
    }
    if (c->rl_window_count < (uint32_t)limit) {
        c->rl_window_count++;
        return 0;
    }
    c->rl_limited++;
    c->rl_limited_total++;
    return 1;
}

/* Say once per RL_REPORT_INTERVAL_MS that this connection is over its ceiling,
 * carrying the count of everything refused since the last time.
 *
 * The message names the difficulty, because that is usually the actual fault:
 * a legitimate connection only reaches this rate when what it was assigned is
 * far below what its hashrate warrants and vardiff is still climbing. A flood
 * reaches it because it is a flood. */
static void submit_rate_report(stratum_server_t *s, stratum_conn_t *c,
                               uint64_t now_mono) {
    if (c->rl_limited == 0) return;
    if (c->rl_reported_ms != 0 &&
        now_mono - c->rl_reported_ms < RL_REPORT_INTERVAL_MS) return;

    LOG_WARN("stratum: %s is over the submit ceiling of %d/s — %u submit(s) "
             "refused since the last report, %llu on this connection. At "
             "difficulty %g its hashrate is producing more shares than the "
             "pool will take; vardiff is raising it",
             c->worker_name[0] ? c->worker_name : "(unauthorized)",
             s->cfg.max_submits_per_sec, c->rl_limited,
             (unsigned long long)c->rl_limited_total, c->difficulty);
    if (s->cfg.on_reject) {
        char msg[192];
        snprintf(msg, sizeof msg,
                 "submitting too fast: %u refused at over %d/s (%llu total)",
                 c->rl_limited, s->cfg.max_submits_per_sec,
                 (unsigned long long)c->rl_limited_total);
        /* Wall clock here, not the monotonic value the interval is measured
         * with: this one is a timestamp that gets stored and read back. */
        s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                         msg, NULL, STRATUM_JOB_AGE_NONE);
    }
    c->rl_limited = 0;
    c->rl_reported_ms = now_mono;
}

/* How many shares a window needs before its minimum achieved difficulty is
 * treated as evidence of the miner's own floor rather than small-sample
 * noise, how far above the assigned difficulty that floor has to sit before
 * we act, and how close under the observed floor we then aim. */
#define VD_FLOOR_MIN_SAMPLES 5
#define VD_FLOOR_TRIGGER     4.0
#define VD_FLOOR_BACKOFF     0.95

/* Vardiff: every cfg.vardiff_window_sec, look at how many shares the
 * connection submitted in that window and rescale its difficulty so the
 * rate converges on cfg.vardiff_target_spm shares/minute. Called from
 * handle_submit() after each accepted share.
 *
 * Conservative algorithm:
 *   ratio = observed_spm / target_spm
 *   if  ratio in [0.5, 2.0] → leave it (avoid jitter)
 *   else                    → new_diff = old_diff * ratio, clamped
 *
 * The rate loop alone cannot correct a miner that enforces its own local
 * difficulty floor above the difficulty we assigned it. Below that floor the
 * miner's submission rate does not depend on our difficulty at all — it
 * submits whatever beats its own target — so the loop has no gradient to
 * follow, and any share rate that happens to land inside the deadband is a
 * fixed point. A miner floored at 256 while assigned 1 therefore sits at 1
 * forever, and since a share is credited at the difficulty WE assigned rather
 * than the one it achieved, the pool books 1/256th of the work it actually
 * received: a hashrate estimate 256x low, and on a pps-classic pool a payout
 * 256x short.
 *
 * ⚠️ Do NOT "correct" this to say share_diff = c->difficulty. It used to, and
 * an earlier revision of this comment still said so; handle_submit now credits
 * `judge_diff`, the difficulty of the share's OWN JOB (2df373a6 — judging a
 * submit at the connection's current difficulty was the pool's single largest
 * source of miner complaints). The argument above is unaffected either way,
 * because the job's difficulty is still a difficulty the pool assigned — just
 * at job time rather than connection-current. The distinction matters only to
 * whoever reads this next: matching the code to this sentence would undo that
 * fix. → feedback_copied-is-not-honoured
 *
 * So alongside the rate loop, watch the difficulty the shares actually
 * achieve. Every share in the window achieving far more than we asked for is
 * a direct measurement of that floor, with none of the rate loop's
 * ambiguity, so raise the assigned difficulty to just under it.
 *
 * Aim just under the observed minimum — but only just, because the two ways
 * of missing the floor are not symmetric. Land above it and the accounting
 * stays exact: the miner keeps submitting everything that beats its own
 * floor, the pool accepts the fraction that also beats the assigned
 * difficulty, and crediting each of those at the assigned value books exactly
 * the work performed. Land below it and every share is accepted but credited
 * at less than it achieved, which is the under-crediting this whole check
 * exists to remove, just at a smaller factor. Overshooting costs the miner
 * rejected submissions; undershooting costs it money.
 *
 * So a small backoff, not a generous one. The observed minimum already sits
 * above the floor — a share clearing floor D achieves D/u for u uniform on
 * (0,1], so the smallest of n of them lands near D*(n+1)/n — which is what
 * leaves the result hovering around the floor instead of well under it.
 * Correcting that overshoot away would center the estimate and, by the
 * asymmetry above, credit worse.
 *
 * VD_FLOOR_TRIGGER is what stops this from ratcheting. Assigning above the
 * floor does raise the next window's observed minimum, but the next retarget
 * only fires if that minimum still clears four times the assigned difficulty,
 * which after a correct jump it does not. It is also what a well-matched
 * miner has to clear on every share in a window to trip this by chance, which
 * at five shares is about one window in thirty thousand — and costs it only a
 * difficulty briefly set too high, which the rate loop then walks back down
 * and which credits it correctly meanwhile.
 *
 * Note this deliberately does not count rejected submissions toward the
 * window. A stale or unknown-job reject is not work at the assigned
 * difficulty, and a low-difficulty reject is by definition work that missed
 * it; feeding either into the rate would push the difficulty up on miners
 * whose real problem is job latency.
 *
 * Always emits a single mining.set_difficulty when diff changes. The
 * client picks it up for the next job notify; we don't force a re-notify
 * because handle_submit keeps accepting shares at the old difficulty for
 * a grace period (diff_grace_ms). */
/* CALLER MUST HOLD c->state_lock. */
/* The vardiff bounds IN FORCE FOR THIS CONNECTION.
 *
 * ⛔ Under the old two-server model the rental port had its own stratum_cfg_t,
 * so `s->cfg.vardiff_min` WAS the rental floor for a rental connection. With
 * one server serving every port there is a single cfg, and reading it directly
 * silently drops the per-port policy: a rental miner would start at 500,000 and
 * then be dragged by vardiff down to the home-miner floor — which is exactly
 * the invalid-share ramp a marketplace cancels an order for. Every bound check
 * must go through here. */
static double conn_vardiff_min(const stratum_server_t *s, const stratum_conn_t *c) {
    return (c && c->pol_vardiff_min > 0.0) ? c->pol_vardiff_min : s->cfg.vardiff_min;
}

static double conn_vardiff_max(const stratum_server_t *s, const stratum_conn_t *c) {
    return (c && c->pol_vardiff_max > 0.0) ? c->pol_vardiff_max : s->cfg.vardiff_max;
}

static void vardiff_maybe_retarget(stratum_server_t *s, stratum_conn_t *c,
                                   uint64_t now,
                                   char **buf, size_t *len)
{
    if (!s->cfg.vardiff_enabled) return;
    if (c->vd_window_start_ms == 0) {
        c->vd_window_start_ms = now;
        c->vd_window_shares = 0;
        c->vd_window_min_achieved = HUGE_VAL;
        c->vd_window_max_assigned = 0.0;
        c->vd_window_stale_diff_shares = 0;
        return;
    }
    uint64_t elapsed_ms = now - c->vd_window_start_ms;
    uint64_t window_ms  = (uint64_t)s->cfg.vardiff_window_sec * 1000ULL;
    if (elapsed_ms < window_ms) return;

    /* ⛔ A WINDOW WITH ONLY STALE-DIFFICULTY SHARES IS DRAINING, NOT IDLE.
     *
     * The miner is demonstrably submitting; we simply have no evidence yet
     * about its rate at the CURRENT difficulty, because its pipeline still
     * holds jobs issued before the last retarget. Falling through here reads
     * observed_spm = 0, takes the idle step, and halves the difficulty every
     * ceiling period -- all the way to vardiff_min -- while the miner works
     * perfectly. A downward runaway, and the exact mirror of the upward one
     * this whole change exists to stop.
     *
     * 🔴 REACHABLE ONLY SINCE THE RETENTION WINDOW WENT 60s -> 300s (d0dca6d).
     * The ceiling here is vardiff_window_sec * vardiff_max_window_mult = 240s,
     * and 240 < 300 opens a band where a job is old enough to starve the window
     * and still young enough for its shares to be accepted. At the old 60s TTL
     * that band was empty: such a job was already evicted and the share came
     * back a stale reject instead. Neither change is wrong alone. Together they
     * open this, which is why it took reviewing the COMBINATION to find.
     * Reproduced walking 3e-9 -> 1.5e-9 -> 7.5e-10 -> 3.75e-10 (claude-11).
     *
     * Restart the window rather than merely returning, so the post-drain
     * measurement is taken over a clean interval instead of one padded with
     * dead time. ⚠️ A genuinely idle miner has BOTH counters at zero, does not
     * match here, and still takes its idle step -- that path is untouched.
     *
     * 📌 The predicate is `stale > current`, not `current == 0`. A PURE drain
     * was the runaway; a MIXED window is the same fault one notch milder and
     * was still open with the narrower test: one or two current shares plus a
     * stale stream does not look empty, so it runs to the 240s ceiling and then
     * divides those two shares by an elapsed that is mostly drain dead-time --
     * 2 over 240s reads 0.5 spm against a target of 12, ratio 0.04, and it
     * takes the idle step down while the miner is working fine. Bounded where
     * the pure case was not (one 2x step, self-correcting on the next clean
     * window) but real, and only real since the TTL went to 300s.
     * Majority-drain is the honest line: if most of what arrived was mined
     * under a difficulty we are no longer assigning, the window is not a
     * measurement of the current one. (claude-21 found this band reviewing the
     * narrower guard.)
     *
     * ⚠️ Bounded by construction: stale shares can only come from jobs issued
     * before the last retarget, and those die at RECENT_JOB_TTL_MS -- after
     * which their shares are refused outright and stop arriving. So deferral
     * cannot outlast the retention window. */
    if (c->vd_window_stale_diff_shares > c->vd_window_shares) {
        c->vd_window_start_ms = now;
        c->vd_window_stale_diff_shares = 0;
        c->vd_window_min_achieved = HUGE_VAL;
        c->vd_window_max_assigned = 0.0;
        return;
    }

    /* A window's share RATE is only meaningful if the window holds enough
     * shares to measure one. At target_spm=12 and a 30s window an on-target
     * connection produces six, and Poisson noise on six samples is +/-41%
     * (1/sqrt(6)) — so `ratio` leaves the [0.5, 2.0] deadband on noise alone
     * and the controller oscillates around the right answer instead of
     * settling on it. Observed in production 2026-08-24: one worker cycling
     * 500000 -> 1141329 -> 569753 -> 500000 within four minutes.
     *
     * So: keep accumulating rather than steering on noise. The extension is
     * bounded, because a connection whose difficulty is genuinely far too
     * high submits almost nothing and must still be able to ratchet down.
     *
     * Leaving the window OPEN (no reset) is the whole mechanism — return
     * before the reset below. */
    if (s->cfg.vardiff_min_samples > 0 &&
        c->vd_window_shares < (uint32_t)s->cfg.vardiff_min_samples) {
        int mult = s->cfg.vardiff_max_window_mult > 0
                     ? s->cfg.vardiff_max_window_mult : 8;
        if (elapsed_ms < window_ms * (uint64_t)mult) return;
    }

    /* Observed shares per minute over this window. */
    double observed_spm = ((double)c->vd_window_shares * 60000.0) /
                          (double)elapsed_ms;
    double target_spm = s->cfg.vardiff_target_spm;
    double ratio = observed_spm / target_spm;

    double old_diff = c->difficulty;
    double new_diff = old_diff;
    if (ratio > 2.0 || ratio < 0.5) {
        new_diff = old_diff * ratio;
        /* Cap each adjustment. A window that met the sample floor is trusted
         * with the historical 4x step; one that only ended because it hit the
         * extension limit is not, and gets the gentler idle step.
         *
         * That distinction is what keeps a proxied rental customer off the
         * floor. One rig arrives as many connections, each going quiet
         * between bursts; at 4x a pair of empty windows cuts difficulty 16x
         * and pins the worker to vardiff_min, which is exactly the state the
         * miner's own firmware then reports as "difficulty too low". */
        double max_step = 4.0;
        if (s->cfg.vardiff_min_samples > 0 &&
            c->vd_window_shares < (uint32_t)s->cfg.vardiff_min_samples) {
            max_step = s->cfg.vardiff_idle_step > 1.0
                         ? s->cfg.vardiff_idle_step : 2.0;
        }
        if (new_diff > old_diff * max_step) new_diff = old_diff * max_step;
        if (new_diff < old_diff / max_step) new_diff = old_diff / max_step;
    }

    /* Every share this window cleared a difficulty far above the one we
     * assigned: the miner is filtering locally at a floor of its own. Raise
     * to just under the floor we measured. Uncapped by the 4x step above —
     * that cap damps an extrapolation from a share rate, whereas this is a
     * value we watched every share in the window exceed.
     *
     * The comparison is never against the rate loop's proposal: an accepted
     * share always achieves at least the difficulty it was accepted under, so
     * testing the window minimum against a proposal just cut by 4x would fire
     * on every such cut and pin the difficulty of every miner that legitimately
     * slowed down.
     *
     * ⛔ old_diff alone is not enough either, and the difference is not
     * hypothetical. A retarget sends set_difficulty WITHOUT re-notifying, by
     * design — every job already carries the difficulty it went out under — so
     * after a cut, shares for jobs the miner still holds keep arriving mined
     * against the OLD, higher number. Judge those against old_diff and the
     * window minimum clears it by construction. The collision is exact: the
     * rate loop's cut is capped at 4x and VD_FLOOR_TRIGGER is 4.0, so a full
     * cut leaves the previous difficulty sitting precisely on the trigger, and
     * the floor then undoes the cut the rate loop just made. Compare against
     * the highest difficulty the window's shares were actually mined under.
     *
     * floor_basis >= old_diff always, so this can only make the check fire
     * less — it cannot introduce a floor trigger where there was none. */
    double floor_basis = c->vd_window_max_assigned > old_diff
                       ? c->vd_window_max_assigned : old_diff;

    /* The floor detector needs the SAME sample floor the rate loop needs,
     * for the same reason. VD_FLOOR_MIN_SAMPLES alone reads vd_window_shares
     * directly, so it is unaffected by vardiff_min_samples — and a window
     * that ended at the vardiff_max_window_mult extension cap holds somewhere
     * between the two. In that band the trigger costs 0.25^n per window
     * (1/1024 at five samples), not the 0.25^20 the sample floor is meant to
     * buy, and the band is populated by exactly the proxied rental
     * connections the sample floor was written for.
     *
     * Requiring the full sample floor costs detection nothing: a miner
     * enforcing a local floor submits at a rate set by THAT floor, not by the
     * difficulty we assigned, so it fills its windows fast — the production
     * case upstream found ran 12-19 spm against a target of 12. A false
     * trigger, by contrast, raises UNCAPPED and is walked back at a capped
     * step, so it costs more to undo than it cost to cause. */
    uint32_t floor_min_samples = VD_FLOOR_MIN_SAMPLES;
    if (s->cfg.vardiff_min_samples > (int)floor_min_samples) {
        floor_min_samples = (uint32_t)s->cfg.vardiff_min_samples;
    }

    int from_floor = 0;
    if (c->vd_window_shares >= floor_min_samples &&
        isfinite(c->vd_window_min_achieved) &&
        c->vd_window_min_achieved > floor_basis * VD_FLOOR_TRIGGER) {
        double floor_diff = c->vd_window_min_achieved * VD_FLOOR_BACKOFF;
        if (floor_diff > new_diff) {
            new_diff = floor_diff;
            from_floor = 1;
        }
    }

    if (new_diff != old_diff) {
        double vd_min = conn_vardiff_min(s, c), vd_max = conn_vardiff_max(s, c);
        if (new_diff < vd_min) new_diff = vd_min;
        if (new_diff > vd_max) new_diff = vd_max;
        /* A miner-requested difficulty is a floor: vardiff may raise this
         * connection above it, never below. Without this the request lasts
         * exactly one window — vardiff sees a rate under target (which is
         * the POINT of a higher difficulty) and drags it straight back
         * down. */
        if (c->requested_min_diff > 0.0 && new_diff < c->requested_min_diff) {
            new_diff = c->requested_min_diff;
        }
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

    double window_floor = c->vd_window_min_achieved;

    /* Reset the window regardless of whether we changed diff. */
    c->vd_window_start_ms = now;
    c->vd_window_shares = 0;
    c->vd_window_stale_diff_shares = 0;
    c->vd_window_min_achieved = HUGE_VAL;
    c->vd_window_max_assigned = 0.0;

    if (new_diff != old_diff) {
        c->difficulty = new_diff;
        c->prev_difficulty = old_diff;
        c->diff_changed_ms = now;
        if (from_floor) {
            LOG_INFO("stratum: vardiff %s: %.0f -> %.0f (miner floor: every "
                     "share in the window cleared difficulty %.0f, %.1f spm "
                     "observed)",
                     c->worker_name, old_diff, new_diff, window_floor,
                     observed_spm);
        } else {
            LOG_INFO("stratum: vardiff %s: %.0f -> %.0f (%.1f spm observed, %.1f target)",
                     c->worker_name, old_diff, new_diff, observed_spm, target_spm);
        }
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
 * ratchets down until it can produce shares again.
 *
 * ⚠️ The rate is `vardiff_idle_step` (default 2.0) per EXTENDED window --
 * vardiff_window_sec x vardiff_max_window_mult -- not 4x per vardiff window,
 * which this comment claimed until 2026-08-29. On the live config (30 s x 8,
 * step 2.0) that is one halving per 240 s: a solo connection started at
 * 65,536 reaches a 1,024 floor in six halvings, about 24 MINUTES. The old
 * wording overstated recovery by roughly 8x in wall-clock terms, and it is
 * the comment anyone sizing a starting difficulty would reason from. */
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
    unsigned seq = atomic_fetch_add(&s->extranonce1_seq, 1);
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

/* Pull a difficulty request out of a stratum password field.
 *
 * The near-universal convention is `d=<number>`, optionally among other
 * comma- or semicolon-separated tokens (`x`, `d=4657000`, ...). cgminer,
 * bosminer, Vnish and LuxOS all let the operator set this field, which is
 * why it is the request channel with the widest reach.
 *
 * Returns 0 and writes *out on success, -1 if the field carries no usable
 * `d=` token. */
static int parse_password_diff(const char *pw, double *out) {
    if (!pw || !out) return -1;
    for (const char *p = pw; *p; ++p) {
        /* Match `d=` only at a token boundary, so "id=7" is not a request. */
        if ((p != pw) && p[-1] != ',' && p[-1] != ';' && p[-1] != ' ') continue;
        if (p[0] != 'd' || p[1] != '=') continue;
        char *end = NULL;
        double v = strtod(p + 2, &end);
        if (end == p + 2) return -1;              /* `d=` with no number */
        if (!(v > 0.0) || v != v) return -1;      /* <= 0, or NaN */
        *out = v;
        return 0;
    }
    return -1;
}

/* Apply a miner's requested difficulty as a floor on this connection.
 * Clamped to cfg.max_suggested_diff, then to the network difficulty — a
 * share target harder than the network target makes the miner discard
 * valid blocks locally before the pool ever sees them.
 *
 * CALLER MUST HOLD c->state_lock. */
static void apply_requested_diff(stratum_server_t *s, stratum_conn_t *c,
                                 double req) {
    if (!(req > 0.0)) return;
    /* <= 0 disables miner requests entirely -- the deploy gate. It still
     * LOGS what was asked for, so a stage that has the feature switched off
     * measures how many miners already send `d=` out of habit from other
     * pools before we let it change anything. Measure, then enable. */
    if (s->cfg.max_suggested_diff <= 0.0) {
        LOG_INFO("stratum: %s requested difficulty %.0f — requests DISABLED "
                 "(max_suggested_diff <= 0), ignoring",
                 c->worker_name[0] ? c->worker_name : "(unauthorized)", req);
        return;
    }
    if (req > s->cfg.max_suggested_diff) {
        LOG_INFO("stratum: %s requested difficulty %.0f above the cap %.0f — "
                 "using the cap",
                 c->worker_name[0] ? c->worker_name : "(unauthorized)",
                 req, s->cfg.max_suggested_diff);
        req = s->cfg.max_suggested_diff;
    }
    double net_diff = current_net_diff(s);
    if (net_diff > 0.0 && req > net_diff) req = net_diff;
    c->requested_min_diff = req;
    if (c->difficulty < req) c->difficulty = req;
}

/* mining.suggest_difficulty: params[0] is the difficulty the miner wants.
 * The formal stratum way to ask; `d=` in the password is the same request
 * from firmware that cannot send this. Either may arrive before or after
 * authorize, so this only records and applies — authorize re-applies it.
 *
 * No response is defined for this method, but answering `true` to a request
 * carrying an id is harmless and keeps strict clients happy. */
static int handle_suggest_difficulty(stratum_server_t *s, stratum_conn_t *c,
                                     cJSON *id, cJSON *params,
                                     char **buf, size_t *len) {
    double req = 0.0;
    if (cJSON_IsArray(params) && cJSON_GetArraySize(params) >= 1) {
        cJSON *d = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsNumber(d)) req = d->valuedouble;
    }
    if (!(req > 0.0)) {
        cJSON *err = make_error(20, "bad params");
        return emit_response(buf, len, id, NULL, err);
    }
    pthread_mutex_lock(&c->state_lock);
    double before = c->difficulty;
    apply_requested_diff(s, c, req);
    double after = c->difficulty;
    pthread_mutex_unlock(&c->state_lock);
    LOG_INFO("stratum: %s suggested difficulty %.0f -> floor %.0f",
             c->worker_name[0] ? c->worker_name : "(unauthorized)",
             req, c->requested_min_diff);
    /* Only tell an ALREADY-authorized miner; before authorize it has no job
     * yet, and authorize emits the difficulty itself. */
    if (c->authorized && after != before) send_set_difficulty(buf, len, after);
    if (id) emit_response(buf, len, id, cJSON_CreateTrue(), NULL);
    return 0;
}

static int handle_authorize(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                            cJSON *params, char **buf, size_t *len) {
    const char *worker = NULL;
    double pw_diff = 0.0;
    if (cJSON_IsArray(params) && cJSON_GetArraySize(params) >= 1) {
        cJSON *w = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsString(w)) worker = w->valuestring;
        /* params[1] is the password. Historically ignored here; it is the
         * `d=<n>` difficulty request channel. */
        if (cJSON_GetArraySize(params) >= 2) {
            cJSON *p = cJSON_GetArrayItem(params, 1);
            if (cJSON_IsString(p)) {
                double v;
                if (parse_password_diff(p->valuestring, &v) == 0) pw_diff = v;
            }
        }
    }
    if (!worker) {
        cJSON *err = make_error(24, "missing worker name");
        return emit_response(buf, len, id, NULL, err);
    }

    /* Username format: <address>[.<rig_label>]. The address part must be
     * a valid bech32 (P2WPKH / P2WSH), bech32m (P2TR) or base58check
     * (P2PKH / P2SH) Bitcoin address; the optional label is a free-form rig
     * identifier. */
    const char *dot = strchr(worker, '.');
    size_t addr_len = dot ? (size_t)(dot - worker) : strlen(worker);
    if (addr_len == 0 || addr_len >= sizeof(c->payout_address)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, worker, c->peer_ip, now_ms(),
                             "stratum username must start with a bitcoin address",
                             NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(24,
            "stratum username must be <bitcoin_address>[.<rig_label>]");
        return emit_response(buf, len, id, NULL, err);
    }
    /* Refuse before taking the address: the miner learns at connect time,
     * which is the only point at which they can still do something about it. */
    if (pps_gated(s)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, worker, c->peer_ip, now_ms(),
                             "pps accrual suspended (difficulty below floor)",
                             NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(24, PPS_GATED_MSG);
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
                s->cfg.on_reject(s->cfg.ctx, worker, c->peer_ip, now_ms(),
                                 rmsg, NULL, STRATUM_JOB_AGE_NONE);
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
                s->cfg.on_reject(s->cfg.ctx, worker, c->peer_ip, now_ms(),
                                 rmsg, NULL, STRATUM_JOB_AGE_NONE);
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
        double hint_floor = conn_vardiff_min(s, c);
        if (hint > 0.0 && hint_floor > 0.0 &&
            hint < hint_floor) {
            LOG_INFO("stratum: %s hint %.0f is below the vardiff floor %.0f — "
                     "starting at the floor",
                     c->worker_name, hint, hint_floor);
            hint = hint_floor;
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
    /* A difficulty the miner asked for wins over the replayed hint — the
     * hint is what it happened to be running at, the request is what it
     * says it wants. `mining.suggest_difficulty` may already have arrived
     * before authorize, in which case re-apply it: the hint above has just
     * overwritten c->difficulty. */
    {
        double req = pw_diff > 0.0 ? pw_diff : c->requested_min_diff;
        if (req > 0.0) {
            apply_requested_diff(s, c, req);
            LOG_INFO("stratum: %s requested difficulty %.0f (floor)",
                     c->worker_name, c->requested_min_diff);
            /* The hint is only what this worker happened to be running at;
             * the request is what it says it wants NOW. A fleet that has
             * shrunk since its history was recorded reconnects asking for a
             * fraction of its replayed difficulty — and a client handed work
             * far above its request treats it as invalid and drops the line
             * before vardiff can ever sample it down. Observed live: a
             * 17.94M replay against a 510k request looped a proxy through
             * authorize every 2 seconds indefinitely. So at authorize time
             * only, an explicit request also LOWERS past the hint — never
             * below the listener's own floor, which is what a marketplace
             * was promised. Mid-session requests keep floor-only semantics:
             * there vardiff has live shares to correct with. */
            if (hint > 0.0 && c->requested_min_diff > 0.0 &&
                c->difficulty > c->requested_min_diff) {
                double lf = conn_vardiff_min(s, c);
                double target = c->requested_min_diff;
                if (lf > 0.0 && target < lf) target = lf;
                if (c->difficulty > target) {
                    LOG_INFO("stratum: %s request %.0f wins over the replayed "
                             "hint %.0f — lowering to %.0f",
                             c->worker_name, c->requested_min_diff, hint,
                             target);
                    c->difficulty = target;
                }
            }
        }
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
    c->vd_window_min_achieved = HUGE_VAL;
    c->vd_window_max_assigned = 0.0;
    c->vd_window_stale_diff_shares = 0;

    /* respond true */
    emit_response(buf, len, id, cJSON_CreateTrue(), NULL);
    /* port= is the LISTENING port, not the peer's. With more than one server
     * in the process (public 3334, rental 3335) the peer address alone cannot
     * say which one a miner reached: proxy and datacenter IPs serve several
     * worker names at once, and one IP routinely holds connections on both
     * ports simultaneously. Share difficulty cannot substitute either --
     * vardiff on the public port climbs past the rental floor, and a
     * returning worker is hinted straight to a high difficulty. Without this
     * field, attributing a share to a port is guesswork. */
    LOG_INFO("stratum: authorized '%s' from %s (fd=%d, port=%d) at difficulty %.0f",
             c->worker_name, c->peer_ip[0] ? c->peer_ip : "?", c->fd,
             c->pol_port > 0 ? c->pol_port : s->cfg.bind_port, c->difficulty);
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
    uint64_t h = fnv1a_bytes(hash_be, 32);
    int dup = 0;
    pthread_mutex_lock(&s->share_dedupe_lock);
    for (size_t i = 0; i < SHARE_DEDUPE_RING; ++i) {
        if (s->share_dedupe[i] == h) { dup = 1; break; }
    }
    if (!dup) {
        s->share_dedupe[s->share_dedupe_head] = h;
        s->share_dedupe_head = (s->share_dedupe_head + 1) % SHARE_DEDUPE_RING;
    }
    pthread_mutex_unlock(&s->share_dedupe_lock);
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

/* The body of mining.submit, with `job` guaranteed live for the duration.
 *
 * Split from handle_submit so the reference find_job() hands back is released
 * on exactly one path. This function has nine exits, and a release on each is
 * a leak — or a double free — waiting for the next edit. */
static int submit_with_job(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                           cJSON *params, stratum_job_t *job,
                           const char *en2, const char *ntime,
                           const char *nonce, char **buf, size_t *len) {
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
            return emit_response(buf, len, id, NULL, err);
        }
        uint32_t mask = c->version_mask ? c->version_mask : VERSION_ROLLING_MASK;
        submit_version =
            (int32_t)(((uint32_t)job->version & ~mask) | (rolled & mask));
    }

    /* job->job_id rather than the submitted string: find_job matched them
     * exactly, and the job is the one thing here guaranteed to outlive the
     * call. */
    if (dedupe_check_and_add(c, job->job_id, en2, ntime, nonce,
                             (uint32_t)submit_version)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                             "duplicate share", NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(22, "duplicate share");
        return emit_response(buf, len, id, NULL, err);
    }

    uint32_t ntime_v, nonce_v;
    if (parse_u32_hex(ntime, &ntime_v) != 0 || parse_u32_hex(nonce, &nonce_v) != 0) {
        cJSON *err = make_error(20, "bad ntime/nonce hex");
        return emit_response(buf, len, id, NULL, err);
    }

    size_t en2_len = 0;
    uint8_t *en2_bytes = hex_to_bytes_alloc(en2, &en2_len);
    if (!en2_bytes) {
        cJSON *err = make_error(20, "bad extranonce2 hex");
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
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                             "bad extranonce2 size", NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(20, "bad extranonce2 size");
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
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                             "coinbase render failed", NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(25, "coinbase render failed");
        return emit_response(buf, len, id, NULL, err);
    }

    /* coinbase = cb1 || ex1 || ex2 || cb2 */
    size_t cb_len = c->cb1_len + STRATUM_EXTRANONCE1_SIZE + en2_len + c->cb2_len;
    uint8_t *cb = malloc(cb_len);
    if (!cb) {
        pthread_mutex_unlock(&c->state_lock);
        free(en2_bytes);
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
    const int have_job_diff = conn_job_diff_lookup(c, job->job_id, &job_diff);
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
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                             "low difficulty", NULL, STRATUM_JOB_AGE_NONE);
        }
        free(cb);
        cJSON *err = make_error(23, "low difficulty");
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
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                             "duplicate share", NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(22, "duplicate share");
        return emit_response(buf, len, id, NULL, err);
    }

    char block_hash_hex[65] = {0};
    /* Whether the NODE took the candidate onto the best chain. Only
     * meaningful when is_block. Defaults to accepted so a server with no
     * on_block hook (tests) behaves as before; every real path below assigns
     * it from the submission, including the assembly-failure path.
     *
     * ⚠️ That default is load-bearing in the other direction too: payout
     * settlement is gated on this flag (see on_block_found), so a production
     * server MUST install an on_block hook. main.c always does. */
    int  block_accepted = 1;
    char submit_err[REASON_TEXT_MAX] = {0};
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
            if (s->cfg.on_block) {
                int rc = s->cfg.on_block(s->cfg.ctx, block_hex,
                                         submit_err, sizeof submit_err);
                block_accepted = (rc == 0);
            }
            free(block_hex);
        } else {
            /* Nothing was submitted, so nothing can have been accepted.
             * Falling through as "found" here would file a block the node
             * was never even shown. */
            block_accepted = 0;
            snprintf(submit_err, sizeof submit_err, "block assembly failed");
        }
        if (!block_accepted) {
            LOG_WARN("stratum: candidate from '%s' at height %u was not "
                     "accepted: %s", c->worker_name, job->height, submit_err);
        }
    }
    free(cb);

    if (s->cfg.on_share) {
        /* Always pass the actual share hash so the dashboard can show the
         * hash of every share (and the user can eyeball its leading zeros
         * to gauge how lucky each share was). When is_block, this string
         * also IS the block hash; otherwise it's a 'just-a-share' hash. */
        s->cfg.on_share(s->cfg.ctx, c->worker_name, c->payout_address,
                        ts_now, share_diff, is_block, sent_hash_hex,
                        c->pol_solo);
    }
    /* Tick vardiff: count this accepted share toward the window, and track
     * the difficulty it actually achieved. Reading the hash as a target
     * gives exactly that: how much harder than difficulty 1 this solution
     * was. The running minimum is what exposes a miner filtering at a local
     * floor above its assigned difficulty.
     *
     * May emit a mining.set_difficulty notification if the window elapsed. */
    pthread_mutex_lock(&c->state_lock);
    /* 🔴 ONLY shares mined at the CURRENT difficulty measure the current rate.
     *
     * A retarget sends mining.set_difficulty WITHOUT re-notifying -- every job
     * already carries the difficulty it went out under -- so after a downward
     * step the miner keeps returning shares for jobs issued at the OLD, higher
     * difficulty. Counting those as evidence about the new difficulty inflates
     * the observed rate. Measured on this pool 2026-08-30: a connection at
     * 942,894, whose true rate there was ~15 spm, read **67 spm**; vardiff
     * stepped it 4x UP to 3,771,575, the next window then read 2-6 spm and it
     * stepped 4x back DOWN. 16 of 98 miners were cycling like this.
     *
     * ⛔ The 4x step cap does not damp that loop -- it SETS ITS AMPLITUDE.
     * `new_diff = old_diff * ratio` compounds a bad reading multiplicatively,
     * so the cap is the only thing bounding it.
     *
     * ckpool refuses the same share for the same reason (stratifier.c
     * add_submit: `if (diff != client->diff) { client->ssdc = 0; return; }`),
     * and credits it at MIN(current, old) rather than at face value. It is also
     * the reasoning upstream applied to the miner-floor check in 5a2e72c -- the
     * rate loop simply never got it.
     *
     * ⚠️ DELIBERATELY NOT resetting vd_window_start_ms here, though ckpool zeroes
     * its counter. It can afford to: a separate "240s since last diff change"
     * timer still forces an evaluation. Our only ceiling is
     * vardiff_max_window_mult, measured from window START -- so resetting the
     * start on every stale share would defer retargeting for as long as the
     * miner holds old jobs, which is precisely the connection most in need of
     * one. Leaving the clock running lets the existing min_samples +
     * max_window_mult logic wait out the drain (240s at defaults) and then act
     * on whatever genuinely current shares arrived. Job cadence is ~31s, so the
     * pipeline drains long before that ceiling. */
    /* 🔴 THE PREDICATE IS share_diff, NOT judge_diff, and the difference is a
     * real leak. judge_diff is the difficulty the share's JOB went out under;
     * share_diff is what the share was actually CREDITED at, and the grace path
     * above (~2149) reassigns it to prev_diff for a miner that applied a
     * set_difficulty one job late. Such a share is on a post-retarget job, so
     * judge_diff == c->difficulty and it would sail through a judge_diff gate —
     * while having been mined at the OLD, easier difficulty. That is the same
     * contamination as the down-leg bug, arriving through the up-leg door.
     * Gating on the credited difficulty closes both with one comparison.
     * (claude-21 found this reviewing the judge_diff version.)
     *
     * ⚠️ Consequence, accepted: line ~2205 reassigns share_diff to the NETWORK
     * target difficulty for a hash that is a block but misses the worker
     * target, so that share is excluded from the rate window too. It is one
     * share at block-discovery frequency and it is not rate evidence about the
     * assigned difficulty anyway. */
    if (share_diff == c->difficulty) {
        c->vd_window_shares++;
        double achieved = target_to_diff(hash_be);
        if (achieved < c->vd_window_min_achieved) {
            c->vd_window_min_achieved = achieved;
        }
    } else {
        c->vd_window_stale_diff_shares++;
    }
    /* judge_diff, not c->difficulty: this share was mined against the
     * difficulty ITS job went out under, which a retarget earlier in this
     * window may already have moved on from.
     *
     * 📌 Upstream calls this `assigned_diff`; our equivalent is `judge_diff`
     * (the per-job difficulty from conn_job_diff_lookup, falling back to the
     * connection's current one). Same quantity, different name — renamed here
     * rather than aliased so there is one name for it in this file. */
    if (judge_diff > c->vd_window_max_assigned) {
        c->vd_window_max_assigned = judge_diff;
    }
    vardiff_maybe_retarget(s, c, now_ms(), buf, len);
    pthread_mutex_unlock(&c->state_lock);
    /* ⛔ This fires for EVERY candidate, accepted or not, and `accepted` is
     * what separates the two. Recording a candidate and paying for one are
     * different acts, and only the second may be gated here.
     *
     * Two miners can solve the same height milliseconds apart. submitblock
     * takes the first and answers the second "inconclusive": a valid block
     * that is not in the chain. Settling the proportional plan against that
     * pays a turn for a reward nobody ever received, which breaks the
     * zero-sum invariant the no-custody design rests on — so the settlement
     * in on_block_found_cb() is gated on `accepted`. The blocks_found ROW is
     * still written, with status rejected and the node's reason, because a
     * refusal we do not record is a refusal we cannot explain later.
     *
     * This is rare at real difficulty and routine at minimum difficulty, which
     * is exactly the window this pool exists to mine. Found by
     * tests/test_burst_regtest.sh. */
    if (is_block && !block_accepted) {
        LOG_INFO("stratum: block %s from '%s' at height %u was NOT accepted by "
                 "the node (%s) — recording it as a rejected candidate, not "
                 "settling payouts. The share itself still counts.",
                 block_hash_hex, c->worker_name, job->height,
                 submit_err[0] ? submit_err : "no reason given");
    }
    if (is_block && s->cfg.on_block_found) {
        int64_t fee_sats = 0;
        if (s->cfg.fee_bps > 0 && s->cfg.operator_address[0]) {
            fee_sats = (job->value_sats * (int64_t)s->cfg.fee_bps) / 10000;
            if (fee_sats < 546) fee_sats = 0; /* matches coinbase dust rule */
        }
        int64_t reward_sats = job->value_sats - fee_sats;
        s->cfg.on_block_found(s->cfg.ctx, c->worker_name,
                              c->payout_address, ts_now, job->height,
                              job->job_id, block_hash_hex,
                              reward_sats, fee_sats,
                              block_accepted, submit_err, c->pol_solo);
    }
    return emit_response(buf, len, id, cJSON_CreateTrue(), NULL);
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
                             c->peer_ip, now_ms(), "unauthorized", NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(24, "unauthorized");
        return emit_response(buf, len, id, NULL, err);
    }
    /* Ceiling check goes here: after authorize (so an unauthorized flood is
     * still counted as such) but BEFORE parsing params, rendering a coinbase
     * or hashing — the whole point is that a refused submit stays cheap.
     * Deliberately NOT one on_reject per refusal: see submit_rate_report. */
    {
        uint64_t rl_now = now_ms();
        if (submit_rate_exceeded(s, c, rl_now)) {
            submit_rate_report(s, c, rl_now);
            cJSON *err = make_error(23, "submitting too fast");
            return emit_response(buf, len, id, NULL, err);
        }
        submit_rate_report(s, c, rl_now);
    }
    if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) < 5) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                             "bad params", NULL, STRATUM_JOB_AGE_NONE);
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

    /* A miner that authorized before the gate closed is still connected and
     * still hashing. Accepting those shares would bank work the pool has
     * already decided not to pay for. */
    if (pps_gated(s)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, now_ms(),
                             "pps accrual suspended (difficulty below floor)",
                             NULL, STRATUM_JOB_AGE_NONE);
        }
        cJSON *err = make_error(24, PPS_GATED_MSG);
        return emit_response(buf, len, id, NULL, err);
    }

    /* Counted reference: the tip watcher may retire and free this job while
     * the submit below is still reading it. */
    stratum_job_t *job = find_job(s, jid);
    if (!job) {
        /* One reason string, three different events — classify before
         * answering. Until this landed, a job we retired at 60 s, an id from
         * the process that ran before this one, and outright garbage all
         * incremented the same counter, so the retention window could not be
         * argued about from data. One clock reading serves both the record and
         * the age so they cannot disagree. */
        uint64_t    ts_now     = now_ms();
        int64_t     job_age_ms = STRATUM_JOB_AGE_NONE;
        const char *kind =
            stratum_classify_job_id(s->start_ms, ts_now, jid, &job_age_ms);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, c->peer_ip, ts_now,
                             "stale or unknown job", kind, job_age_ms);
        }
        cJSON *err = make_error(21, "stale or unknown job");
        return emit_response(buf, len, id, NULL, err);
    }

    int rc = submit_with_job(s, c, id, params, job, en2, ntime, nonce, buf, len);
    stratum_job_free(job);
    return rc;
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
    } else if (strcmp(method->valuestring, "mining.suggest_difficulty") == 0) {
        rc = handle_suggest_difficulty(s, c, id, params, out_buf, out_len);
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
    c->vd_window_min_achieved = HUGE_VAL;
    pthread_mutex_init(&c->state_lock, NULL);
    c->vd_window_max_assigned = 0.0;
    c->vd_window_stale_diff_shares = 0;
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

stratum_job_t *stratum_job_find_for_test(stratum_server_t *s, const char *job_id) {
    return find_job(s, job_id);
}
uint32_t stratum_job_height_for_test(const stratum_job_t *j) {
    return j ? j->height : 0;
}
int64_t stratum_job_value_sats_for_test(const stratum_job_t *j) {
    return j ? j->value_sats : 0;
}

/* Render (or reuse) this connection's coinbase for the current job and hand
 * back the pieces a submit is hashed from, plus the connection's assigned
 * difficulty. Tests use it to compute the hash a given nonce would produce,
 * which is the only way to pick shares achieving a chosen difficulty instead
 * of whatever the first nonce happens to land on — and simulating a miner
 * that filters at its own difficulty floor needs exactly that. */
int stratum_conn_coinbase_for_test(stratum_server_t *s, stratum_conn_t *c,
                                   const char *job_id,
                                   const uint8_t **cb1, size_t *cb1_len,
                                   const uint8_t **cb2, size_t *cb2_len,
                                   const uint8_t **en1) {
    if (!s || !c || !job_id) return -1;
    int rc = -1;
    pthread_rwlock_rdlock(&s->job_lock);
    stratum_job_t *j = s->current_job;
    if (j && strcmp(j->job_id, job_id) == 0 &&
        conn_render_coinbase(s, c, j) == 0) {
        *cb1 = c->cb1; *cb1_len = c->cb1_len;
        *cb2 = c->cb2; *cb2_len = c->cb2_len;
        *en1 = c->extranonce1;
        rc = 0;
    }
    pthread_rwlock_unlock(&s->job_lock);
    return rc;
}

double stratum_conn_difficulty_for_test(const stratum_conn_t *c) {
    return c ? c->difficulty : 0.0;
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

/* Copy the peer_ip of the most recently ACCEPTED connection. Exists so a test
 * can assert what accept() actually recorded — the dual-stack listener's whole
 * risk is that an IPv4 client starts reading as ::ffff:a.b.c.d, and nothing
 * else in the API exposes that. Returns 0 on success, -1 if no connection. */
int stratum_server_last_peer_ip_for_test(stratum_server_t *s, char *out, size_t cap) {
    if (!s || !out || cap == 0) return -1;
    pthread_mutex_lock(&s->conns_lock);
    stratum_conn_t *c = s->conns_head;   /* register() pushes to the head */
    int found = (c != NULL);
    if (c) snprintf(out, cap, "%s", c->peer_ip);
    pthread_mutex_unlock(&s->conns_lock);
    /* `found`, not `c`: once the lock is dropped another thread may have freed
     * the connection, and even comparing a dangling pointer is UB by the letter. */
    return found ? 0 : -1;
}

void stratum_conn_rearm_vardiff_for_test(stratum_conn_t *c) {
    if (!c) return;
    c->vd_window_start_ms = now_ms();
    c->vd_window_shares = 0;
    c->vd_window_min_achieved = HUGE_VAL;
    c->vd_window_max_assigned = 0.0;
    c->vd_window_stale_diff_shares = 0;
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
    /* Two budgets, chosen per check because authorize happens mid-loop.
     * The short one is for sockets that never authenticate; an authorized
     * miner gets the long one, because inbound silence from a miner means
     * "no share to send yet", not "dead". See config.h. */
    const int idle_unauth_sec = s->cfg.idle_timeout_sec;
    const int idle_auth_sec   = s->cfg.idle_timeout_authorized_sec > 0
                                  ? s->cfg.idle_timeout_authorized_sec
                                  : s->cfg.idle_timeout_sec;

    while (!atomic_load(&s->stop)) {
        ssize_t n = recv(c->fd, buf + blen, sizeof(buf) - 1 - blen, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* SO_RCVTIMEO wake. Drop iff we've been silent past the
                 * configured budget. Otherwise loop and try again. */
                int budget_sec = c->authorized ? idle_auth_sec
                                               : idle_unauth_sec;
                uint64_t idle_timeout_ms = budget_sec > 0
                                             ? (uint64_t)budget_sec * 1000u
                                             : 0;
                if (idle_timeout_ms > 0 &&
                    mono_ms() - c->last_activity_ms > idle_timeout_ms) {
                    LOG_INFO("stratum: idle timeout after %ds — closing fd=%d worker='%s'",
                             budget_sec, c->fd,
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

/* Render an accepted peer's address, un-mapping ::ffff:a.b.c.d back to dotted
 * quad.
 *
 * ⛔ The un-mapping is load-bearing, not cosmetic. On a dual-stack listener
 * every IPv4 client arrives as an IPv4-mapped IPv6 address, so peer_ip would
 * start reading "::ffff:169.58.184.136" for connections that today read
 * "169.58.184.136". ecash-rental-live.sh attributes a worker to the rental
 * port by matching the authorize line's peer IP against the addresses `ss`
 * reports as established — and `ss` prints dotted quad. The two would stop
 * matching, and the tool would report ZERO rental workers during a live order:
 * a silent wrong answer feeding the decision that gates restarts.
 *
 * So an IPv4 client must look exactly as it does today, whichever family the
 * listener is bound in. */
static void peer_ip_from_sockaddr(const struct sockaddr_storage *ss,
                                  char *out, size_t cap) {
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)ss;
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            struct in_addr v4;
            memcpy(&v4, &s6->sin6_addr.s6_addr[12], sizeof v4);
            if (inet_ntop(AF_INET, &v4, out, (socklen_t)cap)) return;
        } else if (inet_ntop(AF_INET6, &s6->sin6_addr, out, (socklen_t)cap)) {
            return;
        }
    } else if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)ss;
        if (inet_ntop(AF_INET, &s4->sin_addr, out, (socklen_t)cap)) return;
    }
    snprintf(out, cap, "?");
}

/* Bind one listener slot, carrying the whole dual-stack decision. Split out of
 * stratum_server_start when the server grew from one listening socket to N:
 * the address family, the V6ONLY decision and the backlog are properties of
 * the server, and only the port varies per slot.
 *
 * Address family is chosen from cfg.bind_addr, and "0.0.0.0" keeps meaning
 * exactly what it says.
 *
 *   ""  / "0.0.0.0"  -> IPv4 wildcard          (unchanged; the default)
 *   "::"             -> DUAL-STACK wildcard    (IPv6 + IPv4-mapped)
 *   IPv4 literal     -> that IPv4 address
 *   IPv6 literal     -> that IPv6 address only (V6ONLY on)
 *
 * ⚠️ Deliberately NOT making "0.0.0.0" dual-stack, even though that is what
 * production runs and would have made this arrive for free. Overloading a
 * wildcard that has one obvious meaning is how a binary install changes
 * behaviour nobody asked it to. IPv6 stays CONFIG-GATED: install the binary
 * and nothing moves; set listen_addr = :: and it turns on; revert is one
 * config line and no rebuild.
 *
 * Why dual-stack at all: both hostnames publish AAAA records and nginx answers
 * on [::], so the site looks healthy over IPv6 while stratum, bound IPv4-only,
 * refuses the connection. A miner whose client prefers AAAA -- the RFC 6724
 * default -- never starts mining, and unlike a browser most mining firmware
 * has no Happy Eyeballs fallback to recover with. */
static int listener_bind(stratum_server_t *s, struct stratum_listener_slot *ls) {
    const char *ba = s->cfg.bind_addr;
    int family = AF_INET;
    int dual_stack = 0;
    struct in_addr  v4 = {0};
    struct in6_addr v6;

    if (ba[0] == '\0' || strcmp(ba, "0.0.0.0") == 0) {
        v4.s_addr = htonl(INADDR_ANY);
    } else if (strcmp(ba, "::") == 0) {
        family = AF_INET6; dual_stack = 1; v6 = in6addr_any;
    } else if (inet_pton(AF_INET, ba, &v4) == 1) {
        /* explicit IPv4 literal — keeps its own family, so a test binding
         * 127.0.0.1 is completely untouched by any of this */
    } else if (inet_pton(AF_INET6, ba, &v6) == 1) {
        family = AF_INET6;
    } else {
        LOG_ERROR("stratum: listen_addr '%s' is not an IPv4 or IPv6 address", ba);
        return -1;
    }

    ls->fd = socket(family, SOCK_STREAM, 0);
    if (ls->fd < 0) return -1;
    int one = 1;
    setsockopt(ls->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (family == AF_INET6) {
        /* 0 = accept IPv4-mapped connections too; 1 = this family only.
         * Set EXPLICITLY in both directions: the default is a sysctl
         * (net.ipv6.bindv6only) and inheriting it means the pool's listening
         * behaviour depends on a host setting nobody here records. */
        int v6only = dual_stack ? 0 : 1;
        if (setsockopt(ls->fd, IPPROTO_IPV6, IPV6_V6ONLY,
                       &v6only, sizeof(v6only)) < 0) {
            LOG_ERROR("stratum: IPV6_V6ONLY=%d: %s", v6only, strerror(errno));
            return -1;
        }
    }

    struct sockaddr_storage ss;
    socklen_t sslen;
    memset(&ss, 0, sizeof ss);
    if (family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)(void *)&ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)ls->pol.port);
        a6->sin6_addr = v6;
        sslen = sizeof(*a6);
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)(void *)&ss;
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t)ls->pol.port);
        a4->sin_addr = v4;
        sslen = sizeof(*a4);
    }
    if (bind(ls->fd, (struct sockaddr *)&ss, sslen) < 0) {
        LOG_ERROR("stratum bind %s:%d: %s", ba[0] ? ba : "0.0.0.0",
                  ls->pol.port, strerror(errno));
        return -1;
    }
    /* Print the derived retarget ceiling next to the compiled-in retention
     * window. Neither is visible from the config file -- the ceiling is a
     * product of two keys and the TTL is a #define -- and their relationship
     * is what opened a difficulty runaway once already. The journal should be
     * able to answer "was there a drain band on that run?" without anyone
     * re-deriving it. Same reasoning as the payout-caps line. */
    {
        long ceil_ms = (long)s->cfg.vardiff_window_sec * 1000L *
                       (s->cfg.vardiff_max_window_mult > 0
                          ? s->cfg.vardiff_max_window_mult : 8);
        if (!s->cfg.vardiff_enabled || s->cfg.vardiff_window_sec <= 0) {
            LOG_INFO("stratum: vardiff disabled (job retention %ds)",
                     RECENT_JOB_TTL_MS / 1000);
        } else {
            LOG_INFO("stratum: vardiff retarget ceiling %lds, job retention %ds%s",
                     ceil_ms / 1000L, RECENT_JOB_TTL_MS / 1000,
                     ceil_ms < (long)RECENT_JOB_TTL_MS
                       ? " (ceiling < retention: drain-deferral band exists, guarded)"
                       : "");
        }
    }
    LOG_INFO("stratum: listening on %s:%d%s%s (%s)", ba[0] ? ba : "0.0.0.0",
             ls->pol.port,
             ls->pol.label[0] ? " " : "", ls->pol.label,
             dual_stack ? "dual-stack, IPv4 clients un-mapped to dotted quad"
                        : (family == AF_INET6 ? "IPv6 only" : "IPv4 only"));
    /* Clamped by net.core.somaxconn, so asking for more than the kernel
     * allows is harmless -- asking for less than it allows is not. */
    int backlog = s->cfg.listen_backlog > 0 ? s->cfg.listen_backlog
                                            : STRATUM_DEFAULT_BACKLOG;
    if (listen(ls->fd, backlog) < 0) return -1;
    return 0;
}

/* Apply a listener's difficulty policy to a connection. Everything after this
 * point reads the policy off the connection and never looks at the listener. */
static void conn_apply_listener(stratum_conn_t *c,
                                const stratum_listener_t *pol) {
    if (!c || !pol) return;
    if (pol->initial_diff > 0.0) c->pol_initial_diff = pol->initial_diff;
    if (pol->vardiff_min  > 0.0) c->pol_vardiff_min  = pol->vardiff_min;
    if (pol->vardiff_max  > 0.0) c->pol_vardiff_max  = pol->vardiff_max;
    c->pol_port = pol->port;
    c->pol_solo = pol->solo;
    snprintf(c->pol_label, sizeof c->pol_label, "%s", pol->label);
    /* Before authorize the connection has no assigned difficulty yet, so
     * seeding it here keeps a subscribe-only conn reporting its port's value
     * rather than the default it was born with. */
    if (c->pol_initial_diff > 0.0) c->difficulty = c->pol_initial_diff;
}

void stratum_conn_apply_listener_for_test(stratum_conn_t *c,
                                          const stratum_listener_t *pol) {
    conn_apply_listener(c, pol);
}

static void *listener_thread(void *arg) {
    struct stratum_listener_slot *ls = arg;
    stratum_server_t *s = ls->srv;
    while (!atomic_load(&s->stop)) {
        struct sockaddr_storage cli;
        socklen_t cl = sizeof(cli);
        int fd = accept(ls->fd, (struct sockaddr *)&cli, &cl);
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
        /* The recv timeout only paces the reaper's wake-ups, so it has to be
         * armed whenever *either* budget is live — an operator who disabled
         * the unauthorized timeout but left the authorized one on would
         * otherwise block in recv() forever and never check it. */
        int poll_budget = s->cfg.idle_timeout_sec > 0
                              ? s->cfg.idle_timeout_sec
                              : s->cfg.idle_timeout_authorized_sec;
        if (conn_socket_setup(fd, poll_budget) < 0) {
            LOG_WARN("stratum: socket setup failed for accepted fd: %s",
                     strerror(errno));
            close(fd);
            continue;
        }
        stratum_conn_t *c = stratum_conn_new_for_test(s);
        if (!c) { close(fd); continue; }
        c->fd = fd;
        peer_ip_from_sockaddr(&cli, c->peer_ip, sizeof c->peer_ip);
        /* The port decides the difficulty. */
        conn_apply_listener(c, &ls->pol);
        atomic_fetch_add(&s->conn_count, 1);
        /* ⛔ After conn_register the connection belongs to other threads, and
         * conn_thread FREES it when the miner goes away. Do not touch `c`
         * again here — not even to write a field.
         *
         * That is not theoretical. This loop used to do
         *
         *     pthread_create(&c->thr, NULL, conn_thread, c);
         *     pthread_detach(c->thr);      <- read of freed memory
         *     c->thr_started = 1;          <- WRITE into freed memory
         *
         * and pthread_create can return after conn_thread has already run to
         * completion and freed `c` — overwhelmingly likely when the peer is
         * already gone, i.e. exactly during a mass disconnect. The 4-byte
         * store then lands in a free chunk and corrupts glibc's malloc
         * metadata, which surfaces much later and elsewhere as
         * `corrupted double-linked list` + SIGABRT. That is how the pool
         * died on 2026-08-26 at 20:53 UTC (INC-002), ~1,000 connections
         * being torn down while others were still authorizing.
         *
         * The thread is created ALREADY detached, into a local handle, so
         * there is nothing to clean up and nothing to write back. Neither
         * `c->thr` nor `c->thr_started` is ever read for a connection — the
         * shutdown path joins LISTENER threads (`ls->thr`), never these — so
         * both fields simply go away. */
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) {
            atomic_fetch_sub(&s->conn_count, 1);
            close(fd);
            stratum_conn_free_for_test(c);
            continue;
        }
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        conn_register(s, c);
        pthread_t tid;
        if (pthread_create(&tid, &attr, conn_thread, c) != 0) {
            /* Nothing else can have reached `c` yet on this path: the thread
             * that frees it never started. Unregister and free is safe. */
            conn_unregister(s, c);
            atomic_fetch_sub(&s->conn_count, 1);
            close(fd);
            stratum_conn_free_for_test(c);
            pthread_attr_destroy(&attr);
            continue;
        }
        pthread_attr_destroy(&attr);
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

    s->start_ms = now_ms();

    atomic_init(&s->extranonce1_seq, (unsigned)now_ms());
    pthread_mutex_init(&s->share_dedupe_lock, NULL);

    /* ⛔ Every slot's fd starts at -1, not the 0 calloc leaves behind. The
     * teardown path below closes `fd >= 0` for every slot up to
     * listener_count, so a slot that never got as far as socket() would
     * otherwise close descriptor 0 -- the process's stdin -- on any bind
     * failure with three or more listeners configured. */
    for (int i = 0; i < STRATUM_MAX_LISTENERS; ++i) s->listeners[i].fd = -1;

    /* Listener 0 is always bind_port on the server-wide defaults, so a config
     * naming no extra listeners binds exactly what it always did. The rest
     * come from cfg.listeners, each overriding the difficulty policy for the
     * connections it accepts. */
    s->listeners[0].srv = s;
    s->listeners[0].pol.port = cfg->bind_port;
    s->listener_count = 1;
    for (int i = 0; i < cfg->listener_count &&
                    s->listener_count < STRATUM_MAX_LISTENERS; ++i) {
        if (cfg->listeners[i].port <= 0) continue;
        s->listeners[s->listener_count].srv = s;
        s->listeners[s->listener_count].pol = cfg->listeners[i];
        s->listener_count++;
    }

    for (int i = 0; i < s->listener_count; ++i) {
        struct stratum_listener_slot *ls = &s->listeners[i];
        if (listener_bind(s, ls) < 0) goto bind_failed;
        if (pthread_create(&ls->thr, NULL, listener_thread, ls) != 0) {
            goto bind_failed;
        }
        ls->thr_started = 1;
    }
    *out = s;
    return 0;

    /* A pool that came up on some of its ports is worse than one that did not
     * come up: the operator sees a running process and a marketplace sees a
     * refused connection. Tear down whatever bound and fail the start. */
bind_failed:
    atomic_store(&s->stop, 1);
    for (int i = 0; i < s->listener_count; ++i) {
        struct stratum_listener_slot *ls = &s->listeners[i];
        if (ls->fd >= 0) { shutdown(ls->fd, SHUT_RDWR); close(ls->fd); ls->fd = -1; }
        if (ls->thr_started) { pthread_join(ls->thr, NULL); ls->thr_started = 0; }
    }
    pthread_mutex_destroy(&s->share_dedupe_lock);
    free(s);
    return -1;
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
     * listener's own read of the slot's fd (TSan flagged it 7x in the job
     * rotation stress test) and could have closed the fd while accept() was
     * still using it. After the join nothing else touches the field. */
    for (int i = 0; i < s->listener_count; ++i) {
        struct stratum_listener_slot *ls = &s->listeners[i];
        int fd = ls->fd;
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        if (ls->thr_started) {
            pthread_join(ls->thr, NULL);
            ls->thr_started = 0;
        }
        if (fd >= 0) {
            close(fd);
            ls->fd = -1;
        }
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
    pthread_mutex_destroy(&s->share_dedupe_lock);
    free(s);
}
