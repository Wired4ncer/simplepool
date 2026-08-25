/* The worker page's "actual difficulty" column.
 *
 * It answers "how lucky was this share" by dividing difficulty 1's target by
 * the share's hash. Getting it wrong is not cosmetic: it is the one column an
 * operator can use to notice that a miner is clearing a difficulty far above
 * the one the pool assigned it, which is how a connection pinned at
 * vardiff_min gets spotted. It read 2^32 too high, which put every share in
 * the trillions and made the column useless for that.
 *
 * The cause: difficulty 1's target (0x00000000ffff0000...) already carries
 * eight leading zero nibbles of its own, so only a share's zeros BEYOND those
 * eight make it harder than difficulty 1. Counting all of them multiplied
 * every reading by 16^8.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';

import { worker } from '../lib/stats.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const NOW = Math.floor(Date.now() / 1000);
const handleFor = db => ({ get: () => db });

/* Targets whose difficulty is exact by construction: difficulty 1's target
 * divided by the difficulty wanted. */
const CASES = [
    ['00000000ffff0000000000000000000000000000000000000000000000000000', 1],
    ['0000000000ffff00000000000000000000000000000000000000000000000000', 256],
    ['00000000001f03e07ff07e003e07ff07e003e07ff07e003e07ff07e003e07ff0', 2113],
    /* Seven leading zeros, not eight: a share easier than difficulty 1. The
     * exponent goes negative here, which the old code could never produce. */
    ['00000001fffe0000000000000000000000000000000000000000000000000000', 0.5],
];

function makeDb(hashes) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-diff-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                VALUES (1, 'rig1', ?, ?, 'tb1qexample')`).run(NOW - 100, NOW);
    const ins = db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash)
                            VALUES (1, ?, 1.0, 0, ?)`);
    hashes.forEach((h, i) => ins.run(NOW - i, h));
    return db;
}

test('a share reports the difficulty it actually achieved', () => {
    const db = makeDb(CASES.map(([h]) => h));
    const { shares } = worker(handleFor(db), 'rig1');

    for (const [hash, expected] of CASES) {
        const row = shares.find(s => s.share_hash === hash);
        assert.ok(row, `share ${hash} missing`);
        /* worker() reads the top 32 bits of the hash, so it is an
         * approximation — but a close one, not one off by a factor. */
        const err = Math.abs(row.actual_diff - expected) / expected;
        assert.ok(err < 1e-6,
            `hash ${hash}: expected ~${expected}, got ${row.actual_diff}`);
    }
});

test('achieved difficulty is not inflated by 2^32', () => {
    /* The specific regression. A share of difficulty ~2113 was displayed as
     * ~9.08e12; anything in that range means diff1's own eight zero nibbles
     * are being counted again. */
    const hash = '00000000001f03e07ff07e003e07ff07e003e07ff07e003e07ff07e003e07ff0';
    const db = makeDb([hash]);
    const { shares } = worker(handleFor(db), 'rig1');

    assert.equal(shares.length, 1);
    assert.ok(shares[0].actual_diff < 1e6,
        `actual_diff ${shares[0].actual_diff} is still inflated`);
    assert.ok(Math.abs(shares[0].actual_diff - 2113) < 1,
        `expected ~2113, got ${shares[0].actual_diff}`);
});

test('a share that beats difficulty 1 reads above 1, one that misses reads below', () => {
    /* The boundary the 2^32 error hid completely: with it, even a share that
     * missed difficulty 1 read as billions. */
    const easy = '00000001fffe0000000000000000000000000000000000000000000000000000';
    const hard = '0000000000ffff00000000000000000000000000000000000000000000000000';
    const db = makeDb([easy, hard]);
    const { shares } = worker(handleFor(db), 'rig1');

    assert.ok(shares.find(s => s.share_hash === easy).actual_diff < 1);
    assert.ok(shares.find(s => s.share_hash === hard).actual_diff > 1);
});
