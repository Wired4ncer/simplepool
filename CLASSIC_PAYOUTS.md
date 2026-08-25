# `pps-classic` — traditional coinbase + operator-driven deposits

This is the design behind `pool_mode = pps-classic`, the pool's
Thunder-paying PPS mode. It is implemented and running; this doc
explains the shape and why it looks the way it does.

## Why not deposit straight from the coinbase

The original design (`pool_mode = pps`, since removed) embedded a BIP300
drivechain deposit in every coinbase, so the pool would never custody
BTC. End-to-end validation on regtest **and** on the live forknet server
proved that the LayerTwo-Labs enforcer **does not credit coinbase
outputs as drivechain deposits** — the block is accepted into the chain
but the sidechain Ctip never moves. A side-by-side test:

| deposit shape | Ctip moved? |
| --- | --- |
| canonical `CreateDepositTransaction` (spends mature UTXOs) | yes |
| simplepool coinbase, `OP_DRIVECHAIN(9)` output | **no** |

The rule requires the deposit tx to spend real, mature, spendable
UTXOs; a coinbase does not qualify. This is consensus-level and unlikely
to change, so that mode stranded the block reward and was deleted along
with its coinbase builders. `pps-classic` is what all real drivechain
mining pools converge on instead.

## The flow

1. **Coinbase pays the pool's BTC wallet** — a normal solo-style output
   to `pool_btc_address` for the full net-of-operator-fee reward. The
   pool does briefly custody BTC (a design tradeoff, but the only path
   that actually works). The operator fee stays in BTC, paid to
   `operator_address` out of the same coinbase.
2. **Operator triggers batched deposits to Thunder** via the admin
   dashboard. Each deposit is a real `CreateDepositTransaction` that
   spends accumulated pool UTXOs → OP_DRIVECHAIN + OP_RETURN. This DOES
   credit the Ctip on Thunder.
3. **The payout worker drains the Thunder reserve to miners** — the
   `pps_credits.accrued_sats - paid_sats` sweep under [payout/](payout/),
   with an at-most-once protocol backed by the `payouts_in_flight`
   write-ahead table.

Stratum usernames are **bare base58 Thunder addresses** — the
`s9_<base58>_<hex6>` deposit-format wrapper is rejected, because Thunder
doesn't recognize it at the byte level and a miner authorized with it
would accrue unpayable PPS balance. Validated in
[src/thunder.c](src/thunder.c).

Each accepted share credits the worker's `pps_credits.accrued_sats` at
`rate * difficulty`, truncated to whole sats, where the rate is **derived
from the live template** rather than configured:

```
gross = coinbasevalue / network_difficulty      # fair value of one diff-1 share
rate  = gross * (1 - fee_bps / 10000)           # what the pool actually pays
```

It is recomputed on every template change, so it tracks difficulty and block
value automatically. `fee_bps` is the only fee knob.

