#!/usr/bin/env bash
#
# simplepool installer — run this ON the Linux server you want the pool
# to live on. Interactive by default: it asks where to install, which pool
# mode (solo / pps-classic), your bitcoind RPC, addresses, dashboard domain,
# admin password, nginx/TLS and firewall, shows a summary, then does the
# whole install and tells you what miners should connect to.
#
# One line on a fresh Ubuntu/Debian box:
#
#   curl -fsSL https://raw.githubusercontent.com/LayerTwo-Labs/simplepool/main/scripts/install.sh | sudo bash
#
# Two ways to get the code, asked during the interview:
#
#   release  (default) download the published tarball for this machine's
#            architecture, check it against the release SHA256SUMS, unpack it.
#            No compiler, no git clone, no waiting on a build.
#   source   git clone + `make`. What you want to run an unreleased branch,
#            or on an architecture with no published build.
#
# Either way the result is the same tree at $ROOT, so everything after that
# step — config, database, systemd, nginx — is one code path.
#
# Afterwards, `simplepoolctl` (installed to /usr/local/bin) is how you drive
# the box: status, logs, doctor, upgrade, uninstall.
#
# Sibling script: scripts/deploy-to-server.sh drives an *already installed*
# box from your workstation. This one bootstraps a fresh box from nothing.
#
# Usage:
#   sudo ./scripts/install.sh                      # ask me everything
#   curl -fsSL <raw-url>/install.sh | sudo bash    # standalone
#
# Non-interactive (CI / re-runs) — every prompt has a matching flag:
#   sudo ./scripts/install.sh --non-interactive --yes \
#        --root /home/simplepool --user simplepool \
#        --mode solo --operator-address bc1q... \
#        --bitcoind-url http://127.0.0.1:8332 \
#        --bitcoind-user rpcuser --bitcoind-pass rpcpass \
#        --hostname pool.example.com --admin-user admin
#
# Flags:
#   --from-release [<tag>]    install a published release (default; omit the
#                             tag for the latest)
#   --from-source             git clone + make instead
#   --root <dir>              install directory        (default /home/simplepool)
#   --user <name>             service user             (default simplepool)
#   --repo <url> --branch <b> source to clone/update   (default upstream/main)
#   --mode solo|pps-classic
#   --stratum-port <n>        default 3334
#   --bitcoind-url/-user/-pass
#   --operator-address <addr> BTC address that takes the fee cut
#   --fee-bps <n>             default 100 (= 1%)
#   --pool-btc-address <addr>       pps-classic
#   --thunder-address <addr>        pps-classic reserve address (dashboard
#                                   deposits + payout worker source)
#   --thunder-rpc-url <url>         default http://127.0.0.1:6009
#   --pps-sats-per-diff <n>         default 1000
#   --payout-interval-hours <n>     how often payouts run (default 24)
#   --hostname <fqdn>         dashboard domain (nginx vhost + TLS)
#   --dashboard-port <n>      default 8081 (loopback; nginx fronts it)
#   --admin-user <name>       default admin
#   --admin-password <pw>     default: generated and printed once
#   --tls --email <addr>      run certbot --nginx after the vhost lands
#   --no-dashboard --no-payout --no-nginx --no-firewall --no-deps
#   --enable-firewall         `ufw enable` (OpenSSH is always allowed first)
#   --run-tests               run `make test` after the build (source installs)
#   --non-interactive         never prompt; use flags + saved answers
#   --yes                     skip the final confirmation
#
# Answers are saved to /etc/simplepool/install.env (root, 0600) and are
# reused as defaults on the next run, so re-running is cheap and safe.
#
set -euo pipefail

# ---------------------------------------------------------------- guards ----
[[ "$(uname -s)" == "Linux" ]] || {
    echo "install.sh targets Linux (systemd + apt/dnf). Detected: $(uname -s)." >&2
    echo "For local dev on macOS use: brew install sqlite curl hiredis && make" >&2
    exit 1
}

# The headline install path is `curl ... | sudo bash`, where there is no file
# on disk to point at: BASH_SOURCE[0] is not a readable path, so re-execing
# ourselves and reading our own comment block for --help both have to be
# handled rather than assumed away.
if [[ -n "${BASH_SOURCE[0]:-}" && -f "${BASH_SOURCE[0]}" ]]; then
    SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    PIPED=0
else
    SELF=""
    PIPED=1
fi

usage() {
    if [[ "$PIPED" == "0" ]]; then
        sed -n '2,73p' "$SELF" | sed 's/^# \{0,1\}//'
    else
        # Piped: our source is gone from stdin, so there is nothing to read
        # back. Point at the copy that definitely exists.
        cat <<'USAGE'
simplepool installer.

  curl -fsSL <raw-url>/install.sh | sudo bash                  # interactive
  curl -fsSL <raw-url>/install.sh | sudo bash -s -- --help     # you are here

Flags can be passed after `-s --`. The full list is in the script's header:
  https://github.com/LayerTwo-Labs/simplepool/blob/main/scripts/install.sh
and the walkthrough is in INSTALL.md.
USAGE
    fi
}

if [[ $EUID -ne 0 ]]; then
    if [[ "$PIPED" == "1" ]]; then
        # stdin is already consumed, so we cannot re-exec ourselves. Say what
        # to type instead of failing somewhere deeper with a permissions error.
        echo "install.sh needs root, and a piped script cannot re-run itself." >&2
        echo "Pipe into 'sudo bash', not 'bash':" >&2
        echo "  curl -fsSL <raw-url>/install.sh | sudo bash" >&2
        exit 1
    fi
    command -v sudo >/dev/null 2>&1 || { echo "run as root (no sudo found)" >&2; exit 1; }
    echo "==> re-executing under sudo"
    exec sudo -E bash "$SELF" "$@"
fi

STATE_DIR=/etc/simplepool
STATE_FILE="$STATE_DIR/install.env"

# ---------------------------------------------------------------- output ----
BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; OFF=$'\033[0m'
[[ -t 1 ]] || { BOLD=""; DIM=""; RED=""; GRN=""; YEL=""; OFF=""; }

STEP_N=0
STEP_TOTAL=0
step() { STEP_N=$((STEP_N + 1)); echo; echo "${BOLD}==> [${STEP_N}/${STEP_TOTAL}] $*${OFF}"; }
say()  { echo "    $*"; }
warn() { echo "${YEL}    warning: $*${OFF}" >&2; }
die()  { echo "${RED}fatal: $*${OFF}" >&2; exit 1; }

# ------------------------------------------------------------- defaults -----
ROOT=""
SVC_USER=""
# The canonical repo. Releases, container images and CI all live here; the
# author's personal remote is a mirror and has no published releases, so
# defaulting to it would make --from-release fail on a fresh box.
REPO_SLUG="LayerTwo-Labs/simplepool"
REPO_URL="https://github.com/${REPO_SLUG}.git"
BRANCH="main"
# release = download a published tarball; source = git clone + make.
# Left empty so the interview can pick a context-aware default (a checkout
# you are standing in means source; a piped one-liner means release) without
# overwriting a saved answer or a flag.
SOURCE=""
RELEASE_TAG=""          # empty = whatever the latest release is
MODE=""
STRATUM_PORT="3334"
BITCOIND_URL=""
BITCOIND_USER=""
BITCOIND_PASS=""
OPERATOR_ADDRESS=""
FEE_BPS="100"
COINBASE_TAG="/simplepool/"
POOL_BTC_ADDRESS=""
THUNDER_ADDRESS=""
THUNDER_RPC_URL="http://127.0.0.1:6009"
PPS_SATS_PER_DIFF=""   # empty = derive from the block template (recommended)
PAYOUT_INTERVAL_HOURS="24"
FQDN=""
DASH_PORT="8081"
PUBLIC_STRATUM_URL=""
ADMIN_USER="admin"
ADMIN_PASSWORD=""
EMAIL=""
DO_TLS=0
DO_DASH=1
DO_PAYOUT=""      # decided from MODE unless flagged
DO_NGINX=1
DO_UFW=1
ENABLE_UFW=0
DO_DEPS=1
RUN_TESTS=0
INTERACTIVE=1
ASSUME_YES=0

