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
CTL 的 `PLATFORMS` 里**没有 openbsd**，所以 BSD 轴的 openbsd 包 `ctl-publish` 会 fail-loud 报错，
不静默丢包；要发就得改 ctl 枚举。包名里的 os 标签 → ctl 的 platform 枚举，映射只写在
`.github/actions/ctl-publish/action.yml` 一处：**`openwrt` → `openwrt-sdk`**（2026-10-03 定，
OpenWrt 轴是官方 SDK 24.10 的 musl 构建，arch 名 `mipsel-sf`/`mips-sf` 只在
`VALID_COMBINATIONS['openwrt-sdk']` 里合法；`openwrt` 那一档的 arch 表是另一套，发过去必 422）。
代价要认：客户端解析路径还写死 `openwrt`（`mynet_ctl/app/Http/Controllers/Api/V1/DetectController.php:193`
与 `:209` 自己拼 `gnb_openwrt_<arch>_<version>.tgz`；`mynet_openwrt/luasrc/model/mynet/gnb_installer.lua:414`
只认 `os=="openwrt"`、`:252` 只 alias `{openwrt,linux}`），所以 SDK 包在 ctl 上查得到、设备侧暂时够不着，
要等 ctl/装机脚本认这个 platform。`mingw` 包也故意不发：ctl 的 `windows` 档 ext 是 `zip` 而产物是 tgz，
arch 还要 `x86_64`→`amd64`，口径未定就先红着。

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
