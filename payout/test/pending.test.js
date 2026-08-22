/* A payout is credited when it is MINED, not when it is sent.
 *
 * Two rules are being pinned here, and they pull in opposite directions.
 *
 * 1. At most one payout in flight. Thunder selects UTXOs without excluding
 *    those already spent by transactions in its own mempool, and a transfer
 *    consumes every wallet UTXO — returning the remainder as change that is
 *    unspendable until the tx is mined. So a second transfer picks the very
 *    inputs the first one spends and is rejected:
 *
 *        mempool error: can't add transaction, utxo double spent
 *
 *    Observed in production: one payout broadcast, then four failures every
 *    thirty seconds indefinitely, because nothing in the loop knew to wait.
 *
 * 2. `paid_sats` means settled. Crediting at broadcast made `accrued - paid`
 *    understate the pool's real liability for as long as settlement took —
 *    265 BTC for over four hours on drynet3 — and left no way back if the
 *    transaction never landed.
 *
 * The tension is in telling "confirmed" from "gone". Thunder's get_transaction
 * reports a block_hash only while the transaction is recent; after the
 * sidechain moves on, a long-confirmed txid reads back exactly like one that
 * never existed. Guessing eviction from that silence would re-queue a batch
 * that was already paid, so the loop refuses to guess — see the
 * 'cannot be determined' tests, which are the ones protecting real money.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import Database from 'better-sqlite3';

import { runOnce } from '../lib/payout.js';
import { pendingBatch, listStuck } from '../lib/db.js';

/* Minimal schema — just the tables the payout loop touches. */
function makeDb({ inFlight = [], owed = {} } = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-payout-')), 'p.db');
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
        db.prepare('INSERT INTO workers VALUES (?,?,?)').run(i, name, 'addr' + i);
        db.prepare('INSERT INTO pps_credits VALUES (?,?,0,0)').run(i, sats);
    }
    for (const f of inFlight) {
        db.prepare(`INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
                    VALUES (?,?,?,?)`).run(f.worker_id ?? 1, f.sats ?? 100,
                                           f.txid ?? '', f.started_at ?? 1000);
    }
    return db;
}

/* Thunder stub.
 *   txState  txid -> { known, confirmed }   what get_transaction reports
 *   utxoTxids                               txids the wallet UTXO set derives
 *                                           from — the durable confirmation
 *                                           signal, independent of txState */
function thunderStub({ txState = {}, utxoTxids = [], balance = 10n ** 12n,
                       walletOk = true } = {}) {
    const calls = { transfers: 0, getTx: 0, utxos: 0, mines: 0 };
    return {
        calls,
        /* Mutable so a test can advance the chain between ticks. */
        txState,
        utxoTxids,
        walletOk,
        async balance() { return { available_sats: String(balance), total_sats: String(balance) }; },
        async getTransaction(txid) {
            calls.getTx++;
            return this.txState[txid] ?? { known: false, confirmed: false, blockHash: null };
        },
        async walletUtxos() {
            calls.utxos++;
            if (!this.walletOk) return { ok: false, utxos: [], error: 'ECONNREFUSED' };
            return { ok: true,
                     utxos: this.utxoTxids.map(t => ({ txid: t, address: 'a', sats: 1n })) };
        },
        async mine() { calls.mines++; return { parked: true, completed: false }; },
        async transferBatchDetailed(recipients) {
            calls.transfers++;
            calls.lastBatch = recipients;
            return { txid: `tx${calls.transfers}`, unsigned: {}, signed: {},
                     recipients: recipients.length };
        },
    };
}

const quietLog = { info() {}, warn() {}, error() {}, debug() {} };
const baseCfg = { minSats: 10000n, maxPerTick: 50, dryRun: false, intervalMs: 1000,
                  nudgeMine: true, nudgeIntervalMs: 120000, nudgeStallSec: 300 };
const nowSec = () => Math.floor(Date.now() / 1000);
const cfg = baseCfg;

