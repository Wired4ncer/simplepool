// Stats queries against the simplepool SQLite schema.
//
// All hashrate estimates use:  H/s ≈ sum(difficulty) * 2^32 / windowSec
//
// The handle passed in is the wrapper from lib/db.js — it may not yet be
// connected (proxy hasn't started). In that case we return empty/zero
// shapes so the templates render gracefully.

const TWO_32 = 4294967296;

const EMPTY_OVERVIEW = {
    accepted: 0,
    rejected: 0,
    blocks: 0,
    blocks_pending: 0,
    blocks_orphaned: 0,
    blocks_rejected: 0,
    workers_active: 0,
    hashrate: 0,
    window_sec: 86400,
    db_ready: false,
};

/* A row in blocks_found is a block CANDIDATE. Only status='confirmed' is a
 * block the pool actually mined and can be paid for, so every count and every
 * sum of reward_sats filters on it. Counting all of them is what made a pool
 * that had mined nothing look like it had mined thousands of blocks.
 *
 * The other statuses are reported alongside rather than hidden: a pool whose
 * candidates are nearly all orphaned has a real operational problem, and
 * hiding the rows is what kept it invisible. */
const CONFIRMED = "status = 'confirmed'";

function db(handle) {
    return handle.get();
}

/* Hashrate over an arbitrary window. Uses the standard estimator
 * H/s ≈ sum(difficulty) * 2^32 / windowSec. */
function hashrateOver(d, nowSec, windowSec) {
    const row = d.prepare(
        'SELECT COALESCE(SUM(difficulty),0) AS sum_diff FROM shares WHERE ts >= ?'
    ).get(nowSec - windowSec);
    return (row.sum_diff * TWO_32) / windowSec;
}

export function overview(handle, windowSec = 86400) {
    const d = db(handle);
    if (!d) return { ...EMPTY_OVERVIEW, window_sec: windowSec };
    const nowSec = Math.floor(Date.now() / 1000);
    const since = nowSec - windowSec;

    const acc = d.prepare(
        'SELECT COUNT(*) AS n, COALESCE(SUM(difficulty),0) AS sum_diff, COALESCE(MAX(difficulty),0) AS best FROM shares WHERE ts >= ?'
    ).get(since);
    const rej = d.prepare('SELECT COUNT(*) AS n FROM rejects WHERE ts >= ?').get(since);
    const blk = d.prepare(
        `SELECT COUNT(*) AS n FROM blocks_found WHERE ts >= ? AND ${CONFIRMED}`
    ).get(since);
    const wk = d.prepare(
        'SELECT COUNT(DISTINCT worker_id) AS n FROM shares WHERE ts >= ?'
    ).get(since);
    const lifetime = d.prepare(
        'SELECT COUNT(*) AS shares, COALESCE(MAX(difficulty),0) AS best, MIN(ts) AS oldest_ts FROM shares'
    ).get();
    const last = d.prepare('SELECT MAX(ts) AS ts FROM shares').get();
    const totalRej = d.prepare('SELECT COUNT(*) AS n FROM rejects').get();
    const totalBlk = d.prepare(
        `SELECT COUNT(*) AS n FROM blocks_found WHERE ${CONFIRMED}`
    ).get();
    const candidates = d.prepare(`
        SELECT COUNT(*) FILTER (WHERE status = 'pending')  AS pending,
               COUNT(*) FILTER (WHERE status = 'orphaned') AS orphaned,
               COUNT(*) FILTER (WHERE status = 'rejected') AS rejected
          FROM blocks_found
    `).get();

    const total24h = acc.n + rej.n;
    const rejectRate24h = total24h > 0 ? (rej.n / total24h) * 100 : 0;

    return {
        accepted: acc.n,
        rejected: rej.n,
        blocks: blk.n,
        blocks_pending:  Number(candidates?.pending  || 0),
        blocks_orphaned: Number(candidates?.orphaned || 0),
        blocks_rejected: Number(candidates?.rejected || 0),
        workers_active: wk.n,
        hashrate: (acc.sum_diff * TWO_32) / windowSec,   // 24h estimate
        hashrate_1h: hashrateOver(d, nowSec, 3600),
        hashrate_5m: hashrateOver(d, nowSec, 300),
        best_share_24h: acc.best,
        best_share_lifetime: lifetime.best,
        reject_rate_pct: rejectRate24h,
        shares_lifetime: lifetime.shares,
        rejects_lifetime: totalRej.n,
        blocks_lifetime: totalBlk.n,
        oldest_share_ts: lifetime.oldest_ts,
        last_share_ts: last.ts,
        window_sec: windowSec,
        db_ready: true,
    };
}

