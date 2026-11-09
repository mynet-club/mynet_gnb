# mynet_gnb 维护与发行执行计划

本文档保留历史维护/发行记录。当前 GNB 采用自有版本维护，按需合并作者主线，不再按补丁包组织开发；UTC 当前契约与测试入口见文末“UTC MVP GNB 实现与验收”。历史发行 pin 不因源码开发自动改写。

## 一、设计目标

`mynet_gnb` 维护 opengnb 的源码、MyNet proxy 功能和多平台发行流程。

核心原则：

1. 作者源码和 MyNet 改动分离；
2. 每个作者基线都有独立的本地源码提交号；
3. 每个基线都有独立的 `p1-route-proxy` 补丁；
4. 稳定线和作者开发线互不混用；
5. 所有 `gnb`、`gnb_ctl`、`gnb_es`、`gnb_crypto` 必须由同一次 workflow 构建；
6. 发包只从干净 checkout、干净源码基线和明确补丁栈生成；
7. 作者开发原版不单独发布，只发布“开发源码 + MyNet proxy”验证包。

## 二、版本映射

### 稳定主线

```text
作者仓库：opengnb/opengnb
作者版本：1.6.8
作者 commit：c411f14ee647
本地纯源码导入提交：2313dea3288c98b363b2994c348452ef74cb7107
补丁：packages/1.6.8-c411f14ee647/p1-route-proxy/（2026-10-02 已清理删除：与
     packages/1.6.8-dev/p1-route-proxy 逐字节同构，只差一行路径注释）
```

对应关系：

```text
作者 c411f14ee647
    ↓
本地源码提交 2313dea...
    ↓
主线 p1-route-proxy
```

### 作者开发线

```text
作者来源：gnb(1).zip
作者标识：1.6.8-dev
ZIP SHA256：1daa8c78cb07d48e116530798bf13fbfe7d5549410c23f5aa004ae8aff59c344
本地导入提交：74414d1
分支：x-dev
补丁：packages/1.6.8-dev/p1-route-proxy/
源码包：sources/author-dev/1.6.8-dev-74414d1/gnb-1.6.8-dev.zip
```

同一条开发线的第二个快照（2026-10-02，已在 main 线上，是当前发行基线）：
ZIP SHA256 `2454c303dabdb5e27a13a3fe19b0c4d5066a845d12b75e80172afa84e990c1ae`，
导入提交 `c81b4598faeaf9b4532d1ce6b0d062b5062194d5`，
源码包 `sources/author-dev/1.6.8-dev-2454c303/gnb-1.6.8-dev.zip`。
上面那块写的是 `x-dev` 分支收到的第一包，两条线不共享导入提交。

对应关系：

```text
作者 ZIP 快照
    ↓
本地源码提交 74414d1
    ↓
x-dev p1-route-proxy
```

作者 SHA 和本地 SHA 属于不同命名空间，不能互相替代。

## 三、仓库目录结构

当前上游源码已经从旧的嵌套结构迁移为 opengnb 原生结构：

```text
mynet_gnb/
├── Makefile.inc
├── Makefile.Darwin
├── Makefile.linux
├── Makefile.openwrt
├── src/
├── libs/
├── docs/
├── packages/
├── sources/
├── scripts/
├── .github/workflows/
└── RELEASE-MATRIX.md
```

不要重新引入 `src/src/`。上游源码文件路径应保持为 `src/gnb_core.c`、`src/packet_filter/gnb_pf_route.c` 等。

## 四、补丁策略

当前只保留一个 MyNet 功能补丁：

```text
p1-route-proxy
```

它不是热更新补丁。作者开发线已经包含作者自己的热更新/热日志开发代码，因此 MyNet 不再重复叠加旧的热更新补丁。

两套补丁必须分别维护：

```text
packages/1.6.8-c411f14ee647/p1-route-proxy/
packages/1.6.8-dev/p1-route-proxy/
```

两套补丁的实现可以不同，因为上游结构、函数上下文和生命周期可能不同。禁止把稳定线补丁直接复制到 `x-dev`，也禁止反向复制。

### 开发与发行的职责边界

基线升级是代码开发工作，由开发者或执行 agent 完成。Actions 只验证和构建已经提交的源码及补丁，不负责推断新基线需要怎样改代码，也不负责自动解决冲突或生成新的功能实现。

每次作者更新时，由开发者或执行 agent：

