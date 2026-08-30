#ifndef SIMPLEPOOL_STRATUM_H
#define SIMPLEPOOL_STRATUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

/* coinbase_payout_t, used by stratum_job_set_payouts below. */
#include "coinbase.h"

typedef struct stratum_job stratum_job_t;

/* How many retired jobs the server keeps solvable, on top of the current one.
 *
 * ⛔ THIS IS NOT THE RETENTION WINDOW ON ITS OWN, and reading it as one has
 * already cost us. The effective grace is
 *
 *     min(RECENT_JOB_TTL_MS, STRATUM_RECENT_JOBS × job cadence)
 *
 * and until 2026-08-30 the TTL was 60 s against a ~31 s cadence, so the ring
 * NEVER bound: all 8 slots were dead capacity and the real window was ~60-90 s.
 * "8 jobs × 31 s ≈ 4 minutes" is the obvious arithmetic, it is wrong, and it
 * reached a customer reply once already. Whenever you change one of these two
 * numbers, read the other. RECENT_JOB_TTL_MS lives in stratum.c.
 *
 * Raised 8 → 16 on 2026-08-30 so the ring stays slack at the new 300 s TTL
 * (16 × 31 s ≈ 496 s). If the cadence ever drops far below 30 s — the fork's
 * minimum-difficulty window is the case to worry about — the ring binds first
 * again and this is the number to revisit.
 *
 * Public because anything holding per-job state alongside the server has to
 * retain at least as much of it: pool_mode=proportional keeps a payout plan
 * per job, and a plan ring shorter than this made the oldest still-solvable
 * job settle with no plan — silently paying its finder solo instead of the
 * window. Size against this constant, never against a hand-picked number. */
#define STRATUM_RECENT_JOBS 16

/* Default listen() backlog. 1024 rather than the kernel's own default: a
 * marketplace order arrives as one burst of hundreds of connections, and the
 * queue only has to absorb the burst until the listener thread drains it. */
#define STRATUM_DEFAULT_BACKLOG 1024

/* Extranonce widths, in bytes. Advertised at mining.subscribe, reserved in
 * the coinbase scriptSig, and enforced on every submit — change them here and
 * nowhere else.
 *
 * extranonce2 is 8, not the classic 4. The reason is not search space: at 4
 * bytes one connection already has 2^80 headers per job once nonce and
 * version rolling count, and jobs rotate long before that. The reason is
 * *subdivision*. Rented hashrate does not arrive as many small miners, it
 * arrives as one aggregated worker behind a proxy that splits the extranonce2
 * it is handed into a downstream-miner id (high bytes) plus that miner's own
 * extranonce2 (low bytes). At 4 a proxy spending 3 on addressing leaves its
 * miners one byte, and some firmware refuses to run that narrow. At 8 it can
 * spend 3 and still hand down the conventional 4.
 *
 * 8 rather than the 7 the marketplaces floor at: same cost, satisfies any
 * ">= 7" rule, and 4/8 is the split proxies and firmware already expect. */
#define STRATUM_EXTRANONCE1_SIZE 4
#define STRATUM_EXTRANONCE2_SIZE 8

/* A listening port and the difficulty policy for the miners that arrive on
 * it. The pool serves one kind of miner badly if it serves only one policy:
 * a home ASIC needs a difficulty low enough to report shares regularly, and
 * an aggregated fleet from a hashrate marketplace needs one high enough that
 * its share rate stays sane -- 1 PH/s at difficulty 1024 is ~227 shares per
 * second down a single connection, and the marketplaces refuse to deliver
 * below their own floor for exactly that reason.
 *
 * One difficulty cannot be both, and vardiff cannot bridge it: it moves by at
 * most 4x per window, so climbing from 1 to 65536 takes eight windows -- four
 * minutes at the default -- and the reject flood on the way there is what
 * gets a rented order cancelled. Hence a port per policy, each one already at
 * the right difficulty when the miner connects.
 *
 * A field left at 0 falls back to the server-wide default.
 *
 * ⓘ This replaces our two-`stratum_server_t` design and the `stratum_shared_t`
 * that guarded it. Two servers in one process seeded extranonce1 from the
 * clock milliseconds apart, handed out overlapping values, rendered identical
 * coinbases, and deduped against per-server rings that were blind to each
 * other -- so the collision happened AND its guard looked the wrong way. One
 * server with N listener threads cannot reach that state at all: there is a
 * single sequence and a single ring because there is a single server. The
 * hazard class is deleted rather than defended. */
