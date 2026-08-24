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

Six things. Five are satisfied; the sixth is NiceHash-only and not blocking —
stated plainly so nobody rediscovers them during a paid order.

⚠️ **Satisfying all six is necessary, not sufficient.** They are the marketplace's
*technical* gate. Acceptance also has a **policy** dimension that no amount of
protocol correctness satisfies — see [Policy, not protocol](#policy-not-protocol).
A pool can serve a marketplace order with a literal **zero** reject rate and
still have it cancelled.

| # | Requirement | Status |
|---|---|---|
| 1 | `extranonce2_size >= 7` | ✅ **8** |
| 2 | A dedicated high-difficulty port | ✅ `rental_listen_port` |
| 3 | Rejects as real stratum errors | ✅ codes 20/21/22/23/24/25 + rejects ledger |
| 4 | Judge a submit at **its job's** difficulty | ✅ per-connection job record |
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
floor. It is a floor, **not a pin** — vardiff still ramps *above* it for a large
order, which matters because 1 EH/s at difficulty 500000 is still ~466
shares/sec.

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

So whenever **network difficulty falls below a marketplace's floor**, orders on
the rental port reject-flood until it climbs back. Lowering `rental_min_diff`
does not help — the clamp overrides it either way.

The thresholds, since this is the number that actually matters:

| Marketplace | Works while network difficulty >= | shares/s at 1 PH/s |
|---|---|---|
| Braiins (absolute minimum) | **1,024** | 227 — already their "reject flood" example |
| Braiins (recommended) | **65,536** | 3.6 |
| NiceHash | **500,000** | 0.5 |

The realistic way to hit this is a chain that **resets difficulty to `powLimit`
at a fork activation height** (`pow.cpp`: `if (pindexLast->nHeight + 1 ==
params.EcashHeight) bnNew = bnPowLimit;`, with a Bitcoin-style
`powLimit` that means difficulty 1). It is time-bounded and it recovers in
stages: with the usual 4x-per-retarget cap, difficulty passes 1,024 after 5
retargets, 65,536 after 8 and 500,000 after 10 — so **Braiins comes back
before NiceHash does**. One observed fork ramped ~8-9 retargets in about five
hours, which put every floor within hours rather than days.

⚠️ This is a property of the chain, not a misconfiguration, and no setting
avoids it. **Tell the marketplace up front** rather than letting them discover it
during a paid order that happens to span a fork.

⚠️ Whether any *particular* upcoming fork resets difficulty is a question about
that chain's `EcashHeight` and `powLimit` — **check the mainnet chainparams
rather than assuming the previous fork's behaviour repeats.** The reset fires at
exactly one height per chainparams, so a height that has already passed cannot
fire again.

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

### 3. Judging a submit at its own job's difficulty

Each connection records the difficulty it was on when each job was **sent to
it**, and a submit naming that job is judged against that value rather than
against whatever the connection has retargeted to since.

This matters because the two timescales are wildly different: the server keeps
`STRATUM_RECENT_JOBS` jobs solvable — minutes of history — while a vardiff
window is seconds. A miner can legitimately return work for a job that predates
several retargets, and judging it at the current difficulty throws away work it
performed exactly as instructed. Marketplaces report this as their **single most
common cause of failed pool integrations**: the pool passes the extranonce
check, then collapses the first time difficulty moves.

The credited difficulty is the job's too, not just the accept/reject verdict —
that value is the share's PPLNS weight, so judging correctly but crediting at
the current difficulty would misprice the share instead of dropping it.

The older `prev_difficulty` grace is **kept as a fallback**, for two cases the
per-job record does not cover: a submit for a job this connection was never sent
(no record), and a miner that applies a `mining.set_difficulty` to a *later* job
than the one it arrived with. Stratum does not pin down which job a
`set_difficulty` first applies to, and miners genuinely differ, so that
tolerance is deliberate.

Cost is one small ring per connection — `(STRATUM_RECENT_JOBS + 1) * 2` entries,
about 720 bytes, so ~350 KB at 500 connections — and a short linear scan per
notify and per submit, which is nothing beside the SHA-256 work already done.

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

## Policy, not protocol

The six requirements above are what a marketplace's *software* checks. They are
not the whole of what a marketplace enforces, and the rest is not discoverable
by testing your stratum.

🔴 **Concentration is the one that bites.** Marketplaces have rules against
delivering hashrate that hands a single pool majority control of a chain — the
51 %-attack scenario — and enforcing them by cancelling orders and blacklisting
the destination pool is normal, not exceptional. On a **small chain** this
threshold is far closer than it looks: an order that is unremarkable on Bitcoin
can be a supermajority of a young fork within the hour. The pool operator does
not choose this and cannot see it coming from stratum metrics.

**Watch your share of network hashrate, not just your pool's health.** Compute it
from block timestamps rather than a stats page — an epoch-averaged network
hashrate lags a surge badly and will tell you your share is small while it is
not:

```sh
# seconds per block over the last N blocks, straight from the chain
TIP=$(bitcoin-cli getblockcount)
T1=$(bitcoin-cli getblockheader $(bitcoin-cli getblockhash $TIP)        | jq .time)
T0=$(bitcoin-cli getblockheader $(bitcoin-cli getblockhash $((TIP-50))) | jq .time)
echo "interval: $(( (T1-T0) / 50 ))s/block"
# network hashrate ~= difficulty * 2^32 / interval; your share = your blocks / all blocks
```

⚠️ **A cancelled order is not evidence of a technical fault, and diagnosing it as
one wastes the window in which you could be talking to the marketplace.** Check
your reject rate for that address first: if it is zero, the cause is not your
stratum, and the answer is in the marketplace's message rather than your logs.

⛔ **Get the exact wording of any cancellation or blacklist notice before acting.**
"51 % risk", "unknown pool" and "unsupported chain" demand completely different
responses, and they are indistinguishable from your side.

🔴 **The conflict worth planning around:** a minimum-difficulty window (§"The one
case where this cannot work") is exactly when rented hashrate is cheapest to
point at a small chain — which is also when it most easily produces a
supermajority. **The window that makes rentals attractive is the window that
makes them a policy problem.** If a fork with a difficulty reset is on your
roadmap, settle this with the marketplace *before* it, not during.

## Known gaps

- 🔴 **No visibility into your share of network hashrate.** Nothing in the pool
  warns when it approaches a level that trips marketplace concentration policy,
  and the stats API's network hashrate is epoch-averaged so it understates
  during exactly the surge that matters. Until that exists, measure it by hand
  from block timestamps — see [Policy, not protocol](#policy-not-protocol).
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
servers (asserted **consecutive**, so it cannot pass by luck), the hint floor,
and per-job difficulty — a submit for a job issued two retargets ago is accepted
*and credited* at that job's difficulty, a share under its own job's difficulty
is still rejected, an unrecorded job still falls back to the grace path, and
re-notifying a job does not evict it from the ring.

⚠️ `TEST_EN2` in `tests/test_stratum.c` is guarded by a `_Static_assert` against
`STRATUM_EXTRANONCE2_SIZE`. Changing the width without updating the fixture is a
**compile** error rather than a wrong-width coinbase reaching a real block.
