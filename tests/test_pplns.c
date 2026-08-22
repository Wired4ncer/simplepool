#include "pplns.h"
#include "coinbase.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REWARD    100000000LL   /* 1 BTC after fee */
#define THRESHOLD 1000000LL     /* the plan's prop_min_payout_sats default */

static int64_t sum_payouts(const pplns_payout_t *p, size_t n) {
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i].sats;
    return s;
}

static double sum_claims(const pplns_claim_t *c, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += c[i].claim_fraction;
    return s;
}

static int64_t payout_for(const pplns_payout_t *p, size_t n, const char *addr) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(p[i].address, addr) == 0) return p[i].sats;
    }
    return 0;
}

static double claim_for(const pplns_claim_t *c, size_t n, const char *addr) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(c[i].address, addr) == 0) return c[i].claim_fraction;
    }
    return 0.0;
}

/* The property everything else rests on: a coinbase pays the reward exactly.
 * Not "close to", not "plus carry" — exactly, on every block, in every shape. */
static void assert_conserves(const pplns_payout_t *p, size_t n, int64_t reward) {
    int64_t total = sum_payouts(p, n);
    if (total != reward) {
        fprintf(stderr, "CONSERVATION VIOLATED: paid %lld, reward %lld (%+lld)\n",
                (long long)total, (long long)reward, (long long)(total - reward));
        abort();
    }
}

/* Everyone paid in full means an EMPTY ledger — not a pile of sub-satoshi
 * residue. Regression: a fixed 1e-9 epsilon is smaller than one satoshi of a
 * ~3.2 ECX block, so floor() residue survived and every paid address was
 * reported as a deferred claim. */
static void test_paid_in_full_leaves_no_ledger(void) {
    /* Deliberately indivisible: 3 addresses, a reward that does not divide. */
    pplns_addr_t addrs[] = { { "a", 33.0 }, { "b", 33.0 }, { "c", 34.0 } };
    pplns_claim_t ledger[8] = {0};
    pplns_payout_t payouts[8] = {0};
    size_t np = 0, nl = 0;
    assert(pplns_compute_payouts(REWARD + 7, addrs, 3, ledger, 8, 0, &nl,
                                 THRESHOLD, 12, payouts, &np) == 0);
    assert(np == 3);
    assert_conserves(payouts, np, REWARD + 7);
    if (nl != 0) {
        fprintf(stderr, "ledger should be empty, has %zu:\n", nl);
        for (size_t i = 0; i < nl; i++)
            fprintf(stderr, "  %s %+.12f (%.3f sat)\n", ledger[i].address,
                    ledger[i].claim_fraction,
                    ledger[i].claim_fraction * (double)(REWARD + 7));
        abort();
    }
    printf("ok: everyone paid in full leaves an empty ledger\n");
}

static void test_simple_split(void) {
    pplns_addr_t addrs[] = { { "addrA", 50.0 }, { "addrB", 50.0 } };
    pplns_claim_t ledger[4] = {0};
    pplns_payout_t payouts[4] = {0};
    size_t n_payouts = 0, n_ledger = 0;

    int rc = pplns_compute_payouts(REWARD, addrs, 2,
                                   ledger, 4, 0, &n_ledger,
                                   THRESHOLD, 12, payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts == 2);
    assert_conserves(payouts, n_payouts, REWARD);
    assert(payout_for(payouts, n_payouts, "addrA") == REWARD / 2);
    assert(payout_for(payouts, n_payouts, "addrB") == REWARD / 2);
    /* Both paid in full, so nobody is owed anything. */
    assert(fabs(sum_claims(ledger, n_ledger)) < 1e-9);
    printf("ok: simple 50/50 split\n");
}

static void test_remainder_to_largest(void) {
    /* Three equal shares of an amount not divisible by three. */
    pplns_addr_t addrs[] = { { "a", 1.0 }, { "b", 1.0 }, { "c", 1.0 } };
    pplns_claim_t ledger[8] = {0};
    pplns_payout_t payouts[8] = {0};
    size_t n_payouts = 0, n_ledger = 0;

    int rc = pplns_compute_payouts(100000001LL, addrs, 3,
                                   ledger, 8, 0, &n_ledger,
                                   THRESHOLD, 12, payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts == 3);
    assert_conserves(payouts, n_payouts, 100000001LL);
    printf("ok: remainder distributed, total exact\n");
}