export function leaderboard(handle, windowSec = 86400, limit = 50) {
    const d = db(handle);
    if (!d) return [];
    const nowSec = Math.floor(Date.now() / 1000);
    const since = nowSec - windowSec;
    const since1h = nowSec - 3600;
    const since5m = nowSec - 300;

    // One pass for the 24h totals, then per-worker short-window sums.
    const rows = d.prepare(`
        SELECT w.id              AS id,
               w.name            AS name,
               w.payout_address  AS payout_address,
               COUNT(s.id)       AS shares,
               MAX(s.ts)         AS last_seen,
               COALESCE(SUM(s.difficulty), 0) AS sum_diff
          FROM shares s
          JOIN workers w ON w.id = s.worker_id
         WHERE s.ts >= ?
         GROUP BY w.id
         ORDER BY sum_diff DESC
         LIMIT ?
    `).all(since, limit);

    const sumShort = d.prepare(
        'SELECT COALESCE(SUM(difficulty),0) AS sd FROM shares WHERE worker_id = ? AND ts >= ?'
    );

    const totalDiff = rows.reduce((a, r) => a + r.sum_diff, 0);
    return rows.map(r => {
        const sd1h = sumShort.get(r.id, since1h).sd;
        const sd5m = sumShort.get(r.id, since5m).sd;
        return {
            name: r.name,
            payout_address: r.payout_address || null,
            shares: r.shares,
            last_seen: r.last_seen,
            hashrate_est: (r.sum_diff * TWO_32) / windowSec,
            hashrate_1h: (sd1h * TWO_32) / 3600,
            hashrate_5m: (sd5m * TWO_32) / 300,
            share_of_pool_pct: totalDiff > 0 ? (r.sum_diff / totalDiff) * 100 : 0,
        };
    });
}

/* Roll up the leaderboard by payout_address. Useful when one miner runs
 * several rigs under the same address (`bc1q….rig1`, `bc1q….rig2`, …):
 * each rig appears as its own row in `leaderboard()`, but here we collapse
 * them into one entry keyed by payout_address and list the rig labels. */
export function leaderboardByAddress(handle, windowSec = 86400, limit = 50) {
    const d = db(handle);
    if (!d) return [];
    const since = Math.floor(Date.now() / 1000) - windowSec;

    // Aggregate shares per (payout_address). Workers with no payout_address
    // recorded (legacy rows) fall back to their `name` as the key so they
    // still show up rather than collapsing into one nameless bucket.
    const rows = d.prepare(`
        SELECT COALESCE(NULLIF(w.payout_address, ''), w.name) AS addr,
               COUNT(s.id)                                    AS shares,
               MAX(s.ts)                                      AS last_seen,
               COALESCE(SUM(s.difficulty), 0)                 AS sum_diff,
               COUNT(DISTINCT w.id)                           AS rigs,
               GROUP_CONCAT(DISTINCT w.name)                  AS worker_names
          FROM shares s
          JOIN workers w ON w.id = s.worker_id
         WHERE s.ts >= ?
         GROUP BY addr
         ORDER BY sum_diff DESC
         LIMIT ?
    `).all(since, limit);

    const totalDiff = rows.reduce((a, r) => a + r.sum_diff, 0);
    return rows.map(r => {
        // Extract just the .label parts so we can show "rig1, rig2, rig3"
        // instead of repeating the address.
        const labels = (r.worker_names || '').split(',').map(n => {
            const dot = n.indexOf('.');
            return dot >= 0 ? n.slice(dot + 1) : '';
        }).filter(Boolean);
        return {
            address: r.addr,
            rigs: r.rigs,
            labels,
            shares: r.shares,
            last_seen: r.last_seen,
            hashrate_est: (r.sum_diff * TWO_32) / windowSec,
            share_of_pool_pct: totalDiff > 0 ? (r.sum_diff / totalDiff) * 100 : 0,
        };
    });
}