1. 保存作者源码包或固定完整作者 commit，导入为新的本地纯源码提交；
2. 在该基线的独立开发副本上迁移 proxy 实现，按新 API、结构和生命周期修改实际代码；
3. 将适配后的 proxy 代码提交成固定功能提交，记录它与本地源码基线的关系；
4. 从基线到功能提交用 `git format-patch` 导出对应版本的 `p1-route-proxy`，归档源代码提交和补丁包；
5. 在全新基线副本上应用、编译、测试并逆序回滚，确认补丁能完整复现功能提交的代码；
6. 验证通过后更新基线记录、补丁包 README、SHA256 和发行矩阵，一并提交。

补丁制作 shell 属于开发阶段：校验固定基线 → 在开发副本上落地已经编写好的 proxy 代码 → 测试 → 固定功能提交 → 抽取 patch → 归档补丁包 → 提交打过补丁的发行源码。shell 不能自行推断新基线所需的代码适配；适配实现由开发者或执行 agent 提供。

后续 Actions 只 checkout 固定的发行源码提交，验证版本与源码身份，编译、测试、打包和发行。它不应用补丁、不修改功能代码、不抽取或生成补丁包。main 原版使用纯基线源码提交；另外两组使用各自已经打过补丁并固定的源码提交。

当前状态核对（2026-10-02）：现有 apply.sh 只是应用已有 diff，并未实现上述制作流程；开发包仍有旧嵌套路径补丁与新补丁并存，两套新补丁均尚未包含 Features CLI 源码改动。不能把当前包标为完整交付或安全验收通过。执行 agent 必须先修复并验证这些问题，再固定发行源码提交。

### Features 能力标识属于 proxy 补丁

稳定线和开发线的 `p1-route-proxy` 都必须包含 CLI 输出 `Features: proxy` 的实际源码修改。这是 proxy 功能的一部分，与该基线的配置解析、选路和准入代码一起适配、提交和导出。

一个 `p1-route-proxy` 包可以包含多个按顺序应用的提交，但能力标识必须属于这个包。禁止在应用补丁后由 workflow、打包脚本或额外的源码改写步骤插入 `Features`。Actions 只检查构建后的程序是否正确报告能力：原版没有 proxy 能力行，两组已固定的带补丁源码产物有 `Features: proxy`。

## 五、发行矩阵

Actions 最终应生成三组产物：

| 发行组 | 源码 | 补丁 | 用途 |
|---|---|---|---|
| main | 作者稳定主线 `c411f14ee647` | 无 | 上游对照和回滚 |
| main-patched | 同上 | 稳定线 `p1-route-proxy` | 生产版本 |
| x-dev-patched | 作者 ZIP / 本地 `74414d1` | 开发线 `p1-route-proxy` | 内部测试和提前验证 |

不发布单独的 x-dev 原版包。

四个程序必须在同一个 job、同一个源码 checkout、同一次构建中产生：

```text
gnb
gnb_ctl
gnb_es
gnb_crypto
```

## 六、作者开发线风险记录

反馈文档 `author-dev-feedback-20261002.md`（针对第一个开发快照 `1daa8c78cb07…`）已于
2026-10-02 随该快照退位删除，需要原文时从 git 历史取；这里保留结论与仍要作者给的验收结果。

当前只记录代码观察，不把 MyNet 的 workflow 已经解决的问题重复列为作者要求：

- 热更新采用先停旧 core，再创建新 core；初始化失败时可能半停机（**仍未解决**，作者侧无回滚路径）；
- secure-index worker 的退出标志：第二个快照 `2454c303dabd…` 已由作者本人补上
  （`gnb_secure_index_worker.c:730`、`gnb_secure_index_service_worker.c:465` 各加
  `thread_worker_run_flag = 0`），但启用该功能的热重启退出仍需实机取证；
- 热日志触发接口需要确认主循环是否真正消费 `log_trigger`；
- 共享内存结构发生变化，但 MyNet workflow 会保证四个程序同批构建。

希望作者至少提供的验收结果（原文 §5，发行侧无法代证）：

1. `safe-index on` 连续热重启 10 次；
2. 热重启期间故意制造配置错误，确认旧 core 是否继续服务；
3. 热日志切换 10 次，确认 fd 数量稳定；
4. `gnb` 与 `gnb_ctl` 混用不同构建时能否明确拒绝；
5. 双机真实数据面持续传输时热重启，确认 TCP、UDP、ICMP 是否恢复；
6. Linux、OpenWrt、Windows 至少各完成一次上述关键路径测试。

## 七、已完成内容

- 稳定基线映射已记录；
- 作者开发 ZIP 已归档并记录 SHA256；
- `x-dev` 已建立；
- 上游目录已对齐，消除 `src/src`；
- 稳定线独立 proxy 补丁已生成；
- 开发线独立 proxy 补丁已生成；
- 两套补丁均使用 `p1-route-proxy`；
- 稳定线 proxy 补丁应用后 macOS 四程序编译通过；
- `scripts/apply_packages.sh --src . --packages packages/1.6.8-c411f14ee647` 已通过幂等检查；
- 脚本 lint 已通过。

