opengnb 构建与发行
=================

本仓是 [gnbdev/opengnb](https://github.com/gnbdev/opengnb) 的 fork，保留**上游根布局**
（`Makefile*`、`version`、`src/`、`libs/` 就在仓库根），在此之上做两件事：

1. 我们的功能改动 = **提交**：作者快照的基线提交 + 一个 proxy 提交压在它上面；
2. 编译与发行 = **GitHub Actions**：按通道固定的发行源码提交出多平台产物。

补丁串（`packages/<基线>/p1-route-proxy/patches/*.patch`）是从提交**按需导出**的溯源件，
CI 从不 apply 它。真相只有提交和 `packages/RELEASE-SOURCES`。

源码身份与通道
------------

`packages/RELEASE-SOURCES` 是「CI 允许构建什么」的唯一真相源，只用
`sh scripts/release-sources.sh {list|get|require|channels}` 读，不要 grep 它。

| 通道 | 作者身份 | 基线提交 | 发行提交 | 状态 |
|---|---|---|---|---|
| `main` | 上游 `opengnb/opengnb@c411f14ee647` | `2313dea` | `UNSET` | 尚无根布局纯上游提交，CI 拒绝构建而非替你推断 |
| `main-patched` | 作者开发 ZIP `2454c303dabd…` | `98f8b4ef16af` | `e0228d937d7f` | 生产发行 |

不变量：`git diff <基线提交>..<发行提交>` 就是我们在源码上的全部改动（当前 8 个 proxy 文件），
**冒出第 9 个文件即为混入别的东西**。`release_source_commit` 为 `UNSET` 的通道必须失败。

接作者新快照的固定动作写在 `packages/RELEASE-SOURCES` 的注释里（换基线 → 重放 proxy 提交 →
`git merge --no-ff` 进 `main` → 重打 tag）。钉的是**提交**不是分支名；已发布、已出过包的提交
不许 rebase 或 force-push，否则 CI `git archive` 取不到它只能 fail-loud。

构建与出包
--------

一个脚本干完六种事，各平台的 workflow 只供工具链与发布目录：

```sh
sh scripts/gnb-dist.sh --channel main-patched --os linux --arch amd64 \
    --tag v1.6.8-dev --make make --archive --dist-dir dist
```

```
git archive <release_source_commit> | tar -xf -     # 导出已固定的发行源码树
→ 校验源码身份（version 相符、无 src/src/、src/gnb_proxy.c 与 Features: proxy 在不在）
→ 按 OS 选 Makefile 编译四程序（gnb / gnb_ctl / gnb_es / gnb_crypto）
→ strip → 从产物二进制回读能力 → MANIFEST + SHA256SUMS → tgz
```

OS → Makefile 一律跟作者的 Makefile 走，我们不加编译入口：
`darwin/Makefile.Darwin`、`linux/Makefile.linux`、`openwrt/Makefile.openwrt`、
`mingw/Makefile.mingw_x86_64`、`freebsd/Makefile.freebsd`、`openbsd/Makefile.openbsd`、
`netbsd/Makefile.netbsd`。

四程序必须来自同一份构建，永不出 Partial 包。

触发方式
------

- 推送形如 `v*.*.*` 的 tag（例如 `v1.6.8-dev`）→ 五个 build workflow 全矩阵跑，并对外发行；
- Actions 页面 `workflow_dispatch` 手动跑：**只验构建，不发行**。没有 tag 就没有 Release，
  各 workflow 的 Release 与 CTL 上传步骤都带 `startsWith(github.ref, 'refs/tags/')` 条件跳过
  （包名版本取自 `version` + `g<short-sha>`，只落 workflow 产物）。要窄验证就单独 dispatch
  那一个 workflow，不必动 tag；
- `verify.yml` 跟着 `main` 的 push 跑工具守护与各通道单测。

平台矩阵
--------

| workflow | 平台 | 说明 |
|---|---|---|
| `build_linux.yml` | linux | amd64, arm64, armv7-hardfp, mips(el), mips64(el), riscv64；Debian/Ubuntu 的 glibc 交叉包，静态链接。**armv7 软浮点暂不覆盖**：需要 musl 的 musleabi（GitHub runner 连不通 musl.cc）或 Debian armel 那套包，而无 VFP 的 armv7 设备是极小概率；恢复要改的三处写在 workflow 的注释里 |
| `build_openwrt.yml` | openwrt | 官方 SDK 24.10，动态 musl（运行时由设备提供） |
| `build_darwin.yml` | darwin | `clang -arch x86_64` / `-arch arm64` |
| `build_windows.yml` | windows | MinGW-w64 交叉，`.exe` |
| `build_bsd.yml` | freebsd / openbsd | `vmactions` 虚拟机里 `gmake` |
| `verify.yml` | — | 工具 lint + 在每通道自己的发行源码树里跑 proxy 单测 |

产物命名与发布分级
----------------

生产包名保持 `mynet_ctl` 发布表能解析的契约 `{name}_{platform}_{arch}_{version}.tgz`
（通道名不进文件名，只在包内 `MANIFEST` 与产物目录名里）。内部包在 arch 与 version 之间插通道段，
arch 段不含下划线 ⇒ 这种名字注册表解析不了，也就覆盖不了生产包、不会被当成生产包登记。

publish 作业按产物目录名回 `RELEASE-SOURCES` 查 `publish` 字段分类，不靠文件名模式匹配：
只有 production 包 POST 到 **CTL**（`POST https://ctl.mynet.club/api/v2/releases`，
Bearer token = secret `CTL_RELEASE_UPLOAD_TOKEN`；对外取件路径
`https://ctl.mynet.club/d/gnb/<文件名>`），校验文件按平台命名（`checksums-<platform>.txt`），
避免多个 workflow 并发写同一个同名资产。以前那条 rsync 到 `download.mynet.club` 的链路已废弃
（见 `mynet_ctl/docs/knowledge-base/v2-upgrade-plan.md`），不要再配 `DOWNLOAD_SSH_*`。
上传 token 已在 CTL 现网铸好并配成该仓库的 repository secret（2026-10-03，token 只有
`release:upload` 一条 ability，明文在本机 `~/.config/ctl/gnb-ci-release-upload.token`，库里只存哈希）。

包名的三段（platform / arch / ext）一律落在 mynet_ctl `ReleaseDomainService` 的词表里，
**规范化只做出包侧一次**（`scripts/gnb-dist.sh` 的「发行名规范化」段），不在 `ctl-publish`
里再映射一遍：

| 出包入参 | 包名用的名字 | 为什么 |
|---|---|---|
| `--os mingw --arch x86_64` | `windows` / `amd64`，扩展名 **`.zip`** | `PLATFORMS` 里只有 `windows`（`ReleaseDomainService.php:32-39`），它的 `ext=zip`；`ARCHITECTURES` 没有 `x86_64`（规范名 `amd64`）。记录的扩展名必须等于字节真实格式，否则 Windows 端拿到一个叫 `.zip` 的 tar，解包必炸 |
| `--os openwrt`（SDK 24.10 / musl） | 仍叫 `openwrt`，mips 轴的 arch 就叫 **`mipsel`** / `mips` | 设备侧只查 `openwrt`：`DetectController.php:193` 写死 `$platform='openwrt'`、`:209` 自己拼 `gnb_openwrt_<arch>_<version>.tgz`、`:228-241` 的 `softfpFallback` **双向**但**只在上报 arch 一条记录都没有时才触发**、从不换 platform；`gnb_installer.lua:414` 只认 `os=="openwrt"`。`openwrt-sdk` 那一档虽然枚举里有，但没有任何消费者，发过去就是「查得到、取不到」的死路。**OpenWrt 从此不再有 glibc 那条线**：SDK/musl 构建顶掉老的 `openwrt` glibc 包，沿用同一个 platform 名。**mipsel 两条腿已合成一条（10-03）**：实测 mt7621/mt7620 的 musl interpreter 都是 `/lib/ld-musl-mipsel-sf.so.1`、包架构名都是 `mipsel_24kc` ⇒ 同一份 ABI，旧的 `mipsel` 标签只是 SDK profile 名。**合腿后留 `mipsel`、不叫 `mipsel-softfp`**：ctl 的映射表 `/api/v2/detect/arch-map` 里没有任何 `distrib_arch` 会映射成 `mips*-softfp`（`mipsel_24kc/24kec/74kc/mips32` 全部 → `mipsel`），而 `openwrt/mipsel` 下面已经有 `v1.6.0.a` 那批 `is_latest` 稳定行 ⇒ 发成 softfp 名的话设备永远命中旧行、走不到回退，等于被钉死在老版本。已经用错名发出去的那条只是 `v1.6.8-dev` 行（`is_stable=0`，设备侧永远取不到），不必补发，下个 tag 自然用对名字 |
| `--os openbsd` | `openbsd` / `amd64`、`arm64`，`.tgz` | ctl 的 `PLATFORMS`/`VALID_COMBINATIONS`/`parseFilename` 已加 openbsd（本仓 `mynet_ctl` 侧改动，未提交）。生产 `ctl.mynet.club` 跑的是 2026-03-25 那份、原本没有这个枚举，而 `ReleaseService::validateUpload()`（`app/Services/Release/ReleaseService.php:140-155`）会按枚举校验 ⇒ 没有它 openbsd 那两条就是 422（不是静默丢，job 照红）。**10-03 已把这两行枚举直接补到生产文件上**（备份 `/root/ReleaseDomainService.php.openbsd-20261003.bak`，`php -l` 过，opcache 会自己重校验，不用 reload fpm），仓库侧同样留着，正式部署时不丢 |

`ctl-publish` 对每个包都要求 HTTP 201，任何一包非 201 就让 job 红（缺 token、命名不符、
传输失败、422 都算）—— 静默跳过 `exit 0` 正是上一轮「tag 发了、下载面一个包都没有」的成因。
`variant` 一律不传：`ReleaseService::upload()` 会把 variant 当第 5 参喂 `generateFilename(…, $ext)`，
非空 variant 直接变成文件扩展名，而 `parseFilename` 只认 `tgz|zip|tar.gz|ipk` ⇒ 记录建得出来却下不到。
`is_stable` 按 tag 判（带 `-` 的预发布报**不稳定**）：`refreshLatestFlag()` 只在 `stable()` 作用域里选
latest，把 `-dev` 报成 stable 会推着现网节点升到开发包；代价是 dev 包不会成为 latest，要取它得按版本显式取。
**线上传的值必须是 `1`/`0`，不能是 `true`/`false`**：ctl 校验是 `nullable|boolean`
（`Api/V1/ReleaseController.php:66`），而 multipart 里一切值都是字符串，Laravel 的 `boolean` 只认
`true/false/1/0/"1"/"0"` —— 串 `"false"` 会 422「The is stable field must be true or false」
（10-03 五轴 publish 同时全红就是这个原因）。

**重推同一个 tag 是安全的，前提是包没变。** `releases` 表带
`unique(product, version, platform, arch, variant)`
（`mynet_ctl/database/migrations/2026_03_20_013123_create_ctl_core_tables.php:35`），
而 `ReleaseService::upload()` 里是裸 `Release::create()`、`ReleaseController::store()` 没有
try/catch ⇒ 不做判定的话，重推同一个 tag 时已发过的那几条腿会撞键回 **500**，修好一条腿想重跑
整个作业就永远红。所以 `ctl-publish` 发之前先按公开取件路径 `/d/gnb/<文件名>` 量一次字节：
取不到（404）就正常上传；取到且 sha256 一致就报 `SKIP`（已在下载面，算通过、不重复发）；
取到但 sha256 不同就**红** —— 同一个版本号下内容变了必须换 tag，不覆盖现网正在取的那一份。
若量不到却仍出 500，多半是同键的软删行（`status=0` 不在取件路径上，但仍占着索引）；
要重发得先用 `release:manage` 那个 token 处理旧行。

本地守护
------

```sh
make -f Makefile.guard check      # lint + test，pre-push 钩子跑的就是它
```

`test` 段对每个带补丁包的通道：导出它的 `local_source_commit` 成纯源码树 → 实跑补丁串 →
断言 proxy 标记落地 → 复跑验幂等 → 在这棵派生树里跑包内单测（52 项）。这证明的正是
CI 出包**不再**做的那一步：下一个基线还切不切得出来。

历史遗留（已清理）
------------

2026-10-02 删掉了改造前的出包管线：`scripts/release.sh`、`scripts/sync_source.sh`、
`scripts/release/**`、`packages/rebase.sh`、基线台账 `packages/BASELINE{,-XDEV}`，以及和
`1.6.8-dev/p1-route-proxy` 逐字节同构的 `packages/1.6.8-c411f14ee647/`（只差一行路径注释）。
需要对照当时的判断时从 git 历史取；作者源码包与测试证据留在 `sources/`、`packages/`。

文档
----

- `RELEASE-MATRIX.md`：通道 × 平台的发行口径与 CI 能证明什么
- `packages/README.md`：补丁包约定与历史结论
- `docs/gnb-maintenance-plan.md`：维护计划（补丁/功能侧的口径在这里）