export function worker(handle, name, windowSec = 86400) {
    const d = db(handle);
    if (!d) return { worker: null, shares: [], buckets: [] };

    const w = d.prepare('SELECT * FROM workers WHERE name = ?').get(name);
    if (!w) return { worker: null, shares: [], buckets: [] };

    const since = Math.floor(Date.now() / 1000) - windowSec;

    const sharesRaw = d.prepare(`
        SELECT ts, difficulty, is_block, block_hash AS share_hash
          FROM shares
         WHERE worker_id = ?
         ORDER BY ts DESC
         LIMIT 200
    `).all(w.id);

    // Compute the share's "actual difficulty": diff1_target / hash_value.
    // diff1_target = 0x00000000_ffff0000_0000... (256-bit) — note it already
    // carries 8 leading zero nibbles of its own. Writing the hash as
    // v * 16^(56-i) for i leading zero nibbles and v the next 8 hex digits,
    // and diff1_target as 0xffff0000 * 16^48, the ratio comes out as
    // (0xffff0000 / v) * 16^(i-8). The -8 is diff1's own leading zeros;
    // dropping it overstates every share by 16^8 = 2^32.
    const shares = sharesRaw.map(s => {
        let actual = null;
        if (s.share_hash && /^[0-9a-fA-F]+$/.test(s.share_hash)) {
            // Hash is big-endian hex. Skip the leading zeros, take next 8
            // hex chars as a 32-bit value, then actual_diff ≈ 0xffff0000 / v.
            const h = s.share_hash.toLowerCase();
            let i = 0;
            while (i < h.length && h[i] === '0') i++;
            const slice = h.slice(i, i + 8).padEnd(8, '0');
            const v = parseInt(slice, 16);
            if (v > 0) {
                // Leading zeros beyond diff1's own eight are what make a
                // share harder than difficulty 1; fewer than eight means a
                // share easier than 1, so the exponent may go negative.
                const zeroFactor = Math.pow(16, i - 8);
                actual = (0xffff0000 / v) * zeroFactor;
            }
        }
        return { ...s, actual_diff: actual };
    });

    const sumRow = d.prepare(`
        SELECT COALESCE(SUM(difficulty),0) AS sum_diff,
               COUNT(*)                   AS n
          FROM shares
         WHERE worker_id = ? AND ts >= ?
    `).get(w.id, since);

    // 10-minute buckets across the window.
    const bucketSec = 600;
    const rawBuckets = d.prepare(`
        SELECT (ts / ?) * ? AS bucket,
               COALESCE(SUM(difficulty), 0) AS sum_diff,
               COUNT(*)                    AS n
          FROM shares
         WHERE worker_id = ? AND ts >= ?
         GROUP BY bucket
         ORDER BY bucket ASC
    `).all(bucketSec, bucketSec, w.id, since);

    // Densify so the sparkline has every bucket (zero-fill empty ones).
    const nowBucket = Math.floor(Math.floor(Date.now() / 1000) / bucketSec) * bucketSec;
    const startBucket = Math.floor(since / bucketSec) * bucketSec;
    const byTs = new Map(rawBuckets.map(b => [b.bucket, b]));
    const buckets = [];
    for (let t = startBucket; t <= nowBucket; t += bucketSec) {
        const b = byTs.get(t);
        const sd = b ? b.sum_diff : 0;
        buckets.push({
            ts: t,
            shares: b ? b.n : 0,
            hashrate: (sd * TWO_32) / bucketSec,
        });
    }

    /* Self-service PPS audit — same cross-check the admin view runs, scoped
     * to this one worker. Null in solo mode (no pps_credits row).
     *
     * The cross-check sums shares.credited_sats, which is what each share was
     * actually credited when it was accepted. It is NOT recomputed from a
     * current rate: the rate is derived per-template and moves with network
     * difficulty, so re-deriving history would report a mismatch on every
     * difficulty change. Rate metadata comes from pool_meta for display only. */
    let ppsAudit = null;
    const credit = d.prepare(`
        SELECT accrued_sats, paid_sats, last_updated
          FROM pps_credits WHERE worker_id = ?
    `).get(w.id);
    if (credit) {
        const meta = poolMeta(d);
        const totals = d.prepare(`
            SELECT COUNT(*)                            AS share_count,
                   COALESCE(SUM(difficulty), 0)        AS sum_difficulty,
                   COALESCE(SUM(credited_sats), 0)     AS accrued_computed
              FROM shares
             WHERE worker_id = ?
        `).get(w.id);
        const accrued = Number(credit.accrued_sats || 0);
        const paid    = Number(credit.paid_sats    || 0);
        ppsAudit = {
            rate: meta ? meta.rate_sats_per_diff : null,
            meta,
            /* Miners get the same re-derivation the operator sees — an audit
             * only the pool can run is not much of an audit. */
            verification: rateVerification(d, w.id),
            accrued, paid, owed: accrued - paid,
            last_updated: Number(credit.last_updated || 0),
            share_count:      Number(totals.share_count),
            sum_difficulty:   Number(totals.sum_difficulty),
            accrued_computed: Number(totals.accrued_computed),
            matches:          Number(totals.accrued_computed) === accrued,
        };
    }

    /* Payouts to this worker — same table the admin dashboard reads;
     * miners can verify their own txids from the public view. */
    let payouts = [];
    try {
        payouts = d.prepare(`
            SELECT id, sats, fee_sats, txid, paid_at, note
            FROM   payouts
            WHERE  worker_id = ?
            ORDER  BY paid_at DESC, id DESC
            LIMIT  100
        `).all(w.id).map(r => ({
            id: r.id, sats: Number(r.sats), fee_sats: Number(r.fee_sats),
            txid: r.txid, paid_at: Number(r.paid_at), note: r.note,
        }));
    } catch { /* payouts table missing on very old DBs — fine */ }

    /* Blocks found BY this worker specifically. */
    const workerBlocks = d.prepare(`
        SELECT id, ts, height, hash, reward_sats, fee_sats,
               status, confirmations, checked_via
        FROM   blocks_found
        WHERE  finder_id = ?
        ORDER  BY ts DESC, id DESC
        LIMIT  25
    `).all(w.id).map(r => ({
        id: r.id, ts: Number(r.ts), height: Number(r.height), hash: r.hash,
        reward_sats: Number(r.reward_sats || 0),
        fee_sats:    Number(r.fee_sats || 0),
        status: r.status || 'pending',
        confirmations: Number(r.confirmations || 0),
        checked_via: r.checked_via || null,
    }));

    return {
        worker: {
            name: w.name,
            payout_address: w.payout_address || null,
            first_seen: w.first_seen,
            last_seen: w.last_seen,
            window_shares: sumRow.n,
            window_hashrate: (sumRow.sum_diff * TWO_32) / windowSec,
        },
        shares,
        buckets,
        window_sec: windowSec,
        pps_audit: ppsAudit,
        payouts,
        blocks: workerBlocks,
    };
}