typedef struct {
    int    port;
    double initial_diff;
    double vardiff_min;
    double vardiff_max;
    /* Set only when the operator wrote `min_diff=` on this listener. It is a
     * RECORD OF INTENT -- "this port was explicitly asked for a floor" -- and
     * nothing more. It drives the marketplace warning in config.c and the
     * startup log; the floor itself is delivered through vardiff_min and
     * initial_diff, which parse_listener sets from the same value.
     *
     * ⛔ DELIBERATE DIVERGENCE FROM UPSTREAM. Upstream keeps this floor even
     * when the chain's own difficulty is lower, so a port holds the difficulty
     * it advertised to a marketplace. WE DO NOT, and this is not an oversight
     * to be tidied up on the next merge:
     *
     *   If the share target D exceeds the network target N, every share that
     *   arrives IS a block -- but shares arrive at H/(D*2^32) instead of
     *   H/(N*2^32), because the miner discards anything above the stratum
     *   target locally and never submits it. Block discovery falls by exactly
     *   N/D. That is arithmetic, not risk.
     *
     * On a chain whose difficulty is briefly in the thousands -- a
     * minimum-difficulty window after a fork, which is precisely when this
     * pool most wants to be mining -- a rental port pinned at 500,000 would
     * throw away blocks by two or three orders of magnitude. The marketplace
     * risk it avoids is real but BOUNDED: an order cannot be served until the
     * chain ramps back up, which we tell miners plainly.
     *
     * ⚠️ So the network-difficulty clamp is PROTECTIVE, not a limitation. An
     * earlier revision of this comment said the opposite and cited upstream as
     * the fix. It was wrong. Do not restore it. */
    double min_diff;
    /* Free-form, for logs and for the dashboard to tell miners which port to
     * point which machine at. Empty for the default listener. */
    char   label[32];
    /* SOLO PORT. Set by `listener = port=3336 mode=solo`. A connection that
     * arrives here has its own coinbase pay ITSELF (minus the operator fee)
     * instead of the shared PPLNS payout set, and its shares are recorded
     * solo=1 so they never enter another miner's payout window.
     *
     * ⛔ The mode belongs to the LISTENER, not to the worker name. Overloading
     * the username was considered and rejected: miners already misconfigure it
     * (one authorized as the literal string "worker" and lost 47k shares to a
     * base58 error), and a typo there would silently move a miner between
     * payout schemes. A port is unambiguous, and the miner opts in by pointing
     * their machine somewhere different. */
    int    solo;
} stratum_listener_t;

#define STRATUM_MAX_LISTENERS 8

/* Create a job from template fields. The coinbase is *not* baked into the
 * job — each connection renders its own coinbase paying its miner address
 * (minus the configured operator fee). The job carries everything else
 * the server needs to materialise a per-connection coinbase on demand:
 *   - value_sats:           coinbasevalue from getblocktemplate
 *   - witness_commitment_hex: optional, may be NULL
 *   - en1_size / en2_size:  extranonce sizes, from STRATUM_EXTRANONCE{1,2}_SIZE
 *
 * tx_hex_list may be NULL if tx_count == 0. The job takes ownership of
 * its own heap copies; caller's buffers are not retained.
 *
 * coinbasetxn_hex is optional (may be NULL): when the backend supplied a
 * full coinbase (BIP22 "coinbasetxn"), each connection's coinbase is built
 * from it instead of from scratch. coinbase_has_witness records whether that
 * coinbase is segwit-serialized, so the block assembler re-attaches the
 * witness reserved value at submit time. */
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
    const char *coinbasetxn_hex, int coinbase_has_witness);

void stratum_job_free(stratum_job_t *j);

/* Take an additional reference to a job. stratum_server_set_job() consumes
 * one reference, so publishing the same job to a second server needs one more
 * taken first. Returns j for convenient nesting. */
