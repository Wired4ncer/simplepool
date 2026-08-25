#!/usr/bin/env bash
# End-to-end test of the pps-classic mining path, one-shot for CI:
#
#   bitcoind-patched  <-ZMQ/RPC-  bip300301_enforcer (walletless)
#          ^                              | GBT
#          | submitblock                  v
#          +----------------------- simplepool (pps-classic)
#                                         ^ stratum
#                                         |
#                                  cpuminer.js
#
# Ports are allocated per run (pick_port), so this coexists with a dev
# stack in .regtest/ and with the payout e2e.
#
# Stages:
#   1. download + start bitcoind-patched and a walletless enforcer
#      (REGTEST_SKIP_THUNDER=1 REGTEST_WALLETLESS=1 — thunder and the
#      enforcer wallet play no part in the coinbase-shape e2e)
#   2. basic stratum smoke test against bitcoind directly
#      (tests/test_integration.sh: subscribe/authorize/reject + sqlite)
#   3. activate sidechain #9 by mining enforcer-template blocks. The pool
#      no longer emits drivechain coinbases, but keeping the sidechain
#      active means the enforcer's GBT template still carries the BIP301
#      commitment outputs the coinbase builder has to preserve.
#   4. run simplepool in pool_mode=pps-classic against the enforcer GBT
#   5. mine ONE block through the real stratum path with cpuminer.js
#   6. assert the mined tip's coinbase has the classic shape
#      (inspect-coinbase.sh: pool wallet output + operator fee output,
#      and no OP_DRIVECHAIN)
#
# Deterministic by construction: every run starts from a completely
# fresh data dir (chain state, enforcer DB, logs are wiped), while the
# downloaded binaries are cached in REGTEST_BIN_DIR across runs. The
# data dir defaults to .regtest-e2e/ so it never collides with a local
# dev stack in .regtest/ — a bare `bash tests/test_e2e_regtest.sh` is
# safe.
#
# Env:
#   REGTEST_DIR      e2e data dir, WIPED each run (default: <repo>/.regtest-e2e)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-e2e}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
export REGTEST_SKIP_THUNDER=1
export REGTEST_WALLETLESS=1

BIN="$REGTEST_BIN_DIR"
POOL_BIN="$ROOT/build/simplepool"
POOL_CONF="/tmp/simplepool-e2e.conf"
POOL_LOG="/tmp/simplepool-e2e.log"
POOL_DB="/tmp/simplepool-e2e.db"

# Pick a free port and assign it to the named variable. Not $()-command
# substitution: PICKED must accumulate across calls so two picks can't
# return the same not-yet-bound port.
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
# The pool's own BTC wallet — where pps-classic sends the net-of-fee
# reward. Must differ from OPERATOR_ADDR so the assertion below can tell
# the two coinbase outputs apart.
POOL_BTC_ADDR="bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt"
POOL_PID=""

cli() { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
        -rpcuser=user -rpcpassword=password "$@"; }

stage() { echo; echo "=== e2e: $1"; }

