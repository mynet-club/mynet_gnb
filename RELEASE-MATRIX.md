# GNB release matrix

Two channels, one build pipeline. CI never applies patches and never edits functional
source: it exports the channel's fixed `release_source_commit`, verifies the source
identity, builds, checks the binaries, packages and publishes. The single source of truth
for what CI is allowed to build is `packages/RELEASE-SOURCES` (read it with
`scripts/release-sources.sh`).

| channel | author identifier | local source commit | release source commit | patch package (provenance only) | publish |
|---|---|---|---|---|---|
| main | `c411f14ee647` | `2313dea3288c98b363b2994c348452ef74cb7107` | `UNSET` | none | production |
| main-patched | ZIP SHA256 `2454c303dabd...` | `98f8b4ef16afc11a0452b5e76b9b0f1ddb84f9c1` | `e0228d937d7fc7e8c8b61c531457442bb27e34a9` | `packages/1.6.8-dev/p1-route-proxy` | production |

## Baseline and patch set as of 2026-10-02

**The release baseline is the author's dev package, not the 1.6.8 release.** Hot restart is
only complete in that package: in the released `2313dea` the symbol `gnb_hot_restart_prepare`
is a command word and a declaration (one hit each in `gnb_core.c/.h` and `cli/gnb.c`), with no
worker stop/start sequence behind it. The dev snapshot adds
`gnb_worker_wait_worker_started/stoped` and calls them for `primary / pf_ring / index / detect /
index_service / node`, plus `gnb_log_udp_close()` (called from `gnb_core.c:702`, absent from the
release entirely) and the `MAP_FAILED` fix in `gnb_mmap.c`. One deliberate gap remains in the
author's version: `gnb_secure_index_worker.c` never clears `run_flag` and is not waited on, so a
hot restart with `safe-index on` leaves that worker running — behaviour the author has not
defined, and CI cannot prove either way.

Our own assets are exactly two: the proxy feature (a source commit) and this build matrix. The
hot-restart packages (`p1-hot-restart`, `p1-hot-restart-v2`) and the old `p2-route-proxy` are
deleted, not archived. `p1-route-proxy` stays in the tree only as provenance for the release
commit — CI does not apply it, and `packages/RELEASE-SOURCES` is what decides what is buildable.

`main-patched` = `ffdf816`: base commit `b1d4534` (tree identical to the verified ZIP, SHA256
`1daa8c78cb07...`, re-extracted and compared byte for byte; four differences are macOS metadata)
plus one proxy commit touching eight files. `git diff b1d4534..ffdf816` is the complete list of
our source changes — if that ever shows a ninth file, something unrelated has entered the line.

`main` is not buildable yet: a root-layout pure-upstream commit has to be prepared by the
source/patch line first. The `plan` job of every workflow records such a channel as
skipped in the run summary instead of guessing a commit.


Author identifier and local commit are separate namespaces and must never be substituted
for one another.

## What one build produces

All four programs — `gnb`, `gnb_ctl`, `gnb_es`, `gnb_crypto` — come from the same export,
same compiler invocation and same job. Partial sets are never packaged. Each run also
writes a `MANIFEST` recording channel, author identity, local/patch/release commits,
build command, toolchain, `strip`, capability marks read back from the built binary, and
the archive SHA256.

## Package naming

Production keeps the naming contract `mynet_ctl` parses
(`{name}_{platform}_{arch}_{version}.tgz`, see `ReleaseDomainService::parseFilename`), so
the channel lives in the MANIFEST and the artifact directory rather than the filename.
Internal builds insert the channel segment after the arch, e.g.
`gnb_linux_mipsel_x-dev-patched_v1.6.8.tgz`; the arch field cannot contain an underscore,
so such a name is unparseable by the registry and a dev package can neither overwrite a
production package nor be registered as one. Publish jobs classify by artifact directory
name against `RELEASE-SOURCES`, never by filename glob, and only production packages are
rsynced to the download server.

## Platform expectations

| workflow | platform | note |
|---|---|---|
| `build_darwin.yml` | darwin | `clang -arch x86_64` / `-arch arm64`, one job per arch |
| `build_linux.yml` | linux | cross toolchains; `-static` passed as full `GNB_LDFLAGS`/`GNB_ES_LDFLAGS` replacement, then asserted per binary |
| `build_windows.yml` | windows | MinGW-w64 cross, `.exe` outputs |
| `build_bsd.yml` | freebsd / openbsd | built inside `vmactions` VMs with `gmake` |
| `build_openwrt.yml` | openwrt | official SDK 24.10, dynamically linked musl (device provides the runtime) |
| `verify.yml` | — | tooling lint + proxy unit tests run inside each channel's own exported release source tree |

`scripts/gnb-dist.sh` is the shared implementation; the workflows only supply platform,
arch, toolchain variables and the publish directory.

## What CI proves

Every statement about this release line is a cloud-CI result, read from the run:

- builds and capability marks (`Features: proxy`) of the produced binaries, per channel and arch;
- the proxy unit tests (52 cases) against the channel's own `release_source_commit`;
- tooling lint, including the patch-package index consistency check.

Before any of that, the local `pre-push` gate (`make -f Makefile.guard check`) proves the
other half: for every patch-bearing channel it exports that channel's `local_source_commit`,
applies the patch chain to the clean tree, asserts the proxy marker landed, re-runs for
idempotency, and runs the package's 52 unit tests inside that derived tree. `verify.yml`
proves the pinned commit; the local gate proves the pin is still re-derivable from upstream.

Still open, and not provable by CI as written (plan §8D data-plane items): two-node real NAT
return path, `proxy.conf` hot reload / deletion / syntax-error rollback on a running node,
`route.conf` versus `proxy.conf` priority, full table / fragmentation / PONG expiry /
multi-worker concurrency.

Blocked on the source-preparation line, not on build tooling:

- `main` has no `release_source_commit` (it remains `UNSET`, so CI refuses to build it rather
  than inventing a commit).
- Naming: the dev-snapshot baseline's `version` file still says `1.6.8`, so a raw tag would make
  the production archive `gnb_<os>_<arch>_v1.6.8.tgz` — indistinguishable from one built off the
  released 1.6.8 source, and `mynet_ctl` parses that filename into its release registry. Ruled
  on 2026-10-02 (developer): archives from a non-released identity carry `-dev`. The rule lives
  in `gnb-dist.sh`'s tag block and keys off `author_identity` only — `opengnb/opengnb@…` gets no
  suffix, anything else (an author ZIP) gets `-dev`, and an unrecognized new identity form
  defaults to `-dev` rather than silently colliding. Verified against the parser's regexes:
  `gnb_darwin_arm64_v1.6.8-dev.tgz` splits to name=gnb / platform=darwin / arch=arm64 /
  version=`v1.6.8-dev`, and `validateVersion()` accepts that version string.


