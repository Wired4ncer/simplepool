#ifndef SIMPLEPOOL_COINBASE_H
#define SIMPLEPOOL_COINBASE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *cb1;
    size_t   cb1_len;
    uint8_t *cb2;
    size_t   cb2_len;
} coinbase_parts_t;

/* Bitcoin's standard relay dust threshold for legacy outputs. Below this an
 * output would not be relayed, so sub-dust operator fees and PPLNS payouts
 * are dropped or carried forward instead of emitted. */
#define COINBASE_DUST_SATS 546

/* One payout destination for coinbase_build_from_template_multi(). */
typedef struct {
    const char *address;
    int64_t     sats;
} coinbase_payout_t;

/* Build coinbase1/coinbase2 halves around the extranonce placeholder,
 * single-payout — the entire value_sats goes to payout_address.
 *
 * Equivalent to coinbase_build_split with operator_address=NULL / fee_bps=0.
 * Kept for tests and simple solo configurations.
 *
 * `witness_commitment_hex` may be NULL.
 * Returns 0 ok, negative on error (errbuf populated). */
int coinbase_build(uint32_t height, int64_t value_sats,
                   const char *payout_address,
                   const char *witness_commitment_hex,
                   const char *coinbase_tag,
                   size_t extranonce1_size, size_t extranonce2_size,
                   coinbase_parts_t *out, char *errbuf, size_t errlen);

/* Build coinbase1/coinbase2 with a two-way split:
 *   fee_sats   = value_sats * fee_bps / 10000  (rounded down)
 *   miner_sats = value_sats - fee_sats
 *
 * If `operator_address` is NULL/empty, fee_bps is 0, or `fee_sats` would
 * be below the dust threshold (~546 sats), the whole reward goes to the
 * miner and *out_fee_sats is set to 0. Otherwise both outputs are emitted.
 *
 * *out_miner_sats and *out_fee_sats receive the final split (may be NULL).
 *
 * Returns 0 ok, negative on error. */
int coinbase_build_split(uint32_t height, int64_t value_sats,
                         const char *miner_address,
                         const char *operator_address,
                         int fee_bps,
                         const char *witness_commitment_hex,
                         const char *coinbase_tag,
                         size_t extranonce1_size, size_t extranonce2_size,
                         coinbase_parts_t *out,
                         int64_t *out_miner_sats, int64_t *out_fee_sats,
                         char *errbuf, size_t errlen);

/* Build coinbase1/coinbase2 halves from a server-provided coinbase
 * transaction (BIP22 "coinbasetxn", e.g. from the CUSF enforcer), rather
 * than constructing the coinbase from scratch.
 *
 * `coinbase_tx_hex` is the full serialized coinbase tx (segwit or legacy).
 * The builder:
 *   - appends the extranonce placeholder to the existing scriptSig, after
 *     the BIP34 height push the server already placed there;
 *   - replaces the single spendable (non-OP_RETURN) output — which the
 *     server pays to its own reward address — with the miner payout and an
 *     optional operator-fee output (same split rule as coinbase_build_split);
 *   - preserves every other output byte-for-byte and in order (BIP300/301
 *     commitment OP_RETURNs and the segwit witness commitment).
 *
 * cb1/cb2 are the legacy (no-witness) serialization. *out_has_witness (if
 * non-NULL) reports whether the source tx was segwit-serialized, so the
 * caller can re-attach the witness reserved value when it assembles the
 * block. *out_miner_sats / *out_fee_sats receive the split (may be NULL).
 *
 * `operator_address` / `coinbase_tag` may be NULL. Returns 0 ok, negative on
 * error (errbuf populated). */
int coinbase_build_from_template(const char *coinbase_tx_hex,
                                 const char *miner_address,
                                 const char *operator_address,
                                 int fee_bps,
                                 const char *coinbase_tag,
                                 size_t extranonce1_size,
                                 size_t extranonce2_size,
                                 coinbase_parts_t *out,
                                 int *out_has_witness,
                                 int64_t *out_miner_sats,
                                 int64_t *out_fee_sats,
                                 char *errbuf, size_t errlen);

/* Same as coinbase_build_from_template, but the single spendable reward output
 * is replaced by N payout outputs (in the order given) plus an optional
 * operator-fee output. The caller is responsible for ensuring the payout list
 * fits the block weight budget; this builder only enforces:
 *   - sum(payouts[i].sats) + operator_fee == reward
 *   - every payout and the operator output are at or above COINBASE_DUST_SATS
 *   - scriptSig length stays within the 100-byte coinbase limit
 *
 * out_total_payout_sats receives sum(payouts[i].sats) (may be NULL).
 * out_fee_sats receives the operator fee (may be NULL).
 * Returns 0 ok, negative on error (errbuf populated). */
int coinbase_build_from_template_multi(const char *coinbase_tx_hex,
                                       const coinbase_payout_t *payouts,
                                       size_t n_payouts,
                                       const char *operator_address,
                                       int fee_bps,
                                       const char *coinbase_tag,
                                       size_t extranonce1_size,
                                       size_t extranonce2_size,
                                       coinbase_parts_t *out,
                                       int *out_has_witness,
                                       int64_t *out_total_payout_sats,
                                       int64_t *out_fee_sats,
                                       char *errbuf, size_t errlen);

/* Read the value of a template coinbase's single spendable output — the figure
 * coinbase_build_from_template_multi() derives its fee from and requires the
 * payout list to sum to. Callers computing a payout split must use this rather
 * than the template's "coinbasevalue" field: the builder works off the
 * serialized transaction, and a split computed against a different number is
 * rejected at render time. Refuses a coinbase without exactly one spendable
 * output, matching the builder. Returns 0 ok, negative on error. */
int coinbase_template_reward(const char *coinbase_tx_hex, int64_t *out_reward,
                             char *errbuf, size_t errlen);

void coinbase_parts_free(coinbase_parts_t *p);

/* Count a serialized coinbase's outputs, split into spendable and OP_RETURN.
 * Either out-param may be NULL. The OP_RETURN count distinguishes a coinbase
 * we built (one output: the witness commitment) from one dictated by the CUSF
 * enforcer (plus the mandatory BIP300/301 commitments), which is what tells an
 * observer whether a sidechain can be merge-mined into these blocks.
 * Returns 0 ok, negative on malformed input. */
int coinbase_count_outputs(const char *tx_hex, int *spendable_out,
                           int *op_return_out);

/* Internal helpers exposed for tests. */
int coinbase_address_to_script(const char *addr,
                               uint8_t *out, size_t cap, size_t *out_len,
                               char *errbuf, size_t errlen);

#endif
