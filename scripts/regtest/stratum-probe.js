#!/usr/bin/env node
/* Probe one stratum port: what it advertises, what difficulty it assigns,
 * and — with --flood — whether it enforces the submit ceiling.
 *
 * The pool's own unit tests cover both of those against an in-process server.
 * What they cannot cover is the wiring: that `listener` lines in proxy.conf
 * become real bound sockets, that the port a miner dials decides the
 * difficulty it is handed, and that a flood down a real TCP connection is
 * refused rather than served. That needs a running pool, which is what the
 * regtest e2e has.
 *
 *   node stratum-probe.js --port N [--host H] [--user ADDR]
 *   node stratum-probe.js --port N --flood 1000 [--job-id ID]
 *
 * --job-id overrides the job the flood submits against, and on a low-difficulty
 * chain you want it. The ceiling is checked before a submit's params are read,
 * so a refused one never reaches validation either way -- but the ones UNDER
 * the ceiling do, and on regtest the share target is clamped to a network
 * difficulty so low that about one random hash in two beats it. A flood
 * against the live job would not measure the ceiling, it would mine fifty
 * blocks. Point it at a job id that does not exist and those land as "stale or
 * unknown job" instead, leaving the ceiling as the only thing under test.
 *
 * Prints one line of JSON on stdout — everything else goes to stderr, so the
 * caller can pipe this straight into jq.
 */
'use strict';
const net = require('net');

function arg(name, dflt) {
    const i = process.argv.indexOf(`--${name}`);
    return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}

const HOST    = arg('host', '127.0.0.1');
const PORT    = parseInt(arg('port', '0'), 10);
/* Matches cpuminer.js's default: base58 of twenty zero bytes, which is a
 * valid Thunder address. A pps-classic pool refuses a Bitcoin address on
 * authorize, and that is the mode the regtest e2e runs in. Pass --user for a
 * solo pool, which wants a Bitcoin address instead. */
const USER    = arg('user', '11111111111111111111');
const FLOOD   = parseInt(arg('flood', '0'), 10);
const JOB_ID  = arg('job-id', null);
const TIMEOUT = parseInt(arg('timeout', '30'), 10) * 1000;

if (!PORT) {
    console.error('usage: stratum-probe.js --port N [--flood COUNT]');
    process.exit(2);
}

const out = {
    port: PORT,
    extranonce2_size: null,
    difficulty: null,
    job_id: null,
    submitted: 0,
    refused_too_fast: 0,
    other_response: 0,
    error: null,
};

const sock = net.createConnection({ host: HOST, port: PORT });
sock.setNoDelay(true);

let buf = '';
let nextId = 10;
let floodSent = false;
let floodReplies = 0;

const done = (code) => {
    try { sock.destroy(); } catch { /* already gone */ }
    process.stdout.write(JSON.stringify(out) + '\n');
    process.exit(code);
};

const timer = setTimeout(() => {
    /* A flood that never got all its replies still reports what it saw: the
     * counts are the finding, and a partial read of them is not a failure of
     * the thing under test. */
    if (!out.error && !floodSent) out.error = 'timeout before handshake';
    done(out.error ? 1 : 0);
}, TIMEOUT);
timer.unref?.();

const send = (obj) => sock.write(JSON.stringify(obj) + '\n');

sock.on('error', (e) => { out.error = e.message; done(1); });

sock.on('connect', () => {
    send({ id: 1, method: 'mining.subscribe', params: [] });
    send({ id: 2, method: 'mining.authorize', params: [USER, 'x'] });
});

/* Blast `n` submits with distinct nonces and extranonce2 values, so nothing
 * comes back refused as a duplicate — the only refusal we want to count is
 * the ceiling. Written in one go: the point is to arrive faster than the pool
 * is willing to serve, which a request/response loop would never do. */
function flood(n) {
    floodSent = true;
    const en2Width = (out.extranonce2_size || 8) * 2;
    let chunk = '';
    for (let i = 0; i < n; i++) {
        const en2   = i.toString(16).padStart(en2Width, '0').slice(-en2Width);
        const nonce = (i + 1).toString(16).padStart(8, '0').slice(-8);
        chunk += JSON.stringify({
            id: nextId++, method: 'mining.submit',
            params: [USER, JOB_ID || out.job_id, en2,
                     out.ntime || '60000000', nonce],
        }) + '\n';
        out.submitted++;
    }
    sock.write(chunk);
}

sock.on('data', (d) => {
    buf += d.toString('utf8');
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl);
        buf = buf.slice(nl + 1);
        if (!line.trim()) continue;
        let msg;
        try { msg = JSON.parse(line); } catch { continue; }

        /* An error on subscribe or authorize means no job is ever coming, so
         * waiting for one just burns the timeout and reports nothing useful.
         * Say what the pool said. This is how a probe pointed at a
         * pps-classic pool with a Bitcoin-address username should read --
         * "invalid thunder address", not a 30-second silence. */
        if ((msg.id === 1 || msg.id === 2) && msg.error) {
            const e = msg.error;
            out.error = 'handshake refused: ' +
                (Array.isArray(e) ? String(e[1] || e[0]) : JSON.stringify(e));
            done(1);
            return;
        }

        if (msg.id === 1 && Array.isArray(msg.result)) {
            out.extranonce2_size = msg.result[2];
        } else if (msg.method === 'mining.set_difficulty') {
            out.difficulty = msg.params[0];
        } else if (msg.method === 'mining.notify') {
            out.job_id = msg.params[0];
            out.ntime  = msg.params[7];
            /* Handshake complete: difficulty is known and there is a job to
             * submit against. */
            if (FLOOD > 0 && !floodSent) flood(FLOOD);
            else if (FLOOD === 0) done(0);
        } else if (floodSent && msg.id >= 10) {
            floodReplies++;
            const err = msg.error;
            const text = Array.isArray(err) ? String(err[1] || '')
                       : (err && err.message) ? String(err.message) : '';
            if (text.includes('submitting too fast')) out.refused_too_fast++;
            else out.other_response++;
            if (floodReplies >= out.submitted) done(0);
        }
    }
});
