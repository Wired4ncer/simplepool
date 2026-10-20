/* The admin HTTP surface runs a full payout cycle on POST /tick.
 *
 * It used to have no authentication at all, on the theory that a loopback
 * bind was the boundary. It is not: the operator's browser is on loopback,
 * a body-less POST is a CORS simple request any web page can send, DNS
 * rebinding lets a page do it same-origin, and in the compose stack the
 * worker binds 0.0.0.0 on the docker network. Each test here is one of
 * those doors, plus the one legitimate caller (the dashboard, server-side,
 * with the token) to prove it still gets through.
 *
 * `tick` is stubbed: what is under test is who may reach it, not what a
 * payout cycle does.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { once } from 'node:events';
import { startAdminHttp, hostOnly } from '../lib/admin-http.js';

const TOKEN = 'a'.repeat(64);
const quiet = { debug() {}, info() {}, warn() {}, error() {} };

async function start(opts = {}) {
    const ticks = [];
    const server = startAdminHttp({
        port: 0, bind: '127.0.0.1', ctx: {}, log: quiet, token: TOKEN,
        tick: async () => { ticks.push(1); return { attempted: 0 }; },
        ...opts,
    });
    await once(server, 'listening');
    return { server, ticks, port: server.address().port };
}

/* Raw http.request so the test controls every header — fetch() would add
 * its own and refuses to let us set Host. */
function req(port, { method = 'POST', path = '/tick', headers = {} } = {}) {
    return new Promise((resolve, reject) => {
        const r = http.request({ host: '127.0.0.1', port, method, path,
                                 headers: { host: `127.0.0.1:${port}`, ...headers } },
            (res) => {
                let body = '';
                res.on('data', c => body += c);
                res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(body) }));
            });
        r.on('error', reject);
        r.end();
    });
}

test('POST /tick without a token is 401 and does not run a payout', async (t) => {
    const { server, ticks, port } = await start();
    t.after(() => server.close());
    const r = await req(port);
    assert.equal(r.status, 401);
    assert.equal(ticks.length, 0);
});

test('POST /tick with the wrong token is 401 and does not run a payout', async (t) => {
    const { server, ticks, port } = await start();
    t.after(() => server.close());
    for (const auth of [`Bearer ${'b'.repeat(64)}`, `Bearer ${TOKEN.slice(0, 10)}`,
                        `Basic ${TOKEN}`, TOKEN]) {
        const r = await req(port, { headers: { authorization: auth } });
        assert.equal(r.status, 401, auth);
    }
    assert.equal(ticks.length, 0);
});

test('POST /tick with the token runs one payout cycle', async (t) => {
    const { server, ticks, port } = await start();
    t.after(() => server.close());
    const r = await req(port, { headers: { authorization: `Bearer ${TOKEN}` } });
    assert.equal(r.status, 200);
    assert.deepEqual(r.body, { ok: true, result: { attempted: 0 } });
    assert.equal(ticks.length, 1);
});

test('a request carrying Origin is 403 even with the right token', async (t) => {
    /* What a drive-by page in the operator's browser looks like. */
    const { server, ticks, port } = await start();
    t.after(() => server.close());
    for (const origin of ['https://evil.example', 'null', `http://127.0.0.1:${port}`]) {
        const r = await req(port, { headers: { origin, authorization: `Bearer ${TOKEN}` } });
        assert.equal(r.status, 403, origin);
    }
    assert.equal(ticks.length, 0);
});

test('a rebound Host is 403; loopback names and allow-listed hosts pass', async (t) => {
    const { server, ticks, port } = await start({ allowedHosts: ['host.docker.internal'] });
    t.after(() => server.close());
    const auth = { authorization: `Bearer ${TOKEN}` };
    for (const host of [`evil.example:${port}`, 'evil.example', `127.0.0.1.evil.example:${port}`]) {
        const r = await req(port, { headers: { ...auth, host } });
        assert.equal(r.status, 403, host);
    }
    assert.equal(ticks.length, 0);
    for (const host of [`localhost:${port}`, `[::1]:${port}`, '127.0.0.1',
                        `host.docker.internal:${port}`]) {
        const r = await req(port, { headers: { ...auth, host } });
        assert.equal(r.status, 200, host);
    }
    assert.equal(ticks.length, 4);
});

test('no token configured: /tick is 503, never runs unauthenticated', async (t) => {
    const { server, ticks, port } = await start({ token: '' });
    t.after(() => server.close());
    const r = await req(port, { headers: { authorization: 'Bearer ' } });
    assert.equal(r.status, 503);
    assert.match(r.body.error, /PAYOUT_ADMIN_TOKEN/);
    assert.equal(ticks.length, 0);
});

test('/healthz stays unauthenticated', async (t) => {
    const { server, port } = await start();
    t.after(() => server.close());
    const r = await req(port, { method: 'GET', path: '/healthz' });
    assert.equal(r.status, 200);
});

test('hostOnly strips ports and IPv6 brackets', () => {
    assert.equal(hostOnly('LocalHost:9080'), 'localhost');
    assert.equal(hostOnly('[::1]:9080'), '::1');
    assert.equal(hostOnly('::1'), '::1');
    assert.equal(hostOnly(undefined), '');
});