/* THE REGRESSION. A miner below the threshold is skipped — and the block still
 * pays the reward exactly, rather than underpaying and leaving sats unminted.
 * The old sats-carry model paid 99,900,000 of 100,000,000 here. */
static void test_deferred_miner_does_not_shrink_the_coinbase(void) {
    pplns_addr_t addrs[] = { { "whale", 999.0 }, { "bitaxe", 1.0 } };
    pplns_claim_t ledger[8] = {0};
    pplns_payout_t payouts[8] = {0};
    size_t n_payouts = 0, n_ledger = 0;

    int rc = pplns_compute_payouts(REWARD, addrs, 2,
                                   ledger, 8, 0, &n_ledger,
                                   THRESHOLD, 12, payouts, &n_payouts);
    assert(rc == 0);
    /* The BitAxe's 0.1% cut is 100,000 sats, under the 1,000,000 threshold. */
    assert(n_payouts == 1);
    assert(payout_for(payouts, n_payouts, "bitaxe") == 0);
    assert_conserves(payouts, n_payouts, REWARD);
    assert(payout_for(payouts, n_payouts, "whale") == REWARD);

    /* The BitAxe is owed its 0.1%; the whale was advanced exactly that much. */
    assert(fabs(claim_for(ledger, n_ledger, "bitaxe") - 0.001) < 1e-9);
    assert(fabs(claim_for(ledger, n_ledger, "whale") + 0.001) < 1e-9);
    assert(fabs(sum_claims(ledger, n_ledger)) < 1e-9);
    printf("ok: deferring a miner still pays the reward exactly\n");
}

/* THE OTHER HALF OF THE REGRESSION. When the deferred claim is released, the
 * coinbase must still pay exactly the reward — the old model overpaid by the
 * carried amount here, which is an invalid block. */
static void test_released_claim_does_not_overpay(void) {
    pplns_claim_t ledger[8] = {0};
    pplns_payout_t payouts[8] = {0};
    size_t n_payouts = 0, n_ledger = 0;

    /* Block 1: BitAxe deferred. */
    pplns_addr_t b1[] = { { "whale", 999.0 }, { "bitaxe", 1.0 } };
    assert(pplns_compute_payouts(REWARD, b1, 2, ledger, 8, 0, &n_ledger,
                                 THRESHOLD, 12, payouts, &n_payouts) == 0);
    assert_conserves(payouts, n_payouts, REWARD);

    /* Block 2: the BitAxe has done real work and now clears the threshold. Its
     * deferred claim rides on top. */
    pplns_addr_t b2[] = { { "whale", 900.0 }, { "bitaxe", 100.0 } };
    size_t n_ledger2 = 0;
    assert(pplns_compute_payouts(REWARD, b2, 2, ledger, 8, n_ledger,
                                 &n_ledger2, THRESHOLD, 12,
                                 payouts, &n_payouts) == 0);
    assert(n_payouts == 2);
    assert_conserves(payouts, n_payouts, REWARD);

    /* It is paid its 10% window share PLUS the 0.1% it was owed, and the whale
     * gives back exactly what it was advanced. */
    int64_t bitaxe = payout_for(payouts, n_payouts, "bitaxe");
    assert(bitaxe == 10100000LL);
    assert(payout_for(payouts, n_payouts, "whale") == REWARD - bitaxe);
    /* Debt settled: the ledger is empty again. */
    assert(fabs(sum_claims(ledger, n_ledger2)) < 1e-9);
    assert(fabs(claim_for(ledger, n_ledger2, "bitaxe")) < 1e-9);
    printf("ok: releasing a deferred claim does not overpay\n");
}

/* A miner who disconnects for good is still owed, and collects from a later
 * block even with no shares in that window. */
