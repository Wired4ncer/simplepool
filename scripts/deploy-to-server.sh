#!/usr/bin/env bash
#
# Deploy simplepool (proxy + dashboard + nginx vhost) to a remote Ubuntu
# box. Idempotent: re-running just pulls latest, rebuilds, and restarts.
#
# Usage:
#   ./scripts/deploy-to-server.sh \
#       --host forknet@45.33.100.24 \
#       --root /home/forknet/forknet-software/simplepool \
#       --hostname pool.drivechain.info \
#       --ssh-key ~/.ssh/id_epitetus
#
# Optional flags:
#   --no-nginx          skip nginx install + vhost config
#   --no-dashboard      only deploy the C proxy
#   --branch <name>     git branch to deploy (default: main)
#
# The script will prompt once for the sudo password on the remote host
# (stored in memory for the run) unless SUDO_PASS is exported.
#
# What it does on the remote, in order:
#   1.  git fetch + checkout the requested branch, fast-forward.
#   2.  apt-get install build deps, nodejs, sqlite3, nginx (idempotent).
#   3.  make + record-build.sh              (rebuild the C proxy)
#   4.  npm install in dashboard/            (only if package-lock changed)
#   5.  sqlite3-init data/shares.db          (creates schema if missing)
#   6.  ms-to-seconds timestamp migration    (idempotent UPDATE)
#   7.  Render systemd unit templates with the right user/path and
#       install to /etc/systemd/system/, enable + restart both.
#   8.  Install nginx rate-limit conf.d snippet and vhost in
#       sites-available, symlink to sites-enabled, nginx -t && reload.
#       UFW: allow 80, 443, 3334 if ufw is active.
#
set -euo pipefail

HOST=""
ROOT=""
HOSTNAME_=""
SSH_KEY=""
BRANCH="main"
DO_NGINX=1
DO_DASH=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)       HOST="$2"; shift 2 ;;
        --root)       ROOT="$2"; shift 2 ;;
        --hostname)   HOSTNAME_="$2"; shift 2 ;;
        --ssh-key)    SSH_KEY="$2"; shift 2 ;;
        --branch)     BRANCH="$2"; shift 2 ;;
        --no-nginx)   DO_NGINX=0; shift ;;
        --no-dashboard) DO_DASH=0; shift ;;
        -h|--help)    sed -n '1,30p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

[[ -z "$HOST"     ]] && { echo "--host required" >&2; exit 2; }
[[ -z "$ROOT"     ]] && { echo "--root required" >&2; exit 2; }
[[ -z "$HOSTNAME_" ]] && { echo "--hostname required (e.g. pool.drivechain.info)" >&2; exit 2; }

USER_PART="${HOST%@*}"
HOST_PART="${HOST#*@}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SSH=(ssh)
[[ -n "$SSH_KEY" ]] && SSH+=(-i "$SSH_KEY")
SSH+=(-o StrictHostKeyChecking=accept-new "$HOST")

SCP=(scp)
[[ -n "$SSH_KEY" ]] && SCP+=(-i "$SSH_KEY")

if [[ -z "${SUDO_PASS:-}" ]]; then
    read -rs -p "sudo password for ${HOST}: " SUDO_PASS
    echo
fi
export SUDO_PASS

run_sudo() {
    # The password travels over the ssh session's stdin, never in the remote
    # command line: anything in that string is in the remote shell's argv,
    # which every local user on the server can read from ps or
    # /proc/<pid>/cmdline (base64 hides nothing). sudo -S reads exactly one
    # line from stdin. The command is base64-encoded so quoting/special
    # characters never appear unescaped on the remote side ([A-Za-z0-9+/=]
    # is safe inside any shell quoting), and starts by pointing its own
    # stdin at /dev/null: when sudo doesn't need the password (cached
    # timestamp, NOPASSWD) that line would otherwise still be sitting on
    # stdin for whatever the command runs.
    local cmd_b64
    cmd_b64="$(printf 'exec </dev/null; %s' "$*" | base64 | tr -d '\n')"
    printf '%s\n' "$SUDO_PASS" \
        | "${SSH[@]}" "sudo -S -p '' bash -c \"\$(echo $cmd_b64 | base64 -d)\""
}

run_remote() {
    "${SSH[@]}" "$*"
}

echo "==> [1/8] pull branch ${BRANCH} into ${ROOT}"
run_remote "cd ${ROOT} && git fetch origin && git checkout ${BRANCH} && git reset --hard origin/${BRANCH}"

echo "==> [2/8] ensure apt packages"
run_sudo "DEBIAN_FRONTEND=noninteractive apt-get update -q && \
          DEBIAN_FRONTEND=noninteractive apt-get install -yq \
          build-essential libsqlite3-dev libcurl4-openssl-dev sqlite3 nodejs npm nginx ufw"

echo "==> [3/8] build C proxy"
# An earlier deploy may have built as root; fix ownership so `make` works
# as the unprivileged user.
run_sudo "chown -R ${USER_PART}:${USER_PART} ${ROOT}/build ${ROOT}/data 2>/dev/null || true"
run_remote "cd ${ROOT} && make"
# The proxy embeds its own commit (see src/version.c), so the manifest is
# belt-and-braces for it — but recording one here means /api/versions can still
# name the commit if the binary is ever replaced by one built elsewhere.
run_remote "cd ${ROOT} && ./scripts/record-build.sh simplepool . build/simplepool || true"