/* Latest tip the C proxy is mining on, mirrored from getblocktemplate.
 * Returns null if the proxy hasn't recorded a tip yet (fresh DB). */
/* What the proxy is actually paying, straight from the row it writes on
 * every template change. This is the ONLY place the dashboard should learn
 * the rate — never from its own config, or the audit can disagree with the
 * ledger it exists to check.
 *
 * Returns null on a DB predating pool_meta, in which case callers should
 * present the rate as unknown rather than substituting a guess. */
/* The identity half of pool_meta: which chain the pool builds coinbases
 * for, the tag it stamps into them, and where the money goes.
 *
 * Selected separately from the rate columns, and swallowing its own errors,
 * because the two halves land in different releases: a dashboard upgraded
 * ahead of the proxy reads a pool_meta that has no identity columns yet, and
 * folding this into the main SELECT would turn that into a null poolMeta —
 * losing the rate figures too, to add a banner.
 *
 * Everything is nullable on purpose. Rendering "unknown" is correct until
 * the proxy has restarted and written the row; guessing a network is not. */
function poolIdentity(d) {
    const blank = {
        network: null, network_source: null, coinbase_tag: null,
        operator_address: null, pool_btc_address: null,
    };
    try {
        const r = d.prepare(`
            SELECT network, network_source, coinbase_tag,
                   operator_address, pool_btc_address
              FROM pool_meta WHERE id = 1
        `).get();
        if (!r) return blank;
        /* The proxy binds "" for an unset string; normalise to null so
         * callers have one empty case to test rather than two. */
        const or_ = v => (v === undefined || v === null || v === '') ? null : v;
        return {
            network:          or_(r.network),
            network_source:   or_(r.network_source),
            coinbase_tag:     or_(r.coinbase_tag),
            operator_address: or_(r.operator_address),
            pool_btc_address: or_(r.pool_btc_address),
        };
    } catch {
        return blank;   /* DB predating the identity columns */
    }
}