static void test_departed_miner_is_still_paid(void) {
    pplns_claim_t ledger[8] = {0};
    pplns_payout_t payouts[8] = {0};
    size_t n_payouts = 0, n_ledger = 0;

    pplns_addr_t b1[] = { { "whale", 99.0 }, { "gone", 1.0 } };
    assert(pplns_compute_payouts(REWARD, b1, 2, ledger, 8, 0, &n_ledger,
                                 5000000LL, 12, payouts, &n_payouts) == 0);
    assert(payout_for(payouts, n_payouts, "gone") == 0);
    assert(claim_for(ledger, n_ledger, "gone") > 0.0);

    /* "gone" has no shares at all now. Its claim must still be honoured, and
     * once it clears the threshold it gets an output. */
    pplns_addr_t b2[] = { { "whale", 100.0 } };
    size_t n2 = 0;
    assert(pplns_compute_payouts(REWARD, b2, 1, ledger, 8, n_ledger, &n2,
                                 500000LL, 12, payouts, &n_payouts) == 0);
    assert_conserves(payouts, n_payouts, REWARD);
    assert(payout_for(payouts, n_payouts, "gone") == 1000000LL);
    printf("ok: a departed miner still collects what it is owed\n");
}

static void test_max_outputs_cap(void) {
    pplns_addr_t addrs[20];
    memset(addrs, 0, sizeof addrs);
    for (int i = 0; i < 20; i++) {
        snprintf(addrs[i].address, sizeof addrs[i].address, "addr%02d", i);
        addrs[i].total_difficulty = 100.0 - i; /* descending, all above dust */
    }
    pplns_claim_t ledger[64] = {0};
    pplns_payout_t payouts[64] = {0};
    size_t n_payouts = 0, n_ledger = 0;

    int rc = pplns_compute_payouts(REWARD, addrs, 20,
                                   ledger, 64, 0, &n_ledger,
                                   COINBASE_DUST_SATS, 12,
                                   payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts <= 12);
    assert_conserves(payouts, n_payouts, REWARD);
    /* The eight who were cut are owed; the twelve paid are in debt. */
    assert(fabs(sum_claims(ledger, n_ledger)) < 1e-9);
    printf("ok: output cap holds and the block still pays out exactly\n");
}

/* Hammer it: 200 blocks, shifting miner sets, thresholds that bite. Every block
 * must pay its reward exactly and the ledger must stay zero-sum throughout. */
static void test_conserves_over_many_blocks(void) {
    pplns_claim_t ledger[256] = {0};
    pplns_payout_t payouts[64] = {0};
    size_t n_ledger = 0;
    unsigned seed = 20260819u;

    for (int block = 0; block < 200; block++) {
        pplns_addr_t addrs[24];
        memset(addrs, 0, sizeof addrs);
        size_t n = 3 + (rand_r(&seed) % 20);
        for (size_t i = 0; i < n; i++) {
            snprintf(addrs[i].address, sizeof addrs[i].address,
                     "m%02d", (int)(rand_r(&seed) % 30));
            addrs[i].total_difficulty = 1.0 + (double)(rand_r(&seed) % 10000);
        }
        int64_t reward = 50000000LL + (rand_r(&seed) % 100000000);
        size_t n_payouts = 0, n_out = 0;
        int rc = pplns_compute_payouts(reward, addrs, n,
                                       ledger, 256, n_ledger, &n_out,
                                       THRESHOLD, 12, payouts, &n_payouts);
        assert(rc == 0);
        assert(n_payouts >= 1 && n_payouts <= 12);
        assert_conserves(payouts, n_payouts, reward);
        /* Zero-sum, to within the satoshi the ledger cannot express. A claim
         * worth under one satoshi is pruned rather than carried — it can never
         * be paid — so each participant can shed up to a satoshi of residue.
         * The tolerance is therefore denominated in satoshis and scales with
         * the working set. What must stay EXACT is the payout total, asserted
         * separately by assert_conserves(). */
        double drift_sats = fabs(sum_claims(ledger, n_out)) * (double)reward;
        if (drift_sats > (double)(n + n_out) + 1.0) {
            fprintf(stderr, "LEDGER DRIFT at block %d: %.3f sat over %zu entries "
                    "(%zu addresses)\n", block, drift_sats, n_out, n);
            abort();
        }
        n_ledger = n_out;
    }
    printf("ok: 200 blocks conserve exactly, ledger stays zero-sum\n");
}

