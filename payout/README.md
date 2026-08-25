# simplepool-payout

Thunder payout worker. Drains `pps_credits.accrued_sats - paid_sats` from
the shared SQLite database by issuing Thunder transactions on a cadence,
then writes back `paid_sats`.

Lives outside the C proxy so the hot path (block-template / share
acceptance) is never blocked on Thunder RPC latency. The C proxy is the
only writer of `accrued_sats`; this worker is the only writer of
`paid_sats`. SQLite WAL + a 5-second busy timeout keep them out of each
other's way.

## Run

```
PAYOUT_DB_PATH=../data/shares.db \
THUNDER_RPC_URL=http://127.0.0.1:6009 \
THUNDER_FROM_ADDRESS=<pool base58 thunder address> \
node index.js
```

Dry run — log what would be paid, skip Thunder RPC and DB writes:

```
PAYOUT_DRY_RUN=1 PAYOUT_DB_PATH=../data/shares.db \
  THUNDER_RPC_URL=http://127.0.0.1:6009 \
  THUNDER_FROM_ADDRESS=any \
  node index.js
```

## Config (environment variables)

| var | required | default | meaning |
| --- | --- | --- | --- |
| `PAYOUT_DB_PATH` | yes | — | path to `data/shares.db` (writable) |
| `THUNDER_RPC_URL` | yes | — | Thunder JSON-RPC endpoint, e.g. `http://127.0.0.1:6009` |
| `THUNDER_FROM_ADDRESS` | yes | — | pool reserve address; must equal the dashboard's `POOL_THUNDER_RESERVE_ADDRESS` |
| `THUNDER_RPC_USER` / `THUNDER_RPC_PASS` | no | — | basic-auth if your Thunder node has it (default Thunder build has none) |
| `PAYOUT_INTERVAL_MS` | no | 86400000 (24h) | how often a payout run starts — the batch cadence miners see |
| `PAYOUT_SETTLE_INTERVAL_MS` | no | 30000 | how often an already-broadcast batch is re-checked while it waits for a Thunder block |
| `PAYOUT_RETRY_INTERVAL_MS` | no | 300000 | how long to wait after a tick that tried and got nowhere (transfer failed / reserve short) |
| `PAYOUT_MIN_SATS` | no | 10000 | skip workers below this owed balance |
| `PAYOUT_MAX_PER_TICK` | no | 50 | cap workers paid per scan |
| `PAYOUT_DRY_RUN` | no | — | `1` = log only |
| `PAYOUT_DEBUG` | no | — | `1` = verbose |
| `PAYOUT_NUDGE_MINE` | no | on | `0` = never ask Thunder to mine |
| `PAYOUT_NUDGE_INTERVAL_MS` | no | 120000 | floor between mine attempts |

## What it does each tick

1. **Settle** — if a batch is outstanding, ask Thunder whether it has been
   mined. Confirmed: credit every worker in it now. Still in the mempool:
   nudge Thunder to mine and stop for this tick. Undeterminable: stop and
   log loudly (see below).
2. **Halt if any in-flight row is unresolved** — see *One payout at a time*
3. `SELECT … FROM pps_credits JOIN workers WHERE accrued - paid >= min`,
   excluding anyone with an in-flight row
4. `thunder.mempool()` — bail this tick if Thunder is holding anything unmined
5. `thunder.balance()` — bail this tick if the reserve is short
6. **Broadcast** everyone due in ONE transaction, and stamp its txid onto
   their in-flight rows. Nobody is credited here.

## One payout at a time

Thunder selects UTXOs without excluding those already spent by transactions in
its own mempool, and a transfer consumes every wallet UTXO and returns the
remainder as change — unspendable until it is mined. So a second payout
started while a first is outstanding picks the very inputs the first one
spends, and one of the two ends up live and untracked.

Two rows of defence, because "outstanding" has two shapes:

- A row **with** a txid is a broadcast batch. `settlePending()` waits on it,
  and nothing else goes out until it is seen in a block.
- A row **without** one is a crash around a broadcast, where it is unknown
  whether a transfer went out. That is not settleable — `pendingBatch()`
  ignores it, or the loop would block forever on a phantom txid — but it is
  very much outstanding. `listDue` excludes those *workers*, which is not
  enough: any other worker coming due would start a second payout. So the tick
  halts on `inFlightCount() > 0` until an operator reconciles.

Beyond both, `thunder.mempool()` catches what neither row can describe: a
transfer the ledger has no record of at all.

## Batching

Every due worker goes out in ONE transaction. Thunder advances only when a
mainchain block commits to it and cannot spend the change of an unconfirmed
transaction, so a transaction per recipient costs a sidechain block per
recipient and the queue drains slower than it fills.

The batch is built by asking `create_transfer` for the total and splitting its
payment output into one output per recipient — inputs, utreexo proof and
change untouched, since the proof commits to inputs and not to outputs.