const credited = db => db.prepare('SELECT COUNT(*) n FROM pps_credits WHERE paid_sats > 0').get().n;
const inFlightRows = db => db.prepare('SELECT COUNT(*) n FROM payouts_in_flight').get().n;
const ledgerRows = db => db.prepare('SELECT COUNT(*) n FROM payouts').get().n;

/* ---------- broadcasting is not settling ---------------------------------- */

test('a broadcast credits nobody and leaves the batch in flight', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000 } });
    const thunder = thunderStub();

    const r = await runOnce({ db, thunder, cfg }, quietLog);

    assert.equal(thunder.calls.transfers, 1);
    assert.equal(r.broadcast, 2);
    assert.equal(r.paid, 0, 'sent is not paid');
    assert.equal(credited(db), 0, 'pps_credits.paid_sats must not move on a broadcast');
    assert.equal(ledgerRows(db), 0, 'the payouts ledger records settlements only');
    assert.equal(inFlightRows(db), 2, 'both workers stay in flight until it confirms');
    assert.equal(pendingBatch(db).txid, 'tx1');
});

test('a confirmed batch is credited on the next tick', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000 } });
    const thunder = thunderStub();
    const ctx = { db, thunder, cfg };

    await runOnce(ctx, quietLog);                       /* broadcast tx1 */
    thunder.txState.tx1 = { known: true, confirmed: true };

    const r = await runOnce(ctx, quietLog);             /* settle tx1 */
    assert.equal(r.settled, 2);
    assert.equal(credited(db), 2);
    assert.equal(ledgerRows(db), 2);
    assert.equal(inFlightRows(db), 0);
    assert.equal(db.prepare('SELECT COUNT(DISTINCT txid) n FROM payouts').get().n, 1,
                 'one batch, one txid across every ledger row');
});

test('each worker is credited exactly what was in flight, once', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000 } });
    const thunder = thunderStub();
    const ctx = { db, thunder, cfg };

    await runOnce(ctx, quietLog);
    thunder.txState.tx1 = { known: true, confirmed: true };
    await runOnce(ctx, quietLog);
    await runOnce(ctx, quietLog);   /* an extra tick must not re-credit */

    const rows = db.prepare('SELECT worker_id, paid_sats FROM pps_credits ORDER BY worker_id').all();
    assert.deepEqual(rows, [{ worker_id: 1, paid_sats: 5_000_000 },
                            { worker_id: 2, paid_sats: 6_000_000 }]);
    assert.equal(ledgerRows(db), 2, 'no duplicate ledger rows');
});

/* ---------- the durable confirmation oracle ------------------------------- */

test('a wallet UTXO bearing the txid proves confirmation on its own', async () => {
    /* The case that makes settle-on-confirm workable at all. get_transaction
     * has already forgotten tx1 — it reads as never-existed — but the wallet
     * holds a UTXO created by it, and Thunder admits only confirmed UTXOs.
     * Without this the batch would be undeterminable and the pool would
     * halt every time settlement outlived the RPC's memory. */
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = thunderStub();
    const ctx = { db, thunder, cfg };

    await runOnce(ctx, quietLog);
    thunder.txState = {};              /* get_transaction has forgotten it */
    thunder.utxoTxids = ['tx1'];       /* but its change is in the wallet */

    const r = await runOnce(ctx, quietLog);
    assert.equal(r.settled, 1);
    assert.equal(credited(db), 1);
});

test('an unconfirmed payout blocks the tick instead of double spending', async () => {
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000 }],
        owed: { rig1: 5_000_000, rig2: 6_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: false } } });

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.attempted, 0);
    assert.equal(r.waiting_on, 'abc123');
    assert.equal(r.reason, 'unconfirmed');
    assert.equal(thunder.calls.transfers, 0, 'must not broadcast while one is pending');
    assert.equal(credited(db), 0);
});

/* ---------- refusing to guess -------------------------------------------- */

