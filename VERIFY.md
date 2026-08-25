# `pps-thunder` — verification checklist

Step-by-step checks to confirm each landed piece behaves as advertised.
Tick boxes as you go; each section is independent and you can skip
around. Expected output is shown literally; deviation = something to
investigate.

If a step fails, the section header points at the commit that owns the
behaviour, so `git show <hash>` is a quick way to inspect.

---

## 0 · Prerequisites (one-time)

- [ ] macOS arm64 (the prebuilt regtest binaries are aarch64-darwin).
      On Linux/x86 some sections still work (unit tests, payout worker,
      audit, Redis broadcast) — only the regtest stack needs the
      aarch64 binaries.
- [ ] `brew install hiredis sqlite curl` (hiredis for the C build)
- [ ] `node --version` ≥ 20

---

## 1 · Unit tests pass — `make test`

Owns: every src commit. Quickest end-to-end sanity check.

```
make test
```

- [ ] Last lines of output show:
      ```
      test_coinbase: all tests passed
      test_broadcast: ok
      test_thunder: ok
      ```
- [ ] Each suite reports zero failures.
- [ ] `coinbase` suite shows **8** assertions including:
      ```
      ok: coinbase split fee math
      ok: coinbase_build_from_template (redirect + preserve commitments)
      ok: coinbase_build_from_template fee split
      ```
- [ ] No spurious WARN lines from `test_broadcast` (silenced in commit
      `76f8753`).

---

## 2 · Solo mode unchanged — regression check

Owns: every src commit (must not break the existing behaviour).

In a fresh terminal:

```
cp proxy.conf.example /tmp/solo.conf
# edit /tmp/solo.conf: set bitcoind_url/operator_address to anything;
# leave pool_mode = solo (the default) and redis_url empty.
./build/simplepool /tmp/solo.conf
```

- [ ] Logs read `pool_mode` is solo by default (no `pool_mode=pps-classic`
      line appears).
- [ ] Process binds `:3334` and waits for bitcoind. Same behaviour as
      pre-branch `main`.
- [ ] Kill it. Reload `main`, compare: solo behaviour is identical.

---

## 3 · Redis broadcast — `b1f3537`

In one terminal:

```
redis-server --daemonize yes      # or `brew services start redis`
redis-cli ping                    # → PONG
```

Edit `/tmp/solo.conf` to set:
```
redis_url = redis://127.0.0.1:6379
```

Restart simplepool. In another terminal:
```
redis-cli PSUBSCRIBE 'pool:*'
```

- [ ] simplepool startup log says
      `broadcast: connected to redis 127.0.0.1:6379/0`.
- [ ] On bitcoind tip changes (or initial GBT) you see
      `pool:tip` messages with `height`, `hash`, `observed_at_s`.
- [ ] Connect a miner; accepted shares appear on `pool:shares` as JSON
      with `worker`, `payout_address`, `ts_ms`, `difficulty`, `is_block`.
- [ ] Stop redis (`brew services stop redis`); simplepool keeps running,
      logs `redis connect ... failed`, share processing is unaffected,
      `dropped_redis_down` increments in shutdown stats.

---

## 4 · Thunder address decoder — `f29ea16`

```
./build/test_thunder
```

- [ ] Prints `test_thunder: ok`. (Covers bare base58 + the
      `s9_<b58>_<hex6>` deposit-format wrapper + garbage rejection.)

---

## 5 · Classic coinbase byte layout — `f29ea16` + `571dad4`

The byte-level assertions live in `tests/test_coinbase.c`. Both the bare
builder (`coinbase_build_split`) and the template rewriter
(`coinbase_build_from_template`) are covered. To *see* a real
pps-classic coinbase end-to-end, do section 11.

- [ ] `make test` step 1 passed; the classic layout is asserted at the
      byte level (spendable output, then operator fee, then witness
      commitment, with any backend commitment outputs preserved in
      place).
- [ ] No `drivechain` assertions remain — `pool_mode = pps` and its
      `coinbase_build_drivechain*` builders were removed.

---

## 6 · PPS mode wiring (config + schema) — `8783c0b`

Without running the full regtest stack, you can still verify the
config keys and the schema:

```
sqlite3 /tmp/pps-test.db < schema.sql
sqlite3 /tmp/pps-test.db ".schema pps_credits"
sqlite3 /tmp/pps-test.db ".schema payouts_in_flight"
```

- [ ] Both tables exist and match the columns documented in
      [CLASSIC_PAYOUTS.md](CLASSIC_PAYOUTS.md).
