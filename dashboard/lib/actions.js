/* Backend implementations for the admin dashboard's write actions.
 *
 * Every function returns { ok, msg, detail? } so the route handler can
 * uniformly format flash messages regardless of which action ran. Never
 * throws — errors are captured into { ok: false, msg }.
 *
 * External systems these talk to:
 *   - Thunder JSON-RPC       — nudgeMine, removeFromMempool
 *   - payout worker HTTP     — triggerPayout
 *   - bip300301_enforcer     — createDeposit (ConnectRPC over HTTP)
 */

import { enforcerRpc } from './enforcer.js';
import { validateThunderAddress } from './thunder-address.js';

/* ---------- transaction attempt log ---------- */

/* Record every broadcast attempt, successful or not, into tx_attempts.
 *
 * A failed broadcast used to leave nothing behind but a flash message that
 * vanished on the next page load — and the raw transaction, which is what
 * you actually need to diagnose a rejection, was never surfaced anywhere.
 *
 * Best-effort by design: a logging failure must never change the outcome of
 * the action that was attempted, so this swallows its own errors and returns
 * the row id (or null). Callers pass the fullest error text they have; the
 * column is deliberately untruncated. */
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
        return null;   /* never let logging break the action */
    }
}

/* After a deposit broadcast fails, the enforcer has still built and SIGNED
 * the transaction — it only failed to hand it to bitcoind. That signed tx is
 * in the enforcer's wallet, so it can be recovered and shown to the operator.
 *
 * Matches on amount + sidechain rather than txid, because a failed
 * CreateDepositTransaction returns no txid at all. Returns the newest
 * plausible match, or null. Never throws: this runs on an error path and
 * must not mask the original failure. */
async function recoverDepositTx(enforcerGrpcAddr, sidechainId, valueSats) {
    try {
        const j = await enforcerRpc(enforcerGrpcAddr,
            'cusf.mainchain.v1.WalletService/ListSidechainDepositTransactions',
            {}, 15_000);
        const rows = j?.transactions || [];
        const want = Number(valueSats);
        /* Newest first — the enforcer returns oldest-first in practice, and
         * the attempt we just made is by definition the most recent. */
        for (let i = rows.length - 1; i >= 0; i--) {
            const r  = rows[i];
            const sn = r?.sidechainNumber?.value ?? r?.sidechain_number?.value
                     ?? r?.sidechainNumber ?? r?.sidechain_number;
            if (sn !== undefined && Number(sn) !== Number(sidechainId)) continue;
            const tx = r?.tx || r?.transaction;
            if (!tx) continue;
            /* Unconfirmed only: a confirmed tx cannot be the one that just
             * failed to broadcast. */
            if (tx.confirmationInfo || tx.confirmation_info) continue;
            const sent = Number(tx.sentSats ?? tx.sent_sats ?? 0);
            if (want && sent && Math.abs(sent - want) > Number(tx.feeSats ?? tx.fee_sats ?? 0) + 1000) continue;
            return {
                txid:  tx.txid?.hex  ?? (typeof tx.txid === 'string' ? tx.txid : null),
                rawTx: tx.rawTransaction?.hex ?? tx.raw_transaction?.hex
                       ?? (typeof tx.rawTransaction === 'string' ? tx.rawTransaction : null),
            };
        }
    } catch { /* recovery is a bonus, not a requirement */ }
    return null;
}

/* ---------- Thunder RPC helper ---------- */

async function thunderRpc(rpcUrl, method, params) {
    const body = { jsonrpc: '2.0', id: 1, method, params: params ?? [] };
    const ctl  = new AbortController();
    const t    = setTimeout(() => ctl.abort(), 30_000);
    try {
        const r = await fetch(rpcUrl, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify(body),
            signal: ctl.signal,
        });
        const text = await r.text();
        let j;
        try { j = JSON.parse(text); }
        catch { throw new Error(`thunder rpc ${method}: non-json response: ${text.slice(0, 200)}`); }
        if (j.error) {
            const msg = j.error.message || JSON.stringify(j.error);
            throw new Error(`thunder rpc ${method}: ${msg}`);
        }
        return j.result;
    } finally {
        clearTimeout(t);
    }
}

