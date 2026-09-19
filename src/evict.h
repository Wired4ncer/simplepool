/* evict.h — make room in a FULL block template for the pool's coinbase.
 *
 * WHY THIS EXISTS (2026-09-19, betanet): the enforcer fills its template to
 * the consensus weight limit, reserving room for exactly ITS coinbase — one
 * payout output. Ours carries up to 16 payouts, an operator fee output and a
 * tag. On an empty-ish mempool the difference is noise; on a full one it is
 * fatal in two ways, both measured on beta the day after the fork:
 *
 *   - the pool shrank every coinbase to ONE payout ("cap 1 of 16") so the
 *     block would fit — every block paid a single miner and everyone else's
 *     claims carried forward with no block ever having room for them;
 *   - even the one-output coinbase was 177 WU over, and the node rejected the
 *     block: bad-blk-weight. 1 found block in 7 lost that evening.
 *
 * bitcoind's blockmaxweight cannot help: the enforcer never reads it.
 *
 * The fix is the one every pool that builds its own coinbase has to make:
 * drop transactions from the END of the template until the coinbase we
 * actually render fits. The tail is safe to drop — the enforcer orders txs so
 * ancestors precede descendants, so the last tx never has an in-block
 * dependant. Dropping a tx changes two things the coinbase carries, and BOTH
 * are recomputed here:
 *
 *   1. the WITNESS COMMITMENT (BIP141): sha256d(witness_merkle_root ||
 *      witness_reserved_value), root over [0x00*32, wtxid_1 .. wtxid_n],
 *      reserved value = 32 zero bytes (what stratum.c puts in the submitted
 *      coinbase; see the block assembly there);
 *   2. the REWARD: the template coinbase's single spendable output = subsidy +
 *      fees of the txs it includes; the fees of the dropped txs come off.
 *
 * ⛔ SAFETY GATE. Before evicting anything, the commitment is recomputed for
 * the UNCHANGED template and compared with the enforcer's own
 * default_witness_commitment AND with the copy embedded in its coinbase (the
 * two must agree with each other and with us). If they differ, our arithmetic does not match
 * the backend's for this template and NOTHING is evicted — the caller gets
 * -1 and the old behaviour (shrink the coinbase) stands. A wrong commitment
 * is a wrong block; this gate is what makes the feature safe to switch on.
 */
#ifndef SIMPLEPOOL_EVICT_H
#define SIMPLEPOOL_EVICT_H

#include <stddef.h>
#include <stdint.h>
#include "bitcoind.h"

/* Compute the BIP141 witness commitment for the template's CURRENT tx list
 * with the all-zero reserved value. out[32] is the commitment (the 32 bytes
 * that follow 6a24aa21a9ed). Returns 0 ok, -1 on a malformed tx hex. */
int evict_witness_commitment(const bitcoind_template_t *t, uint8_t out[32],
                             char *errbuf, size_t errlen);

/* Drop transactions from the tail of `t` until the sum of tx weights is at
 * most `weight_limit - need_wu`, where need_wu is the weight the caller's
 * coinbase needs (template coinbase + growth + payouts + fee + safety).
 * Recomputes the witness commitment and writes it BOTH into
 * default_witness_commitment and into the OP_RETURN output inside
 * coinbasetxn_hex (the one the coinbase builder copies verbatim into every
 * rendered coinbase — the only copy that reaches a block), and reduces the
 * reward in coinbasetxn_hex by the dropped fees. Frees the dropped entries.
 *
 * Returns the number of txs evicted (0 when nothing had to go), or -1 and
 * leaves `t` UNTOUCHED when: the safety gate fails, the template has no
 * coinbasetxn / weight_limit, or a tx cannot be parsed. On -1 the caller must
 * fall back to the old behaviour; it must never treat -1 as "0 evicted". */
int evict_tail_for_coinbase(bitcoind_template_t *t, int64_t need_wu,
                            int64_t *out_evicted_wu, int64_t *out_evicted_fee,
                            char *errbuf, size_t errlen);

/* Rewrite the value of the template coinbase's single spendable output
 * (hex in place, same length). Exposed for tests. Returns 0 ok, -1 on a
 * coinbase without exactly one spendable output or a malformed hex. */
int evict_adjust_template_reward(char *coinbasetxn_hex, int64_t delta_sats,
                                 char *errbuf, size_t errlen);

#endif
