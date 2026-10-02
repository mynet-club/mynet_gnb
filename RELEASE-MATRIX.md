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
release entirely) and the `MAP_FAILED` fix in `gnb_mmap.c`. The second snapshot additionally
clears `thread_worker_run_flag` in both secure-index workers, which closes the one gap the first
snapshot left (a busy wait with no timeout in `gnb_worker.c:98-106` would spin forever on nodes
with `safe-index on`). CI still cannot prove the hot-restart path itself — that needs a real
two-node run.

Our own source assets are two commits, and only two: the baseline commit carries one line the
author's tree needs to compile on LP64 (`src/gnb_conf_file.c:841`, definition aligned to the
`gnb_uint64_t` declaration in `gnb_conf_file.h:24`), and the proxy commit carries the feature.
The LP64 conflict is author-side and compile-time — it hit every 64-bit Linux/FreeBSD leg and
riscv64 before that line existed, and macOS/Windows only escaped it because their SDKs typedef
`uint64_t` as `unsigned long long`. Every new author snapshot has to re-emit that line in its
baseline commit, or the 64-bit legs die at `src/gnb_conf_file.o`.

`main-patched` = release commit `e0228d9`: baseline commit `98f8b4e` (imported from the verified
ZIP, SHA256 `2454c303dabd...`, re-extracted and compared byte for byte; the only difference is one
empty directory) plus one proxy commit touching eight files. `git diff 98f8b4e..e0228d9` is the
complete list of our source changes — if that ever shows a ninth file, something unrelated has
entered the line.

`main` is not buildable yet: a root-layout pure-upstream commit has to be prepared first. The
`plan` job of every workflow records such a channel as skipped in the run summary instead of
guessing a commit.

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
`gnb_linux_mipsel_x-dev-patched_v1.6.8.tgz` (that channel has since been folded into
`main-patched`, which is production — the internal path stays as the guard for any future
non-production channel); the arch field cannot contain an underscore,
so such a name is unparseable by the registry and a dev package can neither overwrite a
production package nor be registered as one. Publish jobs classify by artifact directory
name against `RELEASE-SOURCES`, never by filename glob, and only production packages are
POSTed to CTL (`POST /api/v2/releases`, Bearer token from secret `CTL_RELEASE_UPLOAD_TOKEN`,
served back as `https://ctl.mynet.club/d/gnb/<filename>`). The old rsync to the deprecated
`download.mynet.club` is gone. Two registry gaps are surfaced as failures instead of skipped:
ctl's `PLATFORMS` has no `openbsd`, and `VALID_COMBINATIONS['openwrt']` accepts
`mipsel-softfp`/`mipsel-muslsf` but not `mipsel-sf` (that arch belongs to `openwrt-sdk`).

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