test('a txid the node has forgotten is NOT treated as evicted', async () => {
    /* The double-pay trap. A long-confirmed txid and one that never existed
     * are byte-identical from get_transaction, so re-queueing on silence
     * would pay a settled batch a second time. Halt instead. */
    const db = makeDb({
        inFlight: [{ txid: 'gone999', worker_id: 1, sats: 5_000_000 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: {}, utxoTxids: [] });

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.reason, 'undetermined');
    assert.equal(thunder.calls.transfers, 0, 'must not re-pay a batch that may have settled');
    assert.equal(credited(db), 0, 'must not credit a batch it cannot see');
    assert.equal(inFlightRows(db), 1, 'must not abandon the batch either');
});

test('an unreachable node blocks rather than guessing', async () => {
    /* "Cannot tell" must not be read as "nothing pending" — broadcasting
     * against a wallet we cannot see is how the double spend happened. */
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ walletOk: false });
    thunder.getTransaction = async () => ({ known: false, confirmed: false, error: 'ECONNREFUSED' });

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.attempted, 0);
    assert.equal(thunder.calls.transfers, 0);
    assert.equal(credited(db), 0);
});

test('a row with no txid is left alone by the settle path', async () => {
    /* Crashed between INSERT and broadcast: we cannot tell whether anything
     * went out, so it belongs to the operator, not to the loop. It must not
     * be read as a pending batch — that would block on a phantom. */
    const db = makeDb({
        inFlight: [{ txid: '', worker_id: 1, sats: 5_000_000 }],
        owed: { rig1: 5_000_000, rig2: 6_000_000 },
    });
    const thunder = thunderStub();

    assert.equal(pendingBatch(db), null);
    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.broadcast, 1, 'rig2 is still payable');
    assert.equal(thunder.calls.lastBatch.length, 1, 'rig1 stays excluded by listDue');
});

test('listStuck reports unbroadcast rows only, not ones awaiting confirmation', async () => {
    /* Waiting hours for a Thunder block is normal now. Reporting those would
     * cry wolf on every restart and bury the row that does need a human. */
    const db = makeDb({
        inFlight: [{ txid: '',      worker_id: 1, started_at: 0 },
                   { txid: 'tx777', worker_id: 2, started_at: 0 }],
        owed: { rig1: 1, rig2: 1 },
    });
    const stuck = listStuck(db, 300, 100000);
    assert.equal(stuck.length, 1);
    assert.equal(stuck[0].worker_id, 1);
});

/* ---------- nudging Thunder ---------------------------------------------- */

/* The core of the fix. Thunder's `mine` snapshots the mempool into a block
 * body BEFORE taking the miner lock, then parks that snapshot the instant the
 * lock frees. A nudge issued while waiting therefore captures a mempool that
 * predates the NEXT batch and becomes the parked request the moment this one
 * confirms — so the next batch cannot be in the block it produces, and every
 * payout costs two sidechain blocks instead of one. */
test('a batch still within its settling window does not nudge again', async () => {
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000,
                     started_at: nowSec() }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: false } } });

    await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(thunder.calls.mines, 0,
        'its request is already parked with this batch in the body; ' +
        'nudging now parks a stale body for the next one');
});

test('a stalled batch nudges to recover', async () => {
    /* If our post-broadcast request was not carried, nothing is parked and
     * nothing will re-park. After stallSec, nudge. */
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000,
                     started_at: nowSec() - 600 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: false } } });

    await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(thunder.calls.mines, 1, 'nothing else will make Thunder advance');
});

test('the stall nudge is rate-limited across ticks', async () => {
    /* Each nudge costs a mainchain BMM bid, and the tick is far faster than
     * Thunder can produce blocks. */
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000,
                     started_at: nowSec() - 600 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: false } } });
    const ctx = { db, thunder, cfg };

    await runOnce(ctx, quietLog);
    await runOnce(ctx, quietLog);
    await runOnce(ctx, quietLog);
    assert.equal(thunder.calls.mines, 1, 'once per nudgeIntervalMs, not once per tick');
});

/* The rate limiter must never suppress this one: it is the nudge whose body
 * snapshot has to contain the batch just sent. */