# Load saved answers from a previous run (they become the prompt defaults).
if [[ -r "$STATE_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$STATE_FILE"
    say "loaded previous answers from $STATE_FILE"
fi
# Remember what the last run used so we can tell an auto-derived answer
# (which must follow a changed domain/port) from a hand-written one.
PREV_FQDN="${FQDN:-}"
PREV_STRATUM_PORT="${STRATUM_PORT:-}"

# ---------------------------------------------------------------- flags -----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)              ROOT="$2"; shift 2 ;;
        --user)              SVC_USER="$2"; shift 2 ;;
        --repo)              REPO_URL="$2"; shift 2 ;;
        --branch)            BRANCH="$2"; shift 2 ;;
        --mode)              MODE="$2"; shift 2 ;;
        --stratum-port)      STRATUM_PORT="$2"; shift 2 ;;
        --bitcoind-url)      BITCOIND_URL="$2"; shift 2 ;;
        --bitcoind-user)     BITCOIND_USER="$2"; shift 2 ;;
        --bitcoind-pass)     BITCOIND_PASS="$2"; shift 2 ;;
        --operator-address)  OPERATOR_ADDRESS="$2"; shift 2 ;;
        --fee-bps)           FEE_BPS="$2"; shift 2 ;;
        --coinbase-tag)      COINBASE_TAG="$2"; shift 2 ;;
        --pool-btc-address)  POOL_BTC_ADDRESS="$2"; shift 2 ;;
        --thunder-address)   THUNDER_ADDRESS="$2"; shift 2 ;;
        --thunder-rpc-url)   THUNDER_RPC_URL="$2"; shift 2 ;;
        --pps-sats-per-diff) PPS_SATS_PER_DIFF="$2"; shift 2 ;;
        --payout-interval-hours) PAYOUT_INTERVAL_HOURS="$2"; shift 2 ;;
        # --from-release takes an OPTIONAL tag, so the next argument is only
        # consumed when it looks like one ("v0.2.0" or "0.2.0"). Anything
        # else — another flag, or nothing at all — leaves the tag empty and
        # resolves to the latest release.
        --from-release)      SOURCE="release"
                             if [[ "${2:-}" == v* || "${2:-}" =~ ^[0-9] ]]; then
                                 RELEASE_TAG="$2"; shift 2
                             else shift; fi ;;
        --release-tag)       SOURCE="release"; RELEASE_TAG="$2"; shift 2 ;;
        --from-source)       SOURCE="source"; shift ;;
        --hostname)          FQDN="$2"; shift 2 ;;
        --dashboard-port)    DASH_PORT="$2"; shift 2 ;;
        --admin-user)        ADMIN_USER="$2"; shift 2 ;;
        --admin-password)    ADMIN_PASSWORD="$2"; shift 2 ;;
        --email)             EMAIL="$2"; shift 2 ;;
        --tls)               DO_TLS=1; shift ;;
        --no-dashboard)      DO_DASH=0; shift ;;
        --no-payout)         DO_PAYOUT=0; shift ;;
        --no-nginx)          DO_NGINX=0; shift ;;
        --no-firewall)       DO_UFW=0; shift ;;
        --no-deps)           DO_DEPS=0; shift ;;
        --enable-firewall)   ENABLE_UFW=1; shift ;;
        --run-tests)         RUN_TESTS=1; shift ;;
        --non-interactive)   INTERACTIVE=0; ASSUME_YES=1; shift ;;
        --yes|-y)            ASSUME_YES=1; shift ;;
        -h|--help)           usage; exit 0 ;;
        *) die "unknown arg: $1  (try --help)" ;;
    esac
done

[[ -r /dev/tty ]] || INTERACTIVE=0

# ---------------------------------------------------------------- prompts ---
ask() { # ask VAR "question" "fallback default"
    local __var="$1" __q="$2" __def="${3:-}" __cur="" __ans=""
    __cur="${!__var-}"
    [[ -n "$__cur" ]] && __def="$__cur"
    if [[ $INTERACTIVE -eq 0 ]]; then
        printf -v "$__var" '%s' "$__def"; return
    fi
    if [[ -n "$__def" ]]; then
        read -r -p "    $__q [${__def}]: " __ans </dev/tty || true
    else
        read -r -p "    $__q: " __ans </dev/tty || true
    fi
    [[ -z "$__ans" ]] && __ans="$__def"
    printf -v "$__var" '%s' "$__ans"
}

ask_secret() { # ask_secret VAR "question"  (empty answer keeps the current value)
    local __var="$1" __q="$2" __cur="" __ans=""
    __cur="${!__var-}"
    if [[ $INTERACTIVE -eq 0 ]]; then return; fi
    local hint=""
    [[ -n "$__cur" ]] && hint=" [keep current]"
    read -r -s -p "    $__q${hint}: " __ans </dev/tty || true
    echo
    [[ -z "$__ans" ]] && __ans="$__cur"
    printf -v "$__var" '%s' "$__ans"
}

ask_yn() { # ask_yn VAR "question"   — VAR holds/receives 1 or 0
    local __var="$1" __q="$2" __cur="" __ans=""
    __cur="${!__var:-1}"
    if [[ $INTERACTIVE -eq 0 ]]; then printf -v "$__var" '%s' "$__cur"; return; fi
    local def="y"; [[ "$__cur" == "0" ]] && def="n"
    read -r -p "    $__q (y/n) [$def]: " __ans </dev/tty || true
    [[ -z "$__ans" ]] && __ans="$def"
    case "$__ans" in
        [Yy]*) printf -v "$__var" '%s' 1 ;;
        *)     printf -v "$__var" '%s' 0 ;;
    esac
}

ask_choice() { # ask_choice VAR "question" "opt1|desc" "opt2|desc" ...
    local __var="$1" __q="$2"; shift 2
    local opts=() descs=() i choice cur
    for spec in "$@"; do opts+=("${spec%%|*}"); descs+=("${spec#*|}"); done
    cur="${!__var-}"
    [[ -z "$cur" ]] && cur="${opts[0]}"
    if [[ $INTERACTIVE -eq 0 ]]; then printf -v "$__var" '%s' "$cur"; return; fi
    echo "    $__q"
    local defidx=1
    for i in "${!opts[@]}"; do
        printf "      %d) %-12s %s%s%s\n" "$((i+1))" "${opts[$i]}" "$DIM" "${descs[$i]}" "$OFF"
        [[ "${opts[$i]}" == "$cur" ]] && defidx=$((i+1))
    done
    read -r -p "    choice [$defidx]: " choice </dev/tty || true
    [[ -z "$choice" ]] && choice="$defidx"
    if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#opts[@]} )); then
        printf -v "$__var" '%s' "${opts[$((choice-1))]}"
    else
        # allow typing the name directly
        for i in "${!opts[@]}"; do
            [[ "$choice" == "${opts[$i]}" ]] && { printf -v "$__var" '%s' "$choice"; return; }
        done
        die "invalid choice: $choice"
    fi
}

# ------------------------------------------------------------- interview ----
echo
echo "${BOLD}simplepool installer${OFF}"
echo "${DIM}Answers are saved to $STATE_FILE and reused next time.${OFF}"

# Where does the code live? If we're running from inside a checkout, that
# checkout is the default; otherwise we'll clone.
IN_CHECKOUT=""
if [[ "$PIPED" == "0" ]]; then
    CANDIDATE="$(cd "$(dirname "$SELF")/.." && pwd)"
    [[ -f "$CANDIDATE/Makefile" && -f "$CANDIDATE/schema.sql" ]] && IN_CHECKOUT="$CANDIDATE"
