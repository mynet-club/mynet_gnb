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
(`{name}_{platform}_{arch}_{version}.{ext}` — `tgz`, or `zip` on the windows leg; see
`ReleaseDomainService::parseFilename`), so
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
`download.mynet.club` is gone. All three name fields (platform / arch / ext) are normalized
**once, on the packaging side** (`scripts/gnb-dist.sh`, the release-name normalization block);
`ctl-publish` never re-maps them, so the GitHub asset name and the ctl record are the same
contract from the start. Two decisions from 2026-10-03:
**windows ships `.zip`** — `PLATFORMS` has `windows` with `ext=zip`
(`ReleaseDomainService.php:32-39`) and `ARCHITECTURES` has no `x86_64` (canonical `amd64`);
the recorded extension must match the actual bytes or a Windows client unpacks a tar named
`.zip`. **OpenWrt stays on platform `openwrt`** — every consumer asks for `openwrt`
(`DetectController.php:193` hardcodes it and builds `gnb_openwrt_<arch>_<version>.tgz` at
`:209`; `:228-241` only falls back between arch variants (`mipsel` ↔ `mipsel-softfp`) and only when the
reported arch has **no** row at all, never between platforms; `gnb_installer.lua:414` only
accepts `os=="openwrt"`), while
`openwrt-sdk` (`:35`/`:82`) has no consumer at all — posting there would be "listed but
unfetchable". So the SDK/musl builds take over the `openwrt` slot and the glibc line for
OpenWrt is dropped; the mips axis keeps the bare labels `mipsel`/`mips` — those are exactly the
names devices report (`/api/v2/detect/arch-map`, see the merged-leg note below).
**`openbsd` is now in ctl's vocabulary**
(`ReleaseDomainService::PLATFORMS` + `VALID_COMBINATIONS`, and `parseFilename`'s platform
alternation) so the BSD axis can publish it. The deployed app under `ctl.mynet.club` is older
than the repo (`/data/app/mynet.club/ctl`, not a git checkout, last touched 2026-03-25) and its
`PLATFORMS` had neither `openbsd` nor `openwrt-sdk`; since
`ReleaseService::validateUpload()` enforces `validatePlatform`/`validateArch`/
`validateCombination` (`app/Services/Release/ReleaseService.php:140-155`), an OpenBSD upload was
a **422 until that enum landed in production**. It landed 2026-10-03: the two enum lines were
patched into the production file directly (backup `/root/ReleaseDomainService.php.openbsd-20261003.bak`,
`php -l` clean, no fpm reload needed — opcache revalidates), and the repo carries the same change
so a real deploy keeps it.

**Re-pushing a tag does not re-post packages that already match.** `releases` carries
`unique(product, version, platform, arch, variant)`
(`database/migrations/2026_03_20_013123_create_ctl_core_tables.php:35`) while
`ReleaseService::upload()` is a bare `Release::create()` with no try/catch in
`ReleaseController::store()` — without a pre-check, every already-published leg would answer
**500** on a re-run, so fixing one leg could never turn the job green. `ctl-publish` therefore
measures the public fetch path (`/d/gnb/<filename>`) first: 404 → upload; 200 with identical
SHA256 → `SKIP` (counts as success, nothing re-posted); 200 with a different SHA256 → hard
failure, because content under an existing version number means a new tag, not an overwrite of
what nodes are already pulling. A 500 that the pre-check did not predict is almost always a
`status=0` row: disabled releases leave the fetch path but still hold the unique index, and
clearing them needs a `release:manage` token.

**`is_stable` goes on the wire as `1`/`0`, never `true`/`false`.** The endpoint validates it with
`nullable|boolean` (`Api/V1/ReleaseController.php:66`), and in a multipart form every value is a
string — Laravel's `boolean` accepts `true`, `false`, `1`, `0`, `"1"`, `"0"`, so the strings
`"true"`/`"false"` are rejected. All five axes' publish jobs failed with **422** "The is stable
field must be true or false" on 2026-10-03 for exactly this reason; `0`/`1` also land correctly
through the model's boolean cast (`Models/Release.php:24`).

**The two mipsel legs are merged into one, posted as `mipsel` (2026-10-03; the name was corrected
the same day).** `mipsel` (ramips/mt7621) and `mipsel-sf` (ramips/mt7620) were never hard- vs
soft-float: measured 10-03 from a CI artifact, mt7621's ELF interpreter is
`/lib/ld-musl-mipsel-sf.so.1` — identical to mt7620 — and both profiles publish as package arch
`mipsel_24kc`. The old `mipsel` label described the SDK *profile*, not the ABI, so the two legs
published the same ABI twice. One merged leg therefore builds from the mt7621 profile (more
devices).

The arch label must equal **what the device reports**, not what the ABI is: `/api/v2/detect/arch-map`
maps `mipsel_24kc`/`24kec`/`74kc`/`mips32` → `mipsel` and nothing → `mipsel-softfp`, and
`buildDownloads()` consults `softfpFallback()` only when `getLatest()` found **no** row for the
reported arch (`DetectController.php:196-203`). `openwrt/mipsel` already holds `is_latest`
`v1.6.0.a` rows, so publishing only under `mipsel-softfp` would leave every mt7621/mt7620 node
pinned to the stale release — the fallback can never fire for an arch that has a row. That was my
first choice on 10-03, and a live probe of
`GET /api/v2/detect/openwrt?distrib_arch=mipsel_24kc` (answer: `ctl_arch=mipsel`, download URL
`gnb_openwrt_mipsel_v1.6.0.a.tgz`) disproved it. `mips` (ath79) is soft-float
too but stays bare-labelled for the same reason: `mips_24kc` → `mips`, and the `gnb_openwrt_mips_*`
rows already in the download face would stop resolving.

## Platform expectations

| workflow | platform | note |
|---|---|---|
| `build_darwin.yml` | darwin | `clang -arch x86_64` / `-arch arm64`, one job per arch |
| `build_linux.yml` | linux | cross toolchains; `-static` passed as full `GNB_LDFLAGS`/`GNB_ES_LDFLAGS` replacement, then asserted per binary. Shares arch *names* with the OpenWrt axis but not the build: glibc + `-static` here, SDK musl + dynamic there |
| `build_windows.yml` | windows (dist `--os mingw --arch x86_64` → package name `windows`/`amd64`, `.zip`) | MinGW-w64 cross, `.exe` outputs |
| `build_bsd.yml` | freebsd / openbsd | built inside `vmactions` VMs with `gmake`; openbsd needed ctl's enum in production, patched in place 2026-10-03 (see above) |
| `build_openwrt.yml` | openwrt | official SDK 24.10, dynamically linked musl (device provides the runtime); this replaces the old glibc line, `openwrt-sdk` is unused. One merged mipsel leg, posted as `mipsel` (the label devices report), not two |
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


