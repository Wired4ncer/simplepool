/* Read-write SQLite wrapper for the payout worker.
 *
 * The C proxy is the only writer to pps_credits.accrued_sats — every
 * accepted share INSERTs/UPSERTs with `accrued_sats = accrued_sats + delta`
 * via the writer thread. This worker is the only writer to
 * pps_credits.paid_sats, and only ever after a Thunder transaction has been
 * observed CONFIRMED — not merely broadcast. `paid` therefore means settled,
 * and `accrued - paid` is a liability the pool can still be held to.
 *
 * Both writers serialise through WAL with a generous busy_timeout, so
 * brief lock contention during the proxy's batch commit is invisible. */

import Database from 'better-sqlite3';

export function openDb(dbPath) {
    const db = new Database(dbPath, { fileMustExist: true });
    db.pragma('journal_mode = WAL');
    db.pragma('synchronous  = NORMAL');
    db.pragma('busy_timeout = 5000');
    return db;
}

/* Workers with non-zero outstanding balance, joined to the workers table
 * so we have the Thunder address (stored in workers.payout_address by
 * the C proxy on first sight). Filters out:
 *   - balances below minSats
 *   - workers with NO payout_address (shouldn't happen in pps mode but
 *     defensive against legacy rows)
 *   - workers with an in-flight payout row (handled or being handled)
 *
 * The in-flight skip is the safety net for at-most-once payout: a row
 * with txid='' means we crashed between INSERT and broadcast and might
 * have a stuck row, OR between broadcast and finalize. Either way, we
 * don't double-pay; reconciliation is manual (see listStuck). */
export function listDue(db, { minSats, limit }) {
    const rows = db.prepare(`
        SELECT  w.id              AS worker_id,
                w.name            AS worker_name,
                w.payout_address  AS thunder_address,
                c.accrued_sats    AS accrued_sats,
                c.paid_sats       AS paid_sats,
                (c.accrued_sats - c.paid_sats) AS owed_sats
        FROM    pps_credits c
        JOIN    workers     w ON w.id = c.worker_id
        WHERE   (c.accrued_sats - c.paid_sats) >= ?
          AND   w.payout_address IS NOT NULL
          AND   w.payout_address != ''
          AND   NOT EXISTS (
                  SELECT 1 FROM payouts_in_flight p
                   WHERE p.worker_id = w.id
                )
        ORDER BY owed_sats DESC
        LIMIT   ?
    `).all(Number(minSats), limit);
    return rows.map(r => ({ ...r, owed_sats: BigInt(r.owed_sats) }));
}

/* Reserve a payout slot for `workerId`. INSERTed before we touch
 * Thunder; if the worker reboots between INSERT and broadcast the row
 * stays and listDue skips this worker until an operator reconciles.
 * Returns the new row's id. */
export function beginPayout(db, workerId, sats, nowSec) {
    const r = db.prepare(`
        INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
        VALUES (?, ?, '', ?)
    `).run(workerId, Number(sats), nowSec);
    return r.lastInsertRowid;
}

/* Atomic finalize after a successful Thunder broadcast:
 *   1. record the txid on the in-flight row (audit trail)
 *   2. increment pps_credits.paid_sats
 *   3. append to the permanent payouts ledger
 *   4. delete the in-flight row
 * All four happen in one SQLite transaction — so on any crash inside
 * this window the state stays consistent (either everything applied or
 * nothing did), and the startup sweep can finish what 3/4 didn't. */
export function finalizePayout(db, rowId, workerId, sats, feeSats, txid, nowSec) {
    db.transaction(() => {
        db.prepare(`
            UPDATE payouts_in_flight SET txid = ? WHERE id = ?
        `).run(txid, rowId);
        db.prepare(`
            UPDATE pps_credits
               SET paid_sats    = paid_sats + ?,
                   last_updated = ?
             WHERE worker_id    = ?
        `).run(Number(sats), nowSec, workerId);
        db.prepare(`
            INSERT INTO payouts (worker_id, sats, fee_sats, txid, paid_at, note)
            VALUES (?, ?, ?, ?, ?, NULL)
        `).run(workerId, Number(sats), Number(feeSats), txid, nowSec);
        db.prepare(`
            DELETE FROM payouts_in_flight WHERE id = ?
        `).run(rowId);
    })();
}

