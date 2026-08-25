/* Payout loop.
 *
 * Each tick:
 *   1. settlePending() — if a batch is outstanding, ask whether it has been
 *      mined. Confirmed: credit it now (finalizeBatch). Still in the mempool:
 *      nudge Thunder to mine and stop, since its change is unspendable until
 *      then and there is nothing to pay from. Undeterminable: stop and shout.
 *   2. SELECT workers from pps_credits where (accrued - paid) >= minSats
 *      AND no in-flight payout row exists for them.
 *   3. Check the Thunder reserve covers the total plus one fee.
 *   4. Pay them ALL in a single transaction:
 *        a. beginBatch()                — INSERT one in-flight row per worker
 *        b. thunder.transferBatchDetailed() — one broadcast for the batch; on
 *                                         failure abortBatch() and retry next
 *                                         tick with paid_sats untouched
 *        c. attachBatchTxid()           — stamp the txid; the rows STAY in
 *                                         flight, and nobody is credited
 *                                         until step 1 of a later tick sees
 *                                         the transaction in a block
 *
 * Why credit on confirmation rather than on broadcast: `paid` is the pool's
 * record of a debt discharged, and a transaction sitting in a mempool has
 * discharged nothing. Crediting at broadcast made `accrued - paid` understate
 * the real liability for as long as settlement took — observed at 4h+ and
 * 265 BTC on drynet3 — and left no path back if the transaction never landed.
 *
 * Why one transaction rather than one per worker: Thunder only advances when
 * a mainchain block commits to it, and its wallet cannot spend unconfirmed
 * change — so N transactions cost N sidechain blocks, and the queue drains
 * slower than it fills as soon as the pool has more than a handful of miners.
 * Batching makes throughput independent of miner count.
 *
 * The cost is failure isolation: one bad address fails the whole batch rather
 * than just that worker. That is the right trade here — every recipient is a
 * Thunder address the proxy already validated at authorize time, and a batch
 * that fails leaves nobody credited and nobody stranded, so the next tick
 * simply retries. A persistently bad address surfaces as a repeating failure
 * in tx_attempts with the transaction attached.
 *
 * Crash semantics:
 *   - Crash between (a) and (b): rows stay with txid=''. listDue skips those
 *     workers, and listStuck() reports them, until an operator reconciles
 *     (was it broadcast? if yes attach the txid, if no delete the rows). This
 *     is the only genuinely ambiguous state, because a broadcast that
 *     happened is indistinguishable from one that did not.
 *   - Crash between (b) and (c): the same rows with the same remedy, and it
 *     is now a much narrower window — (c) is a local UPDATE, not a credit.
 *   - Crash after (c), before confirmation: nothing special. The rows carry
 *     their txid and the next start settles them normally; this is the
 *     ordinary resting state between a broadcast and a Thunder block.
 *   - Crash mid-finalize: the SQLite transaction commits or rolls back as a
 *     whole, across every worker in the batch. No partial credit. */

import { listDue, listStuck, recordTxAttempt, asRawTx,
         beginBatch, finalizeBatch, abortBatch, attachBatchTxid,
         pendingBatch } from './db.js';

/* Fee model: flat per-tx fee, configurable later. Thunder is a sidechain
 * with relatively low fees; 100 sats covers a one-input one-output tx
 * comfortably on regtest and should be a sane default for early
 * deployments. Will revisit once mainnet fee dynamics are observable. */
const TX_FEE_SATS = 100n;

