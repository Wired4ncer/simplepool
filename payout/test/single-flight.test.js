/* Two payout runs must never overlap.
 *
 * The timer loop and the admin `POST /tick` (the dashboard's "Trigger payout
 * now" button) both call runOnce(). A run reads the ledger synchronously and
 * then awaits Thunder RPCs, so a second run that starts during one of those
 * awaits sees the very same snapshot:
 *
 *   - both see the outstanding batch, both are told it is confirmed, and both
 *     finalize it — paid_sats credited twice and two `payouts` ledger rows for
 *     one transaction. The miner silently loses that much of what they are
 *     owed, because `accrued - paid` now says it was paid.
 *   - both see the same due workers, both pass the balance check, and both
 *     broadcast — the batch paid twice.
 *
 * Two layers, and each is tested on its own:
 *   1. runOnce() is single-flight per database: concurrent calls queue.
 *      runOnceIfIdle(), which /tick uses, answers `busy` instead of queueing.
 *   2. The ledger itself refuses the second write: finalizeBatch() fails if
 *      its in-flight rows are already gone, and beginBatch() fails if any
 *      payout is already in flight. That is what still holds when the second
 *      run is in ANOTHER process (run-once.mjs beside the service).
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import Database from 'better-sqlite3';

import { runOnce, runOnceIfIdle } from '../lib/payout.js';
import { beginBatch, finalizeBatch, pendingBatch } from '../lib/db.js';

function makeDb({ owed = {}, inFlight = [] } = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-single-')), 'p.db');
    const db = new Database(file);
    db.exec(`
        CREATE TABLE workers (id INTEGER PRIMARY KEY, name TEXT, payout_address TEXT);
        CREATE TABLE pps_credits (worker_id INTEGER PRIMARY KEY, accrued_sats INTEGER,
                                  paid_sats INTEGER, last_updated INTEGER);
        CREATE TABLE payouts_in_flight (id INTEGER PRIMARY KEY AUTOINCREMENT,
                                  worker_id INTEGER, sats INTEGER, txid TEXT, started_at INTEGER);
        CREATE TABLE payouts (id INTEGER PRIMARY KEY AUTOINCREMENT, worker_id INTEGER,
                                  sats INTEGER, fee_sats INTEGER, txid TEXT,
                                  paid_at INTEGER, note TEXT);
        CREATE TABLE tx_attempts (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER,
                                  kind TEXT, status TEXT, stage TEXT, txid TEXT, raw_tx TEXT,
                                  amount_sats INTEGER, fee_sats INTEGER, destination TEXT,
                                  worker_id INTEGER, error TEXT, detail TEXT);
    `);
    let i = 0;
    for (const [name, sats] of Object.entries(owed)) {
        i++;
        db.prepare('INSERT INTO workers VALUES (?,?,?)').run(i, name, 'addr1');
        db.prepare('INSERT INTO pps_credits VALUES (?,?,0,0)').run(i, sats);
    }
    for (const f of inFlight) {
        db.prepare(`INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
                    VALUES (?,?,?,?)`).run(f.worker_id, f.sats, f.txid, f.started_at ?? 1000);
    }
    return db;
}

/* Every RPC really yields — a few ms, like a network round trip — which is
 * the window a second run lands in. */
const rpc = v => new Promise(r => setTimeout(() => r(v), 5));

function thunderStub({ confirmed = [] } = {}) {
    const calls = { transfers: 0, getTx: 0 };
    const sent = new Set();     /* broadcast here: in the mempool, unconfirmed */
    return {
        calls,
        async getTransaction(txid) {
            calls.getTx++;
            return rpc(confirmed.includes(txid) ? { known: true, confirmed: true, blockHash: 'bh' }
                     : sent.has(txid)           ? { known: true, confirmed: false, blockHash: null }
                     :                            { known: false, confirmed: false, blockHash: null });
        },
        async walletUtxos() { return rpc({ ok: true, utxos: [] }); },
        async mempool()     { return rpc({ ok: true, count: 0 }); },
        async balance()     { return rpc({ available_sats: String(10n ** 12n) }); },
        async mine()        { return rpc({ parked: true, completed: false }); },
        async transferBatchDetailed(recipients) {
            calls.transfers++;
            sent.add(`tx${calls.transfers}`);
            return rpc({ txid: `tx${calls.transfers}`, unsigned: {}, signed: {},
                         recipients: recipients.length });
        },
    };
}