## 八、未完成任务

### A. 补丁代码与补丁包整理（开发者或执行 agent 执行）

- [ ] 为两个纯源码基线分别固定 proxy 功能提交，用 `git format-patch` 导出对应补丁，并记录完整的基线 SHA、功能提交 SHA 和补丁包校验值；
- [ ] 两套 `p1-route-proxy` 都包含 `Features: proxy` 源码改动；补齐 CLI 行为测试，确认回滚后能力行也被撤销；

- [x] 将 `packages/README.md` 的“当前采用”索引改为新的稳定 `p1-route-proxy`，避免旧 `p1-hot-restart-v2`、`p2-route-proxy` 描述造成歧义；
  2026-10-02 完成：`packages/README.md:33-34` 的包表只剩 `p1-route-proxy`（主线 + 开发线各一）两行**当前采用**，
  三个旧包不再作为目录存在（见下一条）。`packages/README.md:81` 起的 CI 段改成「出包不再 apply 补丁」；
  `scripts/check.sh` 的 `lint_index` 现全绿。
- [x] 为稳定线和开发线补丁各自补齐统一格式的 README：基线、文件、应用、回滚、测试、限制；
  2026-10-02 更正而非新增：两份 README 此前**逐字节相同**（`diff` 无输出），开发线那份把自己写成上游
  tag `c411f14ee647` 基线、并把应用路径写成主线目录。现各自标明真实基线
  （`packages/1.6.8-dev/p1-route-proxy/README.md:11`）、去除并不存在的 P1 前置（同文件 `:12`），
  并把「未在本基线实测」的平台构建行划归主线证据（同文件 `:98-101`）。
- [x] 检查旧 p1/p2/p3 目录是否仅作为历史归档，避免默认脚本误选旧包；
  `scripts/apply_packages.sh:61` 的基线挑选现在只看 `p1-route-proxy/patches/*.patch`，
  `:141` 只应用这一个包。**2026-10-02 按开发者裁定直接删除、不留档**：
  `packages/1.6.8-c411f14ee647/{p1-hot-restart,p1-hot-restart-v2,p2-route-proxy}` 以及
  `packages/1.6.8-c411f14ee647/dist/`（四个旧包 tgz）已 `git rm`，旧目录里那份
  指着 `src/src/` 而跑不起来的 `p2-route-proxy/tests/` 一并消失；结论性文字保留在
  `packages/README.md` 的「历史注意事项（退位包，包目录已删除）」段。
  注意：删除只发生在本分支（`feat/gnb-release-actions`），同样的文件在 `codex/upstream-layout`
  分支与 `packages/BASELINE:20` 的 `PATCH_STACK` 里仍然存在——后者现在指向已删除的包，
  属于补丁线要收的口径，发行侧不代为改动。
  **2026-10-02 收口**：`packages/BASELINE`、`packages/BASELINE-XDEV` 与 `packages/rebase.sh` 已随
  「补丁是真相」的旧口径一并删除，提交是真相；`codex/upstream-layout` 是历史侧枝，不再往里改动。
- [ ] 为两套补丁分别重打 tgz，并做解包 diff 校验。【2026-10-02 作废，不再执行】
  **2026-10-02 作废**：两套补丁只剩一套（`packages/1.6.8-c411f14ee647/p1-route-proxy` 与
  `packages/1.6.8-dev/p1-route-proxy` 逐字节同构，前者已删），包 tgz 这一交付形态也随之取消——
  补丁串按需从提交 `git format-patch` 导出，不再作为独立交付件维护。

### B. 应用脚本

- [x] 将 `scripts/apply_packages.sh` 改为按发行 variant 选择补丁：`main`、`main-patched`、`x-dev-patched`；
  2026-10-02 完成，形态与原文不同：包集合已收敛成一个（`p1-route-proxy`，无前置），所以 variant 差异
  落在**基线目录**上，由 `packages/RELEASE-SOURCES` 提供（`scripts/check.sh:107-131` 按通道枚举、
  `apply_packages.sh --packages packages/<基线>` 选择）。`main` 无补丁包，直接记为不适用而非误打。
- [x] 默认不再应用旧热更新补丁；见 `scripts/apply_packages.sh:141`（唯一一次 `apply_package` 调用）。
- [x] 在纯稳定源码、纯开发源码上分别做一次完整 apply/revert；
  2026-10-02 实测：两棵导出树（`2313dea:src`、`74414d1:src`）apply 后各 8 个文件改动，
  `revert.sh` 后 `git status --porcelain` 行数归 0 且 `src/gnb_proxy.h` 消失；幂等复跑打印「所有包都已应用过」。
  这条守护已进 `scripts/check.sh` 的 `test` 段（`make -f Makefile.guard check` 全绿，两通道各 52 项单测通过）。