if [[ "$DO_DASH" == "1" ]]; then
    echo "==> [4/8] npm install in dashboard/"
    # `npm install` may pull a better-sqlite3 prebuilt binary compiled
    # against a different Node ABI than what's installed here. `npm
    # rebuild` recompiles native modules against the local Node so the
    # dashboard doesn't crash with NODE_MODULE_VERSION mismatch at start.
    run_remote "cd ${ROOT}/dashboard && npm install --no-audit --no-fund && npm rebuild"
fi

echo "==> [5/8] init sqlite schema if missing"
run_remote "cd ${ROOT} && mkdir -p data && [ -s data/shares.db ] || sqlite3 data/shares.db < schema.sql"

echo "==> [6/8] one-shot ms->s timestamp migration (idempotent)"
run_remote "cd ${ROOT} && sqlite3 data/shares.db <<SQL
UPDATE workers      SET first_seen = first_seen/1000 WHERE first_seen > 10000000000;
UPDATE workers      SET last_seen  = last_seen/1000  WHERE last_seen  > 10000000000;
UPDATE shares       SET ts = ts/1000 WHERE ts > 10000000000;
UPDATE rejects      SET ts = ts/1000 WHERE ts > 10000000000;
UPDATE blocks_found SET ts = ts/1000 WHERE ts > 10000000000;
SQL"

echo "==> [7/8] render & install systemd units"
TMP_UNIT="$(mktemp)"; TMP_DASH="$(mktemp)"
# Staging on the remote goes in a fresh private (0700) directory, not at
# fixed /tmp names: /tmp is shared, so any local user on the server could
# pre-create /tmp/simplepool.service (or swap it between the scp and the
# install) and have root install their unit or nginx config.
REMOTE_TMP="$(run_remote 'mktemp -d')"
[[ "$REMOTE_TMP" == /* ]] || { echo "remote mktemp -d failed" >&2; exit 1; }
trap 'rm -f "$TMP_UNIT" "$TMP_DASH"; run_remote "rm -rf ${REMOTE_TMP}" 2>/dev/null || true' EXIT
sed -e "s|@USER@|${USER_PART}|g" -e "s|@ROOT@|${ROOT}|g" \
    "${SCRIPT_DIR}/deploy/systemd/simplepool.service" > "$TMP_UNIT"
sed -e "s|@USER@|${USER_PART}|g" -e "s|@ROOT@|${ROOT}|g" \
    "${SCRIPT_DIR}/deploy/systemd/simplepool-dashboard.service" > "$TMP_DASH"

"${SCP[@]}" "$TMP_UNIT" "${HOST}:${REMOTE_TMP}/simplepool.service" >/dev/null
"${SCP[@]}" "$TMP_DASH" "${HOST}:${REMOTE_TMP}/simplepool-dashboard.service" >/dev/null
run_sudo "install -m 0644 ${REMOTE_TMP}/simplepool.service /etc/systemd/system/simplepool.service && \
          install -m 0644 ${REMOTE_TMP}/simplepool-dashboard.service /etc/systemd/system/simplepool-dashboard.service && \
          systemctl daemon-reload && \
          systemctl enable --now simplepool.service && \
          systemctl restart simplepool.service"

if [[ "$DO_DASH" == "1" ]]; then
    run_sudo "systemctl enable --now simplepool-dashboard.service && \
              systemctl restart simplepool-dashboard.service"
fi

if [[ "$DO_NGINX" == "1" ]]; then
    echo "==> [8/8] install nginx vhost for ${HOSTNAME_}"
    TMP_NGX="$(mktemp)"
    sed -e "s|pool.drivechain.info|${HOSTNAME_}|g" \
        "${SCRIPT_DIR}/deploy/nginx/pool.drivechain.info.conf" > "$TMP_NGX"
    "${SCP[@]}" "$TMP_NGX" "${HOST}:${REMOTE_TMP}/${HOSTNAME_}.conf" >/dev/null
    "${SCP[@]}" "${SCRIPT_DIR}/deploy/nginx/pool-ratelimit.conf" "${HOST}:${REMOTE_TMP}/pool-ratelimit.conf" >/dev/null
    rm -f "$TMP_NGX"

    run_sudo "install -m 0644 ${REMOTE_TMP}/${HOSTNAME_}.conf /etc/nginx/sites-available/${HOSTNAME_} && \
              install -m 0644 ${REMOTE_TMP}/pool-ratelimit.conf /etc/nginx/conf.d/pool-ratelimit.conf && \
              ln -sf /etc/nginx/sites-available/${HOSTNAME_} /etc/nginx/sites-enabled/${HOSTNAME_} && \
              rm -f /etc/nginx/sites-enabled/default && \
              nginx -t && systemctl reload nginx"

    # Open ports if ufw is active.
    run_sudo "if ufw status 2>/dev/null | grep -q 'Status: active'; then \
                ufw allow 80/tcp;  ufw allow 443/tcp;  ufw allow 3334/tcp; \
              fi"
fi

echo "==> deploy done."
echo "    proxy:     $(${SSH[@]} systemctl is-active simplepool 2>/dev/null || echo unknown)"
[[ "$DO_DASH" == "1" ]] && \
echo "    dashboard: $(${SSH[@]} systemctl is-active simplepool-dashboard 2>/dev/null || echo unknown)"
[[ "$DO_NGINX" == "1" ]] && \
echo "    nginx:     $(${SSH[@]} systemctl is-active nginx 2>/dev/null || echo unknown)"
echo
echo "    miners connect to:  stratum tcp://${HOST_PART}:3334"
echo "    dashboard:          http://${HOSTNAME_}/  (once DNS points here)"