export function poolMeta(handle) {
    /* Called from stats.js with a lazy handle and from admin.js with an
     * already-resolved better-sqlite3 Database, so accept either rather
     * than making callers remember which. */
    const d = !handle ? null
            : (typeof handle.get === 'function' ? handle.get() : handle);
    if (!d) return null;
    try {
        const r = d.prepare(`
            SELECT pool_mode, fee_bps, rate_source, rate_sats_per_diff,
                   gross_sats_per_diff, effective_fee_bps, network_difficulty,
                   block_value_sats, credited_from, updated_at
              FROM pool_meta WHERE id = 1
        `).get();
        if (!r) return null;
        const gross = Number(r.gross_sats_per_diff || 0);
        const rate  = Number(r.rate_sats_per_diff  || 0);
        return {
            ...poolIdentity(d),
            pool_mode:           r.pool_mode || 'solo',
            fee_bps:             Number(r.fee_bps || 0),
            rate_source:         r.rate_source || 'derived',
            rate_sats_per_diff:  rate,
            gross_sats_per_diff: gross,
            effective_fee_bps:   Number(r.effective_fee_bps || 0),
            network_difficulty:  Number(r.network_difficulty || 0),
            block_value_sats:    Number(r.block_value_sats || 0),
            credited_from:       Number(r.credited_from || 0),
            updated_at:          Number(r.updated_at || 0),
            /* An override whose implied fee has drifted from fee_bps is the
             * failure this table exists to expose. */
            fee_drift_bps: Number(r.effective_fee_bps || 0) - Number(r.fee_bps || 0),
            accrues: (r.pool_mode || 'solo') === 'pps-classic',
        };
    } catch {
        return null;   /* pre-pool_meta DB */
    }
}

/* Independently re-derive the PPS ledger instead of reporting it.
 *
 * Every other figure on the audit page is a number the proxy wrote and this
 * page repeats. These three are checks:
 *
 *   1. arithmetic — every credited share must satisfy
 *      credited_sats == floor(difficulty * rate_used), where both operands
 *      live on the share's own row. Holds no matter how far the rate has
 *      since moved, because nothing current is consulted. Exact equality is
 *      the right test: the proxy compiles without -ffast-math, so the same
 *      IEEE-754 multiply and truncation reproduce bit-for-bit here.
 *
 *   2. provenance — every rate in the log must follow from the template
 *      inputs recorded beside it and the configured fee. Catches a rate that
 *      was applied consistently but derived wrongly, which check 1 cannot
 *      see.
 *
 *   3. linkage — every rate a share was credited at must appear in the log,
 *      so no share was paid at a rate the pool never published. Scoped to
 *      shares newer than the first logged rate; older ones predate the log
 *      and are counted as unverifiable, not as failures.
 *
 * Returns null on a DB predating these columns, in which case the caller
 * should say the ledger cannot be verified rather than imply it passed.
 * Pass a workerId to scope checks 1 and 3 to one miner. */
