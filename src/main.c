#define _POSIX_C_SOURCE 200809L
#include "bitcoind.h"
#include "broadcast.h"
#include "coinbase.h"
#include "config.h"
#include "log.h"
#include "pplns.h"
#include "share.h"
#include "store.h"
#include "stratum.h"
#include "version.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_shutdown = 0;

static void on_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* ---------- helpers ---------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* hex (big-endian display order) -> bytes (display order). */
static int hex_to_bytes_display(const char *hex, uint8_t *out, size_t expected) {
    size_t n = strlen(hex);
    if (n != expected * 2) return -1;
    for (size_t i = 0; i < expected; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Reverse 32 bytes in-place. */
static void rev32(uint8_t b[32]) {
    for (int i = 0; i < 16; i++) {
        uint8_t t = b[i];
        b[i] = b[31 - i];
        b[31 - i] = t;
    }
}

/* Compute merkle branches for index 0 over [coinbase_placeholder, ...txids_le].
 * branches_out must hold up to tx_count entries. Returns number of branches. */
static size_t compute_merkle_branches_for_idx0(const uint8_t (*txids_le)[32],
                                                size_t tx_count,
                                                uint8_t (*branches_out)[32]) {
    /* Branches: at each level, the sibling of node 0. The leaf-level sibling
     * is txids_le[0] (the first non-coinbase tx) — i.e. branches don't depend
     * on the coinbase content. We can compute by carrying a placeholder leaf
     * (zeros) and recording the sibling at each level. */
    size_t n = tx_count + 1;
    if (n == 1) return 0;

    /* Working buffer with placeholder at idx 0, then txids. */
    uint8_t (*level)[32] = (uint8_t (*)[32])calloc(n, 32);
    if (!level) return 0;
    /* level[0] = zeros (placeholder). */
    for (size_t i = 0; i < tx_count; i++) {
        memcpy(level[i + 1], txids_le[i], 32);
    }

    size_t branch_count = 0;
    while (n > 1) {
        memcpy(branches_out[branch_count++], level[1], 32);

        /* Build next level. */
        size_t new_n = (n + 1) / 2;
        for (size_t i = 0; i < new_n; i++) {
            uint8_t pair[64];
            memcpy(pair, level[2 * i], 32);
            if (2 * i + 1 < n) {
                memcpy(pair + 32, level[2 * i + 1], 32);
            } else {
                memcpy(pair + 32, level[2 * i], 32);
            }
            uint8_t h[32];
            dsha256(pair, 64, h);
            memcpy(level[i], h, 32);
        }
        n = new_n;
    }
    free(level);
    return branch_count;
}

/* ---------- shared server state ---------- */

/* One PPLNS settle plan: the coinbase payouts that went into a job, plus the
 * deferred-claim ledger that becomes authoritative if (and only if) that job's
 * block is accepted. Held until the job can no longer be solved. */
/* One plan per job that can still be solved: everything stratum retains
 * (STRATUM_RECENT_JOBS) plus the current job, doubled for headroom.
 *
 * ⚠️ It was a bare 8 against a retention ring of 8 + the current job — so the
 * OLDEST solvable job never had a plan, and a block found on it fell back to
 * paying its finder directly instead of the PPLNS window. Safe, in that no
 * invalid block or custody is possible either way, but wrong: the window's
 * miners lose a block they earned, and nothing logs it as a defect. */
#define PROP_PLAN_RING     ((STRATUM_RECENT_JOBS + 1) * 2)
#define PROP_PLAN_MAX_PAY  64

typedef struct {
    char            job_id[32];
    uint32_t        height;
    int64_t         reward_after_fee;
    pplns_payout_t  payouts[PROP_PLAN_MAX_PAY];
    size_t          n_payouts;
    /* Both halves of the ledger change this plan represents: the snapshot it
     * was computed FROM, and the state it computed. Settlement applies the
     * difference to whatever the table holds at the time, so a plan built
     * before another block settled no longer erases it. Both owned. */
    pplns_claim_t  *ledger_in;
    size_t          n_ledger_in;
    pplns_claim_t  *ledger;
    size_t          n_ledger;
} prop_plan_t;

typedef struct {
    bitcoind_client_t *btc;
    /* Dedicated client for the tip watcher's (possibly long-polled) GBT
     * requests. A BIP22 long poll parks the request server-side for tens of
     * seconds while holding the client's connection lock — on the shared
     * client that would stall submitblock, so the watcher gets its own. */
    bitcoind_client_t *btc_lp;
    store_t           *store;
    broadcast_t       *bcast;
    stratum_server_t  *srv;
    proxy_config_t    *cfg;

    pthread_mutex_t lock;
    int             last_height;
    char            last_prev_hash[65];
    uint64_t        last_built_ms;

    /* pool_mode=proportional: the last few PPLNS settle plans, keyed on the
     * job_id they were computed for. A block is settled against the plan of the
     * job the miner actually solved, not the newest one — several templates can
     * exist for the same height (mempool churn), each with its own window, and
     * crediting the wrong one would mis-pay carry-forward. Guarded by `lock`.
     * Sized off STRATUM_RECENT_JOBS so it always covers every job stratum will
     * still accept a submit for — see PROP_PLAN_RING. */
    prop_plan_t     prop_plans[PROP_PLAN_RING];
    size_t          prop_plan_next;

    /* Live PPS rate, refreshed whenever a new template arrives. Read on the
     * share path, so it is an atomic double rather than taking `lock` —
     * a share crediting against the previous template's rate for a few
     * microseconds during a swap is immaterial, whereas contending the job
     * lock per share would not be. Zero means "no accrual" (solo, or a
     * template we could not derive a rate from). */
    _Atomic double  pps_rate;

    /* Observed share-difficulty throughput, in difficulty units per second,
     * and the window it is accumulated over. This is the pool's own hashrate
     * expressed in the same units the PPS rate is paid in, which is what the
     * issuance ceiling needs: accrual per second is rate * this. */
    _Atomic double  diff_accum;        /* difficulty seen this window */
    _Atomic uint64_t diff_window_ms;   /* when the window opened */
    _Atomic double  diff_per_sec;      /* last completed measurement, 0 = none */

    /* Set when accrual is refused because network difficulty is below
     * cfg->pps_min_network_difficulty. Read by the stratum server, which
     * turns miners away rather than letting them work uncredited. */
    _Atomic int     pps_gated;

    /* Whether the backend serves getblockhash: 0 unknown, 1 yes, -1 no.
     * Latched on the first "Method not found", because a backend that does
     * not implement the method never starts to — the CUSF enforcer serves
     * exactly getblocktemplate and submitblock. -1 is not a failure state,
     * it selects the observed-tip path. */
    _Atomic int     gbh_state;
} server_ctx_t;

/* How long to accumulate share difficulty before turning it into a rate.
 * Long enough to be stable, short enough that a pool starting up is measured
 * within a minute. */
#define HASHRATE_WINDOW_MS 60000

/* A block this deep stops being re-checked. */
#define BLOCK_FINAL_DEPTH      100
/* Per tip change. Bounds how long reconciliation can hold the shared client's
 * connection lock, which submitblock also needs. */
#define RECONCILE_MAX_PER_TICK  16

/* The rate this proxy will credit at: the operator's override verbatim if
 * set, otherwise fair value derived from the template. See the
 * pps_sats_per_diff commentary in config.h for why an override bypasses
 * fee_bps rather than stacking with it. */
static double effective_pps_rate(const proxy_config_t *cfg,
                                 int64_t value_sats, double net_diff) {
    if (cfg->pps_sats_per_diff > 0.0) return cfg->pps_sats_per_diff;
    return pps_rate_from_template(value_sats, net_diff, cfg->fee_bps);
}

/* ---------- proportional / PPLNS ---------- */

static void prop_plan_clear(prop_plan_t *p) {
    if (!p) return;
    free(p->ledger_in);
    free(p->ledger);
    memset(p, 0, sizeof *p);
}

/* Network difficulty implied by a template, by the same route refresh_pps_rate
 * takes: GBT's target when it supplies one, else derived from nbits. */
static double template_net_diff(const bitcoind_template_t *t) {
    uint8_t target_be[32] = {0};
    if (t->target_hex[0] != '\0' && strlen(t->target_hex) == 64) {
        if (hex_to_bytes_display(t->target_hex, target_be, 32) < 0)
            nbits_to_target(t->bits, target_be);
    } else {
        nbits_to_target(t->bits, target_be);
    }
    return target_to_diff(target_be);
}

/* Gather this template's weight facts and ask coinbase.c how many payout
 * outputs fit. A static cap cannot be right — headroom moves block to block.
 * Measured on the live alpha node, a 1,710-tx template left 2,685 WU (21
 * outputs) while the configured ceiling was 12; on a fuller block 12 would
 * have been too many. After the fork, ECX blocks may be nearly empty and the
 * ceiling is all that limits us. So prop_max_outputs is an upper bound, not
 * the operating value. */
static size_t prop_max_outputs_for_template(const bitcoind_template_t *t,
                                            const proxy_config_t *cfg,
                                            int fee_output,
                                            const size_t *size_hist,
                                            size_t hist_len,
                                            size_t fee_txout_bytes,
                                            int64_t *out_headroom_wu,
                                            size_t *out_predicted_bytes) {
    size_t ceiling = (size_t)cfg->prop_max_outputs;
    if (ceiling > PROP_PLAN_MAX_PAY) ceiling = PROP_PLAN_MAX_PAY;
    if (out_predicted_bytes) *out_predicted_bytes = 0;
    if (!t->coinbasetxn_hex) {
        if (out_headroom_wu) *out_headroom_wu = -1;
        return ceiling;
    }
    int64_t tx_weight = 0;
    for (size_t i = 0; i < t->tx_count; i++) tx_weight += t->txs[i].weight;

    /* What the builder splices into the scriptSig: both extranonces, plus the
     * tag and its length byte. Same constants the probe build passes to
     * coinbase_build_from_template_multi — a literal here went stale when
     * extranonce2 grew 4 → 8 and overstated the headroom by 16 WU. */
    size_t ss_growth = STRATUM_EXTRANONCE1_SIZE + STRATUM_EXTRANONCE2_SIZE;
    size_t taglen = strlen(cfg->coinbase_tag);
    if (taglen) ss_growth += (taglen > 75 ? 75 : taglen) + 1;

    size_t n = coinbase_max_payout_outputs(t->weight_limit, tx_weight,
                                           strlen(t->coinbasetxn_hex) / 2,
                                           ss_growth, fee_output, ceiling,
                                           out_headroom_wu);

    /* Second, independent limit: keep the SERIALIZED COINBASE inside the
     * operator's byte budget. The weight limit above is consensus and is very
     * loose here (measured floor ~7,000 WU spare); this one is marketplace
     * compatibility, and it is the tight one. See prop_max_coinbase_bytes. */
    if (cfg->prop_max_coinbase_bytes > 0) {
        /* Our first payout replaces the template's own spendable output, so
         * its size is a credit. An unreadable template leaves this 0, which
         * credits nothing and costs at most one slot -- the safe direction. */
        size_t slot_bytes = 0;
        (void)coinbase_template_payout_slot_bytes(t->coinbasetxn_hex,
                                                  &slot_bytes, NULL, 0);
        size_t by_bytes = coinbase_max_payout_outputs_bytes(
                              strlen(t->coinbasetxn_hex) / 2, slot_bytes,
                              ss_growth, size_hist, hist_len,
                              fee_output ? fee_txout_bytes : 0,
                              (size_t)cfg->prop_max_coinbase_bytes, ceiling,
                              out_predicted_bytes);
        if (by_bytes < n) n = by_bytes;
    }
    return n;
}

/* The size of the operator's own fee output, or 0 when no fee is taken.
 *
 * It is not a payout and does not pass through pplns_compute_payouts, so it is
 * measured here by hand and charged as a fixed cost. An unset operator address
 * with a fee to pay is a misconfiguration; charge the largest output we emit
 * rather than nothing, which is the direction that stays inside the budget. */
static size_t prop_fee_txout_bytes(const proxy_config_t *cfg, int fee_output) {
    if (!fee_output) return 0;
    if (!cfg->operator_address[0]) return (size_t)(8 + 1 + 34);
    return coinbase_payout_txout_bytes(cfg->operator_address);
}

/* The sizes of every payout output this template could be asked to carry.
 *
 * ⛔ THE SET MUST BE COMPLETE. pplns_compute_payouts builds its working set
 * from exactly two places -- the window addresses and the stored claim ledger,
 * because a miner who has gone away is still owed and still gets paid -- and
 * emits a SUBSET of that union. Sizing over the same union is therefore an
 * upper bound on what is emitted. Sizing over `addrs` alone would NOT be: the
 * first carried-forward claimant holding a bc1p address while the window is
 * all-P2WPKH would push the coinbase past the budget unmeasured.
 *
 * ⚠️ If a future change can introduce a payout address from anywhere but these
 * two arrays, it MUST be added here in the same commit.
 *
 * ⚠️ Deduplication moved from being irrelevant to being load-bearing when this
 * stopped being a max and became a sum: max is idempotent, a sum is not, and
 * an address sitting in BOTH the window and the ledger -- the ordinary case
 * for a miner carrying a claim too small to have been paid -- would otherwise
 * be charged twice. pplns_candidate_txout_hist dedupes by the same rule the
 * payout set itself is built with. */
static size_t prop_candidate_txout_hist(const pplns_addr_t *addrs,
                                        size_t n_addrs,
                                        const pplns_claim_t *ledger_in,
                                        size_t n_ledger_in,
                                        size_t *hist, size_t hist_len) {
    return pplns_candidate_txout_hist(addrs, n_addrs, ledger_in, n_ledger_in,
                                      hist, hist_len);
}

/* Compute the PPLNS payout set for a template.
 *
 * Returns 0 with *plan filled when a usable window exists, 1 when there is no
 * proportional split to make and the caller should fall back to per-miner
 * (solo) rendering, and -1 on a hard error.
 *
 * The 1 case is deliberately not an error: an empty shares table, a template
 * without a server coinbase, or a window whose payouts do not conserve the
 * reward all mean "cannot split this block safely". Paying the finder directly
 * is always valid and never custodial, so the pool keeps mining. */
static int prop_build_plan(server_ctx_t *s, const bitcoind_template_t *t,
                          const char *job_id, prop_plan_t *plan) {
    if (!s || !s->cfg || !s->store || !t || !plan) return -1;
    memset(plan, 0, sizeof *plan);

    if (!t->coinbasetxn_hex || !t->coinbasetxn_hex[0]) {
        LOG_WARN("proportional: template has no coinbasetxn (needs the enforcer "
                 "GBT); falling back to per-miner coinbase for this template");
        return 1;
    }

    /* The reward the builder will check against — read from the serialized
     * coinbase, not from the template's coinbasevalue field. */
    int64_t reward = 0;
    char cerr[256] = {0};
    if (coinbase_template_reward(t->coinbasetxn_hex, &reward, cerr, sizeof cerr) < 0) {
        LOG_WARN("proportional: cannot read template reward (%s); falling back", cerr);
        return 1;
    }

    /* Operator fee, byte-identical to the builder's arithmetic. */
    int64_t fee_sats = 0;
    if (s->cfg->operator_address[0] && s->cfg->fee_bps > 0 && reward > 0) {
        fee_sats = (reward * (int64_t)s->cfg->fee_bps) / 10000;
        if (fee_sats < COINBASE_DUST_SATS) fee_sats = 0;
    }
    int64_t reward_after_fee = reward - fee_sats;
    if (reward_after_fee <= 0) {
        LOG_WARN("proportional: nothing to split (reward=%lld fee=%lld); falling back",
                 (long long)reward, (long long)fee_sats);
        return 1;
    }

    /* Window, in difficulty units so it survives a retarget unchanged. */
    double net_diff = template_net_diff(t);
    if (!isfinite(net_diff) || net_diff <= 0.0) {
        LOG_WARN("proportional: template difficulty is %.4f; falling back", net_diff);
        return 1;
    }
    double want_diff = s->cfg->prop_window_k * net_diff;
    uint64_t now = now_ms();
    uint64_t start_ms = 0;
    double   actual_diff = 0.0;
    int win_truncated = 0;
    if (store_prop_compute_window(s->store, want_diff, now,
                                  s->cfg->prop_window_min_sec,
                                  &start_ms, &actual_diff, &win_truncated) < 0 ||
        actual_diff <= 0.0) {
        LOG_INFO("proportional: no shares in the window yet (wanted %.2f "
                 "difficulty or %d seconds); paying the finder directly",
                 want_diff, s->cfg->prop_window_min_sec);
        return 1;
    }

    if (win_truncated) {
        /* Not fatal — the payouts are still exact over the window we DID read —
         * but the window is shorter than configured, so say so rather than let
         * the pool quietly pay on a window nobody chose. */
        LOG_WARN("proportional: PPLNS window TRUNCATED — read the row cap before "
                 "reaching %.2f difficulty, got %.2f over %llu s. Payouts are "
                 "correct for that shorter window, but it is not the configured "
                 "one. Raise prop_window_min_sec or lower prop_window_k.",
                 want_diff, actual_diff,
                 (unsigned long long)((now - start_ms) / 1000));
    }

    pplns_addr_t  *addrs = NULL; size_t n_addrs = 0;
    if (store_prop_window_addrs(s->store, start_ms, now, &addrs, &n_addrs) < 0 ||
        n_addrs == 0) {
        free(addrs);
        LOG_INFO("proportional: window has no payable addresses; "
                 "paying the finder directly");
        return 1;
    }

    pplns_claim_t *ledger_in = NULL; size_t n_ledger_in = 0;
    if (store_prop_get_ledger(s->store, &ledger_in, &n_ledger_in) < 0) {
        free(addrs);
        LOG_WARN("proportional: reading the claim ledger failed; falling back");
        return 1;
    }

    /* pplns_compute_payouts refuses a ledger that is not zero-sum, because
     * forcing it to balance is what silently inverts the largest claimant's
     * sign. Refusal falls back to paying the finder — correct and safe, but if
     * the STORED ledger is the broken one it will happen on every template from
     * now on, and "payout computation produced nothing" is not a diagnosis.
     * Say what is wrong and what fixes it, once per template, before the fact.
     *
     * The remedy is deliberately not automatic: the rows record who the pool
     * believes it owes a turn, and deleting them is a payout decision. */
    {
        double stored_sum = 0.0;
        for (size_t i = 0; i < n_ledger_in; i++)
            stored_sum += ledger_in[i].claim_fraction;
        if (n_ledger_in > 0 && fabs(stored_sum) > 1e-4) {
            LOG_ERROR("proportional: the stored claim ledger sums to %+.6f of a "
                      "block reward across %zu rows, not zero — every block pays "
                      "exactly one reward, so this describes claims no coinbase "
                      "can settle. Payouts will fall back to paying the finder "
                      "directly until it is fixed. Inspect prop_ledger and, once "
                      "you have decided what it should say, clear it: "
                      "DELETE FROM prop_ledger;",
                      stored_sum, n_ledger_in);
        }
    }

    /* pplns_compute_payouts rewrites the ledger in place, so the array needs
     * room for every window address on top of what is already there. */
    size_t ledger_cap = n_addrs + n_ledger_in + 1;
    pplns_claim_t *ledger = (pplns_claim_t *)calloc(ledger_cap, sizeof(*ledger));
    if (!ledger) { free(addrs); free(ledger_in); return -1; }
    if (n_ledger_in) memcpy(ledger, ledger_in, n_ledger_in * sizeof(*ledger));
    /* ledger_in is NOT freed here — pplns_compute_payouts rewrites `ledger` in
     * place, so this is the only remaining record of what the plan started
     * from, and settlement needs it to compute its delta. */

    int64_t headroom_wu = -1;
    /* Measured over the addresses this block could actually pay -- the window
     * AND the carried-forward claim ledger, since both can be emitted. */
    size_t cand_hist[PPLNS_TXOUT_HIST_LEN];
    size_t candidates = prop_candidate_txout_hist(addrs, n_addrs,
                                                  ledger_in, n_ledger_in,
                                                  cand_hist, PPLNS_TXOUT_HIST_LEN);
    size_t fee_bytes = prop_fee_txout_bytes(s->cfg, fee_sats > 0);
    size_t predicted_bytes = 0;
    size_t max_out = prop_max_outputs_for_template(t, s->cfg, fee_sats > 0,
                                                   candidates ? cand_hist : NULL,
                                                   PPLNS_TXOUT_HIST_LEN,
                                                   fee_bytes, &headroom_wu,
                                                   &predicted_bytes);

    /* The split is denominated in the difficulty these rows actually carry;
     * `actual_diff` is the walk's own measure of the same window and is used
     * for reporting only. They are two queries against a table the writer
     * thread is still committing into, so a small disagreement is expected and
     * harmless. A LARGE one is a defect in the window walk — the fractions the
     * payout would have used were measured against the wrong total — so say so
     * rather than let it pass as a plausible number in a log line. */
    double addr_diff = 0.0;
    for (size_t i = 0; i < n_addrs; i++) addr_diff += addrs[i].total_difficulty;
    if (addr_diff > 0.0 && actual_diff > 0.0 &&
        fabs(addr_diff - actual_diff) / addr_diff > 0.01) {
        LOG_WARN("proportional: window walk and address aggregate disagree by "
                 "%.2f%% (walk %.2f, addresses %.2f over %zu rows) — paying on "
                 "the address total, which is the one the payouts are measured "
                 "against, but the walk should not be this far out",
                 100.0 * fabs(addr_diff - actual_diff) / addr_diff,
                 actual_diff, addr_diff, n_addrs);
    }

    size_t n_payouts = 0, n_ledger_out = 0, n_eligible = 0;
    int rc = pplns_compute_payouts(reward_after_fee,
                                   addrs, n_addrs,
                                   ledger, ledger_cap, n_ledger_in, &n_ledger_out,
                                   s->cfg->prop_min_payout_sats, max_out,
                                   plan->payouts, &n_payouts, &n_eligible);
    free(addrs);
    if (rc < 0 || n_payouts == 0) {
        free(ledger); free(ledger_in);
        LOG_WARN("proportional: payout computation produced nothing "
                 "(rc=%d, %zu addresses, window difficulty %.2f); falling back",
                 rc, n_addrs, actual_diff);
        return 1;
    }

    /* Report the byte budget only when it can actually COST someone a payout.
     *
     * Two conditions, and both are needed. The cap must be SATURATED — if fewer
     * addresses were paid than the cap allowed, something other than the cap
     * did the excluding — and more addresses must have cleared the payout floor
     * than were paid. Comparing payouts against the CANDIDATE count instead
     * reports a loss on nearly every block, because most candidates are below
     * the floor or repaying an advance and were never going to be paid.
     *
     * ⚠️ This is the same failure as the one measured in regtest earlier: the
     * first version of this line said "byte budget binds" on five consecutive
     * blocks that lost nothing, because the arithmetic was right and the claim
     * the line made about it was false. n_eligible is an upper bound on what an
     * uncapped run would have emitted, so the line says how many cleared the
     * floor -- it does not assert an exact number of payouts lost. */
    if (s->cfg->prop_max_coinbase_bytes > 0 &&
        n_payouts == max_out && n_eligible > n_payouts) {
        LOG_INFO("coinbase byte budget is costing payouts: cap %zu reached, "
                 "%zu of %zu distinct candidates cleared the payout floor — "
                 "sized on the %zu largest, fee output %zu B, budget %d B. "
                 "The rest carry forward.",
                 max_out, n_eligible, candidates, max_out, fee_bytes,
                 s->cfg->prop_max_coinbase_bytes);
    }

    /* Conservation guard. A coinbase must pay the reward exactly: short and the
     * difference is never minted, over and the block is invalid. The builder
     * refuses either way, which would take every job down rather than one, so
     * check it here and fall back instead.
     *
     * With the deferred-claim ledger this must never fire: the payout list is
     * renormalised over the emitted set precisely so it sums to the reward. It
     * stays as a belt-and-braces check because the cost of being wrong here is
     * an invalid block or unminted value, not a bad log line. */
    int64_t total = 0;
    for (size_t i = 0; i < n_payouts; i++) total += plan->payouts[i].sats;
    if (total != reward_after_fee) {
        LOG_ERROR("proportional: payout total %lld != reward-after-fee %lld "
                  "(%zu outputs, delta %+lld sats) — this is a bug in the payout "
                  "split, not a configuration problem. Falling back to per-miner "
                  "coinbase for this template.",
                  (long long)total, (long long)reward_after_fee,
                  n_payouts, (long long)(total - reward_after_fee));
        free(ledger); free(ledger_in);
        return 1;
    }

    /* Build the coinbase once, here, before this plan is ever attached to a
     * job. The render path has no fallback in this mode — one coinbase is
     * shared by every connection, so a builder refusal there is not a lost
     * payout, it is every miner getting "coinbase render failed" and the pool
     * serving no work until the next template. The fallback that DOES exist is
     * this one: return 1, hand out per-miner coinbases, keep mining.
     *
     * So the question "will the builder accept this payout set?" has to be
     * asked while the answer can still change something. The builder is the
     * authority on its own rules (dust floors, output count against the
     * scriptSig and weight budget, commitment preservation) and asking it
     * directly is cheaper than keeping a second copy of them in sync here. */
    {
        coinbase_payout_t cb[PROP_PLAN_MAX_PAY];
        for (size_t i = 0; i < n_payouts && i < PROP_PLAN_MAX_PAY; i++) {
            cb[i].address = plan->payouts[i].address;
            cb[i].sats    = plan->payouts[i].sats;
        }
        coinbase_parts_t probe = {0};
        char berr[256] = {0};
        if (n_payouts > PROP_PLAN_MAX_PAY ||
            coinbase_build_from_template_multi(t->coinbasetxn_hex, cb, n_payouts,
                                               s->cfg->operator_address,
                                               s->cfg->fee_bps,
                                               s->cfg->coinbase_tag,
                                               /*en1*/ STRATUM_EXTRANONCE1_SIZE,
                                               /*en2*/ STRATUM_EXTRANONCE2_SIZE,
                                               &probe, NULL, NULL, NULL,
                                               berr, sizeof berr) < 0) {
            LOG_WARN("proportional: the builder refuses this payout set (%s) — "
                     "falling back to per-miner coinbases for this template "
                     "rather than serving no work. %zu outputs over %.2f window "
                     "difficulty.", berr, n_payouts, actual_diff);
            free(probe.cb1); free(probe.cb2);
            free(ledger); free(ledger_in);
            return 1;
        }
        /* The exact serialized size of the coinbase this template would emit —
         * cb1 ‖ en1 ‖ en2 ‖ cb2, the bytes themselves rather than the model
         * that predicted them. The build already happens here on every
         * template, before a single share is issued against the job, and its
         * lengths were being freed unread.
         *
         * ⛔ ALARM, NEVER A GATE. A coinbase past the byte budget is a
         * perfectly valid block; refusing to mine it would convert a
         * marketplace-compatibility preference into a lost block, which is the
         * one expensive failure here. Say so loudly and mine it. */
        size_t actual_bytes = probe.cb1_len + STRATUM_EXTRANONCE1_SIZE +
                              STRATUM_EXTRANONCE2_SIZE + probe.cb2_len;
        if (predicted_bytes && actual_bytes > predicted_bytes) {
            /* 🔴 The estimate is not an upper bound. Under the old "every slot
             * costs the largest candidate" model this could not happen — the
             * slack made under-estimation structurally impossible — and
             * summing the k largest removes exactly that slack on purpose.
             * Suspect the candidate dedupe, the fee-output term, or an address
             * type the sizer does not recognise. */
            LOG_WARN("coinbase estimator UNDER-ESTIMATED: built %zu B against a "
                     "predicted ceiling of %zu B for %zu payouts. The sizing is "
                     "not an upper bound — mining it anyway.",
                     actual_bytes, predicted_bytes, n_payouts);
        }
        if (s->cfg->prop_max_coinbase_bytes > 0 &&
            actual_bytes > (size_t)s->cfg->prop_max_coinbase_bytes) {
            LOG_WARN("coinbase is %zu B, past the %d B budget — a marketplace "
                     "verificator may refuse orders against this pool. Mining "
                     "it: the budget is a commercial preference, the block is "
                     "money.", actual_bytes, s->cfg->prop_max_coinbase_bytes);
        }
        free(probe.cb1); free(probe.cb2);
    }

    snprintf(plan->job_id, sizeof plan->job_id, "%s", job_id ? job_id : "");
    plan->height           = (uint32_t)t->height;
    plan->reward_after_fee = reward_after_fee;
    plan->n_payouts        = n_payouts;
    plan->ledger_in        = ledger_in;
    plan->n_ledger_in      = n_ledger_in;
    plan->ledger           = ledger;
    plan->n_ledger         = n_ledger_out;

    LOG_INFO("proportional: %zu payout outputs over %.2f window difficulty "
             "(want %.1f x network %.2f = %.2f, floor %d s, window spans %llu s), "
             "reward-after-fee %lld sats, %zu deferred claims, "
             "cap %zu of %d (weight headroom %lld WU)",
             n_payouts, actual_diff, s->cfg->prop_window_k, net_diff, want_diff,
             s->cfg->prop_window_min_sec,
             (unsigned long long)((now - start_ms) / 1000),
             (long long)reward_after_fee, n_ledger_out,
             max_out, s->cfg->prop_max_outputs, (long long)headroom_wu);
    return 0;
}

/* Store a plan in the ring, replacing the oldest. Takes ownership of
 * plan->ledger. */
static void prop_plan_remember(server_ctx_t *s, const prop_plan_t *plan) {
    pthread_mutex_lock(&s->lock);
    prop_plan_t *slot = &s->prop_plans[s->prop_plan_next % PROP_PLAN_RING];
    prop_plan_clear(slot);
    *slot = *plan;
    s->prop_plan_next++;
    pthread_mutex_unlock(&s->lock);
}

/* Build a job from a freshly fetched template.
 *
 * In solo and pps-classic modes the coinbase is rendered per-connection inside
 * stratum.c, so only template-level data is passed. In pool_mode=proportional
 * the coinbase is shared by every connection, so the PPLNS payout set is
 * computed here and attached to the job. sctx may be NULL, which skips the
 * proportional path. */
static stratum_job_t *build_job_from_template(server_ctx_t *sctx,
                                              const proxy_config_t *cfg,
                                              const bitcoind_template_t *t,
                                              char *errbuf, size_t errlen) {
    /* Convert tx txids: hex (display BE) -> internal LE. */
    uint8_t (*txids_le)[32] = NULL;
    char **tx_hex_list = NULL;
    if (t->tx_count > 0) {
        txids_le = (uint8_t (*)[32])calloc(t->tx_count, 32);
        tx_hex_list = (char **)calloc(t->tx_count, sizeof(char *));
        if (!txids_le || !tx_hex_list) {
            snprintf(errbuf, errlen, "oom");
            free(txids_le); free(tx_hex_list);
            return NULL;
        }
        for (size_t i = 0; i < t->tx_count; i++) {
            uint8_t be[32];
            if (hex_to_bytes_display(t->txs[i].txid_hex, be, 32) < 0) {
                snprintf(errbuf, errlen, "bad txid hex at %zu", i);
                free(txids_le);
                for (size_t j = 0; j < i; j++) free(tx_hex_list[j]);
                free(tx_hex_list);
                return NULL;
            }
            memcpy(txids_le[i], be, 32);
            rev32(txids_le[i]);
            tx_hex_list[i] = strdup(t->txs[i].data_hex ? t->txs[i].data_hex : "");
        }
    }

    /* Branches. */
    uint8_t (*branches)[32] = NULL;
    size_t branch_count = 0;
    if (t->tx_count > 0) {
        branches = (uint8_t (*)[32])calloc(t->tx_count + 1, 32);
        if (!branches) {
            snprintf(errbuf, errlen, "oom branches");
            free(txids_le);
            for (size_t j = 0; j < t->tx_count; j++) free(tx_hex_list[j]);
            free(tx_hex_list);
            return NULL;
        }
        branch_count = compute_merkle_branches_for_idx0(
            (const uint8_t (*)[32])txids_le, t->tx_count, branches);
    }

    /* prev_hash: GBT gives BE display hex; header wants natural LE bytes. */
    uint8_t prev_le[32] = {0};
    if (hex_to_bytes_display(t->prev_hash_hex, prev_le, 32) < 0) {
        snprintf(errbuf, errlen, "bad prev hash hex");
        free(branches); free(txids_le);
        for (size_t j = 0; j < t->tx_count; j++) free(tx_hex_list[j]);
        free(tx_hex_list);
        return NULL;
    }
    rev32(prev_le);

    uint8_t target_be[32] = {0};
    /* If GBT supplies target hex, use it; else derive from nbits. */
    if (t->target_hex[0] != '\0' && strlen(t->target_hex) == 64) {
        if (hex_to_bytes_display(t->target_hex, target_be, 32) < 0) {
            nbits_to_target(t->bits, target_be);
        }
    } else {
        nbits_to_target(t->bits, target_be);
    }

    /* Millisecond clock plus a counter. The clock alone is not unique: two
     * templates built inside the same millisecond — a long-poll return landing
     * next to the periodic rebuild, or the initial job next to the watcher's
     * first — get the same id, and then find_job() hands a submit the wrong
     * job and the proportional settle path credits the wrong window. The
     * counter makes the id unique within a run regardless of clock resolution
     * or a clock that steps backwards. */
    static _Atomic unsigned long job_seq;
    char job_id[32];
    snprintf(job_id, sizeof job_id, "%llx-%lx",
             (unsigned long long)now_ms(),
             atomic_fetch_add(&job_seq, 1ul));

    /* A server-provided coinbase (BIP22 "coinbasetxn") is segwit-serialized
     * when its version (4 bytes = 8 hex chars) is followed by the segwit
     * marker 0x00 + flag 0x01. The CUSF enforcer uses this canonical form on
     * every network except signet (where no witness commitment is added). */
    int cb_has_witness = 0;
    if (t->coinbasetxn_hex && strlen(t->coinbasetxn_hex) >= 12) {
        const char *h = t->coinbasetxn_hex;
        cb_has_witness = (h[8] == '0' && h[9] == '0' && h[10] == '0' && h[11] == '1');
    }

    stratum_job_t *job = stratum_job_new(
        job_id, t->version, prev_le,
        t->coinbase_value_sats,
        t->default_witness_commitment,
        /*en1*/ STRATUM_EXTRANONCE1_SIZE,
        /*en2*/ STRATUM_EXTRANONCE2_SIZE,
        (const uint8_t (*)[32])branches, branch_count,
        t->bits, t->curtime, target_be,
        (uint32_t)t->height,
        (const char *const *)tx_hex_list, t->tx_count,
        t->coinbasetxn_hex, cb_has_witness);

    /* stratum_job_new copies; free our originals. */
    free(branches);
    free(txids_le);
    if (tx_hex_list) {
        for (size_t j = 0; j < t->tx_count; j++) free(tx_hex_list[j]);
        free(tx_hex_list);
    }
    if (!job) {
        snprintf(errbuf, errlen, "stratum_job_new failed");
        return NULL;
    }

    /* pool_mode=proportional: one shared coinbase paying the PPLNS window.
     * A failure here is never fatal — the job goes out without a payout set and
     * stratum.c renders per-miner coinbases instead, which is what solo does. */
    if (sctx && strcmp(cfg->pool_mode, "proportional") == 0) {
        prop_plan_t plan;
        int prc = prop_build_plan(sctx, t, job_id, &plan);
        if (prc == 0) {
            coinbase_payout_t cb[PROP_PLAN_MAX_PAY];
            for (size_t i = 0; i < plan.n_payouts; i++) {
                cb[i].address = plan.payouts[i].address;
                cb[i].sats    = plan.payouts[i].sats;
            }
            if (stratum_job_set_payouts(job, cb, plan.n_payouts) < 0) {
                LOG_WARN("proportional: attaching payouts to job %s failed; "
                         "this template pays the finder directly", job_id);
                prop_plan_clear(&plan);
            } else {
                prop_plan_remember(sctx, &plan);
            }
        } else if (prc < 0) {
            LOG_WARN("proportional: plan build errored for job %s; "
                     "this template pays the finder directly", job_id);
        }
    }
    return job;
}

/* Recompute the PPS rate from a new template and publish it, so the
 * dashboard reads what is actually being paid instead of keeping a second
 * copy of the config. Cheap and called once per template change.
 *
 * Warns when an override implies a materially different fee from fee_bps —
 * that mismatch is invisible otherwise, and a stale override is how the fee
 * silently drifts to zero (or negative) as difficulty moves. */
/* Close the hashrate window if it has run long enough, and return the best
 * available difficulty-per-second measurement (0 when there is none yet). */
static double observed_diff_per_sec(server_ctx_t *s) {
    uint64_t now = now_ms();
    uint64_t opened = atomic_load_explicit(&s->diff_window_ms, memory_order_relaxed);
    if (opened == 0) {
        atomic_store_explicit(&s->diff_window_ms, now, memory_order_relaxed);
        return 0.0;
    }
    if (now - opened >= HASHRATE_WINDOW_MS) {
        double accum = atomic_exchange_explicit(&s->diff_accum, 0.0,
                                                memory_order_relaxed);
        atomic_store_explicit(&s->diff_window_ms, now, memory_order_relaxed);
        double secs = (double)(now - opened) / 1000.0;
        if (secs > 0.0) {
            atomic_store_explicit(&s->diff_per_sec, accum / secs,
                                  memory_order_relaxed);
        }
    }
    return atomic_load_explicit(&s->diff_per_sec, memory_order_relaxed);
}

static void refresh_pps_rate(server_ctx_t *s, const bitcoind_template_t *t) {
    if (!s || !s->cfg || !t) return;

    uint8_t target_be[32] = {0};
    if (t->target_hex[0] != '\0' && strlen(t->target_hex) == 64) {
        if (hex_to_bytes_display(t->target_hex, target_be, 32) < 0)
            nbits_to_target(t->bits, target_be);
    } else {
        nbits_to_target(t->bits, target_be);
    }
    double net_diff = target_to_diff(target_be);
    int64_t value   = t->coinbase_value_sats;

    int overridden  = s->cfg->pps_sats_per_diff > 0.0;
    double rate     = effective_pps_rate(s->cfg, value, net_diff);
    double gross    = (value > 0 && isfinite(net_diff) && net_diff > 0.0)
                    ? (double)value / net_diff : 0.0;
    /* What the numbers actually imply, which under an override is whatever
     * the operator's arithmetic produced rather than fee_bps. */
    double eff_fee_bps = (gross > 0.0) ? (1.0 - rate / gross) * 10000.0 : 0.0;

    int accrues = strcmp(s->cfg->pool_mode, "pps-classic") == 0;

    /* Two guards, in order. Both only matter while accruing.
     *
     * The floor is the operator's, and it is the one that works from the
     * first share: below the configured difficulty the fair-value formula is
     * not fair, so nothing accrues at all. The ceiling is automatic and needs
     * no configuration, but it needs a hashrate measurement, so it cannot
     * cover the first minute after a restart. They cover each other. */
    double dps = observed_diff_per_sec(s);
    int gated = 0;
    if (accrues && s->cfg->pps_min_network_difficulty > 0.0 &&
        net_diff > 0.0 && net_diff < s->cfg->pps_min_network_difficulty) {
        gated = 1;
        rate = 0.0;
    }
    if (accrues && !gated) {
        double capped = pps_rate_apply_issuance_ceiling(
            rate, value, dps, s->cfg->block_interval_sec);
        if (capped < rate) {
            LOG_WARN("pps rate capped at %.6f sats/diff (fair value says "
                     "%.6f): at %.2f difficulty/s this pool would accrue "
                     "faster than the chain can issue %lld sats every %ds. "
                     "Network difficulty %.2f is below the %.2f this pool's "
                     "own hashrate requires — set "
                     "pps_min_network_difficulty and stop accruing until the "
                     "chain catches up.",
                     capped, rate, dps, (long long)value,
                     s->cfg->block_interval_sec, net_diff,
                     pps_min_safe_difficulty(dps, s->cfg->block_interval_sec));
        }
        rate = capped;
    }

    /* Report the transition, not every template — this path runs per poll. */
    int was_gated = atomic_exchange_explicit(&s->pps_gated, gated,
                                             memory_order_relaxed);
    if (accrues && gated && !was_gated) {
        LOG_WARN("PPS ACCRUAL SUSPENDED: network difficulty %.2f is below the "
                 "configured floor of %.2f. Shares are not being credited "
                 "because at this difficulty each one would be priced as "
                 "though it were worth a whole block. Accrual resumes on its "
                 "own once the chain retargets.",
                 net_diff, s->cfg->pps_min_network_difficulty);
    } else if (accrues && !gated && was_gated) {
        LOG_INFO("pps accrual resumed: network difficulty %.2f is at or above "
                 "the configured floor of %.2f", net_diff,
                 s->cfg->pps_min_network_difficulty);
    }

    /* No floor configured is a real risk, not a neutral default. Say so once
     * there is a measurement to say it with. */
    if (accrues && s->cfg->pps_min_network_difficulty <= 0.0 && dps > 0.0) {
        double need = pps_min_safe_difficulty(dps, s->cfg->block_interval_sec);
        if (need > 0.0 && net_diff > 0.0 && net_diff < need) {
            LOG_WARN("pps_min_network_difficulty is unset and network "
                     "difficulty %.2f is below the %.2f this pool's own "
                     "%.2f difficulty/s requires. Every share is being priced "
                     "as though the chain could absorb it; it cannot. Set "
                     "pps_min_network_difficulty=%.0f",
                     net_diff, need, dps, need);
        }
    }

    atomic_store_explicit(&s->pps_rate, accrues ? rate : 0.0,
                          memory_order_relaxed);

    if (accrues && overridden && gross > 0.0) {
        double drift = eff_fee_bps - (double)s->cfg->fee_bps;
        if (drift < -25.0 || drift > 25.0) {
            LOG_WARN("pps rate override %.4f implies a %.2f%% fee, but "
                     "fee_bps=%d says %.2f%% (network difficulty %.2f, "
                     "block value %lld sats). Fair value is %.4f sats/diff. "
                     "Omit pps_sats_per_diff to derive it automatically.",
                     s->cfg->pps_sats_per_diff, eff_fee_bps / 100.0,
                     s->cfg->fee_bps, s->cfg->fee_bps / 100.0,
                     net_diff, (long long)value,
                     gross * (1.0 - (double)s->cfg->fee_bps / 10000.0));
        }
        if (eff_fee_bps < 0.0) {
            LOG_WARN("pps rate override %.4f EXCEEDS fair value %.4f — the "
                     "pool is paying out more than each share earns.",
                     s->cfg->pps_sats_per_diff, gross);
        }
    }

    if (s->store) {
        uint64_t now_s = (uint64_t)time(NULL);
        store_record_pool_meta(s->store, s->cfg->pool_mode, s->cfg->fee_bps,
                               overridden ? "override" : "derived",
                               accrues ? rate : 0.0, gross,
                               accrues ? eff_fee_bps : 0.0,
                               net_diff, value, now_s);
        /* Append to the rate log so the rate a share was credited at stays
         * recoverable after pool_meta has been overwritten. Only meaningful
         * while accruing — in solo mode the effective rate is 0 and there is
         * nothing to audit. */
        if (accrues) {
            store_record_rate(s->store, overridden ? "override" : "derived",
                              rate, gross, s->cfg->fee_bps,
                              net_diff, value, now_s);
        }

        /* Template history. Recorded in every mode — what the pool is mining
         * is worth showing whether or not it accrues PPS credit. */
        int cb_spendable = 0, cb_op_returns = 0;
        if (t->coinbasetxn_hex) {
            /* Best-effort: a coinbase we cannot parse still gets a row, just
             * with zero counts, rather than losing the whole template. */
            if (coinbase_count_outputs(t->coinbasetxn_hex,
                                       &cb_spendable, &cb_op_returns) < 0) {
                cb_spendable = 0;
                cb_op_returns = 0;
            }
        }
        int64_t tx_fees = 0;
        for (size_t i = 0; i < t->tx_count; i++) {
            if (t->txs[i].fee > 0) tx_fees += t->txs[i].fee;
        }
        char bits_hex[16];
        snprintf(bits_hex, sizeof bits_hex, "%08x", t->bits);

        store_template_t st = {
            .ts_s                = now_s,
            .height              = t->height,
            .prev_hash           = t->prev_hash_hex,
            .bits                = bits_hex,
            .network_difficulty  = net_diff,
            .coinbase_value_sats = value,
            .tx_count            = (int)t->tx_count,
            .tx_fees_sats        = tx_fees,
            /* A server-provided coinbase is the signal: only that path
             * carries the BIP300/301 commitments. */
            .source              = t->coinbasetxn_hex ? "enforcer" : "bitcoind",
            .cb_spendable        = cb_spendable,
            .cb_op_returns       = cb_op_returns,
            .longpoll            = t->longpollid != NULL,
            .rate_sats_per_diff  = accrues ? rate : 0.0,
        };
        store_record_template(s->store, &st);
    }
}

/* ---------- observer hooks ---------- */

static void on_share_cb(void *ctx, const char *worker_name,
                        const char *payout_address, uint64_t ts_ms,
                        double difficulty, int is_block,
                        const char *block_hash_or_null,
                        int solo) {
    server_ctx_t *s = (server_ctx_t *)ctx;

    /* Fold this share into the hashrate window. Difficulty per second is what
     * the issuance ceiling is judged against — accrual per second is exactly
     * rate * this — so it is accumulated in the same units the rate is paid
     * in, before any decision about crediting. */
    if (s) {
        double prev = atomic_load_explicit(&s->diff_accum, memory_order_relaxed);
        atomic_store_explicit(&s->diff_accum, prev + difficulty,
                              memory_order_relaxed);
    }

    /* PPS accrual. Credit the worker proportional to share difficulty at the
     * rate derived from the current template (or the operator's override).
     * Truncates to whole sats; sub-sat dust accumulates per-share so over
     * many shares the rounding error is bounded by 1 sat per row.
     *
     * Only pool_mode=pps-classic accrues. Solo pays each miner directly from
     * their own coinbase, so there is nothing to credit and delta stays 0 —
     * which is also what gets stored on the share row.
     *
     * Computed before the share is recorded so the amount can be written
     * onto the share itself; an audit then reports what was paid rather than
     * recomputing it against a rate that may since have moved. */
    int64_t delta = 0;
    double  rate_used = 0.0;
    if (s && s->cfg && strcmp(s->cfg->pool_mode, "pps-classic") == 0) {
        double rate = atomic_load_explicit(&s->pps_rate, memory_order_relaxed);
        if (rate > 0.0) {
            double d = difficulty * rate;
            /* rate_used is stored only when it is the number that actually
             * produced delta. On the overflow guard below the two would not
             * reconcile, so it stays 0 and the audit reports the row as
             * unverifiable instead of as a mismatch. */
            if (d > 0.0 && d < (double)INT64_MAX) {
                delta = (int64_t)d;
                rate_used = rate;
            }
        }
    }

    if (s && s->store) {
        store_record_share_addr(s->store, worker_name, payout_address,
                                ts_ms, difficulty, is_block,
                                block_hash_or_null, delta, rate_used, solo);
    }
    if (s && s->bcast) {
        broadcast_share(s->bcast, worker_name, payout_address,
                        ts_ms, difficulty, is_block, block_hash_or_null);
    }
    if (delta > 0) {
        if (s->store) {
            store_record_credit(s->store, worker_name, payout_address,
                                ts_ms, delta);
        }
        if (s->bcast) {
            /* accrued_total is the running balance after this credit.
             * Since the writer thread is async we don't know it
             * exactly; pass 0 and let consumers query SQLite for the
             * authoritative number. */
            broadcast_credit(s->bcast, worker_name, ts_ms, delta, 0);
        }
    }
}

/* The stratum layer and the store each name their own "no age" sentinel and
 * neither includes the other's header. This is the seam where they meet, so
 * this is where a divergence has to fail — at compile time, not as ages
 * silently written NULL. */
_Static_assert(STRATUM_JOB_AGE_NONE == STORE_JOB_AGE_NONE,
               "stratum and store disagree about the no-age sentinel");

static void on_reject_cb(void *ctx, const char *worker_name,
                         const char *peer_ip, uint64_t ts_ms,
                         const char *reason, const char *reject_kind,
                         int64_t job_age_ms) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (s && s->store) {
        store_record_reject(s->store, worker_name, peer_ip, ts_ms, reason,
                            reject_kind, job_age_ms);
    }
    if (s && s->bcast) {
        /* ⛔ peer_ip deliberately NOT broadcast. The broadcast feed is what
         * the public dashboard consumes; the address of every miner that
         * mistypes a submit does not belong on it. It is recorded in the
         * local database for operator diagnosis and stops there. */
        broadcast_reject(s->bcast, worker_name, ts_ms, reason);
    }
}

/* Returns 0 when the node accepted the block, non-zero when it refused.
 * The caller records the candidate accordingly — a refusal that goes only to
 * the log is what let rejected candidates be counted as pool revenue. */
static int on_block_cb(void *ctx, const char *block_hex,
                       char *errbuf, size_t errlen) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (!s || !s->btc) {
        snprintf(errbuf, errlen, "no bitcoind client");
        return -1;
    }
    int rc = bitcoind_submit_block(s->btc, block_hex, errbuf, errlen);
    if (rc == 0) {
        LOG_INFO("submitted block to bitcoind successfully");
    } else {
        LOG_ERROR("submitblock failed: %s", errbuf);
    }
    return rc;
}

