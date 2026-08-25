# Cutting a release

A simplepool release is what stands behind the one-line install, so the
process exists to make one thing true: **every published artifact traces back
to a commit anyone can check out.** Nothing is uploaded by hand.

```
PR (bump VERSION)  →  merge  →  git tag vX.Y.Z  →  CI builds + publishes
```

## 1. Bump the version in a PR

`VERSION` lives in the [Makefile](Makefile) and is compiled into the binary —
`simplepool --version` reports it, and so does `/api/versions` on the
dashboard. The release workflow **fails** if the tag and `VERSION` disagree,
because a release whose own binary reports a different version is worse than
no release: it makes every later "which version is this box running?" answer
untrustworthy.

```sh
git checkout -b release-0.2.0
sed -i 's/^VERSION    := .*/VERSION    := 0.2.0/' Makefile
git commit -am "Release 0.2.0"
gh pr create --fill
```

Merge it. Everything below runs against `main`.

## 2. Tag

```sh
git checkout main && git pull
git tag v0.2.0
git push origin v0.2.0
```

That single push triggers two workflows:

| workflow | what it publishes |
| --- | --- |
| [`release.yaml`](.github/workflows/release.yaml) | `simplepool-0.2.0-linux-{amd64,arm64}.tar.gz` + `SHA256SUMS`, attached to a GitHub Release |
| [`build_docker.yaml`](.github/workflows/build_docker.yaml) | `ghcr.io/layertwo-labs/simplepool{,-dashboard,-payout}:v0.2.0` |

## 3. What the release job actually does

Per architecture, on a native runner:

1. Check the tag matches `VERSION`.
2. Run [`scripts/release.sh`](scripts/release.sh) — the same script you would
   run by hand, so a locally built tarball and a released one are the same
   recipe. It builds the binary, refuses to continue if the tree is dirty
   (the binary would not match the source shipped beside it), assembles the
   tarball with `git archive`, and writes a `RELEASE` file plus a
   `build/simplepool.build.json` pinning the binary to the commit by sha256.
3. Unpack the tarball and run `build/simplepool --version` out of it. A
   tarball that doesn't produce a runnable binary never becomes a release,
   because the person who finds out otherwise is an operator running a
   one-liner on a fresh box.

Then a single job merges the per-arch checksums into one `SHA256SUMS` and
creates the Release.

Binaries are built on **Ubuntu 22.04 (glibc 2.35)** so they also run on
Ubuntu 24.04 and Debian 12. That floor is recorded in each tarball's
`RELEASE` file.

## 4. Verify the release exists and installs

```sh
# what the installer will resolve to
curl -fsSL https://api.github.com/repos/LayerTwo-Labs/simplepool/releases/latest \
  | grep '"tag_name"'

# what an operator runs
curl -fsSL https://raw.githubusercontent.com/LayerTwo-Labs/simplepool/main/scripts/install.sh \
  | sudo bash
```

On a box that is already installed, `simplepoolctl upgrade` moves it to the
new release and restarts the services.

## Dry run without tagging

`workflow_dispatch` on the Release workflow builds both tarballs and uploads
them as workflow artifacts **without** creating a Release — use it to check a
build before committing to a tag.

```sh
gh workflow run release.yaml
```

## Hand-building a tarball

```sh
scripts/release.sh                 # dist/simplepool-<version>-linux-<arch>.tar.gz
scripts/release.sh --help
```

Useful for testing the install path against a local file. It is not how
published artifacts are produced — those only ever come from CI.