- [x] 应用后检查 proxy 标记；实际标记是 `src/gnb_proxy.h`（不是原文写的 `src/gnb_proxy_conf.h`，
  那是旧 P2 包的文件名）+ `src/gnb_core.c` 的 `gnb_proxy_cfg_load` + `src/packet_filter/gnb_pf_route.c`
  的 `gnb_proxy_pick_flow`，见 `packages/1.6.8-dev/p1-route-proxy/apply.sh:8-10`
  （原引用的 `packages/1.6.8-c411f14ee647/` 那份逐字节相同，已删）。


### C. Actions（只编译已固定的发行源码）

- [ ] 不在 Actions 中生成或适配功能补丁；补丁应用、源码一致性或测试失败必须阻断发行；
- [ ] 删除打补丁后的 Features 源码插入步骤，改为验证最终二进制的能力输出；

- [ ] 所有平台 workflow 使用仓库根目录上游布局，不再使用 `make -C src`；
- [ ] 增加 variant 矩阵，分别构建三组发行产物；
- [ ] `main` variant checkout 纯源码基线提交；
- [ ] `main-patched` variant checkout 已包含稳定线 proxy 的固定发行源码提交；
- [ ] `x-dev-patched` variant checkout 已包含开发线 proxy 的固定发行源码提交；【2026-10-02 作废：该通道与 main-patched 收敛成同一棵树，三通道降为两通道，见 `packages/RELEASE-SOURCES`】
- [ ] 每个 variant 生成 manifest，记录作者标识、本地源码提交、补丁提交和构建 SHA；
- [ ] 保证四个程序来自同一个 variant job；
- [ ] 上传包名中明确包含 channel，避免开发包覆盖生产包。

### D. 验证

- [ ] macOS：三组 variant 四程序构建；
- [ ] Linux：三组 variant 四程序构建；
- [ ] OpenWrt：至少 x86_64、arm64、mips 代表架构；
- [ ] Windows：三组 variant 构建；
- [ ] proxy 单元测试 52/52；
- [ ] 双机真实 NAT 回程；
- [ ] proxy.conf 热加载、删除、语法错误回滚；
- [ ] route.conf 与 proxy.conf 优先级；
- [ ] 表满、分片、PONG 过期和多 worker 并发；
- [ ] 作者开发线启用和不启用 secure-index 的热重启对照测试。

## 九、推荐执行顺序

```text
1. 完成两个基线的 proxy 代码适配和固定功能提交，补丁内包含 Features
2. 导出并验证两套版本化补丁包，清理补丁索引和旧包选择逻辑
3. 完成 apply/revert 的 main / main-patched / x-dev-patched 选择
4. 完成 Actions variant 矩阵
5. 先跑 macOS / Linux
6. 再跑 OpenWrt / Windows
7. 跑 proxy 单测和双机数据面
8. 记录 manifest 和 checksums
9. 最后才发布生产包
```

生产发布的必要条件是：稳定主线和主线补丁都通过构建与 proxy 验证；`x-dev-patched` 只作为开发验证，不得自动替换生产包。


## 十、平台适配原则

proxy 的选路、配置解析、连接粘性和分片缓存位于平台无关的 `src/gnb_proxy.c/h`。平台差异由已有的 gnb 抽象承担：Unix 使用 pthread 读写锁，Windows 使用 `CRITICAL_SECTION`；IPv4 解析按目标平台的 socket 头文件处理。

因此不为 Darwin、Linux、BSD、Windows、OpenWrt 各维护一份 proxy 源码补丁。两套基线各自只保留一份 `p1-route-proxy`，并在其中修改构建入口：

- `Makefile.inc` 覆盖 Darwin、Linux、BSD、Windows 和 `Makefile.openwrt_local`；
- `Makefile.openwrt.inc` 覆盖 OpenWrt；
- 各平台 Makefile 通过这两个公共文件把 `src/gnb_proxy.c` 编入 `GNB_CORE_OBJS`。

开发阶段不需要为每个平台重复设计 proxy 逻辑，但仍需要在发行矩阵中做编译验证。平台测试的目的主要是发现头文件、锁宏、编译器和链接差异，而不是重新审查一遍选路算法。至少要确认每个平台的 `gnb`、`gnb_ctl`、`gnb_es`、`gnb_crypto` 能从同一套已打补丁源码构建；proxy 行为测试由公共单元测试和代表性数据面测试覆盖。

