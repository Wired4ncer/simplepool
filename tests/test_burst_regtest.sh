#!/usr/bin/env bash
# BURST rehearsal: the min-difficulty window, on regtest.
#
#   bitcoind-patched  <-ZMQ/RPC-  bip300301_enforcer (walletless)
#          ^                              | GBT
#          | submitblock                  v
#          +----------------------- simplepool (pool_mode=proportional)
#                                         ^ stratum
#                                         |
#                              cpuminer.js x N_MINERS
#
# WHY THIS EXISTS, and why test_e2e_regtest.sh is not enough.
#
# At the eCash fork height the target resets to powLimit (src/pow.cpp:81-83,
# "eCash fork activation difficulty reset"), so difficulty is 1 for the 2016
# blocks to the next retarget. Difficulty 1 is ~7.16 MH/s-for-ten-minutes; a
# couple of ASICs are ~10^6 times that. Through that window blocks are NOT
# luck-bound, they are bound by how fast template -> coinbase -> submitblock
# can cycle. The pool will find blocks back-to-back.
#
# Every existing regtest test mines exactly ONE block and asserts its shape.
# One block never exercises what a burst does:
#   - a second block landing while the first is still settling
#   - many blocks settling inside ONE PPLNS window (prop_window_min_sec is a
#     600 s floor precisely because a work-only window is ~3 shares wide at
#     minimum difficulty)
#   - the deferred-claim ledger carrying fractions across consecutive blocks
#     instead of starting from empty
#
# So this mines N blocks in a row, in production mode, with several miners,
# and asserts the invariants that must hold for EVERY one of them.
#
# THE INVARIANTS (see PROPORTIONAL_PAYOUTS.md):
#   1. chain advanced by AT LEAST N. Not "exactly": three miners at difficulty
#      ~0 routinely solve two heights before the round loop can kill them, and
#      that overshoot is the harness losing a race, not a pool defect.
#   2. exactly as many blocks_found rows as the CHAIN GAINED — no duplicates,
#      and nothing extra. Regression guard for 40845c7 "Stop settling a block
#      from writing a second blocks_found row" and for 9212fac "Only record and
#      settle a block the node actually accepted"; a burst is the only thing
#      that makes that class of bug likely.
#   3. no (height,hash) recorded twice
#   4. every coinbase pays out EXACTLY subsidy+fees. Under and the difference
#      is never minted; over and the block is invalid. Checked per block
#      against getblockstats, not just on the tip.
#   5. prop_ledger stays zero-sum -- the pool holds nothing. Deferred claims
#      are signed FRACTIONS of a block reward, so they must cancel.
#   6. the pool process is still alive at the end
#
# It also REPORTS inter-block times. That number is the point of the exercise:
# it is the pool's real cycle time, and at the fork it is what caps how much
# of the window we can take.
#
# Deterministic by construction: fresh chain every run, binaries cached in
# REGTEST_BIN_DIR. Own data dir so it never collides with the dev stack, the
# coinbase e2e, or the payout e2e.
#
# Env:
#   REGTEST_DIR      data dir, WIPED each run (default: <repo>/.regtest-burst)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
#   BURST_BLOCKS     how many blocks to mine in a row (default 12)
#   BURST_MINERS     concurrent miners, 1..3 (default 3)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-burst}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
export REGTEST_SKIP_THUNDER=1
export REGTEST_WALLETLESS=1

BURST_BLOCKS="${BURST_BLOCKS:-12}"
BURST_MINERS="${BURST_MINERS:-3}"

BIN="$REGTEST_BIN_DIR"
POOL_BIN="$ROOT/build/simplepool"
POOL_CONF="/tmp/simplepool-burst.conf"
POOL_LOG="/tmp/simplepool-burst.log"
POOL_DB="/tmp/simplepool-burst.db"

PICKED=""
pick_port() {
    local p
    while :; do
        p=$(( (RANDOM % 20000) + 20001 ))
        [[ " $PICKED " == *" $p "* ]] && continue
        nc -z 127.0.0.1 "$p" 2>/dev/null && continue
        PICKED="$PICKED $p"
        printf -v "$1" '%s' "$p"
        return
    done
}

