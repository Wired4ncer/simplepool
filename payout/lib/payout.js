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
         pendingBatch, inFlightCount } from './db.js';

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

/* Everyone due, grouped by the address their money actually goes to, biggest
 * debt first.
 *
 * Batching every due worker into ONE transaction is the goal and stays the
 * goal: Thunder advances only when a mainchain block commits to it, so a
 * transaction per recipient costs a sidechain block per recipient and the
 * queue drains slower than it fills.
 *
 * Whether that is achievable is the node's decision, not ours. The batch is
 * built by asking `create_transfer` for the total and splitting its payment
 * output into one per recipient — and Thunder >= 0.17.1 (commit a195d67)
 * signs and broadcasts inside `create_transfer`, which takes exactly ONE
 * destination. On such a node there is nothing to split: the response arrives
 * with the whole total already paid to whichever address was listed first, and
 * no error can call that back. It fired twice on avonpool on 2026-08-24 and
 * left an untracked transaction sitting on the wallet's only UTXO.
 *
 * So grouping is the fallback for a node that cannot build the batch, and
 * runOnce() uses it only until the node has proved otherwise — see
 * `canBatchAcrossAddresses`. Rigs share an address either way, so the common
 * case is one transaction regardless.
 *
 * Sorted by debt so the largest liability clears first; ties keep listDue's
 * order. */
