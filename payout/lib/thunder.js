/* Thunder JSON-RPC 2.0 client.
 *
 * Per LayerTwo-Labs/thunder-rust (rpc-api/lib.rs, app/rpc_server.rs):
 *   - HTTP JSON-RPC, default port 6000 + sidechain_number = 6009
 *   - No auth required (permissive CORS, no bearer/basic)
 *   - Methods we care about:
 *       create_transfer(dest, value_sats, fee_sats) -> Transaction (unsigned)
 *       sign_transaction(tx, broadcast?) -> Authorized<Transaction>
 *       submit_transaction(authorized_tx) -> Txid
 *       balance() -> Balance { available_sats, total_sats, ... }
 *       get_wallet_addresses() -> [Address]
 *
 * Thunder v0.17.0 removed the one-shot transfer/withdraw methods in
 * favor of the create/sign/submit triple; transfer() below composes
 * them so callers keep the old broadcast-and-return-txid shape.
 *
 * jsonrpsee uses JSON-RPC 2.0 strict-positional params. */

export class ThunderClient {
    constructor({ url, user, pass, timeoutMs = 10000 }) {
        this.url = url;
        this.timeoutMs = timeoutMs;
        this.auth = (user && pass)
            ? 'Basic ' + Buffer.from(`${user}:${pass}`).toString('base64')
            : null;
        this._id = 0;
    }

