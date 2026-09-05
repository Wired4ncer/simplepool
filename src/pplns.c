#define _POSIX_C_SOURCE 200809L
#include "pplns.h"
#include "coinbase.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A claim is only worth carrying if it is worth at least one satoshi of the
 * block that produced it. Anything smaller is floor() residue, not a deferral.
 *
 * This has to be satoshi-RELATIVE, not a fixed constant: at a ~3.2 ECX reward a
 * single satoshi is ~3.2e-9 of the block, so a fixed 1e-9 epsilon kept residue
 * worth a fraction of a satoshi. That made every paid address linger in the
 * ledger, inflated the "deferred claims" count in the logs to something
 * meaningless, and would have grown prop_ledger by a row per address per block
 * forever. */
static double claim_epsilon(int64_t reward_after_fee) {
    return (reward_after_fee > 0) ? (1.0 / (double)reward_after_fee) : 1e-12;
}

/* Internal working state for one address. */
typedef struct {
    char    address[128];
    double  window_fraction;  /* this block's pro-rata share, from shares */
    double  old_claim;        /* signed carry from the ledger */
    double  claim;            /* window_fraction + old_claim */
    int64_t paid;             /* sats actually emitted */
    int     emit;
} work_t;

static work_t *find_work(work_t *w, size_t n, const char *addr) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(w[i].address, addr) == 0) return &w[i];
    }
    return NULL;
}

static int cmp_claim_desc(const void *a, const void *b) {
    const work_t *const *x = (const work_t *const *)a;
    const work_t *const *y = (const work_t *const *)b;
    if ((*x)->claim > (*y)->claim) return -1;
    if ((*x)->claim < (*y)->claim) return 1;
    return 0;
}

/* Deferral order: how much this address was owed BEFORE this block's shares.
 *
 * Sorting on old_claim rather than claim is the whole point of the reserved
 * slots. claim includes window_fraction, which is precisely the term that lets
 * a large miner outrank every deferred small one on every block. old_claim is
 * what the pool still owes from earlier blocks and nothing else, so the
 * longest-deferred address sorts first and an address just paid -- which went
 * carry-negative absorbing the deferred share as an advance -- sorts last. */
static int cmp_old_claim_desc(const void *a, const void *b) {
    const work_t *const *x = (const work_t *const *)a;
    const work_t *const *y = (const work_t *const *)b;
    if ((*x)->old_claim > (*y)->old_claim) return -1;
    if ((*x)->old_claim < (*y)->old_claim) return 1;
    return 0;
}

/* Documented in pplns.h. Mirrors find_work's dedupe deliberately: same rule,
 * same two arrays, same order, so the set sized here is the set that gets
 * paid. It is O(n^2) in the number of addresses, exactly like find_work — a
 * pool with hundreds of addresses in the window measures in microseconds, and
 * matching the existing rule is worth more here than a faster one that could
 * drift away from it. */
size_t pplns_candidate_txout_hist(const pplns_addr_t *addrs, size_t n_addrs,
                                  const pplns_claim_t *ledger_in,
                                  size_t n_ledger_in,
                                  size_t *hist, size_t hist_len)
{
    if (!hist || hist_len == 0) return 0;
    for (size_t i = 0; i < hist_len; i++) hist[i] = 0;

    size_t cap = n_addrs + n_ledger_in;
    if (cap == 0) return 0;
    const char **seen = (const char **)calloc(cap, sizeof(*seen));
    if (!seen) return 0;   /* caller charges the maximum: the safe direction */
    size_t nseen = 0;

    for (size_t pass = 0; pass < 2; pass++) {
        size_t n = pass == 0 ? n_addrs : n_ledger_in;
        for (size_t i = 0; i < n; i++) {
            const char *a = pass == 0 ? addrs[i].address : ledger_in[i].address;
            if (!a || !a[0]) continue;
            int dup = 0;
            for (size_t k = 0; k < nseen; k++)
                if (strcmp(seen[k], a) == 0) { dup = 1; break; }
            if (dup) continue;
            seen[nseen++] = a;
            size_t b = coinbase_payout_txout_bytes(a);
            if (b >= hist_len) b = hist_len - 1;
            hist[b]++;
        }
    }
    free(seen);
    return nseen;
}

