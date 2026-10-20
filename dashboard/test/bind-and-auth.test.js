/* Where the dashboard listens, and how /admin reads Basic credentials.
 *
 * The bind test boots the real server.js, not just the helper: the bug was
 * app.listen(PORT) with no host, and only the listening socket proves the
 * helper is actually wired in. PORT=0 lets the kernel pick a free port; the
 * startup line reports the address really bound.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { resolveBind } from '../lib/bind.js';
import { parseBasicAuth } from '../lib/basic-auth.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SERVER = path.resolve(__dirname, '../server.js');

const basic = s => 'Basic ' + Buffer.from(s, 'utf8').toString('base64');

/* Boot server.js with `env` and resolve with { host, port, child } once it
 * logs its listening address. */
function boot(env) {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dash-bind-'));
    const child = spawn(process.execPath, [SERVER], {
        cwd: path.dirname(SERVER),
        env: { ...process.env, PORT: '0', PROXY_DB_PATH: path.join(tmp, 'none.db'),
               ADMIN_CREDENTIALS_FILE: '', DASHBOARD_BIND: '', ...env },
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    return new Promise((resolve, reject) => {
        let out = '';
        const timer = setTimeout(() => { child.kill(); reject(new Error('no startup line: ' + out)); }, 10000);
        const onData = d => {
            out += d;
            const m = out.match(/simplepool dashboard on (\S+):(\d+) /);
            if (m) { clearTimeout(timer); resolve({ host: m[1], port: Number(m[2]), child }); }
        };
        child.stdout.on('data', onData);
        child.stderr.on('data', onData);
        child.on('exit', code => { clearTimeout(timer); reject(new Error(`exited ${code}: ${out}`)); });
    });
}

test('resolveBind: loopback unless DASHBOARD_BIND opts in', () => {
    assert.equal(resolveBind({}), '127.0.0.1');
    assert.equal(resolveBind({ DASHBOARD_BIND: '' }), '127.0.0.1');
    assert.equal(resolveBind({ DASHBOARD_BIND: '0.0.0.0' }), '0.0.0.0');
    assert.equal(resolveBind({ DASHBOARD_BIND: '::' }), '::');
});

test('parseBasicAuth splits on the first colon only (RFC 7617)', () => {
    assert.deepEqual(parseBasicAuth(basic('admin:pw')),         { user: 'admin', pass: 'pw' });
    assert.deepEqual(parseBasicAuth(basic('admin:pa:ss')),      { user: 'admin', pass: 'pa:ss' });
    assert.deepEqual(parseBasicAuth(basic('user:pass:extra')),  { user: 'user',  pass: 'pass:extra' });
    assert.deepEqual(parseBasicAuth(basic('admin:')),           { user: 'admin', pass: '' });
    assert.equal(parseBasicAuth(basic('nocolon')), null);
    assert.equal(parseBasicAuth('Bearer abc'), null);
    assert.equal(parseBasicAuth(undefined), null);
});

test('server.js listens on 127.0.0.1 by default and logs it', async () => {
    const { host, child } = await boot({ ADMIN_USER: 'admin', ADMIN_PASSWORD: 'pw' });
    try {
        assert.equal(host, '127.0.0.1');
    } finally { child.kill(); }
});

test('server.js honours DASHBOARD_BIND=0.0.0.0 as the explicit opt-in', async () => {
    const { host, child } = await boot({ ADMIN_USER: 'admin', ADMIN_PASSWORD: 'pw', DASHBOARD_BIND: '0.0.0.0' });
    try {
        assert.equal(host, '0.0.0.0');
    } finally { child.kill(); }
});

test('/admin: a password containing ":" authenticates; trailing junk does not', async () => {
    const { port, child } = await boot({ ADMIN_USER: 'admin', ADMIN_PASSWORD: 'pa:ss' });
    const get = auth => fetch(`http://127.0.0.1:${port}/admin`,
                              { headers: auth ? { authorization: auth } : {} }).then(r => r.status);
    try {
        assert.equal(await get(null), 401);
        assert.notEqual(await get(basic('admin:pa:ss')), 401, 'colon password must log in');
        assert.equal(await get(basic('admin:pa')), 401, 'prefix before the 2nd colon must not');
        assert.equal(await get(basic('admin:pa:ss:x')), 401, 'extra segment must not');
    } finally { child.kill(); }
});
