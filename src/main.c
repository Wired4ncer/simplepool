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

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
} server_ctx_t;

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
                                            int64_t *out_headroom_wu) {
    size_t ceiling = (size_t)cfg->prop_max_outputs;
    if (ceiling > PROP_PLAN_MAX_PAY) ceiling = PROP_PLAN_MAX_PAY;
    if (!t->coinbasetxn_hex) {
        if (out_headroom_wu) *out_headroom_wu = -1;
        return ceiling;
    }
    int64_t tx_weight = 0;
    for (size_t i = 0; i < t->tx_count; i++) tx_weight += t->txs[i].weight;

    /* What the builder splices into the scriptSig: both extranonces, plus the
     * tag and its length byte. Mirrors coinbase_build_from_template_multi. */
    size_t ss_growth = 4 + 4;
    size_t taglen = strlen(cfg->coinbase_tag);
    if (taglen) ss_growth += (taglen > 75 ? 75 : taglen) + 1;

    return coinbase_max_payout_outputs(t->weight_limit, tx_weight,
                                       strlen(t->coinbasetxn_hex) / 2,
                                       ss_growth, fee_output, ceiling,
                                       out_headroom_wu);
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
    size_t max_out = prop_max_outputs_for_template(t, s->cfg, fee_sats > 0,
                                                   &headroom_wu);

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

    size_t n_payouts = 0, n_ledger_out = 0;
    int rc = pplns_compute_payouts(reward_after_fee,
                                   addrs, n_addrs,
                                   ledger, ledger_cap, n_ledger_in, &n_ledger_out,
                                   s->cfg->prop_min_payout_sats, max_out,
                                   plan->payouts, &n_payouts);
    free(addrs);
    if (rc < 0 || n_payouts == 0) {
        free(ledger); free(ledger_in);
        LOG_WARN("proportional: payout computation produced nothing "
                 "(rc=%d, %zu addresses, window difficulty %.2f); falling back",
                 rc, n_addrs, actual_diff);
        return 1;
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
                                               /*en1*/ 4, /*en2*/ 4,
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

    char job_id[32];
    snprintf(job_id, sizeof job_id, "%llx", (unsigned long long)now_ms());

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
        /*en1*/ 4, /*en2*/ 4,
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
                        const char *block_hash_or_null) {
    server_ctx_t *s = (server_ctx_t *)ctx;

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
                                block_hash_or_null, delta, rate_used);
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

static void on_reject_cb(void *ctx, const char *worker_name, uint64_t ts_ms,
                         const char *reason) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (s && s->store) {
        store_record_reject(s->store, worker_name, ts_ms, reason);
    }
    if (s && s->bcast) {
        broadcast_reject(s->bcast, worker_name, ts_ms, reason);
    }
}

static int on_block_cb(void *ctx, const char *block_hex) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (!s || !s->btc) return -1;
    char err[512] = {0};
    int rc = bitcoind_submit_block(s->btc, block_hex, err, sizeof err);
    if (rc == 0) {
        LOG_INFO("submitted block to bitcoind successfully");
    } else {
        /* Carries "rejected: inconclusive" for a valid block that lost the
         * race for the tip. The caller uses this to decide whether the block
         * is recorded at all — it is not merely logged. */
        LOG_ERROR("submitblock failed: %s", err);
    }
    return rc;
}

static void on_block_found_cb(void *ctx, const char *worker_name,
                              const char *finder_address,
                              uint64_t ts_ms, uint32_t height,
                              const char *job_id,
                              const char *block_hash,
                              int64_t reward_sats, int64_t fee_sats) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (s && s->store) {
        store_record_block(s->store, ts_ms, (int)height, block_hash,
                           worker_name, finder_address,
                           reward_sats, fee_sats);
    }

    /* pool_mode=proportional: commit the carry-forward balances belonging to
     * the job that was actually solved. Nothing is committed for a job that had
     * no plan (a fallback template paid its finder directly), and nothing is
     * committed twice — the plan is consumed here.
     *
     * ⚠️ This runs on acceptance by the pool, not by the network. A block that
     * is later reorged out leaves the ledger reflecting a payment that no longer
     * exists. No funds are at stake — the ledger only records who is owed a turn
     * — but the fairness memory is wrong until it washes out. Solo mode has no
     * such state; see ecash-pool-proportional-plan.md §3.6. */
    if (s && s->store && s->cfg &&
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
    if (s && s->bcast) {
        broadcast_block(s->bcast, worker_name, finder_address,
                        ts_ms, height, block_hash, reward_sats, fee_sats);
    }
    LOG_INFO("BLOCK FOUND: height=%u finder=%s reward=%lld fee=%lld hash=%s",
             height, worker_name ? worker_name : "?",
             (long long)reward_sats, (long long)fee_sats,
             block_hash ? block_hash : "?");
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
        pthread_mutex_lock(&s->lock);
        if (t->height != s->last_height ||
            strcmp(t->prev_hash_hex, s->last_prev_hash) != 0) {
            need_rebuild = 1;
        } else if (now_ms() - s->last_built_ms > 30000) {
            /* Periodic refresh for new ntime + included txs. */
            need_rebuild = 1;
        }
        pthread_mutex_unlock(&s->lock);

        if (need_rebuild) {
            char berr[256] = {0};
            stratum_job_t *job = build_job_from_template(s, s->cfg, t, berr, sizeof berr);
            if (!job) {
                LOG_ERROR("rebuild job failed: %s", berr);
                bitcoind_template_free(t);
                continue;
            }
            stratum_server_set_job(s->srv, job);
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

    /* Seed the rate before any share can arrive — a share credited at 0
     * would be silently unpaid. */
    refresh_pps_rate(&sctx, tmpl);

    /* Start stratum server. */
    stratum_cfg_t stcfg;
    memset(&stcfg, 0, sizeof stcfg);
    snprintf(stcfg.bind_addr, sizeof stcfg.bind_addr, "%s", cfg.listen_addr);
    stcfg.bind_port    = cfg.listen_port;
    stcfg.max_conns    = cfg.max_conns;
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
    stcfg.idle_timeout_sec   = cfg.idle_timeout_sec;

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
    stratum_server_set_job(srv, initial_job);
    bitcoind_template_free(tmpl);

    LOG_INFO("stratum listening on %s:%d", cfg.listen_addr, cfg.listen_port);

    /* Signals. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Tip watcher thread. */
    pthread_t watcher;
    pthread_create(&watcher, NULL, tip_watcher, &sctx);

    /* Main loop: wait for shutdown. */
    while (!g_shutdown) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }

    LOG_INFO("shutdown requested");

    pthread_join(watcher, NULL);

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