/* No emitted payout may be below the configured floor -- and therefore none can
 * be dust, since the floor is itself floored at the dust limit.
 *
 * The threshold used to be applied to the PRE-renormalisation cut, which is a
 * different and larger number: payouts are renormalised over the emitted set,
 * and that scale is reward/emit_claim with emit_claim above 1.0 whenever a
 * negative claim sits outside the emitted set. An address could clear the
 * threshold on its cut and be paid a fraction of it. With the floor near dust,
 * that is an output coinbase_build_from_template_multi() refuses -- and since
 * one coinbase serves every connection in this mode, the whole template goes
 * down, not one payout.
 *
 * The fixture asserts its own precondition: "small" must clear the threshold on
 * its cut, or this test would pass without ever reaching the branch. */
static void test_every_payout_clears_the_floor(void) {
    const int64_t reward = 1000000000LL;      /* 10 ECX-ish, in sats */
    const int64_t floor_sats = COINBASE_DUST_SATS;   /* the dangerous setting */

    /* A ledger holding a real debt is what pushes emit_claim above 1.0, and it
     * is the ordinary state after any block that advanced someone. */
    pplns_claim_t ledger[8] = { { "big", +0.5 }, { "debtor", -0.5 } };
    size_t n_in = 2;
    pplns_addr_t addrs[] = {
        { "big",   500000.0 },
        { "mid",   499999.4 },
        { "small",      0.6 },   /* 6e-7 of the window: a 600-sat cut */
    };
    pplns_payout_t payouts[16] = {0};
    size_t np = 0, nl = 0;

    assert(pplns_compute_payouts(reward, addrs, 3, ledger, 8, n_in, &nl,
                                 floor_sats, 12, payouts, &np) == 0);
    assert_conserves(payouts, np, reward);

    /* PRECONDITION: on the pre-renormalisation cut, "small" clears the floor --
     * so the old admission rule would have emitted it. 6e-7 * 1e9 = 600 > 546. */
    double small_cut = (double)reward * (0.6 / 1000000.0);
    if (small_cut < (double)floor_sats) {
        fprintf(stderr, "fixture is wrong: small's cut is %.1f, under the %lld "
                        "floor, so the renormalisation gap is never tested\n",
                small_cut, (long long)floor_sats);
        abort();
    }

    for (size_t i = 0; i < np; i++) {
        if (payouts[i].sats >= floor_sats) continue;
        fprintf(stderr, "payout %zu (%s) is %lld sats, under the %lld floor -- "
                        "coinbase_build_from_template_multi refuses the whole "
                        "build and the pool serves no work for this template\n",
                i, payouts[i].address, (long long)payouts[i].sats,
                (long long)floor_sats);
        abort();
    }
    /* Deferred, not robbed: the claim rolls forward intact. */
    assert(payout_for(payouts, np, "small") == 0);
    assert(fabs(claim_for(ledger, nl, "small") - 6e-7) < 1e-12);
    printf("ok: no emitted payout falls below the floor after renormalisation\n");
}

/* Every address's new claim must be what it was owed minus what it was paid --
 * including the largest one, which is the only entry the code does not compute
 * that way directly.
 *
 * The old test asserted only that the ledger summed to zero, which the code
 * enforced by construction: it set the largest claim to whatever made the sum
 * zero. That is the correct residual when the working set's claims sum to one
 * reward, and silently wrong otherwise -- it could even invert the sign, so an
 * address that over-received was recorded as owed and paid again next block. */
static void test_residuals_are_per_address(void) {
    /* 60/39/1 of the window; the 1% miner's cut is under the 2% floor. */
    pplns_addr_t addrs[] = { { "a", 60.0 }, { "b", 39.0 }, { "c", 1.0 } };
    pplns_claim_t ledger[8] = {0};
    pplns_payout_t payouts[8] = {0};
    size_t np = 0, nl = 0;

    assert(pplns_compute_payouts(REWARD, addrs, 3, ledger, 8, 0, &nl,
                                 2000000LL, 12, payouts, &np) == 0);
    assert(np == 2);
    assert_conserves(payouts, np, REWARD);

    /* c was deferred, so a and b are renormalised over 0.99 between them. */
    for (size_t i = 0; i < 3; i++) {
        const char *who = addrs[i].address;
        double owed = addrs[i].total_difficulty / 100.0;      /* window fraction */
        double got  = (double)payout_for(payouts, np, who) / (double)REWARD;
        double want = owed - got;
        double have = claim_for(ledger, nl, who);
        if (fabs(want - have) > 1e-9) {
            fprintf(stderr, "%s: ledger says %+.9f, but it was owed %.9f and "
                            "paid %.9f -- residual should be %+.9f\n",
                    who, have, owed, got, want);
            abort();
        }
    }
    printf("ok: each residual is what that address was owed minus what it got\n");
}