- [ ] Editing `proxy.conf` to set `pool_mode = pps-classic` without
      `pool_btc_address` makes simplepool refuse to start with
      `config: 'pool_btc_address' is required when pool_mode=pps-classic`.
- [ ] Setting `pool_mode = pps` makes simplepool refuse to start with
      `config: 'pool_mode = pps' was removed …`.
- [ ] A leftover `pool_thunder_reserve_address` / `thunder_sidechain_number`
      / `thunder_op_return_hex` line logs `is obsolete and ignored` and
      does **not** stop startup.

---

## 7 · Payout worker happy path (mock Thunder) — `a05a96f`

```
cd payout && npm install
```

Set up a synthetic DB:

```
mkdir -p /tmp/payout-test && cd /tmp/payout-test
sqlite3 shares.db < /Users/rob/projects/simplepool/schema.sql
sqlite3 shares.db "
  INSERT INTO workers (name, first_seen, last_seen, payout_address) VALUES
    ('alice', 1000, 2000, '11111111111111111111');
  INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated)
    VALUES (1, 50000, 0, 1000);
"
```

Dry-run:

```
PAYOUT_DRY_RUN=1 \
PAYOUT_DB_PATH=./shares.db \
THUNDER_RPC_URL=http://127.0.0.1:6009 \
THUNDER_FROM_ADDRESS=any \
PAYOUT_MIN_SATS=10000 \
PAYOUT_INTERVAL_MS=2000 \
PAYOUT_SETTLE_INTERVAL_MS=2000 \
node /Users/rob/projects/simplepool/payout/index.js
```

- [ ] Prints `payout: DRY alice -> 11111111111111111111 50000 sats`.
- [ ] No DB writes (verify with `sqlite3 shares.db "SELECT * FROM pps_credits"`
      — `paid_sats` still 0).
- [ ] Ctrl-C exits cleanly.

---

## 8 · Payout worker at-most-once protocol — `e0b849e`

Start a tiny mock Thunder RPC in one terminal:

```
cat > /tmp/payout-test/mock-thunder.mjs <<'EOF'
import http from 'node:http';
let n = 0;
http.createServer((req, res) => {
    let body = ''; req.on('data', c => body += c);
    req.on('end', () => {
        const r = JSON.parse(body);
        let result;
        if      (r.method === 'balance')  result = { available_sats: 1_00000000 };
        else if (r.method === 'transfer') result = `txidfake${++n}`;
        else result = null;
        res.writeHead(200, {'content-type': 'application/json'});
        res.end(JSON.stringify({jsonrpc:'2.0', id:r.id, result}));
    });
}).listen(16009, () => console.log('mock listening on :16009'));
EOF
node /tmp/payout-test/mock-thunder.mjs &
```

Run the worker against it (no dry-run):

```
cd /tmp/payout-test
PAYOUT_DB_PATH=./shares.db \
THUNDER_RPC_URL=http://127.0.0.1:16009 \
THUNDER_FROM_ADDRESS=any \
PAYOUT_MIN_SATS=10000 \
PAYOUT_INTERVAL_MS=2000 \
PAYOUT_SETTLE_INTERVAL_MS=2000 \
node /Users/rob/projects/simplepool/payout/index.js
```

After ~3 seconds, Ctrl-C. Then:

```
sqlite3 shares.db "SELECT * FROM pps_credits;"
sqlite3 shares.db "SELECT * FROM payouts_in_flight;"
```

- [ ] `pps_credits` now shows `paid_sats = 50000` (the 50k owed was
      paid).
- [ ] `payouts_in_flight` is **empty** (atomic finalize cleaned up).

Now simulate a crash: pre-populate an in-flight row before starting:

```
sqlite3 shares.db "
  UPDATE pps_credits SET accrued_sats = 80000, paid_sats = 0;
  INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
    VALUES (1, 80000, 'crashedTxid', 100);
"
node /Users/rob/projects/simplepool/payout/index.js
```

- [ ] On startup the worker logs
      `payout: 1 stuck in-flight row(s) (>300s old)` with the txid.
- [ ] `listDue` skips alice; she is NOT paid again (no double-pay).
- [ ] Ctrl-C. `sqlite3 shares.db "SELECT * FROM pps_credits;"` still
      shows `paid_sats = 0`.

---

## 9 · Block-withholding audit — `1ebcea1`

Synthetic dataset proving the audit catches a withholder:

