# mynet_gnb 维护与发行执行计划

本文档是 `mynet_gnb` 当前版本管理、补丁维护和发行工作的执行入口。执行 agent 应先阅读本文，再按“未完成任务”顺序推进。

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
