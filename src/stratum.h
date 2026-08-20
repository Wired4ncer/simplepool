#ifndef SIMPLEPOOL_STRATUM_H
#define SIMPLEPOOL_STRATUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* coinbase_payout_t, used by stratum_job_set_payouts below. */
#include "coinbase.h"

typedef struct stratum_job stratum_job_t;

/* Create a job from template fields. The coinbase is *not* baked into the
 * job — each connection renders its own coinbase paying its miner address
 * (minus the configured operator fee). The job carries everything else
 * the server needs to materialise a per-connection coinbase on demand:
 *   - value_sats:           coinbasevalue from getblocktemplate
 *   - witness_commitment_hex: optional, may be NULL
 *   - en1_size / en2_size:  extranonce sizes, both currently 4
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
typedef void (*block_submit_fn)(void *ctx, const char *block_hex);
/* Fires once per solved block, after the share has been recorded. Used by
 * main.c to insert into blocks_found with reward/fee/finder address. */
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

    /* Drop a connection whose recv() has been silent for this long. Guards
     * against half-open TCPs from crashed miners and misconfigured clients
     * that connect but never authenticate. 0 disables (legacy). Default 600. */
    int    idle_timeout_sec;

    void  *ctx;
    share_observer_fn  on_share;
    reject_observer_fn on_reject;
    block_submit_fn    on_block;
    block_found_fn     on_block_found;
} stratum_cfg_t;

typedef struct stratum_server stratum_server_t;

int  stratum_server_start(const stratum_cfg_t *cfg, stratum_server_t **out);
/* Atomically swap the current job. Takes ownership of new_job. */
void stratum_server_set_job(stratum_server_t *s, stratum_job_t *new_job);
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
int         stratum_conn_subscribed_for_test(const stratum_conn_t *c);

/* Apply the same socket options the listener applies to every accepted
 * connection: TCP_NODELAY, SO_KEEPALIVE + TCP_KEEP{IDLE,INTVL,CNT}, and
 * SO_RCVTIMEO (poll interval derived from idle_timeout_sec). Exposed for
 * tests. */
int stratum_socket_setup_for_test(int fd, int idle_timeout_sec);

/* Process one JSON-RPC line. Appends one or more newline-delimited JSON
 * messages to *out_buf (caller-owned, will be realloc'd). Returns 0 on
 * success, negative on protocol error (caller should disconnect). */
int stratum_handle_message(stratum_server_t *s, stratum_conn_t *c,
                           const char *line,
                           char **out_buf, size_t *out_len);

#endif