stratum_job_t *stratum_job_ref(stratum_job_t *j);

/* Attach a proportional payout list to a job. Copies the array. Called by
 * main.c after computing the PPLNS window for a new template. Returns 0 ok,
 * -1 on oom. */
int stratum_job_set_payouts(stratum_job_t *j,
                            const coinbase_payout_t *payouts,
                            size_t n_payouts);

/* Observer hooks filled in by main.c (typically routed to the sqlite store). */
/* `solo` is 1 when the share arrived on a solo listener. It travels with the
 * share rather than being looked up per worker because a miner may move
 * between ports: the shares they submitted while on PPLNS must KEEP counting
 * in the window they earned, and the ones submitted while solo must never
 * count at all. Classifying by current worker state would retroactively
 * rewrite both. */
typedef void (*share_observer_fn)(void *ctx, const char *worker_name,
                                  const char *payout_address,
                                  uint64_t ts_ms, double difficulty,
                                  int is_block, const char *block_hash_or_null,
                                  int solo);
/* `peer_ip` is the connection the reject arrived on, and it is the whole
 * reason this callback carries more than a worker name: a submit refused
 * before authorize has no worker to blame, so a burst of them (10,776 of them
 * over 08-28/29) was unattributable to anything at all. It is never NULL —
 * every connection has one from accept.
 *
 * `reject_kind` and `job_age_ms` are populated ONLY for "stale or unknown
 * job", which is three distinct events sharing one counter: see
 * stratum_classify_job_id. Every other reject passes (NULL, -1). */
typedef void (*reject_observer_fn)(void *ctx, const char *worker_name,
                                   const char *peer_ip, uint64_t ts_ms,
                                   const char *reason,
                                   const char *reject_kind,
                                   int64_t job_age_ms);

/* "no age", and deliberately NOT -1: a job id stamped slightly ahead of the
 * clock yields a genuinely negative age, and -1 ms is one of the values it can
 * take. A sentinel a real measurement can collide with turns a clock step into
 * a silently absent row — the exact disappearance this column exists to make
 * visible. INT64_MIN is not a reachable age. */
#define STRATUM_JOB_AGE_NONE INT64_MIN

/* The three ways a submit can name a job we cannot find. */
#define STRATUM_REJECT_KIND_EVICTED      "evicted"
#define STRATUM_REJECT_KIND_PRE_RESTART  "unknown_pre_restart"
#define STRATUM_REJECT_KIND_NEVER_ISSUED "never_issued"

/* Classify the job id from a submit that find_job() could not match, and
 * recover the job's age from the id itself.
 *
 * Every job id is "<hex wall-clock ms>-<hex seq>" (main.c), so the creation
 * time travels in the string the miner hands back — it needs no eviction
 * index and survives the job struct being freed. Returns one of the three
 * STRATUM_REJECT_KIND_* strings (never NULL) and writes the age in ms to
 * *age_ms_out, or STRATUM_JOB_AGE_NONE where no age exists:
 *
 *   evicted             we issued it this run and retired it. Age is real,
 *                       and it is the number the retention window has to be
 *                       argued from.
 *   unknown_pre_restart well-formed, but stamped before this process started:
 *                       issued by a previous instance, which is why we have
 *                       no record of it. Age would be meaningless.
 *   never_issued        does not parse, or is stamped in the future beyond
 *                       any plausible clock skew. Not ours.
 *
 * ⚠️ The age is measured on CLOCK_REALTIME (the id's own clock), while the
 * TTL sweep in retire_job() runs on CLOCK_MONOTONIC. They normally track, but
 * a wall-clock step between issue and submit skews the age REPORTED here
 * without skewing the eviction that actually happened. A negative age is
 * exactly that case, and is passed through rather than clamped so it stays
 * visible in the data instead of reading as a fresh job. */
const char *stratum_classify_job_id(uint64_t server_start_ms,
                                    uint64_t now_wall_ms,
                                    const char *job_id, int64_t *age_ms_out);
/* Submits the assembled block upstream. Returns 0 when the node accepted it,
 * non-zero when it refused, filling errbuf with the node's reason.
 *
 * The result is not advisory. A share meeting network difficulty makes a
 * *candidate*, not a block: submitblock refuses stale, duplicate and
 * high-hash candidates routinely, and on a low-difficulty chain that is the
 * common case. Recording one as a block credits the pool with revenue that
 * never existed. */
