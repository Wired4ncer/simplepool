# simplepool — docker deployment

Runs the three simplepool services (stratum proxy, dashboard, payout worker)
against a Thunder daemon and a Bitcoin Core node that live outside this
compose stack (typically on the same host).

## Prerequisites

- Docker Engine 24+ with the `compose` plugin
- A running Bitcoin Core (mainnet / forknet) reachable from this host
- A running Thunder daemon with JSON-RPC exposed on some host:port
- Ports **3334** (stratum) and **8081** (dashboard, override via env) free,
  plus any extra stratum ports you declare with `listener` lines in
  `proxy.conf`

## First run

```bash
cd deploy/docker

# 1. Configure the compose stack
cp .env.example .env
$EDITOR .env

# 2. Provide a proxy.conf. Start from the repo template and edit:
cp ../../proxy.conf.example proxy.conf
$EDITOR proxy.conf
#   IMPORTANT: inside the container the DB path must be /data/shares.db
#              set:  db_path = /data/shares.db
#   bitcoind_url can stay 127.0.0.1 because simplepool runs in host network mode.

# 3. Build images and bring the stack up
docker compose up -d --build

# 4. Watch the logs
docker compose logs -f simplepool
docker compose logs -f dashboard
docker compose logs -f payout
```

## Container-facing paths

Everything reads/writes the same SQLite file via a shared bind mount:

| host path              | container path         | who writes                    |
|------------------------|-----------------------|-------------------------------|
| `../../data/`          | `/data/`              | simplepool (RW), payout (RW), dashboard (RO) |
| `./proxy.conf`         | `/etc/simplepool/proxy.conf` | simplepool reads       |

`../../data` is the repo's `data/` folder — same as the bare-metal deploy.

## Networking

- **simplepool** uses `network_mode: host` so miners hit `:3334` without NAT
  and so `bitcoind_url = http://127.0.0.1:8332` in `proxy.conf` works
  unchanged from bare-metal setups. Host networking also means extra
  `listener` ports need no compose change to be reachable — but they do need
  opening in the host firewall, which nothing here does for you.
- **dashboard** and **payout** use the default bridge; they reach Thunder via
  `host.docker.internal:6009` (which `extra_hosts` maps to the host gateway
  on Linux, native on macOS/Windows). If Thunder lives on a different box,
  set `THUNDER_RPC_URL` to a real hostname/IP.

## Updating after a code change

```bash
git pull
docker compose up -d --build
```

Compose rebuilds only images whose sources changed (via layer cache) and
restarts only the affected containers. Stratum drops connections on restart —
miners auto-reconnect within seconds.

## Common operations

```bash
# Stop the stack
docker compose down

# Tail one service's log
docker compose logs -f payout

# Get a shell in the running dashboard container
docker compose exec dashboard sh

# Rebuild just one image after editing its Dockerfile
docker compose build simplepool
docker compose up -d simplepool

# Payout worker: do a dry-run cycle (no broadcast)
PAYOUT_DRY_RUN=1 docker compose up payout
```

## What this stack does NOT include

- **bitcoind / drivechain-forknet** — the mainchain node
- **Thunder daemon** — the sidechain node
- **bip300301_enforcer** — the BMM enforcer
- **electrs / esplora** — mainchain indexer used by the enforcer

Those form the drivechain infrastructure below simplepool. Install them on
the host (see the project's [INSTALL.md](../../INSTALL.md)) and point
`bitcoind_url` in `proxy.conf` + `THUNDER_RPC_URL` in `.env` at them.
