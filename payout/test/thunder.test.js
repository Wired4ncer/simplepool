/* Which of the three RPCs failed decides whether aborting a payout is safe.
 *
 * create and sign are local to the Thunder node, so a throw from either
 * definitely put nothing on the network and the in-flight row can be dropped
 * freely. Only a throw from submit carries broadcast ambiguity. Getting the
 * stage wrong would either strand payouts or risk double-paying, so it is
 * pinned here — along with the transactions, which are what an operator needs
 * to diagnose the failure and which used to be discarded.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ThunderClient } from '../lib/thunder.js';

/* Stub _call so no network is involved: `failAt` names the RPC that throws. */
function client(failAt) {
    const c = new ThunderClient('http://127.0.0.1:1');
    c._call = async (method) => {
        if (method === failAt) throw new Error(`${method} exploded`);
        if (method === 'create_transfer')   return { unsigned: true };
        if (method === 'sign_transaction')  return { signed: true };
        if (method === 'submit_transaction') return 'txid-abc';
        throw new Error('unexpected ' + method);
    };
    return c;
}

test('happy path returns txid plus both transactions', async () => {
    const r = await client(null).transferDetailed('addr', 100, 1);
    assert.equal(r.txid, 'txid-abc');
    assert.deepEqual(r.unsigned, { unsigned: true });
    assert.deepEqual(r.signed,   { signed: true });
});

test('transfer() still returns just the txid', async () => {
    assert.equal(await client(null).transfer('addr', 100, 1), 'txid-abc');
});

test('create failure: stage=create, no transactions yet', async () => {
    await assert.rejects(client('create_transfer').transferDetailed('a', 1, 1), (e) => {
        assert.equal(e.stage, 'create');
        assert.equal(e.unsigned, null);
        assert.equal(e.signed, null);
        return true;
    });
});

test('sign failure: stage=sign, unsigned tx retained', async () => {
    await assert.rejects(client('sign_transaction').transferDetailed('a', 1, 1), (e) => {
        assert.equal(e.stage, 'sign');
        assert.deepEqual(e.unsigned, { unsigned: true });
        assert.equal(e.signed, null);
        return true;
    });
});

test('submit failure: stage=submit, SIGNED tx retained', async () => {
    /* The important one — this is the tx that hit the node and was refused,
     * so it must survive to be shown. */
    await assert.rejects(client('submit_transaction').transferDetailed('a', 1, 1), (e) => {
        assert.equal(e.stage, 'submit');
        assert.deepEqual(e.signed, { signed: true });
        assert.match(e.message, /submit_transaction exploded/);
        return true;
    });
});

/* `mine` parks a BMM request and then blocks until a mainchain block carries
 * it — minutes on drynet3. We only need the parking, so the call is given a
 * short deadline and abandoned. Reporting that as a failure logged a warning
 * on every nudge and stalled each payout tick for the full RPC timeout. */

test('mine() reports a timeout as parked, not as a failure', async () => {
    const c = new ThunderClient({ url: 'http://127.0.0.1:1' });
    c._call = async () => {
        const e = new Error('This operation was aborted');
        e.name = 'AbortError';
        throw e;
    };
    assert.deepEqual(await c.mine(), { parked: true, completed: false });
});

test('mine() reports a completed call as completed', async () => {
    const c = new ThunderClient({ url: 'http://127.0.0.1:1' });
    c._call = async (m) => (m === 'mine' ? 'ok' : (() => { throw new Error(m); })());
    const r = await c.mine();
    assert.equal(r.parked, true);
    assert.equal(r.completed, true);
});

test('mine() still throws on a real RPC error', async () => {
    /* A node that rejects the call outright is a genuine problem and must not
     * be swallowed as "parked". */
    const c = new ThunderClient({ url: 'http://127.0.0.1:1' });
    c._call = async () => { throw new Error('thunder rpc mine: -1 no mainchain'); };
    await assert.rejects(c.mine(), /no mainchain/);
});

test('mine() uses its own short deadline, not the client timeout', async () => {
    const c = new ThunderClient({ url: 'http://127.0.0.1:1', timeoutMs: 10000 });
    let seen = null;
    c._call = async (_m, _p, timeoutMs) => { seen = timeoutMs; return 'ok'; };
    await c.mine();
    assert.equal(seen, 3000, 'a blocking call must not hold the tick for the full timeout');
});

/* The payout loop tells "the node declined" from "we never got an answer" by
 * one flag on the error, and the two decide opposite things: release the
 * batch and retry, or hold it for a human. So the flag is pinned at the one
 * place that sets it — a JSON-RPC error body — and pinned ABSENT on a
 * transport failure, which is the case that must never look like a refusal. */
async function withFetch(impl, fn) {
    const saved = globalThis.fetch;
    globalThis.fetch = impl;
    try { return await fn(); } finally { globalThis.fetch = saved; }
}

test('an answered RPC error carries rpcRejected', async () => {
    const c = new ThunderClient('http://127.0.0.1:1');
    const err = await withFetch(
        async () => ({ ok: true, json: async () => ({ error: { code: -32000, message: 'utxo double spent' } }) }),
        () => c._call('submit_transaction', []).then(() => null, e => e));
    assert.ok(err instanceof Error);
    assert.equal(err.rpcRejected, true, 'the node ran the method and declined');
    assert.match(err.message, /utxo double spent/);
});

test('a transport failure does NOT carry rpcRejected', async () => {
    const c = new ThunderClient('http://127.0.0.1:1');
    const err = await withFetch(
        async () => { throw new Error('ECONNRESET'); },
        () => c._call('submit_transaction', []).then(() => null, e => e));
    assert.ok(err instanceof Error);
    assert.equal(err.rpcRejected, undefined, 'no answer is not a refusal');
});

test('a non-200 reply does NOT carry rpcRejected', async () => {
    const c = new ThunderClient('http://127.0.0.1:1');
    const err = await withFetch(
        async () => ({ ok: false, status: 502, statusText: 'Bad Gateway' }),
        () => c._call('submit_transaction', []).then(() => null, e => e));
    assert.ok(err instanceof Error);
    assert.equal(err.rpcRejected, undefined, 'a proxy answered, the node may not have');
});
