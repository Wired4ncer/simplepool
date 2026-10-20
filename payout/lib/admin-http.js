/* Small HTTP admin surface for the payout worker.
 *
 * Loopback-only by default (PAYOUT_ADMIN_BIND=127.0.0.1). One endpoint:
 *
 *   POST /tick    — run one payout cycle synchronously, return its result
 *                   as JSON. The dashboard uses this to fire the
 *                   "Trigger payout now" button without waiting for the
 *                   next interval. If the timer loop's run is in progress
 *                   this does NOT start a second one alongside it — two
 *                   overlapping runs credit a confirmed batch twice — and
 *                   returns at once with `result.busy: true`.
 *
 * Deliberately minimal — no auth (loopback bind is the boundary), no
 * express dependency, no framework. */

import { createServer } from 'node:http';
import { runOnceIfIdle } from './payout.js';

export function startAdminHttp({ port, bind, ctx, log }) {
    const server = createServer(async (req, res) => {
        const send = (status, body) => {
            res.statusCode = status;
            res.setHeader('content-type', 'application/json');
            res.end(JSON.stringify(body));
        };
        if (req.method === 'POST' && req.url === '/tick') {
            try {
                const result = await runOnceIfIdle(ctx, log);
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
