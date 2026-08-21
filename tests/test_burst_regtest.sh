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
export REGTEST_BITCOIND_RPC_PORT REGTEST_BITCOIND_ZMQ_PORT \
       REGTEST_ENFORCER_RPC_PORT REGTEST_ENFORCER_GRPC_PORT
export ENFORCER_URL="http://127.0.0.1:$REGTEST_ENFORCER_GRPC_PORT"
echo "  bitcoind=$REGTEST_BITCOIND_RPC_PORT enforcer=$REGTEST_ENFORCER_RPC_PORT pool=$POOL_PORT"
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
ROWS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM blocks_found")
echo "  blocks_found rows: $ROWS (chain gained $BLOCKS_MINED)"
[ "$ROWS" -eq "$BLOCKS_MINED" ] \
    || fail "expected exactly $BLOCKS_MINED blocks_found rows, got $ROWS (duplicate settle? cf. 40845c7 / 9212fac)"

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
GHOSTS=0
for hash in $(sqlite3 "$POOL_DB" "SELECT hash FROM blocks_found"); do
    if ! cli getblock "$hash" 1 >/dev/null 2>&1; then
        echo "  GHOST: $hash recorded in blocks_found but NOT in the chain" >&2
        GHOSTS=$((GHOSTS + 1))
    fi
done
[ "$GHOSTS" -eq 0 ] || fail "$GHOSTS blocks_found rows are not in the chain"
echo "  ✓ every recorded block is in the chain"

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
