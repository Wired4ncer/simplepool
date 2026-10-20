/* "Trigger payout now" against a worker that is already mid-run.
 *
 * The payout worker runs one cycle at a time: two overlapping runs credit a
 * confirmed batch twice. So its /tick answers `result.busy` instead of
 * starting a second run beside the timer's. The button must say so — an
 * all-zero "attempted=0 broadcast=0 …" summary reads as "ran, nothing due",
 * which is not what happened.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';

import { triggerPayout } from '../lib/actions.js';

async function worker(body) {
    const server = createServer((req, res) => {
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify(body));
    });
    await new Promise(r => server.listen(0, '127.0.0.1', r));
    return { url: `http://127.0.0.1:${server.address().port}`, close: () => server.close() };
}

test('a busy worker is reported as a run already in progress', async () => {
    const w = await worker({ ok: true,
        result: { attempted: 0, paid: 0, failed: 0, settled: 0, busy: true } });
    try {
        const r = await triggerPayout({ payoutAdminUrl: w.url });
        assert.equal(r.ok, true);
        assert.match(r.msg, /already in progress/);
        assert.doesNotMatch(r.detail, /attempted=/);
    } finally { w.close(); }
});

test('an idle worker still reports the tick summary', async () => {
    const w = await worker({ ok: true,
        result: { attempted: 2, paid: 0, broadcast: 2, failed: 0, settled: 0, txid: 'ab' } });
    try {
        const r = await triggerPayout({ payoutAdminUrl: w.url });
        assert.equal(r.ok, true);
        assert.equal(r.msg, 'payout tick fired');
        assert.match(r.detail, /attempted=2 broadcast=2/);
    } finally { w.close(); }
});
