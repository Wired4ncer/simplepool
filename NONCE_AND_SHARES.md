# Nonce distribution, share calculation, and audit

How simplepool splits work between connected miners, validates their
share submissions, credits their PPS balance, and lets an operator
(or a suspicious miner) audit every number back to first principles.

Every claim in this document is traceable to a line in the C source or
a table in `shares.db`. Where a specific line is cited (e.g.
`src/stratum.c:688`), that's what to grep to check the code hasn't
drifted.

---

## Part 1 — how the pool divides the search space

### The block header (80 bytes)

Every miner is trying to find a header whose double-SHA256 is smaller
than the network target. The 80-byte block header has these fields:

```
offset  size  field
   0      4   version           (LE, may be "rolled" via a mask)
   4     32   prev_block_hash   (LE)
  36     32   merkle_root       (LE — derived from the coinbase + branches)
  68      4   ntime             (LE)
  72      4   nbits             (LE, network's compact target)
  76      4   nonce             (LE, the tiny 32-bit search space)
```

Only three fields can vary while searching:
- `nonce`               — 2³² = 4.3 billion values, on-header
- version-rolled bits   — a subset of `version` via a mask
- `merkle_root`         — indirectly, by changing the coinbase

The coinbase is where the extranonce lives. Every distinct extranonce
produces a distinct coinbase → a distinct coinbase txid → a distinct
merkle root → a distinct 32-bit nonce search space to sweep.

### The pool's split of the nonce space

simplepool uses the standard stratum-v1 split:

```
coinbase scriptSig layout (assembled at share-check time):

   [ height_push ] [ tag ] [ extranonce1 (4 B) ][ extranonce2 (8 B) ]
                            └── pool assigns ──┘└──  miner picks   ──┘
```

- **extranonce1 (4 bytes, `en1`)** — assigned by the pool when the
  connection subscribes. Immutable for the life of that TCP session.
- **extranonce2 (8 bytes, `en2`)** — the miner's private search field.
  Each `mining.submit` carries an `en2` value; the miner sweeps it
  independently.

Both widths come from `STRATUM_EXTRANONCE1_SIZE` / `STRATUM_EXTRANONCE2_SIZE`
in `src/stratum.h`, and the submit path rejects any `en2` that is not exactly
the advertised width — a mismatched width produces a coinbase whose scriptSig
length prefix disagrees with its contents, which still hashes like a valid
header but can never become a block.

Together they give each connection **2⁹⁶ distinct coinbases** to try
before it has to reconnect for a fresh `en1`. At any real hashrate,
that's effectively unbounded.

**Why `en2` is 8 and not the classic 4.** Not search space — 4 was already
unreachable. The reason is *subdivision*. Rented hashrate from a marketplace
does not arrive as many small miners; it arrives as one aggregated worker
behind a proxy that splits the `en2` it is handed into a downstream-miner id
(high bytes) plus that miner's own `en2` (low bytes). At 4 bytes a proxy
spending 3 on addressing leaves its miners one byte, and some firmware refuses
to run that narrow — which is why Braiins Hashpower rejects a pool URL
advertising fewer than 7 outright. At 8 a proxy can spend 3 and still hand
down the conventional 4.

For each `en2` the miner picks, it then sweeps the header's 4-byte
`nonce` field (2³² hashes) and, if version-rolling was negotiated,
also permutes the masked version bits. So each `en2` value gives
2³² × (rolled-versions) headers to hash.

### Extranonce1 allocation — how uniqueness is guaranteed

```c
/* Straight from the counter — deliberately NOT mixed with the clock. */
unsigned seq = atomic_fetch_add(&s->shared->extranonce1_seq, 1);
uint32_t mix = (uint32_t)seq;
c->extranonce1[0] = (uint8_t)(mix >> 24);
c->extranonce1[1] = (uint8_t)(mix >> 16);
c->extranonce1[2] = (uint8_t)(mix >> 8);
c->extranonce1[3] = (uint8_t)mix;
```

Two properties matter:

1. **Uniqueness across concurrent connects** — `atomic_fetch_add` on
   `extranonce1_seq` guarantees that no two connections can read the
   same `seq` value even if they subscribe in the same nanosecond.
   ⚠️ An earlier version XORed the counter with `now_ms()` to get
   "freshness after wrap". That **destroyed** the guarantee it was
   meant to strengthen: it collides whenever the delta in the clock
   equals the delta in the counter, which a miner opening several
   connections at once hits routinely. The counter is seeded from the
   clock **once**, at construction, so values still differ across
   restarts without the collisions.