test('a broadcast nudges even inside the rate-limit window', async () => {
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000,
                     started_at: nowSec() - 600 }],
        owed: { rig1: 5_000_000, rig2: 6_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: true } } });
    const ctx = { db, thunder, cfg, _lastNudgeMs: Date.now() };

    const r = await runOnce(ctx, quietLog);
    assert.equal(r.settled, 1, 'the old batch cleared');
    assert.ok(r.broadcast > 0, 'and a new one went out');
    assert.equal(thunder.calls.mines, 1,
        'forced despite _lastNudgeMs being moments ago');
});

test('the nudge can be turned off', async () => {
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000,
                     started_at: nowSec() - 600 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: false } } });

    await runOnce({ db, thunder, cfg: { ...baseCfg, nudgeMine: false } }, quietLog);
    assert.equal(thunder.calls.mines, 0);
});

test('an idle pool spends no BMM bids', async () => {
    const db = makeDb({ owed: {} });
    const thunder = thunderStub();
    await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(thunder.calls.mines, 0, 'nudge only when something is waiting to settle');
});

test('a failing mine nudge does not fail the tick', async () => {
    const db = makeDb({
        inFlight: [{ txid: 'abc123', worker_id: 1, sats: 5_000_000,
                     started_at: nowSec() - 600 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: false } } });
    thunder.mine = async () => { throw new Error('no mainchain'); };

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.waiting_on, 'abc123', 'still waiting, still safe');
});

/* ---------- batching ------------------------------------------------------ */

test('every due worker goes out in ONE transaction', async () => {
    /* The whole point of batching: four workers, one broadcast, one
     * sidechain block. One tx per worker would need four. */
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000, rig3: 7_000_000, rig4: 8_000_000 } });
    const thunder = thunderStub();

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(thunder.calls.transfers, 1, 'exactly one broadcast');
    assert.equal(thunder.calls.lastBatch.length, 4, 'all four in the same tx');
    assert.equal(r.broadcast, 4);
    assert.equal(r.failed, 0);
});

test('the whole queue settles in a single confirmation', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000, rig3: 7_000_000 } });
    const thunder = thunderStub();
    const ctx = { db, thunder, cfg };

    await runOnce(ctx, quietLog);
    thunder.txState.tx1 = { known: true, confirmed: true };
    await runOnce(ctx, quietLog);

    assert.equal(db.prepare('SELECT COUNT(*) n FROM pps_credits WHERE accrued_sats > paid_sats').get().n, 0);
});

test('the batch fee sums to exactly one transaction fee', async () => {
    /* Divided across the ledger rows, so SUM(fee_sats) is what was actually
     * spent — a per-row copy of the full fee would triple-count it. */
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000, rig3: 7_000_000 } });
    const thunder = thunderStub();
    const ctx = { db, thunder, cfg };

    await runOnce(ctx, quietLog);
    thunder.txState.tx1 = { known: true, confirmed: true };
    await runOnce(ctx, quietLog);

    assert.equal(db.prepare('SELECT SUM(fee_sats) f FROM payouts').get().f, 100);
});

test('a rejected batch credits nobody and strands nobody', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000, rig3: 7_000_000 } });
    const thunder = thunderStub();
    /* The node answered and declined — a mempool rejection is the everyday
     * shape of this. Nothing was broadcast, so the rows must be released and
     * the batch retried; parking it for a human here would turn the most
     * common transient failure into an outage. The unanswered case is the
     * opposite and is covered below. */
    thunder.transferBatchDetailed = async () => {
        const e = new Error('boom'); e.stage = 'submit'; e.rpcRejected = true; throw e;
    };
    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.paid, 0);
    assert.equal(r.failed, 3);
    assert.equal(credited(db), 0);
    assert.equal(inFlightRows(db), 0,
                 'in-flight rows must be released or the workers are stuck forever');
});

test('no prior payout at all does not block a first payout', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = thunderStub();
    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.broadcast, 1);
    assert.equal(thunder.calls.getTx, 0, 'nothing to check when nothing is in flight');
});

