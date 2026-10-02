# opengnb 自有补丁包

本目录存放我们对 opengnb 源码的自有改动。所有包都**按上游 commit 归档**，每个包是一串独立的
`git format-patch` 补丁，基线就是一个明确的上游 commit——因此上游发新版时可以整体重基，而不是
把改动散落在工作区里。

## 目录约定

```
packages/
  <上游版本>-<上游commit短sha>/      # 一个基线 commit 一个目录
    p1-<slug>/                     # 一个包 = 一个独立功能
      patches/0001-*.patch ...
      README.md                    # 包的语义、配置用法、已知限制、验证记录
      apply.sh                     # 依次应用
      revert.sh                    # 依次回滚
    p2-<slug>/
    dist/                          # 打好的 tgz，按需创建（当前没有：旧包 tgz 已随退位包删除）
  README.md                        # 本文件
  rebase.sh                        # 把某个包的补丁串重基到新 commit 的辅助脚本
```

补丁里的路径是相对**上游仓库根**的（例如 `Makefile.inc`、`src/cli/gnb.c`、`libs/...`）。
本仓自根布局改造后，仓库根就是上游根，所以补丁在仓库根以 `-p1` 应用即可；
历史提交（如纯上游同步提交 `2313dea`）把上游整棵树放在 `src/` 下，那种树要另做布局归一。

## 包与基线

**当前只有一个功能包：`p1-route-proxy`，每个基线目录各一份，无前置依赖。**

| 包 | 基线 commit | 状态 | 内容 |
|---|---|---|---|
| `1.6.8-c411f14ee647/p1-route-proxy` | `c411f14ee647`（上游 tag 1.6.8） | **当前采用** | `proxy.conf` 的地区出口策略、准入与 flow 固定；新增 `src/gnb_proxy.c/h`，改 `Makefile.inc`、`Makefile.openwrt.inc`、`src/cli/gnb.c`（`Features: proxy`）、`src/gnb_core.c`、`src/gnb_pf.c`、`src/packet_filter/gnb_pf_route.c`。1 个补丁。语义/限制/验证记录见 [包内 README](1.6.8-c411f14ee647/p1-route-proxy/README.md)。 |
| `1.6.8-dev-74414d1/p1-route-proxy` | 作者开发快照（ZIP SHA256 `1daa8c78cb07…`，导入 `74414d1`） | **当前采用**（开发线，只出内部包） | 同一功能，按开发线基线重卷的独立补丁串。1 个补丁。 |

**已删除的退位包（2026-10-02，不留档）**：`p1-hot-restart-v2`（5 个补丁）、
`p1-hot-restart`（4 个补丁）、`p2-route-proxy`（proxy 的旧包，编号与前置都属于旧补丁链）。
热更新退位的原因不是它不好用，而是**上游 1.6.8 自带热更新**——`gnb_hot_restart_prepare`
由纯上游同步提交 `2313dea`（提交信息即 `sync(src): opengnb/opengnb@1.6.8`）引入，我们的包是冗余的；
`p2-route-proxy` 的功能已由 `p1-route-proxy` 承接，其 `tests/Makefile` 还指着旧嵌套路径
`../../../../src/src/gnb_proxy_conf.c`，本来就**不可运行**。三个包目录连同 `p2-route-proxy/itest/`
（实机证据脚本与节点备份）一并删除，历史在 git 里，需要对照时从提交 `65d951f` 之前的树里取。


## 应用与回滚

```bash
cd <repo>                                        # 仓库根 = 上游根布局
sh scripts/apply_packages.sh                      # 自动挑选与 src/version 相符的基线
sh scripts/apply_packages.sh --packages packages/1.6.8-dev-74414d1   # 两个 1.6.8* 基线并存时要显式指定
```

**只有一个包，没有前置**：`p1-route-proxy` 直接打在纯上游上。这一点是今天实测出来的，
不是照抄包 README——包 README 原先写着「前置 = `p1-hot-restart-v2`」，而实测
`git apply --check` 在纯上游 1.6.8（根布局）上退出码 0、`apply_packages.sh` 干净应用后
`make -f Makefile.Darwin install` 0 error 出齐四程序、`gnb -v` 报 `Features: proxy`、
`src/gnb_ctl_block.h` 里 `gnb_core_flag` 命中数为 0（即产物不含 P1 v2 的 ctl 改动）。
两条链（主线与开发线）各跑一遍包内单测，均 **52 项通过**。

