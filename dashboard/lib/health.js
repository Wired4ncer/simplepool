/* Hard-failure health checks.
 *
 * Every problem this pool has hit was found by someone running a query, never
 * by being told. These are the conditions that mean money or data is already
 * wrong, evaluated on every page load and surfaced as a banner.
 *
 * Deliberately NOT here: degradation signals (BMM capture, settlement
 * latency, backlog size, reserve headroom). They are worth watching but they
 * fluctuate, and a banner that is sometimes red for a slow afternoon is a
 * banner nobody reads. Everything below is binary and actionable.
 *
 * Each check is independent and self-contained: a check that throws because
 * its table predates the current schema reports `unavailable` rather than
 * taking the page down with it.
 *
 * Evaluated on a timer, not per request. better-sqlite3 is synchronous, and
 * the duplicate-hash check measured 2.0s against 337k shares — running that on
 * the request path with a 15s auto-refresh would stall the whole dashboard.
 * The timer keeps full-history coverage (a windowed check would go green on a
 * duplicate from two days ago that nobody noticed) while page loads only read
 * the last snapshot. See startHealthMonitor / currentHealth.
 */

import { rateVerification } from './stats.js';

/* A payout batch legitimately waits for a Thunder block, which waits for a
 * mainchain block carrying its BMM commitment — routinely ~10 minutes on
 * drynet3. An hour means something is genuinely wrong, not slow. */
const PAYOUT_STALL_SEC = 3600;

function one(d, sql, ...args) {
    return d.prepare(sql).get(...args);
}

/* Run a check, converting a schema mismatch into "unavailable" rather than an
 * exception. A check that cannot run is not a check that passed — and until
 * this returned ok:false it literally did: `ok` is `failing.length === 0`, so a
 * throwing check left /health answering 200 with ok:true. A human saw the
 * amber banner; the uptime monitor watching the endpoint saw green. If the
 * duplicate_shares query started throwing on a schema change, the pool would
 * report healthy while shares were being credited twice.
 *
 * `unavailable` is kept as its own flag, because two different things set it:
 *
 *   - THIS path — the check blew up. Not a pass: ok:false, so /health is 503.
 *   - A check returning {ok:true, unavailable:true} on purpose, meaning "not
 *     applicable yet" (e.g. template_commitments before the first template).
 *     That is a genuine pass and must not page anyone on a fresh deploy.
 *
 * So do NOT collapse this to "unavailable means failing" — the banner's
 * two-tier display and the fresh-install case both depend on the distinction. */
function guard(id, label, fn) {
    try {
        const r = fn();
        return r ? { id, label, ...r } : { id, label, ok: true };
    } catch (e) {
        return { id, label, ok: false, unavailable: true, detail: e.message };
    }
}