/* Has `txid` been mined?
 *
 * Thunder gives no single durable answer, so this asks two sources and
 * accepts only POSITIVE evidence from either:
 *
 *   1. get_transaction -> block_hash. Authoritative, but transient: once the
 *      sidechain advances past the block, the txid reads back as `null` —
 *      byte-identical to a txid that never existed. Verified in production:
 *      a payout reported its block_hash while it was the tip and became
 *      indistinguishable from nonexistent one block later.
 *
 *   2. The wallet UTXO set. Each UTXO records the outpoint that created it,
 *      and Thunder only admits confirmed UTXOs — a transfer's change is not
 *      spendable until mined, which is the whole reason payouts serialise.
 *      So an outpoint bearing our txid IS the confirmation, and it is durable:
 *      the change survives until the NEXT payout spends it, and that cannot
 *      happen until this one is finalized.
 *
 * Absence is never read as confirmation, and — just as important — never as
 * eviction. "The node has forgotten it" and "it confirmed a while ago" look
 * the same from here, so inferring eviction and re-queueing the batch would
 * pay it twice. Unknown means unknown; it blocks and asks for a human.
 *
 * Returns 'confirmed' | 'pending' | 'unknown'. */
async function settlementState(thunder, txid, log) {
    const st = await thunder.getTransaction(txid);
    if (st.confirmed) return 'confirmed';

    const w = await thunder.walletUtxos();
    if (w.ok && w.utxos.some(u => u.txid === txid)) return 'confirmed';

    /* Only now can "in the mempool" be trusted as pending: a tx that is both
     * known-unconfirmed and has produced no wallet output is genuinely still
     * settling. */
    if (st.known && !st.error) return 'pending';

    if (st.error) log.warn(`payout: cannot reach Thunder to check ${short(txid)} (${st.error})`);
    else if (!w.ok) log.warn(`payout: cannot read wallet UTXOs to check ${short(txid)} (${w.error})`);
    return 'unknown';
}

const short = txid => `${txid.slice(0, 16)}…`;

/* Settle or wait on the outstanding batch.
 *
 * Only one payout can be in flight at a time. Thunder's wallet picks UTXOs
 * without excluding those already spent by transactions sitting in its own
 * mempool, and a transfer consumes every wallet UTXO and returns the
 * remainder as change — which is unspendable until the first tx is mined. So
 * until it confirms there is nothing to pay from, and every attempt fails
 * identically with
 *
 *     mempool error: can't add transaction, utxo double spent
 *
 * Returns { blocked } — and credits the batch as a side effect when it has
 * confirmed, which is the only place pps_credits.paid_sats ever moves. */
async function settlePending(ctx, log) {
    const { db, thunder } = ctx;
    const pending = pendingBatch(db);
    if (!pending) return { blocked: false };

    const state = await settlementState(thunder, pending.txid, log);

    if (state === 'confirmed') {
        const total = pending.rows.reduce((a, r) => a + r.sats, 0n);
        finalizeBatch(db, pending.rows, TX_FEE_SATS, pending.txid,
                      Math.floor(Date.now() / 1000));
        log.info(`payout: settled ${pending.rows.length} worker(s), ${total} sats, ` +
                 `txid=${short(pending.txid)} confirmed`);
        return { blocked: false, settled: pending.rows.length };
    }

    if (state === 'unknown') {
        /* Deliberately terminal: not credited, not retried, not abandoned.
         * See settlementState — we cannot distinguish confirmed-and-forgotten
         * from never-existed, and the two demand opposite actions. */
        log.error(
            `payout: CANNOT DETERMINE settlement of ${short(pending.txid)} ` +
            `(${pending.rows.length} worker(s), broadcast ` +
            `${Math.floor(Date.now() / 1000) - pending.started_at}s ago). ` +
            'Not crediting and not retrying — payouts are halted until an ' +
            'operator confirms whether this transaction was mined ' +
            `(payout/README.md -> Reconciling by hand). txid=${pending.txid}`);
        return { blocked: true, txid: pending.txid, reason: 'undetermined' };
    }

    /* Deliberately NOT nudging on every tick while we wait — that cost an
     * entire extra sidechain block per payout.
     *
     * Thunder's `mine` snapshots the mempool into a block body BEFORE it takes
     * the miner lock, then parks that snapshot as its BMM request the instant
     * the lock frees. A nudge issued while waiting therefore captures a
     * mempool that predates the NEXT batch, queues behind the in-flight
     * mine(), and becomes the parked request the moment this batch confirms —
     * so the next batch, broadcast seconds later, cannot be in the block that
     * request produces. It waits for the one after.
     *
     * Measured on drynet3: 7 of 7 cycles had Thunder park 14-93s before the
     * broadcast it was supposed to carry, and 42 Thunder blocks yielded only
     * 25 settlements. One nudge per broadcast (see runOnce) keeps the parked
     * body and the batch in step.
     *
     * The exception is a genuine stall: if our post-broadcast request was not
     * carried (a BMM miss) nothing is parked and nothing will re-park, so
     * after stallSec we nudge to recover. That nudge is safe in the sense that
     * matters — this batch is in the mempool, so the body it builds contains
     * it — and rare enough not to reintroduce the every-tick problem. */
    const waited = Math.floor(Date.now() / 1000) - pending.started_at;
    if (waited >= ctx.cfg.nudgeStallSec) {
        await nudgeMine(ctx, log, { reason: `no block in ${waited}s` });
    }

    log.info(`payout: waiting on ${short(pending.txid)} (unconfirmed, ` +
             `${pending.rows.length} worker(s), ${waited}s); skipping tick. ` +
             'Thunder must mine a block before this settles.');
    return { blocked: true, txid: pending.txid, reason: 'unconfirmed' };
}