typedef int (*block_submit_fn)(void *ctx, const char *block_hex,
                               char *errbuf, size_t errlen);
/* Asked once per authorize for a starting share difficulty for this worker,
 * typically from its own recent history. Returns <= 0 when nothing is known,
 * and initial_diff is used instead.
 *
 * Without this every reconnect — and every pool restart — drops every miner
 * back to initial_diff and makes it climb again, 4x per vardiff window. A
 * multi-TH/s ASIC starting at difficulty 1 floods the pool for minutes and
 * sheds shares at each step of the climb. */
typedef double (*difficulty_hint_fn)(void *ctx, const char *worker_name);
/* Fires once per block candidate, after the share has been recorded. Used by
 * main.c to insert into blocks_found with reward/fee/finder address.
 *
 * `accepted` is whether on_block's submission was taken by the node, and
 * `submit_error` the reason when it was not. A candidate the node refused is
 * still reported here — it is recorded as 'rejected' rather than dropped,
 * because a silent reject is how phantom rewards went unnoticed.
 *
 * ⛔ It therefore fires for blocks that are NOT in the chain. Anything that
 * moves money — pool_mode=proportional settles its payout plan from `job_id`
 * here — MUST gate on `accepted`. Recording a candidate is not the same act
 * as paying for one. */
/* `solo` is 1 when the connection that solved this block arrived on a solo
 * listener. It is here for the SETTLE path, not for accounting: the PPLNS plan
 * is per-TEMPLATE and shared by every connection, so a solo miner solving that
 * template would otherwise consume the plan built for the PPLNS set — marking
 * shareholders as paid out of a coinbase that paid only the solo finder. */
typedef void (*block_found_fn)(void *ctx,
                               const char *worker_name,
                               const char *finder_address,
                               uint64_t ts_ms, uint32_t height,
                               const char *job_id,
                               const char *block_hash,
                               int64_t reward_sats, int64_t fee_sats,
                               int accepted, const char *submit_error,
                               int solo);

