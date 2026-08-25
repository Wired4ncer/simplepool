# Installation guide

Getting simplepool from `git clone` to accepting real shares. Two
tracks:

- **Local dev** — build the pool on macOS or Linux, run the tests,
  drive a full regtest stack from `scripts/regtest/`. This is the
  fastest way to touch every code path.
- **Production** — deploy on a fresh Ubuntu server behind a
  drivechain-patched Bitcoin node, an enforcer, and (optionally) a
  Thunder node. There's a one-shot deploy script; this doc also
  walks through what it does step by step so you can do it by hand.

The doc is mode-agnostic where possible; where mode matters, the two
possibilities are called out clearly:

- **`pool_mode = solo`**    — miners paid direct in the coinbase.
  Simplest. No drivechain, no Thunder, no PPS accrual.
- **`pool_mode = pps-classic`** — traditional coinbase paying a pool
  BTC address, operator-driven Thunder deposits from the admin
  dashboard. This is the mode you want for a Thunder-paying PPS pool.
  See [CLASSIC_PAYOUTS.md](CLASSIC_PAYOUTS.md).

(A third mode, `pool_mode = pps`, put the drivechain deposit directly in
the coinbase. The enforcer never credited it, so it has been removed —
`CLASSIC_PAYOUTS.md` has the evidence.)

For an audit / theory refresher on nonce distribution and share math,
see [NONCE_AND_SHARES.md](NONCE_AND_SHARES.md). For day-to-day
operations after everything is up, see [OPERATOR_GUIDE.md](OPERATOR_GUIDE.md).

---

## Overview of the stack

```
     miner ASIC
        │  stratum+tcp
        ▼
   ┌──────────┐
   │simplepool│──── SQLite ──────── dashboard  (public + /admin)
   │  :3334   │       │
   └────┬─────┘       └──── payout worker ─┐
        │  GBT / submitblock                │
        │                                   │ Thunder JSON-RPC
        ▼                                   ▼
   ┌──────────────┐                    ┌──────────┐
   │  enforcer    │ ── gRPC :50051 ──▶ │ Thunder  │
   │  (:8122 GBT) │                    │  node    │
   └──────┬───────┘                    └──────────┘
          │
          │ JSON-RPC :8332, ZMQ :29000
          ▼
   ┌───────────────────┐         ┌──────────┐
   │ bitcoind-patched  │◀────────│ electrs  │
   └───────────────────┘         └──────────┘
```

For **solo mode**, everything below the enforcer is optional — you
can point simplepool directly at a stock bitcoind. For **pps-classic**,
Thunder is only strictly required at payout time (the coinbase itself
just pays a BTC address). For **pps**, Thunder is nominally required
but the deposit flow doesn't work end-to-end anyway.

---

## Prerequisites

### Common tools