```
cd /tmp/payout-test
sqlite3 shares.db "DELETE FROM shares; DELETE FROM workers;
INSERT INTO workers (id, name, first_seen, last_seen, payout_address) VALUES
  (10, 'alice.honest', 1000, 2000, 'thAlice'),
  (11, 'bob.honest',   1000, 2000, 'thBob'),
  (12, 'eve.withhold', 1000, 2000, 'thEve');"
python3 - <<'PY'
import sqlite3, time
db = sqlite3.connect('shares.db')
now = int(time.time())
def shares(wid, n, n_blocks):
    rows = [(wid, now - 3600 + i, 1.0,
             1 if i < n_blocks else 0,
             f'{wid:02x}{i:08x}'.ljust(64,'0') if i < n_blocks else None)
            for i in range(n)]
    db.executemany('INSERT INTO shares (worker_id,ts,difficulty,is_block,block_hash) VALUES (?,?,?,?,?)', rows)
shares(10, 3000, 30)
shares(11, 3000, 28)
shares(12, 3000, 0)
db.commit(); db.close()
PY

PAYOUT_DB_PATH=./shares.db node /Users/rob/projects/simplepool/payout/audit.js
```

- [ ] Output begins with `!! 1 SUSPICIOUS worker(s)` flagging
      `eve.withhold`.
- [ ] eve's row shows `z ≈ 4.4`, `actual=0`, `expected ≈ 19`.
- [ ] alice and bob have z ≤ 0 (slight over-performance, normal).
- [ ] `--json` mode emits a parseable object with `suspicious: true`
      only for eve.

---

## 10 · BIP300 regtest stack (infra) — `b0da16f` + `6037a25`

```
scripts/regtest/setup.sh       # ~90 MB of prebuilt binaries incl. Thunder
scripts/regtest/start.sh
scripts/regtest/status.sh
```

- [ ] `status.sh` shows all **four** processes `up`: `bitcoind`,
      `electrs`, `bip300301_enforcer`, `thunder`.
- [ ] Thunder log ends with
      `Verified existence of cusf.mainchain.v1.ValidatorService` and
      `starting RPC server at 127.0.0.1:6009`.
- [ ] `curl -sS -u user:password -H 'content-type: application/json' \
      --data '{"jsonrpc":"1.0","id":1,"method":"getblockcount","params":[]}' \
      http://127.0.0.1:18443/` returns a number.
- [ ] `curl -sS -H 'content-type: application/json' \
      --data '{"jsonrpc":"2.0","id":1,"method":"balance","params":[]}' \
      http://127.0.0.1:6009/` returns
      `{"result":{"total_sats":0,"available_sats":0}}` — Thunder RPC
      is reachable (matches what the payout worker expects).

Activate the sidechain, then set up Thunder's wallet:

```
scripts/regtest/activate-thunder.sh
scripts/regtest/thunder-init.sh
```

- [ ] `activate-thunder.sh` first run prints `sidechain 9 is now
      ACTIVE`; second run is a no-op.
- [ ] `thunder-init.sh` prints a fresh mnemonic + wallet address +
      formatted deposit string (`s9_<base58>_<hex6>`); second run
      short-circuits with `already initialised`.

Issue a canonical deposit to the Thunder-owned address it printed,
then observe the enforcer's TwoWayPeg event stream:

```
DEP='<paste the deposit format from thunder-init.sh>'
scripts/enforcer-rpc.sh cusf.mainchain.v1.WalletService/CreateDepositTransaction \
  "{\"sidechain_id\":9,\"address\":\"$DEP\",\"value_sats\":100000000,\"fee_sats\":1000}"
scripts/enforcer-rpc.sh cusf.mainchain.v1.MiningService/GenerateToAddress \
  '{"blocks":1, "address":"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"}'
scripts/enforcer-rpc.sh cusf.mainchain.v1.ValidatorService/GetCtip \
  '{"sidechain_number":9}'
```

- [ ] Ctip returns non-empty with `value: "100000000"` (or the running
      total if earlier deposits happened).

Note: Thunder's own balance staying at 0 after a canonical deposit is
expected until Thunder produces a sidechain block via BMM. BMM is a
Thunder-side concern (thunder-cli's `mine` command blocks on
mainchain coordination) — outside the scope of the pool. The
enforcer-side Ctip update is the authoritative signal that the
deposit was consensus-accepted.

---

## 11 · End-to-end PPS regtest run — `fedbb8e`

This is the loop-closer: pool emits a pps-classic coinbase, the miner
finds a block, bitcoind-patched accepts it. `tests/test_e2e_regtest.sh`
automates this whole section for CI.

