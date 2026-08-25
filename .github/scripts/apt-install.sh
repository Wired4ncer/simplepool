#!/usr/bin/env bash
#
# Install apt packages in CI without refreshing the whole package index.
#
# `apt-get update` on a GitHub runner refreshes SIX repositories — the Ubuntu
# archive plus Microsoft, azure-cli, Google and Chrome — none of which carry
# anything simplepool builds against, and it makes the job depend on all of
# them being reachable. On 2026-08-18 the Azure mirror `Ign`'d every entry,
# apt fell back to archive.ubuntu.com, and `build-test` sat on
# `Get:5 .../noble-security InRelease` for 29 minutes until the run was
# cancelled. It never reached package download at all.
#
# The runner image ships current lists for the Ubuntu archive — a plain
# `apt-get update` reports `Hit:` on the base suite — so the index already on
# disk is enough to install from. Packages that are already present (most of
# these, on a GitHub image) cost nothing.
#
# The one case the shipped index cannot serve is a package superseded by a
# security update whose old .deb has left the pool, which 404s. That is the
# only reason the fallback below exists, and every network wait in it is
# bounded so it cannot repeat the stall it was written to prevent.
#
# Usage: .github/scripts/apt-install.sh <pkg>...
#
set -euo pipefail

[ $# -gt 0 ] || { echo "apt-install.sh: no packages given" >&2; exit 2; }

export DEBIAN_FRONTEND=noninteractive

# Bound every fetch: without a timeout a stalled mirror holds the connection
# open until the job's own limit kills it, which is the failure mode here.
APT_OPTS=(
    -o Acquire::Retries=3
    -o Acquire::http::Timeout=20
    -o Acquire::https::Timeout=20
)

apt_install() {
    sudo -E apt-get install -y --no-install-recommends "${APT_OPTS[@]}" "$@"
}

echo "==> installing without an index refresh: $*"
if apt_install "$@"; then
    exit 0
fi

echo "::warning::apt-get install failed against the image's package index." \
     "Refreshing it once, then retrying. If this becomes routine, the runner" \
     "image's lists have drifted and this script's assumption needs revisiting."

# `sudo timeout` rather than `timeout sudo`, so the kill lands on apt-get
# itself instead of on sudo, which may or may not forward the signal.
sudo -E timeout 180 apt-get update "${APT_OPTS[@]}" \
    || echo "::warning::index refresh did not finish in 180s; retrying the install regardless"

apt_install "$@"