## 十、目录清理指令

清理前必须确认当前分支和工作区状态：

```sh
git branch --show-current
git status --short
```

### 清理编译产物

```sh
make -f Makefile.Darwin clean
find . -name '*.o' -type f -delete
find . -maxdepth 1 -type f \( -name gnb -o -name gnb_ctl -o -name gnb_es -o -name gnb_crypto \) -delete
find . -maxdepth 1 -type d -name bin -prune -exec rm -rf {} +
```

Linux、OpenWrt、BSD 和 Windows 使用对应 Makefile 的 `clean` target；不要手动删除 `src/`、`libs/`、`packages/`、`sources/`。

### 清理临时验证目录

临时构建目录确认不再需要后再删除：

```sh
rm -rf /tmp/gnb-main-patch-20261002
rm -rf /tmp/gnb-dev-patch-20261002
rm -rf /tmp/mynet_gnb_xdev_20261002
```

如果临时目录是 Git worktree，先检查：

```sh
git worktree list
git worktree remove /tmp/mynet_gnb_xdev_20261002
```

### 清理旧嵌套目录

目录迁移完成并确认 `git status`、构建和补丁链全部通过后，检查旧结构：

```sh
test ! -d src/src || echo 'ERROR: 仍存在旧的 src/src 嵌套目录'
find . -path '*/src/src' -type d -print
```

确认没有需要保留的文件后，才可以删除空目录：

```sh
find src -type d -empty -delete
```

### 清理未跟踪文件

先预览，确认没有源码包、测试证据或补丁文件会被删除：

```sh
git clean -ndX
git clean -nd
```

只清理明确的构建目录和忽略文件，不要直接对整个仓库执行 `git clean -fdx`。源码包和测试证据位于 `sources/`、`packages/` 时必须保留。

### 清理旧补丁包

旧补丁只能在新补丁已经完成构建、测试并提交后处理。先查引用：

```sh
rg -n 'p1-hot-restart|p1-hot-restart-v2|p2-route-proxy|p3-log-hot-config' \
  scripts .github packages README.md RELEASE-MATRIX.md
```

确认没有默认 workflow、脚本或文档引用后，再把旧包移动到历史归档目录；不要直接删除仍被 `sources/history.ndjson` 或发布记录引用的目录。

## UTC MVP GNB 实现与验收（2026-11-04）

原始源码基线：`eed65f8675f9e50817c7f55089c98bffe91c90c6`。本批直接开发基线：main 的 `04e6b105b47c25760e5ab44490e38bed70f00da7`。GNB 采用自有版本源码维护，按需合并作者主线；本文前面的补丁包制作流程为历史记录，不指导当前 UTC 开发。源码提交与版本以本仓 `git log` 为准。

### 范围与模块

本批实现 GNB 侧的 per-peer 本地路径策略、收发约束、配置热载、持久恢复和加载状态；不实现 UTC TCP/TLS 服务、租约签发或双端协调器，不声称整个 UTC A-S/A-D 公网交付完成。

| 模块 | 职责 |
|---|---|
| `src/gnb_utc.c/h` | 每实例策略表、config/pair 版本、三态路由、到期锁存、来源检查；上限 128 个历史 peer，含 direct tombstone，不自动清历史 |
| `src/gnb_utc.c/h` 集成入口 | 原生 socket 桥接、按 export 地址族发送、错误不回退、来源匹配、帧长度和诊断计数；原 runtime 文件已合入核心模块 |
| `src/gnb_utc_service.c/h` | 私有文件解析、实例/scope 固定、持久状态、周期更新、boot/PID 和 status；POSIX 实现 |
| route/node/index/primary workers | 最终目标在原生回退前接管；保留原 VPN 加密链；抑制定向 DETECT；保留 Index 注册/查询；来源 guard 及代理来源学习隔离 |

`gnb_node_t` 和 ctl/map 布局未改；`gnb_ctl` 仍可读取并触发原生热重启。核心结构中的可选 UTC manager 是进程私有状态，不进入 map。旧状态无配置时保持原生路由。POSIX 构建公布 `Features: proxy utc-conf utc-conf-v1`；Windows 不公布 UTC 能力，新增通用源码仅做交叉编译兼容检查，不将 POSIX 恢复结果外推到 Windows。

### 冻结的本地执行格式 v1

`conf_dir/utc.conf` 必须为 daemon 当前用户所有、0600、普通文件、非符号链接。执行器先完成业务授权，再原子替换本地投影。GNB 不接管平台授权或签租约。文本固定为：