const quietLog = { info() {}, warn() {}, error() {}, debug() {} };
const cfg = { minSats: 10000n, maxPerTick: 50, dryRun: false,
              nudgeMine: true, nudgeIntervalMs: 120000, nudgeStallSec: 300 };

const paid = (db, id) =>
    db.prepare('SELECT paid_sats FROM pps_credits WHERE worker_id = ?').get(id).paid_sats;
const ledgerRows = db => db.prepare('SELECT COUNT(*) n FROM payouts').get().n;
const inFlightRows = db => db.prepare('SELECT COUNT(*) n FROM payouts_in_flight').get().n;

/* ---------- runOnce is single-flight -------------------------------------- */

test('a /tick landing mid-settle does not credit the batch twice', async () => {
    const db = makeDb({
        owed: { rig1: 5_000_000, rig2: 6_000_000 },
        inFlight: [{ worker_id: 1, sats: 5_000_000, txid: 'tx0' },
                   { worker_id: 2, sats: 6_000_000, txid: 'tx0' }],
    });
    const thunder = thunderStub({ confirmed: ['tx0'] });
    const ctx = { db, thunder, cfg };

    /* The timer loop's run and the admin /tick's run, overlapping. */
    const [a, b] = await Promise.all([runOnce(ctx, quietLog), runOnce(ctx, quietLog)]);

    assert.equal(paid(db, 1), 5_000_000, 'credited exactly once');
    assert.equal(paid(db, 2), 6_000_000, 'credited exactly once');
    assert.equal(ledgerRows(db), 2, 'one ledger row per worker, not two');
    assert.equal(inFlightRows(db), 0);
    assert.equal((a.settled ?? 0) + (b.settled ?? 0), 2, 'exactly one run settled the batch');
    /* And it was the lock that kept them apart, not the ledger's fence: the
     * second run started after the first finished, found nothing to settle,
     * and never asked Thunder about tx0. */
    assert.equal(thunder.calls.getTx, 1, 'the runs were serialised, not raced');
});

test('two overlapping runs broadcast the due workers once, not twice', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000 } });
    const thunder = thunderStub();
    const ctx = { db, thunder, cfg };

    const rs = await Promise.all([runOnce(ctx, quietLog), runOnce(ctx, quietLog)]);

    assert.equal(thunder.calls.transfers, 1, 'one transfer for one set of debts');
    /* Serialised, the second run waits on tx1 rather than racing beginBatch. */
    assert.deepEqual(rs.map(r => r.reason ?? null), [null, 'unconfirmed']);
    assert.equal(inFlightRows(db), 2, 'one in-flight row per worker');
    assert.equal(pendingBatch(db).txid, 'tx1');
});

test('the lock is per ledger, not per ctx object', async () => {
    /* index.js hands the loop and the admin server separate ctx objects over
     * the same db handle; serialising on ctx would serialise nothing. */
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = thunderStub();

    await Promise.all([runOnce({ db, thunder, cfg }, quietLog),
                       runOnce({ db, thunder, cfg }, quietLog)]);

    assert.equal(thunder.calls.transfers, 1);
});

test('runOnceIfIdle answers busy while a run is in progress, and runs when idle', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = thunderStub();
    const ctx = { db, thunder, cfg };

    const loopRun = runOnce(ctx, quietLog);
    const tick = await runOnceIfIdle(ctx, quietLog);
    assert.equal(tick.busy, true);
    assert.equal(tick.attempted, 0);
    await loopRun;
    assert.equal(thunder.calls.transfers, 1);

    /* Idle again: the button does a real run (here: waiting on tx1). */
    const again = await runOnceIfIdle(ctx, quietLog);
    assert.notEqual(again.busy, true);
    assert.equal(again.waiting_on, 'tx1');
});

test('a run that throws does not wedge the lock', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = thunderStub();
    thunder.mempool = async () => { await rpc(); throw new Error('boom'); };
    const ctx = { db, thunder, cfg };

    await assert.rejects(runOnce(ctx, quietLog), /boom/);
    thunder.mempool = async () => rpc({ ok: true, count: 0 });
    const r = await runOnceIfIdle(ctx, quietLog);
    assert.notEqual(r.busy, true);
    assert.equal(r.broadcast, 1);
});

