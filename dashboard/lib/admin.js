// PPS pool operator queries + Thunder reserve balance probe.
//
// All read-only against the same shares.db handle the public dashboard
// uses. Thunder is queried over its JSON-RPC HTTP endpoint (default
// http://127.0.0.1:6009) with a short timeout — we never block a
// request on a slow / down Thunder node.

import { enforcerRpc } from './enforcer.js';
import { poolMeta, rateVerification } from './stats.js';

function unwrap(handle) {
    if (typeof handle?.get === 'function') return handle.get();
    return handle;
}

/* Pool-wide totals across pps_credits. `owed` is the outstanding sat
 * balance the payout worker is responsible for draining. */
export function poolTotals(handle) {
    const db = unwrap(handle);
    if (!db) return { accrued: 0, paid: 0, owed: 0, workers: 0 };
    const r = db.prepare(`
        SELECT COALESCE(SUM(accrued_sats), 0) AS accrued,
               COALESCE(SUM(paid_sats),    0) AS paid,
               COUNT(*)                       AS workers
        FROM   pps_credits
    `).get();
    return {
        accrued: Number(r.accrued),
        paid:    Number(r.paid),
        owed:    Number(r.accrued) - Number(r.paid),
        workers: Number(r.workers),
    };
}

/* Per-worker balances, sorted by outstanding first. `thunder_address`
 * is what the payout worker will target. */
export function perWorkerBalances(handle) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT  w.id                          AS worker_id,
                w.name                        AS worker_name,
                w.payout_address              AS thunder_address,
                c.accrued_sats                AS accrued,
                c.paid_sats                   AS paid,
                (c.accrued_sats - c.paid_sats) AS owed,
                c.last_updated                AS last_updated
        FROM    pps_credits c
        JOIN    workers     w ON w.id = c.worker_id
        ORDER BY owed DESC
    `).all().map(r => ({
        ...r,
        accrued:      Number(r.accrued),
        paid:         Number(r.paid),
        owed:         Number(r.owed),
        last_updated: Number(r.last_updated),
    }));
}

/* Current in-flight payouts.
 *
 * A row WITH a txid is a broadcast payout waiting to be mined — routine, and
 * it clears itself when the worker sees the transaction confirm. Since
 * Thunder advances only a few times a day these are often hours old, which is
 * not by itself a problem.
 *
 * A row with an EMPTY txid is the one that needs a human: the worker crashed
 * between INSERT and the Thunder RPC, so we cannot tell whether the broadcast
 * went out. Never auto-resolved — see payout/README.md for the runbook. */
export function inFlight(handle) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT  p.id, p.worker_id, w.name AS worker_name,
                w.payout_address AS thunder_address,
                p.sats, p.txid, p.started_at
        FROM    payouts_in_flight p
        JOIN    workers           w ON w.id = p.worker_id
        ORDER BY p.started_at ASC
    `).all().map(r => ({
        ...r,
        sats: Number(r.sats),
        started_at: Number(r.started_at),
    }));
}

/* Per-worker audit — "why is my accrued balance N sats?" answered with
 * the stored credits, a cross-check, per-day rollup, and the last N shares.
 *
 * The C proxy credits each accepted share FLOOR(difficulty * rate) and
 * writes that amount to shares.credited_sats. The audit sums that column
 * rather than recomputing it: the rate is derived per template and moves
 * with network difficulty, so re-deriving history against a current rate
 * would show a false mismatch after every difficulty change. Truncation is
 * also per-share, so only the stored per-row values reconstruct the ledger
 * exactly.
 *
 * Rate metadata is read from pool_meta — written by the proxy itself — so
 * this view can never disagree with the process that did the crediting. */