/* A stored ledger that is not zero-sum is unrepresentable, not merely unusual:
 * every block pays exactly one reward, so claims summing to anything else
 * describe a debt no coinbase can settle. Refuse, and let the caller fall back
 * to paying the finder directly -- rather than forcing the books to balance by
 * moving the whole imbalance onto whoever happens to be largest. */
static void test_refuses_a_ledger_that_is_not_zero_sum(void) {
    pplns_addr_t addrs[] = { { "a", 100.0 } };
    pplns_payout_t payouts[8] = {0};
    size_t np = 0, nl = 0;

    /* b is owed 0.2 of a reward and nobody carries the matching debt. */
    pplns_claim_t broken[8] = { { "b", +0.2 } };
    assert(pplns_compute_payouts(REWARD, addrs, 1, broken, 8, 1, &nl,
                                 THRESHOLD, 12, payouts, &np) < 0);

    /* The mirror: a debt with no matching claim. */
    pplns_claim_t broken2[8] = { { "b", -0.2 } };
    assert(pplns_compute_payouts(REWARD, addrs, 1, broken2, 8, 1, &nl,
                                 THRESHOLD, 12, payouts, &np) < 0);

    /* Balanced, and the same block is fine. */
    pplns_claim_t ok[8] = { { "b", +0.2 }, { "c", -0.2 } };
    assert(pplns_compute_payouts(REWARD, addrs, 1, ok, 8, 2, &nl,
                                 THRESHOLD, 12, payouts, &np) == 0);
    printf("ok: a ledger that does not sum to zero is refused, not absorbed\n");
}

/* Bad input is refused rather than silently producing a wrong split. */
static void test_rejects_bad_input(void) {
    pplns_addr_t addrs[] = { { "a", 1.0 } };
    pplns_claim_t ledger[4] = {0};
    pplns_payout_t payouts[4] = {0};
    size_t np = 0, nl = 0;

    /* No difficulty in the window at all: there is no denominator to split on.
     * This used to be expressed as window_difficulty = 0; it is now a property
     * of the rows themselves, which is the point of deriving it from them. */
    pplns_addr_t zero_diff[] = { { "a", 0.0 } };
    assert(pplns_compute_payouts(REWARD, zero_diff, 1, ledger, 4, 0, &nl,
                                 THRESHOLD, 12, payouts, &np) < 0);
    pplns_addr_t neg_diff[] = { { "a", -1.0 } };
    assert(pplns_compute_payouts(REWARD, neg_diff, 1, ledger, 4, 0, &nl,
                                 THRESHOLD, 12, payouts, &np) < 0);
    assert(pplns_compute_payouts(0, addrs, 1, ledger, 4, 0, &nl,
                                 THRESHOLD, 12, payouts, &np) < 0);
    assert(pplns_compute_payouts(REWARD, addrs, 1, ledger, 4, 0, &nl,
                                 THRESHOLD, 0, payouts, &np) < 0);
    /* Below the dust limit is not a valid threshold. */
    assert(pplns_compute_payouts(REWARD, addrs, 1, ledger, 4, 0, &nl,
                                 100, 12, payouts, &np) < 0);
    /* Ledger capacity too small for the working set. */
    assert(pplns_compute_payouts(REWARD, addrs, 1, ledger, 0, 0, &nl,
                                 THRESHOLD, 12, payouts, &np) < 0);
    printf("ok: invalid input refused\n");
}

int main(void) {
    test_simple_split();
    test_paid_in_full_leaves_no_ledger();
    test_remainder_to_largest();
    test_deferred_miner_does_not_shrink_the_coinbase();
    test_released_claim_does_not_overpay();
    test_departed_miner_is_still_paid();
    test_max_outputs_cap();
    test_conserves_over_many_blocks();
    test_every_payout_clears_the_floor();
    test_residuals_are_per_address();
    test_refuses_a_ledger_that_is_not_zero_sum();
    test_rejects_bad_input();
    printf("test_pplns: all tests passed\n");
    return 0;
}
