#ifndef SIMPLEPOOL_PPLNS_H
#define SIMPLEPOOL_PPLNS_H

#include <stddef.h>
#include <stdint.h>

/* One address in the PPLNS window, aggregated by payout address. */
typedef struct {
    char    address[128];
    double  total_difficulty;
} pplns_addr_t;

/* A deferred claim against future blocks, carried as a SIGNED FRACTION of one
 * block reward rather than as sats.
 *
 * Why not sats: a coinbase pays exactly one block reward, no more and no less.
 * Holding back sats for a small miner means that block's coinbase pays less
 * than the reward — and the shortfall is never minted, so the debt is backed by
 * nothing — while releasing it later would need a coinbase that overpays, which
 * is an invalid block.
 *
 * Why not raw difficulty: shares stay in the PPLNS window across several
 * blocks, so rolling an unpaid miner's difficulty forward would count the same
 * work twice. Difficulty is also not comparable across time on this chain —
 * it swings +/-4x every 2016 blocks and resets to powLimit at each fork — so a
 * claim recorded in difficulty units silently changes meaning at every
 * retarget. A fraction of a block reward does not.
 *
 * Semantics: claim_fraction > 0 means the address was skipped and is owed that
 * fraction of a future block; < 0 means it was paid early, covering someone
 * else's skipped share, and owes it back. The pool holds no funds either way —
 * this is a fairness memory, not a balance.
 *
 * INVARIANT: the ledger sums to zero. Every block pays out exactly one reward,
 * so an advance to one miner is always a deferral by another. */
typedef struct {
    char    address[128];
    double  claim_fraction;
} pplns_claim_t;

/* One coinbase payout output. */
typedef struct {
    char    address[128];
    int64_t sats;
} pplns_payout_t;

/* Compute the coinbase payout list and the updated deferred-claim ledger.
 *
 * reward_after_fee: sats available to miners (block reward minus operator fee).
 *                   The payouts ALWAYS sum to exactly this. That is the
 *                   property the whole design turns on.
 * addrs / n_addrs: window shares aggregated by payout address. Their summed
 *         difficulty IS the denominator — there is deliberately no separate
 *         window_difficulty parameter, because a caller that measured the
 *         window with a second query can supply a total that disagrees with
 *         these rows, and fractions that do not sum to 1.0 corrupt the ledger
 *         update below rather than merely rounding it.
 * ledger: in/out. On input, the stored claims. On output, the claims after this
 *         block, including addresses that appear only in the window. Needs
 *         capacity for n_addrs + n_ledger_in entries.
 * min_payout_sats: an address whose cut falls below this is deferred rather
 *         than paid, and its claim rolls forward. If that would leave nobody to
 *         pay, the single largest claim is paid anyway — a block must pay
 *         someone, and the threshold is a convenience, not a rule.
 *         Applied to the amount ACTUALLY PAID, after renormalisation over the
 *         emitted set — never to the pre-renormalisation cut, which is larger
 *         whenever the ledger holds a negative claim. No emitted payout is ever
 *         below this value, and since it is itself floored at the dust limit,
 *         no emitted payout is ever dust. The direction is one-sided on
 *         purpose: someone whose cut just cleared the threshold may still be
 *         deferred, which the ledger remembers and pays later, whereas emitting
 *         a dust output fails the whole coinbase build.
 * max_outputs: cap on payout outputs (block weight, see the plan note §2). The
 *         smallest cuts are deferred until the list fits.
 *
 * Returns 0 on success, -1 on invalid input — which now includes a stored
 * ledger that is materially not zero-sum. That is unrepresentable rather than
 * merely unusual: every block pays exactly one reward, so a ledger summing to
 * anything else describes claims no coinbase can settle. Refusing hands the
 * caller its documented fallback (pay the finder directly) instead of quietly
 * assigning the imbalance to whoever happens to be largest. */
int pplns_compute_payouts(int64_t reward_after_fee,
                          const pplns_addr_t *addrs, size_t n_addrs,
                          pplns_claim_t *ledger, size_t ledger_cap,
                          size_t n_ledger_in, size_t *n_ledger_out,
                          int64_t min_payout_sats, size_t max_outputs,
                          pplns_payout_t *payouts, size_t *n_payouts_out);

#endif /* SIMPLEPOOL_PPLNS_H */
