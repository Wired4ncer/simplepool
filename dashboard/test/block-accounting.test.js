/* Block accounting: a row in blocks_found is a CANDIDATE, not a block.
 *
 * Meeting the network target produces one. submitblock can refuse it, and the
 * chain can reorg it out — on a low-difficulty chain nearly every candidate
 * ends one of those ways. Counting them all is what let one alphanet pool
 * report 158,326 blocks and 3,072,992 BTC of rewards on a chain that had
 * advanced 19,676 blocks since its fork, and it silently disabled the
 * solvency check, which sums those rewards.
 *
 * These tests pin the rule that fixes it: only status='confirmed' counts, and
 * the rest are shown rather than hidden.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import { overview, recentBlocks, allBlocks } from '../lib/stats.js';
import { workerAudit } from '../lib/admin.js';
import * as fmt from '../lib/fmt.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');

const NOW = Math.floor(Date.now() / 1000);

/* stats.js takes a handle with .get(); admin.js unwraps the same shape. */
const handleFor = db => ({ get: () => db });

function makeDb(candidates = []) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-blocks-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                VALUES (1, 'rig1', ?, ?, 'tb1qexample')`).run(NOW - 100, NOW);
    const ins = db.prepare(`INSERT INTO blocks_found
        (ts, height, hash, finder_id, finder_address, reward_sats, fee_sats,
         status, confirmations, checked_via)
        VALUES (?, ?, ?, 1, 'tb1qexample', 309375000, 3125000, ?, ?, ?)`);
    /* Each candidate also has the share that produced it, which is where the
     * admin page's independent block counter reads from. */
    const insShare = db.prepare(`INSERT INTO shares
        (worker_id, ts, difficulty, is_block, block_hash, credited_sats, rate_used)
        VALUES (1, ?, 1000, 1, ?, 0, 0)`);
    candidates.forEach(([hash, status, confs], i) => {
        ins.run(NOW - 10 + i, 800000 + i, hash, status, confs || 0,
                status === 'pending' ? null : 'node');
        insShare.run(NOW - 10 + i, hash);
    });
    return db;
}

const ALL_FOUR = [
    ['h_confirmed', 'confirmed', 12],
    ['h_orphaned',  'orphaned',  0],
    ['h_rejected',  'rejected',  0],
    ['h_pending',   'pending',   0],
];

test('only confirmed candidates are counted as blocks', () => {
    const ov = overview(handleFor(makeDb(ALL_FOUR)));
    assert.equal(ov.blocks, 1, '24h count');
    assert.equal(ov.blocks_lifetime, 1, 'lifetime count');
});

/* An unverified candidate is the case that decides whether the guard is
 * conservative or optimistic. Against a backend serving only
 * getblocktemplate and submitblock, pending is a normal steady state — if it
 * counted, the original bug would be right back. */
test('an unverified candidate is not counted as a block', () => {
    const ov = overview(handleFor(makeDb([['h_pending', 'pending', 0]])));
    assert.equal(ov.blocks, 0);
    assert.equal(ov.blocks_lifetime, 0);
    assert.equal(ov.blocks_pending, 1);
});

test('near-misses are reported separately, not hidden', () => {
    const ov = overview(handleFor(makeDb(ALL_FOUR)));
    assert.equal(ov.blocks_orphaned, 1);
    assert.equal(ov.blocks_rejected, 1);
    assert.equal(ov.blocks_pending, 1);
});

test('a pool with nothing but rejected candidates has mined nothing', () => {
    const db = makeDb(Array.from({ length: 50 }, (_, i) =>
        [`h_${i}`, 'rejected', 0]));
    const ov = overview(handleFor(db));
    assert.equal(ov.blocks_lifetime, 0);
    assert.equal(ov.blocks_rejected, 50);
});

test('block listings carry their verdict', () => {
    const db = makeDb(ALL_FOUR);
    for (const rows of [recentBlocks(handleFor(db), 10),
                        allBlocks(handleFor(db), { limit: 10 }).rows]) {
        assert.equal(rows.length, 4, 'every candidate is still on record');
        const byHash = Object.fromEntries(rows.map(r => [r.hash, r]));
        assert.equal(byHash.h_confirmed.status, 'confirmed');
        assert.equal(byHash.h_orphaned.status, 'orphaned');
        assert.equal(byHash.h_rejected.status, 'rejected');
        assert.equal(byHash.h_pending.status, 'pending');
    }
});

/* shares.is_block is a second, independent block counter. Fixing blocks_found
 * alone would leave the admin worker page still reporting every candidate as
 * a block found by that miner. */
test('the per-worker block counter also counts confirmed only', () => {
    const audit = workerAudit(handleFor(makeDb(ALL_FOUR)), 1);
    assert.equal(audit.totals.blocks_found, 1);
    assert.equal(audit.days.reduce((n, d) => n + d.blocks, 0), 1);
});

/* is_block records what the miner did — met the network target — and must
 * stay true regardless of what the chain later decided. Only the verdict
 * beside it changes. */
test('a share that met the network target stays marked as such', () => {
    const audit = workerAudit(handleFor(makeDb(ALL_FOUR)), 1);
    assert.equal(audit.recent.length, 4);
    assert.ok(audit.recent.every(r => r.is_block === 1));
    const byHash = Object.fromEntries(audit.recent.map(r => [r.block_hash, r]));
    assert.equal(byHash.h_orphaned.block_status, 'orphaned');
    assert.equal(byHash.h_confirmed.block_status, 'confirmed');
});

/* ---------- what a miner actually sees ---------- */

const render = (view, locals) =>
    ejs.renderFile(path.join(VIEWS, view), { ...fmt.all, ...locals },
                   { views: [VIEWS] });

test('the blocks page names every verdict and pays only the confirmed one', async () => {
    const db = makeDb(ALL_FOUR);
    const { rows, next_before } = allBlocks(handleFor(db), { limit: 10 });
    const html = await render('blocks.ejs', {
        rows, next_before,
        fmtBtc: (s) => (s == null ? '—' : (s / 1e8).toFixed(4) + ' BTC'),
        shortAddr: a => a || '—',
        shortHash: h => h.slice(0, 8),
        health: { ok: true, failing: [] },
        pool: null,
    });
    assert.match(html, /in chain/);
    assert.match(html, /orphaned/);
    assert.match(html, /rejected/);
    assert.match(html, /unverified/);
    /* Exactly one row shows a reward: the other three earned nothing, and
     * printing their would-be reward is how 3,072,992 BTC got claimed. */
    assert.equal((html.match(/BTC/g) || []).length, 2, 'reward + fee, once');
});

test('a status never renders blank, even on a row written before the column', async () => {
    const html = await render('blocks.ejs', {
        rows: [{ ts: NOW, height: 800000, hash: 'a'.repeat(64),
                 finder: 'rig1', finder_address: 'tb1q', reward_sats: 1,
                 fee_sats: 0, status: null, confirmations: 0, checked_via: null }],
        next_before: null,
        fmtBtc: () => '—', shortAddr: a => a, shortHash: h => h.slice(0, 8),
        health: { ok: true, failing: [] }, pool: null,
    });
    assert.match(html, /unverified/);
});
