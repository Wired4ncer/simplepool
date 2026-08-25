/* Which clock the loop comes back on.
 *
 * The payout run is a daily batch, so `intervalMs` is 24h — but a tick can
 * end in states where waiting a day is wrong, and each one is invisible from
 * the interval alone. The bug this guards against is the obvious one: set
 * PAYOUT_INTERVAL_MS=24h, and a batch that was broadcast but has not
 * confirmed sits uncredited until tomorrow, because settlePending() only runs
 * from a tick. Same for a transfer that failed — nobody was paid and nobody
 * was credited, and the queue just waits.
 *
 * nextDelayMs() is the whole decision, pulled out of the timer so it can be
 * asserted without one.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { nextDelayMs, humanMs, timerStep } from '../lib/payout.js';

const cfg = { intervalMs: 86_400_000, settleIntervalMs: 30_000, retryIntervalMs: 300_000 };

test('a quiet tick waits the full daily interval', () => {
    assert.equal(nextDelayMs(cfg, { attempted: 0, paid: 0, failed: 0, settled: 0 }),
                 cfg.intervalMs);
});

test('a batch that settled cleanly waits the full daily interval', () => {
    assert.equal(nextDelayMs(cfg, { attempted: 0, paid: 0, failed: 0, settled: 3 }),
                 cfg.intervalMs);
});

test('a freshly broadcast batch comes back on the settle clock', () => {
    /* runOnce returns the txid it just broadcast. Nobody in that batch is
     * credited until a later tick sees it in a block. */
    const res = { attempted: 2, paid: 0, broadcast: 2, failed: 0, settled: 0, txid: 'ab12' };
    assert.equal(nextDelayMs(cfg, res), cfg.settleIntervalMs);
});

test('an unconfirmed batch comes back on the settle clock', () => {
    const res = { attempted: 0, paid: 0, failed: 0, settled: 0,
                  waiting_on: 'ab12', reason: 'unconfirmed' };
    assert.equal(nextDelayMs(cfg, res), cfg.settleIntervalMs);
});

test('a failed transfer retries well before tomorrow', () => {
    const res = { attempted: 2, paid: 0, failed: 2, settled: 0 };
    assert.equal(nextDelayMs(cfg, res), cfg.retryIntervalMs);
});

test('a short reserve retries well before tomorrow', () => {
    const res = { attempted: 0, paid: 0, failed: 0, settled: 0, reserve_short: true };
    assert.equal(nextDelayMs(cfg, res), cfg.retryIntervalMs);
});

test('a tick blocked on an unmined mempool retries in minutes, not tomorrow', () => {
    /* Nothing was broadcast and nobody was credited, so this is a tick that
     * got nowhere rather than a completed run. The mempool it is waiting on
     * clears in one Thunder block; sleeping off the daily interval would
     * strand the queue for a day behind it. */
    const res = { attempted: 0, paid: 0, failed: 0, settled: 0, mempool_blocked: 1 };
    assert.equal(nextDelayMs(cfg, res), cfg.retryIntervalMs);
});

test('an undetermined settlement backs off to the retry clock, not the settle clock', () => {
    /* Terminal until an operator reconciles it — re-logging the CANNOT
     * DETERMINE error every 30s for a day would bury everything else. */
    const res = { attempted: 0, paid: 0, failed: 0, settled: 0,
                  waiting_on: 'ab12', reason: 'undetermined' };
    assert.equal(nextDelayMs(cfg, res), cfg.retryIntervalMs);
});

test('a thrown tick is treated as a failure, not as a completed run', () => {
    /* startLoop synthesises { failed: 1 } from the catch block. */
    assert.equal(nextDelayMs(cfg, { failed: 1 }), cfg.retryIntervalMs);
});

test('humanMs renders each scale the operator actually sees', () => {
    assert.equal(humanMs(30_000),     '30s');
    assert.equal(humanMs(300_000),    '5m');
    assert.equal(humanMs(86_400_000), '24h');
});

test('an ordinary delay is handed to setTimeout whole', () => {
    assert.deepEqual(timerStep(86_400_000), { wait: 86_400_000, remaining: 0 });
    assert.deepEqual(timerStep(30_000),     { wait: 30_000,     remaining: 0 });
});

test('a delay past the 32-bit timer ceiling is served in hops', () => {
    /* setTimeout keeps its delay in a signed 32-bit int, so anything larger
     * wraps and fires immediately — a monthly cadence would become a spin
     * that broadcasts every tick rather than once a month. */
    const MAX = 2_147_483_647;
    const monthly = 30 * 24 * 3600 * 1000;          // 2,592,000,000 > MAX
    const first = timerStep(monthly);
    assert.equal(first.wait, MAX);
    assert.equal(first.remaining, monthly - MAX);

    // and the remainder finishes in one more hop, totalling the full wait
    const second = timerStep(first.remaining);
    assert.equal(second.remaining, 0);
    assert.equal(first.wait + second.wait, monthly);
});
