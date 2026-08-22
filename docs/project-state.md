# Project state

## Current work: review fixes (branch `review-fixes-2026-08-22`)

Branched from `proportional` @ 5d74406. The pool is **running in production**,
so the rule for this branch is: fix what is clearly safe, write down what is
not. Deferred items live in [REVIEW_NOTES_2026-08-22.md](REVIEW_NOTES_2026-08-22.md).

### Landed

| Fix | Where | Severity |
|---|---|---|
| Payout retry after an unanswered broadcast could pay a batch twice | payout/lib/{payout,thunder}.js | high |
| `stratum_server_stop` returned while connection threads were still live (use-after-free on shutdown) | src/stratum.c | high |
| Job ids collided within a millisecond | src/main.c | low |
| Rejected shares evicted real entries from the server-wide dedupe ring | src/stratum.c | low |
| Tip-watcher `pthread_create` return was ignored | src/main.c | low |
| Template carrying both `coinbasevalue` and `coinbasetxn` degraded silently | src/bitcoind.c | low (warn only) |

### The one judgement call worth remembering

The payout fix is **stage-aware, not blanket**. An earlier version treated every
`submit`-stage failure as possibly-broadcast and parked the batch for an
operator. That was wrong in the common case: a mempool rejection is the node
*answering*, it happens routinely, and parking on it would turn the most
frequent transient failure into an outage — which is what the existing test
`a failed batch credits nobody and strands nobody` was protecting.

So `_call` now tags genuine JSON-RPC error responses with `rpcRejected`, and
only an **unanswered** submit (timeout, transport failure, unknown stage) keeps
the rows. That test was updated to say `rpcRejected` explicitly and renamed to
`a rejected batch …`; the unanswered case got its own test beside it.

### Verification status

- All 8 buildable C suites pass; `test_stratum` is 134 including a new
  shutdown-drain test, **verified to fail without the fix** (not just to pass
  with it).
- `test_broadcast` does not build here — hiredis is not installed, and its
  failure takes the whole `make test` target down with it. `broadcast.c` is
  untouched by this branch. On Arch/Manjaro: `pacman -S hiredis`.
- Payout suite: 53 tests pass. better-sqlite3 11.10.0 has no prebuild for
  Node 26 and will not compile, so these were run against a local shim over the
  built-in `node:sqlite` (untracked, since deleted). **They have not been run
  against real better-sqlite3** — worth doing on a machine with a supported Node
  before this ships.