After section 10 the stack is up. Run `scripts/regtest/validate.sh` —
it bootstraps 150 blocks, then prints a `proxy.conf` snippet. Save it:

```
cat > /tmp/regtest-proxy.conf <<'EOF'
listen_addr = 127.0.0.1
listen_port = 13334
bitcoind_url = http://127.0.0.1:18444
operator_address = bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080
fee_bps = 100
coinbase_tag = /simplepool-regtest/
pool_mode = pps-classic
pool_btc_address = bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt
pps_sats_per_diff = 1000
initial_diff = 0.0000001
vardiff_enabled = 0
db_path = /tmp/regtest-shares.db
EOF
sqlite3 /tmp/regtest-shares.db < schema.sql
```

In terminal A:
```
./build/simplepool /tmp/regtest-proxy.conf
```

- [ ] Log shows `pool_mode=pps-classic: pool_btc_address=bcrt1qqypq…,
      pps_sats_per_diff=1000.00`.
- [ ] Log shows `stratum listening on 127.0.0.1:13334`.

In terminal B:
```
node scripts/regtest/cpuminer.js --timeout 60
```

- [ ] Miner connects, subscribes, authorizes with the default 20-base58
      Thunder address.
- [ ] After many "low difficulty" rejects (share target is still smaller
      than network target), a `submit -> true` and
      `block found and accepted ✓` appear within a few seconds.
- [ ] Terminal A simplepool log shows `BLOCK FOUND: height=N
      finder=11111111111111111111 reward=4950000000 fee=50000000`
      followed by `submitted block to bitcoind successfully`.

Inspect the coinbase:

```
POOL_BTC_ADDRESS=bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt \
OPERATOR_ADDRESS=bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080 \
    scripts/regtest/inspect-coinbase.sh
```

- [ ] One output pays the pool wallet 49.5 BTC (`>>> pool output found`).
- [ ] One output pays the operator fee P2WPKH 0.5 BTC
      (`>>> operator fee output found`), and it is the smaller of the two.
- [ ] No `OP_DRIVECHAIN` output is reported — the script exits non-zero
      if one shows up.
- [ ] Script prints `==> classic coinbase shape OK`.

---

## 12 · The coinbase-deposit Ctip finding — `fedbb8e`

The critical empirical result. With section 11 finished:

```
scripts/enforcer-rpc.sh cusf.mainchain.v1.ValidatorService/GetCtip \
  '{"sidechain_number":9}'
```

- [ ] Returns `{}`. **The coinbase deposit did NOT credit the Ctip**,
      even though the block was accepted into the chain.

Now issue a canonical deposit to confirm the rule difference:

```
scripts/enforcer-rpc.sh cusf.mainchain.v1.WalletService/CreateDepositTransaction \
  '{"sidechain_id":9, "address":"11111111111111111111",
    "value_sats":100000000, "fee_sats":1000}'

scripts/enforcer-rpc.sh cusf.mainchain.v1.MiningService/GenerateToAddress \
  '{"blocks":1, "address":"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"}'

scripts/enforcer-rpc.sh cusf.mainchain.v1.ValidatorService/GetCtip \
  '{"sidechain_number":9}'
```

- [ ] Now returns `{"ctip": {"txid": {...}, "value": "100000000"}}`.
- [ ] Side-by-side: same enforcer, same active sidechain, only the
      deposit source differs. Coinbase-as-deposit doesn't work; deposit
      tx from spendable UTXOs does.

This is the architectural finding that killed `pool_mode = pps` and
drove the operator-triggered deposit design in
[CLASSIC_PAYOUTS.md](CLASSIC_PAYOUTS.md).

---

## 13 · Teardown

```
scripts/regtest/stop.sh
brew services stop redis 2>/dev/null
rm -rf /tmp/payout-test /tmp/regtest-proxy.conf /tmp/regtest-shares.db /tmp/solo.conf /tmp/pps-test.db
```

- [ ] No leftover processes (`pgrep -f bip300301_enforcer` returns
      nothing).
- [ ] `.regtest/` directory still present (gitignored; safe to keep
      for repeat runs).

---

## Wrap-up

If every box ticks:
- the PPS plumbing is correct end-to-end at the byte level,
- the payout worker has correct at-most-once semantics,
- the audit catches synthetic withholders,
- the regtest stack reproduces the architectural finding that coinbase
  deposits aren't Ctip-credited by the current LayerTwo-Labs enforcer.

The two-step deposit that finding forced is the admin dashboard's
"Deposit to Thunder" action — see
[CLASSIC_PAYOUTS.md](CLASSIC_PAYOUTS.md).
