#define _POSIX_C_SOURCE 200809L
#include "pplns.h"
#include "coinbase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal working state for one address. */
typedef struct {
    char    address[128];
    double  weight;
    int64_t base_sats;       /* floor(reward * weight), before carry */
    int64_t old_carry;
    int64_t balance;         /* base_sats + old_carry */
    int     emit;            /* 1 if emitted as payout */
} work_addr_t;

static int cmp_addr_desc(const void *a, const void *b) {
    const pplns_addr_t *x = (const pplns_addr_t *)a;
    const pplns_addr_t *y = (const pplns_addr_t *)b;
    if (x->total_difficulty > y->total_difficulty) return -1;
    if (x->total_difficulty < y->total_difficulty) return 1;
    return 0;
}

static int cmp_payout_asc(const void *a, const void *b) {
    const work_addr_t *x = (const work_addr_t *)a;
    const work_addr_t *y = (const work_addr_t *)b;
    if (x->balance < y->balance) return -1;
    if (x->balance > y->balance) return 1;
    return 0;
}

static work_addr_t *find_work_addr(work_addr_t *wa, size_t n, const char *addr) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(wa[i].address, addr) == 0) return &wa[i];
    }
    return NULL;
}

static pplns_carry_t *find_carry(pplns_carry_t *carry, size_t n, const char *addr) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(carry[i].address, addr) == 0) return &carry[i];
    }
    return NULL;
}

int pplns_compute_payouts(int64_t reward_after_fee,
                          double window_difficulty,
                          const pplns_addr_t *addrs, size_t n_addrs,
                          pplns_carry_t *carry, size_t carry_cap,
                          size_t n_carry_in, size_t *n_carry_out,
                          int64_t min_payout_sats, size_t max_outputs,
                          pplns_payout_t *payouts, size_t *n_payouts_out) {
    if (!addrs || n_addrs == 0 || !payouts || !n_payouts_out ||
        !carry || !n_carry_out || window_difficulty <= 0.0 ||
        reward_after_fee < 0 || min_payout_sats < COINBASE_DUST_SATS ||
        max_outputs == 0 || carry_cap < n_addrs + n_carry_in) {
        return -1;
    }

    /* Make a mutable copy and ensure descending order. */
    pplns_addr_t *sorted = (pplns_addr_t *)calloc(n_addrs, sizeof(*sorted));
    if (!sorted) return -1;
    memcpy(sorted, addrs, n_addrs * sizeof(*sorted));
    qsort(sorted, n_addrs, sizeof(*sorted), cmp_addr_desc);

    work_addr_t *wa = (work_addr_t *)calloc(n_addrs, sizeof(*wa));
    if (!wa) { free(sorted); return -1; }

    /* Distribute the reward proportionally, truncating to whole sats. */
    int64_t distributed = 0;
    for (size_t i = 0; i < n_addrs; i++) {
        snprintf(wa[i].address, sizeof wa[i].address, "%s", sorted[i].address);
        wa[i].weight = sorted[i].total_difficulty / window_difficulty;
        double d = (double)reward_after_fee * wa[i].weight;
        if (d < 0.0) d = 0.0;
        if (d > (double)INT64_MAX) d = (double)INT64_MAX;
        wa[i].base_sats = (int64_t)d;
        wa[i].old_carry = 0;
        wa[i].emit = 0;
        distributed += wa[i].base_sats;
    }

    /* Remainder from floor() goes to the largest shareholder deterministically,
     * keeping the total base allocation exactly equal to reward_after_fee. */
    int64_t remainder = reward_after_fee - distributed;
    if (remainder != 0 && n_addrs > 0) {
        /* Guard against negative remainder from rounding edge cases. */
        if (wa[0].base_sats + remainder < 0) remainder = -wa[0].base_sats;
        wa[0].base_sats += remainder;
    }

    /* Merge existing carry balances into the working state. */
    for (size_t i = 0; i < n_carry_in; i++) {
        work_addr_t *w = find_work_addr(wa, n_addrs, carry[i].address);
        if (w) {
            w->old_carry = carry[i].pending_sats;
        }
    }

    for (size_t i = 0; i < n_addrs; i++) {
        wa[i].balance = wa[i].base_sats + wa[i].old_carry;
        if (wa[i].balance < 0) wa[i].balance = 0;
        if (wa[i].balance >= min_payout_sats) {
            wa[i].emit = 1;
        }
    }

    /* If too many outputs, convert the smallest payouts to carry until under
     * the cap. This is the output-weight safety valve. */
    size_t emit_count = 0;
    for (size_t i = 0; i < n_addrs; i++) if (wa[i].emit) emit_count++;
    if (emit_count > max_outputs) {
        /* Sort only the emitting addresses by balance ascending and demote the
         * smallest until the count fits. */
        work_addr_t **emitting = (work_addr_t **)calloc(emit_count, sizeof(*emitting));
        if (!emitting) { free(wa); free(sorted); return -1; }
        size_t e = 0;
        for (size_t i = 0; i < n_addrs; i++) {
            if (wa[i].emit) emitting[e++] = &wa[i];
        }
        qsort(emitting, emit_count, sizeof(*emitting),
              (int (*)(const void *, const void *))cmp_payout_asc);
        size_t demote = emit_count - max_outputs;
        for (size_t i = 0; i < demote; i++) {
            emitting[i]->emit = 0;
        }
        free(emitting);
    }

    /* Write outputs. */
    size_t np = 0;
    for (size_t i = 0; i < n_addrs; i++) {
        if (wa[i].emit) {
            if (np >= max_outputs) break; /* should not happen */
            snprintf(payouts[np].address, sizeof payouts[np].address, "%s", wa[i].address);
            payouts[np].sats = wa[i].balance;
            np++;
        }
    }
    *n_payouts_out = np;

    /* Write updated carry balances. Every address in the window ends up either
     * paid out (carry drops to 0) or carried forward at its full balance. */
    size_t nc = 0;
    for (size_t i = 0; i < n_addrs; i++) {
        if (wa[i].emit) {
            /* Payout consumed the old carry too. */
            if (wa[i].old_carry > 0) {
                pplns_carry_t *c = find_carry(carry, n_carry_in, wa[i].address);
                if (c) c->pending_sats = 0;
            }
        } else {
            int64_t new_carry = wa[i].balance;
            pplns_carry_t *c = find_carry(carry, n_carry_in, wa[i].address);
            if (c) {
                c->pending_sats = new_carry;
            } else {
                if (nc + n_carry_in >= n_addrs + n_carry_in) {
                    /* Caller did not provide enough capacity. */
                    free(wa); free(sorted); return -1;
                }
                if (n_carry_in + nc >= carry_cap) {
                    free(wa); free(sorted); return -1;
                }
                snprintf(carry[n_carry_in + nc].address,
                         sizeof carry[n_carry_in + nc].address, "%s", wa[i].address);
                carry[n_carry_in + nc].pending_sats = new_carry;
                nc++;
            }
        }
    }
    *n_carry_out = n_carry_in + nc;

    free(wa);
    free(sorted);
    return 0;
}