- `git`, `make`, a C11 compiler (`gcc` or `clang`)
- `sqlite3` CLI + headers
- `libcurl` headers
- `libhiredis` headers (Redis client — required by the broadcast module,
  even if you don't enable Redis broadcast)
- `node` v20+ (for the dashboard and payout worker)

### macOS (Homebrew)

```sh
brew install sqlite curl hiredis node
xcode-select --install    # if `cc` isn't available
```

### Ubuntu 24.04

```sh
sudo apt update
sudo apt install -y build-essential libsqlite3-dev libcurl4-openssl-dev \
    libhiredis-dev sqlite3 curl unzip
# Node from NodeSource so you get a modern version
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo bash -
sudo apt install -y nodejs
```

---

## Part A — local dev (macOS or Linux)

### 1. Clone and build

```sh
git clone https://github.com/LayerTwo-Labs/simplepool.git
cd simplepool
make
```

Binary lands at `build/simplepool`. Should build clean with
`-Werror` on both macOS and Ubuntu.

### 2. Run the tests

```sh
make test
```

Seven suites: `test_share`, `test_bitcoind`, `test_stratum`,
`test_store`, `test_coinbase`, `test_broadcast`, `test_thunder`.
All must pass — they cover the coinbase byte layout, stratum protocol
handling, share hashing, and the Thunder address decoder.

### 3. Regtest stack (optional but recommended)

If you want to drive the whole drivechain stack locally and see a real
block get mined by simplepool, `scripts/regtest/` has everything:

```sh
scripts/regtest/setup.sh         # downloads bitcoind-patched, electrs,
                                 # bip300301_enforcer, Thunder prebuilts
scripts/regtest/start.sh         # brings up all four processes
scripts/regtest/status.sh        # ps-style summary
scripts/regtest/activate-thunder.sh  # proposes + acks sidechain #9
scripts/regtest/thunder-init.sh      # generates a Thunder wallet + address
scripts/regtest/validate.sh          # bootstraps 150 blocks, probes GBT
node scripts/regtest/cpuminer.js --timeout 60  # actually mine a block
POOL_BTC_ADDRESS=<pool addr> scripts/regtest/inspect-coinbase.sh
scripts/regtest/stop.sh
```

Full details in [scripts/regtest/README.md](scripts/regtest/README.md).
Everything runs under `.regtest/` (gitignored) — safe to re-run without
touching your system.

The regtest stack matches the production stack process-for-process
(same enforcer, same electrs, same Thunder), so if your changes work
against it, they'll work against forknet or mainnet-drivechain.

---

## Part B — production install on Ubuntu

Three paths, in the order most people want them:

1. **The installer** — run one line on the box. Interviews you, then does
   everything below. This is Part B.1.
2. **`deploy-to-server.sh`** — drive an already-installed box from your
   workstation. For iterating on unreleased code. Part B.2.
3. **The manual walkthrough** — every step by hand, for integrating into an
   existing setup. Part B.3.

### B.1 — the installer (fresh Ubuntu / Debian box)

```sh
curl -fsSL https://raw.githubusercontent.com/LayerTwo-Labs/simplepool/main/scripts/install.sh | sudo bash
```

It asks where to install, which pool mode, your bitcoind RPC and addresses,
the dashboard domain and admin password, and whether to set up nginx, TLS and
the firewall — then does the whole install and prints what miners should
connect to. Every answer is saved to `/etc/simplepool/install.env` and reused
as the default next time, so **re-running it is how you change your mind**
about any of them.

#### Where the code comes from

The first question is the one worth understanding:

| | `release` (default) | `source` |
| --- | --- | --- |
| how | downloads the published tarball for this machine's architecture and verifies it against the release `SHA256SUMS` | `git clone` + `make` |
| needs | curl, the runtime shared libraries | a C toolchain, git, and a few minutes |
| gives you | exactly what CI built and tagged | any branch, any architecture |
| upgrade | `simplepoolctl upgrade` | re-run with `--from-source` |

Both end with the same tree at `$ROOT` — the release tarball *is* a checkout
with a prebuilt `build/simplepool` and a `RELEASE` file in it. Everything
after that step (config, database, systemd, nginx) is one code path, so a
release box and a source box differ only in where the binary came from.

Released binaries are built on Ubuntu 22.04, so they need **glibc 2.35 or
newer** — Ubuntu 22.04+, Debian 12+. On anything older, or on an
architecture with no published build, use `--from-source`.

#### Non-interactive

Every prompt has a flag, so the same script drives CI and re-runs:

```sh
sudo ./scripts/install.sh --non-interactive --yes \
     --from-release \
     --root /home/simplepool --user simplepool \
     --mode pps-classic \
     --operator-address bc1q... --pool-btc-address bc1q... \
     --thunder-address <base58> \
     --bitcoind-url http://127.0.0.1:8332 \
     --bitcoind-user rpcuser --bitcoind-pass rpcpass \
     --hostname pool.example.com --tls --email you@example.com \
     --payout-interval-hours 24
```

`sudo ./scripts/install.sh --help` lists them all.

#### Afterwards: `simplepoolctl`

The installer drops `simplepoolctl` into `/usr/local/bin`. It reads the same
`/etc/simplepool/install.env`, so it needs no configuration of its own:

```sh
simplepoolctl status            # services, ports, versions, ledger totals
simplepoolctl doctor            # binary runs? bitcoind reachable? DB writable?
simplepoolctl logs payout -f    # one service, or 'all' (the default)
simplepoolctl config            # where every config file is, and what's in it
sudo simplepoolctl restart proxy
sudo simplepoolctl upgrade      # next release (or rebuild, on a source box)
sudo simplepoolctl uninstall    # --purge also deletes the ledger
```

`doctor` is the one to run when something is wrong: it checks the binary
actually executes on this machine, that `operator_address` (and
`pool_btc_address` in pps-classic) are set, that the schema is loaded and the
data directory is writable by the service user, that bitcoind answers
`getblockchaininfo` with the configured credentials, and that something is
listening on the stratum port.

Once it is up, jump to **Part D** to review `proxy.conf` for your mode — the
installer has already written the keys it asked about.

### B.2 — deploy-to-server.sh (from your workstation)

`scripts/deploy-to-server.sh` handles installing deps, cloning,
building, initializing SQLite, dropping systemd units, and setting up
nginx. From your local machine:

```sh
./scripts/deploy-to-server.sh \
    --host     user@pool.example.com \
    --root     /home/user/simplepool \
    --hostname pool.example.com \
    --ssh-key  ~/.ssh/your_key
```

What it does (idempotent — re-run after every code change):

1. `git fetch && git reset --hard origin/main` on the remote checkout.
2. `apt-get install` build deps + Node.js + sqlite3 + nginx + ufw.
3. `make` the C proxy.
4. `npm install` in `dashboard/` and `payout/`.
5. Initialise `data/shares.db` from `schema.sql` if missing.
6. Render the systemd unit templates in `deploy/systemd/` with the
   right `USER` / `ROOT` substitutions, install to
   `/etc/systemd/system/`, `enable --now` each.
7. Drop the nginx vhost from `deploy/nginx/` into `sites-available`
   and enable it, open ports 80 / 443 / 3334 via `ufw` if active —
   plus any extra stratum ports declared with `listener` lines in
   `proxy.conf`. The installer reads those back from the finished
   config and opens them too; if you add one later, open it yourself.

After that runs cleanly, jump to **Part D** (configuring `proxy.conf`
for your chosen mode) — everything else is already up.

### B.3 — manual walkthrough

Skip if you ran the installer or the deploy script. Otherwise:

1. **Create a service user + workdir**:
   ```sh
   sudo adduser --system --group --home /home/simplepool --shell /bin/bash simplepool
   sudo mkdir -p /home/simplepool/data
   sudo chown -R simplepool:simplepool /home/simplepool
   ```
2. **Clone and build** under that user:
   ```sh
   sudo -u simplepool -H bash -lc '
     cd /home/simplepool &&
     git clone https://github.com/LayerTwo-Labs/simplepool.git . &&
     make -j$(nproc)
   '
   ```
3. **Install Node deps** for the dashboard and payout worker:
   ```sh
   sudo -u simplepool -H bash -lc '
     cd /home/simplepool/dashboard && npm install --no-fund --no-audit
     cd /home/simplepool/payout    && npm install --no-fund --no-audit
   '
   ```
4. **Initialise the database**:
   ```sh
   sudo -u simplepool sqlite3 /home/simplepool/data/shares.db \
       < /home/simplepool/schema.sql
   ```
5. **Install systemd units**. Templates in `deploy/systemd/` use
   `@USER@` and `@ROOT@` placeholders — substitute before dropping in:
   ```sh
   for u in simplepool.service simplepool-dashboard.service simplepool-payout.service; do
     sudo sed -e "s|@USER@|simplepool|g" -e "s|@ROOT@|/home/simplepool|g" \
         /home/simplepool/deploy/systemd/$u \
         | sudo tee /etc/systemd/system/$u >/dev/null
   done
   sudo systemctl daemon-reload
   ```

Do not `enable --now` yet — you need `proxy.conf` first (Part D).

---

## Part C — the drivechain stack

If you're running `pool_mode=solo` against a stock bitcoind, skip
this part.

### bitcoind-patched (BIP300/301-aware Bitcoin Core fork)

Prebuilt binaries:

- arm64 macOS: <https://releases.drivechain.info/L1-bitcoin-patched-v30.2-aarch64-apple-darwin.zip>
- x86_64 macOS: <https://releases.drivechain.info/L1-bitcoin-patched-latest-x86_64-apple-darwin.zip>
- Linux x86_64: check the releases index at <https://releases.drivechain.info/>

Config skeleton (`~/.drivechain-forknet/bitcoin.conf` or wherever):

```
server=1
listen=1
txindex=1
rest=1
fallbackfee=0.0001
[main-or-signet-or-regtest-or-forknet]    # pick one
rpcuser=<generated>
rpcpassword=<generated>
rpcport=8332
zmqpubrawblock=tcp://127.0.0.1:29000
zmqpubsequence=tcp://127.0.0.1:29000
```

**`rest=1` is required by the enforcer** — it queries via the REST
interface at boot. If you forget, the enforcer will refuse to start
with a clear error message.

### electrs

Prebuilt: <https://releases.drivechain.info/electrs-latest-aarch64-apple-darwin.zip>
(or the x86_64 variant). On Linux, either build from source
(<https://github.com/mempool/electrs>) or use the equivalent zip.

Launch:
```sh
electrs \
    --network <same as bitcoind> \
    --daemon-dir <bitcoind data dir> \
    --daemon-rpc-addr 127.0.0.1:8332 \
    --cookie "<rpcuser>:<rpcpassword>" \
    --db-dir /var/lib/electrs \
    --electrum-rpc-addr 127.0.0.1:50001 \
    --jsonrpc-import \
    --timestamp
```

### bip300301_enforcer

Prebuilt: <https://releases.drivechain.info/bip300301-enforcer-latest-aarch64-apple-darwin.zip>
(or x86_64). Launch:

```sh
bip300301_enforcer \
    --data-dir /var/lib/enforcer \
    --enable-wallet \
    --enable-mempool \
    --wallet-auto-create \
    --wallet-sync-source electrum \
    --wallet-electrum-host 127.0.0.1 \
    --wallet-electrum-port 50001 \
    --node-rpc-addr 127.0.0.1:8332 \
    --node-rpc-user <rpcuser> \
    --node-rpc-pass <rpcpassword> \
    --node-zmq-addr-sequence tcp://127.0.0.1:29000 \
    --serve-rpc-addr 127.0.0.1:8122 \
    --serve-json-rpc-addr 127.0.0.1:8123 \
    --serve-grpc-addr 127.0.0.1:50051
```

The enforcer's `getblocktemplate` endpoint on `:8122` is what
simplepool talks to. Port `:50051` is the gRPC surface for
sidechain management (used by the deposit runbook in
[OPERATOR_GUIDE.md](OPERATOR_GUIDE.md)).

### Thunder (needed for `pool_mode=pps-classic` payouts)

Prebuilt: <https://releases.drivechain.info/L2-S9-Thunder-latest-aarch64-apple-darwin.zip>
(no x86_64 Linux prebuilt as of this doc — build from source at
<https://github.com/LayerTwo-Labs/thunder-rust> if needed). Launch:

```sh
thunder \
    --headless \
    --datadir /var/lib/thunder \
    --network <same as your enforcer> \
    --mainchain-grpc-url http://127.0.0.1:50051 \
    --net-addr 127.0.0.1:4009 \
    --rpc-addr 127.0.0.1:6009 \
    --log-level INFO
```

Wait for the enforcer's gRPC to be up before starting Thunder — it
verifies both `ValidatorService` and `WalletService` at boot and exits
if either is unreachable. On a fresh setup, seed the wallet:

```sh
MNEMONIC=$(thunder-cli generate-mnemonic)
echo "SAVE THIS OFF-BOX: $MNEMONIC"
thunder-cli set-seed-from-mnemonic "$MNEMONIC"
```

---

## Part D — configuring `proxy.conf`

`cp proxy.conf.example proxy.conf` then edit. The example lives in the
repo root and has every key documented inline. Below are the minimum
useful configs per mode.

### Solo mode

```
listen_addr = 0.0.0.0
listen_port = 3334
bitcoind_url = http://127.0.0.1:8332
bitcoind_user = <rpcuser>
bitcoind_pass = <rpcpassword>

operator_address = bc1q...      # your BTC wallet for the 1% fee
fee_bps          = 100
coinbase_tag     = /your-pool/

pool_mode = solo                 # (default; can be omitted)

initial_diff       = 1
vardiff_enabled    = 1
vardiff_target_spm = 12
vardiff_min        = 1
vardiff_max        = 1e12
vardiff_window_sec = 30

# Rented hashrate needs its own port and its own floor. min_diff is kept even
# where the chain is easier, which costs blocks on that port — see
# proxy.conf.example.
# listener = port=3335 min_diff=65536 label=braiins

idle_timeout_sec            = 600    # socket that never authorized
idle_timeout_authorized_sec = 7200   # a working miner between shares

# Per-connection ceiling on mining.submit. Far above anything a correctly
# configured miner reaches; it bounds what one badly mismatched connection
# (a fleet on a home-miner port) can cost. 0 disables.
max_submits_per_sec = 20000

db_path = /home/simplepool/data/shares.db
log_level = info
```

Miner username: `<their BTC address>[.<rig_label>]`. Password ignored.

### PPS-classic mode

```
# ... same as solo, plus:
pool_mode = pps-classic
pool_btc_address = bc1q...       # pool wallet; ideally an enforcer-owned
                                 # address (see OPERATOR_GUIDE.md open items)
pps_sats_per_diff = 1000
```

Miner username: `<their Thunder address>[.<rig_label>]`. Startup logs
`pool_mode=pps-classic: pool_btc_address=…`.

### Optional: Redis broadcast

Add to any mode's `proxy.conf`:

```
redis_url                    = redis://127.0.0.1:6379
redis_publish_timeout_ms     = 200
redis_reconnect_backoff_ms   = 2000
```

Subscribers get JSON on channels `pool:shares`, `pool:rejects`,
`pool:blocks`, `pool:tip`, `pool:credits`.

---

## Part E — dashboard (public + admin)

The dashboard reads the same SQLite the C proxy writes. It's a single
Node process serving both the public `/` view and the admin `/admin`
view; the admin routes are only enabled when credentials are configured:
either both `ADMIN_USER` and `ADMIN_PASSWORD` in the env, or
`ADMIN_CREDENTIALS_FILE` pointing at a `user:password` file (handy for
docker secrets — same shape as the enforcer's RPC cookie). The file
takes precedence when both are set. A trailing newline in the file is
ignored; neither the user nor the password may contain `:` (it's the
HTTP Basic auth separator — the dashboard refuses to boot if one does).

### Systemd drop-in

The shipped `deploy/systemd/simplepool-dashboard.service` unit sets
sane defaults. Override deployment-specific things with a drop-in
so you don't fork the shipped template:

```sh
sudo mkdir -p /etc/systemd/system/simplepool-dashboard.service.d
sudo tee /etc/systemd/system/simplepool-dashboard.service.d/local.conf <<'CONF'
[Service]
# Public "connect a miner" URL rendered on the About card
Environment=PUBLIC_STRATUM_URL=stratum+tcp://pool.example.com:3334

# Admin panel — omit both vars to keep /admin returning 503
Environment=ADMIN_USER=admin
Environment=ADMIN_PASSWORD=<paste from openssl rand -base64 21 | tr -d '=+/'>

# Only used by /admin: needed for the reserve-balance probe and audit
# math cross-check
Environment=THUNDER_RPC_URL=http://127.0.0.1:6009
Environment=POOL_THUNDER_RESERVE_ADDRESS=<same as proxy.conf's reserve address>
Environment=POOL_PPS_SATS_PER_DIFF=1000
CONF
sudo systemctl daemon-reload
sudo systemctl enable --now simplepool-dashboard.service
```

**Generate a fresh admin password** — do NOT commit or share:

```sh
openssl rand -base64 21 | tr -d '=+/' \
    | sudo tee /root/simplepool-admin-cred.txt
sudo chmod 600 /root/simplepool-admin-cred.txt
```

Then paste into the drop-in and reload. To rotate later, see the
scripted one-liner in [OPERATOR_GUIDE.md](OPERATOR_GUIDE.md#rotating-the-admin-password).

### Reverse proxy (recommended)

The dashboard binds `0.0.0.0:8081` — reachable directly. In
production you probably want nginx / caddy in front of it. Solo
`deploy/nginx/simplepool.conf` has a working template.

Do NOT expose `/admin` on plain HTTP over the internet without at
least TLS from nginx. Basic auth over plain HTTP leaks the password
on every request.

---

## Part F — payout worker (PPS modes only)

The payout worker drains `pps_credits.accrued_sats - paid_sats` by
issuing Thunder transactions. Deploy as a systemd service:

```sh
# assumes deploy/systemd/simplepool-payout.service was already installed
# during Part B (or via deploy-to-server.sh)
sudo mkdir -p /etc/systemd/system/simplepool-payout.service.d
sudo tee /etc/systemd/system/simplepool-payout.service.d/local.conf <<'CONF'
[Service]
Environment=THUNDER_FROM_ADDRESS=<same as the dashboard's POOL_THUNDER_RESERVE_ADDRESS>
# Below have defaults; override if you want:
# Environment=PAYOUT_MIN_SATS=10000
# Payout runs are a daily batch (24h). The settle clock is separate on
# purpose: a batch already broadcast is re-checked every 30s, because
# nobody in it is credited until a tick sees it in a Thunder block.
# Environment=PAYOUT_INTERVAL_MS=86400000
# Environment=PAYOUT_SETTLE_INTERVAL_MS=30000
# Environment=PAYOUT_RETRY_INTERVAL_MS=300000
# Environment=PAYOUT_MAX_PER_TICK=50
CONF
sudo systemctl daemon-reload
sudo systemctl enable --now simplepool-payout.service
sudo journalctl -u simplepool-payout.service -f
```

The worker is idle when the Thunder reserve has no funds — it logs
`payout: reserve short — available=0 needed=N` and skips harmlessly,
retrying on the 5-minute retry clock rather than the daily one. See the deposit runbook in
[OPERATOR_GUIDE.md](OPERATOR_GUIDE.md) for how to actually fund it.

For dry-run (see the exact payout it WOULD send without doing it),
run manually with `PAYOUT_DRY_RUN=1`:

```sh
sudo -u simplepool -H bash -lc '
  cd /home/simplepool/payout &&
  PAYOUT_DRY_RUN=1 \
  PAYOUT_DB_PATH=/home/simplepool/data/shares.db \
  THUNDER_RPC_URL=http://127.0.0.1:6009 \
  THUNDER_FROM_ADDRESS=any \
  PAYOUT_INTERVAL_MS=2000 \
  PAYOUT_SETTLE_INTERVAL_MS=2000 \
  node index.js
'
```

---

## Part G — first-run verification

Start everything:

```sh
sudo systemctl enable --now simplepool.service
sudo systemctl enable --now simplepool-dashboard.service
sudo systemctl enable --now simplepool-payout.service   # PPS modes only
sudo systemctl status simplepool simplepool-dashboard simplepool-payout
```

Then check:

1. **Pool is listening**:
   ```sh
   ss -tlnp | grep :3334
   ```
2. **Pool talks to bitcoind / enforcer**:
   ```sh
   sudo journalctl -u simplepool -n 20 --no-pager
   # look for:  "new job: height=... prev=... txs=..."
   # log_level=info will show one line per template refresh
   ```
3. **Dashboard reachable**:
   ```sh
   curl -sSf http://127.0.0.1:8081/ >/dev/null && echo dashboard ok
   ```
4. **Admin panel gated**:
   ```sh
   curl -sS -o /dev/null -w "no auth: %{http_code}\n" http://127.0.0.1:8081/admin
   # expect: no auth: 401
   ADMIN_PASS=$(sudo cat /root/simplepool-admin-cred.txt | cut -d' ' -f3)
   curl -sS -o /dev/null -w "with pw: %{http_code}\n" -u "admin:$ADMIN_PASS" http://127.0.0.1:8081/admin
   # expect: with pw: 200
   ```
5. **First miner authorizes**:
   Point a miner at `stratum+tcp://<your-host>:3334` with the mode's
   expected username format (BTC for solo, Thunder for pps / pps-classic).
   Watch a share come in:
   ```sh
   sudo journalctl -u simplepool -f | grep SUBMIT
   ```
   The `[SUBMIT CHECK]` line shows the sent hash, worker target, and
   network target — see [NONCE_AND_SHARES.md](NONCE_AND_SHARES.md#the-share-validation-flow).
6. **Accrual is landing** (PPS modes only):
   ```sh
   sudo -u simplepool sqlite3 /home/simplepool/data/shares.db \
       "SELECT * FROM pps_credits"
   ```
   Should show a row with a growing `accrued_sats`.
7. **Audit cross-check green**:
   Open `http://<your-host>:8081/admin`, log in, click `audit →` on the
   worker's row. The **Cross-check** card should show ✓ — meaning the
   stored `accrued_sats` matches `Σ FLOOR(diff × rate)`. If it shows ⚠,
   the `POOL_PPS_SATS_PER_DIFF` env var doesn't match your
   `proxy.conf`'s `pps_sats_per_diff` — fix and restart the dashboard.

---

## Part H — quick troubleshooting

Full troubleshooting is in [OPERATOR_GUIDE.md](OPERATOR_GUIDE.md#troubleshooting).
Install-time trouble usually falls into one of these:

- **`make` fails with `hiredis/hiredis.h: No such file or directory`** —
  libhiredis dev headers aren't installed. See the prereqs.
- **Pool exits at boot with `initial GBT failed: curl: Could not connect
  to server`** — bitcoind or the enforcer isn't running / not
  reachable at the URL in `proxy.conf`. Verify with `curl` against
  the URL manually.
- **Enforcer exits with `Bitcoin Core REST server is not enabled`** —
  `rest=1` missing from bitcoin.conf.
- **Thunder exits with `Unable to verify existence of CUSF mainchain
  service(s)`** — Thunder started before the enforcer's gRPC (`:50051`)
  was ready. If you're using systemd, add
  `After=bip300301-enforcer.service` and `Requires=` to your
  Thunder unit; if running by hand, sleep 2s.
- **`config error: 'pool_btc_address' is required when pool_mode=pps-classic`** —
  self-explanatory; set it.
- **The pool logs `stratum listening on 0.0.0.0:3335` but miners are
  refused** — the port is bound and the firewall is dropping it. Nothing
  in the pool's log can tell you this, because from the pool's side
  everything worked. Adding a `listener` to `proxy.conf` does not open a
  port in `ufw`:
  ```sh
  sudo ufw allow 3335/tcp
  sudo simplepoolctl doctor     # checks every configured port, and ufw
  ```
  `doctor` reports listening and allowed separately for each port, which
  is the distinction that matters: a marketplace measuring a firewalled
  port reads the pool as down.
- **`stratum bind 0.0.0.0:3334: Address already in use`** — an old
  simplepool is still running. Match it by **exact process name**,
  not by pattern (a `pkill -f simplepool` from an SSH session will
  kill your own shell):
  ```sh
  sudo pkill -9 -x simplepool
  ```
- **Miner authorize is rejected with `invalid thunder address`** —
  in pps / pps-classic mode the username must be a Thunder base58
  address. See [NONCE_AND_SHARES.md](NONCE_AND_SHARES.md#extranonce1-allocation--how-uniqueness-is-guaranteed).

---

## Where to go next

- Running the pool day-to-day: [OPERATOR_GUIDE.md](OPERATOR_GUIDE.md)
- Auditing the numbers: [NONCE_AND_SHARES.md](NONCE_AND_SHARES.md)
- Design rationale for pps-classic vs pps:
  [CLASSIC_PAYOUTS.md](CLASSIC_PAYOUTS.md)
- Regtest full walk-through:
  [scripts/regtest/README.md](scripts/regtest/README.md)
- Branch-level test plan: [VERIFY.md](VERIFY.md)