/* Ask Thunder to attempt BMM.
 *
 * Thunder advances only when a mainchain block commits to it and nothing
 * schedules that, so a broadcast payout otherwise waits for a human to press
 * the button — measured at over four hours in production, with the whole
 * queue stalled behind it. Nudging here rather than on a timer means it fires
 * exactly when something is waiting to settle: no pending batch, no BMM bid
 * spent on an empty block.
 *
 * Called in exactly two places, and the distinction is the whole point:
 *   - immediately after a broadcast, so the body Thunder snapshots contains
 *     the batch we just sent. This one must never be skipped, so it is not
 *     rate-limited — it is already bounded by the settlement cadence.
 *   - to break a stall, when a batch has waited long enough that its request
 *     was evidently not carried. Rate-limited, because repeated nudges are
 *     what put a stale body in the parked slot to begin with (see
 *     settlePending).
 *
 * Best-effort by construction. A failed nudge must not fail the tick — the
 * batch is already broadcast and safe, and a later tick will try again. */
async function nudgeMine(ctx, log, { force = false, reason = '' } = {}) {
    const { thunder, cfg } = ctx;
    if (!cfg.nudgeMine) return false;
    const now = Date.now();
    if (!force && ctx._lastNudgeMs && now - ctx._lastNudgeMs < cfg.nudgeIntervalMs) {
        return false;
    }
    ctx._lastNudgeMs = now;
    try {
        const r = await thunder.mine();
        log.info(`payout: nudged Thunder to mine${reason ? ` (${reason})` : ''}` +
                 (r.completed ? '' : ' — BMM request parked, awaiting a mainchain block'));
        return true;
    } catch (e) {
        log.warn(`payout: mine nudge failed (${e.message}); will retry next tick`);
        return false;
    }
}