**Whether that is possible is the node's decision.** Thunder ≥ 0.17.1 (commit
`a195d67`) signs and broadcasts inside `create_transfer`, and it takes a single
destination:

```rust
async fn create_transfer(&self, dest: Address, value_sats: u64, fee_sats: u64)
    -> RpcResult<Txid> { … self.app.sign_and_send(tx) … }
```

There is no unsigned transaction to split and no second destination to name, so
on such a node the response arrives with the whole total already paid to
whichever address was listed first — uncallable. The loop therefore pays one
address per transaction *until a transfer proves the node can do better*: a
result that came back unsigned means the splice worked, and from then on every
address goes out together. An unproven node is treated as the restrictive one,
because the cost of guessing wrong is somebody else's balance.

Rigs share an address, so a single miner is one transaction either way. If you
want true multi-address batching against a modern Thunder, the fix belongs
upstream — `create_transfer` needs either a `broadcast: false` flag or a
multi-output form.

The remaining cost is failure isolation: one bad address fails the batch. That
is the right trade — every recipient is an address the proxy validated at
authorize time, and a failed batch credits nobody and strands nobody.

## Three clocks, not one

Payouts run **once a day**. That is `PAYOUT_INTERVAL_MS`, and it is the only
cadence a miner ever sees: a single batched transaction every 24h paying
everyone over `PAYOUT_MIN_SATS`.

The daily interval deliberately does not govern what happens to a batch that
has already gone out, because two of the states a tick can end in are ruined
by a long wait:

| after a tick that… | next tick in | why |
| --- | --- | --- |
| did nothing, or settled a batch cleanly | `PAYOUT_INTERVAL_MS` (24h) | the ordinary daily cadence |
| broadcast a batch, or is still waiting on one | `PAYOUT_SETTLE_INTERVAL_MS` (30s) | nobody in the batch is credited until a tick sees it in a Thunder block, and the stall-recovery nudge only fires from a tick |
| failed to broadcast, or found the reserve short | `PAYOUT_RETRY_INTERVAL_MS` (5m) | nothing was sent and nobody was credited — the run did not happen, so it is retried rather than skipped to tomorrow |
| could not determine a settlement | `PAYOUT_RETRY_INTERVAL_MS` (5m) | terminal until an operator reconciles; re-logging it every 30s for a day buries everything else |

`nextDelayMs()` in [lib/payout.js](lib/payout.js) is the whole decision, and
[test/cadence.test.js](test/cadence.test.js) pins each row of that table.

To force a run without waiting for the next one, use the dashboard's
**Trigger payout now** button (or `POST /payout/run` on the worker's admin
HTTP surface) — restarting the service also ticks immediately.

## `paid` means mined, not sent

`pps_credits.paid_sats` moves only when a transaction has been observed in a
block. A payout sitting in a mempool has discharged no debt, so counting it as
paid makes `accrued - paid` understate what the pool actually owes — measured
at 265 BTC for over four hours on drynet3 — and leaves no way back if the
transaction never lands.

Telling "confirmed" from "gone" is the hard part, because Thunder offers no
single durable answer. Two sources are consulted and only **positive**
evidence from either is accepted:

- `get_transaction` → `block_hash`. Authoritative but transient: once the
  sidechain moves past the block, a long-confirmed txid reads back as `null`,
  byte-identical to one that never existed.
- The **wallet UTXO set**, where each UTXO records the outpoint that created
  it. Thunder admits only confirmed UTXOs, so an outpoint bearing our txid is
  itself the confirmation — and it is durable, because that change output
  survives until the next payout spends it, which cannot happen until this one
  is finalized.

Silence is never read as confirmation, and never as eviction either. Inferring
"it must have been dropped" from a forgotten txid would re-queue a batch that
had already been paid.

## Nudging Thunder

Thunder advances only when a mainchain block commits to it, and nothing
schedules that — so without help a broadcast payout waits for a human to press
a button, and the whole queue waits behind it. The loop calls Thunder's `mine`
in exactly two places, and **when** it fires matters as much as that it does.

### Once per broadcast, not once per tick

`mine` builds a block body from the mempool *before* it takes the miner lock,
then parks that snapshot as its BMM request the moment the lock frees:

```rust
let body = types::Body::new(Vec::new(), coinbase);   // mempool snapshot HERE
let mut miner_write = miner.write().await;           // then block on the lock
miner_write.attempt_bmm(bribe.to_sat(), 0, header, body)
```

So a nudge issued *while waiting* captures a mempool that predates the next
batch, queues behind the in-flight `mine`, and becomes the parked request the
instant the current batch confirms. The next batch — broadcast seconds later —
cannot be in the block that request produces, so it waits for the one after.
Every payout then costs two sidechain blocks instead of one.