fi
# Running from a checkout you already have means you probably want that
# checkout built. Arriving via the one-liner means there is nothing to build
# from, so a published release is the only answer that works out of the box.
[[ -n "$SOURCE" ]] || SOURCE="$([[ -n "$IN_CHECKOUT" ]] && echo source || echo release)"

echo
echo "${BOLD}-- location --${OFF}"
ask ROOT     "install directory" "${IN_CHECKOUT:-/home/simplepool}"
ROOT="${ROOT%/}"
DEFAULT_USER="simplepool"
[[ -d "$ROOT" ]] && DEFAULT_USER="$(stat -c '%U' "$ROOT" 2>/dev/null || echo simplepool)"
[[ "$DEFAULT_USER" == "root" ]] && DEFAULT_USER="${SUDO_USER:-simplepool}"
ask SVC_USER "service user (created if missing)" "$DEFAULT_USER"

echo
echo "${BOLD}-- where the code comes from --${OFF}"
ask_choice SOURCE "how should simplepool get onto this box?" \
    "release|download the published build for this machine — no compiler, no clone" \
    "source|git clone and compile from source (unreleased branches, other arches)"

if [[ "$SOURCE" == "release" ]]; then
    ask RELEASE_TAG "release tag (blank = the latest release)" "$RELEASE_TAG"
elif [[ ! -f "$ROOT/Makefile" ]]; then
    say "$ROOT has no checkout — it will be cloned"
    ask REPO_URL "git repository to clone" "$REPO_URL"
    ask BRANCH   "branch"                  "$BRANCH"
else
    ask BRANCH   "git branch to deploy (blank = leave the working tree alone)" "$BRANCH"
fi

echo
echo "${BOLD}-- pool mode --${OFF}"
ask_choice MODE "how does this pool pay miners?" \
    "solo|coinbase pays the miner directly; username = BTC address" \
    "pps-classic|coinbase pays the pool; miners accrue and are paid in Thunder"

echo
echo "${BOLD}-- stratum + bitcoind --${OFF}"
ask STRATUM_PORT  "stratum listen port"                  "$STRATUM_PORT"
ask BITCOIND_URL  "bitcoind RPC url"                     "${BITCOIND_URL:-http://127.0.0.1:8332}"
ask BITCOIND_USER "bitcoind rpcuser (blank = no auth)"   "$BITCOIND_USER"
ask_secret BITCOIND_PASS "bitcoind rpcpassword"
ask OPERATOR_ADDRESS "operator BTC address (receives the fee cut)" "$OPERATOR_ADDRESS"
ask FEE_BPS       "fee in basis points (100 = 1%, max 1000)" "$FEE_BPS"
ask COINBASE_TAG  "coinbase tag"                          "$COINBASE_TAG"

case "$MODE" in
    pps-classic)
        ask POOL_BTC_ADDRESS  "pool BTC address (coinbase pays here)"        "$POOL_BTC_ADDRESS"
        ask THUNDER_ADDRESS   "pool Thunder reserve address (base58)"        "$THUNDER_ADDRESS"
        # Blank means the proxy derives the rate per template from
        # coinbasevalue, network difficulty and fee_bps. A fixed value goes
        # stale as difficulty moves and bypasses fee_bps entirely, so the
        # default is deliberately empty.
        ask PPS_SATS_PER_DIFF "sats/diff override (blank = derive from template, recommended)" "$PPS_SATS_PER_DIFF"
        ;;
esac

echo
echo "${BOLD}-- dashboard --${OFF}"
ask_yn DO_DASH "install the web dashboard (public stats + /admin)?"
if [[ "$DO_DASH" == "1" ]]; then
    ask FQDN      "public domain name for the dashboard (blank = IP only)" "$FQDN"
    ask DASH_PORT "dashboard port (loopback / behind nginx)"               "$DASH_PORT"
    DEFAULT_STRATUM_URL="stratum+tcp://${FQDN:-<pool-host>}:${STRATUM_PORT}"
    # If last run's answer was just the derived default and the domain or
    # port has since changed, let the new default win; a hand-written URL
    # (miners pointed at a different host) is left alone.
    if [[ -n "$PREV_FQDN$PREV_STRATUM_PORT" && \
          "$PUBLIC_STRATUM_URL" == "stratum+tcp://${PREV_FQDN:-<pool-host>}:${PREV_STRATUM_PORT}" && \
          "$PUBLIC_STRATUM_URL" != "$DEFAULT_STRATUM_URL" ]]; then
        PUBLIC_STRATUM_URL=""
    fi
    ask PUBLIC_STRATUM_URL "stratum URL shown on the 'Connect a miner' card" "$DEFAULT_STRATUM_URL"
    ask ADMIN_USER "admin username for /admin (HTTP Basic)" "$ADMIN_USER"
    if [[ -z "$ADMIN_PASSWORD" ]]; then
        ask_secret ADMIN_PASSWORD "admin password (blank = generate a strong one)"
    else
        ask_secret ADMIN_PASSWORD "admin password"
    fi
    if [[ -z "$ADMIN_PASSWORD" ]]; then
        ADMIN_PASSWORD="$(openssl rand -base64 21 | tr -d '=+/')"
        ADMIN_PASSWORD_GENERATED=1
    fi
    # HTTP Basic auth uses ':' to separate the two, and so does the
    # credentials file — the dashboard refuses to boot if either has one.
    if [[ "$ADMIN_USER" == *:* || "$ADMIN_PASSWORD" == *:* ]]; then
        die "admin user/password must not contain ':'"
    fi

    if [[ -n "$FQDN" ]]; then
        ask_yn DO_NGINX "configure nginx as a reverse proxy for $FQDN?"
    else
        DO_NGINX=0
        warn "no domain name given — skipping nginx; dashboard will be on :$DASH_PORT directly"
    fi
    if [[ "$DO_NGINX" == "1" ]]; then
        ask_yn DO_TLS "request a Let's Encrypt certificate with certbot?"
        [[ "$DO_TLS" == "1" ]] && ask EMAIL "email for Let's Encrypt notices" "$EMAIL"
        [[ "$DO_TLS" == "1" && -z "$EMAIL" ]] && { warn "no email — skipping certbot"; DO_TLS=0; }
    fi
else
    DO_NGINX=0; DO_TLS=0
fi

echo
echo "${BOLD}-- payout worker + host --${OFF}"
if [[ "$MODE" == "solo" ]]; then
    DO_PAYOUT=0
    say "solo mode — no payout worker needed (miners are paid in the coinbase)"
else
    [[ -z "$DO_PAYOUT" ]] && DO_PAYOUT=1
    ask_yn DO_PAYOUT "install the Thunder payout worker?"
    if [[ "$DO_PAYOUT" == "1" ]]; then
        ask THUNDER_RPC_URL "Thunder RPC url" "$THUNDER_RPC_URL"
        # This is the batch cadence miners experience. It does not affect how
        # quickly an already-broadcast payout is confirmed and credited —
        # that runs on its own 30s clock inside the worker.
        ask PAYOUT_INTERVAL_HOURS "how often should payouts run, in hours" "$PAYOUT_INTERVAL_HOURS"
    fi
fi
[[ -z "$DO_PAYOUT" ]] && DO_PAYOUT=0

ask_yn DO_DEPS   "install system packages (build tools, node 20, sqlite, nginx)?"
ask_yn RUN_TESTS "run the C test suite after building?"
if command -v ufw >/dev/null 2>&1; then
    ask_yn DO_UFW "add ufw rules (OpenSSH, 80, 443, $STRATUM_PORT)?"
    if [[ "$DO_UFW" == "1" ]] && ! ufw status 2>/dev/null | grep -q 'Status: active'; then
        ask_yn ENABLE_UFW "ufw is inactive — enable it? (OpenSSH is allowed first)"
    fi
else
    DO_UFW=0
fi