export function rateVerification(handle, workerId = null) {
    const d = !handle ? null
            : (typeof handle.get === 'function' ? handle.get() : handle);
    if (!d) return null;
    const scoped = workerId !== null && workerId !== undefined;
    /* Two spellings because the orphan query aliases the table. */
    const where   = scoped ? 'AND worker_id = ?'   : '';
    const whereS  = scoped ? 'AND s.worker_id = ?' : '';
    const run     = (sql) => scoped ? d.prepare(sql).get(workerId)
                                    : d.prepare(sql).get();
    try {
        const shares = run(`
            SELECT COUNT(*)                                              AS total,
                   COUNT(*) FILTER (WHERE credited_sats > 0)             AS credited,
                   COUNT(*) FILTER (WHERE rate_used > 0)                 AS verifiable,
                   COUNT(*) FILTER (WHERE rate_used > 0
                       AND credited_sats <> CAST(difficulty * rate_used AS INTEGER))
                                                                         AS mismatched,
                   COUNT(*) FILTER (WHERE rate_used = 0 AND credited_sats > 0)
                                                                         AS unverifiable
              FROM shares
             WHERE 1 = 1 ${where}
        `);
        const rates = d.prepare(`
            SELECT COUNT(*) AS total,
                   COUNT(*) FILTER (WHERE ABS(rate_sats_per_diff
                       - (block_value_sats * 1.0 / network_difficulty)
                         * (1 - fee_bps / 10000.0)) > 1e-9)  AS inconsistent,
                   MIN(ts) AS first_ts
              FROM rate_history
        `).get();
        /* NULL-safe by construction: with an empty log MIN(ts) is NULL, the
         * ts comparison yields NULL, and nothing is counted — so a pool that
         * has not published a rate yet reports 0 orphans, not "everything is
         * an orphan". */
        const orphans = run(`
            SELECT COUNT(*) AS n
              FROM shares s
             WHERE s.rate_used > 0
               AND s.ts >= (SELECT MIN(ts) FROM rate_history)
               AND NOT EXISTS (SELECT 1 FROM rate_history r
                                WHERE r.rate_sats_per_diff = s.rate_used)
               ${whereS}
        `);
        const mismatched   = Number(shares.mismatched   || 0);
        const inconsistent = Number(rates.inconsistent  || 0);
        const orphaned     = Number(orphans.n           || 0);
        const credited     = Number(shares.credited     || 0);
        const verifiable   = Number(shares.verifiable   || 0);
        return {
            ok: mismatched === 0 && inconsistent === 0 && orphaned === 0,
            share_count:  Number(shares.total || 0),
            credited, verifiable,
            unverifiable: Number(shares.unverifiable || 0),
            mismatched, orphaned,
            /* What share of the credited ledger these checks actually cover.
             * 100% on a DB written entirely by this build or later. */
            coverage_pct: credited > 0 ? (verifiable / credited) * 100 : 100,
            rate_rows:         Number(rates.total || 0),
            rates_inconsistent: inconsistent,
            rate_log_from:     Number(rates.first_ts || 0),
        };
    } catch {
        return null;   /* DB predates rate_used / rate_history */
    }
}

/* What the pool is mining right now, and what it mined before.
 *
 * The row is appended by the proxy on each material template change, so the
 * newest row is the current job and the rest is history. `source` is the
 * field worth reading: 'bitcoind' means the pool built its own coinbase and
 * the block carries no BIP300/301 commitments, so no sidechain can be
 * merge-mined into it — a condition invisible from every other page.
 *
 * Returns null on a DB predating the table. */