```text
utc-v1 <scope-token> <instance-token> <local-uuid> <config-gen>
peer <peer-uuid> <pair-id> <pair-gen> <mode> <expires-at> <family> <address-32hex> <port> <ipv6-scope-id> <manifest-digest-64hex>
```

- token 长 1–63，限字母数字和 `_-.`；ID/代号为非零 uint64 十进制，不允许回绕；摘要/地址为小写 hex。未知键、超长/溢出、重复 peer/pair、错误 scope/节点与版本冲突拒绝整次更新。
- mode 1 为 UTC，family 为 4/6；IPv4 在前四字节、后十二字节为零；端口/有效期非零。mode 0 为显式 direct，expiry/family/address/port/scope 必须全零，也必须推进 pair_gen/config_gen。
- export = family/address/port/scope 的 UDP 下一跳，可以是本地 Client，也可以是远端 Edge。不可用地址族在配置阶段拒绝，send 错误不落回原生候选。
- config_gen 管整份文件，pair_gen 管单 peer；未改 pair 不刷新单调截止、不解除已锁存阻断。省略受管 peer 或删配置不解除管理；只有显式新代 direct 才释放。
- UR0/UR1 **服务端**可能向任意地址转发，无法完整识别最终 peer。MVP 拒绝它们与未释放 UTC 约束共存（包括重启恢复），而不是悄悄放行或全局关闭其他 peer 的 UR。无 UTC 约束时原 UR 行为保留；自动选路、多跳规划、动态出口验证归外部后续工作。

更新错误由同一枚举/状态口径报告：CONFIG_INVALID、VERSION_REWIND、GENERATION_CONFLICT、CAPACITY_EXCEEDED、UNSUPPORTED、SCOPE_MISMATCH；I/O/恢复失败另报 STORAGE_FAILED、RECOVERY_REQUIRED 等。config/policy/status 不含 VPN 私钥、业务 payload、订单或价格。

### utc-v2 水位与续约

`utc-v2` 的 peer `mode` 为 0=显式 direct，1=A-S（端点来自签名 TargetBinding 的远端 UDP 目标），2=A-D（peer 把 UDP 发给本机 utcd client 的每 pair 回环监听）。mode 2 必须携带非零 `expires_at`/`port`/`family`（4/6），端点只能是回环地址（IPv4 `::ffff:127.0.0.0/8`、IPv6 `::1`，`scope_id` 为零），manifest 仍是授权 TargetBinding 摘要；非回环、零端口、零有效期、IPv6 scope 非零或未知 mode 拒绝整份投影。mode 2 与 mode 1 共用 export 数据路径，持久策略表仍只存 0/1；两者切换因授权摘要不同须升 `pair_gen`。

`utc-v2` 的每个 peer 携带 `lease_epoch`/`placement_epoch`，GNB 将其与 `pair_gen` 一起存入本地策略表和持久状态，作为每 peer 的单调水位。同 `pair_gen` 的内容变化只在纯续约时允许：`pair_id`/`mode`/`family`/`address`/`port`/`scope_id`/`digest` 必须与现有行逐字段相同，且 `expires_at`、`lease_epoch`、`placement_epoch` 均不得回退。普通 RENEW（`rotate=false`）保持两个 epoch 不变、只延长 `expires_at`，同样接受；epoch 前进也接受。身份字段任一变化报 GENERATION_CONFLICT，必须升 `pair_gen`（ADR-0005）。任一水位（过期时间或 epoch）回退报 VERSION_REWIND；`config_gen` 未变的重复投影仍仅对完全相同的字段幂等。**续约只推进水位，不解除已锁存阻断**；恢复后的 export 仍需更大 `pair_gen` 的显式投影才能转发。`pair_gen` 增大仍按原规则允许完整策略变更；本轮不额外施加跨代 epoch 检查。

`utc-v1` 无 epoch 字段，其记录按原有规则解码为 epoch 0；同 `pair_gen` 的内容变化保持原 GENERATION_CONFLICT 语义，不因本次扩展而放宽。

### 恢复与状态

`utc.managed` 是永久保留的完整预备状态，不只是布尔标记；`utc.state.0/1/2` 是另外三份完整副本。规范文本包含所有受管/已释放记录和水位，以 SHA-512 检查完整性；它不替代受信文件权限或业务授权。当前导出一律写 `state-v2`：

```text
state-v2 <scope-token> <instance-token> <local-uuid> <config-gen>
entry <peer-uuid> <pair-id> <pair-gen> <mode> <expires-at> <family> <address-32hex> <port> <ipv6-scope-id> <manifest-digest-64hex> <lease-epoch> <placement-epoch>
```

