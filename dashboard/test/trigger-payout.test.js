/* "Trigger payout now" → the payout worker's POST /tick.
 *
 * The worker now refuses /tick without `Authorization: Bearer
 * <PAYOUT_ADMIN_TOKEN>` (payout/lib/admin-http.js explains why a loopback
 * bind was not enough). The dashboard is the one legitimate caller, so it
 * has to send exactly that header — and must not send an Origin header, which
 * the worker treats as a browser and refuses outright.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { once } from 'node:events';
import { triggerPayout } from '../lib/actions.js';

async function fakeWorker(status = 200, body = { ok: true, result: { attempted: 2 } }) {
    const seen = [];
    const server = http.createServer((req, res) => {
        seen.push({ method: req.method, url: req.url, headers: req.headers });
        res.statusCode = status;
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify(body));
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    return { server, seen, url: `http://127.0.0.1:${server.address().port}` };
}

test('triggerPayout sends the token as a bearer header and no Origin', async (t) => {
    const w = await fakeWorker();
    t.after(() => w.server.close());
    const r = await triggerPayout({ payoutAdminUrl: w.url, payoutAdminToken: 's3cret' });
    assert.equal(r.ok, true);
    assert.match(r.detail, /attempted=2/);
    assert.equal(w.seen.length, 1);
    assert.equal(w.seen[0].method, 'POST');
    assert.equal(w.seen[0].url, '/tick');
    assert.equal(w.seen[0].headers.authorization, 'Bearer s3cret');
    assert.equal(w.seen[0].headers.origin, undefined);
});

test('without a token the call still goes out, and the worker\'s refusal is surfaced', async (t) => {
    const w = await fakeWorker(503, { ok: false, error: 'PAYOUT_ADMIN_TOKEN not set on the payout worker' });
    t.after(() => w.server.close());
    const r = await triggerPayout({ payoutAdminUrl: w.url });
    assert.equal(w.seen[0].headers.authorization, undefined);
    assert.equal(r.ok, false);
    assert.match(r.detail, /PAYOUT_ADMIN_TOKEN/);
});
