#!/usr/bin/env bash
# Record a mainchain-to-Thunder deposit in the pool's deposits ledger so
# it shows up in the admin dashboard.
#
# The enforcer's CreateDepositTransaction RPC returns a mainchain txid;
# after Ctip confirms the deposit, invoke this script with what you know
# and it inserts a row into `deposits`.
#
# Usage:
#   scripts/log-deposit.sh \
#     --db /home/forknet/pps-thunder-test/data/shares.db \
#     --txid <mainchain btc txid> \
#     --sats <sats_deposited> \
#     --fee <fee_sats> \
#     --recipient <bare Thunder base58 address> \
#     --ctip-before <n> --ctip-after <n> \
#     [--note "reason / notes"]

set -euo pipefail
DB=""
TXID=""
SATS=""
FEE=""
RECIP=""
BEFORE=""
AFTER=""
NOTE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --db)          DB="$2"; shift 2 ;;
        --txid)        TXID="$2"; shift 2 ;;
        --sats)        SATS="$2"; shift 2 ;;
        --fee)         FEE="$2"; shift 2 ;;
        --recipient)   RECIP="$2"; shift 2 ;;
        --ctip-before) BEFORE="$2"; shift 2 ;;
        --ctip-after)  AFTER="$2"; shift 2 ;;
        --note)        NOTE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

for req in DB TXID SATS FEE RECIP; do
    if [[ -z "${!req}" ]]; then
        echo "missing required flag: --$(echo $req | tr A-Z a-z)" >&2
        exit 2
    fi
done

if [[ "$RECIP" == *_* ]]; then
    echo "recipient '$RECIP' contains '_' — looks like a deposit-format wrapper;" >&2
    echo "use the bare base58 (thunder-cli get-new-address). Aborting." >&2
    exit 3
fi

# Every value below is spliced into SQL text, so each one is checked before
# it gets there: a note like "pool's top-up" used to break the INSERT, and a
# crafted --txid or --sats could close the VALUES list and write arbitrary
# rows into the ledger the admin dashboard audits against. Numbers and
# identifiers have a fixed shape and are matched against it, so nothing
# outside the pattern reaches the SQL at all; the note is the one free-text
# field and is quoted the SQL way, by doubling every '. (sqlite3's
# `.parameter set` would not help: it parses its value as SQL too.)
bad() { echo "$1" >&2; exit 2; }
[[ "$TXID" =~ ^[0-9a-fA-F]{64}$ ]] \
    || bad "--txid must be a 64-character hex txid, got '$TXID'"
# Bare base58 — the alphabet dashboard/lib/thunder-address.js decodes (no
# 0, O, I or l). Anything else cannot be a Thunder address.
[[ "$RECIP" =~ ^[1-9A-HJ-NP-Za-km-z]+$ ]] \
    || bad "--recipient must be a bare base58 Thunder address, got '$RECIP'"
# 18 digits stays inside SQLite's signed 64-bit INTEGER.
for n in SATS FEE BEFORE AFTER; do
    [[ -z "${!n}" || "${!n}" =~ ^[0-9]{1,18}$ ]] \
        || bad "--$(echo "$n" | tr '[:upper:]' '[:lower:]') must be a non-negative integer, got '${!n}'"
done
sql_int()  { if [[ -n "$1" ]]; then echo "$1"; else echo NULL; fi; }
sql_text() { local q="'"; if [[ -n "$1" ]]; then echo "'${1//$q/$q$q}'"; else echo NULL; fi; }

NOW=$(date +%s)
sqlite3 "$DB" <<SQL
INSERT INTO deposits
  (ts, btc_txid, sats_deposited, fee_sats, thunder_recipient,
   ctip_seq_before, ctip_seq_after, notes)
VALUES
  ($NOW, '$TXID', $SATS, $FEE, '$RECIP',
   $(sql_int "$BEFORE"),
   $(sql_int "$AFTER"),
   $(sql_text "$NOTE"));
SQL

echo "logged deposit: $SATS sats to $RECIP (txid $TXID)"
