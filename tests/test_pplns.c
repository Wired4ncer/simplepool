#include "pplns.h"
#include "coinbase.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t sum_payouts(const pplns_payout_t *p, size_t n) {
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i].sats;
    return s;
}

static int64_t sum_carry(const pplns_carry_t *c, size_t n) {
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += c[i].pending_sats;
    return s;
}

static void test_simple_split(void) {
    pplns_addr_t addrs[] = {
        { "addrA", 50.0 },
        { "addrB", 50.0 },
    };
    pplns_carry_t carry[4] = {0};
    pplns_payout_t payouts[4] = {0};
    size_t n_payouts = 0, n_carry = 0;

    int rc = pplns_compute_payouts(
        1000000LL, 100.0, addrs, 2,
        carry, 4, 0, &n_carry,
        COINBASE_DUST_SATS, 12,
        payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts == 2);
    assert(sum_payouts(payouts, n_payouts) == 1000000LL);
    assert(n_carry == 0);
    printf("ok: simple 50/50 split\n");
}

static void test_remainder_to_largest(void) {
    /* 1/3 and 2/3 split on 1,000,001 sats. The 2/3 shareholder gets the 1-sat
     * remainder so the total is exact. */
    pplns_addr_t addrs[] = {
        { "addrA", 33.33333333 },
        { "addrB", 66.66666667 },
    };
    /* Force order: addrB is largest. */
    pplns_carry_t carry[4] = {0};
    pplns_payout_t payouts[4] = {0};
    size_t n_payouts = 0, n_carry = 0;

    int rc = pplns_compute_payouts(
        1000001LL, 100.0, addrs, 2,
        carry, 4, 0, &n_carry,
        COINBASE_DUST_SATS, 12,
        payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts == 2);
    assert(sum_payouts(payouts, n_payouts) == 1000001LL);
    /* Largest shareholder is sorted first after internal sort. */
    assert(strcmp(payouts[0].address, "addrB") == 0);
    printf("ok: remainder to largest shareholder\n");
}

static void test_carry_forward(void) {
    /* Two miners, tiny shares, min_payout keeps them below threshold. */
    pplns_addr_t addrs[] = {
        { "addrA", 1.0 },
        { "addrB", 1.0 },
    };
    pplns_carry_t carry[4] = {0};
    pplns_payout_t payouts[4] = {0};
    size_t n_payouts = 0, n_carry = 0;

    int rc = pplns_compute_payouts(
        1000000LL, 2.0, addrs, 2,
        carry, 4, 0, &n_carry,
        600000LL, 12,
        payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts == 0);
    assert(n_carry == 2);
    assert(sum_carry(carry, n_carry) == 1000000LL);
    printf("ok: sub-threshold balances carried forward\n");
}

static void test_carry_paid_out_once_threshold_crossed(void) {
    /* addrA already has a carry balance, current share pushes it over. */
    pplns_addr_t addrs[] = {
        { "addrA", 1.0 },
    };
    pplns_carry_t carry[4] = {
        { "addrA", 400000LL },
    };
    pplns_payout_t payouts[4] = {0};
    size_t n_payouts = 0, n_carry = 1;

    int rc = pplns_compute_payouts(
        1000000LL, 1.0, addrs, 1,
        carry, 4, n_carry, &n_carry,
        600000LL, 12,
        payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts == 1);
    assert(payouts[0].sats == 1000000LL + 400000LL);
    assert(n_carry == 0 || carry[0].pending_sats == 0);
    printf("ok: old carry pushes balance over threshold\n");
}

static void test_max_outputs_cap(void) {
    /* Four shareholders, but cap at 2 outputs. Smallest two become carry. */
    pplns_addr_t addrs[] = {
        { "addrA", 40.0 },
        { "addrB", 30.0 },
        { "addrC", 20.0 },
        { "addrD", 10.0 },
    };
    pplns_carry_t carry[8] = {0};
    pplns_payout_t payouts[8] = {0};
    size_t n_payouts = 0, n_carry = 0;

    int rc = pplns_compute_payouts(
        1000000LL, 100.0, addrs, 4,
        carry, 8, 0, &n_carry,
        COINBASE_DUST_SATS, 2,
        payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts == 2);
    /* addrA and addrB are paid; addrC and addrD are carried. */
    assert(sum_payouts(payouts, n_payouts) + sum_carry(carry, n_carry) == 1000000LL);
    printf("ok: max outputs cap merges smallest payouts\n");
}

static void test_accounting_invariant_random(void) {
    /* Property test: for a random-ish share set, payouts + net_new_carry == reward. */
    pplns_addr_t addrs[8];
    double diff = 0.0;
    for (size_t i = 0; i < 8; i++) {
        snprintf(addrs[i].address, sizeof addrs[i].address, "addr%zu", i);
        addrs[i].total_difficulty = (double)(i + 1) * 10.0;
        diff += addrs[i].total_difficulty;
    }
    pplns_carry_t carry[16] = {0};
    pplns_payout_t payouts[16] = {0};
    size_t n_payouts = 0, n_carry = 0;

    int rc = pplns_compute_payouts(
        1234567LL, diff, addrs, 8,
        carry, 16, 0, &n_carry,
        COINBASE_DUST_SATS, 12,
        payouts, &n_payouts);
    assert(rc == 0);
    assert(sum_payouts(payouts, n_payouts) + sum_carry(carry, n_carry) == 1234567LL);
    printf("ok: accounting invariant (random set)\n");
}

int main(void) {
    test_simple_split();
    test_remainder_to_largest();
    test_carry_forward();
    test_carry_paid_out_once_threshold_crossed();
    test_max_outputs_cap();
    test_accounting_invariant_random();
    printf("test_pplns: all tests passed\n");
    return 0;
}