# ---------------------------------------------------------------- derived ---
DB_PATH="$ROOT/data/shares.db"
[[ "$MODE" == "pps" ]] && die "--mode pps was removed (the enforcer never credited coinbase deposits) — use pps-classic"
[[ "$MODE" =~ ^(solo|pps-classic)$ ]] || die "invalid --mode: $MODE"
[[ "$STRATUM_PORT" =~ ^[0-9]+$ ]] || die "invalid stratum port: $STRATUM_PORT"
[[ "$DASH_PORT"    =~ ^[0-9]+$ ]] || die "invalid dashboard port: $DASH_PORT"
[[ "$FEE_BPS" =~ ^[0-9]+$ && "$FEE_BPS" -le 1000 ]] || die "fee_bps must be 0..1000"
[[ "$MODE" == "pps-classic" && -z "$POOL_BTC_ADDRESS" ]] && \
    warn "pps-classic without pool_btc_address — the proxy will refuse to start"

[[ "$SOURCE" =~ ^(release|source)$ ]] || die "invalid source: $SOURCE (release|source)"
[[ "$PAYOUT_INTERVAL_HOURS" =~ ^[0-9]+$ && "$PAYOUT_INTERVAL_HOURS" -ge 1 ]] || \
    die "--payout-interval-hours must be a whole number of hours >= 1 (got: $PAYOUT_INTERVAL_HOURS)"
PAYOUT_INTERVAL_MS=$(( PAYOUT_INTERVAL_HOURS * 3600 * 1000 ))

STEP_TOTAL=11
# Both source-selection paths cost exactly one step (download, or clone) plus
# one more (verify the prebuilt binary, or build it), so the base is the same
# either way. The C test suite is the only extra, and it needs a source tree
# to build from.
[[ "$RUN_TESTS" == "1" && "$SOURCE" == "source" ]] && STEP_TOTAL=$((STEP_TOTAL + 1))
[[ "$DO_NGINX"  == "1" ]] && STEP_TOTAL=$((STEP_TOTAL + 1))
[[ "$DO_UFW"    == "1" ]] && STEP_TOTAL=$((STEP_TOTAL + 1))

# ---------------------------------------------------------------- confirm ---
echo
echo "${BOLD}-- summary --${OFF}"
printf "    %-22s %s\n" "install dir"     "$ROOT"
printf "    %-22s %s\n" "service user"    "$SVC_USER"
if [[ "$SOURCE" == "release" ]]; then
    printf "    %-22s %s\n" "source"     "release ${RELEASE_TAG:-<latest>} from ${REPO_SLUG}"
else
    printf "    %-22s %s\n" "source"     "build from ${BRANCH:-<working tree untouched>}"
fi
printf "    %-22s %s\n" "pool mode"       "$MODE"
printf "    %-22s %s\n" "stratum"         "0.0.0.0:$STRATUM_PORT"
printf "    %-22s %s\n" "bitcoind"        "$BITCOIND_URL"
printf "    %-22s %s\n" "operator address" "${OPERATOR_ADDRESS:-${RED}UNSET${OFF}}"
printf "    %-22s %s\n" "fee"             "${FEE_BPS} bps"
[[ -n "$POOL_BTC_ADDRESS" ]] && printf "    %-22s %s\n" "pool BTC address" "$POOL_BTC_ADDRESS"
[[ -n "$THUNDER_ADDRESS"  ]] && printf "    %-22s %s\n" "thunder reserve"  "$THUNDER_ADDRESS"
printf "    %-22s %s\n" "database"        "$DB_PATH"
printf "    %-22s %s\n" "dashboard"       "$([[ $DO_DASH == 1 ]] && echo "yes (:$DASH_PORT${FQDN:+, $FQDN})" || echo no)"
printf "    %-22s %s\n" "nginx / TLS"     "$([[ $DO_NGINX == 1 ]] && echo "yes$([[ $DO_TLS == 1 ]] && echo ' + certbot')" || echo no)"
printf "    %-22s %s\n" "payout worker"   "$([[ $DO_PAYOUT == 1 ]] && echo "yes ($THUNDER_RPC_URL, every ${PAYOUT_INTERVAL_HOURS}h)" || echo no)"
printf "    %-22s %s\n" "firewall"        "$([[ $DO_UFW == 1 ]] && echo "rules$([[ $ENABLE_UFW == 1 ]] && echo ' + enable')" || echo skip)"
echo

if [[ "$ASSUME_YES" != "1" ]]; then
    CONFIRM=""
    read -r -p "    proceed? (y/n) [y]: " CONFIRM </dev/tty || true
    [[ -z "$CONFIRM" || "$CONFIRM" =~ ^[Yy] ]] || { echo "aborted"; exit 1; }
fi

# ------------------------------------------------------------ save answers --
# The directory has to be traversable by the service user (it reads the
# admin credentials file from here); the secrets are protected per-file.
install -d -m 0755 "$STATE_DIR"
{
    echo "# simplepool install answers — written by scripts/install.sh"
    echo "# contains secrets; root-only, 0600."
    # simplepoolctl reads this file too — DB_PATH, SOURCE and RELEASE_TAG are
    # here so it can report and upgrade without re-deriving any of them.
    for v in ROOT SVC_USER REPO_SLUG REPO_URL BRANCH SOURCE RELEASE_TAG \
             MODE STRATUM_PORT BITCOIND_URL \
             BITCOIND_USER BITCOIND_PASS OPERATOR_ADDRESS FEE_BPS COINBASE_TAG \
             POOL_BTC_ADDRESS THUNDER_ADDRESS THUNDER_RPC_URL PPS_SATS_PER_DIFF \
             PAYOUT_INTERVAL_HOURS DB_PATH \
             FQDN DASH_PORT PUBLIC_STRATUM_URL ADMIN_USER ADMIN_PASSWORD EMAIL \
             DO_TLS DO_DASH DO_PAYOUT DO_NGINX DO_UFW DO_DEPS RUN_TESTS; do
        printf '%s=%q\n' "$v" "${!v-}"
    done
} > "$STATE_FILE"
chmod 0600 "$STATE_FILE"

# ---------------------------------------------------------------- helpers ---
# Set once the service user exists (step 3); npm and git both need a real
# HOME, and a --system user's may be unset. The npm cache lives outside the
# checkout so it never shows up as untracked cruft in `git status`.
SVC_HOME="$ROOT"
NPM_CACHE=/var/cache/simplepool/npm
if command -v runuser >/dev/null 2>&1; then
    as_user() { runuser -u "$SVC_USER" -- env "HOME=$SVC_HOME" "npm_config_cache=$NPM_CACHE" "$@"; }
elif command -v sudo >/dev/null 2>&1; then
    as_user() { sudo -u "$SVC_USER" env "HOME=$SVC_HOME" "npm_config_cache=$NPM_CACHE" "$@"; }
else
    die "need runuser or sudo to drop privileges to $SVC_USER"
fi

