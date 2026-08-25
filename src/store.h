#ifndef SIMPLEPOOL_STORE_H
#define SIMPLEPOOL_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "pplns.h"

typedef struct store store_t;

typedef struct {
    char path[512];
    int  commit_window_ms;     /* default 100 */
    int  commit_max_shares;    /* default 100 */
    /* Days of template history to keep; 0 disables pruning. Enforced when a
     * new template row is opened, so a busy pool trims itself and an idle
     * one leaves the table alone. */
    int  templates_retention_days;
} store_cfg_t;

/* Open the DB (creates file + applies schema if missing). Starts a writer
 * thread. Returns 0 ok, negative on error. */
int store_open(const store_cfg_t *cfg, store_t **out);

/* Stop the writer thread (drains queue, commits), close DB, free. */
void store_close(store_t *s);

/* Record an accepted share. Thread-safe. Returns immediately - the actual
 * INSERT is batched on the writer thread. Returns 0 if queued, negative if
 * the queue is full (caller may log/drop).
 *
 * share_hash_or_null is the SHA256 of the share's block header in big-endian
 * hex. When is_block=1 it is also the block hash. For older callers / tests
 * that still pass NULL the row is stored with NULL in the hash column. */
int store_record_share(store_t *s, const char *worker_name,
                       uint64_t ts_ms, double difficulty,
                       int is_block, const char *share_hash_or_null);

/* Record a rejected share. */
int store_record_reject(store_t *s, const char *worker_name,
                        uint64_t ts_ms, const char *reason);

/* Record a block found. Thread-safe.
 * finder_address may be NULL (legacy callers); reward_sats/fee_sats may be
 * 0 to skip recording. */
/* Lifecycle of a block candidate. A share that meets network difficulty is
 * only ever a *candidate*: submitblock can refuse it, and even an accepted
 * one can be reorged out. Only CONFIRMED means the pool mined a block that
 * is in the chain, and only CONFIRMED may be counted as pool revenue —
 * summing rewards across every row is what silently disabled the solvency
 * check. PENDING is not a transient: against a backend that answers only
 * getblocktemplate/submitblock there may be nothing that can verify a block
 * for some time, and "not yet verified" must count as nothing. */
#define STORE_BLOCK_PENDING    0
#define STORE_BLOCK_CONFIRMED  1
#define STORE_BLOCK_ORPHANED   2
#define STORE_BLOCK_REJECTED   3

/* The text written to blocks_found.status. Never NULL. */
const char *store_block_status_text(int status);

/* A candidate still in play: not yet resolved, or confirmed but not yet deep
 * enough to be final. */
typedef struct {
    char hash[80];
    int  height;
} store_block_candidate_t;

/* Candidates worth re-checking against the node, most recent first, at most
 * `cap` of them. Rows already `rejected` or `orphaned` are settled, and a
 * `confirmed` row past `final_depth` confirmations is treated as final and
 * stops being re-checked. Returns how many were written, or negative on
 * error. Rows older than the node path reaches are handled in bulk by
 * store_reconcile_blocks_from_templates(). */
int store_list_unresolved_blocks(store_t *s, int tip_height, int final_depth,
                                 store_block_candidate_t *out, size_t cap);

/* Set one candidate's verdict. `checked_via` is 'node' or 'tips' — which
 * source answered, the same distinction pool_meta.network_source draws. */
int store_set_block_status(store_t *s, const char *hash, int status,
                           int confirmations, const char *checked_via);

/* Classify candidates from the templates table alone — no RPC.
 *
 * Every getblocktemplate poll is an observation of the node's tip: a template
 * building height H+1 with prev_hash X says the tip at H was X. `templates`
 * keeps one row per materially distinct template, so it is a historical chain
 * of tips this pool actually saw, and comparing a candidate against the most
 * recent observation at its height+1 says whether it is still the chain's.
 *
 * This is the only path available against a backend that serves nothing but
 * getblocktemplate and submitblock, and it is also how a table of pre-existing
 * rows gets classified in bulk. A candidate whose height+1 was never observed
 * stays pending — which counts as nothing — rather than being guessed at.
 *
 * Writes the resulting totals (not deltas) to the out params when non-NULL. */
