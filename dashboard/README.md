# simplepool dashboard

A small read-only Node.js dashboard for the [simplepool](../) solo-mining
stratum server (the one with direct miner payouts + a small operator
fee). It opens a **snapshot** of the proxy's SQLite database in
read-only WAL mode and serves a public web page with hashrate, a worker
leaderboard, recent blocks, a historical "blocks found by the pool"
page, and a per-worker drilldown.

Each worker row in the leaderboard is a `<bitcoin_address>.<rig_label>`
pair (stratum username convention); the `workers.payout_address` column
lets future views roll up multiple rigs per address. The historical
blocks panel at `/blocks` reads `blocks_found` and shows finder,
payout address, on-chain reward, and operator fee for every block the
pool has ever solved.

## Prerequisites

- Node 20+
- The `simplepool` binary writing to a SQLite file (default `../data/shares.db`)
- A periodic snapshot at `../data/shares.snapshot.db` produced via
  `sqlite3 shares.db ".backup shares.snapshot.db"` on a cron — see the
  proxy [README](../README.md#database--dashboard-snapshot).

The dashboard opens the snapshot **read-only**. SQLite's online backup is
atomic, so the snapshot is always a consistent view of the live DB and the
proxy is never blocked by dashboard queries. It is safe to point multiple
dashboard instances at the same snapshot file.

## Install

```
cd simplepool/dashboard
npm install
cp .env.example .env
# edit .env if your DB path or port differs
```

## Run

```
npm start          # production
npm run dev        # auto-restart on file change
```

Defaults: `PORT=8081`, `PROXY_DB_PATH=../data/shares.snapshot.db`.

If the snapshot file doesn't exist yet, the dashboard starts anyway and
displays "no data yet" until the first `.backup` produces it. You can also
point `PROXY_DB_PATH` at the live `shares.db` if you don't want to run a
snapshot cron — SQLite's WAL mode makes that safe too, just less isolated.

## Endpoints

| Path                  | Description                                             |
| --------------------- | ------------------------------------------------------- |
| `/`                   | Overview, leaderboard, last 5 blocks                    |
| `/blocks`             | Full historical "blocks found by the pool", paginated   |
| `/blocks?before=<ts>` | Next page (older than the given UNIX timestamp)         |
| `/worker/:name`       | Per-worker drilldown                                    |
| `/api/overview`       | JSON                                                    |
| `/api/leaderboard`    | JSON                                                    |
| `/api/worker/:name`   | JSON                                                    |
| `/api/blocks`         | JSON paginated, `?limit=N&before=<ts>` (default 50)     |
| `/api/versions`       | Build provenance of every component (see below)         |
| `/api/status`         | Everything at once: pool, node, health, versions        |
| `/healthz`            | `{ ok: true, db_ready: bool }`                          |
| `/health`             | Full hard-failure detail; 503 when a check is failing   |

`/api/status` is the one URL to poll if you want a single document: pool
totals and hashrate, mainchain tip, the health checks, and which commit of
each component is running. It always returns 200 — it is a report, and a
report that a check is failing was still produced successfully. Watch
`health.ok` for the condition and `/health` for a status code to alert on.

## Pool identity

Every page carries a strip under the header naming what this pool actually
is: the **network** its coinbases are built for, the **mode** (`solo` or
`pps-classic`) and fee, the **coinbase tag**, the **operator address** the
fee is paid to, and — under `pps-classic` — the **pool wallet** the
net-of-fee reward goes to. `/api/status` returns the same five fields under
`pool`.

None of it is derivable from the stratum URL a miner was handed. The port
looks identical whether the pool is mining mainnet or regtest, whether a
block pays its finder or the pool's wallet, and whoever collects the fee.

The dashboard does **not** take these from its own environment. The proxy
writes them to `pool_meta` at startup and the dashboard reads them back —
same rule as the PPS rate, and for the same reason: a second copy of the
config is a copy that can disagree with the pool it claims to describe.
The practical consequence is that the strip reads `unknown` until the proxy
has restarted onto a build that publishes them. That is deliberate; a banner
that asserts the wrong network is worse than one that admits it doesn't know.

`network_source` says how the network was determined:

| Value      | Meaning |
| ---------- | ------- |
| `node`     | `getblockchaininfo` answered. Authoritative. |
| `inferred` | It did not — the CUSF enforcer serves only `getblocktemplate` and `submitblock` — so the network was read off the operator address. Cannot distinguish testnet from signet, and says so. |

A non-mainnet pool is flagged with a warn-coloured rule, because "why has my
payout not arrived" and "this pool is mining signet" are frequently the same
question.

## "About the numbers on this page"

The explanatory card on `/` branches on `pool_mode`, because almost nothing
in it is shared between the modes:

| | `solo` | `pps-classic` |
| --- | --- | --- |
| A share that isn't a block | worth nothing | credited at the live rate |
| Block reward goes to | the finder, in the coinbase | the pool's BTC wallet |
| Stratum username | a **Bitcoin** address (P2WPKH / P2PKH / P2SH — **not** taproot) | a **Thunder** address |
| Rejection if you get it wrong | `invalid payout address in stratum username` | `invalid thunder address` |

That last row is why this is not cosmetic. `src/stratum.c` branches on
`pps_enabled` at authorize, so the card's instructions are load-bearing: a
solo pool that tells miners to use a Thunder address is telling them to do
the one thing that cannot work.

Every figure comes from `pool_meta` — rate, gross, fee, operator address,
pool wallet, network — and the address examples follow the pool's network, so
a signet pool shows `tb1q…` rather than `bc1q…`. Nothing in the card is a
literal. The version this replaced hardcoded *"1 000 sats × share
difficulty"*, which was never true of a rate that is derived per template and
moves with difficulty; a pinned rate (`rate_source = override`) is now called
out with the fee it actually implies.

Unknown mode gets prose naming both, and no username form — same rule as the
identity strip, since guessing wrong costs a miner real time.

## Build provenance — `/api/versions`

Answers "which commit is this pool actually running?" for simplepool, the
bip300301 enforcer, thunder, and bitcoind — without SSHing anywhere.

The commit is looked for **in the artifact first and in a source tree only as
a last resort**, because a checkout's HEAD is not what the running binary was
built from. Three sources, in descending order of what they prove:

| `provenance` | Source | What it proves |
| ------------ | ------ | -------------- |
| `binary`   | `<bin> --version` prints the commit | The answer travels inside the thing it describes. Nothing to keep in sync. |
| `manifest` | `<bin>.build.json`, pinned by sha256 | Recorded at build time; survives the source being deleted. |
| `checkout` | the git tree beside the binary | What *would* be built now — not what is running. Never required. |

simplepool and the enforcer embed their commit, so they resolve at `binary`.
thunder and bitcoind print a version number and no commit; they resolve at
`manifest` once you have run `scripts/record-build.sh` (below). Without a
manifest they fall back to `checkout` and say so in `notes`.

Alongside the resolved `commit` / `branch` / `repo`, each component carries
the evidence it was derived from:

- **`running`** — the live process, found through `/proc/<pid>/exe`, so it
  describes what is serving traffic rather than whatever now sits at the
  configured path. `binary_replaced: true` means the file was rebuilt and the
  service never restarted; `process_found: false` means nothing is running it.
- **`manifest`** — the recorded build, with `verified` false if the binary's
  sha256 has moved since (rebuilt without re-recording). A stale manifest is
  reported, and ranked *below* the checkout rather than trusted.
- **`checkout`** — remote, branch, HEAD, and whether tracked files have been
  modified since (`dirty`).

`commit_matches` cross-checks whichever pairs exist, and is `null` — never an
optimistic `true` — when only one source knew a commit. `all_clean` plus
`needs_review` summarise the whole report in one field.

### Recording a build

Run this right after building any component that doesn't embed its own
commit, in the same script that does the build so the two can't drift:

```
scripts/record-build.sh thunder \
    ~/forknet-software/thunder-rust \
    ~/forknet-software/thunder-rust/target/release/thunder_app
```

It writes `<binary>.build.json` next to the binary. After that the checkout
can be deleted and `/api/versions` still answers — source is a build-time
dependency, not a runtime one. `scripts/deploy-to-server.sh` does this for
simplepool automatically.

### Configuration

Paths default to the layout this pool deploys with — all four checkouts as
siblings of the simplepool repo — and are overridden per component with
`SIMPLEPOOL_REPO_DIR` / `SIMPLEPOOL_BIN`, `ENFORCER_REPO_DIR` /
`ENFORCER_BIN`, `THUNDER_REPO_DIR` / `THUNDER_BIN`, `BITCOIN_REPO_DIR` /
`BITCOIND_BIN`, and `<COMPONENT>_BUILD_MANIFEST` for a manifest kept
somewhere other than beside the binary. Set both of a pair empty to drop that
component. `VERSIONS_USE_CHECKOUT=0` disables the git fallback entirely, for
a deployment that ships binaries without source.

Results are cached for `VERSIONS_TTL_MS` (default 5 min) since they only
change on restart; add `?force=1` right after a redeploy.

Absolute paths are deliberately not in the response; repo URLs are stripped
of any embedded credentials.

## Public deployment (nginx)

Reverse-proxy with optional basic auth, gzip, and a tiny cache for `/api/*`:

```nginx
proxy_cache_path /var/cache/nginx/simplepool levels=1:2 keys_zone=simplepool:10m
                 max_size=100m inactive=10m use_temp_path=off;

server {
    listen 80;
    server_name pool.example.com;

    gzip on;
    gzip_types text/plain text/css application/json application/javascript;

    # auth_basic           "simplepool";
    # auth_basic_user_file /etc/nginx/.htpasswd;

    location /api/ {
        proxy_pass         http://127.0.0.1:8081;
        proxy_cache        simplepool;
        proxy_cache_valid  200 5s;
        add_header         X-Cache-Status $upstream_cache_status;
    }

    location / {
        proxy_pass         http://127.0.0.1:8081;
        proxy_set_header   Host $host;
        proxy_set_header   X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header   X-Forwarded-Proto $scheme;
    }
}
```

## How hashrate is estimated

```
H/s ≈ sum(difficulty over window) * 2^32 / seconds_the_shares_span
```

This is the standard pool estimator, with one correction. The divisor is
the span the shares actually cover — first share in the window to now —
not the nominal width of the window. Dividing by time nothing was mined
in reports a rate nobody ran at, and it goes wrong exactly when someone
is most likely to be looking: a pool eleven hours old reads half its true
rate against a 24 h window, and a rig ten minutes into a rented contract
reads 1/144th of what it is doing. Both heal on their own as the window
fills, which is why it survived so long — by the time anyone doubts the
number it is right again.

The span is clamped at both ends: never longer than the nominal window,
and never shorter than a minute, so a single share a few seconds old
cannot divide by ~0 and report a gigahash spike. With no shares at all it
falls back to the nominal window, so an idle pool reports zero rather
than a clamped fraction of nothing.

It still converges slowly for workers with very few shares in-window —
that is variance, not a divisor problem.