export function templates(handle, { limit = 50 } = {}) {
    const d = !handle ? null
            : (typeof handle.get === 'function' ? handle.get() : handle);
    if (!d) return null;
    try {
        /* last_seen/polls arrived when repeat polls started folding into the
         * row they match. A DB written by an older proxy that has not been
         * restarted yet has neither, and every row there is a single
         * observation — so ts/1 are the honest values, not placeholders. */
        const cols  = new Set(d.prepare('PRAGMA table_info(templates)').all().map(c => c.name));
        if (cols.size === 0) return null;
        const spans = cols.has('last_seen') && cols.has('polls');
        const rows = d.prepare(`
            SELECT id, ts, height, prev_hash, bits, network_difficulty,
                   coinbase_value_sats, tx_count, tx_fees_sats, source,
                   cb_spendable, cb_op_returns, longpoll, rate_sats_per_diff,
                   ${spans ? 'COALESCE(NULLIF(last_seen, 0), ts)' : 'ts'} AS last_seen,
                   ${spans ? 'MAX(COALESCE(polls, 1), 1)'         : '1'}  AS polls
              FROM templates
             ORDER BY id DESC
             LIMIT ?
        `).all(Math.max(1, Math.min(500, Number(limit) || 50)));
        if (rows.length === 0) {
            return { current: null, history: [], total: 0, commitments_ok: null };
        }
        const norm = r => ({
            id:        Number(r.id),
            ts:        Number(r.ts),
            height:    Number(r.height),
            prev_hash: r.prev_hash,
            bits:      r.bits,
            network_difficulty:  Number(r.network_difficulty),
            coinbase_value_sats: Number(r.coinbase_value_sats),
            tx_count:      Number(r.tx_count),
            tx_fees_sats:  Number(r.tx_fees_sats),
            source:        r.source,
            cb_spendable:  Number(r.cb_spendable),
            cb_op_returns: Number(r.cb_op_returns),
            longpoll:      !!r.longpoll,
            rate_sats_per_diff: Number(r.rate_sats_per_diff),
            /* A row is a span, not an instant: ts is when this template was
             * first served, last_seen the most recent poll that still matched
             * it, and polls how many polls that covers. */
            last_seen:  Number(r.last_seen),
            polls:      Number(r.polls),
            held_sec:   Math.max(0, Number(r.last_seen) - Number(r.ts)),
            /* The subsidy is whatever is left once fees are removed. Derived
             * rather than stored: it is a property of the chain's schedule,
             * not of the template. */
            subsidy_sats: Number(r.coinbase_value_sats) - Number(r.tx_fees_sats),
            /* A server-dictated coinbase carries the sidechain commitments
             * alongside the witness commitment, so more than one OP_RETURN
             * is the observable signature of a mergeable block. */
            has_commitments: r.source === 'enforcer' && Number(r.cb_op_returns) > 1,
        });
        const total = d.prepare('SELECT COUNT(*) AS n FROM templates').get().n;
        const all   = rows.map(norm);
        return {
            current: all[0],
            history: all.slice(1),
            total:   Number(total),
            /* Whether the pool is currently producing blocks a sidechain can
             * be merge-mined into. */
            commitments_ok: all[0].has_commitments,
        };
    } catch {
        return null;   /* DB predates the templates table */
    }
}

export function nodeStatus(handle) {
    const d = db(handle);
    if (!d) return null;
    const row = d.prepare(
        'SELECT tip_height, tip_hash, tip_observed_at, updated_at FROM node_status WHERE id = 1'
    ).get();
    if (!row) return null;
    const nowSec = Math.floor(Date.now() / 1000);
    return {
        tip_height: row.tip_height,
        tip_hash: row.tip_hash,
        tip_observed_at: row.tip_observed_at,
        updated_at: row.updated_at,
        seconds_since_tip:    row.tip_observed_at ? nowSec - row.tip_observed_at : null,
        seconds_since_update: row.updated_at      ? nowSec - row.updated_at      : null,
    };
}