/* ---------- the ledger fences itself -------------------------------------- */

test('finalizing the same batch twice fails the second time and credits nothing', () => {
    const db = makeDb({
        owed: { rig1: 5_000_000, rig2: 6_000_000 },
        inFlight: [{ worker_id: 1, sats: 5_000_000, txid: 'tx0' },
                   { worker_id: 2, sats: 6_000_000, txid: 'tx0' }],
    });
    /* Both runs read the batch before either finalized it. */
    const first  = pendingBatch(db);
    const second = pendingBatch(db);

    finalizeBatch(db, first.rows, 100n, first.txid, 2000);
    assert.throws(() => finalizeBatch(db, second.rows, 100n, second.txid, 2001),
                  e => e.code === 'PAYOUT_LOST_RACE');

    assert.equal(paid(db, 1), 5_000_000);
    assert.equal(paid(db, 2), 6_000_000);
    assert.equal(ledgerRows(db), 2);
});

test('a finalize that loses one row rolls back the whole batch', () => {
    /* Whole-batch atomicity holds for the fence too: no worker in the batch is
     * credited if any of its rows is already gone. */
    const db = makeDb({
        owed: { rig1: 5_000_000, rig2: 6_000_000 },
        inFlight: [{ worker_id: 1, sats: 5_000_000, txid: 'tx0' },
                   { worker_id: 2, sats: 6_000_000, txid: 'tx0' }],
    });
    const batch = pendingBatch(db);
    db.prepare('DELETE FROM payouts_in_flight WHERE id = ?').run(batch.rows[1].rowId);

    assert.throws(() => finalizeBatch(db, batch.rows, 100n, batch.txid, 2000),
                  e => e.code === 'PAYOUT_LOST_RACE');
    assert.equal(paid(db, 1), 0, 'rig1 not credited either');
    assert.equal(ledgerRows(db), 0);
    assert.equal(inFlightRows(db), 1, 'rig1\'s row is still in flight');
});

test('beginBatch refuses while any payout is already in flight', () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000 } });
    beginBatch(db, [{ worker_id: 1, sats: 5_000_000n }], 1000);

    /* The same worker again — the double broadcast. */
    assert.throws(() => beginBatch(db, [{ worker_id: 1, sats: 5_000_000n }], 1001),
                  e => e.code === 'PAYOUT_LOST_RACE');
    /* A different worker — a second transaction against the same UTXOs. */
    assert.throws(() => beginBatch(db, [{ worker_id: 2, sats: 6_000_000n }], 1001),
                  e => e.code === 'PAYOUT_LOST_RACE');
    assert.equal(inFlightRows(db), 1);
});

/* ---------- a second process --------------------------------------------- */

/* run-once.mjs beside the service: a second connection to the same file, which
 * the in-process lock cannot see. The ledger's fence has to hold on its own,
 * and the run that loses must neither throw nor write. */
test('a run on another connection that loses the settle race credits nothing and does not throw', async () => {
    const db = makeDb({
        owed: { rig1: 5_000_000 },
        inFlight: [{ worker_id: 1, sats: 5_000_000, txid: 'tx0' }],
    });
    const other = new Database(db.name);
    const thunder = thunderStub({ confirmed: ['tx0'] });

    const rs = await Promise.all([runOnce({ db, thunder, cfg }, quietLog),
                                  runOnce({ db: other, thunder, cfg }, quietLog)]);

    assert.equal(thunder.calls.getTx, 2, 'both really did race');
    assert.equal(paid(db, 1), 5_000_000, 'credited exactly once');
    assert.equal(ledgerRows(db), 1);
    assert.equal(rs.reduce((a, r) => a + (r.settled ?? 0), 0), 1);
});

test('a run on another connection that loses the begin race broadcasts nothing', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const other = new Database(db.name);
    const thunder = thunderStub();

    const rs = await Promise.all([runOnce({ db, thunder, cfg }, quietLog),
                                  runOnce({ db: other, thunder, cfg }, quietLog)]);

    assert.equal(thunder.calls.transfers, 1);
    assert.equal(inFlightRows(db), 1);
    assert.deepEqual(rs.map(r => r.reason ?? null).sort(), ['lost-race', null].sort());
});
