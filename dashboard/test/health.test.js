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

import { subsidyAt, health, startHealthMonitor, currentHealth } from '../lib/health.js';
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
    /* Mined comfortably more than owed. Explicitly CONFIRMED: solvency counts
     * only blocks verified to be in the chain, so a candidate that is merely
     * recorded funds nothing. */
    db.prepare(`INSERT INTO blocks_found (ts,height,hash,reward_sats,fee_sats,status,confirmations,checked_via)
                VALUES (1700000000, 840001, 'b1', 309375000, 3125000, 'confirmed', 101, 'node')`).run();
    /* Height and value have to agree: 312,500,000 sats is the subsidy in the
     * era beginning at 840,000, and a template claiming it at height 2 — where
     * the subsidy is 50 BTC — is the exact inconsistency the block_value check
     * exists to catch. */
    db.prepare(`INSERT INTO templates (ts,height,prev_hash,bits,network_difficulty,
                    coinbase_value_sats,tx_count,tx_fees_sats,source,cb_spendable,
                    cb_op_returns,longpoll,rate_sats_per_diff)
                VALUES (1700000000, 840002, 'aa', '1a', 111157.455, 312500000, 1, 0,
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

/* Replace the confirmed block with one in some other state. Each of these is
 * a candidate that pays nothing, so each must leave the pool insolvent. */
const withBlockStatus = (status) => {
    const db = makeDb();
    db.prepare('UPDATE blocks_found SET status = ?').run(status);
    return db;
};

test('a healthy pool reports nothing', () => {
    const h = health(makeDb());
    assert.equal(h.ok, true, failing(h).join(','));
    assert.deepEqual(failing(h), []);
    assert.deepEqual(h.unavailable.map(c => c.id), []);
});

/* The bug this whole column exists for. submitblock refuses stale, duplicate
 * and high-hash candidates routinely — on a low-difficulty chain almost every
 * one of them — and each refusal used to be summed as pool revenue, so the
 * solvency guard reported healthy no matter how much was owed. */
test('a rejected candidate funds nothing', () => {
    const h = health(withBlockStatus('rejected'));
    assert.ok(failing(h).includes('margin'));
});

test('an orphaned candidate funds nothing', () => {
    const h = health(withBlockStatus('orphaned'));
    assert.ok(failing(h).includes('margin'));
});

/* Pending is not a transient here: against a backend that serves only
 * getblocktemplate and submitblock there may be nothing able to verify a
 * block for some time. Counting it optimistically would reintroduce exactly
 * the bug, so it has to count as nothing. */
test('an unverified candidate funds nothing', () => {
    const h = health(withBlockStatus('pending'));
    assert.ok(failing(h).includes('margin'));
});

test('a pool whose candidates never reach the chain is caught', () => {
    const db = makeDb();
    /* Keep the confirmed block so solvency stays green and only the orphan
     * rate can fire — otherwise the assertion proves nothing. */
    for (let i = 0; i < 20; i++) {
        db.prepare(`INSERT INTO blocks_found (ts,height,hash,reward_sats,fee_sats,status)
                    VALUES (?, ?, ?, 0, 0, 'orphaned')`)
          .run(1700000100 + i, 100 + i, `orphan${i}`);
    }
    const h = health(db);
    assert.ok(failing(h).includes('orphan_rate'));
});

/* A pool that has found nothing yet has not failed at anything. */
test('no settled candidates is not an orphan-rate failure', () => {
    const db = makeDb();
    db.prepare('DELETE FROM blocks_found').run();
    const h = health(db);
    assert.ok(!failing(h).includes('orphan_rate'));
});

/* An inflated block value is the silent one. On the coinbasetxn path the
 * backend supplies the coinbase, so the block stays valid and nothing
 * complains — but the same number sets the PPS rate, so every miner is
 * overpaid by the same factor and the pool owes money it never earned. */
test('a block value above the subsidy schedule is caught', () => {
    const db = makeDb();
    db.prepare('UPDATE templates SET coinbase_value_sats = ?')
      .run(312500000 * 6);
    const h = health(db);
    assert.ok(failing(h).includes('block_value'));
});

/* The other direction of the same parse: reading the coinbasetxn `fee` field
 * as BIP22-strict fees rather than the total output value leaves the block
 * looking nearly worthless. */
test('a block value far below the subsidy is caught', () => {
    const db = makeDb();
    db.prepare('UPDATE templates SET coinbase_value_sats = 1200').run();
    const h = health(db);
    assert.ok(failing(h).includes('block_value'));
});

/* Fees genuinely push the coinbase above the subsidy, and that is not a
 * fault — only a value that cannot be explained by fees is. */
test('a busy block with real fees is not flagged', () => {
    const db = makeDb();
    db.prepare(`UPDATE templates SET coinbase_value_sats = ?, tx_fees_sats = ?`)
      .run(312500000 + 40000000, 40000000);
    const h = health(db);
    assert.ok(!failing(h).includes('block_value'));
});

test('the subsidy schedule is the standard one', () => {
    assert.equal(subsidyAt(0), 50e8);
    assert.equal(subsidyAt(209999), 50e8);
    assert.equal(subsidyAt(210000), 25e8);
    assert.equal(subsidyAt(840000), 312500000);
    assert.equal(subsidyAt(210000 * 64), 0);
});

/* The check that would have caught the production blow-up before it cost
 * anything. Twenty shares of difficulty 10-29 over 19 seconds is ~20
 * difficulty/s, needing ~12,000 — against a pool_meta difficulty of 1. */
test('a difficulty too low for PPS to be fair is caught', () => {
    const db = makeDb();
    db.prepare('UPDATE pool_meta SET network_difficulty = 1').run();
    const h = health(db);
    assert.ok(failing(h).includes('pps_difficulty'));
});

test('a properly calibrated difficulty passes', () => {
    const db = makeDb();   /* fixture is mainnet difficulty */
    const h = health(db);
    assert.ok(!failing(h).includes('pps_difficulty'));
});

/* Solo pays from the block's own coinbase, so there is no rate to be unfair. */
test('solo mode is not judged on PPS difficulty', () => {
    const db = makeDb();
    db.prepare("UPDATE pool_meta SET pool_mode='solo', network_difficulty=1").run();
    const h = health(db);
    assert.ok(!failing(h).includes('pps_difficulty'));
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
    assert.equal(h.checks.length, 10, 'every check still reported');
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
