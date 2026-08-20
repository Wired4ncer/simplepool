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

int pplns_compute_payouts(int64_t reward_after_fee,
                          double window_difficulty,
                          const pplns_addr_t *addrs, size_t n_addrs,
                          pplns_claim_t *ledger, size_t ledger_cap,
                          size_t n_ledger_in, size_t *n_ledger_out,
                          int64_t min_payout_sats, size_t max_outputs,
                          pplns_payout_t *payouts, size_t *n_payouts_out) {
    if (!addrs || n_addrs == 0 || !payouts || !n_payouts_out ||
        !ledger || !n_ledger_out || window_difficulty <= 0.0 ||
        reward_after_fee <= 0 || min_payout_sats < COINBASE_DUST_SATS ||
        max_outputs == 0 || ledger_cap < n_addrs + n_ledger_in) {
        return -1;
    }

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
        e->window_fraction += addrs[i].total_difficulty / window_difficulty;
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

    /* Rank by claim so both the threshold and the output cap fall on the
     * smallest claims, and the largest is easy to reach. */
    work_t **rank = (work_t **)calloc(nw, sizeof(*rank));
    if (!rank) { free(w); return -1; }
    for (size_t i = 0; i < nw; i++) rank[i] = &w[i];
    qsort(rank, nw, sizeof(*rank), cmp_claim_desc);

    /* Emit the addresses whose cut clears the threshold, best claims first,
     * up to the output cap. A negative claim is an address that was paid early
     * and is repaying; it is never emitted. */
    size_t n_emit = 0;
    for (size_t i = 0; i < nw && n_emit < max_outputs; i++) {
        if (rank[i]->claim <= 0.0) break;   /* sorted: nothing better follows */
        double cut = (double)reward_after_fee * rank[i]->claim;
        if (cut < (double)min_payout_sats) break;
        rank[i]->emit = 1;
        n_emit++;
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
     * paid absorbs the deferred share as an advance and goes claim-negative. */
    double emit_claim = 0.0;
    for (size_t i = 0; i < nw; i++) if (w[i].emit) emit_claim += w[i].claim;
    if (!(emit_claim > 0.0)) { free(rank); free(w); return -1; }

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