export async function runOnce(ctx, log) {
    const { db, thunder, cfg } = ctx;

    /* Settle first, pay second — and both before listDue, because the answer
     * does not depend on who is owed, and on a blocked pool this is the
     * difference between one log line per tick and one per due worker.
     *
     * Settling here is what makes `paid_sats` mean confirmed: the previous
     * batch is credited at the moment we can see it in a block, never at the
     * moment it was sent. */
    let settled = 0;
    if (!cfg.dryRun) {
        const st = await settlePending(ctx, log);
        settled = st.settled ?? 0;
        if (st.blocked) {
            return { attempted: 0, paid: 0, failed: 0, settled,
                     waiting_on: st.txid, reason: st.reason };
        }
    }

    const due = listDue(db, { minSats: cfg.minSats, limit: cfg.maxPerTick });
    if (due.length === 0) {
        log.debug?.('payout: no due workers');
        return { attempted: 0, paid: 0, failed: 0, settled };
    }

    const totalOwed = due.reduce((a, r) => a + r.owed_sats, 0n);
    /* One transaction, one fee — not one per recipient. */
    const totalFees = TX_FEE_SATS;
    log.info(`payout: ${due.length} due, total owed=${totalOwed} sats, fee=${totalFees}`);

    if (!cfg.dryRun) {
        let bal;
        try {
            bal = await thunder.balance();
        } catch (e) {
            log.warn(`payout: balance() failed (${e.message}); skipping tick`);
            return { attempted: 0, paid: 0, failed: 0, settled };
        }
        const avail = BigInt(bal.available_sats ?? bal.total_sats ?? 0);
        if (avail < totalOwed + totalFees) {
            log.warn(
                `payout: reserve short — available=${avail} needed=${totalOwed + totalFees}; ` +
                'partial payouts disabled this tick'
            );
            return { attempted: 0, paid: 0, failed: 0, settled, reserve_short: true };
        }
    }

    const now_s = Math.floor(Date.now() / 1000);

    if (cfg.dryRun) {
        for (const r of due) {
            log.info(`payout: DRY ${r.worker_name} -> ${r.thunder_address} ${r.owed_sats} sats`);
        }
        return { attempted: due.length, paid: 0, failed: 0, settled };
    }

    /* Everyone due goes out in ONE transaction. Paying them one at a time
     * would cost one sidechain block each, and Thunder only advances when a
     * mainchain block commits to it — so the queue would drain slower than it
     * fills as soon as the pool has more than a handful of miners. */
    const batch = due.map(r => ({
        worker_id: r.worker_id, worker_name: r.worker_name,
        address: r.thunder_address, sats: r.owed_sats,
    }));
    const rowIds = beginBatch(db, batch, now_s);

    let res;
    try {
        res = await thunder.transferBatchDetailed(
            batch.map(b => ({ address: b.address, sats: b.sats })), TX_FEE_SATS);
    } catch (e) {
        /* Whether these rows may be released depends on WHERE it failed.
         *
         * create and sign are local: nothing reached the network, so this is a
         * clean abort and the next tick retries with paid_sats untouched.
         *
         * submit splits in two, and the split is the whole point.
         *
         * If the node ANSWERED with an error (e.rpcRejected — a mempool
         * rejection being the everyday case) it ran the method and declined:
         * nothing was broadcast, so this is a clean abort too and the next tick
         * retries normally. That is the common path and it must stay cheap.
         *
         * If we never got an answer, it is not. The node may have accepted the
         * transaction and lost the reply, and on thunder >= 0.17.1 it has
         * DEFINITELY broadcast — create_transfer signs and sends internally,
         * paying the whole total to one address (see transferBatchDetailed).
         * Deleting the in-flight rows there hands the same batch back to the
         * next tick, which rebroadcasts the moment the first transaction
         * confirms and frees its inputs. That pays twice, and repeats every
         * tick until the reserve is empty.
         *
         * So on an unanswered submit the rows STAY, with txid='' — listDue
         * keeps skipping these workers and listStuck reports them. That is
         * exactly the ambiguous state the crash-semantics contract at the top
         * of this file already defines, and the only honest one: a broadcast
         * that happened cannot be told apart from one that did not.
         *
         * An unknown stage is treated as an unanswered submit. The safe
         * reading of "we do not know where it failed" is "it may have gone
         * out". */
        /* create is NOT unconditionally safe. On thunder >= 0.17.1
         * create_transfer signs and broadcasts internally (see
         * transferBatchDetailed), so an unanswered create — timeout, transport
         * failure — may already be on the network, exactly like an unanswered
         * submit. Releasing its rows there is the double-payment this whole
         * branch exists to prevent, just one stage earlier. An ANSWERED create
         * (rpcRejected) is a clean abort: the node ran the method and declined.
         *
         * sign stays unconditional. It only runs on thunder < 0.17.1, where
         * create_transfer merely builds and signing is local — nothing can have
         * reached the network by then. */
        const clean = e.stage === 'sign' ||
                      ((e.stage === 'create' || e.stage === 'submit') &&
                       e.rpcRejected === true);
        if (clean) abortBatch(db, rowIds);
        recordTxAttempt(db, {
            kind: 'payout', status: 'failed', stage: e.stage || 'unknown',
            txid: e.txid ?? null,
            rawTx: asRawTx(e.signed || e.unsigned),
            amountSats: totalOwed, feeSats: TX_FEE_SATS,
            destination: batch.length === 1 ? batch[0].address : `${batch.length} recipients`,
            workerId: batch.length === 1 ? batch[0].worker_id : null,
            error: e.message || String(e),
        });
        if (clean) {
            log.warn(`payout: batch of ${batch.length} failed at ${e.stage} — nothing was ` +
                     `broadcast, retrying next tick: ${e.message}`);
        } else {
            log.error(
                `payout: batch of ${batch.length} failed at ${e.stage || 'an unknown stage'} ` +
                `and MAY have been broadcast: ${e.message}. Leaving ${batch.length} ` +
                'in-flight row(s) with no txid so nobody can be paid twice — these ' +
                'workers stay skipped until an operator reconciles ' +
                '(payout/README.md -> Reconciling by hand).');
        }
        return { attempted: due.length, paid: 0, failed: due.length, settled,
                 ambiguous: !clean };
    }

    recordTxAttempt(db, {
        kind: 'payout', status: 'broadcast', stage: 'submit',
        txid: res.txid, rawTx: asRawTx(res.signed),
        amountSats: totalOwed, feeSats: TX_FEE_SATS,
        destination: batch.length === 1 ? batch[0].address : `${batch.length} recipients`,
        workerId: batch.length === 1 ? batch[0].worker_id : null,
    });

    /* Broadcast is not settlement: stamp the txid and leave the rows in
     * flight. Nobody is credited until a later tick sees the transaction in a
     * block. If this throws, the rows keep txid='' — the ambiguous case that
     * listStuck reports and only an operator can resolve, exactly as before. */
    try {
        attachBatchTxid(db, rowIds, res.txid);
        log.info(`payout: broadcast ${batch.length} worker(s), ${totalOwed} sats, ` +
                 `txid=${res.txid} — awaiting confirmation`);
        for (const b of batch) log.info(`  ${b.worker_name} -> ${b.address} ${b.sats} sats`);
        /* Forced: this is the nudge whose mempool snapshot has to contain the
         * batch above. Letting the rate limiter skip it hands the parked BMM
         * slot to a body that predates the broadcast, which costs a whole
         * sidechain block. */
        await nudgeMine(ctx, log, { force: true, reason: 'batch just broadcast' });
        return { attempted: due.length, paid: 0, broadcast: batch.length,
                 failed: 0, settled, txid: res.txid };
    } catch (e) {
        log.error(`payout: could not record txid=${res.txid} after broadcast: ${e.message}; ` +
                  `${batch.length} in-flight row(s) left without a txid — ` +
                  'manual reconciliation required — see payout/README.md');
        return { attempted: due.length, paid: 0, failed: batch.length, settled };
    }
}

