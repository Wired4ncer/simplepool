#!/usr/bin/env bash
# Validate the pps-classic coinbase shape end-to-end:
#
#   1. Bootstrap the regtest L1: generate ~150 blocks to a stock P2WPKH
#      address so we have spendable coins and a sane chain height.
#   2. Activate sidechain #9 (Thunder) via the enforcer's BIP300
#      propose/ack flow. The coinbase no longer carries a deposit, but an
#      active sidechain means the GBT template still carries the BIP301
#      commitment outputs the coinbase builder must preserve.
#   3. Configure simplepool in pool_mode=pps-classic, pointed at the
#      enforcer's getblocktemplate endpoint (127.0.0.1:18444).
#   4. Connect a stratum miner (here: cpuminer-style via Python) for a
#      few seconds — we just need it to mine ONE block. Regtest difficulty
#      is trivially low.
#   5. Read the new tip's coinbase tx and assert:
#        - the spendable output pays pool_btc_address
#        - the operator BTC fee output is present at fee_bps
#        - no OP_DRIVECHAIN output is emitted (pool_mode=pps is gone)
#
# Moving the accumulated BTC into Thunder is a separate, operator-driven
# step — see CLASSIC_PAYOUTS.md.
#
# This script is intentionally a "guided runbook" — each step prints
# clearly so a human can read the output and intervene.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="${REGTEST_DIR:-$ROOT/.regtest}"
BIN="${REGTEST_BIN_DIR:-$REGTEST/bin}"
DATA="$REGTEST/data"

cli() { "$BIN/bitcoin-cli" -datadir="$DATA/bitcoind" -regtest -rpcuser=user -rpcpassword=password "$@"; }

echo "==> sanity check stack"
"$ROOT/scripts/regtest/status.sh"

echo ""
echo "==> activate sidechain #9 (Thunder)"
"$ROOT/scripts/regtest/activate-thunder.sh"

echo ""
echo "==> bootstrap chain (mine 150 to miner wallet so coinbase matures)"
ADDR="$(cli -rpcwallet=miner getnewaddress '' bech32)"
echo "  miner address: $ADDR"
# Separate wallet address for the pool's pps-classic coinbase output, so
# the runbook's inspect-coinbase.sh can tell it apart from the fee output.
POOL_BTC_ADDR="$(cli -rpcwallet=miner getnewaddress '' bech32)"
echo "  pool BTC address: $POOL_BTC_ADDR"
cli generatetoaddress 150 "$ADDR" > /dev/null
echo "  height now: $(cli getblockcount)"

echo ""
echo "==> probe enforcer GBT (should include drivechain commitments)"
GBT="$(curl -sS -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":1,"method":"getblocktemplate","params":[{"rules":["segwit"]}]}' \
    http://127.0.0.1:18444/)"
echo "$GBT" | python3 -c "
import json, sys
r = json.loads(sys.stdin.read())['result']
print(f'  height={r.get(\"height\")} coinbasevalue={r.get(\"coinbasevalue\")}')
print(f'  has coinbasetxn: {bool(r.get(\"coinbasetxn\"))}')
cb = r.get('coinbasetxn')
if cb:
    print(f'  coinbasetxn hex bytes={len(cb.get(\"data\", \"\"))//2}')
"

echo ""
echo "==> next steps (manual, since CPU mining a bech32 work is fiddly):"
echo ""
echo "  1. In another terminal, run simplepool in pps-classic mode against the enforcer:"
echo ""
echo "       cat > /tmp/regtest-proxy.conf <<EOF"
echo "       listen_addr = 127.0.0.1"
echo "       listen_port = 13334"
echo "       bitcoind_url = http://127.0.0.1:18444"
echo "       operator_address = $ADDR"
echo "       fee_bps = 100"
echo "       pool_mode = pps-classic"
echo "       pool_btc_address = $POOL_BTC_ADDR"
echo "       pps_sats_per_diff = 1000"
echo "       # No listener line: no port promises a floor, so the"
echo "       # network-difficulty clamp applies and CPU mining stays possible."
echo "       initial_diff = 0.0000001"
echo "       vardiff_enabled = 0"
echo "       db_path = /tmp/regtest-shares.db"
echo "       EOF"
echo "       mkdir -p /tmp/regtest-data"
echo "       ./build/simplepool /tmp/regtest-proxy.conf"
echo ""
echo "  2. Point any stratum miner at 127.0.0.1:13334 with username ="
echo "     a valid Thunder address (any 20-byte hash base58-encoded)."
echo "     The block reward lands in pool_btc_address; the miner accrues"
echo "     pps_credits, paid out of Thunder separately."
echo ""
echo "  3. After a block is mined, run:"
echo "       POOL_BTC_ADDRESS=$POOL_BTC_ADDR OPERATOR_ADDRESS=$ADDR \\"
echo "           scripts/regtest/inspect-coinbase.sh"
echo ""
echo "     to assert the classic output layout (pool wallet + operator"
echo "     fee, no OP_DRIVECHAIN)."