static void on_block_found_cb(void *ctx, const char *worker_name,
                              const char *finder_address,
                              uint64_t ts_ms, uint32_t height,
                              const char *job_id,
                              const char *block_hash,
                              int64_t reward_sats, int64_t fee_sats,
                              int accepted, const char *submit_error,
                              int solo) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    /* Accepted only makes it a candidate the chain has not rejected — it is
     * still 'pending' until something verifies the block is in the chain.
     * Nothing here may write 'confirmed'. */
    int status = accepted ? STORE_BLOCK_PENDING : STORE_BLOCK_REJECTED;
    if (s && s->store) {
        store_record_block(s->store, ts_ms, (int)height, block_hash,
                           worker_name, finder_address,
                           reward_sats, fee_sats, status,
                           accepted ? NULL : submit_error);
    }

    /* pool_mode=proportional: commit the carry-forward balances belonging to
     * the job that was actually solved. Nothing is committed for a job that had
     * no plan (a fallback template paid its finder directly), and nothing is
     * committed twice — the plan is consumed here.
     *
     * ⛔ GATED ON `accepted`. This callback now fires for every candidate,
     * including ones submitblock refused — settling a refused candidate would
     * consume its plan and pay out a turn for a block that does not exist.
     * Recording a candidate and paying for one are different acts.
     *
     * ⚠️ `accepted` still means the node took it, not that it survived. A
     * block later reorged out leaves the ledger reflecting a payment that no
     * longer exists. No funds are at stake — the ledger only records who is
     * owed a turn — but the fairness memory is wrong until it washes out.
     * reconcile_blocks() resolves chain membership for the blocks_found row;
     * it does NOT unwind a settled plan. See ecash-pool-proportional-plan.md
     * §3.6. */
    /* ⛔ !solo IS LOad-BEARING. The PPLNS plan is per-TEMPLATE and shared by
     * every connection, so a solo miner can solve the very template a plan was
     * built for. Settling it then marks the shareholders as PAID out of a
     * coinbase that paid only the solo finder — measured on regtest 2026-08-27:
     * the ledger swung 0.7 per claimant, turning one shareholder's +0.30 claim
     * into a -0.40 debt for a block it received nothing from. No funds move; it
     * is the fairness memory that decides who is paid first out of the NEXT
     * real block, and production carries 139 non-zero prop_ledger rows.
     *
     * This is a defect solo INTRODUCES: before it, every block in proportional
     * mode was a PPLNS block, so settling was unconditionally right.
     *
     * ⚠️ SKIP, do NOT consume or clear the plan. On a low-difficulty chain two
     * miners can solve the same job; if a solo connection solves job X and a
     * PPLNS connection also does, the second one SHOULD still settle. Clearing
     * here would silently destroy that. Left alone the plan ages out of the
     * ring, which is also the right semantics — from the PPLNS book's view a
     * solo block is simply a block that did not pay them, like one another
     * pool found. */
    if (s && s->store && s->cfg && accepted && !solo &&
        strcmp(s->cfg->pool_mode, "proportional") == 0 && job_id) {
        prop_plan_t settled;
        memset(&settled, 0, sizeof settled);
        int found = 0;
        pthread_mutex_lock(&s->lock);
        for (size_t i = 0; i < PROP_PLAN_RING; i++) {
            prop_plan_t *p = &s->prop_plans[i];
            if (p->job_id[0] && strcmp(p->job_id, job_id) == 0) {
                settled = *p;            /* takes the carry allocation */
                memset(p, 0, sizeof *p); /* consumed: never settle it twice */
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&s->lock);

        if (found) {
            if (store_prop_settle_block(s->store, ts_ms,
                                        settled.ledger_in, settled.n_ledger_in,
                                        settled.ledger, settled.n_ledger) < 0) {
                LOG_ERROR("proportional: settling block %s (job %s) failed — "
                          "%zu payouts and %zu deferred claims are NOT recorded",
                          block_hash ? block_hash : "?", job_id,
                          settled.n_payouts, settled.n_ledger);
            } else {
                LOG_INFO("proportional: settled block %s — %zu coinbase payouts, "
                         "%zu deferred claims",
                         block_hash ? block_hash : "?",
                         settled.n_payouts, settled.n_ledger);
            }
            prop_plan_clear(&settled);
        } else {
            LOG_INFO("proportional: block %s came from job %s, which had no "
                     "payout plan — its coinbase paid the finder directly",
                     block_hash ? block_hash : "?", job_id);
        }
    }
    /* pool:blocks carries solved blocks. A candidate the node refused is not
     * one, so it does not go out on that channel — the DB row is where a
     * refusal is visible. */
    if (s && s->bcast && accepted) {
        broadcast_block(s->bcast, worker_name, finder_address,
                        ts_ms, height, block_hash, reward_sats, fee_sats);
    }
    if (accepted) {
        LOG_INFO("BLOCK CANDIDATE ACCEPTED: height=%u finder=%s reward=%lld "
                 "fee=%lld hash=%s (pending confirmation)",
                 height, worker_name ? worker_name : "?",
                 (long long)reward_sats, (long long)fee_sats,
                 block_hash ? block_hash : "?");
    } else {
        LOG_WARN("BLOCK CANDIDATE REJECTED: height=%u finder=%s hash=%s "
                 "reason=%s", height, worker_name ? worker_name : "?",
                 block_hash ? block_hash : "?",
                 submit_error && submit_error[0] ? submit_error : "unknown");
    }
}

