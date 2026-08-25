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

Both widths come from `STRATUM_EXTRANONCE1_SIZE` /
`STRATUM_EXTRANONCE2_SIZE` in `src/stratum.h`; the `en2` size is what
the pool reports as the third element of the `mining.subscribe` result.

`en1` is fixed for the life of the connection, so each connection has
**2⁶⁴ distinct coinbases** to try before it would need to reconnect for
a fresh one; across the pool the `(en1, en2)` pair space is 2⁹⁶. At any
real hashrate, both are effectively unbounded.

#### Why extranonce2 is 8 bytes and not the classic 4

Not for search space. Even at 4 bytes a single connection gets 2⁸⁰
headers per job (see the version-rolling section below), which a 1 EH/s
farm would take about two weeks to exhaust — and jobs rotate every
template. Capacity was never the constraint.

The reason is **subdivision**. A stratum proxy sitting between the pool
and a farm fans one upstream connection out to many downstream miners
by splitting the `en2` field it was given: high bytes become a
downstream-miner id, low bytes are passed down as that miner's own
`en2`. At 4 bytes upstream, a proxy spending 3 on addressing leaves its
miners a single byte — 256 values — and some firmware refuses to run
that narrow. At 8 the proxy can spend 3 and still hand down the
conventional 4, so every downstream miner sees an ordinary pool.

**Changing these widths is consensus-relevant, not cosmetic.** `cb1`
ends with the scriptSig length varint, computed once from
`en1_size + en2_size` when the coinbase is rendered. An `en2` of any
other width produces a coinbase whose declared scriptSig length
disagrees with the bytes that follow it — an invalid transaction whose
header nonetheless hashes fine. `handle_submit` therefore rejects any
submission whose `en2` is not exactly `job->en2_size`
(`src/stratum.c`, error `wrong extranonce2 size`) rather than crediting
a share for work that could never become a block.

For each `en2` the miner picks, it then sweeps the header's 4-byte
`nonce` field (2³² hashes) and, if version-rolling was negotiated,
also permutes the masked version bits. So each `en2` value gives
2³² × (rolled-versions) headers to hash.

### Extranonce1 allocation — how uniqueness is guaranteed

In `handle_subscribe` (`src/stratum.c`):

```c
/* Take extranonce1 straight from the server counter. */
unsigned seq = atomic_fetch_add(&s->extranonce1_seq, 1);
uint32_t mix = (uint32_t)seq;
c->extranonce1[0] = (uint8_t)(mix >> 24);
c->extranonce1[1] = (uint8_t)(mix >> 16);
c->extranonce1[2] = (uint8_t)(mix >> 8);
c->extranonce1[3] = (uint8_t)mix;
```

**Uniqueness across concurrent connects** — `atomic_fetch_add` on
`extranonce1_seq` guarantees that no two connections can read the same
`seq` value even if they subscribe in the same nanosecond. The counter
is seeded from the clock once at startup, so a restart doesn't hand out
the same values to a fresh set of connections.

The counter is *not* mixed with the clock at use. An earlier version
did (`seq ^ now_ms()`), and that destroyed the uniqueness guarantee it
was meant to reinforce: the XOR collides whenever the delta in the
clock equals the delta in the counter — an even `seq` at an even
millisecond and the next `seq` one millisecond later land on the same
value. A miner opening several connections at once hit that routinely,
and two connections sharing an `en1` render identical coinbases, so
both find the same hash from the same nonce.

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
headers, and one connection covers 2⁶⁴ × 2⁴⁸ = 2¹¹² over the full `en2`
sweep.

**The pool never re-uses a version-rolled header for share
validation**: on submit, the miner tells us the exact rolled version
they used (`rolled=…` in the `[SUBMIT CHECK]` log lines), we
reconstruct the header they hashed, we re-hash it ourselves, and we
compare *that* hash against the worker/network targets. Any
manipulation outside the mask is rejected as invalid.

### Rolling ntime

The third multiplier, and the only one that needs no negotiation at
all. A miner may advance the header's `ntime` while it holds a job; the
pool takes the value submitted, puts it in the header verbatim, and
treats every distinct value as a distinct share. The per-connection
dedupe key includes `ntime`, so rolling it produces genuinely new work
rather than repeats of the same one.

