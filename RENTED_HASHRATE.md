# Serving rented hashrate (Braiins Hashpower, NiceHash)

How to make this pool a valid destination for a hashrate marketplace, what the
marketplaces actually require, and the one case where it cannot work.

Everything here is off by default. A pool that never wants rented hashrate can
ignore this file — except [the extranonce2 width](#1-extranonce2-width), which
changed for everyone.

## Why this is not just "another port"

Rented hashrate does not arrive as many small miners. It arrives as **one
aggregated worker** behind the marketplace's proxy, and that single fact drives
every requirement below:

- the proxy has to subdivide the `extranonce2` space across the fleet behind
  it, so a 4-byte field is too narrow;
- all of that fleet's hashrate lands on **one connection**, so the share
  difficulty has to be high enough that the connection is not flooded.

## What the marketplaces require

Six things. Four are satisfied, two are not — stated plainly so nobody
rediscovers them during a paid order.

| # | Requirement | Status |
|---|---|---|
| 1 | `extranonce2_size >= 7` | ✅ **8** |
| 2 | A dedicated high-difficulty port | ✅ `rental_listen_port` |
| 3 | Rejects as real stratum errors | ✅ codes 20/21/22/23/24/25 + rejects ledger |
| 4 | Judge a submit at **its job's** difficulty | 🟠 **partial** — see [Known gaps](#known-gaps) |
| 5 | Version rolling / ASICBoost | ✅ BIP310 `mining.configure`, BIP320 mask |
| 6 | `d=` in the password field | ⛔ **not parsed** — NiceHash only |

Known floors, for sizing `rental_min_diff`:

| Marketplace | Minimum share difficulty |
|---|---|
| Braiins Hashpower | 1024 (they recommend **65536**) |
| NiceHash | **500000** |

`500000` clears both with a single port.

### 1. extranonce2 width

`STRATUM_EXTRANONCE2_SIZE` in `src/stratum.h`, now **8** (extranonce1 stays 4).

The reason is **not** search space — 4 bytes already gave one connection 2⁸⁰
headers per job once nonce and version rolling are counted, and jobs rotate long
before that. The reason is **subdivision**: the marketplace's proxy spends the
high bytes of `extranonce2` on a downstream-miner id and hands the low bytes to
the miner. At 4 bytes a proxy spending 3 on addressing leaves one byte — 256
values — and some firmware refuses to run that narrow.

8 rather than the 7 the marketplaces floor at: same cost, satisfies any ">= 7"
rule, and 4/8 is the split proxies and firmware already expect.

Check any pool the way the marketplaces do:

```sh
(echo '{"id":1,"method":"mining.subscribe","params":[]}'; sleep 1) \
  | nc your.pool 3334 | head -1 | jq -r '.result[2]'      # must be >= 7
```

⚠️ **Deploy note.** Miners renegotiate the width on `mining.subscribe`, so a
restart is enough for well-behaved clients. Anything that hardcodes 4 starts
getting `bad extranonce2 size` rejects instead of silently mining work that
could never become a block — that is the point, but **watch the reject rate on
the first restart.**

### 2. The rental port

```conf
rental_listen_port = 3335       # 0 / unset = off
rental_min_diff    = 500000     # clears Braiins and NiceHash
rental_max_conns   = 0          # 0 = inherit max_conns
```

A second `stratum_server_t` in the same process. It shares the job, the
callbacks, the store and the PPLNS window with the public port — **the two
listeners differ only in the difficulty they serve.** A block found on either
settles against the same window and the same coinbase.

`rental_min_diff` is applied as **both** the starting difficulty and the vardiff
floor.

⛔ **Do not try to reach the floor by letting vardiff ramp.** That starts the
connection below the marketplace's minimum, and orders get cancelled for invalid
shares before the ramp finishes. ⛔ **Do not lower the public port's difficulty
to serve a marketplace** either — at difficulty 1024, 1 PH/s is ~227 shares/sec
from a *single* connection (`hashrate / (diff × 2³²)`), and 1 PH/s is a typical
marketplace *minimum* order.

## 🔴 The one case where this cannot work

**The network-difficulty clamp beats the rental floor, deliberately.**

Vardiff never sets a share difficulty above the current *network* difficulty: a
share target harder than the network target means the miner discards hashes that
would have been valid blocks, before the pool ever sees them. That clamp applies
on the rental port too, and it wins.

So during a **minimum-difficulty window** — a fork that resets difficulty to
`powLimit` — every connection including the rental port is clamped to ~1, and a
marketplace order will reject-flood until difficulty ramps back.

This matters because that window is *exactly* when rented hashrate is most
wanted. It is time-bounded (one observed fork ramped 1 → 262,144 in ~5 hours),
but it is a property of the chain, not a misconfiguration, and there is no
setting that avoids it. **Tell the marketplace rather than letting them discover
it during a paid order.**

## Running two listeners safely

Two things in a stratum server are only correct while there is exactly one of
them, and **both silently double-credit shares** once a second listener exists:

- **The extranonce1 counter.** Seeded from the clock at construction, so two
  servers built in one process seed within a millisecond of each other and hand
  out overlapping `extranonce1` values. Two connections with the same
  `extranonce1` render identical coinbases, mine identical headers, and find the
  same hash from the same nonce — half the hashrate wasted, and the share
  credited twice.
- **The share dedupe ring**, which is what would otherwise catch exactly that.
  Per-server rings are blind to each other, so the collision happens *and* the
  guard is looking the wrong way.

Both live in a `stratum_shared_t` that every server in the process shares.
`stratum_cfg_t.shared` left NULL makes the server allocate a private one, which
is correct for a lone server and for every unit test.

⚠️ **Anything that adds a third listener must pass the same `stratum_shared_t`.**
This is the invariant to check first if duplicate shares ever appear.

### The difficulty hint

`on_difficulty_hint` replays what a **worker name** converged to previously, and
it knows nothing about which port earned it — so a miner that mined the public
port at difficulty 1 and then points a rented fleet at the rental port would be
seeded at 1, under the marketplace minimum. The hint is therefore floored at
`vardiff_min`.

⚠️ The floor applies to the **hint only**, not to `initial_diff`. A server whose
`initial_diff` sits below its `vardiff_min` is a valid configuration — it starts
easy and lets the first retarget lift it — and widening the floor to cover
`initial_diff` breaks that.

## Known gaps

- 🟠 **Per-job difficulty at submit (requirement 4).** A submit for a retired job
  is currently judged at the connection's *current* difficulty, falling back to
  `prev_difficulty` for a **time**-based grace of
  `max(2 × vardiff_window_sec, 60s)`. That is one level deep, while the job ring
  is `STRATUM_RECENT_JOBS` (8) deep — two fast retargets inside the grace window
  and the oldest difficulty is gone. Marketplaces report this as their **single
  most common cause of failed integrations**: a pool passes the extranonce check
  and then collapses when difficulty changes. The fix is to record the
  connection's difficulty at the moment each job was sent to it and judge a
  submit against that. **Worth landing before a first paid order.**
- ⛔ **`d=` in the password field (requirement 6).** `mining.authorize` ignores
  `params[1]`. NiceHash uses it to request a difficulty; Braiins does not. A
  rental port pinned at 500000 already serves NiceHash's floor, so this is not
  blocking, but a NiceHash integration that wants to *vary* difficulty needs it.

## Verification

Both regtest harnesses exercise the width against a real enforcer and node:

```sh
REGTEST_SKIP_THUNDER=1 bash tests/test_e2e_regtest.sh    # pps-classic, one block
bash tests/test_burst_regtest.sh                          # proportional, many blocks
```

⚠️ `test_burst_regtest.sh` currently ends in a **pre-existing** failure —
`PROP_PLAN_RING is not covering every solvable job` — that is unrelated to the
extranonce width or the rental port. It reproduces on the parent commit with the
same five other invariants passing. Read the invariants it prints, not just its
exit code: the width is proven by `all N coinbases pay the reward exactly`.
`test_payout_regtest.sh` needs the Thunder sidechain node and does **not**
accept `REGTEST_SKIP_THUNDER=1`.

The e2e asserts a block is **mined through stratum and accepted by the node**
with the enforcer's M4 AckBundles commitment intact — the coinbase scriptSig now
carries 12 extranonce bytes, and a width mismatch produces a coinbase whose
length prefix disagrees with its contents, so this is the check that matters.

Unit coverage lives in `tests/test_stratum.c`: the advertised width, rejection
one byte under and one byte over, the shared extranonce1 sequence across two
servers (asserted **consecutive**, so it cannot pass by luck), and the hint
floor.

⚠️ `TEST_EN2` in `tests/test_stratum.c` is guarded by a `_Static_assert` against
`STRATUM_EXTRANONCE2_SIZE`. Changing the width without updating the fixture is a
**compile** error rather than a wrong-width coinbase reaching a real block.
