#!/usr/bin/env bash
#
# Build a simplepool release tarball.
#
# The tarball is a git checkout with a prebuilt binary dropped into it —
# not a trimmed-down runtime bundle. That is deliberate: `scripts/install.sh`,
# the systemd templates, the nginx vhost, `schema.sql` and the dashboard all
# already expect a checkout at $ROOT, so shipping the same shape means the
# release path and the build-from-source path converge after one step instead
# of forking into two sets of layout assumptions. The only difference an
# operator can observe is that `build/simplepool` arrived prebuilt and there
# is a RELEASE file next to it.
#
# Contents:
#   RELEASE                       version / commit / arch / build time
#   build/simplepool              the binary, built here
#   build/simplepool.build.json   provenance, pinned to the binary by sha256
#   <everything tracked in git>   src, dashboard, payout, deploy, docs, scripts
#
# Usage:
#   scripts/release.sh                     # version from the Makefile
#   scripts/release.sh --version 0.2.0     # override
#   scripts/release.sh --out /tmp/dist
#   scripts/release.sh --allow-dirty       # build from an uncommitted tree
#
# Writes <out>/simplepool-<version>-linux-<arch>.tar.gz and a matching
# .sha256 file. CI calls this exact script (see .github/workflows/release.yaml)
# so a hand-cut tarball and a released one are byte-for-byte the same recipe.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION=""
OUT="$REPO_ROOT/dist"
ARCH=""
REF="HEAD"
ALLOW_DIRTY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --out)     OUT="$2";     shift 2 ;;
        --arch)    ARCH="$2";    shift 2 ;;
        --ref)     REF="$2";     shift 2 ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        -h|--help) sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "release.sh: unknown arg: $1" >&2; exit 2 ;;
    esac
done

git rev-parse --git-dir >/dev/null 2>&1 || {
    echo "release.sh: not a git checkout — the tarball is assembled with git archive" >&2
    exit 1
}

# The version the binary will report, so the tarball cannot claim one thing
# while `simplepool --version` says another.
[[ -n "$VERSION" ]] || VERSION="$(sed -n 's/^VERSION[[:space:]]*:=[[:space:]]*//p' Makefile | head -1)"
[[ -n "$VERSION" ]] || { echo "release.sh: could not read VERSION from the Makefile" >&2; exit 1; }

# The binary is compiled from the working tree; the source beside it comes
# from `git archive $REF`. On a dirty tree those are two different programs,
# and the tarball would ship a binary that provably does not correspond to the
# source shipped with it — the exact drift build/simplepool.build.json exists
# to make visible. Refuse rather than record it and hope someone reads the
# JSON. CI is always clean, so this only ever fires for a hand-run.
if [[ -n "$(git status --porcelain --untracked-files=no)" && "$ALLOW_DIRTY" != "1" ]]; then
    echo "release.sh: the working tree has uncommitted changes." >&2
    echo "  The binary would be built from them while the source in the tarball comes" >&2
    echo "  from $REF, so the two would not match. Commit first, or pass --allow-dirty" >&2
    echo "  if you are deliberately building a throwaway." >&2
    git status --short --untracked-files=no >&2
    exit 1
fi

if [[ -z "$ARCH" ]]; then
    case "$(uname -m)" in
        x86_64|amd64)  ARCH=amd64 ;;
        aarch64|arm64) ARCH=arm64 ;;
        *) echo "release.sh: unsupported machine $(uname -m) — pass --arch" >&2; exit 1 ;;
    esac
fi

NAME="simplepool-${VERSION}-linux-${ARCH}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "==> building $NAME"

# Build first: no point assembling a tarball around a binary that doesn't
# compile. -j is safe here; the Makefile's version header is generated under
# an explicit prerequisite.
make clean >/dev/null
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
[[ -x build/simplepool ]] || { echo "release.sh: no binary at build/simplepool" >&2; exit 1; }

# The binary states its own commit; if it can't run here, it can't run for an
# operator either, and shipping it would just move the failure downstream.
./build/simplepool --version >/dev/null || {
    echo "release.sh: build/simplepool --version failed — refusing to ship it" >&2; exit 1; }

# git archive rather than a copy of the working tree: only tracked files, no
# stray build output, no local proxy.conf with somebody's RPC password in it.
echo "==> staging tracked files from $REF"
git archive --format=tar --prefix="$NAME/" "$REF" | tar -x -C "$STAGE"

install -d "$STAGE/$NAME/build"
install -m 0755 build/simplepool "$STAGE/$NAME/build/simplepool"
scripts/record-build.sh simplepool "$REPO_ROOT" build/simplepool \
    --out "$STAGE/$NAME/build/simplepool.build.json" >/dev/null

COMMIT="$(git rev-parse "$REF")"
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
# glibc is the real portability floor for a dynamically linked C binary, and
# it is the one thing an operator cannot discover from the file name. State it.
GLIBC="$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+$' || echo unknown)"

cat > "$STAGE/$NAME/RELEASE" <<EOF
name=$NAME
version=$VERSION
commit=$COMMIT
arch=$ARCH
built_at=$BUILT_AT
built_against_glibc=$GLIBC
EOF

install -d "$OUT"
TARBALL="$OUT/$NAME.tar.gz"
tar -czf "$TARBALL" -C "$STAGE" "$NAME"

if command -v sha256sum >/dev/null 2>&1; then
    (cd "$OUT" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
else
    (cd "$OUT" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
fi

echo "==> $TARBALL"
echo "    version $VERSION  commit ${COMMIT:0:7}  arch $ARCH  glibc >= $GLIBC"
echo "    $(cat "$OUT/$NAME.tar.gz.sha256")"