test('pendingBatch groups by the newest txid and ignores unbroadcast rows', async () => {
    const db = makeDb({
        inFlight: [{ txid: '',    worker_id: 1 },
                   { txid: 'new', worker_id: 2 },
                   { txid: 'new', worker_id: 3 }],
        owed: { a: 1, b: 1, c: 1 },
    });
    const p = pendingBatch(db);
    assert.equal(p.txid, 'new');
    assert.equal(p.rows.length, 2);
});

/* ---- two batches in flight ---------------------------------------------
 *
 * Only one batch should ever be in flight, but an operator reconciling by hand
 * can attach a txid to a stranded batch while another is already out. The loop
 * has to drain them in order.
 *
 * pendingBatch used to take the txid from the NEWEST row and started_at from
 * the OLDEST row overall — mixing two batches into one report. Two consequences:
 * the stall timer measured against the wrong epoch, and the older batch was
 * never selected at all. Since finalizeBatch only clears the rows it is handed,
 * and listDue skips any worker holding an in-flight row, everyone in the
 * skipped batch stopped being paid permanently and silently.
 */
test('two batches in flight: the older one is reported, with its own epoch', () => {
    const db = makeDb({
        inFlight: [
            { worker_id: 1, sats: 100, txid: 'tx_old', started_at: 1000 },
            { worker_id: 2, sats: 200, txid: 'tx_old', started_at: 1005 },
            { worker_id: 3, sats: 300, txid: 'tx_new', started_at: 9000 },
        ],
    });

    const p = pendingBatch(db);

    /* Precondition: both batches really are present, so this is testing the
     * selection and not an empty table. */
    assert.equal(db.prepare('SELECT COUNT(*) n FROM payouts_in_flight').get().n, 3);

    assert.equal(p.txid, 'tx_old', 'oldest batch settles first — nothing is stranded');
    assert.equal(p.rows.length, 2, 'only the rows belonging to that txid');
    assert.deepEqual(p.rows.map(r => r.worker_id), [1, 2]);
    assert.equal(p.started_at, 1000, "epoch comes from this batch's own first row");
    /* The newest batch's row must not leak into the older batch's report. */
    assert.ok(!p.rows.some(r => r.worker_id === 3));
});

test('a single batch is unaffected by the ordering change', () => {
    const db = makeDb({
        inFlight: [
            { worker_id: 1, sats: 100, txid: 'tx_a', started_at: 4242 },
            { worker_id: 2, sats: 200, txid: 'tx_a', started_at: 4243 },
        ],
    });
    const p = pendingBatch(db);
    assert.equal(p.txid, 'tx_a');
    assert.equal(p.rows.length, 2);
    assert.equal(p.started_at, 4242);
});

/* ---------- a failed broadcast must not become a second broadcast --------- */

/* The three stages are not equivalent, and treating them as one was a
 * double-pay. create and sign are local calls that cannot have reached the
 * network; submit can have been accepted and then lost its reply, and on
 * thunder >= 0.17.1 create_transfer signs and broadcasts internally, so the
 * funds are definitely out. Releasing the in-flight rows on submit hands the
 * same batch to the next tick, which rebroadcasts as soon as the first
 * transaction confirms and frees its inputs — paying twice, every tick, until
 * the reserve is gone. */

function failingThunder(stage, { message = 'boom', txid = null,
                                 rpcRejected = false } = {}) {
    const t = thunderStub();
    t.transferBatchDetailed = async () => {
        t.calls.transfers++;
        const e = new Error(message);
        e.stage = stage;
        if (txid) e.txid = txid;
        if (rpcRejected) e.rpcRejected = true;
        throw e;
    };
    return t;
}

/* sign is local on every thunder that reaches it, and an ANSWERED create is
 * the node declining to act. Both are clean aborts. */