/* Called once at startup. Doesn't auto-resolve; logs anything older
 * than `staleAfterSec` so the operator can investigate. */
export function reportStuck(ctx, log, staleAfterSec = 300) {
    const now_s = Math.floor(Date.now() / 1000);
    const rows = listStuck(ctx.db, staleAfterSec, now_s);
    if (rows.length === 0) return;
    log.warn(`payout: ${rows.length} stuck in-flight row(s) (>${staleAfterSec}s old):`);
    for (const r of rows) {
        const age = now_s - r.started_at;
        const state = r.txid ? `broadcast txid=${r.txid}` : 'no txid';
        log.warn(`  worker=${r.worker_name} sats=${r.sats} age=${age}s ${state} ` +
                 `(reconcile: payout/README.md, row id=${r.id})`);
    }
}

/* How long to wait before the next tick, given what this one did.
 *
 * The payout run itself is a daily batch — that is the cadence miners see,
 * and it is what `intervalMs` means. But two of the states a tick can end in
 * must not wait a day, and both are invisible from the interval alone:
 *
 *   - A batch was broadcast and has not confirmed. Nobody in it is credited
 *     until a later tick sees it in a Thunder block (see settlePending), and
 *     the stall-recovery nudge only fires from a tick. Re-checking on the
 *     daily clock would leave a real, already-sent payout uncredited for up
 *     to 24 hours and would let a missed BMM request sit unrecovered for the
 *     same. So an outstanding batch is re-checked on `settleIntervalMs`.
 *
 *   - A tick tried and got nowhere: the transfer failed, or the reserve did
 *     not cover what is owed. Nothing was broadcast and nobody was credited,
 *     so this is not a completed run and the queue is still full. It comes
 *     back on `retryIntervalMs` rather than tomorrow — long enough not to
 *     spin on a stuck reserve, short enough that a transient RPC failure
 *     doesn't cost a day.
 *
 * An undetermined settlement is deliberately grouped with the retries: it is
 * terminal until an operator reconciles it, and re-logging that at the
 * settle cadence would be pure noise.
 *
 * Everything else — nothing due, or a batch that settled cleanly — waits the
 * full interval. */