int store_reconcile_blocks_from_templates(store_t *s, int tip_height,
                                          int *confirmed, int *orphaned,
                                          int *pending);

/* Collapse rows that share a block hash, then create the UNIQUE index on it.
 *
 * Deliberately not a migration: CREATE UNIQUE INDEX fails outright on a table
 * that already holds duplicates, and the migration runner only tolerates
 * "duplicate column" — so as a migration it would silently never exist on the
 * databases that needed it. Duplicates arise because the stratum dedupe guard
 * is an in-memory ring that empties on restart, so the same solution can be
 * recorded twice. The surviving row keeps the earliest sighting but inherits
 * any resolved status its duplicates reached. */
int store_finalize_block_hash_index(store_t *s);

/* `status` is one of STORE_BLOCK_*; `submit_error` is the reason string from
 * submitblock and may be NULL for anything but a rejected candidate. A
 * height <= 0 is refused outright. */
int store_record_block(store_t *s, uint64_t ts_ms, int height,
                       const char *hash, const char *finder_name,
                       const char *finder_address,
                       int64_t reward_sats, int64_t fee_sats,
                       int status, const char *submit_error);

/* Record an accepted share with the miner's payout_address so the worker
 * row can be tagged. payout_address may be NULL (legacy/tests). The
 * share_hash semantics match store_record_share() above.
 *
 * credited_sats is what this share was credited at the rate in force when
 * it was accepted, stored on the row so audits report history instead of
 * recomputing it against whatever rate is current. Pass 0 in solo mode or
 * when no accrual applies.
 *
 * rate_used is the multiplicand that produced credited_sats. Pass the exact
 * double that was multiplied, not a rounded copy: the audit re-derives
 * CAST(difficulty * rate_used AS INTEGER) and expects credited_sats back
 * bit-for-bit, which is what makes the credit checkable rather than merely
 * recorded. Pass 0.0 whenever credited_sats is 0. */
int store_record_share_addr(store_t *s, const char *worker_name,
                            const char *payout_address,
                            uint64_t ts_ms, double difficulty,
                            int is_block, const char *share_hash_or_null,
                            int64_t credited_sats, double rate_used);

/* PPS credit: add delta_sats to the worker's accrued_sats in pps_credits.
 * Async (writer thread). delta_sats must be > 0. payout_address (the
 * miner's Thunder address) is tagged onto the workers row as usual. */
int store_record_credit(store_t *s, const char *worker_name,
                        const char *payout_address,
                        uint64_t ts_ms, int64_t delta_sats);

/* Publish what this proxy is actually paying, so the dashboard reads the
 * effective rate from the running process instead of keeping its own copy
 * of the config. Single-row upsert on id=1, synchronous (called at most
 * once per template change, not per share).
 *
 * rate_source is "derived" or "override". gross is fair value before the
 * fee; rate is net of it. effective_fee_bps is what the pair actually
 * implies, which under an override need not equal fee_bps. */
/* The pool's identity: which chain it builds coinbases for, the tag it
 * stamps into them, and the addresses the money goes to. Upserts the same
 * id=1 row as store_record_pool_meta() but touches only these columns, so
 * call order between the two does not matter.
 *
 * Written once at startup, because none of it changes while the process
 * runs — and it is written to the DB at all because the dashboard must not
 * hold its own copy of the proxy's config. network_source is "node" when
 * getblockchaininfo answered and "inferred" when the network was read off
 * the operator address instead. Pass a NULL/empty pool_btc_address in solo
 * mode; it is stored as NULL so "not applicable" reads differently from
 * "configured blank". */
int store_record_pool_identity(store_t *s, const char *network,
                               const char *network_source,
                               const char *coinbase_tag,
                               const char *operator_address,
                               const char *pool_btc_address);

int store_record_pool_meta(store_t *s, const char *pool_mode, int fee_bps,
                           const char *rate_source,
                           double rate_sats_per_diff,
                           double gross_sats_per_diff,
                           double effective_fee_bps,
                           double network_difficulty,
                           int64_t block_value_sats,
                           uint64_t updated_ts_s);