export function health(handle) {
    const d = !handle ? null
            : (typeof handle.get === 'function' ? handle.get() : handle);
    if (!d) {
        return { ok: false, db_ready: false, checks: [], failing: [],
                 unavailable: [] };
    }

    const checks = [];

    /* Accepted work that never reached the DB. The miner was already told
     * "accepted", so this is an unrecoverable shortfall against them — and no
     * other query can see it, because the rows were never written. */
    checks.push(guard('events_lost', 'Shares accepted but never stored', () => {
        const r = one(d, 'SELECT events_lost AS n FROM pool_meta WHERE id = 1');
        const n = Number(r?.n || 0);
        return { ok: n === 0, value: n,
                 detail: n > 0
                    ? `${n} accepted event(s) lost to failed commits; those shares are uncredited`
                    : null };
    }));

    /* The extranonce1 collision class: two connections rendering identical
     * coinbases and submitting the same hash, credited twice.
     *
     * COUNT(block_hash) not COUNT(*) — the latter counts NULL hashes from
     * legacy rows that COUNT(DISTINCT ...) ignores, which reports every one of
     * them as a duplicate. */
    checks.push(guard('duplicate_shares', 'Duplicate share hashes', () => {
        const r = one(d, `SELECT COUNT(block_hash) - COUNT(DISTINCT block_hash) AS n
                            FROM shares`);
        const n = Number(r?.n || 0);
        return { ok: n === 0, value: n,
                 detail: n > 0
                    ? `${n} share(s) credited more than once — every PPS figure is inflated`
                    : null };
    }));

    /* The three ledger checks from the audit: a credit that does not follow
     * from its own stored rate, a published rate that does not follow from its
     * template, and a credit at a rate never published. */
    checks.push(guard('ledger', 'Ledger arithmetic and rate provenance', () => {
        const v = rateVerification(d);
        if (!v) return { ok: true, unavailable: true, detail: 'DB predates the rate audit' };
        const bad = [];
        if (v.mismatched > 0) bad.push(`${v.mismatched} credit(s) ≠ difficulty × rate_used`);
        if (v.rates_inconsistent > 0) bad.push(`${v.rates_inconsistent} published rate(s) not derivable from their template`);
        if (v.orphaned > 0) bad.push(`${v.orphaned} share(s) credited at an unpublished rate`);
        return { ok: bad.length === 0, value: v.mismatched + v.rates_inconsistent + v.orphaned,
                 detail: bad.length ? bad.join('; ') : null };
    }));

    /* Mined minus owed. Negative means the pool cannot cover its PPS
     * liability out of what it has actually earned. */
    checks.push(guard('margin', 'Pool solvency', () => {
        const r = one(d, `
            SELECT (SELECT COALESCE(SUM(reward_sats),0) + COALESCE(SUM(fee_sats),0)
                      FROM blocks_found)
                 - (SELECT COALESCE(SUM(credited_sats),0) FROM shares) AS margin`);
        const m = Number(r?.margin || 0);
        return { ok: m >= 0, value: m,
                 detail: m < 0
                    ? `owed ${(-m / 1e8).toFixed(4)} BTC more than mined`
                    : null };
    }));

    /* In-flight rows with no txid mean the worker died around a broadcast and
     * cannot tell whether it went out. Never auto-resolves — the one state
     * that genuinely needs a human. */
    checks.push(guard('payout_ambiguous', 'Payouts stuck without a txid', () => {
        const r = one(d, `SELECT COUNT(*) AS n FROM payouts_in_flight
                           WHERE txid IS NULL OR txid = ''`);
        const n = Number(r?.n || 0);
        return { ok: n === 0, value: n,
                 detail: n > 0
                    ? `${n} in-flight row(s) with no txid — needs manual reconciliation (payout/README.md)`
                    : null };
    }));

    /* A batch waiting far longer than a Thunder block should take. */
    checks.push(guard('payout_stalled', 'Payout settling', () => {
        const r = one(d, `SELECT COALESCE(MAX(strftime('%s','now') - started_at), 0) AS age,
                                 COUNT(*) AS n
                            FROM payouts_in_flight WHERE txid IS NOT NULL AND txid <> ''`);
        const age = Number(r?.age || 0);
        const n   = Number(r?.n || 0);
        if (n === 0) return { ok: true, value: 0 };
        return { ok: age < PAYOUT_STALL_SEC, value: age,
                 detail: age >= PAYOUT_STALL_SEC
                    ? `a batch has been unconfirmed for ${Math.floor(age / 60)} min — Thunder may not be advancing`
                    : null };
    }));

    /* Blocks that carry no BIP300/301 commitments are valid and pay miners,
     * so nothing else complains — but no sidechain can be merge-mined into
     * them, which is what stalled Thunder before. */
    checks.push(guard('template_commitments', 'Templates carry sidechain commitments', () => {
        const r = one(d, `SELECT source, cb_op_returns FROM templates
                           ORDER BY id DESC LIMIT 1`);
        if (!r) return { ok: true, unavailable: true, detail: 'no templates recorded yet' };
        const ok = r.source === 'enforcer' && Number(r.cb_op_returns) > 1;
        return { ok, value: Number(r.cb_op_returns),
                 detail: ok ? null
                    : `mining ${r.source} templates with ${r.cb_op_returns} OP_RETURN(s) — no sidechain can merge-mine` };
    }));

    const failing     = checks.filter(c => !c.ok);
    const unavailable = checks.filter(c => c.unavailable);
    return { ok: failing.length === 0, db_ready: true, checks, failing, unavailable };
}

/* ---- snapshot, so page loads never pay for the scan --------------------- */

/* `null` until the first pass completes. Rendered as "checking", never as
 * healthy: silence has to mean "not yet known", or a monitor that never ran
 * looks identical to a pool with nothing wrong. */
let snapshot = null;
let timer = null;

export function currentHealth() {
    return snapshot;
}

export function startHealthMonitor(handle, { intervalMs = 300000 } = {}) {
    const run = () => {
        const startedAt = Date.now();
        try {
            snapshot = { ...health(handle), checked_at: Math.floor(startedAt / 1000),
                         took_ms: Date.now() - startedAt };
        } catch (e) {
            /* The monitor failing is itself a failure worth showing, rather
             * than leaving the previous (possibly stale, possibly green)
             * snapshot in place indefinitely. */
            snapshot = { ok: false, db_ready: false, checks: [], unavailable: [],
                         checked_at: Math.floor(startedAt / 1000),
                         failing: [{ id: 'monitor', label: 'Health monitor',
                                     ok: false, detail: e.message }] };
        }
    };
    run();
    timer = setInterval(run, intervalMs);
    timer.unref?.();     /* never hold the process open */
    return () => { clearInterval(timer); timer = null; };
}
