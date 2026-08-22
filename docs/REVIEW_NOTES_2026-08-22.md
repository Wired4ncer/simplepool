# Review findings not fixed on this branch

Companion to the fixes on `review-fixes-2026-08-22`. Everything here was found
in the same review and deliberately left alone, because the change is riskier
than the defect while the pool is running. Each entry says what is wrong, what
it costs today, and what the fix would be.

Ordered by how much they matter.

---

## 1. Job broadcast blocks on every miner in turn, under `conns_lock`

`stratum_server_set_job` (src/stratum.c) walks the connection list and does a
blocking `write_all` to each one while holding `conns_lock`. `SO_SNDTIMEO`
bounds a single write at `SEND_TIMEOUT_SEC` (10s) and a miner that trips it is
dropped, so one stalled peer cannot hang the pool forever — that part already
works.

What is not bounded is the **sum**. k stalled miners delay everyone after them
in the list by up to 10k seconds, and `conns_lock` is also what accept and
disconnect need, so new connections stall behind the same walk. Miners late in
the list keep mining the previous template for that whole window: work that
cannot win, on a pool whose entire purpose is fast template turnover.

**Why not fixed here:** the fix is to send outside the lock, which means
snapshotting the connection list — and a connection can be freed by its own
thread the moment the lock is released. Doing it safely needs per-connection
refcounting (the pattern `stratum_job_t` already uses for jobs) or a deferred
free. That is a real change to connection lifetime, in the same code as the
shutdown fix on this branch. Landing both at once makes a regression hard to
attribute.

**Fix when there is time:** give `stratum_conn_t` an `atomic_uint refs`, take a
reference under `conns_lock`, release the lock, write, then drop the reference;
free the connection at the last one. Then a slow miner delays only itself.

---

## 2. `ntime` from a miner is never validated

`handle_submit` accepts any 32-bit `ntime`. There is no economic exploit — the
hash still has to meet the target — but a solved block stamped outside the
consensus range (`mintime` .. now+2h) is rejected by the node as `time-too-old`
or `time-too-new`, and the reward is simply gone. This is the same class of
loss the extranonce2-length check exists to prevent, and that one is enforced.

`bitcoind_parse_template` already reads `mintime` into `t->min_time`, and
already clamps `curtime` up to it. The value is then dropped: it never reaches
`stratum_job_t`, so submit-time has nothing to check against.

**Why not fixed here:** the check has to reject, and a wrong bound rejects
*valid* shares from every miner at once. Getting the roll window right wants
observation of what the fleet actually sends before it is enforced.

**Fix when there is time:** carry `min_time` onto the job (an additive
`stratum_job_set_time_bounds()` avoids touching the `stratum_job_new` signature
and its callers), then log — do not reject — when a submission falls outside
`[min_time, ntime + 7200]`. Run that for a while. If the log stays quiet, turn
it into a reject.

---

## 3. `blocks_found.reward_sats` can disagree with what the coinbase paid

`on_block_found` recomputes reward and fee from `job->value_sats` and the dust
rule. In `pool_mode=proportional` the coinbase actually pays amounts derived
from `coinbase_template_reward()`, read out of the serialized `coinbasetxn`.
When a backend's `coinbasevalue`/negated-fee disagrees with the coinbase it
also sent, the recorded row is wrong.

The ledger of record is the block itself, so nothing is mispaid — but this is
the number the dashboard shows an operator, and a payout figure that is quietly
approximate is worse than one that is obviously missing.

**Fix when there is time:** pass the plan's `reward_after_fee` (already exact,
already per-job) through to the block-found path instead of recomputing.

---

## 4. Vardiff measures intervals on `CLOCK_REALTIME`

`now_ms()` in src/stratum.c reads `CLOCK_REALTIME`, and its result is what
`vd_window_start_ms` and `diff_changed_ms` are compared against. An NTP step
therefore lands directly in vardiff: forward, and the difficulty grace period
after a retarget expires early, so in-flight shares at the old difficulty are
rejected; backward, and `ts_now - diff_changed` underflows (both are `uint64_t`)
to an enormous number, which also drops the grace.

Self-correcting within a window or two, and only on a clock step — which is why
it is here and not on the branch.

**Fix when there is time:** `mono_ms()` already exists. Switch the vardiff
window and grace fields to it, at all four sites (`handle_authorize`,
`vardiff_check_idle`, `vardiff_maybe_retarget`, and the grace comparison in
`handle_submit`). Keep `ts_now` on the realtime clock — it is the share's
timestamp and goes to the store and the observers.

---

## 5. A payout batch that leaves no change can strand settlement permanently

`settlementState` (payout/lib/payout.js) treats a batch as confirmed on either
`get_transaction` reporting a `block_hash` — which stops being true once the
sidechain moves past the block — or a wallet UTXO carrying the txid. A batch
that consumed the wallet exactly, leaving no change output, has no second
signal. Miss the `get_transaction` window and the state is `unknown` forever,
which deliberately halts all payouts until a human intervenes.

The refusal to guess is right: the alternative is paying twice. But the case is
reachable without anything going wrong, and it is not in the runbook.

**Fix when there is time:** nothing in the code. Add the recovery procedure to
payout/README.md, next to the existing reconciliation section — how to confirm
the transaction out of band and hand-finalize the batch.

---

## Not a finding, but worth knowing

`make test` does not build on a machine without hiredis: `tests/test_broadcast.mk`
fails at `#include <hiredis/hiredis.h>` and takes the whole target with it, so
the other eight suites never run. On Arch/Manjaro that is `pacman -S hiredis`.
Making the broadcast suite skip rather than fail when the header is absent would
let the rest of the suite run anywhere.
