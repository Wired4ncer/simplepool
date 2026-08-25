/* Block-withholding statistical audit.
 *
 * Every accepted share at difficulty D should land as a block with
 * probability D/N (N = network difficulty). Over a long enough window a
 * worker's share of the pool's accrued difficulty should match its
 * share of blocks found. A worker that consistently submits shares but
 * NEVER finds blocks — when statistically they should have — is
 * potentially withholding solutions (a classic PPS attack: the pool
 * pays per share, the attacker keeps the block reward off-pool).
 *
 * We sidestep needing the network difficulty by comparing each worker's
 * accrued-share fraction against their block-finder fraction within the
 * same window. The audit is meaningful only once expected_solutions per
 * worker >= 5; below that, randomness dominates.
 *
 * Suspicion metric (z-score under Poisson assumption):
 *     z = (expected - actual) / sqrt(expected)
 * z >= 3 is roughly a 1-in-740 false-positive rate under honest mining.
 *
 * Run:
 *     PAYOUT_DB_PATH=../data/shares.db node audit.js
 *     PAYOUT_DB_PATH=../data/shares.db node audit.js --window-hours 168
 *     PAYOUT_DB_PATH=../data/shares.db node audit.js --json
 */

import { openDb } from './lib/db.js';

function parseArgs(argv) {
    const out = { windowHours: 24, json: false, zThreshold: 3.0 };
    for (let i = 2; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--window-hours') out.windowHours = parseFloat(argv[++i]);
        else if (a === '--z-threshold') out.zThreshold = parseFloat(argv[++i]);
        else if (a === '--json') out.json = true;
        else if (a === '-h' || a === '--help') {
            console.log('usage: audit.js [--window-hours N] [--z-threshold Z] [--json]');
            process.exit(0);
        } else {
            console.error(`unknown arg: ${a}`);
            process.exit(2);
        }
    }
    return out;
}

export function computeAudit(db, { windowHours, zThreshold }) {
    const now = Math.floor(Date.now() / 1000);
    const since = now - Math.round(windowHours * 3600);

    /* Pool-wide totals over the window.
     *
     * Counted from shares.is_block, and DELIBERATELY not filtered to
     * blocks_found.status='confirmed' the way the dashboard's block counts
     * are. The question here is whether a miner is quietly discarding the
     * submission that happens to solve a block — so what matters is what they
     * submitted, not what the chain later did with it. A miner whose solution
     * is refused by the node or reorged out has withheld nothing, and marking
     * them down for it would flag honest miners on exactly the low-difficulty
     * chains where orphans are routine.
     *
     * Both sides of the ratio come from the same column, so the statistic
     * stays internally consistent. It is called `solutions` rather than
     * `blocks` because it is not the same number the dashboard reports, and
     * an operator comparing the two should see that immediately. */
    const total = db.prepare(`
        SELECT
            COALESCE(SUM(difficulty), 0) AS accrued_diff,
            COUNT(*) FILTER (WHERE is_block = 1) AS solutions_found
        FROM shares
        WHERE ts >= ?
    `).get(since);
    const poolDiff     = Number(total.accrued_diff || 0);
    const poolSolutions = Number(total.solutions_found || 0);

    /* Per-worker accrued + actual, from the same column for the same
     * reason. */
    const rows = db.prepare(`
        SELECT
            w.id   AS worker_id,
            w.name AS worker_name,
            COALESCE(SUM(s.difficulty), 0) AS accrued_diff,
            COUNT(*) FILTER (WHERE s.is_block = 1) AS actual_solutions,
            COUNT(*) AS share_count
        FROM workers w
        JOIN shares  s ON s.worker_id = w.id
        WHERE s.ts >= ?
        GROUP BY w.id
        HAVING accrued_diff > 0
    `).all(since);

    const findings = rows.map(r => {
        const accrued = Number(r.accrued_diff);
        const actual  = Number(r.actual_solutions);
        const share   = poolDiff > 0 ? accrued / poolDiff : 0;
        const expected = poolSolutions * share;
        /* z = (expected - actual) / sqrt(expected); guard against div-by-0. */
        const z = expected > 0 ? (expected - actual) / Math.sqrt(expected) : 0;
        return {
            worker_id:     r.worker_id,
            worker_name:   r.worker_name,
            share_count:   Number(r.share_count),
            accrued_diff:  accrued,
            pool_share:    share,
            expected_solutions: expected,
            actual_solutions:   actual,
            z_score:         z,
            suspicious:      expected >= 5 && z >= zThreshold,
        };
    });
    /* Sort: suspicious first, then by z descending. */
    findings.sort((a, b) =>
        (b.suspicious ? 1 : 0) - (a.suspicious ? 1 : 0) ||
        b.z_score - a.z_score);

    return {
        window_hours:  windowHours,
        since_ts:      since,
        pool: {
            accrued_diff:    poolDiff,
            solutions_found: poolSolutions,
            workers:      rows.length,
        },
        workers: findings,
    };
}

function formatReport(audit, zThreshold) {
    const lines = [];
    lines.push(`window: ${audit.window_hours}h  ` +
               `(since ${new Date(audit.since_ts * 1000).toISOString()})`);
    lines.push(`pool:   accrued=${audit.pool.accrued_diff.toFixed(2)}  ` +
               `solutions=${audit.pool.solutions_found}  ` +
               `workers=${audit.pool.workers}`);
    lines.push('');

    const suspicious = audit.workers.filter(w => w.suspicious);
    if (suspicious.length > 0) {
        lines.push(`!! ${suspicious.length} SUSPICIOUS worker(s) (z >= ${zThreshold}, expected >= 5):`);
        for (const w of suspicious) {
            lines.push(`   ${w.worker_name.padEnd(28)} ` +
                       `expected=${w.expected_solutions.toFixed(2)} ` +
                       `actual=${w.actual_solutions} ` +
                       `z=${w.z_score.toFixed(2)} ` +
                       `share=${(w.pool_share * 100).toFixed(2)}%`);
        }
        lines.push('');
    }

    lines.push('all workers:');
    lines.push('  worker                       expected  actual   z   share   shares');
    for (const w of audit.workers) {
        const mark = w.suspicious ? '!' : (w.expected_solutions < 5 ? '·' : ' ');
        lines.push(`  ${mark} ${w.worker_name.padEnd(26)} ` +
                   `${w.expected_solutions.toFixed(2).padStart(7)}   ` +
                   `${String(w.actual_solutions).padStart(5)}   ` +
                   `${w.z_score.toFixed(2).padStart(5)}  ` +
                   `${(w.pool_share * 100).toFixed(2).padStart(5)}%  ` +
                   `${w.share_count}`);
    }
    lines.push('');
    lines.push('legend: ! = suspicious, · = expected<5 (insufficient data)');
    return lines.join('\n');
}

/* CLI entry. */
if (import.meta.url === `file://${process.argv[1]}`) {
    const args = parseArgs(process.argv);
    const dbPath = process.env.PAYOUT_DB_PATH;
    if (!dbPath) {
        console.error('PAYOUT_DB_PATH is required');
        process.exit(2);
    }
    const db = openDb(dbPath);
    const audit = computeAudit(db, args);
    if (args.json) {
        console.log(JSON.stringify(audit, null, 2));
    } else {
        console.log(formatReport(audit, args.zThreshold));
    }
    db.close();
}
