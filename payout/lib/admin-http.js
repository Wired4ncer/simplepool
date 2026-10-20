/* Small HTTP admin surface for the payout worker.
 *
 * Loopback-only by default (PAYOUT_ADMIN_BIND=127.0.0.1). One endpoint:
 *
 *   POST /tick    — run one payout cycle synchronously, return its result
 *                   as JSON. The dashboard uses this to fire the
 *                   "Trigger payout now" button without waiting for the
 *                   next interval. Requires `Authorization: Bearer
 *                   <PAYOUT_ADMIN_TOKEN>`.
 *   GET  /healthz — liveness, unauthenticated.
 *
 * The loopback bind is NOT a security boundary on its own, which is why
 * /tick carries a shared secret:
 *
 *   - the operator's browser runs on loopback. A body-less POST is a CORS
 *     "simple request": any web page the operator visits can fire it at
 *     127.0.0.1:9080 without a preflight. It cannot read the answer, but
 *     it does not need to — the payout already ran.
 *   - DNS rebinding turns that blind POST into a same-origin one, so the
 *     page could read the response too.
 *   - in the compose stack the worker binds 0.0.0.0 inside the docker
 *     network, so any container on it could trigger payouts.
 *
 * So, in order, every request:
 *   1. carrying an Origin header is refused (403). The dashboard calls us
 *      server-side from Node and never sends one; a browser always does on
 *      a cross-origin POST. This kills the drive-by POST even before the
 *      token is checked, and keeps /healthz out of a browser's reach too.
 *   2. whose Host is not on the allow-list (loopback names, the configured
 *      bind address, PAYOUT_ADMIN_ALLOWED_HOSTS) is refused (403). A
 *      rebound name arrives with the attacker's hostname in Host — this is
 *      the standard rebinding defence and costs nothing.
 *   3. to /tick must carry the token. Compared over sha256 digests with
 *      timingSafeEqual, so neither a prefix nor the length leaks through
 *      response timing (same scheme as the dashboard's credEq()).
 *
 * No token configured means /tick answers 503 rather than running
 * unauthenticated: an install that predates the token keeps booting and
 * paying on its timer, the button says why it stopped working, and the
 * startup log says what to set. Fail closed, but loudly.
 *
 * Deliberately minimal — no express dependency, no framework. */

import { createServer } from 'node:http';
import { createHash, timingSafeEqual } from 'node:crypto';
import { runOnce }      from './payout.js';

const LOOPBACK_HOSTS = ['localhost', '127.0.0.1', '::1'];

/* Hostname part of a Host header, lowercased, port and IPv6 brackets
 * stripped. Returns '' for anything we cannot parse — which then fails
 * the allow-list, as it should. */
export function hostOnly(h) {
    const v = String(h || '').trim().toLowerCase();
    if (!v) return '';
    if (v.startsWith('[')) {
        const end = v.indexOf(']');
        return end > 0 ? v.slice(1, end) : '';
    }
    /* A bare IPv6 literal without brackets has several colons and no port. */
    if (v.indexOf(':') !== v.lastIndexOf(':')) return v;
    return v.split(':')[0];
}

export function allowedHostSet(bind, extra = []) {
    const set = new Set(LOOPBACK_HOSTS);
    /* A wildcard bind is not a name anyone sends in Host. */
    if (bind && bind !== '0.0.0.0' && bind !== '::') set.add(hostOnly(bind));
    for (const h of extra) {
        const n = hostOnly(h);
        if (n) set.add(n);
    }
    return set;
}

function tokenEq(a, b) {
    const h = v => createHash('sha256').update(String(v ?? '')).digest();
    return timingSafeEqual(h(a), h(b));
}

export function startAdminHttp({ port, bind, ctx, log, token = '',
                                 allowedHosts = [], tick = runOnce }) {
    const hosts = allowedHostSet(bind, allowedHosts);
    if (!token) {
        log.warn('admin: PAYOUT_ADMIN_TOKEN is not set — POST /tick will ' +
                 'answer 503 (the dashboard\'s "Trigger payout now" button ' +
                 'stops working; timed payouts are unaffected). Set the same ' +
                 'PAYOUT_ADMIN_TOKEN on the payout worker and the dashboard.');
    }
    const server = createServer(async (req, res) => {
        const send = (status, body) => {
            res.statusCode = status;
            res.setHeader('content-type', 'application/json');
            res.end(JSON.stringify(body));
        };
        if (req.headers.origin !== undefined) {
            return send(403, { ok: false, error: 'cross-origin requests refused' });
        }
        if (!hosts.has(hostOnly(req.headers.host))) {
            return send(403, { ok: false, error: 'host not allowed' });
        }
        if (req.method === 'POST' && req.url === '/tick') {
            if (!token) {
                return send(503, { ok: false,
                    error: 'PAYOUT_ADMIN_TOKEN not set on the payout worker' });
            }
            const h = req.headers.authorization || '';
            if (!h.startsWith('Bearer ') || !tokenEq(h.slice(7), token)) {
                res.setHeader('WWW-Authenticate', 'Bearer realm="simplepool payout"');
                return send(401, { ok: false, error: 'unauthorised' });
            }
            try {
                const result = await tick(ctx, log);
                return send(200, { ok: true, result });
            } catch (e) {
                log.error(`admin: /tick threw: ${e.message}`);
                return send(500, { ok: false, error: e.message });
            }
        }
        if (req.method === 'GET' && req.url === '/healthz') {
            return send(200, { ok: true });
        }
        return send(404, { ok: false, error: 'not found' });
    });
    server.listen(port, bind, () => {
        log.info(`payout admin http listening on ${bind}:${port}`);
    });
    return server;
}