typedef struct {
    char   bind_addr[64];
    int    bind_port;
    int    max_conns;            /* default 500 */
    double initial_diff;         /* default 1.0 */
    /* Coinbase split — in solo mode each connection's coinbase pays the
     * miner directly. In PPS mode (pps_enabled=1) every coinbase instead
     * pays the single pool-owned pool_btc_address. In proportional mode
     * the coinbase pays the PPLNS window shareholders directly. In all
     * modes (value * fee_bps / 10000) goes to operator_address as a BTC
     * fee. */
    char   operator_address[128];
    int    fee_bps;
    char   coinbase_tag[64];

    /* pool_mode: "solo" | "pps-classic" | "proportional". */
    char    pool_mode[16];

    /* PPS (pool_mode=pps-classic). When pps_enabled = 1:
     *  - mining.authorize accepts Thunder addresses (base58 of 20-byte hash)
     *  - the share observer's payout_address argument is the miner's
     *    Thunder address (for PPS accrual), not a Bitcoin address.
     *  - every miner gets the same coinbase: coinbase_build_split paying
     *    pool_btc_address for the miner-share and operator_address for the
     *    fee. Deposits into Thunder happen off-band via the admin
     *    dashboard, not in the coinbase.
     */
    int     pps_enabled;
    char    pool_btc_address[128];   /* pps-classic: coinbase spendable output */

    /* Points at the proxy's PPS accrual gate — non-zero while network
     * difficulty is below the configured floor and nothing is being credited.
     * NULL when the caller has no gate.
     *
     * The server reads it so it can turn miners away instead of accepting
     * work it will not pay for. A miner whose shares are accepted but never
     * credited is mining for free without being told, which is worse than
     * being refused. */
    const _Atomic int *pps_gate;
    int     pps_refuse_shares_below_min;

    /* Vardiff (see config.h for prose). 0 disables and pins to initial_diff. */
    int    vardiff_enabled;
    double vardiff_target_spm;
    double vardiff_min;
    double vardiff_max;
    int    vardiff_window_sec;
    int    vardiff_min_samples;      /* see config.h; 0 = legacy behaviour */
    double max_suggested_diff;       /* cap on a miner-requested difficulty */
    int    vardiff_max_window_mult;  /* see config.h */
    double vardiff_idle_step;        /* see config.h */

    /* Drop a connection whose recv() has been silent for this long. Guards
     * against half-open TCPs from crashed miners and misconfigured clients
     * that connect but never authenticate. 0 disables (legacy). Default 600. */
    int    idle_timeout_sec;

    /* Same, for a connection that has authorized. See config.h — a miner
     * with nothing to submit is silent, so this must be sized against the
     * expected share interval at the difficulty floor, not against TCP
     * liveness. <= 0 falls back to idle_timeout_sec (legacy behaviour). */
    int    idle_timeout_authorized_sec;

    /* listen() backlog: how many completed handshakes the kernel may hold
     * before accept() takes them. <= 0 uses STRATUM_DEFAULT_BACKLOG.
     *
     * This was hardcoded at 64, which is far too small for a hashrate
     * marketplace: those fan out hundreds of connections in a burst, and a
     * full accept queue makes the kernel DROP the SYN. The client sees a
     * connection that never completed, the pool sees nothing at all -- there
     * is no accept() error to log, because there was no accept. The only
     * evidence is TcpExt:ListenOverflows, which reached 769 on this pool
     * before anyone thought to read it. */
    int    listen_backlog;

    /* Extra listeners beyond bind_port, each with its own difficulty policy.
     * bind_port is always served, using the server-wide defaults, so a config
     * that names none of these behaves exactly as before. */
    stratum_listener_t listeners[STRATUM_MAX_LISTENERS];
    int    listener_count;

    /* Ceiling on mining.submit per second, per connection. 0 disables.
     *
     * A connection's share rate is its hashrate divided by the difficulty it
     * was assigned, and nothing stops those from being wildly mismatched: an
     * aggregated fleet pointed at a home-miner port is 1 PH/s against
     * difficulty 1, which is ~232,000 submits per second down one socket.
     * Validating a submit costs about 9 microseconds, so that single
     * connection asks for more than two cores -- and every share it lands
     * goes through the store's ring, which drops events once full. Work the
     * miner was told was accepted then never gets credited.
     *
     * ⛔ Ships OFF (0) and stays off until real miner behaviour has been
     * measured against it -- same discipline as the five gate keys. Refusing
     * a submit is a miner-visible action; it does not get a default. */
    int    max_submits_per_sec;

    void  *ctx;
    share_observer_fn  on_share;
    difficulty_hint_fn on_difficulty_hint;   /* optional */
    reject_observer_fn on_reject;
    block_submit_fn    on_block;
    block_found_fn     on_block_found;
} stratum_cfg_t;

typedef struct stratum_server stratum_server_t;

int  stratum_server_start(const stratum_cfg_t *cfg, stratum_server_t **out);
/* Atomically swap the current job. Takes ownership of new_job. */
/* Publish `new_job` and broadcast it to every subscribed connection.
 * Consumes one reference to `new_job`.
 *
 * `clean` becomes the mining.notify clean_jobs flag: pass 1 only when the
 * previous block changed, so work in progress is now worthless. Pass 0 for a
 * same-tip template refresh — the miner's current job is still valid, and
 * flushing it throws away partial progress across the whole fleet. The pool
 * keeps STRATUM_RECENT_JOBS solvable either way, so a share arriving against
 * the older job is still accepted. */
void stratum_server_set_job(stratum_server_t *s, stratum_job_t *new_job,
                            int clean);
void stratum_server_stop(stratum_server_t *s);
void stratum_server_free(stratum_server_t *s);

/* ----------------------------------------------------------------------- */
/* Internal API exposed for unit tests. Not for general consumers.         */
/* ----------------------------------------------------------------------- */

/* A small per-connection state used by stratum_handle_message. Tests
 * construct one of these directly. */
typedef struct stratum_conn stratum_conn_t;

/* Allocate a connection state attached to a server. Used by tests; the
 * real listener uses an internal allocator. */
