#ifndef SIMPLEPOOL_STRATUM_H
#define SIMPLEPOOL_STRATUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* coinbase_payout_t, used by stratum_job_set_payouts below. */
#include "coinbase.h"

typedef struct stratum_job stratum_job_t;

/* How many retired jobs the server keeps solvable, on top of the current one.
 * A submit naming a job older than `current + STRATUM_RECENT_JOBS` is rejected
 * as unknown.
 *
 * Public because anything holding per-job state alongside the server has to
 * retain at least as much of it: pool_mode=proportional keeps a payout plan
 * per job, and a plan ring shorter than this made the oldest still-solvable
 * job settle with no plan — silently paying its finder solo instead of the
 * window. Size against this constant, never against a hand-picked number. */
#define STRATUM_RECENT_JOBS 8

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

/* State that must be common to every stratum server in the process.
 *
 * Two things in a server are only correct while there is exactly one of them,
 * and both silently produce double-credited shares once a second server
 * exists (the rental port):
 *
 *   - the extranonce1 counter. It is seeded from the clock at startup, so two
 *     servers constructed in the same process seed within a millisecond of
 *     each other and hand out overlapping extranonce1 values. Two connections
 *     with the same extranonce1 render identical coinbases, mine identical
 *     headers, and find the same hash from the same nonce.
 *   - the share dedupe ring, which is what would otherwise catch exactly that.
 *     Per-server rings are blind to each other, so the collision happens *and*
 *     the guard against it is looking the wrong way.
 *
 * Servers sharing one of these draw extranonce1 from a single sequence and
 * dedupe against a single ring. Pass the same pointer to every
 * stratum_server_start() in the process; leave cfg.shared NULL and the server
 * allocates a private one, which is the correct behaviour for a lone server
 * and for every test. */
typedef struct stratum_shared stratum_shared_t;

stratum_shared_t *stratum_shared_new(void);
void              stratum_shared_free(stratum_shared_t *sh);

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
typedef void (*share_observer_fn)(void *ctx, const char *worker_name,
                                  const char *payout_address,
                                  uint64_t ts_ms, double difficulty,
                                  int is_block, const char *block_hash_or_null);
typedef void (*reject_observer_fn)(void *ctx, const char *worker_name,
                                   uint64_t ts_ms, const char *reason);
/* Submits a solved block to the node. Returns 0 only when the node ACCEPTED it
 * onto the best chain; non-zero for every other outcome.
 *
 * ⚠️ Non-zero is not always an error in the usual sense. Core answers
 * "inconclusive" for a block that is perfectly valid but lost the race for the
 * tip to another block at the same height. That block is not in the chain, so
 * it must not be recorded as found and must not settle payouts — see the
 * on_block_found gate in submit_share(). */
typedef int (*block_submit_fn)(void *ctx, const char *block_hex);
/* Asked once per authorize for a starting share difficulty for this worker,
 * typically from its own recent history. Returns <= 0 when nothing is known,
 * and initial_diff is used instead.
 *
 * Without this every reconnect — and every pool restart — drops every miner
 * back to initial_diff and makes it climb again, 4x per vardiff window. A
 * multi-TH/s ASIC starting at difficulty 1 floods the pool for minutes and
 * sheds shares at each step of the climb. */
typedef double (*difficulty_hint_fn)(void *ctx, const char *worker_name);
/* Fires once per solved block, after the share has been recorded, and ONLY
 * when on_block submitted it and the node accepted it. Used by main.c to
 * insert into blocks_found with reward/fee/finder address, and to settle the
 * proportional payout plan. Both of those describe a block that is in the
 * chain, so neither may run for one that is not. */
typedef void (*block_found_fn)(void *ctx,
                               const char *worker_name,
                               const char *finder_address,
                               uint64_t ts_ms, uint32_t height,
                               const char *job_id,
                               const char *block_hash,
                               int64_t reward_sats, int64_t fee_sats);

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

    /* Shared across every server in the process; NULL = allocate a private
     * one. See stratum_shared_t above — this is not optional when more than
     * one server runs, it is what keeps extranonce1 unique and share dedupe
     * effective across ports. */
    stratum_shared_t *shared;

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

/* Process one JSON-RPC line. Appends one or more newline-delimited JSON
 * messages to *out_buf (caller-owned, will be realloc'd). Returns 0 on
 * success, negative on protocol error (caller should disconnect). */
int stratum_handle_message(stratum_server_t *s, stratum_conn_t *c,
                           const char *line,
                           char **out_buf, size_t *out_len);

#endif
