/* Health checks.
 *
 * Every one of these conditions was, at some point, only discoverable by
 * someone deciding to run a query. Each test builds a DB in the failing state
 * and asserts the check catches it — and, just as importantly, that a healthy
 * DB stays quiet, because a banner that cries wolf is a banner nobody reads.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import { health, startHealthMonitor, currentHealth } from '../lib/health.js';
import * as fmt from '../lib/fmt.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');

/* Derived exactly as the proxy derives it, not copied as a literal: the rate
 * check compares to 1e-9, so a rounded constant makes the "healthy" fixture
 * fail its own audit. */
const BLOCK_VALUE = 312500000;
const NET_DIFF    = 111157.455;
const GROSS       = BLOCK_VALUE / NET_DIFF;
const RATE        = GROSS * (1 - 100 / 10000);

/* A healthy DB: credits that recompute, a published rate that derives from
 * its template, an enforcer template carrying commitments, no payouts stuck. */
function makeDb() {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-health-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));

    db.prepare('INSERT INTO workers (id,name,first_seen,last_seen) VALUES (1,?,0,0)')
      .run('rig1');
    for (let i = 0; i < 20; i++) {
        const diff = 10 + i;
        db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash,
                                        credited_sats, rate_used)
                    VALUES (1, ?, ?, 0, ?, ?, ?)`)
          .run(1700000000 + i, diff, `hash${i}`,
               Math.floor(diff * RATE), RATE);
    }
    db.prepare(`INSERT INTO rate_history (ts, rate_sats_per_diff, gross_sats_per_diff,
                    fee_bps, network_difficulty, block_value_sats, rate_source)
                VALUES (1700000000, ?, ?, 100, 111157.455, 312500000, 'derived')`)
      .run(RATE, GROSS);
    /* Mined comfortably more than owed. */
    db.prepare(`INSERT INTO blocks_found (ts,height,hash,reward_sats,fee_sats)
                VALUES (1700000000, 1, 'b1', 309375000, 3125000)`).run();
    db.prepare(`INSERT INTO templates (ts,height,prev_hash,bits,network_difficulty,
                    coinbase_value_sats,tx_count,tx_fees_sats,source,cb_spendable,
                    cb_op_returns,longpoll,rate_sats_per_diff)
                VALUES (1700000000, 2, 'aa', '1a', 111157.455, 312500000, 1, 0,
                        'enforcer', 1, 3, 1, ?)`).run(RATE);
    db.prepare(`INSERT INTO pool_meta (id,pool_mode,fee_bps,rate_source,
                    rate_sats_per_diff,gross_sats_per_diff,effective_fee_bps,
                    network_difficulty,block_value_sats,credited_from,updated_at,events_lost)
                VALUES (1,'pps-classic',100,'derived',?,?,100,111157.455,312500000,
                        1700000000,1700000000,0)`)
      .run(RATE, GROSS);
    return db;
}

const failing = h => h.failing.map(c => c.id);

test('a healthy pool reports nothing', () => {
    const h = health(makeDb());
    assert.equal(h.ok, true, failing(h).join(','));
    assert.deepEqual(failing(h), []);
    assert.deepEqual(h.unavailable.map(c => c.id), []);
});

test('shares accepted but never stored are caught', () => {
    const db = makeDb();
    db.prepare('UPDATE pool_meta SET events_lost = 7 WHERE id = 1').run();
    const h = health(db);
    assert.ok(failing(h).includes('events_lost'));
    assert.match(h.failing.find(c => c.id === 'events_lost').detail, /uncredited/);
});

test('duplicate share hashes are caught', () => {
    const db = makeDb();
    db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash,
                                    credited_sats, rate_used)
                VALUES (1, 1700000099, 10, 0, 'hash0', ?, ?)`)
      .run(Math.floor(10 * RATE), RATE);
    assert.ok(failing(health(db)).includes('duplicate_shares'));
});

/* The bug this check would have had: COUNT(*) counts NULL hashes that
 * COUNT(DISTINCT ...) ignores, reporting every legacy row as a duplicate. */