/* Append one row to the append-only rate log, unless the newest row already
 * carries exactly these values — so the table grows with rate changes, not
 * with template polls. Synchronous; called at most once per template change
 * alongside store_record_pool_meta().
 *
 * This is the provenance half of the audit: shares.rate_used proves the
 * arithmetic was applied consistently, and this proves the rate itself
 * followed from the template and the configured fee. */
int store_record_rate(store_t *s, const char *rate_source,
                      double rate_sats_per_diff,
                      double gross_sats_per_diff,
                      int fee_bps,
                      double network_difficulty,
                      int64_t block_value_sats,
                      uint64_t ts_s);

/* One block template, as the pool received it. */
typedef struct {
    uint64_t    ts_s;
    int         height;
    const char *prev_hash;
    const char *bits;                /* nbits as hex */
    double      network_difficulty;
    int64_t     coinbase_value_sats; /* subsidy + fees */
    int         tx_count;
    int64_t     tx_fees_sats;
    /* "enforcer" when the backend dictated the coinbase (BIP22 coinbasetxn),
     * "bitcoind" when we built our own. Only the former carries BIP300/301
     * commitments, so only the former lets a sidechain be merge-mined. */
    const char *source;
    int         cb_spendable;        /* server coinbase outputs; 0 when we build it */
    int         cb_op_returns;
    int         longpoll;            /* server supports BIP22 long polling */
    double      rate_sats_per_diff;  /* PPS rate derived from this template */
} store_template_t;

/* Record the template being mined. Synchronous; called once per poll.
 *
 * Keyed on what makes the *work* different — tip, nBits, source and coinbase
 * shape. A poll matching the newest row updates that row in place (refreshing
 * the block value, tx set and rate, bumping `polls` and `last_seen`) instead
 * of appending: the block value drifts with every mempool tick, so keying on
 * it appended a near-duplicate row per poll — thousands a day, nearly all of
 * them fee churn at a height already recorded.
 *
 * Prunes history older than cfg.templates_retention_days when it opens a new
 * row. */
int store_record_template(store_t *s, const store_template_t *t);

/* Record / refresh the upstream bitcoind tip the proxy is mining on.
 * Single-row upsert keyed on id=1. tip_observed_at is preserved when
 * (height, hash) match the existing row, so 'time since last tip change'
 * stays meaningful across repeated polls of the same tip. Synchronous —
 * not routed through the writer thread (it's called at most once per
 * bitcoind_poll_interval_ms). */
int store_record_node_tip(store_t *s, int height, const char *hash,
                          uint64_t observed_ts_s, uint64_t updated_ts_s);

/* Flush and wait until all currently-queued events are committed. Useful
 * for tests and clean shutdown before exit. Returns 0 ok, negative on
 * timeout (default 5s). */
int store_flush(store_t *s);

/* Stats for /metrics / health endpoints. Lockless reads of atomics. */
typedef struct {
    uint64_t shares_queued;
    uint64_t shares_committed;
    uint64_t shares_dropped;
    uint64_t rejects_queued;
    uint64_t rejects_committed;
    uint64_t blocks_committed;
    uint64_t batches;
    uint64_t pg_errors;        /* poorly named; sqlite errors */
    /* Events that left the ring but never reached the DB, after every
     * commit retry failed. Accepted work that will never be credited —
     * distinct from shares_dropped, which is enqueue-side overflow.
     * Must be 0; anything else is a ledger shortfall against miners. */
    uint64_t events_lost;
} store_stats_t;
void store_get_stats(store_t *s, store_stats_t *out);

/* Optional: override default ring buffer capacity (events). Must be called
 * before store_open by setting a global; for tests only. */
void store_test_set_ring_capacity(size_t cap);

/* Override the PPLNS window walk's page size and row cap so paging and
 * truncation can be exercised without inserting millions of shares. Pass <= 0
 * to restore the defaults. For tests only. */
void store_test_set_window_limits(int page, long max_rows);