/* ---------- Action #4: nudge Thunder to mint a sidechain block ---------- */

export async function nudgeMine({ thunderRpcUrl }) {
    try {
        const result = await thunderRpc(thunderRpcUrl, 'mine', []);
        return {
            ok: true,
            msg: 'Thunder mine nudged',
            detail: result ? String(result).slice(0, 200) : 'ok',
        };
    } catch (e) {
        /* Thunder often returns "BMM request with same prev_bytes already
         * exists" when it can't create a new BMM commit yet. That's not
         * really a failure from the operator's POV — surface it plainly. */
        const msg = e.message || String(e);
        return { ok: false, msg: 'nudge mine failed', detail: msg };
    }
}

/* ---------- Action #3: remove a stuck tx from Thunder's mempool ---------- */

const TXID_RE = /^[0-9a-fA-F]{64}$/;

export async function removeFromMempool({ thunderRpcUrl, txid }) {
    if (!TXID_RE.test(txid || '')) {
        return { ok: false, msg: 'invalid txid', detail: 'expected 64 hex chars' };
    }
    try {
        const result = await thunderRpc(thunderRpcUrl, 'remove_from_mempool', [txid]);
        return {
            ok: true,
            msg: `removed ${txid.slice(0, 12)}… from Thunder mempool`,
            detail: result != null ? String(result).slice(0, 200) : 'ok',
        };
    } catch (e) {
        return { ok: false, msg: 'remove_from_mempool failed', detail: e.message };
    }
}

/* ---------- Action #2: kick the payout worker to run one cycle ---------- */

export async function triggerPayout({ payoutAdminUrl }) {
    if (!payoutAdminUrl) {
        return { ok: false, msg: 'PAYOUT_ADMIN_URL not configured' };
    }
    const ctl = new AbortController();
    const t   = setTimeout(() => ctl.abort(), 60_000);
    try {
        const r = await fetch(new URL('/tick', payoutAdminUrl), {
            method: 'POST',
            signal: ctl.signal,
        });
        const j = await r.json().catch(() => ({}));
        if (!r.ok || j.ok === false) {
            return {
                ok: false,
                msg: 'payout worker /tick failed',
                detail: j.error || `http ${r.status}`,
            };
        }
        const res = j.result || {};
        return {
            ok: true,
            msg: `payout tick fired`,
            detail: `attempted=${res.attempted ?? 0} broadcast=${res.broadcast ?? 0} ` +
                    `settled=${res.settled ?? 0} failed=${res.failed ?? 0}` +
                    (res.reserve_short ? ' (reserve short)' : '') +
                    (res.waiting_on ? ` (waiting on ${String(res.waiting_on).slice(0, 16)}…)` : ''),
        };
    } catch (e) {
        return { ok: false, msg: 'payout worker unreachable', detail: e.message };
    } finally {
        clearTimeout(t);
    }
}

/* ---------- Action #1: BTC → Thunder deposit (via bip300301_enforcer) ---------- */

/* The enforcer speaks ConnectRPC on the same port as its gRPC (:50051),
 * so this is a plain JSON POST:
 *
 *   POST http://ENFORCER_ADDR/cusf.mainchain.v1.WalletService/CreateDepositTransaction
 *   { sidechain_id, address, value_sats, fee_sats }
 *
 * On success the enforcer returns a txid; we insert into the pool DB's
 * `deposits` table so the admin ledger shows the operator action.
 */