export function nextDelayMs(cfg, res) {
    if (res?.reason === 'undetermined')       return cfg.retryIntervalMs;
    if (res?.waiting_on || res?.txid)         return cfg.settleIntervalMs;
    if (res?.failed > 0 || res?.reserve_short) return cfg.retryIntervalMs;
    return cfg.intervalMs;
}

/* setTimeout keeps its delay in a signed 32-bit int. Anything larger wraps
 * and the timer fires IMMEDIATELY — so a config asking for, say, monthly
 * payouts would not slow the loop down, it would turn it into a spin that
 * broadcasts on every tick. Long waits are therefore served in chunks. */
const MAX_TIMEOUT_MS = 2_147_483_647;   /* ~24.8 days */

/* One hop of a possibly-too-long wait: what to hand setTimeout now, and what
 * is still owed afterwards. Pulled out so the clamp is testable without a
 * timer. */
export function timerStep(ms) {
    return ms > MAX_TIMEOUT_MS
        ? { wait: MAX_TIMEOUT_MS, remaining: ms - MAX_TIMEOUT_MS }
        : { wait: ms, remaining: 0 };
}

export const humanMs = ms =>
    ms >= 3600000 ? `${+(ms / 3600000).toFixed(2)}h`
  : ms >= 60000   ? `${+(ms / 60000).toFixed(2)}m`
  :                 `${+(ms / 1000).toFixed(2)}s`;

export function startLoop(ctx, log) {
    let stopped = false;
    let timer = null;

    const tick = async () => {
        if (stopped) return;
        let res = null;
        try {
            res = await runOnce(ctx, log);
        } catch (e) {
            log.error(`payout: unexpected error: ${e.stack || e.message}`);
            /* An exception is not a completed run: come back on the retry
             * clock rather than sleeping off the whole daily interval. */
            res = { failed: 1 };
        }
        if (!stopped) {
            const delay = nextDelayMs(ctx.cfg, res);
            /* Only worth a line when it isn't the ordinary cadence — that is
             * exactly when an operator wondering "why hasn't it paid yet"
             * needs to see which clock the worker is on. */
            if (delay !== ctx.cfg.intervalMs) {
                log.info(`payout: next tick in ${humanMs(delay)}`);
            } else {
                log.debug?.(`payout: next run in ${humanMs(delay)}`);
            }
            arm(delay);
        }
    };

    /* Schedule `tick` in at most MAX_TIMEOUT_MS hops, so a long interval is
     * actually waited out rather than wrapping to zero. */
    const arm = (ms) => {
        const { wait, remaining } = timerStep(ms);
        timer = remaining > 0
            ? setTimeout(() => { if (!stopped) arm(remaining); }, wait)
            : setTimeout(tick, wait);
    };

    tick();

    return {
        stop() {
            stopped = true;
            if (timer) clearTimeout(timer);
        },
    };
}