It is bounded, and the bound is there for the chain's sake rather than
the pool's. Consensus refuses a block whose timestamp is more than two
hours ahead of network-adjusted time — but such a header still hashes,
still clears a share target, and looks like perfectly good work here.
Without a check the pool credits it, and if it happens to beat the
network target, assembles a block that `submitblock` then throws out: a
solved block lost with nothing but a warning in the log.

    job.ntime - 600  ≤  submitted ntime  ≤  job.ntime + 7200

Measured from the job rather than the wall clock, which keeps it
conservative: a job only gets older while the miner holds it, so
anything inside this window is inside the consensus window too. The
backward tolerance exists because a stratum proxy that rewrites the
field, or a rig with a skewed clock, can land slightly behind — that is
honest work and rejecting it would cost the miner shares. Both ends are
deliberately loose. This catches a broken client; it does not police
timestamps.

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

- **Worker target** — the difficulty *the submitted job* went out
  under, not whatever the connection has drifted to since. A hash
  below this counts as an accepted share and earns PPS credit.

  The distinction matters because `mining.set_difficulty` takes effect
  on the *next* job the miner is notified of. A miner still working a
  job it was handed before a retarget is mining to the difficulty that
  job carried, and on a slow chain those shares keep arriving long
  after the retarget. The pool records the difficulty each job was
  notified under, per connection, and judges the submit against that —
  so any number of retargets can happen in between without turning
  honest work into rejects. A share is credited at the difficulty it
  is judged under.

  Some firmware applies a `set_difficulty` to work already in hand
  rather than waiting for the next job. When that moves the difficulty
  *down*, the resulting shares are below what the job was sent at but
  are exactly what the miner was told to do, so they are accepted at
  the lower value. A share is never credited at a difficulty it did
  not actually meet.

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

`src/stratum.c` — the submit path. The first four steps are refusals
that cost nothing, deliberately placed before any hashing: each one
describes a header that could never become a block, so validating it
would be effort spent to reach the same answer more slowly.

1. **Submit ceiling.** If the connection is past
   `max_submits_per_sec` for the current second → reject with
   `submitting too fast`, before the parameters are even read. Reported
   periodically with a count rather than once per refusal, so a flood
   does not write itself into the table that exists to account for
   shares.
2. Look up the job by `job_id`. If unknown / expired → reject with
   `stale or unknown job`.
3. **Per-connection dedupe.** A repeat of
   `(job_id, en2, ntime, nonce, version)` this connection already sent
   → reject with `duplicate share`.
4. **ntime bounds.** The submitted `ntime` must lie within
   `-600 s … +7200 s` of the value this job went out with → otherwise
   reject with `ntime out of range`. See *Rolling ntime* above for why
   an unbounded timestamp costs blocks rather than shares.
5. **extranonce2 width.** `len(en2)` must be exactly the 8 bytes
   advertised on `mining.subscribe` → otherwise reject with
   `wrong extranonce2 size`. The width is baked into `cb1`'s scriptSig
   length varint, so any other width describes a transaction that
   cannot be mined.
6. Reconstruct the coinbase from cached `cb1/cb2` + `(en1, en2)`.
7. Compute the merkle root from `dsha256(coinbase) → branches`.
8. Reconstruct the header with the miner's `version|ntime|nonce`.
9. `dsha256(header)` → `sent_hash`.
10. **Global hash dedupe.** If this exact hash has already been
    credited — on any connection, under any job id — → reject with
    `duplicate share`. One solution is one solution regardless of how
    the submission was framed.
11. Log a `[SUBMIT CHECK]` line with `sent_hash`, `worker_target`,
    `network_target`, and version fields (this is what appears in
    `logs/simplepool.log`).
12. Compare against both targets:
    - `sent_hash > worker_target` **and** `sent_hash > network_target`
      → reject with `low difficulty`, insert a row into `rejects`.
    - Otherwise → insert a row into `shares` with the SHA256 of the
      header as `block_hash` (nullable elsewhere), **the difficulty
      that job went out under** as the share's difficulty (see *The two
      targets* — not whatever the connection has drifted to since), and
      `is_block = 1` iff `sent_hash ≤ network_target`.
    - If block: also enqueue `submitblock` to the backend.

