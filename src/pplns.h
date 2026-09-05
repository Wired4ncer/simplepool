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

/* Histogram length: one bucket per possible payout-output size in bytes. The
 * largest output this pool emits is 8 + 1 + 34 = 43 B (P2TR / P2WSH), so 44
 * buckets cover every size including zero. */
#define PPLNS_TXOUT_HIST_LEN 44

/* Count the DISTINCT addresses this block could pay, by the size in bytes of
 * the coinbase output each would need, into hist[0..hist_len-1].
 *
 * ⛔ The dedupe is not an optimisation, it is the correctness condition. The
 * working set pplns_compute_payouts builds is one entry PER DISTINCT ADDRESS
 * across the window and the claim ledger (see find_work), and an address in
 * both is the ordinary case: an in-window miner also carrying a claim too
 * small to have been paid last time. Counting it twice inflates the multiset
 * and re-creates the over-charge the caller is sizing to avoid.
 *
 * Returns the number of distinct addresses counted. Zero means the caller must
 * fall back to charging the maximum per output — that is also what happens if
 * the internal allocation fails, which is why the count is returned rather
 * than inferred from the histogram. */
size_t pplns_candidate_txout_hist(const pplns_addr_t *addrs, size_t n_addrs,
                                  const pplns_claim_t *ledger_in,
                                  size_t n_ledger_in,
                                  size_t *hist, size_t hist_len);

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
 * carry_slots: how many of max_outputs are reserved for the addresses owed most
 *         from PREVIOUS blocks, rather than for the largest claims this one.
 *         0 restores the pure largest-claim selection exactly.
 *
 *         Why this exists: ranking on `claim` alone is window_fraction +
 *         old_claim, and a large miner's window fraction on its own dwarfs the
 *         biggest carry a small miner can accumulate. The large miners
 *         therefore retake the top slots on EVERY block -- their negative carry
 *         never gets deep enough to fall below the small claimants before the
 *         next block's window fraction tops them back up -- and the deferred
 *         queue does not advance. Measured on alphanet 2026-09-05: over 31
 *         blocks, 279 payout slots reached 34 distinct addresses, 12 of which
 *         took 91%, while 88 addresses holding 1.2751 ECX of claims were paid
 *         nothing. 28 of those were already above min_payout_sats, so the floor
 *         was not what excluded them.
 *
 *         The priority signal is `old_claim`, the carry the ledger already
 *         holds, and NOT a timestamp. It needs no new column and cannot go
 *         stale: what the ledger owes an address from earlier blocks IS how
 *         long and how badly it has been deferred, and an address just paid is
 *         carry-NEGATIVE and so sorts to the bottom by construction.
 *         (prop_ledger.last_settled_ts cannot serve here -- it is rewritten for
 *         every row on every settlement, paid or not.)
 *
 *         Reserved slots the carry pass cannot fill are handed BACK to the
 *         largest-claim selection, so a non-zero carry_slots can never pay
 *         fewer addresses than 0 would have.
 *
 *         This does not change what anyone is OWED -- the ledger is zero-sum
 *         either way -- only how often they are paid. Nor does it widen the
 *         coinbase: at most max_outputs are emitted whichever addresses are
 *         chosen, which is exactly the bound coinbase_max_payout_outputs_bytes
 *         relies on (it charges the k LARGEST candidates by byte size, so it is
 *         an upper bound whichever k are selected).
 *
 * Returns 0 on success, -1 on invalid input — which now includes a stored
 * ledger that is materially not zero-sum. That is unrepresentable rather than
 * merely unusual: every block pays exactly one reward, so a ledger summing to
 * anything else describes claims no coinbase can settle. Refusing hands the
 * caller its documented fallback (pay the finder directly) instead of quietly
 * assigning the imbalance to whoever happens to be largest. */
/* n_eligible_out (optional): how many addresses cleared the payout floor with
 * max_outputs IGNORED. It is what makes "the output cap cost someone a payout"
 * answerable: fewer payouts than candidates is the ordinary state — most
 * candidates are below the floor or repaying an advance — so a caller
 * comparing payouts against the candidate count reports a loss on nearly every
 * block. Compare against this instead. It is measured before renormalisation,
 * so it is an upper bound on what an uncapped run would have emitted, and a
 * caller must not state it as an exact count of payouts lost. */
int pplns_compute_payouts(int64_t reward_after_fee,
                          const pplns_addr_t *addrs, size_t n_addrs,
                          pplns_claim_t *ledger, size_t ledger_cap,
                          size_t n_ledger_in, size_t *n_ledger_out,
                          int64_t min_payout_sats, size_t max_outputs,
                          size_t carry_slots,
                          pplns_payout_t *payouts, size_t *n_payouts_out,
                          size_t *n_eligible_out);

#endif /* SIMPLEPOOL_PPLNS_H */