守护在 `scripts/check.sh` 的 `test` 段：每个带补丁包的通道都在**其 `local_source_commit`
导出的纯上游树**上实跑「应用 → 标记落地 → 复跑幂等 → 包内单测」。之所以不用当前工作区，
是因为 HEAD 的 `src/` 已经含 proxy 代码，在其上打补丁只能测出幂等，测不出这条不变量。

**注意，不要手工写 `git apply -p1`**：`git apply` 在 git 仓库内是按**仓库根**解析补丁路径的，
一旦目标树嵌在仓库子目录里（历史布局的 `<repo>/src/`），`-p1` 会直接报 `No such file or directory`；
更危险的是 `-p2` 会返回 0 却只打印 `Skipped patch`，什么都不改。所以一律用 `apply_packages.sh`，
它会用 `git rev-parse --show-prefix` 自动补 `--directory=<前缀>`，并把日志里出现
`Skipped patch` / `应用失败` 当成失败退出。


## 已评估、暂不收录

| 候选 | 来源 | 结论 |
|---|---|---|
| ctl block 64 位扩容（`entry_table256` 改 `uint64_t*`、`gnb_block64_t`、`gnb_ctl_block_size()`、`node_num` 加宽） | 作者快照 C | **不做**。实测 `sizeof(gnb_node_t)` = 3480 B，256 节点仅 0.8 MB；要靠节点数触到 32 位偏移上限需约 **123 万个节点**，而 `node_num` 上限就是 route.conf 的行数。收益为零，代价却是与 P1 改同一块共享内存布局（两包互相冲突、每次上游变基都要人肉解），并放大「gnb/gnb_ctl/gnb_es 必须同一份构建」的风险面。将来真需要时可按当时的上游 commit 重新抽取——它是自包含的一组（`gnb_block64_t` / `gnb_ctl_block_size` / `ALIGN_UP_8` / `gnb_def.h` 里的宏）。 |
| 运行时日志热配置（`core.log.*` 一族 ctl 键 + `core_zone->log_trigger`） | 作者快照 C | **已作废，不进入当前补丁链**。现行补丁只有 `p1-route-proxy` 一个包；热更新也不再由我们提供（上游 1.6.8 自带）。 |
| `gnb_mmap` 的 `MAP_FAILED` 判定修复 | 作者快照 C | **已并入 P1 v2**（补丁 0005）。 |
| 日志归档文件名带秒（`*_%Y_%m_%d_%H.%M.%S.arc.log`） | 作者快照 C | 并入 P3（P3 会重做 log ctx）。 |
| Windows 默认驱动 TAP→DEFAULT、wintun 读线程 50 ms 轮询退出 | 作者快照 C | 待定，仅在 Windows 生效；`build_windows.yml` 在跑，需要发 Windows 版时再收。 |

## CI / 发版：出包不再 apply 补丁

发行口径已经改成：**Actions 只编译 `packages/RELEASE-SOURCES` 里为每个通道固定的
`release_source_commit`**，不再在构建前 apply 补丁。

```
每个通道的 job：
  git archive <release_source_commit> | tar -x     # 导出已固定的发行源码树
  → 校验源码身份（无 src/src/、version 相符、src/gnb_proxy.c 在不在、Features 行在不在）
  → 编译四程序（gnb / gnb_ctl / gnb_es / gnb_crypto）
  → strip → 能力验收 → MANIFEST + SHA256SUMS → 打包
```

所以 workflow 里**没有** `Apply packages` 这一步了，`scripts/apply_packages.sh` 只服务源码
准备线（切下一个 `release_source_commit` 之前，用它证明补丁串还能干净打上去）。
构建与校验的实现在 `scripts/gnb-dist.sh`，通道字段读 `scripts/release-sources.sh`，
包命名与发布分级见 `RELEASE-MATRIX.md`。