# Set `key = value` in a config file: replaces the first live *or commented*
# occurrence, appends if the key isn't there at all. Value goes through the
# environment so slashes/backslashes in passwords survive.
conf_set() {
    local file="$1" key="$2" val="$3" tmp
    tmp="$(mktemp)"
    CONF_VAL="$val" awk -v key="$key" '
        BEGIN { done = 0 }
        {
            probe = $0
            sub(/^[ \t]*#[ \t]*/, "", probe)
            eq = index(probe, "=")
            if (!done && eq > 0) {
                k = substr(probe, 1, eq - 1)
                gsub(/^[ \t]+|[ \t]+$/, "", k)
                if (k == key) { print key " = " ENVIRON["CONF_VAL"]; done = 1; next }
            }
            print
        }
        END { if (!done) print key " = " ENVIRON["CONF_VAL"] }
    ' "$file" > "$tmp"
    mv "$tmp" "$file"
}

# Comment a key out (used to drop mode-specific keys that no longer apply).
conf_unset() {
    local file="$1" key="$2" tmp
    tmp="$(mktemp)"
    awk -v key="$key" '
        {
            probe = $0; eq = index(probe, "=")
            if (eq > 0 && probe !~ /^[ \t]*#/) {
                k = substr(probe, 1, eq - 1); gsub(/^[ \t]+|[ \t]+$/, "", k)
                if (k == key) { print "# " $0; next }
            }
            print
        }
    ' "$file" > "$tmp"
    mv "$tmp" "$file"
}

PKG=""
command -v apt-get >/dev/null 2>&1 && PKG=apt
command -v dnf     >/dev/null 2>&1 && PKG="${PKG:-dnf}"

# =========================== 1. system packages =============================
step "system packages"
if [[ "$DO_DEPS" == "1" ]]; then
    case "$PKG" in
        apt)
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -q
            # A release install skips the compiler and git, but still needs
            # the shared libraries the binary links against. It asks for the
            # -dev packages rather than the runtime ones because those are
            # the names that stay put across distro releases: libhiredis-dev
            # is libhiredis-dev everywhere, while the runtime package is
            # libhiredis0.14 on 22.04 and libhiredis1.1.0 on 24.04.
            PKGS=(curl ca-certificates tar gzip python3 openssl ufw sqlite3
                  libsqlite3-dev libcurl4-openssl-dev libhiredis-dev)
            [[ "$SOURCE" == "source" ]] && PKGS+=(build-essential git unzip)
            apt-get install -yq "${PKGS[@]}"
            [[ "$DO_NGINX" == "1" ]] && apt-get install -yq nginx
            [[ "$DO_TLS"   == "1" ]] && apt-get install -yq certbot python3-certbot-nginx
            ;;
        dnf)
            PKGS=(curl ca-certificates tar gzip python3 openssl sqlite
                  sqlite-devel libcurl-devel hiredis-devel)
            [[ "$SOURCE" == "source" ]] && PKGS+=(gcc gcc-c++ make git unzip)
            dnf install -y "${PKGS[@]}"
            [[ "$DO_NGINX" == "1" ]] && dnf install -y nginx
            [[ "$DO_TLS"   == "1" ]] && dnf install -y certbot python3-certbot-nginx
            ;;
        *) die "no apt-get or dnf found — install the deps listed under 'Prerequisites' in INSTALL.md, then re-run with --no-deps" ;;
    esac
else
    say "skipped (--no-deps)"
fi

# ============================== 2. node 20 ==================================
step "node.js >= 20"
NODE_MAJOR=0
command -v node >/dev/null 2>&1 && NODE_MAJOR="$(node -v | sed 's/^v\([0-9]*\).*/\1/')"
if (( NODE_MAJOR < 20 )); then
    if [[ "$DO_DEPS" == "1" ]]; then
        say "installing Node.js 20 from NodeSource (found: ${NODE_MAJOR:-none})"
        case "$PKG" in
            apt) curl -fsSL https://deb.nodesource.com/setup_20.x | bash - && apt-get install -yq nodejs ;;
            dnf) curl -fsSL https://rpm.nodesource.com/setup_20.x | bash - && dnf install -y nodejs ;;
        esac
    else
        die "node >= 20 required (found ${NODE_MAJOR:-none}); drop --no-deps or install it yourself"
    fi
fi
say "node $(node -v), npm $(npm -v)"
# No extra tooling needed for the /admin deposit action — the dashboard
# talks to the enforcer over plain HTTP (ConnectRPC).

# ============================ 3. service user ===============================
step "service user '$SVC_USER'"
if id -u "$SVC_USER" >/dev/null 2>&1; then
    say "already exists (uid $(id -u "$SVC_USER"))"