解码同时接受既有 `state-v1`（无末尾两个 epoch 字段，水位视为 0，仅用于恢复旧状态）；`state-v2` 条目缺少 epoch 字段、或 `state-v1` 条目多出该字段均视为损坏。四份副本/`utc.managed` 的写入顺序、选代与 fail-closed 语义不变；重启、断电、坏文件不得回退 epoch 或复活旧代。升级不产生同代 v1/v2 混合副本：持久化只在 `config_gen` 前进时发生，旧副本代数必然更低。

先逐份临时文件写全/fsync/rename/目录 fsync，四份都成功后才发布内存策略。初次启用在 managed 记录写入后中断，也能恢复已知受管集合；没有被激活的预备状态不能凭磁盘记录转发 export。部分持久化失败时冻结更新、阻断本次涉及的已有 export；未变更 pair 保持原约束并独立到期，等待可信恢复；不倒退版本补偿。

重启在有效副本中选最高代，export 全部先 BLOCKED；需要外部重新授权并发布新代才能恢复，direct tombstone 保留。运行时损坏/缺文件不会清空内存约束；单副本损坏由其他副本恢复，非受管 peer 保持原生行为。所有可信副本都丢失/损坏则拒绝该实例启动，属于显式灾难恢复失败，不能记作“其他 peer 未受影响”。

启动器须在目录外持久保存 `utc_managed` 历史标记，已受管实例冷启动时传入 `--utc-managed`。无可信状态时拒绝启动，包括全部状态被删除。首次启用可在原生实例运行中加载配置；热重启自动继承受管断言。Windows 检测到 UTC 状态或该参数时拒绝启动。agent/A2 尚须完成目录外元数据和参数接线。

`utc.status` 每秒原子刷新，格式为：

```text
status-v1 <scope> <instance> <local-uuid> <boot-id-32hex> <config-gen> <generated-at> <last-operation-reason> <pid>
metrics <kernel-accepted-tx-packets> <policy-blocked-tx> <send-errors> <rejected-rx>
policy <与配置 peer 行相同的字段>
peer <peer-uuid> <pair-id> <pair-gen> <APPLIED|BLOCKED> <reason> <manifest-digest-64hex> <local-digest-128hex>
```

local-digest 是对应规范 `policy ...\n` 行（含换行）的 SHA-512，用于核对本端投影；共同 manifest digest 与本端摘要分开。计数器为当前 boot 的诊断，不是额度或交付计量，kernel accepted 不等于远端收到。执行器须核对 scope/instance/PID/当前 boot/代号/摘要与新鲜度；APPLIED 仅为已持久并加载的事实，GNB 不写 VERIFIED/EFFECTIVE。`utc.status` 格式保持不变，agent 不依赖 epoch 字段；水位只持久化在策略表与 `state-v2` 中。

service open 另写引导文件 `utc.identity`，内容为 `identity-v1 <local-uuid> <boot-id-32hex> <pid>`，0600 普通文件、原子替换；不存在或 boot/PID 变化时更新，已一致则不重写。它是 agent 发布 `utc-v2` 所需的身份提示，不是受管状态，不参与 `has_state`/恢复判定，写失败与 status 同级处理（不影响路由或旧表）。

热更新与到期检查在 primary UDP 周期执行，不依赖 TUN 流量。墙钟到期和单调截止均阻断，回拨不能解除已锁存过期。原生 core.flag=2 热重启在 workers 停止后移交 writer，恢复新 boot 与阻断状态，不遗留旧锁或复用旧成功状态。

### 可复现验证与边界

```sh
make -f Makefile.Darwin -j4                  # macOS
make -f Makefile.linux -j4                   # Linux
make -C tests check
make -f Makefile.guard check
make -C tests check CFLAGS='-O1 -g -Wall -Wextra -Werror -std=c99 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer'
# 以下只在隔离 Linux/root 环境运行，使用新 namespace/新密钥，不接触生产：
sudo python3 tests/test_utc_topology.py --binary-dir "$PWD"
sudo python3 tests/test_utc_topology.py --binary-dir "$PWD" --dual --safe-index
```

测试会清理其进程、network namespaces、私钥与 map，保留脱敏日志/UDP 元数据及结果摘要。`test_utc_topology.py` 的 TCP bridge 只是驱动夹具，不是 utcd 或 mTLS/租约实现，不冒充生产 UTC 服务。CI `utc-main-tests` 直接测 main 源码，独立于历史发行通道 pin；没有修改发行 pin、发 tag、上传包或部署。