int pplns_compute_payouts(int64_t reward_after_fee,
                          const pplns_addr_t *addrs, size_t n_addrs,
                          pplns_claim_t *ledger, size_t ledger_cap,
                          size_t n_ledger_in, size_t *n_ledger_out,
                          int64_t min_payout_sats, size_t max_outputs,
                          size_t carry_slots,
                          pplns_payout_t *payouts, size_t *n_payouts_out,
                          size_t *n_eligible_out) {
    if (n_eligible_out) *n_eligible_out = 0;
    if (!addrs || n_addrs == 0 || !payouts || !n_payouts_out ||
        !ledger || !n_ledger_out ||
        reward_after_fee <= 0 || min_payout_sats < COINBASE_DUST_SATS ||
        max_outputs == 0 || ledger_cap < n_addrs + n_ledger_in) {
        return -1;
    }

    /* The denominator is the difficulty ACTUALLY SUPPLIED here, summed, not a
     * window total measured by a separate query.
     *
     * This used to be a caller-provided window_difficulty, and the two can
     * disagree: store_prop_compute_window() walks the window with its own
     * cursor while store_prop_window_addrs() aggregates it with a second query,
     * and a share committed between them — or a share the walk dropped at a
     * page boundary — lands in one and not the other. Every fraction is then
     * measured against a total that is not the total, the fractions no longer
     * sum to 1.0, and the zero-sum ledger update below silently mis-assigns the
     * difference to the largest claimant. Deriving the denominator here makes
     * "the fractions sum to one" structural rather than a caller's promise. */
    double denom = 0.0;
    for (size_t i = 0; i < n_addrs; i++) {
        if (addrs[i].address[0] == '\0') continue;
        if (!isfinite(addrs[i].total_difficulty) ||
            addrs[i].total_difficulty < 0.0) return -1;
        denom += addrs[i].total_difficulty;
    }
    if (!(denom > 0.0)) return -1;

    /* The working set is every address in the window, plus every address that
     * still holds a claim from an earlier block — a miner who has gone away is
     * still owed, and still gets paid out of a later block. */
    size_t cap = n_addrs + n_ledger_in;
    work_t *w = (work_t *)calloc(cap, sizeof(*w));
    if (!w) return -1;
    size_t nw = 0;

    for (size_t i = 0; i < n_addrs; i++) {
        if (addrs[i].address[0] == '\0') continue;
        work_t *e = find_work(w, nw, addrs[i].address);
        if (!e) {
            e = &w[nw++];
            snprintf(e->address, sizeof e->address, "%s", addrs[i].address);
        }
        /* Duplicate rows are summed rather than overwritten — the store groups
         * by address, but nothing in the type says it must. */
        e->window_fraction += addrs[i].total_difficulty / denom;
    }
    for (size_t i = 0; i < n_ledger_in; i++) {
        if (ledger[i].address[0] == '\0') continue;
        work_t *e = find_work(w, nw, ledger[i].address);
        if (!e) {
            e = &w[nw++];
            snprintf(e->address, sizeof e->address, "%s", ledger[i].address);
        }
        e->old_claim += ledger[i].claim_fraction;
    }
    if (nw == 0) { free(w); return -1; }

    for (size_t i = 0; i < nw; i++) {
        w[i].claim = w[i].window_fraction + w[i].old_claim;
        if (!isfinite(w[i].claim)) { free(w); return -1; }
    }

    /* The ledger update at the bottom gives the largest emitted claim whatever
     * residual keeps the ledger summing to zero. That IS its honest residual —
     * but only while the working set's claims sum to exactly one block reward.
     * Off that, the forced value is wrong by the imbalance, and can even invert
     * the sign: an address that over-received gets recorded as owed, and is
     * paid again on the next block.
     *
     * Window fractions sum to 1.0 by construction now, so the only way to miss
     * is a stored ledger that is not itself zero-sum.
     *
     * The tolerance is relative and deliberately loose, because this is a
     * corruption backstop and not a noise policeman. Legitimate drift comes
     * from pruning claims worth under one satoshi, which is a few parts in
     * 10^7 of a reward at any realistic participant count — and it cannot be
     * expressed in satoshis here anyway, since a claim is a fraction of the
     * block that MINTED it and rewards differ block to block. What this must
     * catch is the failure above, where the imbalance is a whole miner's share:
     * the sign-flip case is 0.4 of a reward, four thousand times this bound. */
    double claim_sum = 0.0;
    for (size_t i = 0; i < nw; i++) claim_sum += w[i].claim;
    if (fabs(claim_sum - 1.0) > 1e-4) {
        free(w);
        return -1;
    }

    /* Rank by claim so both the threshold and the output cap fall on the
     * smallest claims, and the largest is easy to reach. */
    work_t **rank = (work_t **)calloc(nw, sizeof(*rank));
    if (!rank) { free(w); return -1; }
    for (size_t i = 0; i < nw; i++) rank[i] = &w[i];
    qsort(rank, nw, sizeof(*rank), cmp_claim_desc);

    /* Hold back the reserved slots so the merit pass cannot consume the whole
     * cap. Merit always keeps at least one slot: a block's largest legitimate
     * claimant is never displaced wholesale by the deferral queue, and at
     * max_outputs == 1 (a template whose byte budget left room for a single
     * payout) the reservation disappears entirely rather than handing that one
     * output to someone other than the top claim. */
    size_t reserved = carry_slots;
    if (reserved + 1 > max_outputs) reserved = max_outputs - 1;
    const size_t merit_cap = max_outputs - reserved;

    /* Emit the addresses whose cut clears the threshold, best claims first,
     * up to the output cap. A negative claim is an address that was paid early
     * and is repaying; it is never emitted. */
    size_t n_emit = 0, n_eligible = 0;
    for (size_t i = 0; i < nw; i++) {
        if (rank[i]->claim <= 0.0) break;   /* sorted: nothing better follows */
        double cut = (double)reward_after_fee * rank[i]->claim;
        if (cut < (double)min_payout_sats) break;
        /* Counted with the cap IGNORED: the caller cannot otherwise tell a cap
         * that cost someone a payout from one that capped below a set the
         * payout floor had already excluded. Both look like "fewer paid than
         * candidates" from outside, and only the first is worth reporting.
         *
         * Counted against the FULL cap, not merit_cap: it answers "how many
         * would an uncapped run have emitted", which the reservation does not
         * change. Reserving slots must not make the byte budget look like it
         * cost more payouts than it did. */
        n_eligible++;
        if (n_emit < merit_cap) {
            rank[i]->emit = 1;
            n_emit++;
        }
    }
    if (n_eligible_out) *n_eligible_out = n_eligible;

    /* Fill the reserved slots from the deferral queue: the addresses owed most
     * from earlier blocks, subject to exactly the same eligibility rules the
     * merit pass applies. Ordered by old_claim, so this cannot be starved by a
     * large miner's window fraction -- which is the failure it exists to fix.
     *
     * The floor test is `continue`, not `break`: old_claim order says nothing
     * about claim order, so a below-floor address here does not imply the rest
     * are below it too. Only the old_claim <= 0 test may break, because that
     * one IS the sort key. */
    if (reserved > 0 && n_emit < max_outputs) {
        work_t **byold = (work_t **)calloc(nw, sizeof(*byold));
        if (!byold) { free(rank); free(w); return -1; }
        for (size_t i = 0; i < nw; i++) byold[i] = &w[i];
        qsort(byold, nw, sizeof(*byold), cmp_old_claim_desc);
        for (size_t i = 0; i < nw && n_emit < max_outputs; i++) {
            work_t *e = byold[i];
            if (e->old_claim <= 0.0) break; /* sorted: nobody else is waiting */
            if (e->emit) continue;
            if (e->claim <= 0.0) continue;  /* unreachable while window >= 0 */
            double cut = (double)reward_after_fee * e->claim;
            if (cut < (double)min_payout_sats) continue;
            e->emit = 1;
            n_emit++;
        }
        free(byold);
    }

    /* Give back any reserved slot the deferral queue could not fill.
     *
     * ⛔ Without this, turning the reservation on could pay FEWER addresses
     * than turning it off -- exactly the regression a fairness change must not
     * ship. Whenever there is no queue to serve, the selection collapses back
     * to the pure largest-claim one. */
    if (n_emit < max_outputs) {
        for (size_t i = 0; i < nw && n_emit < max_outputs; i++) {
            if (rank[i]->claim <= 0.0) break;
            double cut = (double)reward_after_fee * rank[i]->claim;
            if (cut < (double)min_payout_sats) break;
            if (rank[i]->emit) continue;
            rank[i]->emit = 1;
            n_emit++;
        }
    }
    /* A block must pay someone: if the threshold excluded everybody, pay the
     * single largest positive claim regardless. */
    if (n_emit == 0) {
        if (rank[0]->claim <= 0.0) { free(rank); free(w); return -1; }
        rank[0]->emit = 1;
        n_emit = 1;
    }

    /* Renormalise over the emitted set, so the coinbase pays out the reward
     * exactly. Whoever is deferred this block keeps their claim; whoever is
     * paid absorbs the deferred share as an advance and goes claim-negative.
     *
     * ⚠️ The threshold above was applied to the PRE-renormalisation cut, and the
     * two are not the same number. The scale is reward/emit_claim, and
     * emit_claim exceeds 1.0 whenever a negative claim sits OUTSIDE the emitted
     * set — which is the state after any block that advanced someone, i.e. the
     * normal one. Every emitted address is then paid strictly less than the cut
     * that admitted it.
     *
     * That gap can carry an admitted address below the dust limit, and
     * coinbase_build_from_template_multi refuses the WHOLE build if any output
     * is under it. Since one coinbase is shared by every connection in this
     * mode, that is not a lost payout — it is every miner getting "coinbase
     * render failed" and the pool serving no work for that template.
     *
     * So re-check against what will actually be paid and drop the smallest
     * offender until the set is stable. Dropping only ever RAISES what the
     * remaining addresses are paid — emit_claim falls, so the scale rises — so
     * this terminates and can never re-break an address it has already cleared.
     * The dropped address is deferred, not robbed: its claim rolls forward in
     * the ledger exactly as a below-threshold one does. */
    double emit_claim = 0.0;
    for (;;) {
        emit_claim = 0.0;
        for (size_t i = 0; i < nw; i++) if (w[i].emit) emit_claim += w[i].claim;
        if (!(emit_claim > 0.0)) { free(rank); free(w); return -1; }
        if (n_emit <= 1) break;          /* a block must pay someone */

        work_t *worst = NULL;
        for (size_t i = 0; i < nw; i++) {
            if (!w[i].emit) continue;
            double d = (double)reward_after_fee * (w[i].claim / emit_claim);
            if (d >= (double)min_payout_sats) continue;
            if (!worst || w[i].claim < worst->claim) worst = &w[i];
        }
        if (!worst) break;
        worst->emit = 0;
        n_emit--;
    }

    int64_t distributed = 0;
    for (size_t i = 0; i < nw; i++) {
        if (!w[i].emit) continue;
        double d = (double)reward_after_fee * (w[i].claim / emit_claim);
        if (d < 0.0) d = 0.0;
        if (d > (double)reward_after_fee) d = (double)reward_after_fee;
        w[i].paid = (int64_t)d;             /* floor */
        distributed += w[i].paid;
    }
    /* The floor() remainder goes to the largest emitted claim, deterministically.
     * Sum of outputs must equal the reward exactly — under and the difference is
     * never minted, over and the block is invalid. */
    work_t *largest = NULL;
    for (size_t i = 0; i < nw; i++) {
        if (w[i].emit && (!largest || w[i].claim > largest->claim)) largest = &w[i];
    }
    if (!largest) { free(rank); free(w); return -1; }
    largest->paid += reward_after_fee - distributed;
    if (largest->paid < 0) { free(rank); free(w); return -1; }

    /* Write the payout list, largest claim first. */
    size_t np = 0;
    for (size_t i = 0; i < nw && np < max_outputs; i++) {
        work_t *e = rank[i];
        if (!e->emit || e->paid <= 0) continue;
        snprintf(payouts[np].address, sizeof payouts[np].address, "%s", e->address);
        payouts[np].sats = e->paid;
        np++;
    }
    *n_payouts_out = np;

    /* New ledger: what each address was owed this block, minus what it actually
     * received. Computed against the sats really emitted, not the ideal split,
     * so rounding is absorbed here rather than accumulating silently.
     *
     * The largest emitted claim takes the residual, which keeps the ledger
     * summing to exactly zero in floating point instead of drifting. */
    const double eps = claim_epsilon(reward_after_fee);
    size_t nl = 0;
    double sum_others = 0.0;
    for (size_t i = 0; i < nw; i++) {
        if (&w[i] == largest) continue;
        w[i].claim -= (double)w[i].paid / (double)reward_after_fee;
        sum_others += w[i].claim;
    }
    largest->claim = -sum_others;

    for (size_t i = 0; i < nw; i++) {
        if (fabs(w[i].claim) < eps) continue;   /* worth less than one satoshi */
        if (nl >= ledger_cap) { free(rank); free(w); return -1; }
        snprintf(ledger[nl].address, sizeof ledger[nl].address, "%s", w[i].address);
        ledger[nl].claim_fraction = w[i].claim;
        nl++;
    }
    *n_ledger_out = nl;

    free(rank);
    free(w);
    return 0;
}