    async _call(method, params, timeoutMs = this.timeoutMs) {
        const id = ++this._id;
        const headers = { 'Content-Type': 'application/json' };
        if (this.auth) headers.Authorization = this.auth;
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(), timeoutMs);
        let res;
        try {
            res = await fetch(this.url, {
                method: 'POST',
                headers,
                body: JSON.stringify({ jsonrpc: '2.0', id, method, params }),
                signal: ctrl.signal,
            });
        } finally {
            clearTimeout(t);
        }
        if (!res.ok) {
            throw new Error(`thunder rpc ${method}: HTTP ${res.status} ${res.statusText}`);
        }
        const body = await res.json();
        if (body.error) {
            const e = body.error;
            throw new Error(`thunder rpc ${method}: ${e.code} ${e.message}`);
        }
        return body.result;
    }

    /* Returns { available_sats, total_sats, ... } — Thunder's Balance struct.
     * We only need available_sats to gate payouts. */
    async balance() {
        return this._call('balance', []);
    }

    /* Where a broadcast transaction currently stands.
     *
     *   { known: false }                    node has never seen this txid
     *   { known: true, confirmed: false }   in the mempool, holding its inputs
     *   { known: true, confirmed: true }    mined, inputs released
     *
     * `get_transaction` returns `{ tx, block_hash }`, with both null for an
     * unknown txid and `block_hash` null while a known tx is unconfirmed —
     * so the two fields have to be read together. This distinction is what
     * lets the payout loop tell "still settling" from "gone", which decide
     * opposite things: wait, or stop waiting.
     *
     * Never throws: an unreachable node returns { known: false, error }, and
     * callers must treat that as "cannot tell" rather than "not pending". */
    async getTransaction(txid) {
        try {
            const r = await this._call('get_transaction', [txid]);
            const tx = r?.tx ?? null;
            const blockHash = r?.block_hash ?? r?.blockHash ?? null;
            return { known: tx !== null, confirmed: blockHash !== null, blockHash };
        } catch (e) {
            return { known: false, confirmed: false, blockHash: null, error: e.message };
        }
    }

    /* How many transactions Thunder is holding but has not mined.
     *
     * `get_block_template` assembles the block Thunder WOULD blind-merge-mine
     * right now, so its body is the mempool. Nothing is requested and no BMM
     * bid is spent by asking.
     *
     * The payout loop uses this as a pre-flight check, and the reason it is
     * meaningful there is that settlePending() runs first: once that reports
     * nothing outstanding, anything still in the mempool is a transaction the
     * ledger has no record of — and it is spending the same wallet UTXOs the
     * next transfer would. See the gate in runOnce().
     *
     * Never throws. An unreachable node reports { ok: false }, which callers
     * must read as "cannot tell", never as "blocked": refusing to pay because
     * a diagnostic RPC is down would be an outage of its own. */
    async mempool() {
        try {
            const t = await this._call('get_block_template', []);
            const txs = t?.block?.body?.transactions;
            return { ok: true, count: Array.isArray(txs) ? txs.length : 0 };
        } catch (e) {
            return { ok: false, count: 0, error: e.message };
        }
    }

    /* Pay many recipients in a single Thunder transaction.
     *
     * Thunder's create_transfer builds a one-recipient tx, and its wallet
     * cannot spend the change of an unconfirmed transaction — so paying N
     * workers as N transactions means N sidechain blocks, and Thunder only
     * advances when a mainchain block commits to it. Batching removes that
     * ceiling entirely: the whole queue settles in one block.
     *
     * Rather than construct a transaction from scratch — the kind of thing
     * that loses money when one field is wrong — this asks Thunder to build a
     * transfer for the TOTAL and then splits that one output into one per
     * recipient. Inputs, the utreexo proof and the change output are whatever
     * Thunder chose and are never touched. The proof commits to inputs
     * (`targets`/`hashes`), not outputs, so it stays valid.
     *
     * Value conservation is structural — the outputs we insert sum to exactly
     * the one we removed — and asserted anyway before signing, because the
     * failure mode is silently burning the difference as fee.
     *
     * `recipients` is [{ address, sats }]. One flat fee covers the whole
     * transaction, not one per recipient. */
    async transferBatchDetailed(recipients, feeSats) {
        if (!Array.isArray(recipients) || recipients.length === 0) {
            throw new Error('transferBatch: no recipients');
        }
        const amounts = recipients.map(r => BigInt(r.sats));
        if (amounts.some(v => v <= 0n)) throw new Error('transferBatch: non-positive amount');
        const total = amounts.reduce((a, b) => a + b, 0n);
        /* Sat amounts stay well inside 2^53 (all of Bitcoin is ~2.1e15), but
         * the JSON layer is numbers, so refuse rather than round silently. */
        if (total > BigInt(Number.MAX_SAFE_INTEGER)) {
            throw new Error(`transferBatch: total ${total} exceeds safe integer range`);
        }

        let unsigned = null, signed = null;
        const fail = (stage, err) => { err.stage = stage; err.unsigned = unsigned; err.signed = signed; return err; };

        try {
            unsigned = await this._call('create_transfer',
                [recipients[0].address, Number(total), Number(feeSats)]);
        } catch (e) { throw fail('create', e); }

        /* thunder >= 0.17.1 (commit a195d67, "RPC: sign and broadcast txs
         * created via create_*") changed create_transfer to sign and broadcast
         * internally, returning a bare Txid instead of an unsigned tx. The
         * splice-the-outputs path below cannot run against that: by the time we
         * see the response the transaction is already on the network, and it
         * pays the WHOLE total to recipients[0].
         *
         * That is correct only when every recipient shares one address, which is
         * the normal case here -- a miner's rigs all authenticate with the same
         * Thunder address and differ only by the .rig suffix, so they collapse
         * to a single output anyway.
         *
         * When the addresses genuinely differ we cannot fix it after the fact:
         * the node has already paid the full total to one of them. That is
         * reported at the 'submit' stage rather than 'create', because the funds
         * ARE on the network and the caller must treat it with the usual
         * broadcast ambiguity instead of as a clean abort. */
        const nodeTxid = typeof unsigned === 'string'
            ? unsigned
            : (typeof unsigned?.txid === 'string' ? unsigned.txid : null);
        if (nodeTxid !== null) {
            const distinct = new Set(recipients.map(r => r.address));
            if (distinct.size !== 1) {
                const err = fail('submit', new Error(
                    `transferBatch: thunder already broadcast ${nodeTxid} paying the ` +
                    `full ${total} sats to ${recipients[0].address}, but this batch has ` +
                    `${distinct.size} distinct addresses. Funds are on the network -- ` +
                    `reconcile by hand (payout/README.md).`));
                /* Hand the txid back. This is not a clean abort: a live
                 * transaction is holding the wallet's UTXOs, and a caller that
                 * drops its in-flight rows here has no record that anything went
                 * out — so it retries into `utxo double spent` on every tick
                 * from then on. avonpool did that 216 times over 24h. Keeping
                 * the rows against this txid makes settlePending() block the
                 * queue instead, which is the correct response to funds on the
                 * network that we did not intend to send. */
                err.broadcastTxid = nodeTxid;
                throw err;
            }
            return { txid: nodeTxid, unsigned: null, signed: null,
                     recipients: recipients.length, total, broadcastByNode: true };
        }

        const outs = unsigned?.outputs;
        if (!Array.isArray(outs) || outs.length === 0) {
            throw fail('create', new Error('create_transfer returned no outputs'));
        }
        const valueOf = o => BigInt(o?.content?.Value ?? o?.content?.value ?? 0);
        const sum = a => a.reduce((acc, o) => acc + valueOf(o), 0n);
        const before = sum(outs);

        /* Match on address AND amount: the change output could coincidentally
         * carry the same value, and splitting the change instead of the
         * payment would send the entire wallet to one worker. Ambiguity is a
         * hard error, never a guess. */
        const hits = outs.reduce((acc, o, i) =>
            (o.address === recipients[0].address && valueOf(o) === total) ? acc.concat(i) : acc, []);
        if (hits.length !== 1) {
            throw fail('create', new Error(
                `transferBatch: expected exactly one output of ${total} to ` +
                `${recipients[0].address}, found ${hits.length}`));
        }

        outs.splice(hits[0], 1, ...recipients.map((r, i) => ({
            address: r.address,
            content: { Value: Number(amounts[i]) },
        })));

        const after = sum(outs);
        if (after !== before) {
            throw fail('create', new Error(
                `transferBatch: value not conserved (${before} -> ${after}); refusing to sign`));
        }

        try { signed = await this._call('sign_transaction', [unsigned, false]); }
        catch (e) { throw fail('sign', e); }
        try {
            const txid = await this._call('submit_transaction', [signed]);
            return { txid, unsigned, signed, recipients: recipients.length, total };
        } catch (e) { throw fail('submit', e); }
    }

    /* Build, sign, broadcast a Thunder tx from the node's wallet to `dest`.
     * Returns the txid (hex). Throws on insufficient funds, bad address, etc.
     *
     * Three RPCs under the hood. Only submit_transaction can leave a tx
     * on the network, so a throw from create/sign is always a clean
     * abort; a throw from submit carries the same broadcast ambiguity
     * the old one-shot transfer had, and payout.js already treats it
     * that way (abort + retry next tick, stuck-row sweep as backstop). */
    async transfer(dest, valueSats, feeSats) {
        return (await this.transferDetailed(dest, valueSats, feeSats)).txid;
    }

    /* Same three RPCs, but keeps hold of the intermediate transactions and
     * reports which step failed.
     *
     * The transaction is what an operator needs to diagnose a rejection, and
     * previously it was discarded — a failure surfaced as a bare message with
     * no way to inspect what had been built. On error this throws with
     * `.stage` ('create' | 'sign' | 'submit') and whatever transactions had
     * been produced by then attached, so the caller can record them.
     *
     * The stage also disambiguates the broadcast question: create and sign
     * are local, so a throw from either is a clean abort that definitely put
     * nothing on the network. Only a throw from `submit` carries the usual
     * did-it-or-didn't-it ambiguity. */
    async transferDetailed(dest, valueSats, feeSats) {
        let unsigned = null, signed = null;
        const fail = (stage, err) => {
            err.stage    = stage;
            err.unsigned = unsigned;
            err.signed   = signed;
            return err;
        };
        try {
            unsigned = await this._call('create_transfer',
                [dest, Number(valueSats), Number(feeSats)]);
        } catch (e) { throw fail('create', e); }
        /* thunder >= 0.17.1 signs and broadcasts internally, returning a Txid.
         * Single dest, so there is no address ambiguity to guard against. */
        {
            const nodeTxid = typeof unsigned === 'string'
                ? unsigned
                : (typeof unsigned?.txid === 'string' ? unsigned.txid : null);
            if (nodeTxid !== null) {
                return { txid: nodeTxid, unsigned: null, signed: null,
                         broadcastByNode: true };
            }
        }
        try {
            signed = await this._call('sign_transaction', [unsigned, false]);
        } catch (e) { throw fail('sign', e); }
        try {
            const txid = await this._call('submit_transaction', [signed]);
            return { txid, unsigned, signed };
        } catch (e) { throw fail('submit', e); }
    }

    async getWalletAddresses() {
        return this._call('get_wallet_addresses', []);
    }

    /* Every wallet UTXO, each tagged with the outpoint that created it.
     *
     * Returns [{ txid, address, sats }] for UTXOs that came from a regular
     * transaction, and drops Deposit/Coinbase outpoints — they have no
     * originating sidechain txid, so they can never answer the question this
     * is used for.
     *
     * Never throws: an unreachable node returns { ok: false }, because
     * "cannot tell" and "no such UTXO" decide opposite things and must not
     * collapse into the same empty array. */
    async walletUtxos() {
        let raw;
        try {
            raw = await this._call('get_wallet_utxos', []);
        } catch (e) {
            return { ok: false, utxos: [], error: e.message };
        }
        if (!Array.isArray(raw)) return { ok: false, utxos: [], error: 'not an array' };
        const utxos = [];
        for (const u of raw) {
            const txid = u?.outpoint?.Regular?.txid ?? null;
            if (typeof txid !== 'string') continue;
            utxos.push({
                txid,
                address: u?.output?.address ?? null,
                sats: BigInt(u?.output?.content?.Value ?? u?.output?.content?.value ?? 0),
            });
        }
        return { ok: true, utxos };
    }

    /* Ask Thunder to attempt BMM — the sidechain equivalent of mining a block.
     *
     * Thunder advances only when a mainchain block commits to it, and nothing
     * schedules that on its own, so without this a broadcast payout sits in
     * the mempool indefinitely (observed: 4h+, and the queue behind it stops
     * entirely). The mainchain side is the pool's own coinbase, so the
     * commitment lands on the next block the pool wins.
     *
     * `mine` PARKS a BMM request and then blocks until a mainchain block
     * carries it — minutes on drynet3, and unbounded on a quiet chain. We only
     * need the parking, so this waits just long enough for the request to be
     * created (observed at ~170ms) and then walks away. The abort does not
     * undo it: the BMM transaction is already on the mainchain by then.
     *
     * So a timeout here is the EXPECTED outcome, not an error, and is reported
     * as { parked: true, completed: false } rather than thrown. Treating it as
     * a failure logged a warning on every single nudge and stalled each tick
     * for the full RPC timeout.
     *
     * Costs a BMM bid on the mainchain, so callers must rate-limit and only
     * call it when something is genuinely waiting to settle. */
    async mine(timeoutMs = 3000) {
        try {
            const r = await this._call('mine', [], timeoutMs);
            return { parked: true, completed: true, result: r };
        } catch (e) {
            if (e?.name === 'AbortError' || e?.name === 'TimeoutError') {
                return { parked: true, completed: false };
            }
            throw e;
        }
    }
}