Note the order of the last comparison. The network verdict wins over
the share verdict: on a low-difficulty chain a share target can be
*harder* than the network target, so a hash may be a valid block while
failing the share check. That one has to be submitted, never rejected —
and it is credited at the difficulty it provably met.

### Vardiff

The pool auto-adjusts each connection's difficulty to hit a target
share-rate (`vardiff_target_spm` shares/minute, default 12). See
`src/stratum.c` for the vardiff loop; the tunables in `proxy.conf` are:

- `vardiff_enabled` — 0/1
- `vardiff_target_spm` — target shares per minute
- `vardiff_min` / `vardiff_max` — clamps
- `vardiff_window_sec` — how often to retarget

`mining.set_difficulty` fires whenever the connection's target changes.
The active job stays valid — the target check is per-share, and every
job already carries the difficulty it went out under (see *The two
targets*), so work the miner is already holding stays acceptable at
that difficulty for as long as the job itself lives.

Two things bound the result, and on a low-difficulty chain they pull in
opposite directions:

- **The network ceiling.** Share difficulty is not raised above the
  network difficulty, because a miner filters locally against the
  stratum target — a share target harder than the network target makes
  it discard valid blocks before the pool ever sees them.
- **The port's promised floor.** A listener may state a `min_diff`, and
  that floor is kept *even where the chain's own difficulty is lower*.

Where they meet, the floor wins. A rented-hashrate marketplace measures
the difficulty on the wire, not the one a port advertises, so a port
that promised 65536 and quietly served 1200 has its order cancelled
with nothing on the pool side recording why. What that costs is blocks:
miners on such a port discard roughly `min_diff / network_difficulty`
of the solutions they find. The pool warns at startup for every port in
that position, and the dashboard reports it.

This is affordable only because the floor is **per-port and opt-in**.
`promised_min_diff` is recorded separately from `vardiff_min` precisely
so the two can be told apart: a listener that never stated one — which
includes `listen_port`, every regtest config, and any young forknet —
is clamped exactly as it always was and keeps every block.

### Per-port difficulty policy

One difficulty cannot serve both a home ASIC and a rented fleet. A
marketplace aggregates the whole fleet behind a *single* connection, so
1 PH/s at difficulty 1024 is ~227 shares per second down one socket,
and vardiff cannot bridge the gap — it moves at most 4x per window, so
climbing from 1 to 65536 takes eight windows and the reject flood on
the way is what gets the order cancelled. The miner has to *arrive* at
the right difficulty:

```
listener = port=3335 min_diff=65536 label=braiins
listener = port=3336 min_diff=500000 label=nicehash
```

Each listener binds its own port; connections accepted there start at
that difficulty and never vardiff below it, while `listen_port` keeps
serving home miners unchanged. A config naming no listeners binds
exactly what it always did.

A connection's share rate is also capped outright by
`max_submits_per_sec` (default 20000, per connection). Nothing stops
hashrate and assigned difficulty from being badly mismatched — a fleet
pointed at a home-miner port is the usual cause — and past the ceiling
a submit is refused before any validation work.

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
| hashrate reads low for a rig that just connected, or a pool that just started | **no longer a thing.** Every rate is divided by the span the shares actually cover, not the nominal window, so a rig ten minutes into a 24h window is measured over ten minutes | if a pre-0.3.0 dashboard is reading a 0.3.0 DB, that is the version gap, not the data |
| rejects reading `ntime out of range` | the rig rolled `ntime` outside `job.ntime - 600 … + 7200`; usually a badly wrong clock | compare the rig's clock to the pool host's |
| rejects reading `submitting too fast` | the connection is past `max_submits_per_sec` — almost always a fleet on a port whose difficulty is far too low for it | move it to a port with a `min_diff`, or raise the ceiling if the low difficulty is deliberate |
| a rental port serves a lower difficulty than configured | it set `vardiff_min` but not `min_diff`; only a promised floor survives the network-difficulty ceiling | add `min_diff=` to that `listener` line — and read what it costs in blocks first |
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