else
    HOME_DIR="$ROOT"
    [[ "$ROOT" == /home/* ]] || HOME_DIR="/home/$SVC_USER"
    if command -v adduser >/dev/null 2>&1 && adduser --help 2>&1 | grep -q -- --system; then
        adduser --system --group --home "$HOME_DIR" --shell /bin/bash "$SVC_USER"
    else
        useradd --system --create-home --home-dir "$HOME_DIR" --shell /bin/bash --user-group "$SVC_USER"
    fi
    say "created (home: $HOME_DIR)"
fi
SVC_HOME="$(getent passwd "$SVC_USER" | cut -d: -f6)"
[[ -d "$SVC_HOME" ]] || SVC_HOME="$ROOT"

# ============================ 4. get the code ===============================
# Which prebuilt tarball this machine can run.
release_arch() {
    local m
    m="$(dpkg --print-architecture 2>/dev/null || uname -m)"
    case "$m" in
        amd64|x86_64)  echo amd64 ;;
        arm64|aarch64) echo arm64 ;;
        *) die "no published build for $m — re-run with --from-source to compile here" ;;
    esac
}

# Ask GitHub what the newest release is. Kept separate from the download so a
# tag the operator typed is never silently replaced by a different one.
latest_release_tag() {
    curl -fsSL --max-time 20 "https://api.github.com/repos/${REPO_SLUG}/releases/latest" 2>/dev/null \
        | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1
}

fetch_release() {
    local arch tag ver base tmp tarball
    arch="$(release_arch)"

    tag="$RELEASE_TAG"
    if [[ -z "$tag" ]]; then
        say "asking github for the latest release of ${REPO_SLUG}"
        tag="$(latest_release_tag || true)"
        [[ -n "$tag" ]] || die "could not find a published release for ${REPO_SLUG}.
    If none has been cut yet, build from source instead:  --from-source"
    fi
    RELEASE_TAG="$tag"
    ver="${tag#v}"
    base="https://github.com/${REPO_SLUG}/releases/download/${tag}"
    tarball="simplepool-${ver}-linux-${arch}.tar.gz"

    tmp="$(mktemp -d)"
    # A partial download must not be left behind looking like a good one.
    # EXIT rather than RETURN, because die() exits outright and a RETURN trap
    # would never fire. The path is expanded NOW, not at exit: $tmp is local
    # to this function and would be empty by the time the trap runs, making
    # the cleanup `rm -rf ""` — which fails, and would take the script's exit
    # status down with it on an otherwise successful install.
    trap "rm -rf '$tmp'" EXIT

    say "downloading $tarball ($tag)"
    curl -fL --progress-bar --max-time 600 -o "$tmp/$tarball" "$base/$tarball" \
        || die "could not download $base/$tarball
    Check that release $tag has a build for $arch, or use --from-source."

    # The checksum is not decoration: this binary is about to run as a service
    # with the pool's payout addresses in its config, and it arrived over the
    # network. A release without SHA256SUMS is treated as a failure, not as a
    # reason to skip the check.
    say "verifying against SHA256SUMS"
    curl -fsSL --max-time 60 -o "$tmp/SHA256SUMS" "$base/SHA256SUMS" \
        || die "release $tag has no SHA256SUMS — refusing to install an unverified binary"
    grep -q " \{1,2\}${tarball}\$" "$tmp/SHA256SUMS" \
        || die "SHA256SUMS for $tag does not mention $tarball — refusing to install"
    ( cd "$tmp" && sha256sum -c --ignore-missing SHA256SUMS ) \
        || die "checksum mismatch on $tarball — the download is corrupt or tampered with"

    tar -xzf "$tmp/$tarball" -C "$tmp"
    local unpacked
    unpacked="$tmp/simplepool-${ver}-linux-${arch}"
    [[ -f "$unpacked/Makefile" && -x "$unpacked/build/simplepool" ]] \
        || die "$tarball did not unpack into the expected layout"

    install -d -o "$SVC_USER" -g "$SVC_USER" "$ROOT"
    # Copy rather than replace: data/, proxy.conf and the *.bak files are not
    # in the tarball (they are gitignored), so an overlay upgrades the code
    # and leaves the ledger and the operator's config exactly where they are.
    cp -a "$unpacked/." "$ROOT/"
    say "unpacked $tag into $ROOT"
    sed 's/^/    /' "$ROOT/RELEASE" 2>/dev/null || true
}

if [[ "$SOURCE" == "release" ]]; then
    step "download release"
    # Overlaying a tarball onto a checkout produces a tree that is neither:
    # git reports every file as modified and the next `git pull` fights the
    # release. Say so instead of creating it.
    [[ -d "$ROOT/.git" ]] && die "$ROOT is a git checkout, and a release tarball would be laid on top of it.
    Pick one:  --from-source   (build this checkout)
               --root <dir>    (install the release somewhere else)"
    fetch_release
else
    step "source code in $ROOT"
    # git refuses to operate on a tree owned by another user ("dubious
    # ownership"), so take ownership before touching it — a previous run as
    # root may well have left root-owned objects here.
    [[ -d "$ROOT" ]] && chown -R "$SVC_USER:$SVC_USER" "$ROOT"
    if [[ -d "$ROOT/.git" ]]; then
        if [[ -n "$BRANCH" ]]; then
            say "fetching origin/$BRANCH"
            as_user git -C "$ROOT" fetch origin --prune
            as_user git -C "$ROOT" checkout "$BRANCH"
            as_user git -C "$ROOT" reset --hard "origin/$BRANCH"
        else
            say "working tree left as-is"
        fi
    elif [[ -f "$ROOT/Makefile" ]]; then
        say "non-git checkout — left as-is"
    else
        install -d -o "$SVC_USER" -g "$SVC_USER" "$ROOT"
        say "cloning $REPO_URL ($BRANCH)"
        as_user git clone --branch "$BRANCH" "$REPO_URL" "$ROOT"
    fi
fi
[[ -f "$ROOT/Makefile" && -f "$ROOT/schema.sql" ]] || die "$ROOT does not look like a simplepool tree"
# An earlier root-run build leaves root-owned objects behind; fix the tree.
chown -R "$SVC_USER:$SVC_USER" "$ROOT"

# ============================== 5. the binary ===============================
if [[ "$SOURCE" == "release" ]]; then
    step "check the prebuilt binary runs here"
    # The one thing a prebuilt binary can fail at on a specific box is
    # dynamic linking — a too-old glibc, or a missing libhiredis. Finding
    # that out now beats finding it out from a crash-looping unit.
    if ! "$ROOT/build/simplepool" --version >/dev/null 2>&1; then
        warn "$("$ROOT/build/simplepool" --version 2>&1 | head -3)"
        # `|| true`: grep finding nothing must not pre-empt the die() below,
        # which is the message that actually tells the operator what to do.
        [[ -n "$(command -v ldd)" ]] && { ldd "$ROOT/build/simplepool" 2>&1 | grep -i "not found" | sed 's/^/    /' || true; }
        die "the prebuilt binary will not run on this machine.
    Usually an unsatisfied shared library or a glibc older than the build host's.
    Re-run with --from-source to compile against this system instead."
    fi
    say "$("$ROOT/build/simplepool" --version 2>/dev/null | head -1)"
else
    step "build the C proxy"
    as_user make -C "$ROOT" -j"$(nproc)"
    [[ -x "$ROOT/build/simplepool" ]] || die "build produced no binary at $ROOT/build/simplepool"
    say "built $ROOT/build/simplepool ($(stat -c %s "$ROOT/build/simplepool") bytes)"
    # A source install has no RELEASE file, and a stale one left over from a
    # previous release install would misreport what is running.
    rm -f "$ROOT/RELEASE"

    if [[ "$RUN_TESTS" == "1" ]]; then
        step "C test suite"
        as_user make -C "$ROOT" test
    fi
fi

# =========================== 6. node modules ================================
step "node dependencies"
install -d -o "$SVC_USER" -g "$SVC_USER" -m 0755 "$NPM_CACHE"
npm_install_in() { # npm_install_in <dir>
    local d="$1"
    [[ -d "$d" ]] || { warn "$d missing — skipped"; return; }
    if [[ -f "$d/package-lock.json" ]]; then
        as_user bash -c 'cd "$1" && npm ci --omit=dev --no-fund --no-audit' _ "$d"
    else
        as_user bash -c 'cd "$1" && npm install --omit=dev --no-fund --no-audit' _ "$d"
    fi
    # better-sqlite3 ships a prebuilt binding per Node ABI; if the one we
    # got doesn't match this Node, recompile from source.
    printf "require('better-sqlite3');\n" > "$d/.abi-check.cjs"
    if ! as_user node "$d/.abi-check.cjs" >/dev/null 2>&1; then
        say "better-sqlite3 ABI mismatch in $(basename "$d") — rebuilding from source"
        as_user bash -c 'cd "$1" && npm rebuild better-sqlite3 --build-from-source' _ "$d"
        as_user node "$d/.abi-check.cjs" >/dev/null 2>&1 || die "better-sqlite3 still broken in $d"
    fi
    rm -f "$d/.abi-check.cjs"
}
[[ "$DO_DASH"   == "1" ]] && npm_install_in "$ROOT/dashboard"
[[ "$DO_PAYOUT" == "1" ]] && npm_install_in "$ROOT/payout"
[[ "$DO_DASH" == "0" && "$DO_PAYOUT" == "0" ]] && say "nothing to install"

# ============================== 7. database =================================
step "sqlite database"
install -d -o "$SVC_USER" -g "$SVC_USER" -m 0755 "$ROOT/data"
as_user sqlite3 "$DB_PATH" < "$ROOT/schema.sql"
# Older checkouts kept the deposits table in a separate file.
[[ -f "$ROOT/deploy/schema/deposits.sql" ]] && as_user sqlite3 "$DB_PATH" < "$ROOT/deploy/schema/deposits.sql"
# Idempotent ms -> seconds fix for databases written by pre-2025 builds.
as_user sqlite3 "$DB_PATH" <<'SQL'
UPDATE workers      SET first_seen = first_seen/1000 WHERE first_seen > 10000000000;
UPDATE workers      SET last_seen  = last_seen/1000  WHERE last_seen  > 10000000000;
UPDATE shares       SET ts = ts/1000 WHERE ts > 10000000000;
UPDATE rejects      SET ts = ts/1000 WHERE ts > 10000000000;
UPDATE blocks_found SET ts = ts/1000 WHERE ts > 10000000000;
SQL
chown -R "$SVC_USER:$SVC_USER" "$ROOT/data"
chmod 0640 "$DB_PATH"
say "$(as_user sqlite3 "$DB_PATH" "SELECT count(*) FROM sqlite_master WHERE type='table';") tables at $DB_PATH"

# ============================= 8. proxy.conf ================================
step "proxy.conf"
TMP_CONF="$(mktemp)"
if [[ -f "$ROOT/proxy.conf" ]]; then
    cp "$ROOT/proxy.conf" "$TMP_CONF"
    install -o "$SVC_USER" -g "$SVC_USER" -m 0640 "$ROOT/proxy.conf" \
            "$ROOT/proxy.conf.bak.$(date +%Y%m%d%H%M%S)"
    say "existing proxy.conf backed up; managed keys updated in place"
else
    cp "$ROOT/proxy.conf.example" "$TMP_CONF"
fi

conf_set "$TMP_CONF" listen_addr      "0.0.0.0"
conf_set "$TMP_CONF" listen_port      "$STRATUM_PORT"
conf_set "$TMP_CONF" bitcoind_url     "$BITCOIND_URL"
if [[ -n "$BITCOIND_USER" ]]; then
    conf_set "$TMP_CONF" bitcoind_user "$BITCOIND_USER"
    conf_set "$TMP_CONF" bitcoind_pass "$BITCOIND_PASS"
else
    # No credentials: the RPC call must go out without a basic-auth header.
    conf_unset "$TMP_CONF" bitcoind_user
    conf_unset "$TMP_CONF" bitcoind_pass
fi
conf_set "$TMP_CONF" operator_address "$OPERATOR_ADDRESS"
conf_set "$TMP_CONF" fee_bps          "$FEE_BPS"
conf_set "$TMP_CONF" coinbase_tag     "$COINBASE_TAG"
conf_set "$TMP_CONF" pool_mode        "$MODE"
conf_set "$TMP_CONF" db_path          "$DB_PATH"

# The Thunder reserve address is a dashboard/payout-worker concern, not a
# proxy one — the coinbase never touches Thunder. Strip the retired keys
# from any proxy.conf left over from a pool_mode=pps install.
conf_unset "$TMP_CONF" pool_thunder_reserve_address
conf_unset "$TMP_CONF" thunder_sidechain_number
conf_unset "$TMP_CONF" thunder_op_return_hex

case "$MODE" in
    solo)
        conf_unset "$TMP_CONF" pool_btc_address
        ;;
    pps-classic)
        conf_set "$TMP_CONF" pool_btc_address  "$POOL_BTC_ADDRESS"
        if [[ -n "$PPS_SATS_PER_DIFF" ]]; then
            conf_set   "$TMP_CONF" pps_sats_per_diff "$PPS_SATS_PER_DIFF"
        else
            conf_unset "$TMP_CONF" pps_sats_per_diff
        fi
        ;;
esac

install -o "$SVC_USER" -g "$SVC_USER" -m 0640 "$TMP_CONF" "$ROOT/proxy.conf"
rm -f "$TMP_CONF"
say "wrote $ROOT/proxy.conf (mode=$MODE)"

CONF_OK=1
if [[ -z "$OPERATOR_ADDRESS" || "$OPERATOR_ADDRESS" == *REPLACEME* ]]; then
    warn "operator_address is unset — the proxy will refuse to start"
    CONF_OK=0
fi
if [[ "$MODE" == "pps-classic" && ( -z "$POOL_BTC_ADDRESS" || "$POOL_BTC_ADDRESS" == *REPLACE* ) ]]; then
    warn "pool_btc_address is unset — the proxy will refuse to start"; CONF_OK=0
fi
if [[ "$MODE" == "pps-classic" && -z "$THUNDER_ADDRESS" ]]; then
    warn "no Thunder reserve address — the dashboard cannot deposit and the payout worker cannot pay"
fi

# ===================== 9. systemd units + drop-ins ==========================
step "systemd units"
# Newer dashboards read the admin credentials from a "user:password" file,
# which keeps them out of the unit and out of the process environment.
# Older checkouts only understand ADMIN_USER + ADMIN_PASSWORD.
CRED_FILE="$STATE_DIR/admin.cred"
ADMIN_CRED_FILE_SUPPORTED=0
if grep -q 'ADMIN_CREDENTIALS_FILE' "$ROOT/dashboard/server.js" 2>/dev/null; then
    ADMIN_CRED_FILE_SUPPORTED=1
fi
render_unit() { # render_unit <name>
    sed -e "s|@USER@|${SVC_USER}|g" -e "s|@ROOT@|${ROOT}|g" \
        "$ROOT/deploy/systemd/$1" > "/etc/systemd/system/$1"
    chmod 0644 "/etc/systemd/system/$1"
}
render_unit simplepool.service
[[ "$DO_DASH"   == "1" ]] && render_unit simplepool-dashboard.service
[[ "$DO_PAYOUT" == "1" ]] && render_unit simplepool-payout.service

# Deployment-specific values live in drop-ins so the shipped templates stay
# pristine and a future `git pull` can update them.
if [[ "$DO_DASH" == "1" ]]; then
    install -d -m 0755 /etc/systemd/system/simplepool-dashboard.service.d
    DROPIN=/etc/systemd/system/simplepool-dashboard.service.d/local.conf
    {
        echo "# Generated by scripts/install.sh — edit here, not in the unit."
        echo "[Service]"
        echo "Environment=PORT=${DASH_PORT}"
        echo "Environment=PROXY_DB_PATH=${DB_PATH}"
        echo "Environment=PUBLIC_STRATUM_URL=${PUBLIC_STRATUM_URL}"
        if [[ "$ADMIN_CRED_FILE_SUPPORTED" == "1" ]]; then
            # Keeps the password out of the unit file and out of
            # `systemctl show` / the process environment.
            echo "Environment=ADMIN_CREDENTIALS_FILE=${CRED_FILE}"
        else
            echo "Environment=ADMIN_USER=${ADMIN_USER}"
            echo "Environment=ADMIN_PASSWORD=${ADMIN_PASSWORD}"
        fi
        # No POOL_PPS_SATS_PER_DIFF: the dashboard reads the effective rate
        # from pool_meta, which the proxy writes, so the audit cannot drift
        # from the ledger it checks.
        [[ -n "$THUNDER_ADDRESS" ]] && echo "Environment=POOL_THUNDER_RESERVE_ADDRESS=${THUNDER_ADDRESS}"
        echo "Environment=THUNDER_RPC_URL=${THUNDER_RPC_URL}"
        if [[ "$DO_PAYOUT" == "1" ]]; then
            echo "Environment=PAYOUT_ADMIN_URL=http://127.0.0.1:9080"
        else
            echo "Environment=PAYOUT_ADMIN_URL="
        fi
        echo "ReadWritePaths=${ROOT}/data"
    } > "$DROPIN"
    if [[ "$ADMIN_CRED_FILE_SUPPORTED" == "1" ]]; then
        chmod 0644 "$DROPIN"
        say "dashboard drop-in: $DROPIN"
    else
        chmod 0600 "$DROPIN"   # this checkout has no credentials-file support
        say "dashboard drop-in: $DROPIN (0600 — contains the admin password)"
    fi
fi

if [[ "$DO_PAYOUT" == "1" ]]; then
    install -d -m 0755 /etc/systemd/system/simplepool-payout.service.d
    PDROPIN=/etc/systemd/system/simplepool-payout.service.d/local.conf
    {
        echo "# Generated by scripts/install.sh"
        echo "[Service]"
        echo "Environment=PAYOUT_DB_PATH=${DB_PATH}"
        echo "Environment=THUNDER_RPC_URL=${THUNDER_RPC_URL}"
        echo "Environment=THUNDER_FROM_ADDRESS=${THUNDER_ADDRESS}"
        echo "# Payout runs are a batch: every ${PAYOUT_INTERVAL_HOURS}h everyone over"
        echo "# PAYOUT_MIN_SATS goes out in one Thunder transaction."
        echo "Environment=PAYOUT_INTERVAL_MS=${PAYOUT_INTERVAL_MS}"
        echo "# Settlement is a separate, much shorter clock on purpose: nobody in a"
        echo "# broadcast batch is credited until a tick sees it confirmed in a Thunder"
        echo "# block, so this must not follow the cadence above. Left at the worker's"
        echo "# defaults (30s / 5m) unless you have a reason to move them."
    } > "$PDROPIN"
    chmod 0644 "$PDROPIN"
    if [[ -z "$THUNDER_ADDRESS" ]]; then
        warn "THUNDER_FROM_ADDRESS is empty — the payout worker will exit on start"
    fi
fi

systemctl daemon-reload

# The credentials file doubles as the operator's copy of the password.
# The dashboard treats a set-but-unreadable file as a fatal boot error, so
# it has to be owned by the service user.
if [[ "$DO_DASH" == "1" ]]; then
    printf '%s:%s\n' "$ADMIN_USER" "$ADMIN_PASSWORD" > "$CRED_FILE"
    chown "$SVC_USER:$SVC_USER" "$CRED_FILE"
    chmod 0400 "$CRED_FILE"
fi

# ============================== 10. nginx ===================================
if [[ "$DO_NGINX" == "1" ]]; then
    step "nginx vhost for $FQDN"
    VHOST="/etc/nginx/sites-available/${FQDN}"
    # certbot edits this file in place to add the :443 server block —
    # regenerating it from the template would silently drop TLS.
    if [[ -f "$VHOST" ]] && grep -q 'managed by Certbot' "$VHOST"; then
        say "$VHOST is certbot-managed — left untouched"
    else
        sed -e "s/pool\.drivechain\.info/${FQDN}/g" \
            -e "s|127\.0\.0\.1:8081|127.0.0.1:${DASH_PORT}|g" \
            "$ROOT/deploy/nginx/pool.drivechain.info.conf" > "$VHOST"
        chmod 0644 "$VHOST"
    fi
    install -m 0644 "$ROOT/deploy/nginx/pool-ratelimit.conf" /etc/nginx/conf.d/pool-ratelimit.conf
    install -d /etc/nginx/sites-enabled
    ln -sf "/etc/nginx/sites-available/${FQDN}" "/etc/nginx/sites-enabled/${FQDN}"
    rm -f /etc/nginx/sites-enabled/default
    nginx -t
    systemctl enable --now nginx >/dev/null 2>&1 || true
    systemctl reload nginx
    say "vhost live: http://${FQDN}/"

    if [[ "$DO_TLS" == "1" && -f "/etc/letsencrypt/live/${FQDN}/fullchain.pem" ]]; then
        say "certificate for $FQDN already present — leaving renewal to certbot.timer"
    elif [[ "$DO_TLS" == "1" ]]; then
        say "requesting a certificate for $FQDN"
        if certbot --nginx -d "$FQDN" --non-interactive --agree-tos -m "$EMAIL" --redirect; then
            say "TLS active: https://${FQDN}/"
        else
            warn "certbot failed — is DNS for $FQDN pointing at this box and :80 reachable?"
            warn "re-run later: certbot --nginx -d $FQDN"
        fi
    else
        warn "no TLS — /admin uses HTTP Basic auth, which leaks the password over plain HTTP"
    fi
fi

# ============================= 11. firewall =================================
if [[ "$DO_UFW" == "1" ]]; then
    step "firewall rules"
    ufw allow OpenSSH               >/dev/null 2>&1 || ufw allow 22/tcp >/dev/null 2>&1 || true
    ufw allow "${STRATUM_PORT}/tcp" >/dev/null 2>&1 || true
    if [[ "$DO_NGINX" == "1" ]]; then
        ufw allow 80/tcp  >/dev/null 2>&1 || true
        ufw allow 443/tcp >/dev/null 2>&1 || true
    fi
    if [[ "$ENABLE_UFW" == "1" ]] && ! ufw status | grep -q 'Status: active'; then
        ufw --force enable
    fi
    say "$(ufw status | head -1)  (allowed: OpenSSH, ${STRATUM_PORT}$([[ $DO_NGINX == 1 ]] && echo ', 80, 443'))"
    say "${DASH_PORT}/tcp deliberately NOT opened — nginx fronts the dashboard"
fi

# ========================== 12. simplepoolctl ===============================
step "simplepoolctl"
# The operator's entry point from here on. It reads $STATE_FILE for
# everything about this box, so installing it is a copy, not a configuration.
if [[ -f "$ROOT/scripts/simplepoolctl" ]]; then
    install -m 0755 "$ROOT/scripts/simplepoolctl" /usr/local/bin/simplepoolctl
    say "installed /usr/local/bin/simplepoolctl"
    say "${DIM}status | logs | doctor | upgrade | uninstall${OFF}"
else
    warn "$ROOT/scripts/simplepoolctl not found — skipping (older tree?)"
fi

# ============================ 13. start services ============================
step "enable + start services"
# A unit that crash-loops must not abort the install — the status block
# below is more useful than a bare `set -e` exit.
start_unit() { # start_unit <unit>
    systemctl enable "$1" >/dev/null 2>&1 || warn "could not enable $1"
    systemctl restart "$1" || warn "$1 failed to start — journalctl -u ${1%.service} -n 50"
}
if [[ "$CONF_OK" == "1" ]]; then
    start_unit simplepool.service
else
    systemctl enable simplepool.service >/dev/null 2>&1 || true
    warn "simplepool.service enabled but NOT started — finish $ROOT/proxy.conf first, then:"
    warn "  systemctl start simplepool"
fi
[[ "$DO_DASH" == "1" ]] && start_unit simplepool-dashboard.service
if [[ "$DO_PAYOUT" == "1" && -n "$THUNDER_ADDRESS" ]]; then
    start_unit simplepool-payout.service
elif [[ "$DO_PAYOUT" == "1" ]]; then
    systemctl enable simplepool-payout.service >/dev/null 2>&1 || true
    warn "payout worker enabled but not started (no Thunder address)"
fi

# ============================== verification ================================
sleep 2
echo
echo "${BOLD}-- status --${OFF}"
svc_state() { systemctl is-active "$1" 2>/dev/null || echo inactive; }
report() { # report <label> <unit>
    local s; s="$(svc_state "$2")"
    if [[ "$s" == "active" ]]; then printf "    %-12s ${GRN}%s${OFF}\n" "$1" "$s"
    else printf "    %-12s ${RED}%s${OFF}   ${DIM}journalctl -u %s -n 50${OFF}\n" "$1" "$s" "$2"; fi
}
report "proxy" simplepool.service
[[ "$DO_DASH"   == "1" ]] && report "dashboard" simplepool-dashboard.service
[[ "$DO_PAYOUT" == "1" ]] && report "payout"    simplepool-payout.service
[[ "$DO_NGINX"  == "1" ]] && report "nginx"     nginx.service

if command -v ss >/dev/null 2>&1; then
    echo
    ss -ltnp 2>/dev/null | grep -E ":(${STRATUM_PORT}|${DASH_PORT})\b" | sed 's/^/    /' || \
        warn "neither :${STRATUM_PORT} nor :${DASH_PORT} is listening yet"
fi
if [[ "$DO_DASH" == "1" ]]; then
    if curl -fsS --max-time 5 "http://127.0.0.1:${DASH_PORT}/healthz" >/dev/null 2>&1; then
        say "healthz: ${GRN}ok${OFF}"
    else
        warn "dashboard /healthz did not respond — journalctl -u simplepool-dashboard -n 50"
    fi
fi

# ================================ summary ===================================
echo
echo "${BOLD}-- done --${OFF}"
say "miners connect to:  stratum+tcp://${FQDN:-<this-host>}:${STRATUM_PORT}"
case "$MODE" in
    solo) say "  stratum username:  <miner BTC address>[.<rig>]" ;;
    *)    say "  stratum username:  <miner Thunder address>[.<rig>]" ;;
esac
if [[ "$DO_DASH" == "1" ]]; then
    if [[ "$DO_NGINX" == "1" ]]; then
        say "dashboard:          http$([[ $DO_TLS == 1 ]] && echo s)://${FQDN}/"
        say "admin:              http$([[ $DO_TLS == 1 ]] && echo s)://${FQDN}/admin"
    else
        say "dashboard:          http://<this-host>:${DASH_PORT}/"
    fi
    say "admin user:         ${ADMIN_USER}"
    if [[ "${ADMIN_PASSWORD_GENERATED:-0}" == "1" ]]; then
        echo "    admin password:     ${BOLD}${ADMIN_PASSWORD}${OFF}   ${DIM}(shown once)${OFF}"
    fi
    say "credentials file:   ${CRED_FILE} (${SVC_USER}, 0400)"
fi
say "config:             $ROOT/proxy.conf"
say "database:           $DB_PATH"
if [[ "$SOURCE" == "release" ]]; then
    say "installed:          release ${RELEASE_TAG} (${ROOT}/RELEASE)"
else
    say "installed:          built from source (${BRANCH:-working tree})"
fi
[[ "$DO_PAYOUT" == "1" ]] && say "payouts:            every ${PAYOUT_INTERVAL_HOURS}h"
echo
say "${BOLD}next:${OFF}"
say "  simplepoolctl status        what is running, on which ports"
say "  simplepoolctl doctor        check the things that break in production"
say "  simplepoolctl logs -f       follow every service at once"
say "  simplepoolctl upgrade       move to the next release"
echo
say "re-run this script any time — it reuses your answers from $STATE_FILE"
echo