stratum_conn_t *stratum_conn_new_for_test(stratum_server_t *s);
void            stratum_conn_free_for_test(stratum_conn_t *c);

/* Test accessors — connection internals are otherwise opaque. */
/* Rendered coinbase halves + extranonce1 for the current job, so tests can
 * reproduce the hash a submit will produce and mine nonces to a chosen
 * difficulty. Returns 0 on success. */
int stratum_conn_coinbase_for_test(stratum_server_t *s, stratum_conn_t *c,
                                   const char *job_id,
                                   const uint8_t **cb1, size_t *cb1_len,
                                   const uint8_t **cb2, size_t *cb2_len,
                                   const uint8_t **en1);
double      stratum_conn_difficulty_for_test(const stratum_conn_t *c);
/* Apply a listener's difficulty policy to a connection, exactly as the accept
 * path does when a miner arrives on that port. Exposed so per-port policy can
 * be tested without binding a fixed port, which in CI is a race with whatever
 * else is on the box. */
void        stratum_conn_apply_listener_for_test(stratum_conn_t *c,
                                                 const stratum_listener_t *pol);
const char *stratum_conn_worker_name_for_test(const stratum_conn_t *c);
const char *stratum_conn_payout_address_for_test(const stratum_conn_t *c);
int         stratum_conn_authorized_for_test(const stratum_conn_t *c);

/* Force the connection's vardiff state directly, bypassing the retarget path.
 *
 * Exists because the interesting case for per-job difficulty — a submit for a
 * job issued SEVERAL retargets ago — cannot be reached through
 * vardiff_maybe_retarget in a unit test: raising difficulty needs accepted
 * shares, and once difficulty is high enough to reject a random hash no
 * further share is accepted to drive the next retarget. Setting `prev`
 * models the second retarget having overwritten the original difficulty,
 * which is exactly what puts it beyond the one-deep grace. */
void stratum_conn_force_difficulty_for_test(stratum_conn_t *c,
                                            double cur, double prev);
int         stratum_conn_subscribed_for_test(const stratum_conn_t *c);

/* Live connection threads. Zero after stratum_server_stop() returns is the
 * observable form of "no connection thread can touch this server again",
 * which is what makes tearing the server down afterwards safe. */
int         stratum_server_conn_count_for_test(const stratum_server_t *s);

/* peer_ip of the most recently accepted connection; 0 ok, -1 if none. */
int stratum_server_last_peer_ip_for_test(stratum_server_t *s, char *out, size_t cap);

/* Apply the same socket options the listener applies to every accepted
 * connection: TCP_NODELAY, SO_KEEPALIVE + TCP_KEEP{IDLE,INTVL,CNT}, and
 * SO_RCVTIMEO (poll interval derived from idle_timeout_sec). Exposed for
 * tests. */
int stratum_socket_setup_for_test(int fd, int idle_timeout_sec);

/* Link `c` into the server's live-connection list with `fd`, so that
 * stratum_server_set_job() will render and broadcast to it. Without this a
 * test connection is invisible to the broadcast path (fd < 0 is skipped), and
 * the concurrency between the tip watcher and a submitting connection — the
 * whole reason c->state_lock exists — cannot be reached from a unit test.
 * The caller keeps ownership of the fd. */
void stratum_conn_register_for_test(stratum_server_t *s, stratum_conn_t *c,
                                    int fd);
/* Test-only: look a job up exactly as the submit path does, returning a
 * COUNTED reference the caller must stratum_job_free(). Exists so a test can
 * pin the property that makes the submit path safe — that a job stays valid
 * for a holder even after the tip watcher has retired and freed it. */
stratum_job_t *stratum_job_find_for_test(stratum_server_t *s, const char *job_id);
uint32_t stratum_job_height_for_test(const stratum_job_t *j);
int64_t  stratum_job_value_sats_for_test(const stratum_job_t *j);

/* Process one JSON-RPC line. Appends one or more newline-delimited JSON
 * messages to *out_buf (caller-owned, will be realloc'd). Returns 0 on
 * success, negative on protocol error (caller should disconnect). */
int stratum_handle_message(stratum_server_t *s, stratum_conn_t *c,
                           const char *line,
                           char **out_buf, size_t *out_len);

#endif