export function workerAudit(handle, workerId, { recentLimit = 100, dayLimit = 30 } = {}) {
    const db = unwrap(handle);
    if (!db) return null;
    const worker = db.prepare(`
        SELECT w.id, w.name, w.payout_address, w.first_seen, w.last_seen,
               c.accrued_sats, c.paid_sats, c.last_updated
        FROM   workers w
        LEFT JOIN pps_credits c ON c.worker_id = w.id
        WHERE  w.id = ?
    `).get(workerId);
    if (!worker) return null;

    /* Rate provenance from the proxy itself — never from dashboard config. */
    const meta = poolMeta(db);
    /* The one part of this page that checks rather than reports. */
    const verification = rateVerification(db, workerId);

    const totals = db.prepare(`
        SELECT COUNT(*)                              AS share_count,
               COALESCE(SUM(difficulty), 0)          AS sum_difficulty,
               COALESCE(SUM(credited_sats), 0)       AS accrued_computed,
               MIN(ts)                               AS first_ts,
               MAX(ts)                               AS last_ts,
               COUNT(*) FILTER (WHERE is_block = 1 AND EXISTS (
                   SELECT 1 FROM blocks_found b
                    WHERE b.hash = shares.block_hash
                      AND b.status = 'confirmed'))  AS blocks_found
        FROM   shares
        WHERE  worker_id = ? AND is_block IS NOT NULL
    `).get(workerId);

    /* Day-level rollup: (day, shares, sum_diff, sats_credited). Only
     * days with activity, most-recent first. */
    const days = db.prepare(`
        SELECT DATE(ts, 'unixepoch') AS day,
               COUNT(*)              AS shares,
               SUM(difficulty)       AS sum_diff,
               SUM(credited_sats)    AS accrued_delta,
               COUNT(*) FILTER (WHERE is_block = 1 AND EXISTS (
                   SELECT 1 FROM blocks_found b
                    WHERE b.hash = shares.block_hash
                      AND b.status = 'confirmed')) AS blocks
        FROM   shares
        WHERE  worker_id = ?
        GROUP  BY day
        ORDER  BY day DESC
        LIMIT  ?
    `).all(workerId, dayLimit);

    /* Most-recent shares, cheapest to derive running_accrued client-side.
     *
     * is_block means the hash met the network target — it is what the miner
     * did, and it stays true whatever the chain later decided. Whether that
     * became a block is a different question, answered by block_status, and
     * conflating the two is why this page reported a worker as having found
     * thousands of blocks. */
    const recent = db.prepare(`
        SELECT s.id, s.ts, s.difficulty, s.is_block, s.block_hash,
               s.credited_sats AS credit_sats,
               b.status AS block_status
        FROM   shares s
        LEFT   JOIN blocks_found b ON b.hash = s.block_hash
        WHERE  s.worker_id = ?
        ORDER  BY s.ts DESC
        LIMIT  ?
    `).all(workerId, recentLimit);

    return {
        worker: {
            id: Number(worker.id),
            name: worker.name,
            thunder_address: worker.payout_address,
            first_seen: Number(worker.first_seen),
            last_seen:  Number(worker.last_seen),
        },
        rate: meta ? meta.rate_sats_per_diff : null,
        meta,
        verification,
        ledger: {
            accrued: Number(worker.accrued_sats || 0),
            paid:    Number(worker.paid_sats    || 0),
            owed:    Number(worker.accrued_sats || 0) - Number(worker.paid_sats || 0),
            last_updated: Number(worker.last_updated || 0),
        },
        totals: {
            share_count:     Number(totals.share_count),
            sum_difficulty:  Number(totals.sum_difficulty),
            accrued_computed: Number(totals.accrued_computed),
            first_ts:        Number(totals.first_ts || 0),
            last_ts:         Number(totals.last_ts  || 0),
            blocks_found:    Number(totals.blocks_found),
        },
        days: days.map(d => ({
            day:           d.day,
            shares:        Number(d.shares),
            sum_diff:      Number(d.sum_diff),
            accrued_delta: Number(d.accrued_delta),
            blocks:        Number(d.blocks),
        })),
        recent: recent.map(r => ({
            id:          Number(r.id),
            ts:          Number(r.ts),
            difficulty:  Number(r.difficulty),
            is_block:    Number(r.is_block),
            block_hash:  r.block_hash,
            block_status: r.block_status || null,
            credit_sats: Number(r.credit_sats),
        })),
    };
}

/* Most recent successful payouts, newest first. Joined to workers so
 * the operator can eyeball who was paid + jump to their audit page. */
export function recentPayouts(handle, limit = 25) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT  p.id, p.worker_id, w.name AS worker_name,
                w.payout_address AS thunder_address,
                p.sats, p.fee_sats, p.txid, p.paid_at, p.note
        FROM    payouts p
        JOIN    workers w ON w.id = p.worker_id
        ORDER   BY p.paid_at DESC, p.id DESC
        LIMIT   ?
    `).all(limit).map(r => ({
        ...r,
        sats:     Number(r.sats),
        fee_sats: Number(r.fee_sats),
        paid_at:  Number(r.paid_at),
    }));
}

/* Payouts for a specific worker (used on the per-worker audit page). */
export function payoutsForWorker(handle, workerId, limit = 100) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT id, sats, fee_sats, txid, paid_at, note
        FROM   payouts
        WHERE  worker_id = ?
        ORDER  BY paid_at DESC, id DESC
        LIMIT  ?
    `).all(workerId, limit).map(r => ({
        ...r,
        sats:     Number(r.sats),
        fee_sats: Number(r.fee_sats),
        paid_at:  Number(r.paid_at),
    }));
}