Both numbers are written onto the share row — `credited_sats` and
`rate_used` — so the credit can be re-derived later without knowing what the
rate happened to be at the time. See [Verifying the ledger](#verifying-the-ledger).

## Config

```
pool_mode = pps-classic

# Where mined BTC lands. Should be a wallet the operator controls
# and that has enough age/maturity for later deposit-tx use.
pool_btc_address = bc1q...

# operator_address and fee_bps behave exactly as in solo mode.
```

Do **not** set `pps_sats_per_diff`. It pins the rate to a constant that
cannot track difficulty, is taken net of fee (so it silently bypasses
`fee_bps`), and drifts toward paying more than each share earns as
difficulty moves. It exists only as an escape hatch; the proxy logs a
warning when a pinned rate implies a fee more than 25 bps from `fee_bps`.

### Where the fee lands

`fee_bps` is applied in two independent places, and whether that is one
deduction or two depends on your addresses:

- **The coinbase** splits `fee_bps` of the block to `operator_address` and
  the rest to `pool_btc_address`.
- **The PPS rate** is reduced by `fee_bps` before miners are credited.

When `operator_address == pool_btc_address` the split is a no-op — the pool
receives the whole block — and the fee is collected once, via the rate. The
pool then runs with a `fee_bps` margin over its expected payout, which is
the buffer that absorbs bad luck.

When they differ, the operator takes the cut on-chain per block and the pool
entity runs at **break-even in expectation** with no buffer, while still
bearing full PPS variance. Both arrangements are coherent; pick deliberately,
because the second one turns a run of bad luck into a shortfall.

The pool's Thunder reserve address is **not** a proxy config key — the
coinbase never touches Thunder. It is set on the dashboard
(`POOL_THUNDER_RESERVE_ADDRESS`) and on the payout worker
(`THUNDER_FROM_ADDRESS`), which are the two components that actually
speak to Thunder.

## Coinbase builder

`src/coinbase.c`: `coinbase_build_split` emits
`[pool_btc_p2wpkh, operator_fee, witness_commit]` — that IS the
classic-mode layout, called with `miner_address = pool_btc_address` in
`stratum.c`. When the backend dictates the coinbase (the CUSF enforcer
path), `coinbase_build_from_template` rewrites the spendable output the
same way while preserving the BIP301 commitment outputs byte-for-byte.

## Database

- `pps_credits` — one row per worker: `worker_id`, `accrued_sats`,
  `paid_sats`, `last_updated`. The C proxy only INCREMENTs
  `accrued_sats`; the payout worker only writes `paid_sats`.
- `payouts_in_flight` — write-ahead log for the at-most-once payout
  protocol. INSERT before broadcast; one atomic transaction after
  (`txid` write + `paid_sats +=` + DELETE row). `listDue()` skips any
  worker with an in-flight row, so a crash mid-payout can't double-pay.
  Runbook in [payout/README.md](payout/README.md).
- `deposits` — one row per operator-triggered Thunder deposit:

```sql
CREATE TABLE IF NOT EXISTS deposits (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts            INTEGER NOT NULL,           -- unix seconds
  btc_txid      TEXT    NOT NULL,           -- mainchain deposit tx
  sats_deposited INTEGER NOT NULL,
  fee_sats      INTEGER NOT NULL,
  thunder_recipient TEXT NOT NULL,          -- deposit-format address
  ctip_seq_before INTEGER,                  -- for audit trail
  ctip_seq_after  INTEGER,
  notes         TEXT
);
CREATE INDEX IF NOT EXISTS deposits_ts_idx ON deposits(ts);
```

## Admin controls

The **"Deposit to Thunder"** card on `/admin/deposits`:

- Shows the pool wallet's spendable balance, what's waiting on-chain vs
  already deposited (from the `deposits` table), and the current Thunder
  reserve balance.
- **POST /admin/deposit** — fields `amount_sats`, `fee_sats`. Calls the
  enforcer's gRPC `WalletService/CreateDepositTransaction` with the
  pool's Thunder reserve address as the destination. On success,
  `INSERT INTO deposits` + refresh reserve balance.
- Same basic auth as the read-only admin view, plus an `Origin`-header
  check.

## Block-withholding audit

[payout/audit.js](payout/audit.js) — standalone read-only CLI. For each
worker over a window:
`expected_solutions = pool_solutions × (worker_accrued_diff / pool_accrued_diff)`;
`z = (expected − actual) / sqrt(expected)`. Flags suspicious when
`expected ≥ 5` and `z ≥ 3` (~1-in-740 false positives under honest
Poisson sampling). No schema changes; safe to run while the proxy is
writing.

Counted from `shares.is_block`, **not** from confirmed blocks. Withholding is
about what a miner submitted; a solution the node refused or the chain reorged
out was still submitted. So this number legitimately exceeds the "Blocks
found" the dashboard reports, which counts only what reached the chain.

## Locked-in decisions

- **Manual vs auto deposits.** Manual — the operator clicks a button per
  deposit. An auto-batching worker is a later improvement; no schema
  change required, just a new service that posts to `/admin/deposit`.
- **One pool BTC address vs many.** One is simpler and matches how
  drivechain-launcher wallets typically hold funds. A rolling set of
  addresses is a follow-up.
- **Deposit fee precision.** Locked to
  `enforcer.WalletService.CreateDepositTransaction`'s `fee_sats` field.
  The operator eyeballs the current fee market and picks a number.
- **Thunder payout fee.** Flat 100 sats for now; needs revisiting once
  Thunder fee dynamics are observable.
- **Confirmation tracking.** Thunder's RPC doesn't expose per-tx
  confirmation counts, so a successful broadcast is treated as final.
  Matches the rest of the Thunder tooling today.
- **Stuck in-flight rows** are reconciled by the operator, not
  automatically — we can't safely tell "broadcast didn't happen" from
  "broadcast happened, finalize crashed" without a Thunder-side
  mempool/chain lookup.

## Verifying the ledger

The audit page reports what the pool credited. These four queries **check**
it, and can be run by anyone with a copy of `shares.db` — no trust in the
dashboard required:

```sql
-- 1. Arithmetic. Every credited share must re-derive from the pair stored on
--    its own row. Nothing current is consulted, so this holds no matter how
--    far the rate has since moved.
SELECT COUNT(*) FROM shares
 WHERE rate_used > 0
   AND credited_sats <> CAST(difficulty * rate_used AS INTEGER);

-- 2. Provenance. Every rate the pool published must follow from the template
--    inputs recorded beside it. Catches a rate applied consistently but
--    derived wrongly — which (1) cannot see.
SELECT COUNT(*) FROM rate_history
 WHERE ABS(rate_sats_per_diff
       - (block_value_sats * 1.0 / network_difficulty)
         * (1 - fee_bps / 10000.0)) > 1e-9;

-- 3. Linkage. No share may be credited at a rate the pool never published.
SELECT COUNT(*) FROM shares s
 WHERE s.rate_used > 0
   AND s.ts >= (SELECT MIN(ts) FROM rate_history)
   AND NOT EXISTS (SELECT 1 FROM rate_history r
                    WHERE r.rate_sats_per_diff = s.rate_used);

-- 4. Solvency. What the pool mined must cover what it owes. The margin
--    decomposes into the fee plus luck; a negative result means the pool
--    cannot pay out of what it has earned.
--
--    CONFIRMED ONLY. A row in blocks_found is a block *candidate*: submitblock
--    may have refused it, and the chain may have reorged it out. Neither pays
--    anything, so summing every row credits the pool with revenue that never
--    existed — on a low-difficulty chain that is almost the whole table, and
--    it makes this query answer "solvent" no matter what is owed.
SELECT (SELECT COALESCE(SUM(reward_sats),0) + COALESCE(SUM(fee_sats),0)
          FROM blocks_found WHERE status = 'confirmed')
     - (SELECT COALESCE(SUM(credited_sats),0) FROM shares) AS margin_sats;
```

The first three must all return **0**. Query 4 should be positive and close
to `Σ difficulty × gross × fee_bps/10000` once luck is accounted for:

```sql
SELECT ROUND((SELECT SUM(difficulty) FROM shares)
             / (SELECT network_difficulty FROM pool_meta)) AS expected_blocks,
       (SELECT COUNT(*) FROM blocks_found
         WHERE status = 'confirmed')                       AS actual_blocks;
```

A large gap between `expected_blocks` and `actual_blocks` on a chain the pool
is genuinely hashing is worth chasing before anything else — check how the
candidates settled:

```sql
SELECT status, COUNT(*), COALESCE(SUM(reward_sats),0) AS would_have_paid
FROM   blocks_found GROUP BY status;
```

`rejected` means the node refused the submission (its reason is in
`submit_error`); `orphaned` means it was accepted and then reorged out;
`pending` means nothing has been able to verify it yet, which is a normal
resting state against a backend that serves only `getblocktemplate` and
`submitblock`. None of the three earns anything.

Exact equality in (1) is the right test rather than a tolerance: the proxy
builds without `-ffast-math`, so SQLite reproduces the same IEEE-754 multiply
and truncation bit-for-bit.

Shares accepted before `rate_used` existed carry 0 and are excluded from (1)
and (3). Their `credited_sats` is still authoritative — there is simply no
stored multiplicand to check it against — and the audit page reports them as
*unverifiable* rather than as failures. `rate_history` is safe to prune:
check (1) does not depend on it.
