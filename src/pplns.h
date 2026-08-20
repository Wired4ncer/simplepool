#ifndef SIMPLEPOOL_PPLNS_H
#define SIMPLEPOOL_PPLNS_H

#include <stddef.h>
#include <stdint.h>

/* One address in the PPLNS window, already aggregated by payout address and
 * sorted by total_difficulty descending (largest first). */
typedef struct {
    char    address[128];
    double  total_difficulty;
} pplns_addr_t;

/* Carry-forward balance for an address. The pool holds these pending_sats as a
 * liability until a future block makes the address eligible for a coinbase
 * output. */
typedef struct {
    char    address[128];
    int64_t pending_sats;
} pplns_carry_t;

/* One coinbase payout output. */
typedef struct {
    char    address[128];
    int64_t sats;
} pplns_payout_t;

/* Compute the PPLNS payout list and updated carry-forward balances.
 *
 * reward_after_fee: sats available for miners (block reward minus operator fee).
 * window_difficulty: sum of difficulties in the window; must be > 0.
 * addrs: window shares aggregated by address, sorted by total_difficulty
 *        descending. The largest shareholder receives the rounding remainder.
 * n_addrs: number of aggregated addresses.
 * carry: in/out array of existing carry balances. On output it contains every
 *        address that did not receive a payout, including new addresses. The
 *        caller must provide enough capacity (n_addrs + n_carry_in entries).
 * n_carry_in: number of valid entries in carry on input.
 * n_carry_out: number of valid entries in carry on output.
 * min_payout_sats: smallest output value emitted; smaller balances are carried.
 * max_outputs: maximum number of payout outputs (not counting operator fee).
 * payouts: output array, caller must allocate at least max_outputs entries.
 * n_payouts_out: number of payouts written.
 *
 * Accounting invariant: sum(payouts) + (sum(new_carry) - sum(old_carry)) ==
 * reward_after_fee. The coinbase therefore pays exactly sum(payouts) to miners
 * and the operator fee is separate.
 *
 * Returns 0 on success, -1 on invalid input. */
int pplns_compute_payouts(int64_t reward_after_fee,
                          double window_difficulty,
                          const pplns_addr_t *addrs, size_t n_addrs,
                          pplns_carry_t *carry, size_t carry_cap,
                          size_t n_carry_in, size_t *n_carry_out,
                          int64_t min_payout_sats, size_t max_outputs,
                          pplns_payout_t *payouts, size_t *n_payouts_out);

#endif /* SIMPLEPOOL_PPLNS_H */