/* Drop an in-flight row that we failed to broadcast — the Thunder RPC threw
 * before any txid was returned, so paid_sats was untouched and the worker is
 * safe to retry next tick. */
export function abortPayout(db, rowId) {
    db.prepare(`DELETE FROM payouts_in_flight WHERE id = ?`).run(rowId);
}

/* ---------- batched payouts ------------------------------------------------
 *
 * One Thunder transaction pays many workers, so the in-flight rows for the
 * whole batch have to appear and settle together. A per-worker finalize would
 * leave a window where some workers are credited for a transaction the others
 * are still "waiting" on — and a crash inside that window is unreconcilable,
 * because the txid is the same for all of them. */

export function beginBatch(db, items, nowSec) {
    const ins = db.prepare(`
        INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
        VALUES (?, ?, '', ?)
    `);
    return db.transaction(() =>
        items.map(i => ins.run(i.worker_id, Number(i.sats), nowSec).lastInsertRowid)
    )();
}

/* Stamp the txid onto a batch's in-flight rows, immediately after broadcast.
 *
 * This is deliberately NOT the finalize: broadcasting is not settling. The
 * rows stay in payouts_in_flight — so listDue keeps skipping these workers,
 * and nothing is credited — until the transaction is observed in a block.
 * The txid is what lets the next tick ask whether that has happened. */
export function attachBatchTxid(db, rowIds, txid) {
    const upd = db.prepare('UPDATE payouts_in_flight SET txid = ? WHERE id = ?');
    db.transaction(() => { for (const id of rowIds) upd.run(txid, id); })();
}

/* The batch currently awaiting confirmation, or null.
 *
 * Rows with txid='' are excluded: those are a crash between INSERT and
 * broadcast, where we cannot tell whether anything went out. They are
 * unresolvable from here and belong to listStuck and the operator, not to
 * the settle path — inferring either way would risk paying twice or not at
 * all. Only one batch can be outstanding, so the newest txid is the one. */
export function pendingBatch(db) {
    const rows = db.prepare(`
        SELECT  p.id AS rowId, p.worker_id, p.sats, p.txid, p.started_at,
                w.name AS worker_name
        FROM    payouts_in_flight p
        LEFT JOIN workers w ON w.id = p.worker_id
        WHERE   p.txid IS NOT NULL AND p.txid != ''
        ORDER BY p.id ASC
    `).all();
    if (rows.length === 0) return null;

    /* Settle the OLDEST batch first, and take its epoch from its own first row.
     *
     * Two bugs lived here. The txid came from the newest row while started_at
     * came from rows[0] — the oldest row OVERALL — so with two batches in
     * flight the stall-nudge timer and the "broadcast Ns ago" log both measured
     * against the wrong batch. And picking the newest txid stranded the older
     * one permanently: finalizeBatch only clears the rows it was handed, and
     * listDue excludes any worker with ANY in-flight row, so every worker in
     * the skipped batch silently stopped being paid, forever.
     *
     * Rows are ordered by id, so rows[0] belongs to the oldest batch. In the
     * normal single-batch case this is identical to what it did before; when an
     * operator has hand-attached a second txid during reconciliation, batches
     * now drain in order instead of one being abandoned. */
    const txid = rows[0].txid;
    const batch = rows.filter(r => r.txid === txid);
    return {
        txid,
        started_at: batch[0].started_at,
        rows: batch.map(r => ({ ...r, sats: BigInt(r.sats) })),
    };
}

/* Credit every worker in the batch, atomically, against one txid.
 *
 * Called only once the transaction is confirmed. The transaction pays a
 * single fee covering all recipients. It is divided evenly across the ledger
 * rows with the remainder on the first, so SUM(payouts.fee_sats) equals what
 * was actually spent — which is what any accounting query over that column
 * assumes. */