Measured on drynet3 before this was fixed: Thunder parked its request 14–93s
*ahead* of the broadcast it was meant to carry in 7 of 7 cycles, and 42 Thunder
blocks produced only 25 settlements.

The loop therefore nudges:

- **right after a broadcast**, so the snapshot contains the batch just sent.
  Never rate-limited — it is already bounded by the settlement cadence.
- **to break a stall**, once a batch has sat unconfirmed for
  `PAYOUT_NUDGE_STALL_SEC` (default 300s), which means its request was not
  carried and nothing will re-park. Rate-limited by
  `PAYOUT_NUDGE_INTERVAL_MS`.

Do not lower `PAYOUT_NUDGE_STALL_SEC` towards the tick interval — that
reintroduces the stale-snapshot problem the split exists to avoid.

It fires only while something is genuinely waiting to settle, so an idle pool
spends no BMM bids on empty blocks. A failed nudge never fails the tick.

## At-most-once payout protocol

1. `INSERT INTO payouts_in_flight (worker_id, sats, txid='')` — one row per
   worker, before Thunder is touched. `listDue()` skips any worker with an
   in-flight row.
2. `transferBatchDetailed(recipients, fee)` — broadcast. Three RPCs under the
   hood (`create_transfer` → `sign_transaction` → `submit_transaction`); only
   the last can put a tx on the network. On clean failure (an error before a
   txid is returned) the rows are DELETEd and the workers are eligible next
   tick.
3. `attachBatchTxid()` — stamp the txid. The rows **stay** in flight.
4. On a later tick, once the transaction is confirmed: in ONE SQLite
   transaction across the whole batch, `paid_sats += sats`, append to
   `payouts`, DELETE the in-flight rows.

Crash semantics:

| crash point | row state | action |
| --- | --- | --- |
| after (1), before (2) | `txid=''` | manual: did the broadcast happen? |
| after (2), before (3) | `txid=''` | manual: same question, narrower window |
| after (3), before (4) | `txid` set | none — this is the ordinary waiting state |
| inside (4) | atomic across the batch — fully applied or fully rolled back | none |

`reportStuck()` logs in-flight rows older than 5 minutes **that have no
txid**. Rows with a txid are routinely hours old — Thunder produces only a
handful of blocks a day — and reporting them would bury the one row that
actually needs a human.

### Reconciling by hand

Two situations need an operator. Neither is auto-resolved, because both turn
on a question only a human can answer: *did this transaction make it onto the
sidechain?*

**A row with no txid.** The worker died around the broadcast, so it is unknown
whether anything went out. **Payouts are halted** while it sits there — see
*One payout at a time* — so this is the one to resolve first. Check the Thunder
node, then:

```sh
# the tx is live or mined — adopt it, and the normal settle path takes over
sqlite3 data/shares.db "UPDATE payouts_in_flight SET txid = '<txid>' WHERE id = <id>;"

# it never went out — release the workers to be paid again next tick
sqlite3 data/shares.db "DELETE FROM payouts_in_flight WHERE id = <id>;"
```

**`Thunder is holding N unmined transaction(s)`.** Settlement found nothing
outstanding, yet Thunder's mempool is not empty — so there is a transfer out
of the pool wallet that this ledger has no record of, spending the UTXOs the
next payout would use. Every batch built against it dies as `utxo double
spent` at the *create* stage, which looks like a clean abort, so an unguarded
loop retries forever and pays nobody. The gate stops that, and deliberately
does **not** mine: a block here would confirm a transfer the pool never
recorded.

```sh
CLI=…/thunder_app_cli
# what is in there, and where is it going?
sudo -u forknet $CLI get-block-template | jq '.block.body.transactions[].outputs'
```

If it is meant to go out, mine it (`$CLI mine`) and reconcile the ledger by
hand afterwards — nobody in it is credited. If it is not, drop it and the next
tick rebuilds the batch correctly:

```sh
sudo -u forknet $CLI remove-from-mempool <txid>
```

**`CANNOT DETERMINE settlement`.** The loop can see neither the transaction
nor any wallet UTXO from it, so it has halted payouts rather than guess.
Establish the truth first — the recipients' balances are the ground truth:

```sh
CLI=…/thunder_app_cli
sudo -u forknet $CLI get-transaction <txid>      # non-null = still in mempool
sudo -u forknet $CLI get-wallet-utxos | grep <txid>
```

If it was mined, finalize the batch by hand (all rows sharing the txid, in one
transaction). If it truly never landed, `DELETE` those rows and the workers are
paid again on the next tick. Do not delete rows you have not positively shown
to be unmined — that is how a batch gets paid twice.

### Retracting a batch

**Only when the distribution is wrong — never because it is slow.**