export function recentBlocks(handle, limit = 25) {
    const d = db(handle);
    if (!d) return [];
    return d.prepare(`
        SELECT b.ts,
               b.height,
               b.hash,
               b.finder_address,
               b.reward_sats,
               b.fee_sats,
               b.status,
               b.confirmations,
               b.checked_via,
               w.name AS finder
          FROM blocks_found b
          LEFT JOIN workers w ON w.id = b.finder_id
         ORDER BY b.ts DESC
         LIMIT ?
    `).all(limit);
}

/* Paginated full history. `beforeTs` is the exclusive upper bound used
 * for the "older" link; pass null/undefined for the first page. */
export function allBlocks(handle, { limit = 50, beforeTs = null } = {}) {
    const d = db(handle);
    if (!d) return { rows: [], next_before: null };
    const cap = Math.min(Math.max(Number(limit) || 50, 1), 200);
    let rows;
    if (beforeTs == null) {
        rows = d.prepare(`
            SELECT b.ts, b.height, b.hash, b.finder_address,
                   b.reward_sats, b.fee_sats, b.status, b.confirmations,
                   b.checked_via, w.name AS finder
              FROM blocks_found b
              LEFT JOIN workers w ON w.id = b.finder_id
             ORDER BY b.ts DESC
             LIMIT ?
        `).all(cap);
    } else {
        rows = d.prepare(`
            SELECT b.ts, b.height, b.hash, b.finder_address,
                   b.reward_sats, b.fee_sats, b.status, b.confirmations,
                   b.checked_via, w.name AS finder
              FROM blocks_found b
              LEFT JOIN workers w ON w.id = b.finder_id
             WHERE b.ts < ?
             ORDER BY b.ts DESC
             LIMIT ?
        `).all(Number(beforeTs), cap);
    }
    const next_before = rows.length === cap ? rows[rows.length - 1].ts : null;
    return { rows, next_before };
}

const SATS_PER_BTC = 100000000;
export function fmtBtc(sats) {
    if (sats == null) return '—';
    const v = Number(sats) / SATS_PER_BTC;
    if (!isFinite(v)) return '—';
    return v.toFixed(8).replace(/0+$/, '').replace(/\.$/, '') + ' BTC';
}

const UNITS = ['H/s', 'KH/s', 'MH/s', 'GH/s', 'TH/s', 'PH/s', 'EH/s'];
export function fmtHashrate(hps) {
    if (!hps || !isFinite(hps) || hps <= 0) return '0 H/s';
    let i = 0;
    let v = hps;
    while (v >= 1000 && i < UNITS.length - 1) {
        v /= 1000;
        i++;
    }
    return `${v.toFixed(2)} ${UNITS[i]}`;
}

/* Percentage of the pool. A small rig next to a large ASIC is
 * legitimately a tiny fraction — `toFixed(2)` on 0.000044 renders
 * "0.00%" and looks like a bug. Adaptive precision so every
 * contributor sees a non-zero number:
 *   >= 1%       — 2 decimals   (e.g., "54.32%")
 *   >= 0.01%    — 3 decimals   (e.g., "0.523%")
 *   >= 0.0001%  — 4 decimals   (e.g., "0.0523%")
 *   >  0        — 2 sig figs  (e.g., "4.4e-5%")
 *   == 0        — "0%"
 * Kept as a stat-lib export so both the pool-wide and per-worker
 * pages can share it. */
export function fmtPct(p) {
    if (p == null || !isFinite(p)) return '—';
    if (p === 0) return '0%';
    const abs = Math.abs(p);
    if (abs >= 1)      return p.toFixed(2)  + '%';
    if (abs >= 0.01)   return p.toFixed(3)  + '%';
    if (abs >= 0.0001) return p.toFixed(4)  + '%';
    return p.toPrecision(2) + '%';
}