- **`release_source_commit` 为 `UNSET` 的通道必须失败**：`gnb-dist.sh` 与 `release-sources.sh require`
  会显式报错，不会替你推断或生成一个源码提交。当前 `main` 与 `x-dev-patched` 就是这个状态。
- **`main` 缺的是「纯上游 + 根布局」那一个提交**：`2313dea` 的内容是根布局（`Makefile.inc` 在其 `src/` 下），
  但在那个提交里它整体挂在仓库的 `src/` 下，`make -f Makefile.linux` 找不到入口。
- **`main-patched` 现在指向的 `cbe619d` 仍含 P1 v2 的改动**（`MAP_FAILED` 0→1、
  `core_flag` 4→10、`run_flag = 0` 5→13，相对纯上游 `2313dea`）。按「热更新退位、只留 proxy」
  的口径，它应当被一个「纯上游 + `p1-route-proxy`」的新提交替换；替换内容今天已实测成立
  （见上「应用与回滚」），只差把它切成提交。**在它被替换之前，CI 出的 `main-patched` 产物里
  是带着 P1 v2 那份 ctl 改动的**——这一点别用文档口径掩盖掉。

## 现行包的行为变更提示（升级前必读）

- **准入白名单会从「形同虚设」变成真的生效**：上游 1.6.8 的准入检查返回 `GNB_PF_NOROUTE`
  后被分发循环忽略，包照样写 tun；`p1-route-proxy` 把它修成与 route 阶段一致。升级后原本能
  通过的未登记目的包会被丢弃。要保留旧行为就在该节点打开 `proxy on`，或不采用本包。
  （结论来自旧 P2 包的同源改动实测 + 现包 README 里「`client` 的 `proxy on` 不自动开启准入」
  与「client 不自动放宽准入」单测项；现包在这条上的独立实测以 [包内 README](1.6.8-c411f14ee647/p1-route-proxy/README.md) 为准。）
- **`gnb` / `gnb_ctl` / `gnb_es` / `gnb_crypto` 必须来自同一份构建**：现包不碰 ctl block 共享内存
  布局（实测纯上游 + 现包的树里 `gnb_core_flag` 命中 0），但 ctl 键集与结构体定义仍出自同一份源码，
  跨通道混用二进制会解析错位。

## 历史注意事项（退位包，包目录已删除）

下面这些结论属于已退位的 P1 v2 / 旧 P2 / P3，**不再适用于当前发行产物**。包目录本身已在
2026-10-02 删除，留档只留这一段结论：回溯当时为什么这么判，以及将来要重新采用时不必重做实验。

- **热重启的 fd 泄漏、`safe_index` 挂死、`gnb_mmap` 误判这三处缺陷已在 P1 v2 修掉**（v1 / 作者快照 B、C 都没有修）：
  - `gnb_log_udp_open()` 每次建 core 新开 2 个 UDP socket（外加日志文件 fd）却不关闭 → v2 在旧 core 的 release
    路径上接入了 `close_log()`。实测连续 3 次重启，总 fd 稳定 13/13/13/13、UDP 5/5/5/5（修前是 13→15→17→19）。
  - `gnb_secure_index_worker` / `gnb_secure_index_service_worker` 的线程只把 `run_flag` 置 1 不置 0，
    而停机流程会阻塞等它变 0 → v2 补了置 0。实测 `safe-index on` 下能走完 `hot restart finish!` 并续跑
    （未修版本会卡在 `WAIT worker [gnb_secure_index_worker] stoped...`，CPU 100%、日志不再推进）。
  - `gnb_mmap()` 用 `NULL` 判断 `mmap` 失败 → v2 改成 `MAP_FAILED`。
