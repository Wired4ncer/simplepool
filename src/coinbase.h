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

/* Serialized size of the template coinbase's single spendable output, i.e. the
 * bytes our first payout output replaces. Needed to size a coinbase byte
 * budget exactly rather than assuming the template pays the same address type
 * we do. Same parse and same refusals as coinbase_template_reward().
 * Returns 0 ok, negative on error. */
int coinbase_template_payout_slot_bytes(const char *coinbase_tx_hex,
                                        size_t *out_bytes,
                                        char *errbuf, size_t errlen);

/* Weight of one extra coinbase payout output, sized for the LARGEST kind this
 * pool will emit: 8-byte value + 1-byte script length + 34-byte P2TR
 * scriptPubKey = 43 bytes, non-witness, so x4.
 *
 * ⛔ 172, NOT 124. It was 124 — the P2WPKH figure — from before taproot payouts
 * were accepted, and leaving it there would have been a silent block-loss bug
 * rather than a cosmetic one: this constant divides the weight headroom to
 * decide how many payouts fit, so understating an output's cost lets MORE
 * outputs through than actually fit. Sixteen taproot payouts would have been
 * 768 WU heavier than budgeted, comfortably past COINBASE_WEIGHT_SAFETY_WU, and
 * the block is then rejected at submitblock — the exact "costs the entire
 * block" failure the note below this one warns about.
 *
 * One number for every output type, deliberately, rather than measuring each
 * payout's real script: this whole helper over-states on purpose (it counts the
 * template coinbase as all-non-witness for the same reason), and the operator's
 * `prop_max_outputs` ceiling is what binds in practice — the weight bound only
 * takes over on a nearly-full block, which is precisely when erring small is
 * worth the lost capacity. */
#define COINBASE_PAYOUT_TXOUT_WU 172

/* Weight deliberately left unused. The server's accounting and ours can differ
 * by a few bytes (varint boundaries, a server that revises its coinbase), and
 * being one weight unit over the limit costs the entire block. */
#define COINBASE_WEIGHT_SAFETY_WU 400

/* How many payout outputs a template can carry without going overweight.
 *
 * A server that dictates the coinbase budgets weight for the outputs IT put
 * there — the CUSF enforcer reserves room for exactly one payout txout. Every
 * output we add beyond that, and the extranonce and tag we splice into the
 * scriptSig, is weight the template did not plan for. On a nearly-full block
 * that surplus is what pushes it past the limit and gets it rejected at
 * submitblock.
 *
 * weight_limit:            BIP22 "weightlimit"; <= 0 means the server did not
 *                          say, so `ceiling` is returned unchanged — with
 *                          nothing to measure, guessing a limit is worse than
 *                          deferring to the operator.
 * tx_weight_total:         summed "weight" of the template's transactions.
 * template_coinbase_bytes: serialized length of the template's coinbase.
 *                          Counted as all-non-witness (x4), which over-states
 *                          it and therefore errs toward fewer outputs.
 * scriptsig_growth_bytes:  extranonce + tag bytes the builder will splice in.
 * fee_output:              non-zero if an operator fee output will be emitted;
 *                          it consumes one of the available slots.
 * ceiling:                 operator's configured maximum.
 *
 * Returns at least 1 — one payout output is already inside the server's own
 * budget, and a block must pay someone. *out_headroom_wu (optional) receives
 * the computed spare weight, or -1 when it could not be computed. */
/* Serialized size of the payout output this address produces: 8-byte value +
 * 1-byte script-length varint + its scriptPubKey. 31 bytes for P2WPKH, 43 for
 * P2TR and P2WSH, 34 for P2PKH, 32 for P2SH.
 *
 * An address this pool cannot decode is charged the LARGEST it emits (43)
 * rather than skipped. Such an address should never reach a payout set --
 * stratum validates through this same decoder at authorize -- but if one does,
 * over-charging costs a payout slot while under-charging breaks the budget the
 * caller asked for. */
size_t coinbase_payout_txout_bytes(const char *address);

/* How many payout outputs fit inside a serialized-coinbase BYTE budget.
 *
 * This is a marketplace-compatibility limit, NOT a consensus one. See
 * prop_max_coinbase_bytes in config.h for why it exists: a hashrate
 * marketplace validates our coinbase before placing an order, and the same
 * payout COUNT is a different SIZE depending on address type, so a pool that
 * only caps the count can drift across a size threshold it cannot see.
 *
 * ⛔ payout_txout_bytes must be the LARGEST output in the candidate set, not an
 * average and not the first one. The payouts actually emitted are a subset of
 * the candidates chosen by claim size, so any smaller figure lets the real
 * coinbase exceed the budget.
 *
 * budget_bytes 0 disables the cap and returns `ceiling` unchanged.
 *
 * ⚠️ Deliberately conservative: template_coinbase_bytes is counted in full,
 * including the spendable output that our first payout REPLACES (~31 B).
 * Parsing the template to reclaim those bytes would make the estimate exact
 * and the failure direction worse -- a wrong parse would overstate the budget
 * and put us back across the threshold silently, which is the failure this
 * exists to prevent. Erring ~31 B small costs at most one payout slot.
 *
 * Returns at least 1: a block must pay someone, and a budget too small for
 * even one payout is a misconfiguration to be reported, not a block to be
 * suppressed. */
size_t coinbase_max_payout_outputs_bytes(size_t template_coinbase_bytes,
                                         size_t template_slot_bytes,
                                         size_t scriptsig_growth_bytes,
                                         size_t payout_txout_bytes,
                                         int fee_output,
                                         size_t budget_bytes,
                                         size_t ceiling);

size_t coinbase_max_payout_outputs(int64_t weight_limit,
                                   int64_t tx_weight_total,
                                   size_t template_coinbase_bytes,
                                   size_t scriptsig_growth_bytes,
                                   int fee_output,
                                   size_t ceiling,
                                   int64_t *out_headroom_wu);

void coinbase_parts_free(coinbase_parts_t *p);

/* Count a serialized coinbase's outputs, split into spendable and OP_RETURN.
 * Either out-param may be NULL. The OP_RETURN count distinguishes a coinbase
 * we built (one output: the witness commitment) from one dictated by the CUSF
 * enforcer (plus the mandatory BIP300/301 commitments), which is what tells an
 * observer whether a sidechain can be merge-mined into these blocks.
 * Returns 0 ok, negative on malformed input. */
int coinbase_count_outputs(const char *tx_hex, int *spendable_out,
                           int *op_return_out);

/* The network an address encodes ("main", "regtest", "test/signet",
 * "test/signet/regtest"), or NULL when it parses as neither bech32 nor
 * base58check. Coarser than getblockchaininfo's `chain`: several networks
 * share version bytes and HRPs, and this reports only what the encoding
 * proves. Useful when the block-template backend cannot be asked. */
const char *coinbase_address_network(const char *addr);

/* Whether a chain name — from getblockchaininfo or from
 * coinbase_address_network() — means mainnet. Anything unrecognised counts
 * as a test chain, so a name we don't know never reads as "main". */
int coinbase_network_is_mainnet(const char *network);

/* Internal helpers exposed for tests. */
int coinbase_address_to_script(const char *addr,
                               uint8_t *out, size_t cap, size_t *out_len,
                               char *errbuf, size_t errlen);

#endif