OPERATOR_ADDR="bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
# Three distinct valid regtest P2WPKH addresses. The pool validates payout
# addresses and rejects malformed ones outright, and they must differ so the
# proportional split has more than one claimant to divide between.
MINER_ADDRS=(
    "bcrt1qqufp62pn8ey4ghm2wkqgh94p4jmu9nwcpw8fns"
    "bcrt1q9sm5yn2cvdh8npy0n2jmpw7x68ww0uhafjy8yc"
    "bcrt1q29wxwuna3zfea2d5hl9dtc8t7cqsc9ez2rum2a"
)
POOL_PID=""

cli() { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
        -rpcuser=user -rpcpassword=password "$@"; }

stage() { echo; echo "=== burst: $1"; }
fail()  { echo "FAIL: $1" >&2; exit 1; }

dump_logs() {
    echo "!!! burst FAILED — recent logs:" >&2
    for f in "$REGTEST_DIR"/logs/*.log "$POOL_LOG"; do
        [ -f "$f" ] || continue
        echo "--- tail $f" >&2
        tail -40 "$f" >&2
    done
}

cleanup() {
    [ -n "$POOL_PID" ] && kill "$POOL_PID" 2>/dev/null || true
    "$ROOT/scripts/regtest/stop.sh" || true
    rm -rf "$LOCK"
}

# Same one-run-per-data-dir lock as the other suites: two runs sharing
# REGTEST_DIR would wipe each other's chain state mid-flight.
LOCK="$REGTEST_DIR.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "FAIL: $LOCK exists — another run of this suite is active." >&2
    echo "If it crashed and left the lock behind, clear it with:" >&2
    echo "  REGTEST_DIR=$REGTEST_DIR scripts/regtest/stop.sh && rm -rf $LOCK" >&2
    exit 1
fi
trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT
trap 'exit 130' INT TERM

[ "$BURST_MINERS" -ge 1 ] && [ "$BURST_MINERS" -le 3 ] \
    || fail "BURST_MINERS must be 1..3 (got $BURST_MINERS)"

stage "allocate stack ports"
pick_port REGTEST_BITCOIND_RPC_PORT
pick_port REGTEST_BITCOIND_ZMQ_PORT
pick_port REGTEST_ENFORCER_RPC_PORT
pick_port REGTEST_ENFORCER_GRPC_PORT
pick_port POOL_PORT
pick_port RENTAL_PORT
export REGTEST_BITCOIND_RPC_PORT REGTEST_BITCOIND_ZMQ_PORT \
       REGTEST_ENFORCER_RPC_PORT REGTEST_ENFORCER_GRPC_PORT
export ENFORCER_URL="http://127.0.0.1:$REGTEST_ENFORCER_GRPC_PORT"
echo "  bitcoind=$REGTEST_BITCOIND_RPC_PORT enforcer=$REGTEST_ENFORCER_RPC_PORT pool=$POOL_PORT rental=$RENTAL_PORT"
echo "  blocks=$BURST_BLOCKS miners=$BURST_MINERS"

stage "wipe burst data dir (fresh chain every run)"
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run"

stage "build simplepool"
make -C "$ROOT" -j >/dev/null

stage "download prebuilt binaries"
"$ROOT/scripts/regtest/setup.sh"

stage "start bitcoind-patched + walletless enforcer"
"$ROOT/scripts/regtest/start.sh"

stage "activate sidechain #9 via enforcer-template mining"
# Keeps the enforcer's GBT carrying the BIP301 commitment outputs that the
# coinbase builder must preserve byte-for-byte. Without this the burst would
# exercise a simpler template than the fork will actually hand us.
"$ROOT/scripts/regtest/activate-thunder.sh"

stage "start simplepool in PROPORTIONAL mode against enforcer GBT"
rm -f "$POOL_DB"
cat > "$POOL_CONF" <<EOF
listen_addr = 127.0.0.1
listen_port = ${POOL_PORT}

bitcoind_url = http://127.0.0.1:${REGTEST_ENFORCER_RPC_PORT}
bitcoind_poll_interval_ms = 500

operator_address = ${OPERATOR_ADDR}
# Non-zero on purpose: production alpha runs fee_bps=0, at which the fee falls
# below the dust limit and NO operator output is emitted at all. That would
# leave the multi-output coinbase path — the one mainnet will use — untested
# through the whole burst. 100 keeps the operator output in the shape.
fee_bps = 100
coinbase_tag = /simplepool-burst/

# ⛔ A SECOND COINBASE CAP, deliberately. The ring assertion at the end of this
# file is the only test that checks PROP_PLAN_RING covers every solvable job,
# and with no listener override it only ever exercised ONE plan per template --
# the single case where the old sizing was adequate. Per-listener caps make a
# template write one plan PER DISTINCT CAP, which is what exhausted the ring;
# a burst run that does not configure two caps cannot see that.
# 815 is the measured marketplace ceiling; 0 server-wide means uncapped
# elsewhere, so this is exactly the deploy recipe, with n_caps = 2.
prop_max_coinbase_bytes = 0
listener = port=${RENTAL_PORT} max_coinbase_bytes=815 label=rental

pool_mode = proportional
# Production values, deliberately. The 600 s floor means every block in this
# burst settles inside ONE window, which is exactly the min-difficulty case:
# a work-only window would be ~3 shares wide at difficulty 1.
prop_window_k        = 3
prop_window_min_sec  = 600

# Clamped down to network difficulty at connect time, so any nonce that finds
# a block also passes the share check (see cpuminer.js).
initial_diff = 0.0000001
vardiff_enabled = 0

db_path = ${POOL_DB}
log_level = debug
EOF
"$POOL_BIN" "$POOL_CONF" > "$POOL_LOG" 2>&1 &
POOL_PID=$!
for _ in $(seq 1 20); do nc -z 127.0.0.1 "$POOL_PORT" 2>/dev/null && break; sleep 1; done
kill -0 "$POOL_PID" 2>/dev/null || fail "simplepool died on startup"

stage "mine at least $BURST_BLOCKS blocks BACK-TO-BACK through the real stratum path"
TIP_BEFORE=$(cli getblockcount)
TARGET=$((TIP_BEFORE + BURST_BLOCKS))
echo "  tip before: $TIP_BEFORE   target: $TARGET"
BURST_START=$(date +%s)
LAST_TS=$BURST_START
GAPS=""
SEEN=$TIP_BEFORE
ROUND=0
# A round can yield more than one block, so rounds are not blocks. Bound the
# loop anyway: without this a pool that stops finding blocks spins forever.
MAX_ROUNDS=$((BURST_BLOCKS * 3 + 6))
while [ "$SEEN" -lt "$TARGET" ]; do
    ROUND=$((ROUND + 1))
    [ "$ROUND" -le "$MAX_ROUNDS" ] \
        || fail "gave up after $MAX_ROUNDS rounds at height $SEEN (target $TARGET)"
    # Miners run CONCURRENTLY: several claimants in one PPLNS window is the
    # case a single-miner test can never produce. The first to find the block
    # ends the round; the rest are killed and reconnect on the next iteration.
    # Round ends when THE CHAIN ADVANCES, not when a miner exits 0.
    # cpuminer.js prints "block found and accepted" on a submitblock that the
    # pool answered without a transport error — which is NOT the same as the
    # node accepting the block. Two miners racing the same height produce one
    # winner and one "inconclusive"; both miners report success. Polling the
    # height is the only measure of what actually happened.
    height_before=$(cli getblockcount)
    pids=""
    for m in $(seq 1 "$BURST_MINERS"); do
        node "$ROOT/scripts/regtest/cpuminer.js" \
            --port "$POOL_PORT" \
            --user "${MINER_ADDRS[$((m-1))]}.burst$m" \
            --timeout 120 >>"$POOL_LOG.miner" 2>&1 &
        pids="$pids $!"
    done
    found=0
    for _ in $(seq 1 240); do
        [ "$(cli getblockcount)" -gt "$height_before" ] && { found=1; break; }
        alive=0
        for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done
        [ "$alive" -eq 0 ] && break
        sleep 0.5
    done
    for p in $pids; do kill "$p" 2>/dev/null || true; done
    # Wait ONLY on the miner pids. A bare `wait` waits on EVERY background job
    # of this script, and simplepool is one of them ($POOL_PID, started with &
    # above) — it never exits, so the harness parked here forever at the end of
    # round 1, in do_wait, with the blocks already found and the miners already
    # reaped. The symptom looked like a pool stall and was not one.
    for p in $pids; do wait "$p" 2>/dev/null || true; done
    [ "$found" -eq 1 ] || fail "round $ROUND: chain did not advance past $height_before"

    now=$(date +%s)
    h=$(cli getblockcount)
    # A round routinely yields MORE than one block: at difficulty ~0 several
    # miners solve within milliseconds of each other and keep right on mining
    # while the kill is still in flight. Book one gap per block actually
    # gained so the throughput figure stays per-block, not per-round.
    gained=$((h - SEEN))
    for _ in $(seq 1 "$gained"); do
        GAPS="$GAPS $(( (now - LAST_TS) / gained ))"
    done
    LAST_TS=$now
    SEEN=$h
    printf "  round %2d: +%d block(s) -> tip=%s (%d/%d, +%ss)\n" \
        "$ROUND" "$gained" "$h" "$((SEEN - TIP_BEFORE))" "$BURST_BLOCKS" \
        "$((now - BURST_START))"
done
BURST_END=$(date +%s)
TIP_AFTER=$(cli getblockcount)
BLOCKS_MINED=$((TIP_AFTER - TIP_BEFORE))

stage "INVARIANT 1 — chain advanced by at least $BURST_BLOCKS"
# "At least", not "exactly". Overshoot is a property of the HARNESS losing the
# race to kill three miners, not of the pool, and asserting an exact count
# failed the run for the one reason the burst is supposed to produce. What the
# pool must get right is that it records exactly as many blocks as the chain
# actually gained — that is INVARIANT 2, against $BLOCKS_MINED.
echo "  height: $TIP_BEFORE -> $TIP_AFTER  (+$BLOCKS_MINED, asked for $BURST_BLOCKS)"
[ "$BLOCKS_MINED" -ge "$BURST_BLOCKS" ] \
    || fail "expected at least +$BURST_BLOCKS blocks, got +$BLOCKS_MINED"

stage "INVARIANT 4 — every coinbase pays EXACTLY subsidy+fees"
# Per block, not just the tip. This is the invariant that cannot be relaxed:
# pay less and the difference is simply never minted; pay more and the block
# is invalid.
for h in $(seq $((TIP_BEFORE + 1)) "$TIP_AFTER"); do
    hash=$(cli getblockhash "$h")
    expected=$(cli getblockstats "$h" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["subsidy"]+d["totalfee"])')
    paid=$(cli getblock "$hash" 2 | python3 -c '
import json,sys
b=json.load(sys.stdin)
cb=b["tx"][0]
print(sum(round(o["value"]*1e8) for o in cb["vout"]))')
    [ "$paid" = "$expected" ] \
        || fail "height $h: coinbase pays $paid sats, expected exactly $expected"
done
echo "  ✓ all $BLOCKS_MINED coinbases pay the reward exactly"

stage "stop the pool cleanly to flush the batched writer"
# Patience must exceed the GBT long poll. main.c does pthread_join(watcher)
# as the FIRST step of shutdown, and the watcher sits in a blocking long-polled
# getblocktemplate that parks up to 30 s. Nothing interrupts it, so SIGINT-to-
# exit is bounded by however much of that park is left. During mining every
# poll returns instantly (the chain keeps advancing), so the park only appears
# once the miners are dead — i.e. exactly here. 10 s was not enough and failed
# a run whose mining had been perfect.
sleep 1
kill -INT "$POOL_PID" 2>/dev/null || true
for _ in $(seq 1 45); do kill -0 "$POOL_PID" 2>/dev/null || break; sleep 1; done
kill -0 "$POOL_PID" 2>/dev/null \
    && fail "pool did not exit on SIGINT within 45 s (longer than one long-poll park)"
POOL_PID=""

stage "INVARIANT 2 — exactly $BLOCKS_MINED blocks_found rows, no duplicates"
# Counted against what the CHAIN gained, not against BURST_BLOCKS: every block
# the pool mined must be recorded once, and nothing else may be. A block that
# lost a submitblock race ("inconclusive") is not one the chain gained and must
# not appear here — cf. 9212fac.
# ⛔ COUNT CONFIRMED ROWS, NOT ALL ROWS. blocks_found is a CANDIDATE table --
# store.c: "A row is a *candidate* until something says otherwise. Only
# status='confirmed' means 'this pool mined a block that is in the chain' --
# every count and every solvency sum must filter on it." A losing submitblock
# race is recorded as status='rejected' ON PURPOSE, so it can be counted and
# investigated rather than vanishing.
#
# This assertion counted every row and so failed whenever the burst did the
# one thing a burst is for: several miners at difficulty ~0 racing the same
# height, where the node accepts one solution and refuses the rest. Measured
# 2026-09-06 on the DEPLOYED bba0118: 24 rows for 12 blocks -- 12 confirmed +
# 12 rejected, 24 DISTINCT hashes, not one duplicate. The failure message
# blamed a "duplicate settle" that was not happening, and the test could not
# pass on any recent commit. Assert what the schema actually promises.
ROWS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM blocks_found")
CONFIRMED=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM blocks_found WHERE status='confirmed'")
OTHER=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM blocks_found WHERE status NOT IN ('confirmed','rejected','orphaned','pending')")
echo "  blocks_found: $ROWS rows, $CONFIRMED confirmed (chain gained $BLOCKS_MINED)"
[ "$CONFIRMED" -eq "$BLOCKS_MINED" ] \
    || fail "expected exactly $BLOCKS_MINED CONFIRMED blocks_found rows, got $CONFIRMED (duplicate settle? cf. 40845c7 / 9212fac)"
[ "$OTHER" -eq 0 ] \
    || fail "$OTHER blocks_found rows carry a status outside {confirmed,rejected,orphaned,pending}"

stage "INVARIANT 3 — no (height,hash) recorded twice"
DUPES=$(sqlite3 "$POOL_DB" \
    "SELECT count(*) FROM (SELECT height,hash FROM blocks_found GROUP BY height,hash HAVING count(*)>1)")
[ "$DUPES" -eq 0 ] || fail "$DUPES duplicated (height,hash) rows in blocks_found"
echo "  ✓ no duplicates"

stage "INVARIANT 3b — every blocks_found row is ACTUALLY IN THE CHAIN"
# The one a single-block test can never reach. When two miners find the same
# height within milliseconds, submitblock accepts one and answers the other
# "inconclusive" — a valid block that lost the race and is not in the chain.
# Recording that as found overcounts blocks AND settles payouts against a
# reward nobody received.
# Both directions, because each catches the opposite lie. A CONFIRMED row that
# is not in the chain overcounts blocks and settles payouts against a reward
# nobody received. A REJECTED row that IS in the chain means the pool threw
# away a block it really mined -- and would never settle its window.
# ⛔ "getblock succeeded" IS NOT "in the chain". Core answers getblock for any
# block in its INDEX, stale ones included, and reports confirmations = -1 for a
# block it knows but has not built on. The original form of this check tested
# only that the node had heard of the block, while its name and message claimed
# it tested chain membership -- so a stale block recorded as found would have
# passed it. Read confirmations and mean what the stage says.
conf_of() {
    local out
    out=$(cli getblock "$1" 1 2>/dev/null) || { echo unknown; return; }
    printf '%s' "$out" | python3 -c "
import json, sys
try:
    print(json.load(sys.stdin).get('confirmations', 'unknown'))
except Exception:
    print('unknown')"
}
GHOSTS=0
for hash in $(sqlite3 "$POOL_DB" "SELECT hash FROM blocks_found WHERE status='confirmed'"); do
    c=$(conf_of "$hash")
    if [ "$c" = unknown ] || [ "$c" -lt 0 ] 2>/dev/null; then
        echo "  GHOST: $hash confirmed in blocks_found but confirmations=$c" >&2
        GHOSTS=$((GHOSTS + 1))
    fi
done
[ "$GHOSTS" -eq 0 ] || fail "$GHOSTS confirmed blocks_found rows are not on the active chain"
# The opposite lie: a block the pool disowned that the chain actually built on
# would be a block mined and never settled. A losing race is EXPECTED to be
# present-but-stale (confirmations = -1); that is not a defect, so only an
# ACTIVE-chain rejected row fails here.
DISOWNED=0
for hash in $(sqlite3 "$POOL_DB" "SELECT hash FROM blocks_found WHERE status='rejected'"); do
    c=$(conf_of "$hash")
    if [ "$c" != unknown ] && [ "$c" -ge 0 ] 2>/dev/null; then
        echo "  DISOWNED: $hash recorded rejected but is on the active chain (confirmations=$c)" >&2
        DISOWNED=$((DISOWNED + 1))
    fi
done
[ "$DISOWNED" -eq 0 ] || fail "$DISOWNED rejected blocks_found rows are on the active chain"
echo "  ✓ confirmed rows are all in the chain, rejected rows are all absent from it"

stage "INVARIANT 5 — prop_ledger is zero-sum (the pool holds nothing)"
# claim_fraction is a SIGNED fraction of one block reward: positive means the
# address was skipped and is owed a cut of a future block, negative means it
# was paid early. They must cancel. REAL, so compare against an epsilon.
LEDGER=$(sqlite3 "$POOL_DB" "SELECT COALESCE(SUM(claim_fraction),0) FROM prop_ledger")
ROWS_L=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM prop_ledger")
echo "  prop_ledger rows=$ROWS_L sum(claim_fraction)=$LEDGER"
python3 -c "
import sys
v=float('$LEDGER')
if abs(v) > 1e-9:
    print('FAIL: prop_ledger sums to %r, not zero — the pool is holding or owing funds' % v, file=sys.stderr)
    sys.exit(1)
" || exit 1
echo "  ✓ zero-sum"

# ...but an EMPTY table sums to zero trivially, so say whether that check meant
# anything. The deferred path has to have been entered and carried ACROSS
# blocks for a burst to have tested the thing a burst is for; a run where every
# block paid everyone in full proves conservation and nothing about carry.
SETTLES=$(grep -c "proportional: settled block" "$POOL_LOG" || true)
CARRIED=$(grep "proportional: settled block" "$POOL_LOG" \
          | grep -cvE "0 deferred claims" || true)
# ⛔ SHORT, STABLE SUBSTRING — and verified to still exist before it is trusted.
# This grep read "which had no payout plan", a whole sentence copied out of the
# log line. When that line was reworded (to stop it claiming the coinbase had
# "paid the finder directly", which was false), the grep silently stopped
# matching anything: NOPLAN became permanently 0 and the invariant below
# always passed. The assertion that exists to catch an under-sized plan ring
# was blinded by an edit to the message it reads, with nothing to show for it —
# the test still ran and still passed. Match the fewest words that identify the
# event, and fail loudly if even those stop being emitted.
# The phrase now names ONLY the real defect. It used to be "no payout plan",
# which the pool logged for two unrelated things: a plan built and lost by the
# ring (a silent ledger divergence) and a block found before the first PPLNS
# window exists (normal, and it happens on the first block of every burst).
# The assertion fired on the benign one and meant nothing. main.c distinguishes
# them now via had_payout_set, so grep for the defect.
NOPLAN_PHRASE="PLAN RING MISS"
if ! grep -qF "$NOPLAN_PHRASE" "$ROOT/src/main.c"; then
    fail "the log phrase this test greps for (\"$NOPLAN_PHRASE\") is no longer in src/main.c — the no-plan assertion below would silently pass forever. Update both together."
fi
NOPLAN=$(grep -cF "$NOPLAN_PHRASE" "$POOL_LOG" || true)
echo "  settles=$SETTLES with-carry=$CARRIED no-plan=$NOPLAN"
if [ "$CARRIED" -eq 0 ]; then
    echo "  ⚠️  VACUOUS: no block carried a deferred claim, so the zero-sum"
    echo "      assertion above cannot tell 'exercised and correct' from"
    echo "      'never happened'. Lower prop_min_payout_sats or add a miner"
    echo "      small enough to be deferred, and run it again."
fi
# A pooled block settling with no plan means the plan ring did not cover a job
# stratum still accepted a submit for -- the coinbase paid its finder solo
# instead of the window. PROP_PLAN_RING is sized off STRATUM_RECENT_JOBS to
# make this impossible; if it shows up, that sizing is wrong again.
if [ "$NOPLAN" -gt 0 ]; then
    fail "$NOPLAN block(s) settled with no payout plan -- PROP_PLAN_RING is not covering every solvable job"
fi

stage "INVARIANT 6 — pool survived the burst"
grep -iE "panic|segfault|assertion failed" "$POOL_LOG" && fail "pool log contains a crash signature"
echo "  ✓ no crash signature in the pool log"

stage "THROUGHPUT — the number this test exists to produce"
WORKERS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM workers WHERE payout_address IS NOT NULL")
SHARES=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM shares")
echo "  workers=$WORKERS shares=$SHARES blocks=$ROWS"
echo "  wall clock: $((BURST_END - BURST_START))s for $BURST_BLOCKS blocks"
echo "  per-block gaps (s):$GAPS"
python3 -c "
g=[int(x) for x in '''$GAPS'''.split()]
if g:
    print('  mean %.1fs  min %ds  max %ds' % (sum(g)/len(g), min(g), max(g)))
    print()
    print('  NOTE: on regtest this is dominated by cpuminer.js, NOT by the pool.')
    print('  Read it as an UPPER BOUND on pool cycle time, and watch it for a')
    print('  block that takes wildly longer than its neighbours — that is the')
    print('  shape a settle-path stall would have.')
"

stage "PASS"