2. **Uniqueness across *servers*** — the counter lives in a
   `stratum_shared_t` that every stratum server in the process shares,
   not in the server itself. The pool runs two listeners when the
   rental port is enabled, and two servers each seeding their own
   clock-derived counter would seed within a millisecond of each other
   and hand out overlapping `en1` values. Two connections with the same
   `en1` render identical coinbases, mine identical headers, and find
   the same hash from the same nonce — half the hashrate wasted and the
   share credited twice.

**No two miners on the pool are searching the same
`(header, coinbase, nonce)` triple.** That's the fairness guarantee
simplepool makes; there's no reference to a random pool from `/dev/urandom`
or any other entropy source needed to enforce it.

### Multi-rig, same-address

Two ASICs authorizing with the SAME Thunder address but DIFFERENT
`.<rig_label>` suffixes get:

- **Distinct `en1` values** (they're two connections)
- **Merged accrual** in `pps_credits` if the code just keyed on address
- **BUT distinct worker rows** because the `workers.name` uniqueness
  constraint keeps them separate ledger-wise

That means a single miner running `basement.rig` and `garage.rig` from
the same Thunder address sees each rig on the leaderboard and can
attribute contributions per box.

### Version rolling

If a miner supports it (advertised via `mining.configure`), the pool
negotiates a version-bit mask. `src/stratum.c` emits the mask on the
authorize response; the miner is allowed to XOR any bit in the mask
into the header's `version` field.

Current default mask: **`0x1fffe000`** — the 16 bits between position
13 and 28. That expands the effective per-`(en1, en2)` search space by
2¹⁶, so a single `en2` value covers 2³² × 2¹⁶ = 2⁴⁸ ≈ 280 trillion
headers.

**The pool never re-uses a version-rolled header for share
validation**: on submit, the miner tells us the exact rolled version
they used (`rolled=…` in the `[SUBMIT CHECK]` log lines), we
reconstruct the header they hashed, we re-hash it ourselves, and we
compare *that* hash against the worker/network targets. Any
manipulation outside the mask is rejected as invalid.

---

## Part 2 — how shares are calculated

### Header reassembly on submit

When a miner submits `(job_id, en2, ntime, nonce, version_bits)`,
the pool has everything it needs to reconstruct the exact header the
miner hashed:

```
coinbase_hex = cb1_hex || en1 || en2 || cb2_hex
coinbase_txid = dSHA256(coinbase_hex)          # 32 bytes, LE

merkle_root = coinbase_txid
for branch in job.merkle_branches:
    merkle_root = dSHA256(merkle_root || branch)

header = version_LE || prev_hash_LE || merkle_root_LE ||
         ntime_LE || nbits_LE || nonce_LE

share_hash = dSHA256(header)                   # 32 bytes, LE
```

Any deviation from what the miner claims produces a completely
different `share_hash`, which then fails the target check → reject.

### The two targets

Every share is checked against TWO thresholds:

- **Worker target** — the difficulty the pool is currently holding
  this connection at (set via `mining.set_difficulty` per vardiff).
  A hash below this counts as an accepted share and earns PPS credit.

- **Network target** — the current chain difficulty from
  `getblocktemplate` / the enforcer. A hash below this is a valid
  block that the pool will submit via `submitblock`.

Both targets are stored/transmitted as 256-bit numbers, big-endian.
For a hash `h` (interpreted as an unsigned 256-bit big-endian
integer), the checks are:

- Share valid ⇔ `h ≤ worker_target`
- Block found ⇔ `h ≤ network_target`

Since network difficulty is much higher than any reasonable worker
difficulty, `network_target < worker_target` almost always → a
block-finding hash also satisfies the share check → the same submit
counts both as a paid share AND a block.

### The share validation flow

`src/stratum.c` — the submit path:

1. Look up the job by `job_id`. If unknown / expired → reject with
   `stale share`.
2. Reconstruct the coinbase from cached `cb1/cb2` + `(en1, en2)`.
3. Compute the merkle root from `dsha256(coinbase) → branches`.
4. Reconstruct the header with the miner's `version|ntime|nonce`.
5. `dsha256(header)` → `sent_hash`.
6. Log a `[SUBMIT CHECK]` line with `sent_hash`, `worker_target`,
   `network_target`, and version fields (this is what appears in
   `logs/simplepool.log`).
7. Compare against both targets:
   - `sent_hash > worker_target` → reject with `low difficulty`,
     insert a row into `rejects`.
   - Otherwise → insert a row into `shares` with the SHA256 of the
     header as `block_hash` (nullable elsewhere), the connection's
     difficulty as the share's difficulty, and `is_block = 1` iff
     `sent_hash ≤ network_target`.
   - If block: also enqueue `submitblock` to the backend.

### Vardiff

The pool auto-adjusts each connection's difficulty to hit a target
share-rate (`vardiff_target_spm` shares/minute, default 12). See
`src/stratum.c` for the vardiff loop; the tunables in `proxy.conf` are:

- `vardiff_enabled` — 0/1
- `vardiff_target_spm` — target shares per minute
- `vardiff_min` / `vardiff_max` — clamps
- `vardiff_window_sec` — how often to retarget

`mining.set_difficulty` fires whenever the connection's target changes.
The active job stays valid — the target check is per-share.

### Difficulty of a share

Bitcoin's "pdiff-1" target is `0xffff * 2^208`. A share at difficulty
`D` is one whose hash is below `pdiff-1 / D`. Concretely, given a
`worker_target`, the equivalent difficulty is:

```
difficulty = pdiff_1_target / worker_target
```

That's the number stored in `shares.difficulty` for each accepted
share, and the same number used to compute the PPS credit.

### Worked example (from the live pool right now)

The current worker target on the running ASIC is:

```
worker_target = 0x000003e7fc180000...  (hex prefix; rest is zeros)
```

Position of the significant bytes: 5 leading hex zeros followed by
`03e7fc18`. That's `0x03e7fc18 × 2^(51×4)` = `0x03e7fc18 × 2^204`.

pdiff-1 = `0xffff × 2^208`.

```
difficulty = (0xffff × 2^208) / (0x03e7fc18 × 2^204)
           = 0xffff × 2^4 / 0x03e7fc18
           = 65535 × 16 / 65407512
           ≈ 0.01603
```

So this rig's shares are being credited at ~0.016 difficulty each.
That matches what shows up in `shares.difficulty` per row.

Sanity-check hashrate from share rate:

- share_rate = shares_per_second = H / (D × 2^32)
- Observed 14 shares in the last minute → 0.233/s at D=0.016
- H = 0.233 × 0.016 × 2^32 ≈ 16 MH/s

The pool's `hashrate` derivation on the public dashboard follows the
same formula, over `overview.window_sec` (default 24h).

---

## Part 3 — how payouts are computed

### The formula

The C proxy credits every accepted share with:

```
delta_sats = FLOOR(difficulty × pps_sats_per_diff)
```

`src/main.c` at the `on_share_cb`:

```c
int64_t delta = (int64_t)(difficulty * s->cfg->pps_sats_per_diff);
if (delta > 0) {
    if (s->store) {
        store_record_credit(s->store, worker_name, payout_address,
                            ts_ms, delta);
    }
    ...
}
```

The cast to `int64_t` is a **per-share floor**. That matters for the
audit — see Part 4.

### The `pps_credits` table

One row per worker:

```sql
CREATE TABLE pps_credits (
  worker_id     INTEGER PRIMARY KEY REFERENCES workers(id),
  accrued_sats  INTEGER NOT NULL DEFAULT 0,
  paid_sats     INTEGER NOT NULL DEFAULT 0,
  last_updated  INTEGER NOT NULL
);
```

- `accrued_sats` — monotonically-increasing sum of `delta_sats` across
  every share this worker has ever landed. Only the C proxy writes.
- `paid_sats`   — monotonically-increasing sum of settled Thunder
  transfers. Only the payout worker writes.
- Owed at any moment = `accrued_sats - paid_sats`.

The invariants are enforced by the app code, not by DB constraints.
Both fields must always increase; a decrease means somebody edited
the DB by hand.

### The at-most-once payout protocol

`payouts_in_flight` is a WAL for pending payouts. Every payout goes
through three steps:

1. `INSERT INTO payouts_in_flight (worker_id, sats, txid='')`.
2. `thunder.transfer(addr, sats, fee)` — receives a `txid`.
3. ONE atomic SQLite transaction:
   - `UPDATE payouts_in_flight SET txid = ?`
   - `UPDATE pps_credits SET paid_sats = paid_sats + ?`
   - `DELETE FROM payouts_in_flight WHERE id = ?`

A crash between (1) and (2) leaves a row with `txid=''` — the payout
worker's `listDue` skips this worker until the operator reconciles.
A crash between (2) and (3) leaves a row with `txid` set — the
operator can verify the tx on Thunder and finalize by hand.

Neither state can produce a double-pay because `listDue` skips any
worker with any in-flight row. See `payout/lib/db.js` and
`payout/lib/payout.js`.

---

## Part 4 — auditing every number

### Per-share record

Each row in `shares` records exactly what was credited:

```sql
CREATE TABLE shares (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id   INTEGER NOT NULL REFERENCES workers(id),
  ts          INTEGER NOT NULL,   -- unix seconds
  difficulty  REAL NOT NULL,      -- the worker target's difficulty at submit
  is_block    INTEGER NOT NULL DEFAULT 0,
  block_hash  TEXT               -- the header dSHA256 (also block hash if is_block=1)
);
```

The row lets you reproduce the credit for a single share as:

```
credit_sats = CAST(difficulty * pps_sats_per_diff AS INTEGER)
            = FLOOR(difficulty × rate)
```

### Reproducing "why is my balance N sats?"

Given a worker's `worker_id` and the pool's current `pps_sats_per_diff`
(call it `rate`), everything below is derivable from `shares` alone —
the `pps_credits.accrued_sats` field is a running total the C proxy
maintains, but you can always recompute it:

```sql
-- expected accrued_sats for one worker, computed from raw shares
SELECT SUM(CAST(difficulty * :rate AS INTEGER)) AS accrued_computed
FROM   shares
WHERE  worker_id = :wid;
```

If this **matches** `pps_credits.accrued_sats` → the C proxy has been
running correctly since the DB was initialized.

If it **doesn't match** → one of:

- The DB was populated across a rate change. The proxy credits shares
  at whatever `pps_sats_per_diff` was set at the time of that share;
  the recomputation above uses a single `rate`. Grep the pool log
  for the startup line `pool_mode=…: … pps_sats_per_diff=…` around
  suspicious timestamps.
- Somebody edited `pps_credits` by hand. Check `sqlite3 .headers on
  .mode column select rowid, * from pps_credits;` for anything odd.
- A SQLite corruption. Extremely rare in WAL mode. If suspected,
  make a `.backup` before touching anything else.

### The admin cross-check

The `/admin/worker/:id` page ([dashboard/views/admin-worker.ejs](dashboard/views/admin-worker.ejs))
implements exactly this — with a visual ✓ / ⚠ badge. It shows:

- **Σ difficulty** (raw sum, no truncation)
- **rate** (`POOL_PPS_SATS_PER_DIFF` env → the value proxy.conf shipped)
- **Σ FLOOR(diff × rate)** — the authoritative sum
- **pps_credits.accrued_sats** — what's stored
- Match indicator

There's also a naive `FLOOR(Σ diff × rate)` shown next to the
authoritative number so operators can quantify how many sats the
per-share truncation "cost" over the window (usually a handful of
sats — one per share whose fractional contribution got rounded away).

### Per-worker audit page — what a miner sees

At `/admin/worker/:id` an operator can:

- Verify the cross-check (see above).
- See the **daily breakdown**: how many shares landed each day, the
  Σ difficulty for the day, sats credited for the day, whether any of
  those shares also happened to be blocks.
- See the **last 100 shares** with a `running_accrued` column that
  walks oldest-first through the visible slice. A miner can literally
  point at share row #872 and read off exactly how many sats they had
  at that moment.

The `/api/admin/worker/:id` JSON endpoint returns the same numbers.
Feed it to a monitoring script or a per-miner email report.

### Reconciling with the enforcer's Ctip

When the classic-mode deposit flow is running, there's a second
audit angle: **every deposit is recorded in the `deposits` table**
with the mainchain `btc_txid`, the amount, the fee, and the Ctip
sequence numbers before + after. That lets an operator or auditor
correlate:

- Sum of `deposits.sats_deposited` since deployment → how much BTC
  crossed onto Thunder.
- Sum of `pps_credits.paid_sats` → how much left Thunder to miners.
- The Ctip's current `value` → how much sits in the reserve.

The three should balance modulo fees. Discrepancies point at either
an off-by-one in the deposit ledger (checkable against
`ValidatorService/GetTwoWayPegData`) or an unrecorded manual
Thunder-side transaction (should never happen in a well-run pool).

### The public dashboard's per-worker page

At `/worker/:name` (no auth) — miner-facing — the same fundamentals
without the operator-only cross-checks. It shows recent shares,
hashrate estimates from the share stream, and blocks found by the
worker. If a miner asks "how much am I owed?", direct them to their
own row on the leaderboard for the numbers; direct them to the
`/admin/worker/:id` page if they want the audit trail (behind basic
auth, invite-only).

### SQL cookbook — reproduce any number from the DB

**Every share you've ever landed:**

```sql
SELECT id, datetime(ts,'unixepoch') AS ts_utc, difficulty, is_block, block_hash
FROM   shares
WHERE  worker_id = :wid
ORDER  BY ts DESC;
```

**Your all-time accrual, computed:**

```sql
SELECT SUM(CAST(difficulty * :rate AS INTEGER)) AS accrued
FROM   shares
WHERE  worker_id = :wid;
```

**Your accrual over an arbitrary window:**

```sql
SELECT SUM(CAST(difficulty * :rate AS INTEGER)) AS accrued
FROM   shares
WHERE  worker_id = :wid
  AND  ts BETWEEN :from_unix AND :to_unix;
```

**Who found the last block:**

```sql
SELECT height, hash, datetime(ts,'unixepoch') AS ts_utc, finder_id,
       finder_address, reward_sats
FROM   blocks_found
WHERE  status = 'confirmed'
ORDER  BY ts DESC LIMIT 1;
```

Drop the `WHERE` to see candidates that did not make it — but read `status`
before believing a `reward_sats`: only a confirmed block was ever paid.

**Rejects, grouped by reason, last hour:**

```sql
SELECT reason, COUNT(*) AS n
FROM   rejects
WHERE  ts > strftime('%s','now','-1 hour')
GROUP  BY reason
ORDER  BY n DESC;
```

**Every deposit ever made:**

```sql
SELECT datetime(ts,'unixepoch') AS ts_utc, sats_deposited,
       fee_sats, thunder_recipient, btc_txid,
       ctip_seq_before, ctip_seq_after
FROM   deposits
ORDER  BY ts DESC;
```

---

## Common mismatches and what they mean

| symptom | likely cause | to check |
| --- | --- | --- |
| Audit page shows ⚠ | `POOL_PPS_SATS_PER_DIFF` env doesn't match `proxy.conf` | grep both, restart dashboard |
| accrued grows without shares | somebody wrote to `pps_credits` by hand | `.timeline` the DB; look for gaps in `shares.id` |
| paid_sats > sum of `deposits` | payout worker sending BTC-equivalent that never entered Thunder | reconcile via Thunder RPC's tx history |
| shares.is_block=1 but the `blocks_found` row says `rejected` | the node refused the submission — normal on a low-difficulty chain | read `blocks_found.submit_error` for the node's own reason |
| `blocks_found` rows stuck at `pending` | nothing can verify them: the backend does not serve `getblockhash` and no template has been observed at their height+1 | expected against the CUSF enforcer; they count as nothing until verified |
| many `orphaned` candidates | the pool's blocks keep losing races — a slow or badly-peered node, or templates lagging the tip | check the "Blocks reaching the chain" health check and the node's peer count |
| dashboard hashrate lower than the miner's own display | actual delivery is lower than the miner claims (thermal / net / firmware); nonce distribution is fine | see [OPERATOR_GUIDE.md](OPERATOR_GUIDE.md) troubleshooting |
| single share value looks tiny | vardiff pushed worker difficulty down; per-share reward compresses; but shares_per_minute is up so payout per minute stays the same | look at `Σ difficulty` per hour on the audit page — that number is invariant to vardiff |

---

## What the pool CANNOT audit

- **Whether a miner is actually delivering the hashes they claim.** If
  they underclock and send fewer shares, the pool sees fewer shares
  and credits accordingly. That's normal, not fraud. See the
  block-withholding audit in [payout/audit.js](payout/audit.js) for
  the statistical fraud detector — it flags workers whose observed
  block-find rate is significantly below expected.
- **What Thunder does with the deposit** once it's on the sidechain.
  We only see the enforcer's Ctip. Thunder-side crediting to
  individual sidechain addresses is Thunder's business; if a miner
  says "I never got the coins", verify via `thunder-cli
  get-transaction <txid>` on a Thunder node.
- **Whether the operator has actually deposited what they should
  have.** That's a business trust question, not a technical one. The
  `deposits` ledger is honest about what the operator DID; there's
  no consensus-level enforcement that says the operator MUST deposit
  by any particular date.

For those three, transparency is the only defense — that's what
this doc, the audit page, and the SQL cookbook above are for.
