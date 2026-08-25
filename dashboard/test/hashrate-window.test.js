/* Hashrate is sum(difficulty) * 2^32 / elapsed, and `elapsed` is the part that
 * was wrong: every estimate divided by the nominal window regardless of how
 * much of it the shares actually covered.
 *
 * That understates exactly when someone is most likely to be looking. A pool
 * whose database was just wiped reads half its true rate for the first twelve
 * hours. A rented rig that connected ten minutes ago reads 1/144th of itself
 * on the leaderboard, so the one miner an operator most wants to see arrive is
 * the one the page hides. Both heal silently as the window fills, which is why
 * it survived this long.
 *
 * These tests pin the rule: divide by the span the shares cover, clamped to
 * the nominal window above and to MIN_SPAN_SEC below.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';

import { overview, leaderboard, worker } from '../lib/stats.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const TWO_32 = 4294967296;
const DAY = 86400;

const handleFor = db => ({ get: () => db });
const now = () => Math.floor(Date.now() / 1000);

/* Relative comparison: the estimator reads Date.now() itself, so a span can
 * land a second either side of what the test intended. */
function near(actual, expected, tolerance = 0.02) {
    const err = Math.abs(actual - expected) / expected;
    assert.ok(err <= tolerance,
        `expected ~${expected.toExponential(3)}, got ${actual.toExponential(3)} (off by ${(err * 100).toFixed(1)}%)`);
}

/* `rigs` is [name, [[secondsAgo, difficulty], ...]]. */
function makeDb(rigs) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-hr-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    const insW = db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                             VALUES (?, ?, ?, ?, ?)`);
    const insS = db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, credited_sats, rate_used)
                             VALUES (?, ?, ?, 0, 0, 0)`);
    const t = now();
    rigs.forEach(([name, shares], i) => {
        const id = i + 1;
        const oldest = shares.length ? Math.max(...shares.map(s => s[0])) : 0;
        insW.run(id, name, t - oldest, t, `bc1qtest${id}`);
        for (const [ago, diff] of shares) insS.run(id, t - ago, diff);
    });
    return db;
}

/* One hour of shares inside a 24h window is an hour's worth of hashrate, not
 * a twenty-fourth of it. */
test('a window the shares only partly cover divides by the covered part', () => {
    const shares = [];
    for (let i = 0; i < 60; i++) shares.push([3600 - i * 60, 1000]);
    const ov = overview(handleFor(makeDb([['rig1', shares]])), DAY);

    const sumDiff = 60 * 1000;
    near(ov.hashrate, (sumDiff * TWO_32) / 3600);
    near(ov.window_effective_sec, 3600);

    /* The old form. Keeping it here states the size of the error: 24x. */
    const stale = (sumDiff * TWO_32) / DAY;
    assert.ok(ov.hashrate > stale * 20, 'must not report the whole-window average');
});

/* No regression once the window is genuinely full. */
test('a window the shares fill completely is unchanged', () => {
    const shares = [];
    for (let i = 0; i < 24; i++) shares.push([DAY - i * 3600, 1000]);
    const ov = overview(handleFor(makeDb([['rig1', shares]])), DAY);

    near(ov.hashrate, (24 * 1000 * TWO_32) / DAY);
    near(ov.window_effective_sec, DAY);
});

/* The clamp. Without it one share seconds old divides by ~0. */
test('a single fresh share is damped by the minimum span, not amplified', () => {
    const ov = overview(handleFor(makeDb([['rig1', [[5, 1000]]]])), DAY);

    near(ov.hashrate, (1000 * TWO_32) / 60);
    assert.equal(ov.window_effective_sec, 60);
    assert.ok(Number.isFinite(ov.hashrate), 'must never divide by zero');
});

/* An idle pool reports nothing, rather than 0 inflated by a clamped span. */
test('no shares reports zero over the nominal window', () => {
    const ov = overview(handleFor(makeDb([['rig1', []]])), DAY);

    assert.equal(ov.hashrate, 0);
    assert.equal(ov.hashrate_5m, 0);
    assert.equal(ov.window_effective_sec, DAY);
});

/* The case that started this: a rented rig arrives mid-window. It should rank
 * for what it is doing now, not for its share of a day it was absent for. */
test('a rig that joined mid-window is ranked at the rate it is actually running', () => {
    const steady = [];
    for (let i = 0; i < 24; i++) steady.push([DAY - i * 3600, 100]);
    const arrived = [];
    for (let i = 0; i < 10; i++) arrived.push([600 - i * 60, 1000]);

    const board = leaderboard(handleFor(makeDb([['old', steady], ['new', arrived]])), DAY);
    const byName = Object.fromEntries(board.map(r => [r.name, r]));

    near(byName.new.hashrate_est, (10 * 1000 * TWO_32) / 600);
    near(byName.old.hashrate_est, (24 * 100 * TWO_32) / DAY);
    assert.ok(byName.new.hashrate_est > byName.old.hashrate_est,
        'the newcomer is hashing far harder and must read that way');
});

/* The worker page's own 24h figure goes through the same correction. */
test('the worker page uses the covered span too', () => {
    const shares = [];
    for (let i = 0; i < 30; i++) shares.push([1800 - i * 60, 500]);
    const w = worker(handleFor(makeDb([['rig1', shares]])), 'rig1', DAY);

    near(w.worker.window_hashrate, (30 * 500 * TWO_32) / 1800);
});