export function finalizeBatch(db, rows, totalFeeSats, txid, nowSec) {
    const n = BigInt(rows.length);
    const fee = BigInt(totalFeeSats);
    const share = n > 0n ? fee / n : 0n;
    const remainder = n > 0n ? fee - share * n : 0n;

    db.transaction(() => {
        rows.forEach((r, i) => {
            const rowFee = share + (i === 0 ? remainder : 0n);
            db.prepare('UPDATE payouts_in_flight SET txid = ? WHERE id = ?')
              .run(txid, r.rowId);
            db.prepare(`
                UPDATE pps_credits
                   SET paid_sats = paid_sats + ?, last_updated = ?
                 WHERE worker_id = ?
            `).run(Number(r.sats), nowSec, r.worker_id);
            db.prepare(`
                INSERT INTO payouts (worker_id, sats, fee_sats, txid, paid_at, note)
                VALUES (?, ?, ?, ?, ?, ?)
            `).run(r.worker_id, Number(r.sats), Number(rowFee), txid, nowSec,
                   rows.length > 1 ? `batch of ${rows.length}` : null);
            db.prepare('DELETE FROM payouts_in_flight WHERE id = ?').run(r.rowId);
        });
    })();
}

export function abortBatch(db, rowIds) {
    const del = db.prepare('DELETE FROM payouts_in_flight WHERE id = ?');
    db.transaction(() => { for (const id of rowIds) del.run(id); })();
}

/* How many payout rows are in flight at all, txid or not.
 *
 * pendingBatch() answers a narrower question — which batch can be settled —
 * and deliberately ignores rows with txid='', because those cannot be settled
 * from here. But "cannot be settled" is not "not outstanding": such a row is a
 * crash around a broadcast, so a transfer may well be sitting on the wallet's
 * UTXOs. listDue only excludes the workers named in those rows, so a DIFFERENT
 * worker coming due would start a second payout against the same inputs — and
 * that is the one-transaction-at-a-time invariant broken, from the one state
 * where we least understand what is already out there.
 *
 * So the loop blocks on this, not just on pendingBatch(). */
export function inFlightCount(db) {
    return db.prepare('SELECT COUNT(*) AS n FROM payouts_in_flight').get().n;
}

/* In-flight rows older than `staleAfterSec` that need a human.
 *
 * Only rows with txid='' qualify. Since payouts settle on confirmation
 * rather than on broadcast, a row WITH a txid is the ordinary waiting state —
 * Thunder advances a handful of times a day, so those are routinely hours old
 * and reporting them would be crying wolf on every restart. A row without one
 * is the genuinely ambiguous case: we crashed around the broadcast and cannot
 * tell whether it went out. Never auto-resolved. */
export function listStuck(db, staleAfterSec, nowSec) {
    return db.prepare(`
        SELECT  p.id, p.worker_id, w.name AS worker_name,
                w.payout_address AS thunder_address,
                p.sats, p.txid, p.started_at
        FROM    payouts_in_flight p
        JOIN    workers           w ON w.id = p.worker_id
        WHERE   p.started_at < ?
          AND   (p.txid IS NULL OR p.txid = '')
        ORDER BY p.started_at ASC
    `).all(nowSec - staleAfterSec);
}

/* Record a broadcast attempt, successful or not, into tx_attempts.
 *
 * A failed payout previously left only a log line: the transaction that was
 * built — the thing you actually need to diagnose a rejection — was
 * discarded. This keeps it.
 *
 * Best-effort: logging must never change the outcome of the payout it is
 * describing, so this swallows its own errors. In particular it must not
 * throw on a database that predates the tx_attempts table. */
export function recordTxAttempt(db, {
    kind, status, stage = null, txid = null, rawTx = null,
    amountSats = null, feeSats = null, destination = null,
    workerId = null, error = null, detail = null,
}) {
    if (!db || typeof db.prepare !== 'function') return null;
    try {
        const info = db.prepare(`
            INSERT INTO tx_attempts
                (ts, kind, status, stage, txid, raw_tx, amount_sats, fee_sats,
                 destination, worker_id, error, detail)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        `).run(Math.floor(Date.now() / 1000), kind, status, stage, txid, rawTx,
               amountSats === null ? null : Number(amountSats),
               feeSats    === null ? null : Number(feeSats),
               destination, workerId, error,
               detail === null ? null
                   : (typeof detail === 'string' ? detail : JSON.stringify(detail)));
        return info.lastInsertRowid;
    } catch {
        return null;
    }
}

/* Thunder's RPCs return transactions as JSON objects, not hex. Store a
 * canonical JSON rendering so the operator has the exact bytes that were
 * signed; fall back to a string as-is if a future version returns hex. */
export function asRawTx(tx) {
    if (tx == null) return null;
    if (typeof tx === 'string') return tx;
    try { return JSON.stringify(tx); } catch { return null; }
}