export async function createDeposit({
    enforcerGrpcAddr, sidechainId, address, valueSats, feeSats,
    db,   /* writable handle */
}) {
    /* Validate inputs up front — enforcer errors are less friendly. */
    const sid = Number(sidechainId);
    if (!Number.isInteger(sid) || sid < 0 || sid > 255) {
        return { ok: false, msg: 'invalid sidechain_id (expected 0..255)' };
    }
    /* Validate the recipient the same way the proxy validates a miner's
     * address at authorize time. This form moves real BTC onto the sidechain
     * and there is no second confirmation, so a typo here is unrecoverable —
     * a length check was never enough. */
    const addrCheck = validateThunderAddress(address);
    if (!addrCheck.ok) {
        return { ok: false, msg: 'invalid recipient address', detail: addrCheck.msg };
    }
    const val = BigInt(valueSats || 0);
    if (val <= 0n) return { ok: false, msg: 'value_sats must be > 0' };
    const fee = BigInt(feeSats || 0);
    if (fee < 0n) return { ok: false, msg: 'fee_sats must be >= 0' };

    const params = { sidechain_id: sid, address, value_sats: Number(val), fee_sats: Number(fee) };

    let j;
    try {
        j = await enforcerRpc(enforcerGrpcAddr,
            'cusf.mainchain.v1.WalletService/CreateDepositTransaction',
            params, 60_000);
    } catch (e) {
        /* Broadcast failed. The enforcer still SIGNED the transaction, so
         * recover it and show the operator what was actually built — that is
         * the only way to diagnose a node-side rejection such as
         * `-26 scriptpubkey`. The full error is stored untruncated. */
        const full = e.message || String(e);
        const rec  = await recoverDepositTx(enforcerGrpcAddr, sid, val);
        recordTxAttempt(db, {
            kind: 'deposit', status: 'failed', stage: 'broadcast',
            txid: rec?.txid || null, rawTx: rec?.rawTx || null,
            amountSats: val, feeSats: fee, destination: address,
            error: full, detail: params,
        });
        return {
            ok: false,
            msg: 'CreateDepositTransaction failed',
            detail: full,
            txid:  rec?.txid  || null,
            rawTx: rec?.rawTx || null,
        };
    }

    /* Enforcer returns JSON with a `txid` field (bytes as base64 or hex
     * depending on version). */
    const txid =
        j.txid?.hex || j.txid?.value?.hex ||
        (typeof j.txid === 'string' ? j.txid : null);

    /* Fetch the raw hex so a successful deposit is just as inspectable as a
     * failed one. */
    const rec = await recoverDepositTx(enforcerGrpcAddr, sid, val);

    recordTxAttempt(db, {
        kind: 'deposit', status: 'broadcast', stage: 'broadcast',
        txid: txid || rec?.txid || null, rawTx: rec?.rawTx || null,
        amountSats: val, feeSats: fee, destination: address,
        detail: params,
    });

    /* Permanent deposit ledger. Column names must match schema.sql — they
     * previously did not (ctip_before/ctip_after/note vs
     * ctip_seq_before/ctip_seq_after/notes), so every INSERT threw and no
     * deposit was ever recorded, successful or otherwise. */
    let dbNote = '';
    if (db && typeof db.prepare === 'function') {
        try {
            db.prepare(`
                INSERT INTO deposits
                    (ts, btc_txid, sats_deposited, fee_sats, thunder_recipient,
                     ctip_seq_before, ctip_seq_after, notes)
                VALUES (?, ?, ?, ?, ?, NULL, NULL, ?)
            `).run(Math.floor(Date.now() / 1000), txid || '(pending)',
                   Number(val), Number(fee), address,
                   `via admin dashboard, sidechain_id=${sid}`);
        } catch (e) {
            dbNote = ` (deposit-ledger write failed: ${e.message})`;
        }
    }

    return {
        ok: true,
        msg: `deposit tx submitted${dbNote}`,
        detail: txid ? `txid=${txid}, ${val} sats to ${address}` :
                       `see raw enforcer response: ${JSON.stringify(j).slice(0, 200)}`,
        txid:  txid || rec?.txid || null,
        rawTx: rec?.rawTx || null,
    };
}