export function groupByAddress(rows) {
    const byAddress = new Map();
    for (const r of rows) {
        const g = byAddress.get(r.thunder_address);
        if (g) { g.rows.push(r); g.owed += r.owed_sats; }
        else byAddress.set(r.thunder_address,
                           { address: r.thunder_address, rows: [r], owed: r.owed_sats });
    }
    return [...byAddress.values()]
        .sort((a, b) => (a.owed < b.owed ? 1 : a.owed > b.owed ? -1 : 0));
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

    /* One payout transaction at a time, and "at a time" is counted in rows, not
     * in settleable batches.
     *
     * settlePending() has just handled every row that carries a txid. What can
     * still be here is a row with txid='' — a crash around a broadcast, where
     * it is unknown whether a transfer went out. listDue excludes those
     * WORKERS, but not the situation: any other worker coming due would start
     * a second payout, and Thunder picks its inputs without excluding what its
     * own mempool has already spent, so the two collide on the same UTXOs. One
     * of them is then live and untracked, which is the state this whole file
     * exists to avoid.
     *
     * reportStuck() and payout/README.md tell the operator how to resolve it;
     * until they do, nothing new goes out. */
    if (!cfg.dryRun) {
        const outstanding = inFlightCount(db);
        if (outstanding > 0) {
            log.error(
                `payout: ${outstanding} in-flight payout row(s) are unresolved — no txid, ` +
                'so it cannot be told whether a transfer went out. Not starting another ' +
                'payout: a second transaction would spend the same UTXOs. Reconcile first ' +
                '(payout/README.md -> Reconciling by hand).');
            return { attempted: 0, paid: 0, failed: 0, settled, reason: 'in-flight-unresolved' };
        }
    }

    const allDue = listDue(db, { minSats: cfg.minSats, limit: cfg.maxPerTick });
    if (allDue.length === 0) {
        log.debug?.('payout: no due workers');
        return { attempted: 0, paid: 0, failed: 0, settled };
    }

    /* Batch everyone into one transaction where the node can build one.
     *
     * `_nodeBroadcastsOnCreate` is what a previous transfer proved about this
     * Thunder: false means create_transfer handed back an unsigned transaction
     * and the splice worked, so a multi-address batch is safe and every due
     * worker goes out together. true means it signed and broadcast on its own,
     * so only one destination is expressible. undefined means no transfer has
     * come back yet — and an unproven node is treated as the dangerous one,
     * because the cost of guessing wrong is somebody else's balance.
     *
     * Learned from the result rather than probed, because every probe of this
     * question is itself a transfer. One address goes out first; the answer
     * arrives with it. */
    const groups = groupByAddress(allDue);
    const canBatchAcrossAddresses = ctx._nodeBroadcastsOnCreate === false;
    const due    = canBatchAcrossAddresses ? allDue : groups[0].rows;
    const queued = canBatchAcrossAddresses ? 0 : groups.length - 1;

    const totalOwed = due.reduce((a, r) => a + r.owed_sats, 0n);
    /* One transaction, one fee — not one per recipient. */
    const totalFees = TX_FEE_SATS;
    log.info(`payout: ${due.length} due, total owed=${totalOwed} sats, fee=${totalFees}` +
             (queued > 0 ? ` (this node cannot batch across addresses, so paying ` +
                           `${groups[0].address} now; ${queued} more address(es) follow ` +
                           'on later ticks)' : ''));

    /* Do not build a transfer against a Thunder that cannot settle one.
     *
     * settlePending() has already established that WE have nothing
     * outstanding, so anything still in the mempool is a transaction the
     * ledger knows nothing about — and it is spending the very UTXOs the next
     * transfer would pick, because Thunder selects inputs without excluding
     * those its own mempool has already spent. Every attempt then fails with
     *
     *     mempool error: can't add transaction, utxo double spent
     *
     * at the 'create' stage, which reads as a clean abort, so the loop retries
     * on the next tick and the next, indefinitely. That is the shape of the
     * avonpool outage: 216 identical failures across 24 hours, nobody paid.
     *
     * Deliberately NOT nudged. Mining is what confirms that transaction, and
     * an untracked transfer out of the pool wallet is exactly the thing not to
     * confirm on a timer — on avonpool it would have paid one miner's balance
     * to another. A block here moves money, so it is the operator's call:
     * either mine it deliberately, or drop it with `remove-from-mempool` and
     * let the loop rebuild the batch correctly.
     *
     * An unreachable node is deliberately not treated as blocked: the transfer
     * would fail safely on its own, and refusing to pay because a diagnostic
     * RPC is down would be a self-inflicted outage. */
    if (!cfg.dryRun) {
        const mp = await thunder.mempool();
        if (mp.ok && mp.count > 0) {
            log.error(
                `payout: Thunder is holding ${mp.count} unmined transaction(s) that this ` +
                'ledger has no record of, and they are spending the wallet\'s UTXOs — ' +
                'payouts are halted. Not creating a batch (it could only fail as a ' +
                'double spend) and not mining one either, because mining is what would ' +
                'confirm a transfer this pool never recorded. An operator must decide: ' +
                'mine it if it is meant to go out, or drop it with remove-from-mempool ' +
                'and let the next tick rebuild the batch ' +
                '(payout/README.md -> Reconciling by hand).');
            return { attempted: 0, paid: 0, failed: 0, settled, mempool_blocked: mp.count };
        }
        if (!mp.ok) {
            log.debug?.(`payout: could not read Thunder's mempool (${mp.error}); proceeding`);
        }
    }

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
        /* Only one thing produces a broadcastTxid: create_transfer signing and
         * sending on its own. Whatever else went wrong, the node has answered
         * the batching question, and it must never be asked again. */
        if (e.broadcastTxid) ctx._nodeBroadcastsOnCreate = true;

        recordTxAttempt(db, {
            kind: 'payout', status: e.broadcastTxid ? 'broadcast' : 'failed',
            stage: e.stage || 'unknown', txid: e.broadcastTxid || null,
            rawTx: asRawTx(e.signed || e.unsigned),
            amountSats: totalOwed, feeSats: TX_FEE_SATS,
            destination: batch.length === 1 ? batch[0].address : `${batch.length} recipients`,
            workerId: batch.length === 1 ? batch[0].worker_id : null,
            error: e.message || String(e),
        });

        /* A failure that still put a transaction on the network is not an
         * abort, and must never be treated as one. Dropping the in-flight rows
         * would leave the ledger with no record of a live transfer, so the next
         * tick would build another against UTXOs that transaction already
         * spends, and every tick after that would too. Keeping the rows against
         * its txid hands the batch to settlePending(), which blocks the queue
         * and says so once — the difference between one alert and 216 silent
         * retries. Nobody is credited either way: settlement still requires
         * seeing it in a block. */
        if (e.broadcastTxid) {
            attachBatchTxid(db, rowIds, e.broadcastTxid);
            log.error(
                `payout: batch of ${batch.length} FAILED WITH FUNDS ON THE NETWORK ` +
                `(txid=${e.broadcastTxid}): ${e.message} — holding the batch against ` +
                'that txid and halting payouts. Nobody is credited until an operator ' +
                'reconciles (payout/README.md -> Reconciling by hand).');
            return { attempted: due.length, paid: 0, failed: 0, settled,
                     waiting_on: e.broadcastTxid, reason: 'broadcast-unintended' };
        }

        /* The whole batch fails together, which is the point: no worker is
         * credited for a transaction that did not go out. */
        abortBatch(db, rowIds);
        log.warn(`payout: batch of ${batch.length} ${e.stage || 'transfer'} failed: ${e.message}`);
        return { attempted: due.length, paid: 0, failed: due.length, settled };
    }

    /* What this transfer proved about the node, for the next tick's batching
     * decision. `broadcastByNode` is set only on the create-and-broadcast
     * path; an unsigned transaction that we spliced, signed and submitted
     * leaves it undefined, and that is the node that can batch. */
    ctx._nodeBroadcastsOnCreate = res.broadcastByNode === true;

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
    /* Blocked on someone else's unmined transaction: nothing was broadcast and
     * nobody was credited, so this is a tick that got nowhere, not a completed
     * run. It comes back on the retry clock — sleeping off a daily interval
     * would strand the queue for a day behind a mempool that clears in
     * minutes. */
    if (res?.mempool_blocked)                 return cfg.retryIntervalMs;
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