/* Typical share difficulty this worker was recently running at, for seeding a
 * reconnecting miner instead of restarting it at initial_diff.
 *
 * The median of its LAST 32 shares — newest first, then median. Not the mean
 * (a vardiff ramp's tail of tiny difficulties drags it down) and not the
 * median over the whole lookback (same problem: an Avalon converged at 13,680
 * had an hour-median of 4, because repeated restarts meant most of its shares
 * came from the climb). The last few dozen shares are where vardiff settled.
 * Returns 0.0 when the worker has no usable history. */
double store_worker_recent_difficulty(store_t *s, const char *worker_name,
                                      int lookback_sec);

/* ---------- proportional / PPLNS helpers ---------- */

/* Read the deferred-claim ledger into *out. Caller must free(*out).
 * Returns 0 ok, negative on error. */
int store_prop_get_ledger(store_t *s, pplns_claim_t **out, size_t *n);

/* Find the PPLNS window boundary: starting from the newest share with
 * ts <= before_ms, walk back in time until BOTH conditions hold — the
 * cumulative difficulty has reached window_difficulty, AND the walk has reached
 * at least min_window_sec seconds before before_ms. The window is therefore
 * whichever of the two is larger.
 *
 * min_window_sec is the floor that keeps the window meaningful when difficulty
 * collapses (it resets to powLimit at every fork, which would otherwise leave
 * the window a handful of shares wide). Pass 0 to disable it.
 *
 * Whole seconds are indivisible: every share sharing the boundary timestamp is
 * included, so the window never splits a second between two miners.
 *
 * Writes the oldest timestamp in the window to *out_start_ms and the actual
 * cumulative difficulty to *out_actual_difficulty (may be >= window_difficulty,
 * and will be when the time floor binds).
 *
 * *out_truncated (optional) is set to 1 when the walk hit its row cap before
 * satisfying the work target, i.e. the window returned is SHORTER than asked
 * for. Callers must surface that: a short window is not a wrong answer that
 * announces itself — the pool keeps paying, just on a window nobody chose.
 *
 * Returns 0 ok, -1 if no shares. */
int store_prop_compute_window(store_t *s, double window_difficulty,
                              uint64_t before_ms, int min_window_sec,
                              uint64_t *out_start_ms,
                              double *out_actual_difficulty,
                              int *out_truncated);

/* Query shares within [start_ms, end_ms] grouped by payout address, sorted by
 * total difficulty descending. Writes *out (caller frees) and *n.
 * Returns 0 ok, negative on error. */
int store_prop_window_addrs(store_t *s, uint64_t start_ms, uint64_t end_ms,
                            pplns_addr_t **out, size_t *n);

/* Apply the result of a found block to prop_ledger, as a DELTA.
 *
 * ledger_in is the ledger the payout plan was computed from; ledger_out is the
 * ledger pplns_compute_payouts() produced from it. What lands in the table is
 * (current + ledger_out - ledger_in), per address, computed inside the same
 * transaction that writes it.
 *
 * ⚠️ It takes both halves, and not just the post-block state, because the plan
 * is built when the TEMPLATE is fetched and settled when a block is found —
 * different threads, an unbounded gap apart, with no ordering between one
 * block's settle and the next template's read of the ledger. Writing
 * ledger_out wholesale therefore published a snapshot that could already be
 * stale, and a second block settling from an overlapping snapshot silently
 * dropped the first block's claims. The delta is immune to that: two plans
 * built from the same snapshot compose instead of overwriting, and since both
 * halves are zero-sum the delta is too, so the stored ledger stays zero-sum.
 *
 * An address in the table that appears in neither half is left alone — that is
 * the whole point. An address whose result lands on zero is removed.
 *
 * ⛔ Does NOT touch blocks_found. store_record_block() is the sole writer of
 * that table and is called for every block; a second insert here duplicated
 * every pooled block (see store.c). Returns 0 ok, negative on error. */
int store_prop_settle_block(store_t *s, uint64_t ts_ms,
                            const pplns_claim_t *ledger_in, size_t n_ledger_in,
                            const pplns_claim_t *ledger_out, size_t n_ledger_out);

#endif /* SIMPLEPOOL_STORE_H */