test('legacy shares with no hash are not mistaken for duplicates', () => {
    const db = makeDb();
    for (let i = 0; i < 5; i++) {
        db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash,
                                        credited_sats, rate_used)
                    VALUES (1, ?, 10, 0, NULL, 0, 0)`).run(1700001000 + i);
    }
    const h = health(db);
    assert.ok(!failing(h).includes('duplicate_shares'), failing(h).join(','));
});

test('a credit that does not equal difficulty x rate_used is caught', () => {
    const db = makeDb();
    db.prepare('UPDATE shares SET credited_sats = credited_sats + 1 WHERE id = 1').run();
    assert.ok(failing(health(db)).includes('ledger'));
});

test('insolvency is caught', () => {
    const db = makeDb();
    db.prepare('UPDATE blocks_found SET reward_sats = 1, fee_sats = 0').run();
    const h = health(db);
    assert.ok(failing(h).includes('margin'));
    assert.match(h.failing.find(c => c.id === 'margin').detail, /more than mined/);
});

test('an in-flight payout with no txid is caught', () => {
    const db = makeDb();
    db.prepare(`INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
                VALUES (1, 500, '', ?)`).run(Math.floor(Date.now() / 1000));
    const h = health(db);
    assert.ok(failing(h).includes('payout_ambiguous'));
    assert.match(h.failing.find(c => c.id === 'payout_ambiguous').detail, /reconciliation/);
});

/* A payout waiting on a Thunder block is the ordinary resting state — the
 * whole reason the original "why is this in flight" question came up. It must
 * not light the banner. */
test('a payout settling normally is not a failure', () => {
    const db = makeDb();
    db.prepare(`INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
                VALUES (1, 500, 'abc', ?)`).run(Math.floor(Date.now() / 1000) - 600);
    const h = health(db);
    assert.equal(h.ok, true, failing(h).join(','));
});

test('a payout stalled for over an hour is caught', () => {
    const db = makeDb();
    db.prepare(`INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
                VALUES (1, 500, 'abc', ?)`).run(Math.floor(Date.now() / 1000) - 7200);
    assert.ok(failing(health(db)).includes('payout_stalled'));
});

test('mining templates that carry no sidechain commitments is caught', () => {
    const db = makeDb();
    db.prepare(`INSERT INTO templates (ts,height,prev_hash,bits,network_difficulty,
                    coinbase_value_sats,tx_count,tx_fees_sats,source,cb_spendable,
                    cb_op_returns,longpoll,rate_sats_per_diff)
                VALUES (1700000100, 3, 'bb', '1a', 111157.455, 312500000, 1, 0,
                        'bitcoind', 0, 0, 1, ?)`).run(RATE);
    const h = health(db);
    assert.ok(failing(h).includes('template_commitments'));
    assert.match(h.failing.find(c => c.id === 'template_commitments').detail, /merge-mine/);
});

/* An enforcer template with only the witness commitment is the subtler form
 * of the same failure — blocks valid, miners paid, no sidechain mineable. */
test('an enforcer template with only a witness commitment is caught', () => {
    const db = makeDb();
    db.prepare(`INSERT INTO templates (ts,height,prev_hash,bits,network_difficulty,
                    coinbase_value_sats,tx_count,tx_fees_sats,source,cb_spendable,
                    cb_op_returns,longpoll,rate_sats_per_diff)
                VALUES (1700000100, 3, 'bb', '1a', 111157.455, 312500000, 1, 0,
                        'enforcer', 1, 1, 1, ?)`).run(RATE);
    assert.ok(failing(health(db)).includes('template_commitments'));
});

/* A check that cannot run must not read as a pass, and must not take the
 * page down either. */
test('a DB missing a table degrades to unavailable, not to healthy', () => {
    const db = makeDb();
    db.exec('DROP TABLE payouts_in_flight');
    const h = health(db);
    assert.ok(h.unavailable.some(c => c.id === 'payout_ambiguous'));
    assert.equal(h.checks.length, 7, 'every check still reported');
    /* The assertion this test was missing, and the reason the bug shipped:
     * the comment above always said "must not read as a pass", but nothing
     * checked it, so ok stayed true and /health answered 200. */
    assert.equal(h.ok, false, 'a check that crashed must not leave ok true');
    assert.ok(h.failing.some(c => c.id === 'payout_ambiguous'));
});

/* The other half of the distinction: "not applicable yet" IS a pass. A fresh
 * deploy with no templates recorded must not page anyone. */
test('a check that is merely not-applicable-yet stays healthy', () => {
    const db = makeDb();
    /* The fixture seeds a template, so clear it: this is the fresh-deploy
     * state, where template_commitments has nothing to judge yet. */
    db.exec('DELETE FROM templates');
    const h = health(db);
    const tc = h.checks.find(c => c.id === 'template_commitments');
    assert.ok(tc.unavailable, 'no templates yet -> unavailable');
    assert.equal(tc.ok, true, 'but still a pass, not a failure');
    assert.equal(h.ok, true, 'a fresh deploy must not page anyone');
    assert.equal(h.failing.length, 0);
});

test('no DB handle is not healthy', () => {
    const h = health(null);
    assert.equal(h.ok, false);
    assert.equal(h.db_ready, false);
});

/* ---------- the snapshot the pages actually read -------------------------- */

test('the monitor publishes a snapshot immediately, then can be stopped', () => {
    const db = makeDb();
    const stop = startHealthMonitor(db, { intervalMs: 60000 });
    const h = currentHealth();
    assert.ok(h, 'first pass runs synchronously, so page one is never blank');
    assert.equal(h.ok, true);
    assert.ok(h.checked_at > 0);
    assert.equal(typeof h.took_ms, 'number');
    stop();
});

/* ---------- rendering ----------------------------------------------------- */

const render = (view, locals) =>
    ejs.renderFile(path.join(VIEWS, view), { ...fmt.all, ...locals },
                   { views: [VIEWS] });

test('the banner renders the failing checks', async () => {
    const db = makeDb();
    db.prepare('UPDATE pool_meta SET events_lost = 3 WHERE id = 1').run();
    const html = await render('partial/health-banner.ejs', { health: health(db) });
    assert.match(html, /health check.*failing/);
    assert.match(html, /Shares accepted but never stored/);
});

test('the banner is silent on a healthy pool', async () => {
    const html = await render('partial/health-banner.ejs', { health: health(makeDb()) });
    assert.equal(html.trim(), '', 'no banner at all when everything passes');
});

test('the banner renders before the first pass without throwing', async () => {
    const html = await render('partial/health-banner.ejs', { health: null });
    assert.equal(html.trim(), '');
});