/* ---------- block confirmation ---------- */

/* Decide which of the pool's candidates are actually in the chain.
 *
 * Preferred path is getblockhash: authoritative, one call per unresolved
 * candidate. Not always available — the CUSF enforcer, which is the backend a
 * drivechain pool must point at, answers "Method not found" to everything but
 * getblocktemplate and submitblock. So fall back to the chain of tips the pool
 * has already observed: a template building height H+1 with prev_hash X says
 * the node's tip at H was X. That needs no RPC at all.
 *
 * Whichever answered is recorded in checked_via, the same way
 * pool_meta.network_source distinguishes an authoritative answer from an
 * inferred one. Nothing here invents a verdict: a candidate that neither path
 * can speak to stays pending, and pending counts as nothing. */
static void reconcile_blocks(server_ctx_t *s, int tip_height) {
    if (!s || !s->store || tip_height <= 0) return;

    if (atomic_load(&s->gbh_state) >= 0) {
        store_block_candidate_t cands[RECONCILE_MAX_PER_TICK];
        int n = store_list_unresolved_blocks(s->store, tip_height,
                                             BLOCK_FINAL_DEPTH, cands,
                                             RECONCILE_MAX_PER_TICK);
        for (int i = 0; i < n; ++i) {
            char have[80] = {0};
            char gerr[256] = {0};
            int rc = bitcoind_get_block_hash(s->btc, cands[i].height, have,
                                             sizeof have, gerr, sizeof gerr);
            if (rc == BITCOIND_ERR_UNSUPPORTED) {
                atomic_store(&s->gbh_state, -1);
                LOG_INFO("backend does not serve getblockhash — confirming "
                         "blocks from the observed chain of template tips "
                         "instead");
                break;
            }
            if (rc != 0) {
                /* Transient. Leave the rows alone and retry on the next tip
                 * rather than recording a verdict we did not get. */
                LOG_WARN("getblockhash(%d) failed: %s", cands[i].height, gerr);
                return;
            }
            atomic_store(&s->gbh_state, 1);
            int match = strcasecmp(have, cands[i].hash) == 0;
            store_set_block_status(s->store, cands[i].hash,
                                   match ? STORE_BLOCK_CONFIRMED
                                         : STORE_BLOCK_ORPHANED,
                                   match ? tip_height - cands[i].height + 1 : 0,
                                   "node");
            if (!match) {
                LOG_WARN("block %s at height %d is no longer in the chain — "
                         "marked orphaned", cands[i].hash, cands[i].height);
            }
        }
        if (atomic_load(&s->gbh_state) > 0) return;
    }

    int confirmed = 0, orphaned = 0, pending = 0;
    if (store_reconcile_blocks_from_templates(s->store, tip_height, &confirmed,
                                              &orphaned, &pending) == 0) {
        LOG_DEBUG("block reconcile: confirmed=%d orphaned=%d pending=%d",
                  confirmed, orphaned, pending);
    }
}