A broadcast batch that has not confirmed is almost always a *correct* payout
waiting for a Thunder block, and waiting costs nothing: accruals keep
accumulating in `pps_credits`, so whoever came due meanwhile is paid by the
next tick once the block lands. The loop halts and says so; the right response
is to get a block, not to rewrite the payment.

Retract only when the transaction pays the wrong people or the wrong amounts —
the failure mode fixed in this PR, where a node that broadcasts inside
`create_transfer` paid a whole multi-address batch to one address.

**Two things to understand before doing it.**

*Retracting does not cancel a BMM attempt.* `app.mine()` snapshots the mempool
into a block body **before** requesting BMM, and connects that snapshot if the
commitment lands:

```rust
let BlockTemplate { bribe, header, body, .. } = self.build_block_template(fee).await?;
let bmm_txid = miner_write.attempt_bmm(bribe.to_sat(), 0, header, body).await?;
if let Some((main_hash, header, body)) = miner_write.confirm_bmm().await? {
    self.node.submit_block(main_hash, &header, &body).await?   // the snapshotted body
```

`remove_from_mempool` touches the node's mempool only. If a request is already
in flight the transaction can still land afterwards. Check Thunder's log for an
`attempt BMM: created TX` with no following `confirm BMM` line — that is a
parked request, and it is worth waiting for it to resolve first.

*You cannot pay twice.* A transfer consumes every wallet UTXO, so any rebuilt
payout spends the same inputs and is a double-spend of the original. Only one
can ever be in the chain. What is at risk is crediting the wrong distribution
if the retracted one lands after you have moved on.

```sh
CLI=…/thunder_app_cli

sudo systemctl stop simplepool-payout          # or it builds into the gap

# 1. it must be unconfirmed: tx non-null, block_hash null
sudo -u forknet $CLI get-transaction <txid>

# 2. drop it, and prove it is gone
sudo -u forknet $CLI remove-from-mempool <txid>
sudo -u forknet $CLI get-transaction <txid>                     # expect null
sudo -u forknet $CLI get-block-template | jq '.block.body.transactions|length'

# 3. release the rows so the batch is rebuilt from current balances
sqlite3 data/shares.db "DELETE FROM payouts_in_flight WHERE txid = '<txid>';"

sudo systemctl start simplepool-payout
```

Nobody was credited, so nothing in `pps_credits` needs undoing — that is the
whole point of settling on confirmation rather than on broadcast.

Afterwards, watch for the retracted txid coming back. If it ever appears as a
wallet UTXO outpoint, it won and the replacement is dead, so the ledger needs
correcting by hand:

```sh
sudo -u forknet $CLI get-wallet-utxos | grep <old-txid>
```

## Block-withholding audit (`audit.js`)

PPS-specific fraud check. A worker can submit valid shares to collect
PPS payouts and then quietly *not submit* the ones that would be
blocks, keeping the on-chain reward off-pool. Detection is statistical:
each worker's accrued share difficulty as a fraction of the pool's
total predicts what fraction of blocks they should have found.

```
PAYOUT_DB_PATH=../data/shares.db node audit.js
PAYOUT_DB_PATH=../data/shares.db node audit.js --window-hours 168
PAYOUT_DB_PATH=../data/shares.db node audit.js --json    # for cron / slack
```

For each worker over the window:
- **expected_solutions** = `pool_solutions * (worker_accrued / pool_accrued)`
- **actual_solutions**   = network-target solutions they actually submitted
- **z**                  = `(expected - actual) / sqrt(expected)`

A worker is flagged `suspicious` when:
- `expected_solutions >= 5` (below this, randomness dominates), AND
- `z >= 3` (≈1-in-740 false-positive rate under honest mining)

**Solutions, not blocks.** These counts come from `shares.is_block` and are
deliberately *not* filtered to confirmed blocks the way the dashboard's block
counts are. The question here is whether a miner is quietly discarding the
submission that solves a block, so what matters is what they submitted — a
miner whose solution the node refused, or whose block was reorged out, has
withheld nothing. Filtering on confirmed would flag honest miners on exactly
the low-difficulty chains where orphans are routine. Expect this number to
exceed the dashboard's "Blocks found"; both are correct, and they answer
different questions.

Run on a cron and pipe the `--json` output to your alert sink of choice.
The audit reads the DB only — safe to run while the proxy is writing.

## Known gaps (deliberate)

- **Flat 100-sat fee.** Will need a smarter fee model once Thunder
  fee dynamics are observable. Currently hardcoded in `lib/payout.js`.
- **No confirmation tracking.** Thunder's RPC doesn't expose per-tx
  confirmation counts; we treat a successful broadcast as final.
  That's how the rest of Thunder tooling works today.
- **Manual reconciliation.** Crashes between broadcast and finalize
  need an operator. The alternative (auto-reconcile via Thunder
  mempool/chain lookup) is fragile without a getrawtransaction-style
  endpoint and is left as a follow-up.
