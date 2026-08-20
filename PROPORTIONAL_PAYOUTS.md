# `proportional` — coinbase-direct PPLNS

This is the design behind `pool_mode = proportional`: every miner in the
PPLNS window is paid **directly from the block's coinbase**, pro-rata by
share difficulty. There is no pool wallet, no payout worker, and no
balance held on anyone's behalf.

It is implemented and unit-tested; it has been run against a live CUSF
enforcer template, but **no block has been found with it yet** — see
[Status](#status).

## What it is, in one paragraph

`solo` pays whoever found the block. `proportional` pays everyone whose
shares are in the window. Both build the coinbase from the server's
`getblocktemplate` and rewrite its single spendable output; solo rewrites
it per-connection, proportional replaces it with N outputs shared by
every connection. The commitments the server put in the coinbase —
BIP300/301 messages, the segwit witness commitment — are preserved
byte-for-byte in both.

| | `solo` | `proportional` |
| --- | --- | --- |
| Coinbase | per-miner; each job pays its own connection | **one shared coinbase**, sessions differ only by `extranonce1` |
| Paid on a block | the finder | every address in the window, pro-rata |
| Operator fee | one extra output | one extra output (unchanged) |
| State | none | `prop_ledger` — deferred *claims*, not balances |
| Pool holds funds | no | no |

## The window

Measured in **work**, not share count and not wall-clock:

```
W      = prop_window_k × current_network_difficulty
window = newest shares walking back until Σ share.difficulty ≥ W
                                   AND   the walk reached ≥ prop_window_min_sec back
```

The window is whichever of the two is **larger**.

Work alone is the right unit because a share-count window changes meaning
by orders of magnitude across a retarget. But work alone is not enough:
on a chain whose difficulty can reset to `powLimit`, `k` blocks of work
can be a handful of shares — a window seconds wide, which squeezes out
every miner but the most recent. `prop_window_min_sec` is the floor that
keeps the window meaningful whatever the difficulty does.

Note that "3 blocks of work" means three of **this pool's** expected
blocks. At a small share of network hashrate that is a long time in
wall-clock — good hopping resistance, but a new miner's share ramps up
over roughly that period, which is worth telling miners plainly.

Whole seconds are indivisible: every share sharing the boundary timestamp
is included, so the window never splits a second between two miners.

## Payout computation, per template

1. Aggregate the window by **payout address** (not worker name — see
   [Addresses, not workers](#addresses-not-workers)).
2. Each address's claim = its share of the window + whatever the ledger
   says it is owed.
3. Anything below `prop_min_payout_sats`, or outside the number of
   outputs the block's spare weight allows, is **deferred** rather than
   dropped.
4. The payouts are **renormalised over the addresses actually being
   paid**, so the outputs sum to exactly `reward − fee`.
5. The ledger records what each address was owed minus what it received.

### The invariant everything rests on

> **The coinbase pays the block reward exactly.** Not approximately, and
> not "plus carry".

Under it, the difference is never minted — value simply vanishes. Over
it, the block is invalid. Both P2Pool implementations enforce the same
rule (Monero P2Pool's `split_reward` double-checks the total; Bitcoin
P2Pool raises if the outputs do not sum to the subsidy), and
`coinbase_build_from_template_multi` refuses to build a coinbase that
violates it.

`main.c` re-checks it before handing the payouts to the builder. With the
current model that check should never fire; it stays because the cost of
being wrong is an invalid block, not a bad log line.

### Deferred claims are weight, not satoshis

`prop_ledger` holds a **signed fraction of one block reward** per
address:

- `> 0` — the address was skipped and is owed that fraction of a future
  block.
- `< 0` — it was paid early, covering someone else's skipped share, and
  owes it back.
- **The ledger sums to zero**, always. Every block pays exactly one
  reward, so an advance to one miner is a deferral by another.

Two rejected alternatives, both of which look simpler and are wrong:

**Carrying satoshis** cannot be settled inside one coinbase. Holding back
sats for a small miner makes that block pay less than the reward, and the
shortfall is never minted — the debt is backed by nothing. Releasing it
later needs a coinbase that overpays, which is an invalid block.

**Carrying raw difficulty** double-counts. PPLNS shares stay in the
window across several blocks, so rolling a skipped miner's difficulty
forward counts the same work twice. Difficulty is also not comparable
across time on a chain that retargets ±4× and resets at a fork: a claim
recorded in difficulty units silently changes meaning at every retarget.
A fraction of a block reward does not.

A claim worth less than one satoshi of the block that produced it is
dropped rather than carried — it can never be paid, so carrying it is
noise. The ledger is therefore zero-sum to within a satoshi per
participant. What stays exact is the payout total.

## The output cap is derived, not configured

Every payout output beyond the first is weight the template did **not**
budget for. A server that dictates the coinbase reserves room for the
outputs it put there — the CUSF enforcer reserves exactly one payout
txout — so each extra P2WPKH output costs 31 bytes = **124 WU** of the
block's remaining slack, as does the extranonce and tag spliced into the
scriptSig. On a nearly-full block that surplus is what pushes the block
over the limit and loses it at `submitblock`.

So `prop_max_outputs` is an **upper bound**, not the operating value.
The real cap is computed per template from BIP22 `weightlimit` and the
transactions' own weights (`coinbase_max_payout_outputs`). Measured on a
live node, a 1,710-transaction template left 2,685 WU — room for 21
outputs — while a static cap of 12 was deferring miners; on a fuller
template 12 would have been too many.

When the server omits `weightlimit` there is nothing to measure and the
configured value stands. Guessing a limit is worse than deferring to the
operator.

## Addresses, not workers

The window keys on **payout address**. Worker labels (`address.rig1`) are
display-only, deliberately: keying on worker name would let anyone split
their hashrate across labels to game the window. Two machines mining to
one address are one participant sharing one payout — which surprises
people, so say it in your front-end.

## What miners will see

A miner whose claim is deferred **is not in that block's coinbase**.
AxeOS 2.13.x and similar firmware decode the coinbase from
`mining.notify`, match the outputs against the configured address, and
warn when they find none. That warning is correct and it is a good
feature — do not tell miners to switch it off. Explain instead:

- deferral is not loss; the claim is kept and paid from a later block,
- the same banner on a **custodial** pool is permanent, because that
  coinbase pays the pool and never the miner,
- and their payment, when it comes, is verifiable by anyone in the block.

## Config

```ini
pool_mode = proportional

# Window: whichever is LARGER, k blocks of work or this many seconds.
prop_window_k        = 3
prop_window_min_sec  = 600

# Below this, an address is deferred rather than paid. Must be >= the
# 546-sat dust limit. Higher values mean small miners appear in fewer
# coinbases, which is what trips miner-firmware warnings.
prop_min_payout_sats = 1000000

# Upper bound on payout outputs. The effective cap is derived per
# template from the block's spare weight; this only limits it.
prop_max_outputs     = 12
```

`operator_address` and `fee_bps` behave exactly as in solo mode. Note
that at `fee_bps = 0` the fee falls below the dust limit and **no
operator output is emitted at all** — a materially different coinbase
shape from a non-zero fee, so test whichever one you intend to run.

## Requirements and fallbacks

Proportional mode needs a **server-provided coinbase** (`coinbasetxn`),
i.e. a backend like the CUSF enforcer. Without one there is nothing to
rewrite and no commitments to preserve.

Every failure path falls back to per-miner (solo-shaped) coinbases rather
than dropping jobs — no window yet, no server coinbase, an unreadable
reward, or payouts that fail the conservation check. **The pool never
stops handing out work.** Paying the finder directly is always valid and
never custodial; it is simply not yet proportional.

## Settlement

The ledger is committed when a block is found, keyed on the **job id** of
the job that was actually solved — several templates can share a height,
each with its own window, and settling the wrong one would mis-pay.

⚠️ Settlement happens on pool-side acceptance, not network confirmation.
A block that is later reorged out leaves the ledger reflecting a payment
that no longer exists. No funds are at stake — the ledger only records
whose turn it is — but the fairness memory is wrong until it washes out.

## Database

```sql
CREATE TABLE prop_ledger (
  address         TEXT PRIMARY KEY,
  claim_fraction  REAL NOT NULL DEFAULT 0,   -- signed, sums to zero
  last_settled_ts INTEGER
);
```

It is replaced wholesale on settlement: `pplns_compute_payouts` returns
the complete post-block state, and a claim that has settled to zero is
absent from it. Merging row by row would leave stale rows and break the
sums-to-zero property.

An earlier `prop_balances` table held satoshis. It never ran anywhere and
the migration drops it rather than converting it.

## Status

- ✅ Payout maths, builder, store and stratum integration — implemented,
  unit-tested, `-Werror` and sanitizer clean.
- ✅ Verified against a **live enforcer template**: OP_RETURNs preserved
  byte-for-byte, outputs summing exactly to the template reward, fee
  exactly `fee_bps`, locktime copied.
- ✅ Two miners on distinct addresses, one shared coinbase, both paid
  pro-rata, on a live pool.
- ⛔ **No block has been found in this mode.** `submitblock`, the
  block-found callback and ledger settlement are covered by unit tests
  only. Treat the first real block as the test it is, and read the ledger
  afterwards.