| 要求 | 代码与证据 |
|---|---|
| D1/D3 收发严格约束 | socket 测试 + 真实加密 GNB 数据报从错误来源重放，拒绝计数增加且 TUN RX 不增加 |
| D2/D9 布局与默认兼容 | node/map 头无差异；真实第三 peer、Index 和原生 gnb_ctl 热重启；未配置 UTC 的第三节点正常 |
| D4/D6 候选与回退 | 定向 DETECT 抑制、最终目标 route 回调覆盖、尾部再检查；真实有 native 候选时选中 pair 无原生 UDP；TCP 中断不回退 |
| D5 UR/Index/LAN 边界 | Index 普通/secure 注册、查询与响应被捕获；UR 组合显式拒绝；LAN 广播无目标排除，定向 LAN_PING 仍受来源 guard |
| D7/D8/D14 配置、热载、状态 | 配置/版本/作用域/坏文件/缺文件测试；空闲到期、boot/PID 与原生热重启；UTC 格式严格版本化；`state-v1`/`state-v2` 解码、epoch 纯续约水位与 `utc.identity` |
| D11 失效与恢复 | 初次/后续更新 × 错误/进程退出 × 24 I/O 边界 × 单副本损坏，共 288 个故障用例；删除、过期、重启不复活 |
| D13 单侧/双侧出口 | 真实 GNB A-S 与 A-D 驱动拓扑，各自验证双向 TUN、稳定端口与 TCP 重连；IPv4/IPv6 socket 收发单测 |
| D10/D12 双端协调边界 | GNB 提供 pair_gen/摘要/boot/APPLIED；prepare/verify/confirm/补偿是 agent/A2 的后续集成，不由驱动伪造全局成功 |

验收范围：本地 macOS 与隔离 Linux 的源码构建、单元/故障注入/真实 GNB 驱动拓扑；不是公网加速证明，不含 UTC 商业授权、mTLS、计量或整条 A-S/A-D 发布验收。完整 UTC 后续仍需用真实 utcd/agent 替换夹具，并核验公网环境与租约/协调联动。

2026-11-04 最终源码验证：A-S 与 A-D/secure Index 各 18 项驱动拓扑断言通过，第三 peer 使用原始 `eed65f8` 二进制；两套构建的 src C/头文件逐字节相同。ASan/UBSan 与 288 个故障注入用例通过。OpenWrt 构建入口已补入 UTC 对象及头依赖，Linux 上相关源码编译通过；完整链接受隔离环境缺少 libz 开发库阻断，未宣称 OpenWrt SDK/设备验收通过。进程退出注入不替代物理断电测试。**2026-11-07 复验：用 OpenWrt 24.10 SDK（armsr-armv8、x86-64）+ 官方 `zlib-dev` 的 `libz.a` 完成 GNB 链接（动态 musl），产物 `Features: proxy utc-conf utc-conf-v1`，在 B（aarch64 ImmortalWRT）与 A（x86_64 OpenWrt）实机启动并创建 TUN；libz 阻断已解除。另用 SDK `ipkg-build` 打包 `mynet-gnb` IPK（x86_64、aarch64_generic），在 A 上 `opkg install`→运行 UTC 策略 APPLIED→`opkg remove` 全流程通过。B 的 ImmortalWRT 使用 apk 3.0（v2 包被拒），已用 apk-tools 3 `mkpkg` 打 v3 APK（aarch64_cortex-a53），在 B 上 `apk add --allow-untrusted`→UTC APPLIED→`apk del` 通过。**

2026-11-04 A2 接线批：按用户要求，将原 gnb_utc_runtime.c/h 的收发与帧检查实现合入 gnb_utc.c/h，worker 保留调用；gnb_utc_service.c/h 保留文件解析、持久 I/O 和单写者管理，避免把磁盘机制塞入 packet 路径。配置/策略/收发故障测试及 Darwin 构建通过；本批完整集成回归仍在推进。

A2 接线续批：启动/关闭、热重启受管断言和 UR/平台能力判断也已收拢至 gnb_utc.c/h；core/CLI 仅调用 attach/detach/require_managed。原始基线 eed65f8，本批父提交 d5d3908。单元/288 个故障用例、Darwin/Linux 构建通过；隔离 Linux 全量 A-S/A-D 各 18 项再次通过（8s4103ux / hbq70z08），真实 agent CLI+utcd 的 A-S smoke 7 项通过（7_t2q5ml）。这些驱动/实验通路证据不代表双端协调、签名租约或 mTLS 已完成。

真实 utcd 重连回归（2026-11-04）：--utcd/--agent 的实验拓扑增至 9 项，包括保持 Client 进程、固定出口恢复双向 TUN、恢复期间无原生 A-B UDP。最新隔离 Linux 证据 `/tmp/gnb-utc-topology-ydvnm0qi`。该夹具仍明确标为 token-lab，不计作完整 A2/公网认证验收；GNB 生产源码本批未变。