for (const [stage, opts] of [['sign', {}], ['create', { rpcRejected: true }]]) {
    test(`a ${stage}-stage failure is a clean abort and retries`, async () => {
        const db = makeDb({ owed: { rig1: 5_000_000 } });
        const r = await runOnce({ db, thunder: failingThunder(stage, opts), cfg }, quietLog);

        assert.equal(r.ambiguous, false, `${stage} cannot have reached the network`);
        assert.equal(inFlightRows(db), 0, 'rows are released so the next tick retries');
        assert.equal(credited(db), 0);

        /* The retry is the point: with the rows gone the worker is due again. */
        const again = await runOnce({ db, thunder: thunderStub(), cfg }, quietLog);
        assert.equal(again.broadcast, 1);
    });
}

test('an UNANSWERED create keeps the rows — 0.17.1 broadcasts inside create', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const r = await runOnce({ db, thunder: failingThunder('create'), cfg }, quietLog);

    assert.equal(r.ambiguous, true,
        'create_transfer signs and broadcasts internally on thunder >= 0.17.1, ' +
        'so an unanswered call may already be on the network');
    assert.equal(credited(db), 0, 'nothing is credited — it may never have landed');
    assert.equal(inFlightRows(db), 1, 'the row STAYS: this may already be on the network');

    const next = thunderStub();
    await runOnce({ db, thunder: next, cfg }, quietLog);
    assert.equal(next.calls.transfers, 0, 'a second broadcast here is a double payment');

    assert.equal(listStuck(db, 0, nowSec() + 1).length, 1, 'reported for an operator');
});

test('a rejected submit is a clean abort and retries', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = failingThunder('submit', { rpcRejected: true });
    const r = await runOnce({ db, thunder, cfg }, quietLog);

    assert.equal(r.ambiguous, false, 'the node answered: nothing went out');
    assert.equal(inFlightRows(db), 0);

    const next = thunderStub();
    assert.equal((await runOnce({ db, thunder: next, cfg }, quietLog)).broadcast, 1);
});

test('an UNANSWERED submit keeps the rows so the batch cannot go out twice', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const r = await runOnce({ db, thunder: failingThunder('submit'), cfg }, quietLog);

    assert.equal(r.ambiguous, true);
    assert.equal(credited(db), 0, 'nothing is credited — it may never have landed');
    assert.equal(inFlightRows(db), 1, 'the row STAYS: this may already be on the network');
    assert.equal(pendingBatch(db), null, 'no txid, so it is not a settleable batch');

    /* The next tick must not rebroadcast: listDue skips workers holding an
     * in-flight row, which is what makes the at-most-once guarantee hold. */
    const next = thunderStub();
    const again = await runOnce({ db, thunder: next, cfg }, quietLog);
    assert.equal(next.calls.transfers, 0, 'a second broadcast here is a double payment');
    assert.equal(again.attempted, 0);

    /* And it is reported for the operator rather than left silent. */
    assert.equal(listStuck(db, 0, nowSec() + 1).length, 1);
});

test('an unknown stage is treated as possibly-broadcast', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const t = thunderStub();
    t.transferBatchDetailed = async () => { throw new Error('no stage attached'); };

    const r = await runOnce({ db, thunder: t, cfg }, quietLog);

    assert.equal(r.ambiguous, true, '"where did it fail" unknown means "it may have gone out"');
    assert.equal(inFlightRows(db), 1);
});

test('a txid known at failure time is recorded for reconciliation', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = failingThunder('submit', {
        message: 'thunder already broadcast abc123 paying the full total to addr1',
        txid: 'abc123',
    });

    await runOnce({ db, thunder, cfg }, quietLog);

    const att = db.prepare(
        `SELECT status, stage, txid FROM tx_attempts ORDER BY id DESC LIMIT 1`).get();
    assert.equal(att.status, 'failed');
    assert.equal(att.stage, 'submit');
    assert.equal(att.txid, 'abc123',
        'the operator cannot reconcile a broadcast whose txid was thrown away');
    /* Still not stamped onto the in-flight row: that would let a later tick
     * settle it and credit workers this transaction did not actually pay. */
    assert.equal(pendingBatch(db), null);
});