dump_logs() {
    echo "!!! e2e FAILED — recent logs:" >&2
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

# One run per data dir: two runs of the SAME suite share REGTEST_DIR
# (pidfiles + chain state), so the second run's wipe pulls the rug from
# under the first, and the first's cleanup then kills the second's
# freshly started daemons via the recreated pidfiles. Different suites
# coexist fine (own dirs, dynamic ports); same-suite runs are excluded
# here. Take the lock BEFORE installing traps — a refused run must not
# stop the owner's stack on its way out.
LOCK="$REGTEST_DIR.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "FAIL: $LOCK exists — another run of this suite is active." >&2
    echo "If it crashed and left the lock behind, clear it with:" >&2
    echo "  REGTEST_DIR=$REGTEST_DIR scripts/regtest/stop.sh && rm -rf $LOCK" >&2
    exit 1
fi

trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT
# Chain INT/TERM into the EXIT trap — bash skips EXIT traps when killed
# by an unhandled signal, which is how Ctrl-C used to orphan the stack.
trap 'exit 130' INT TERM

stage "allocate stack ports"
# Dynamic per-run ports: this test can run alongside a dev stack (or
# the payout e2e) without cross-wiring or refusing to start.
pick_port REGTEST_BITCOIND_RPC_PORT
pick_port REGTEST_BITCOIND_ZMQ_PORT
pick_port REGTEST_ENFORCER_RPC_PORT
pick_port REGTEST_ENFORCER_GRPC_PORT
pick_port POOL_PORT
pick_port INT_POOL_PORT
pick_port RENTAL_A_PORT
pick_port RENTAL_B_PORT
pick_port RENTAL_C_PORT
export REGTEST_BITCOIND_RPC_PORT REGTEST_BITCOIND_ZMQ_PORT \
       REGTEST_ENFORCER_RPC_PORT REGTEST_ENFORCER_GRPC_PORT
export ENFORCER_URL="http://127.0.0.1:$REGTEST_ENFORCER_GRPC_PORT"
echo "  bitcoind=$REGTEST_BITCOIND_RPC_PORT zmq=$REGTEST_BITCOIND_ZMQ_PORT" \
     "enforcer=$REGTEST_ENFORCER_RPC_PORT/$REGTEST_ENFORCER_GRPC_PORT" \
     "pool=$POOL_PORT smoke-pool=$INT_POOL_PORT" \
     "rental=$RENTAL_A_PORT/$RENTAL_B_PORT/$RENTAL_C_PORT"

stage "wipe e2e data dir (fresh chain every run)"
# Chain state must not leak between runs: a pre-activated sidechain or
# an aged chain changes what the later stages actually exercise (e.g.
# median-time-past vs curtime). Binaries in $BIN are the only cache.
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run"

stage "build simplepool"
make -C "$ROOT" -j >/dev/null

stage "download prebuilt binaries"
"$ROOT/scripts/regtest/setup.sh"

stage "start bitcoind-patched + walletless enforcer"
"$ROOT/scripts/regtest/start.sh"

stage "basic stratum smoke test (solo mode, direct bitcoind)"
PATH="$BIN:$PATH" BITCOIND_USER=user BITCOIND_PASS=password \
    PORT="$INT_POOL_PORT" RPC_PORT="$REGTEST_BITCOIND_RPC_PORT" \
    bash "$HERE/test_integration.sh"

stage "activate sidechain #9 via enforcer-template mining"
"$ROOT/scripts/regtest/activate-thunder.sh"

stage "start simplepool in pps-classic mode against enforcer GBT"
rm -f "$POOL_DB"
cat > "$POOL_CONF" <<EOF
listen_addr = 127.0.0.1
listen_port = ${POOL_PORT}

bitcoind_url = http://127.0.0.1:${REGTEST_ENFORCER_RPC_PORT}
bitcoind_poll_interval_ms = 500

operator_address = ${OPERATOR_ADDR}
fee_bps = 100
coinbase_tag = /simplepool-e2e/

pool_mode = pps-classic
pool_btc_address = ${POOL_BTC_ADDR}
# The share difficulty is clamped to the network difficulty, which on
# regtest is ~4.66e-10 — so the rate must be huge for a share to accrue
# whole sats (credit = trunc(difficulty * pps_sats_per_diff), 0 is
# dropped). 1e10 * 4.66e-10 ≈ 4 sats.
pps_sats_per_diff = 10000000000

# Clamped down to the network difficulty at connect time, so any nonce
# that finds a block also passes the share check (see cpuminer.js).
#
# listen_port declares no min_diff, so nothing overrides that clamp here and
# CPU mining stays possible. Only the rental listeners below promise a floor,
# and cpuminer.js never dials those.
initial_diff = 0.0000001
vardiff_enabled = 0

# Two more ports, to prove a `listener` line becomes a real bound socket
# serving its own difficulty. Both are set BELOW the regtest network
# difficulty (~4.66e-10) on purpose: share difficulty is clamped to the
# chain, so anything above it would come back clamped and all three ports
# would look identical — which would make this assert nothing.
listener = port=${RENTAL_A_PORT} min_diff=1e-10 label=rental-a
listener = port=${RENTAL_B_PORT} min_diff=3e-10 label=rental-b

# And one ABOVE the chain's own difficulty, which is the case min_diff exists
# for. Everything else about a rental port can be demonstrated below the
# clamp; this is the one thing that cannot, because it IS the clamp being
# overridden. A marketplace measures the difficulty on the wire, so a port
# that promised 1e-8 has to serve 1e-8 even on a chain sitting at 4.66e-10 --
# the alternative is an order cancelled for a reason nothing here records.
# Nothing mines this port, so the blocks that trade costs are not at stake in
# this run.
listener = port=${RENTAL_C_PORT} min_diff=1e-8 label=rental-c

# Low enough that a flood trips it immediately. cpuminer.js only submits
# when it beats the NETWORK target — about one submit per run — so this
# cannot interfere with the block-mining stage above.
max_submits_per_sec = 100

db_path = ${POOL_DB}
log_level = debug
EOF
"$POOL_BIN" "$POOL_CONF" > "$POOL_LOG" 2>&1 &
POOL_PID=$!
for _ in $(seq 1 20); do nc -z 127.0.0.1 "$POOL_PORT" 2>/dev/null && break; sleep 1; done
if ! kill -0 "$POOL_PID" 2>/dev/null; then
    echo "simplepool died on startup" >&2
    exit 1
fi

stage "mine one block through stratum with cpuminer.js"
TIP_BEFORE=$(cli getblockcount)
node "$ROOT/scripts/regtest/cpuminer.js" --port "$POOL_PORT" --timeout 120
TIP_AFTER=$(cli getblockcount)
echo "height: $TIP_BEFORE -> $TIP_AFTER"
if [ "$TIP_AFTER" -le "$TIP_BEFORE" ]; then
    echo "FAIL: block was submitted but the chain did not advance" >&2
    exit 1
fi

stage "assert pps-classic coinbase shape on the new tip"
POOL_BTC_ADDRESS="$POOL_BTC_ADDR" OPERATOR_ADDRESS="$OPERATOR_ADDR" \
    "$ROOT/scripts/regtest/inspect-coinbase.sh"

stage "assert every stratum port serves its own difficulty"
# The unit tests cover the policy itself. What only a running pool can show
# is the wiring: that a `listener` line became a bound socket, and that the
# port a miner dials decides what it is handed.
probe() { node "$ROOT/scripts/regtest/stratum-probe.js" --port "$1" --timeout 30; }

# rental-a and rental-b are only meaningful while they sit below the chain's
# difficulty: they are there to show that a port serves ITS OWN number, and
# picking values under the ceiling keeps that separable from the floor. (The
# floor itself is rental-c's job, above.) Check the assumption rather than
# trust it: if regtest ever moves, this should say so plainly instead of
# failing as a difficulty mismatch nobody can explain. WAL means the live DB
# reads fine while the pool is running.
NET_DIFF=$(sqlite3 "$POOL_DB" "SELECT COALESCE(network_difficulty,0) FROM pool_meta WHERE id = 1")
echo "  network difficulty=$NET_DIFF"
awk -v d="$NET_DIFF" 'BEGIN { exit !(d > 3e-10) }' || {
    echo "FAIL: network difficulty $NET_DIFF is not above the 3e-10 that" >&2
    echo "      rental-a and rental-b configure — they would then be holding" >&2
    echo "      floors rather than sitting under the ceiling, which is" >&2
    echo "      rental-c's job, and this stage would prove it twice and the" >&2
    echo "      clamp not at all." >&2
    echo "      Lower the rental-a/rental-b min_diff values in this script." >&2
    exit 1
}

PROBE_D=$(probe "$POOL_PORT")
PROBE_A=$(probe "$RENTAL_A_PORT")
PROBE_B=$(probe "$RENTAL_B_PORT")
PROBE_C=$(probe "$RENTAL_C_PORT")
echo "  default  $PROBE_D"
echo "  rental-a $PROBE_A"
echo "  rental-b $PROBE_B"
echo "  rental-c $PROBE_C"

# The gate the marketplaces actually check, on every port.
for p in "$PROBE_D" "$PROBE_A" "$PROBE_B" "$PROBE_C"; do
    echo "$p" | jq -e '.extranonce2_size >= 7' >/dev/null || {
        echo "FAIL: a port advertises extranonce2_size below 7" >&2; exit 1; }
done

# Each rental port serves what it was configured for, not the default and
# not each other. Ranges rather than equality: these are doubles that have
# been through JSON twice.
echo "$PROBE_A" | jq -e '.difficulty > 0.9e-10 and .difficulty < 1.1e-10' >/dev/null || {
    echo "FAIL: rental-a did not serve its configured difficulty" >&2; exit 1; }
echo "$PROBE_B" | jq -e '.difficulty > 2.9e-10 and .difficulty < 3.1e-10' >/dev/null || {
    echo "FAIL: rental-b did not serve its configured difficulty" >&2; exit 1; }
DIFF_A=$(echo "$PROBE_A" | jq -r '.difficulty')
DIFF_B=$(echo "$PROBE_B" | jq -r '.difficulty')
[ "$DIFF_A" != "$DIFF_B" ] || {
    echo "FAIL: both rental ports served the same difficulty" >&2; exit 1; }

# rental-c promised a floor above the chain, and gets it. This is the single
# assertion that a probe pointed at a rental port reads what the port
# advertises rather than what the chain can back -- which is the whole reason
# a marketplace can be served at all. Clamped, it would come back at
# NET_DIFF (~4.66e-10) instead, four orders of magnitude down.
echo "$PROBE_C" | jq -e '.difficulty > 0.9e-8 and .difficulty < 1.1e-8' >/dev/null || {
    echo "FAIL: rental-c did not hold its promised min_diff above the" >&2
    echo "      network difficulty — it served $(echo "$PROBE_C" | jq -r .difficulty)" >&2
    exit 1; }

# And the default port, which promised nothing, is still clamped down to the
# chain. Both behaviours in one run: the floor is opt-in, not global.
echo "$PROBE_D" | jq -e ".difficulty <= ($NET_DIFF * 1.01)" >/dev/null || {
    echo "FAIL: the default port was not clamped to the network difficulty" >&2
    exit 1; }

stage "assert the submit ceiling refuses a flood"
# Against a job id that does not exist. The ceiling is checked before a
# submit's params are read, so a refused one never reaches validation either
# way — but the ones UNDER the ceiling do, and here the share target is
# clamped to a network difficulty so low that roughly one hash in two beats
# it. Flooding the live job would not measure the ceiling, it would mine
# fifty blocks.
FLOOD=$(node "$ROOT/scripts/regtest/stratum-probe.js" \
        --port "$POOL_PORT" --flood 600 --job-id nosuchjob --timeout 60)
echo "  $FLOOD"
echo "$FLOOD" | jq -e '.refused_too_fast > 100' >/dev/null || {
    echo "FAIL: the submit ceiling did not refuse a 600-submit flood" >&2; exit 1; }

# Throttled, not banned, and not fatal: the pool still serves after it.
PROBE_AFTER=$(probe "$RENTAL_A_PORT")
echo "$PROBE_AFTER" | jq -e '.difficulty > 0' >/dev/null || {
    echo "FAIL: pool stopped serving after the flood" >&2; exit 1; }

stage "assert pool DB recorded the accepted share"
# give the batched writer a moment, then stop the pool cleanly to flush
sleep 1
kill -INT "$POOL_PID" 2>/dev/null || true
for _ in 1 2 3 4 5; do kill -0 "$POOL_PID" 2>/dev/null || break; sleep 1; done
POOL_PID=""
WORKERS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM workers WHERE payout_address IS NOT NULL")
SHARES=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM shares")
BLOCKS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM blocks_found")
CREDITS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM pps_credits WHERE accrued_sats > 0")
echo "workers=$WORKERS shares=$SHARES blocks_found=$BLOCKS pps_credits=$CREDITS"
[ "$WORKERS" -ge 1 ] || { echo "FAIL: no worker with payout_address" >&2; exit 1; }
[ "$SHARES"  -ge 1 ] || { echo "FAIL: no accepted shares" >&2; exit 1; }
[ "$BLOCKS"  -ge 1 ] || { echo "FAIL: no blocks_found row" >&2; exit 1; }
[ "$CREDITS" -ge 1 ] || { echo "FAIL: no pps credit accrued" >&2; exit 1; }

stage "assert the proxy published its ports"
# The dashboard reads the DB and nothing else, so an unpublished port list
# is a banner that cannot tell a miner which port to use.
LISTENERS=$(sqlite3 "$POOL_DB" "SELECT COALESCE(listeners,'') FROM pool_meta WHERE id = 1")
echo "  listeners=$LISTENERS"
echo "$LISTENERS" | jq -e 'length == 4' >/dev/null || {
    echo "FAIL: pool_meta.listeners does not describe all four ports" >&2; exit 1; }
# promised_min_diff is what lets the dashboard tell "holding a floor and
# losing blocks" from "quietly serving less than advertised". They need
# opposite advice, so publishing only one number cannot express both.
echo "$LISTENERS" | jq -e 'map(select(.label == "rental-c" and .promised_min_diff > 0)) | length == 1' >/dev/null || {
    echo "FAIL: pool_meta.listeners does not record the promised floor" >&2; exit 1; }
echo "$LISTENERS" | jq -e 'map(select(.port == '"$POOL_PORT"' and .promised_min_diff == 0)) | length == 1' >/dev/null || {
    echo "FAIL: the default port is published as promising a floor" >&2; exit 1; }
echo "$LISTENERS" | jq -e 'map(select(.label == "rental-a")) | length == 1' >/dev/null || {
    echo "FAIL: labelled listener missing from pool_meta.listeners" >&2; exit 1; }

stage "assert the flood was reported once, not once per refusal"
# Reporting each refusal would put the flood into the table that exists to
# account for shares — hundreds of rows describing one condition. The
# report is periodic, plus a final flush when the connection goes, so a
# 500-refusal burst is a couple of rows.
RL_ROWS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM rejects WHERE reason LIKE 'submitting too fast%'")
echo "  rate-limit reject rows=$RL_ROWS"
[ "$RL_ROWS" -ge 1 ] || { echo "FAIL: the flood was never reported" >&2; exit 1; }
[ "$RL_ROWS" -le 5 ] || {
    echo "FAIL: $RL_ROWS rows for one flood — reporting is not aggregated" >&2; exit 1; }

stage "PASS"
