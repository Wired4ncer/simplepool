#ifndef SIMPLEPOOL_BITCOIND_H
#define SIMPLEPOOL_BITCOIND_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char url[512];
    char user[128];
    char pass[256];
    long timeout_ms;          /* libcurl timeout, default 10000 */
} bitcoind_cfg_t;

typedef struct {
    /* opaque CURL handle + lock */
    void *_curl;
    void *_lock;              /* pthread_mutex_t* */
    bitcoind_cfg_t cfg;
} bitcoind_client_t;

typedef struct bitcoind_template_tx {
    char *data_hex;           /* owned */
    char txid_hex[65];        /* null-terminated hex */
    int64_t fee;
    int weight;
} bitcoind_template_tx_t;

typedef struct {
    int height;
    char prev_hash_hex[65];
    int64_t coinbase_value_sats;
    char target_hex[65];      /* 32-byte target as hex */
    uint32_t bits;            /* nbits */
    uint32_t curtime;
    int32_t version;
    char *default_witness_commitment; /* owned, may be NULL */
    int64_t min_time;
    /* Server-provided coinbase (BIP22 "coinbasetxn"), as full serialized tx
     * hex. NULL when the server returns "coinbasevalue" and we build our own
     * coinbase. Set by backends like the CUSF enforcer that must dictate the
     * coinbase (mandatory BIP300/301 commitments). When set,
     * coinbase_value_sats is derived from the tx's total output value. */
    char *coinbasetxn_hex;            /* owned, may be NULL */
    /* BIP22 long-poll token from the server, or NULL when the server does
     * not support long polling. Echo it via
     * bitcoind_get_block_template_lp() to have the next request parked
     * until the template goes stale. */
    char *longpollid;                 /* owned, may be NULL */
    bitcoind_template_tx_t *txs;       /* owned array */
    size_t tx_count;
    /* BIP22 "weightlimit" — the block weight the template was built against.
     * 0 when the server omits it, in which case callers must assume nothing
     * about headroom rather than guessing a limit. Needed because our coinbase
     * adds outputs the server did NOT budget weight for: the enforcer reserves
     * room for exactly one payout txout. */
    int64_t weight_limit;
} bitcoind_template_t;

/* Initialize a client. Does NOT make a network call. Returns 0 on success,
 * negative on bad config. */
int bitcoind_client_init(bitcoind_client_t *out, const bitcoind_cfg_t *cfg);

/* Free internal resources. */
void bitcoind_client_free(bitcoind_client_t *c);

/* Sanity ping — calls getblockchaininfo. Returns 0 ok, negative on RPC/net
 * error. Useful at startup. */
int bitcoind_ping(bitcoind_client_t *c, char *errbuf, size_t errlen);

/* Which chain the backend reports (getblockchaininfo `chain`): "main",
 * "test", "signet" or "regtest". Returns 0 and fills `out` on success.
 * Negative when the backend does not implement getblockchaininfo — the CUSF
 * enforcer does not — in which case the caller must infer the network some
 * other way rather than assert one. */
int bitcoind_get_chain(bitcoind_client_t *c, char *out, size_t cap,
                       char *errbuf, size_t errlen);

/* Fetch a block template. Allocates the result; caller must call
 * bitcoind_template_free. Returns 0 ok, negative on error. */
int bitcoind_get_block_template(bitcoind_client_t *c,
                                bitcoind_template_t **out,
                                char *errbuf, size_t errlen);

/* Like bitcoind_get_block_template, but passes a BIP22 `longpollid` (from a
 * previous template's ->longpollid) so a supporting server parks the request
 * until the template is stale. The request can legitimately take tens of
 * seconds, so use a dedicated client whose cfg.timeout_ms comfortably
 * exceeds the server's long-poll window — a parked request holds the
 * client's connection lock, which would stall submitblock on a shared
 * client. longpollid == NULL behaves exactly like the plain variant. */
int bitcoind_get_block_template_lp(bitcoind_client_t *c,
                                   const char *longpollid,
                                   bitcoind_template_t **out,
                                   char *errbuf, size_t errlen);

/* Submit a serialized block (raw hex). Returns 0 if accepted, negative on
 * error. errbuf gets the rejection reason on negative. */
int bitcoind_submit_block(bitcoind_client_t *c, const char *block_hex,
                          char *errbuf, size_t errlen);

/* Returned when the backend does not implement the method at all, as opposed
 * to failing to answer it. The CUSF enforcer serves exactly getblocktemplate
 * and submitblock, so callers must treat this as a routine answer and fall
 * back — not as an error to retry. */
#define BITCOIND_ERR_UNSUPPORTED (-32)

/* The hash of the block at `height` on the backend's active chain, written to
 * `out` as lowercase hex. This is how a block the pool submitted is checked
 * to still be in the chain: a hash that differs from ours means ours was
 * reorged out.
 *
 * Returns 0 on success, BITCOIND_ERR_UNSUPPORTED when the backend does not
 * serve getblockhash, and another negative value on a real failure. */
int bitcoind_get_block_hash(bitcoind_client_t *c, int height,
                            char *out, size_t cap,
                            char *errbuf, size_t errlen);

void bitcoind_template_free(bitcoind_template_t *t);

/* Internal helper exposed for testing: parse a getblocktemplate JSON
 * response into a template struct. `result_json` is the value of the JSON
 * "result" field (cJSON object). Returns 0 ok, negative on missing fields. */
int bitcoind_parse_template(void /* cJSON* */ *result_json,
                            bitcoind_template_t **out,
                            char *errbuf, size_t errlen);

#endif