/* Starting share difficulty for a reconnecting worker: what it was actually
 * running at, so a pool restart does not send every miner back down to
 * initial_diff and make it climb again. Looks back an hour. */
static double on_difficulty_hint_cb(void *ctx, const char *worker_name) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (!s || !s->store || !worker_name) return 0.0;
    return store_worker_recent_difficulty(s->store, worker_name, 3600);
}

/* ---------- tip watcher ---------- */

static void *tip_watcher(void *arg) {
    server_ctx_t *s = (server_ctx_t *)arg;
    /* BIP22 long-poll token from the previous template. While set, requests
     * are parked server-side until the template goes stale, so the loop
     * needs no sleep — the response IS the new-tip notification. Empty means
     * the server doesn't long poll (e.g. stock bitcoind config without it,
     * or an older enforcer) and we fall back to interval polling. */
    char lpid[128] = {0};
    int consec_errs = 0;
    while (!g_shutdown) {
        if (lpid[0] == '\0') {
            uint64_t delay_ms = (uint64_t)s->cfg->bitcoind_poll_interval_ms;
            if (consec_errs > 0) {
                /* BIP22: failed requests SHOULD be retried with exponential
                 * backoff — retrying with no real delay is explicitly
                 * forbidden, and matters when the configured poll interval
                 * is aggressive (e.g. 10ms). 1s doubling to a 32s cap. */
                int shift = consec_errs - 1 < 5 ? consec_errs - 1 : 5;
                uint64_t backoff_ms = 1000ULL << shift;
                if (backoff_ms > delay_ms) delay_ms = backoff_ms;
            }
            struct timespec ts;
            ts.tv_sec  = (time_t)(delay_ms / 1000);
            ts.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
        if (g_shutdown) break;

        char err[512] = {0};
        bitcoind_template_t *t = NULL;
        if (bitcoind_get_block_template_lp(s->btc_lp, lpid[0] ? lpid : NULL,
                                           &t, err, sizeof err) < 0) {
            LOG_WARN("getblocktemplate %s failed: %s",
                     lpid[0] ? "long poll" : "poll", err);
            /* Drop to poll mode: the nanosleep above paces the retries, and
             * a server that stopped long polling is handled gracefully. */
            lpid[0] = '\0';
            if (consec_errs < 16) consec_errs++;
            continue;
        }
        consec_errs = 0;

        /* GBT returns the height of the NEXT block to mine and the hash
         * of the current tip in prev_hash_hex. Mirror that into the DB
         * so the dashboard can show 'latest block from the node' and
         * 'time since the last block' without any RPC of its own.
         * The upsert preserves tip_observed_at when the tip is the same. */
        uint64_t now_s = (uint64_t)time(NULL);
        store_record_node_tip(s->store, t->height - 1, t->prev_hash_hex,
                              now_s, now_s);
        if (s->bcast) {
            broadcast_node_tip(s->bcast, t->height - 1, t->prev_hash_hex, now_s);
        }

        int need_rebuild = 0;
        int new_tip      = 0;
        pthread_mutex_lock(&s->lock);
        if (t->height != s->last_height ||
            strcmp(t->prev_hash_hex, s->last_prev_hash) != 0) {
            need_rebuild = 1;
            new_tip      = 1;
        } else if (now_ms() - s->last_built_ms > 30000) {
            /* Periodic refresh for new ntime + included txs. */
            need_rebuild = 1;
        }
        pthread_mutex_unlock(&s->lock);

        /* A new tip is exactly when a candidate's fate can have changed:
         * either it is the one that extended the chain, or something else
         * was. */
        if (t->height - 1 != s->last_height) reconcile_blocks(s, t->height - 1);
        /* The two branches above are exactly the clean_jobs distinction, and
         * it used to be computed here and then thrown away: every job went
         * out flagged clean. On a same-tip refresh that tells every miner to
         * discard valid work in progress, several times a minute, for nothing.
         * A block change is the only thing that makes work worthless. */
        int clean = new_tip || s->cfg->clean_jobs_on_refresh;

        if (need_rebuild) {
            char berr[256] = {0};
            stratum_job_t *job = build_job_from_template(s, s->cfg, t, berr, sizeof berr);
            if (!job) {
                LOG_ERROR("rebuild job failed: %s", berr);
                bitcoind_template_free(t);
                continue;
            }
            /* One server, one publish: every listener broadcasts from the
             * same connection list, so all ports necessarily agree on job_id,
             * payout plan and merkle branches. Under the two-server model this
             * agreement had to be arranged by hand, by passing an extra
             * reference to the second server. */
            stratum_server_set_job(s->srv, job, clean);
            /* Difficulty and block value move with the template, so the
             * rate has to move with it too. */
            refresh_pps_rate(s, t);
            pthread_mutex_lock(&s->lock);
            s->last_height = t->height;
            snprintf(s->last_prev_hash, sizeof s->last_prev_hash, "%s",
                     t->prev_hash_hex);
            s->last_built_ms = now_ms();
            pthread_mutex_unlock(&s->lock);
            LOG_INFO("new job: height=%d prev=%.16s... txs=%zu",
                     t->height, t->prev_hash_hex, t->tx_count);
        }
        if (t->longpollid) {
            if (lpid[0] == '\0') {
                LOG_INFO("getblocktemplate long polling enabled");
            }
            snprintf(lpid, sizeof lpid, "%s", t->longpollid);
        } else {
            lpid[0] = '\0';
        }
        bitcoind_template_free(t);
    }
    return NULL;
}

/* ---------- pool identity ---------- */

/* Which chain this pool is mining, and how confidently we know it.
 *
 * getblockchaininfo is authoritative, so ask first. It is also not always
 * available: the CUSF enforcer serves getblocktemplate and submitblock and
 * answers "Method not found" to everything else, and that enforcer is
 * precisely the backend a drivechain pool has to point at for BIP300/301
 * commitments. So fall back to the network encoded in operator_address —
 * which is weaker (it cannot tell testnet from signet) but never wrong about
 * mainnet — and record which of the two answered, so the dashboard can say
 * "inferred" instead of asserting.
 *
 * Also the only place the two are ever compared. A mainnet operator address
 * on a test chain, or the reverse, pays the fee to a script nobody on that
 * chain controls: the block is valid, the coinbase looks fine, and the
 * money is gone. That is worth a loud line in the journal. */
static void resolve_network(bitcoind_client_t *btc, const proxy_config_t *cfg,
                            char *net, size_t net_cap,
                            char *src, size_t src_cap) {
    const char *from_addr = coinbase_address_network(cfg->operator_address);
    char node_chain[32] = {0};
    char nerr[256] = {0};

    if (bitcoind_get_chain(btc, node_chain, sizeof node_chain,
                           nerr, sizeof nerr) == 0) {
        snprintf(net, net_cap, "%s", node_chain);
        snprintf(src, src_cap, "node");
        if (from_addr &&
            coinbase_network_is_mainnet(node_chain) !=
            coinbase_network_is_mainnet(from_addr)) {
            LOG_WARN("operator_address '%s' is a %s address but the node is "
                     "on '%s' — the %d bps fee would pay a script nobody on "
                     "this chain controls. Fix operator_address before "
                     "mining a block.",
                     cfg->operator_address, from_addr, node_chain,
                     cfg->fee_bps);
        }
        return;
    }

    if (from_addr) {
        snprintf(net, net_cap, "%s", from_addr);
        snprintf(src, src_cap, "inferred");
        LOG_INFO("network: backend does not answer getblockchaininfo (%s); "
                 "inferred '%s' from operator_address", nerr, from_addr);
        return;
    }

    snprintf(net, net_cap, "unknown");
    snprintf(src, src_cap, "unknown");
    LOG_WARN("network: could not determine which chain this pool is mining "
             "(getblockchaininfo: %s, and operator_address '%s' encodes no "
             "network)", nerr, cfg->operator_address);
}

/* ---------- usage ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [config_path]\n"
            "  config_path  path to proxy.conf (default ./proxy.conf)\n"
            "  --version    print build provenance (version, commit, branch)\n",
            prog);
}

int main(int argc, char **argv) {
    /* Before ANY socket exists. stratum_server_start() spawns the listener,
     * so setting this afterwards left a window where a miner that connected
     * and vanished could kill the process with SIGPIPE on the first write. */
    signal(SIGPIPE, SIG_IGN);
    const char *cfg_path = "./proxy.conf";
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0) {
            version_print();
            return 0;
        }
        cfg_path = argv[1];
    }

    /* Load config. */
    proxy_config_t cfg;
    char err[512] = {0};
    if (proxy_config_load(cfg_path, &cfg, err, sizeof err) < 0) {
        fprintf(stderr, "config error: %s\n", err);
        return 2;
    }
    /* Fail fast on a misconfigured operator_address — otherwise every
     * coinbase render at runtime would warn and drop the job. This catches
     * the proxy.conf.example placeholder ("bcrt1q...") and any typo. */
    {
        uint8_t op_spk[64];
        size_t  op_spk_len = sizeof op_spk;
        char    op_err[256] = {0};
        if (coinbase_address_to_script(cfg.operator_address, op_spk,
                                       sizeof op_spk, &op_spk_len,
                                       op_err, sizeof op_err) < 0) {
            fprintf(stderr,
                    "config error: invalid operator_address '%s': %s\n"
                    "  set operator_address in %s to a real bitcoin "
                    "address (e.g. bc1q... on mainnet)\n",
                    cfg.operator_address, op_err, cfg_path);
            return 2;
        }
    }
    log_init(cfg.log_level);
    /* The commit goes in the first log line so the journal records which build
     * each run was, long after the binary has been replaced. */
    LOG_INFO("%s starting (config=%s)", version_line(), cfg_path);

    /* bitcoind client. */
    bitcoind_client_t btc = {0};
    bitcoind_cfg_t bcfg = {0};
    snprintf(bcfg.url,  sizeof bcfg.url,  "%s", cfg.bitcoind_url);
    snprintf(bcfg.user, sizeof bcfg.user, "%s", cfg.bitcoind_user);
    snprintf(bcfg.pass, sizeof bcfg.pass, "%s", cfg.bitcoind_pass);
    bcfg.timeout_ms = 10000;
    if (bitcoind_client_init(&btc, &bcfg) < 0) {
        fprintf(stderr, "bitcoind_client_init failed\n");
        return 3;
    }
    /* Second client for the tip watcher (see server_ctx_t.btc_lp). A BIP22
     * long poll parks server-side — 30s on the CUSF enforcer — so this
     * client's timeout must comfortably exceed the server's window. */
    bitcoind_client_t btc_lp = {0};
    bitcoind_cfg_t bcfg_lp = bcfg;
    bcfg_lp.timeout_ms = 90000;
    if (bitcoind_client_init(&btc_lp, &bcfg_lp) < 0) {
        fprintf(stderr, "bitcoind_client_init (long poll) failed\n");
        bitcoind_client_free(&btc);
        bitcoind_client_free(&btc_lp);
        return 3;
    }
    /* The ping is a getblockchaininfo sanity check. Some block-template
     * backends that accept unauthenticated JSON-RPC don't implement it, so
     * skip the ping when no credentials are configured — the initial
     * getblocktemplate below still validates connectivity. */
    if (cfg.bitcoind_user[0] != '\0' || cfg.bitcoind_pass[0] != '\0') {
        if (bitcoind_ping(&btc, err, sizeof err) < 0) {
            fprintf(stderr, "bitcoind ping failed: %s\n", err);
            bitcoind_client_free(&btc);
            return 3;
        }
        LOG_INFO("bitcoind ping ok");
    } else {
        LOG_INFO("bitcoind: no RPC credentials configured, "
                 "skipping getblockchaininfo ping");
    }

    /* Store. */
    store_cfg_t scfg = {0};
    snprintf(scfg.path, sizeof scfg.path, "%s", cfg.db_path);
    scfg.commit_window_ms  = cfg.commit_window_ms;
    scfg.commit_max_shares = cfg.commit_max_shares;
    scfg.templates_retention_days = cfg.templates_retention_days;
    store_t *store = NULL;
    if (store_open(&scfg, &store) < 0) {
        fprintf(stderr, "store_open failed for %s\n", cfg.db_path);
        bitcoind_client_free(&btc);
        bitcoind_client_free(&btc_lp);
        return 4;
    }

    /* ⓘ THE COMPATIBILITY SHIM — this is what makes the architecture swap
     * config-neutral.
     *
     * The rental port used to be a second stratum_server_t. It is now a second
     * listener inside the one server, which is the whole point: two servers
     * seeded extranonce1 from the clock milliseconds apart, handed out
     * colliding values, rendered identical coinbases, and deduped against
     * per-server rings blind to each other. One server cannot reach that state.
     *
     * ⛔ The PRODUCTION CONFIG IS NOT TOUCHED. rental_listen_port /
     * rental_min_diff / rental_max_conns keep working exactly as they do
     * today by being mapped onto a listener here. Migrating the config to
     * `listener = port=... min_diff=...` is a SEPARATE decision (U2), not a
     * hostage to installing this binary. An operator who has already written
     * a `listener` line gets that and the rental keys are ignored — the config
     * validator refuses the two naming the same port.
     *
     * ⛔ THIS COMMENT USED TO CLAIM A BEHAVIOUR CHANGE THAT DOES NOT EXIST, and
     * it is corrected rather than deleted because the wrong version was
     * load-bearing in a migration decision on 2026-08-27. It said a listener's
     * min_diff is kept even when the chain's difficulty is lower. It is not.
     * `min_diff` is a RECORD OF INTENT only — it drives the marketplace warning
     * and the startup log; the floor is delivered through vardiff_min and
     * initial_diff, and the NETWORK-DIFFICULTY CLAMP in stratum.c overrides
     * both, unconditionally, after them. There is deliberately no pol_min_diff
     * on the connection to enforce it with (see stratum.c's "⛔ NO
     * pol_min_diff"). stratum_listener_t.min_diff carries the full reasoning
     * and ends "An earlier revision of this comment said the opposite and cited
     * upstream as the fix. It was wrong. Do not restore it." This was that
     * revision, surviving in a second file.
     *
     * Migrating rental_listen_port to a `listener` line therefore changes NO
     * difficulty behaviour. What it DOES change is everything below: the legacy
     * keys are ignored the moment any listener line exists. */
    if (cfg.rental_listen_port > 0 && cfg.listener_count == 0) {
        stratum_listener_t *l = &cfg.listeners[cfg.listener_count++];
        memset(l, 0, sizeof *l);
        l->port         = cfg.rental_listen_port;
        l->initial_diff = cfg.rental_min_diff;
        l->vardiff_min  = cfg.rental_min_diff;
        l->min_diff     = cfg.rental_min_diff;
        if (cfg.vardiff_max < cfg.rental_min_diff) {
            cfg.vardiff_max = cfg.rental_min_diff;
        }
        snprintf(l->label, sizeof l->label, "rental");
        /* ⚠️ rental_max_conns has no per-listener equivalent: max_conns is
         * server-wide now. Raise the server cap to whichever is larger rather
         * than silently lowering the rental port's headroom — the cap refuses
         * SILENTLY, and this one peaked at 1,975 of 2,000 during order 4. */
        if (cfg.rental_max_conns > cfg.max_conns) {
            LOG_INFO("stratum: max_conns raised %d -> %d to preserve "
                     "rental_max_conns under the single-server model",
                     cfg.max_conns, cfg.rental_max_conns);
            cfg.max_conns = cfg.rental_max_conns;
        }
    }


    /* Pool identity into the DB, before anything else can read the table.
     * The dashboard shows this to miners; see store.h for why it lives in
     * the DB rather than in the dashboard's own environment. */
    {
        char network[32] = {0}, network_src[16] = {0};
        resolve_network(&btc, &cfg, network, sizeof network,
                        network_src, sizeof network_src);
        const int pps = strcmp(cfg.pool_mode, "pps-classic") == 0;
        LOG_INFO("pool identity: network=%s (%s) mode=%s fee=%d bps tag=\"%s\" "
                 "operator=%s%s%s",
                 network, network_src, cfg.pool_mode, cfg.fee_bps,
                 cfg.coinbase_tag, cfg.operator_address,
                 pps ? " pool_btc=" : "", pps ? cfg.pool_btc_address : "");
        /* Say what the payout caps ACTUALLY are at startup. A config value
         * that is parsed without complaint is not thereby in effect -- an
         * unknown key here only warns -- and prop_max_coinbase_bytes exists
         * precisely to prevent a silent drift, so it must not itself be
         * silent. This line is how "is it on?" gets answered from the journal
         * instead of from the config file the operator believes is loaded. */
        if (strcmp(cfg.pool_mode, "proportional") == 0) {
            if (cfg.prop_max_coinbase_bytes > 0)
                LOG_INFO("payout caps: max %d outputs, coinbase <= %d bytes",
                         cfg.prop_max_outputs, cfg.prop_max_coinbase_bytes);
            else
                LOG_INFO("payout caps: max %d outputs, no coinbase byte budget "
                         "(prop_max_coinbase_bytes unset)",
                         cfg.prop_max_outputs);
        }
        /* Publish the ports so the dashboard can tell a miner which one to
         * dial. Labels are constrained to [A-Za-z0-9_-] at config parse time,
         * so this needs no escaping.
         *
         * ⛔ promised_min_diff is NOT published. Upstream emits it because its
         * listener floor outranks the network-difficulty clamp; ours does not
         * (see stratum.c, where pol_min_diff deliberately does not exist), so
         * publishing the field would describe a promise nothing enforces. A
         * consumer would tell an operator the rental port holds 500,000 above
         * the chain when it does not. min_diff — the floor actually served,
         * network clamp and all — is the honest number and the only one here. */
        char lj[4096];
        size_t lo = 0;
        int dropped = 0;
        lo += (size_t)snprintf(lj + lo, sizeof lj - lo,
                               "[{\"port\":%d,\"label\":\"\","
                               "\"min_diff\":%.10g,\"initial_diff\":%.10g}",
                               cfg.listen_port, cfg.vardiff_min,
                               cfg.initial_diff);
        for (int i = 0; i < cfg.listener_count; ++i) {
            const stratum_listener_t *l = &cfg.listeners[i];
            char one[256];
            int n = snprintf(one, sizeof one,
                             ",{\"port\":%d,\"label\":\"%s\","
                             "\"min_diff\":%.10g,\"initial_diff\":%.10g}",
                             l->port, l->label,
                             l->vardiff_min > 0 ? l->vardiff_min : cfg.vardiff_min,
                             l->initial_diff > 0 ? l->initial_diff : cfg.initial_diff);
            if (n < 0 || lo + (size_t)n >= sizeof lj - 2) { dropped++; continue; }
            memcpy(lj + lo, one, (size_t)n);
            lo += (size_t)n;
        }
        lj[lo++] = ']';
        lj[lo] = '\0';
        if (dropped) {
            LOG_WARN("pool identity: %d listener(s) did not fit the published "
                     "port list — the dashboard will not show them", dropped);
        }
        store_record_pool_identity(store, network, network_src,
                                   cfg.coinbase_tag, cfg.operator_address,
                                   pps ? cfg.pool_btc_address : NULL, lj);
    }

    /* Broadcast (optional). */
    broadcast_cfg_t bcfg2 = {0};
    snprintf(bcfg2.url, sizeof bcfg2.url, "%s", cfg.redis_url);
    bcfg2.publish_timeout_ms   = cfg.redis_publish_timeout_ms;
    bcfg2.reconnect_backoff_ms = cfg.redis_reconnect_backoff_ms;
    broadcast_t *bcast = NULL;
    if (broadcast_open(&bcfg2, &bcast) < 0) {
        LOG_WARN("broadcast_open failed; continuing without redis");
        bcast = NULL;
    }

    /* Initial template + job. */
    bitcoind_template_t *tmpl = NULL;
    if (bitcoind_get_block_template(&btc, &tmpl, err, sizeof err) < 0) {
        fprintf(stderr, "initial GBT failed: %s\n", err);
        store_close(store);
        bitcoind_client_free(&btc);
        bitcoind_client_free(&btc_lp);
        return 5;
    }

    /* Server context (must outlive callbacks). Built before the first job
     * because pool_mode=proportional computes that job's payout set from the
     * store, and a first job without one would pay the finder alone. */
    server_ctx_t sctx;
    memset(&sctx, 0, sizeof sctx);
    pthread_mutex_init(&sctx.lock, NULL);
    sctx.btc    = &btc;
    sctx.btc_lp = &btc_lp;
    sctx.store  = store;
    sctx.bcast = bcast;
    sctx.cfg   = &cfg;
    sctx.last_height = tmpl->height;
    snprintf(sctx.last_prev_hash, sizeof sctx.last_prev_hash, "%s", tmpl->prev_hash_hex);
    sctx.last_built_ms = now_ms();

    stratum_job_t *initial_job = build_job_from_template(&sctx, &cfg, tmpl, err, sizeof err);
    if (!initial_job) {
        fprintf(stderr, "build initial job failed: %s\n", err);
        bitcoind_template_free(tmpl);
        store_close(store);
        bitcoind_client_free(&btc);
        bitcoind_client_free(&btc_lp);
        return 6;
    }

    /* Seed node_status from the initial template so the dashboard has data
     * to show before the first watcher poll fires. */
    {
        uint64_t now_s = (uint64_t)time(NULL);
        store_record_node_tip(store, tmpl->height - 1, tmpl->prev_hash_hex,
                              now_s, now_s);
        if (bcast) {
            broadcast_node_tip(bcast, tmpl->height - 1, tmpl->prev_hash_hex, now_s);
        }
    }

    /* Classify whatever is already on record, once, before serving.
     *
     * Rows written before blocks_found had a status are all 'pending', which
     * counts as nothing — correct, but useless. The templates table is a log
     * of the tips this pool observed, so one bulk SQL pass settles every
     * candidate whose next height was ever seen, with no RPC and no reliance
     * on a backend that may serve only two methods. What it cannot reach
     * stays pending.
     *
     * Then, and only then, the UNIQUE index on hash: it fails outright on a
     * table that still holds duplicates, which is why it is not a migration —
     * the migration runner would swallow that failure as a warning and leave
     * the index missing on exactly the databases that needed it. */
    {
        int confirmed = 0, orphaned = 0, pending = 0;
        if (store_reconcile_blocks_from_templates(store, tmpl->height - 1,
                                                  &confirmed, &orphaned,
                                                  &pending) == 0) {
            LOG_INFO("blocks on record: confirmed=%d orphaned=%d pending=%d",
                     confirmed, orphaned, pending);
            if (pending > 0) {
                LOG_INFO("%d block candidate(s) could not be verified from "
                         "observed tips and count as nothing until they are",
                         pending);
            }
        }
        store_finalize_block_hash_index(store);
    }

    /* Seed the rate before any share can arrive — a share credited at 0
     * would be silently unpaid. */
    refresh_pps_rate(&sctx, tmpl);

    /* Start stratum server. */
    stratum_cfg_t stcfg;
    memset(&stcfg, 0, sizeof stcfg);
    snprintf(stcfg.bind_addr, sizeof stcfg.bind_addr, "%s", cfg.listen_addr);
    stcfg.bind_port    = cfg.listen_port;
    stcfg.max_conns    = cfg.max_conns;
    /* rcfg is a copy of stcfg, so the rental listener inherits this too. */
    stcfg.listen_backlog = cfg.listen_backlog;
    /* The listener table, already carrying the rental shim's entry if the
     * operator is still on the rental_* keys. */
    memcpy(stcfg.listeners, cfg.listeners, sizeof stcfg.listeners);
    stcfg.listener_count  = cfg.listener_count;
    stcfg.max_submits_per_sec = cfg.max_submits_per_sec;
    stcfg.static_diff_enabled = cfg.static_diff_enabled;
    stcfg.initial_diff = cfg.initial_diff;
    snprintf(stcfg.operator_address, sizeof stcfg.operator_address, "%s",
             cfg.operator_address);
    stcfg.fee_bps      = cfg.fee_bps;
    snprintf(stcfg.coinbase_tag, sizeof stcfg.coinbase_tag, "%s",
             cfg.coinbase_tag);
    stcfg.vardiff_enabled    = cfg.vardiff_enabled;
    stcfg.vardiff_target_spm = cfg.vardiff_target_spm;
    stcfg.vardiff_min        = cfg.vardiff_min;
    stcfg.vardiff_max        = cfg.vardiff_max;
    stcfg.vardiff_window_sec = cfg.vardiff_window_sec;
    stcfg.vardiff_min_samples     = cfg.vardiff_min_samples;
    stcfg.max_suggested_diff      = cfg.max_suggested_diff;
    stcfg.vardiff_max_window_mult = cfg.vardiff_max_window_mult;
    stcfg.vardiff_idle_step       = cfg.vardiff_idle_step;
    stcfg.idle_timeout_sec   = cfg.idle_timeout_sec;
    stcfg.idle_timeout_authorized_sec = cfg.idle_timeout_authorized_sec;

    /* PPS. pool_mode=pps-classic takes Thunder-address usernames, pays every
     * coinbase into the pool's BTC wallet, and accrues per-share credits. */
    stcfg.pps_enabled = (strcmp(cfg.pool_mode, "pps-classic") == 0);
    /* pool_mode is what stratum.c derives its proportional render path from. */
    snprintf(stcfg.pool_mode, sizeof stcfg.pool_mode, "%s", cfg.pool_mode);
    snprintf(stcfg.pool_btc_address, sizeof stcfg.pool_btc_address, "%s",
             cfg.pool_btc_address);

    if (stcfg.pps_enabled) {
        /* Fail fast on a misconfigured pool_btc_address so we don't drop
         * every rendered job at runtime. */
        uint8_t spk[64];
        size_t  spk_len = sizeof spk;
        char    perr[256] = {0};
        if (coinbase_address_to_script(cfg.pool_btc_address, spk, sizeof spk,
                                       &spk_len, perr, sizeof perr) < 0) {
            fprintf(stderr,
                    "config error: invalid pool_btc_address '%s': %s\n",
                    cfg.pool_btc_address, perr);
            return 2;
        }
        LOG_INFO("pool_mode=pps-classic: pool_btc_address=%s, pps_sats_per_diff=%.2f",
                 cfg.pool_btc_address, cfg.pps_sats_per_diff);
    }
    stcfg.ctx            = &sctx;
    stcfg.on_share       = on_share_cb;
    stcfg.on_difficulty_hint = on_difficulty_hint_cb;
    stcfg.on_reject      = on_reject_cb;
    stcfg.on_block       = on_block_cb;
    stcfg.on_block_found = on_block_found_cb;
    /* Let the server see the accrual gate so it can refuse work the pool has
     * decided not to pay for. */
    stcfg.pps_gate = &sctx.pps_gated;
    stcfg.pps_refuse_shares_below_min = cfg.pps_refuse_shares_below_min;

    /* Validate the rental port BEFORE anything is started, so a bad value is
     * a clean config error rather than a half-built pool. */
    if (cfg.rental_listen_port > 0) {
        if (cfg.rental_listen_port == cfg.listen_port) {
            fprintf(stderr, "config error: rental_listen_port (%d) must differ "
                            "from listen_port\n", cfg.rental_listen_port);
            stratum_job_free(initial_job);
            bitcoind_template_free(tmpl);
            store_close(store);
            bitcoind_client_free(&btc);
            bitcoind_client_free(&btc_lp);
            return 2;
        }
        if (cfg.rental_min_diff <= 0.0) {
            fprintf(stderr, "config error: rental_min_diff must be > 0\n");
            stratum_job_free(initial_job);
            bitcoind_template_free(tmpl);
            store_close(store);
            bitcoind_client_free(&btc);
            bitcoind_client_free(&btc_lp);
            return 2;
        }
    }

    stratum_server_t *srv = NULL;
    if (stratum_server_start(&stcfg, &srv) < 0) {
        fprintf(stderr, "stratum_server_start failed\n");
        stratum_job_free(initial_job);
        bitcoind_template_free(tmpl);
        store_close(store);
        bitcoind_client_free(&btc);
        bitcoind_client_free(&btc_lp);
        return 7;
    }
    sctx.srv = srv;
    stratum_server_set_job(srv, initial_job, 1);

    LOG_INFO("stratum listening on %s:%d", cfg.listen_addr, cfg.listen_port);
    if (cfg.listener_count > 0) {
        /* Logged from stcfg, not cfg: after the shim above this is the list
         * the server actually bound, whichever config spelling produced it. */
        for (int i = 0; i < cfg.listener_count; ++i) {
            LOG_INFO("stratum listening on %s:%d [%s] at difficulty %.0f "
                     "(vardiff floor; the network difficulty still clamps it down)",
                     cfg.listen_addr, cfg.listeners[i].port,
                     cfg.listeners[i].label[0] ? cfg.listeners[i].label : "-",
                     cfg.listeners[i].min_diff);
        }
    }

    bitcoind_template_free(tmpl);

    /* Signals. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Tip watcher thread. Without it nothing ever fetches a new template, so
     * the pool would hand every miner the same initial job forever while
     * looking perfectly healthy — fail loudly instead. */
    pthread_t watcher;
    /* pthread_create RETURNS the error number and does not set errno, so
     * strerror(errno) here would print an unrelated stale error on the one
     * path where the message is all the operator gets. */
    int watcher_rc = pthread_create(&watcher, NULL, tip_watcher, &sctx);
    if (watcher_rc != 0) {
        fprintf(stderr, "fatal: could not start the tip watcher: %s\n",
                strerror(watcher_rc));
        LOG_ERROR("fatal: could not start the tip watcher: %s",
                  strerror(watcher_rc));
        stratum_server_stop(srv);
        stratum_server_free(srv);
        store_close(store);
        if (bcast) broadcast_close(bcast);
        bitcoind_client_free(&btc);
        bitcoind_client_free(&btc_lp);
        return 8;
    }

    /* Main loop: wait for shutdown. */
    while (!g_shutdown) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }

    LOG_INFO("shutdown requested");

    pthread_join(watcher, NULL);

    /* One server: stopping it joins every listener thread and drains every
     * connection, so there is no ordering between ports to get right. */
    stratum_server_stop(srv);
    stratum_server_free(srv);

    store_flush(store);
    store_stats_t stats;
    store_get_stats(store, &stats);
    LOG_INFO("final stats: shares_committed=%llu rejects_committed=%llu blocks=%llu sqlite_errs=%llu events_lost=%llu",
             (unsigned long long)stats.shares_committed,
             (unsigned long long)stats.rejects_committed,
             (unsigned long long)stats.blocks_committed,
             (unsigned long long)stats.pg_errors,
             (unsigned long long)stats.events_lost);
    /* Loud and separate, because it is the one number here that means miners
     * are owed work the ledger has no record of. Nothing else reports it. */
    if (stats.events_lost > 0) {
        LOG_ERROR("store: %llu accepted event(s) never reached the DB this run "
                  "— those shares are uncredited and unrecoverable",
                  (unsigned long long)stats.events_lost);
    }
    store_close(store);

    if (bcast) {
        broadcast_stats_t bs;
        broadcast_get_stats(bcast, &bs);
        LOG_INFO("broadcast: published=%llu enqueued=%llu "
                 "dropped(queue=%llu,redis=%llu) reconnects=%llu",
                 (unsigned long long)bs.published,
                 (unsigned long long)bs.enqueued,
                 (unsigned long long)bs.dropped_queue_full,
                 (unsigned long long)bs.dropped_redis_down,
                 (unsigned long long)bs.reconnects);
        broadcast_close(bcast);
    }

    bitcoind_client_free(&btc);
    bitcoind_client_free(&btc_lp);
    pthread_mutex_destroy(&sctx.lock);

    LOG_INFO("simplepool exited cleanly");
    return 0;
}