/* Mainchain → Thunder deposits the operator has made, newest first. */
export function recentDeposits(handle, limit = 25) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT id, ts, btc_txid, sats_deposited, fee_sats, thunder_recipient,
               ctip_seq_before, ctip_seq_after, notes
        FROM   deposits
        ORDER  BY ts DESC, id DESC
        LIMIT  ?
    `).all(limit).map(r => ({
        ...r,
        ts:              Number(r.ts),
        sats_deposited:  Number(r.sats_deposited),
        fee_sats:        Number(r.fee_sats),
    }));
}

/* Recent blocks the pool has found — same query the public dashboard
 * uses (block.hash is the identifier; the coinbase txid is derivable
 * from `bitcoin-cli getblock <hash>`). Included here so admin ops see
 * the full BTC-flow chain in one place. */
export function recentBlocksFound(handle, limit = 15) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT b.id, b.ts, b.height, b.hash, b.finder_id, b.finder_address,
               b.reward_sats, b.fee_sats,
               b.status, b.confirmations, b.checked_via,
               w.name AS finder_name
        FROM   blocks_found b
        LEFT   JOIN workers w ON w.id = b.finder_id
        ORDER  BY b.ts DESC, b.id DESC
        LIMIT  ?
    `).all(limit).map(r => ({
        ...r,
        ts:          Number(r.ts),
        reward_sats: Number(r.reward_sats || 0),
        fee_sats:    Number(r.fee_sats || 0),
    }));
}

/* Probe the enforcer wallet's BTC balance (WalletService.GetBalance) via
 * ConnectRPC — a plain JSON POST, same transport the deposit action uses.
 * Short timeout so the admin page renders even when the enforcer is slow.
 * Returns { ok, confirmed_sats, pending_sats, has_synced, error? }.  */
export async function enforcerBalance(enforcerGrpcAddr, timeoutMs = 3000) {
    try {
        const j = await enforcerRpc(enforcerGrpcAddr,
            'cusf.mainchain.v1.WalletService/GetBalance', {}, timeoutMs);
        return {
            ok: true,
            confirmed_sats: Number(j.confirmedSats ?? 0),
            pending_sats:   Number(j.pendingSats   ?? 0),
            has_synced:     Boolean(j.hasSynced),
        };
    } catch (e) {
        const msg = (e.message || String(e)).slice(0, 200);
        return { ok: false, error: msg, confirmed_sats: 0, pending_sats: 0, has_synced: false };
    }
}

/* Minimal Thunder JSON-RPC client — we only need `balance()`. Short
 * timeout so the admin page renders promptly even when Thunder is
 * unreachable. */
export async function thunderBalance(rpcUrl, timeoutMs = 1500) {
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), timeoutMs);
    try {
        const res = await fetch(rpcUrl, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'balance', params: [] }),
            signal: ctrl.signal,
        });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const body = await res.json();
        if (body.error) throw new Error(`${body.error.code} ${body.error.message}`);
        return { ok: true, available_sats: Number(body.result?.available_sats ?? 0),
                 total_sats: Number(body.result?.total_sats ?? 0) };
    } catch (e) {
        return { ok: false, error: e.message, available_sats: 0, total_sats: 0 };
    } finally {
        clearTimeout(t);
    }
}

/* Broadcast attempts — successes and failures — newest first.
 *
 * `deposits` and `payouts` hold what succeeded. This holds what was *tried*,
 * with the transaction attached, which is what an operator needs when the
 * node rejects something. Filter by kind ('deposit' | 'payout') or pass null
 * for both.
 *
 * Tolerates a database predating the table: returns [] rather than throwing,
 * so an older DB degrades to "no history" instead of a 500. */
export function recentTxAttempts(handle, { kind = null, limit = 25, failedOnly = false } = {}) {
    const db = unwrap(handle);
    if (!db) return [];
    try {
        const where = [];
        const args  = [];
        if (kind)       { where.push('a.kind = ?');       args.push(kind); }
        if (failedOnly) { where.push("a.status = 'failed'"); }
        const sql = `
            SELECT a.id, a.ts, a.kind, a.status, a.stage, a.txid, a.raw_tx,
                   a.amount_sats, a.fee_sats, a.destination, a.worker_id,
                   a.error, a.detail, w.name AS worker_name
              FROM tx_attempts a
              LEFT JOIN workers w ON w.id = a.worker_id
             ${where.length ? 'WHERE ' + where.join(' AND ') : ''}
             ORDER BY a.ts DESC, a.id DESC
             LIMIT ?`;
        args.push(limit);
        return db.prepare(sql).all(...args).map(r => ({
            id:          Number(r.id),
            ts:          Number(r.ts),
            kind:        r.kind,
            status:      r.status,
            stage:       r.stage,
            txid:        r.txid,
            raw_tx:      r.raw_tx,
            amount_sats: r.amount_sats === null ? null : Number(r.amount_sats),
            fee_sats:    r.fee_sats    === null ? null : Number(r.fee_sats),
            destination: r.destination,
            worker_id:   r.worker_id === null ? null : Number(r.worker_id),
            worker_name: r.worker_name,
            error:       r.error,
            detail:      r.detail,
            ok:          r.status === 'broadcast',
        }));
    } catch {
        return [];   /* pre-tx_attempts DB */
    }
}