- **P1 v2 的残留限制**：① ctl block 布局变了（`core_zone` 多了 `gnb_core_flag`），`gnb` / `gnb_ctl` / `gnb_es`
  必须来自同一份构建，混用会解析错位；② 重启期间会短暂出现 `xxx.old` map 文件，正常结束会删掉，异常退出可能残留；
  ③ index-service 模式与 lite 模式不支持热重启；④ v2 采用作者 C 版的「先停后建」，若这期间重读配置或建新 core 失败，
  进程会停在半停机状态并留下 `.old` 文件（v1 的「先建后停」在同样情况下旧核心还在跑）；⑤ 每个 core 的日志 UDP
  socket 绑的是 `*:0`，所以重启后它的 fd 与临时端口必然会换新（总数不增长），对"既有监听端点"的断言才成立。
- **P2 会让准入白名单从"形同虚设"变成真的生效**（默认行为变更）：上游 1.6.8 的准入检查返回
  `GNB_PF_NOROUTE` 后被分发循环忽略，包照样写 tun；P2 把它修成与 route 阶段一致。升级后
  原本能通过的未登记目的包会被丢弃。要保留旧行为就在该节点打开 `proxy on`，或不采用 P2。
- **P3 的热配置值不会跨热重启保留**：P1 v2 的重启路径会重新读 `node.conf` 并 `memcpy` 覆盖共享内存里的
  `conf_st`，所以用 `core.log.*` 改过的值在热重启后会回退到配置文件里的值（实测 `console_log_level[1]→[2]`）。
  想让某个值长期生效就写进 `node.conf`；只当临时排障手段用就没关系。
- **P3 换日志目录时目标目录必须已存在且可写**：`open()` 失败时 fd 保持 `-1`，文件日志会中断。
  P3 的补丁 0006 已把它从"静默"改成**显式告警**
  （`WARNING log file not opened, log_path[...] std_fd[-1] ...`，`GNB_LOG1` + `fprintf(stderr)`）；
  注意 daemon 模式下 stderr 会被接到 `/dev/null`，只剩 `GNB_LOG1` 那条通道可见。
  实测：目录不存在时出告警、补建目录后再切一次即恢复。运维脚本换目录前仍应先 `mkdir -p`。
- **P3 改了归档文件名**（行为变更）：后缀从 `.log.arc` 变成 `.arc.log`，且文件名带秒
  （`std_2026_09_21_01.32.36.arc.log`）。仓库内没有引用，但外部按旧名清理日志的脚本要同步。
- **P3 把布局影响扩大到了 conf zone**（新增 `log_verbose`/`log_trace`），「`gnb` / `gnb_ctl` / `gnb_es`
  必须同一份构建」这条约束依然成立，且比只有 P1 v2 时更容易踩到。

## 上游发新版后怎么迭代

```bash
# 把某个包的补丁串重基到新基线（脚本会 clone 上游、checkout 新 ref、git am -3 依次应用）
./packages/rebase.sh 1.6.9 packages/1.6.8-c411f14ee647/p1-route-proxy

# 冲突时脚本会停下并打印处理提示；解决后 git am --continue
# 全部应用成功后，脚本会打印如何把新补丁串导出为 packages/<新版本>-<新sha>/p1-.../patches/
```

重基完成、并重新跑过编译与验证后，请在 `packages/<新版本>-<新commit>/` 下新建目录存放新补丁串，
并在上面的表格里加一行；旧目录保留，便于回溯。

## 记录约定

每个包的 `README.md` 至少写清：

1. 基线 commit 的完整 sha；
2. 每个补丁改了什么、为什么；
3. 配置项与使用方式（含可直接复制的例子）；
4. **已知限制与行为变更**（尤其是会让既有部署行为发生变化的地方，要显式警告）；
5. 验证记录：编译结果、实测做了什么、哪些没做及原因。

**改了包内文件（补丁、README、脚本）之后必须重打 tgz，并复核一致性**：

```bash
cd packages/<版本>-<sha>
mkdir -p dist
tar -czf dist/<包名>-<版本>-<sha>.tgz <包名>
mkdir -p /tmp/tc && tar -xzf dist/<包名>-<版本>-<sha>.tgz -C /tmp/tc && diff -r /tmp/tc/<包名> <包名>
```

（tgz 是交付件，与包目录不一致等于发出去的包和仓库里的不是一份东西。曾经踩过一次：
改了 README 与脚本但忘了重打，结果 p2/p3 的 tgz 落后于目录。）
