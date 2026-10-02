# opengnb 自有改动与补丁包

**真相是提交，补丁串是导出件。** 我们在作者源码上的改动以两个提交存在：

```
作者快照（ZIP 逐字节导入）        →  基线提交   packages/RELEASE-SOURCES: local_source_commit
基线提交 + proxy 提交（8 个文件）  →  发行提交   packages/RELEASE-SOURCES: release_source_commit
```

`packages/<基线>/p1-route-proxy/` 存的是从这两个提交**导出**的 `git format-patch` 串、包内单测和
语义记录。CI 出包从不 apply 它（它按 `release_source_commit` 直接构建），它服务两件事：
给守护当「纯基线树还能重放出这套改动」的证据，以及给将来对照/迁移留一份自足的载体。

## 目录约定

```
packages/
  <版本>-<线>/                      # 按线命名，不按快照命名：作者再发快照不必改目录
    BASELINE                        # 该线的基线提交 / ZIP SHA256
    p1-route-proxy/
      patches/0001-*.patch
      README.md                     # 包的语义、配置用法、已知限制、验证记录
      apply.sh  revert.sh           # 逐条应用 / 回滚（bash）
      tests/                        # 包内单测（52 项，在导出树里跑）
  README.md                         # 本文件：包索引
  RELEASE-SOURCES                   # 通道 ↔ 提交 SSOT（只用 scripts/release-sources.sh 读）
```

补丁里的路径相对**上游仓库根**（`Makefile.inc`、`src/cli/gnb.c`、`libs/...`）。本仓已是根布局，
所以在仓库根以 `-p1` 应用即可；历史提交（如 `2313dea`）把上游整棵树放在 `src/` 下，那种树要另做
布局归一。

## 包与基线

**当前只有一个功能包：`p1-route-proxy`，无前置依赖。**

| 包 | 基线 | 状态 | 内容 |
|---|---|---|---|
| `1.6.8-dev/p1-route-proxy` | 作者开发快照，基线提交 `98f8b4ef16af`（ZIP SHA256 `2454c303dabd…`），发行提交 `e0228d937d7f` | **当前采用**（生产发行） | `proxy.conf` 的地区出口策略、准入与 flow 固定；新增 `src/gnb_proxy.c/h`，改 `Makefile.inc`、`Makefile.openwrt.inc`、`src/cli/gnb.c`（`Features: proxy`）、`src/gnb_core.c`、`src/gnb_pf.c`、`src/packet_filter/gnb_pf_route.c`。1 个补丁，8 个文件。语义/限制/验证记录见 [包内 README](1.6.8-dev/p1-route-proxy/README.md)。 |

**已删除的退位件（2026-10-02，不留档）**：

- `packages/1.6.8-c411f14ee647/p1-route-proxy` —— 同一个功能打在**上游 1.6.8 发行版**基线上的孪生串。
  删除前逐文件比对过：补丁正文、`apply.sh`、`revert.sh`、`tests/` 与现包**逐字节相同**，
  唯一差异是 `tests/test_proxy_conf.c` 头部那行 `make -C` 的路径注释。两条基线（发行版 / 开发快照）
  的上下文不同、不可互换，所以只保留当前采用那条线的包。
- `p1-hot-restart`（4 个补丁）、`p1-hot-restart-v2`（5 个）、`p2-route-proxy`（proxy 的旧包，编号与
  前置都属于旧补丁链），连同 `p2-route-proxy/itest/`（实机证据脚本与节点备份）一并删除。热更新退位
  不是因为不好用，而是**上游开发包自带热重启**；`p2-route-proxy` 的功能已由 `p1-route-proxy` 承接，
  其 `tests/Makefile` 还指着旧嵌套路径 `../../../../src/src/gnb_proxy_conf.c`，本来就不可运行。
  历史在 git 里，需要对照时从当时的树取；下面「历史注意事项」保留结论，免得重做实验。
- 基线台账 `packages/BASELINE`、`packages/BASELINE-XDEV`（`PATCH_STACK` 那套）与重基辅助脚本
  `packages/rebase.sh`。它们属于「补丁是真相」的旧口径；现在接新快照的动作是重放提交，
  不是 rebase 补丁文件（步骤见 `packages/RELEASE-SOURCES` 注释）。

## 应用与回滚

```bash
cd <repo>                                              # 仓库根 = 上游根布局
sh scripts/apply_packages.sh                            # 按 src/version 自动挑基线目录
sh scripts/apply_packages.sh --packages packages/1.6.8-dev   # 显式指定
```

守护在 `scripts/check.sh` 的 `test` 段（`make -f Makefile.guard check`）：每个带补丁包的通道都在
**其 `local_source_commit` 导出的纯源码树**上实跑「应用 → 标记落地 → 复跑幂等 → 包内单测」。
之所以不用当前工作区，是因为 HEAD 的 `src/` 已含 proxy 代码，在其上打补丁只能测出幂等，
测不出「纯基线 + 补丁串仍然成立」这条不变量。

**不要手工写 `git apply -p1`**：`git apply` 在 git 仓库内按**仓库根**解析补丁路径，目标树嵌在仓库
子目录里时 `-p1` 会直接报 `No such file or directory`；更危险的是 `-p2` 返回 0 却只打印
`Skipped patch`，什么都不改。所以一律走 `apply_packages.sh` —— 它用 `git rev-parse --show-prefix`
自动补 `--directory=<前缀>`，并把日志里出现 `Skipped patch` / `应用失败` 当失败退出。

## 升级前必读的行为变更

- **准入白名单会从「形同虚设」变成真的生效**：上游的准入检查返回 `GNB_PF_NOROUTE` 后被分发循环忽略，
  包照样写 tun；`p1-route-proxy` 把它修成与 route 阶段一致。升级后原本能通过的未登记目的包会被丢弃。
  要保留旧行为就在该节点打开 `proxy on`，或不采用本包。
- **`gnb` / `gnb_ctl` / `gnb_es` / `gnb_crypto` 必须来自同一份构建**：现包不碰 ctl block 的共享内存
  布局，但 ctl 键集与结构体定义出自同一份源码，跨通道混用二进制会解析错位。发行由单个 job 同批构建
  四程序来保证。

## 已评估、暂不收录

| 候选 | 来源 | 结论 |
|---|---|---|
| ctl block 64 位扩容（`entry_table256` 改 `uint64_t*`、`gnb_block64_t`、`gnb_ctl_block_size()`、`node_num` 加宽） | 作者快照 | **不做**。实测 `sizeof(gnb_node_t)` = 3480 B，256 节点仅 0.8 MB；要靠节点数触到 32 位偏移上限需约 **123 万个节点**，而 `node_num` 上限就是 route.conf 的行数。收益为零，代价却是与 proxy 改同一块共享内存布局（两边互相冲突、每次换基线都要人肉解），并放大「四程序必须同一份构建」的风险面。真需要时按当时的基线重新抽取——它是自包含的一组（`gnb_block64_t` / `gnb_ctl_block_size` / `ALIGN_UP_8` / `gnb_def.h` 里的宏）。 |
| 运行时日志热配置（`core.log.*` 一族 ctl 键 + `core_zone->log_trigger`） | 作者快照 | **不进入我们的改动**。热更新现由作者开发包原生提供。 |
| `gnb_mmap` 的 `MAP_FAILED` 判定、日志 UDP fd 关闭、secure-index worker 退出标志 | 作者快照 | **作者侧已修**（`gnb_log_udp_close`、`MAP_FAILED` 判失败、第二包里补上的 `thread_worker_run_flag = 0`），我们不再自己打这三处。 |
| Windows 默认驱动 TAP→DEFAULT、wintun 读线程 50 ms 轮询退出 | 作者快照 | 作者原生行为，我们不覆盖；`build_windows.yml` 在跑，需要发 Windows 版时再验。 |

## 历史注意事项（退位包，包目录已删除）

下面这些结论属于已退位的 P1 v2 / 旧 P2 / P3，**不适用于当前发行产物**。留着是为了回溯当时为什么
这么判，以及将来重新采用时不必重做实验。

- **热重启的 fd 泄漏、`safe_index` 挂死、`gnb_mmap` 误判这三处缺陷当时由 P1 v2 修掉**（作者当时那份快照
  没修）：
  - `gnb_log_udp_open()` 每次建 core 新开 2 个 UDP socket（外加日志文件 fd）却不关闭 → v2 在旧 core 的
    release 路径接入 `close_log()`。实测连续 3 次重启，总 fd 稳定 13/13/13/13、UDP 5/5/5/5（修前 13→15→17→19）。
  - `gnb_secure_index_worker` / `gnb_secure_index_service_worker` 的线程只把 `run_flag` 置 1 不置 0，
    而停机流程阻塞等它变 0 → v2 补了置 0。实测 `safe-index on` 下能走完 `hot restart finish!` 并续跑
    （未修版本卡在 `WAIT worker [gnb_secure_index_worker] stoped...`，CPU 100%、日志不再推进）。
  - `gnb_mmap()` 用 `NULL` 判断 `mmap` 失败 → v2 改成 `MAP_FAILED`。
- **P1 v2 的残留限制**：① ctl block 布局变了（`core_zone` 多了 `gnb_core_flag`），四程序必须同一份构建；
  ② 重启期间会短暂出现 `xxx.old` map 文件，正常结束会删掉，异常退出可能残留；③ index-service 与 lite
  模式不支持热重启；④ 采用「先停后建」，这期间重读配置或建新 core 失败会停在半停机并留下 `.old`
  （「先建后停」在同样情况下旧核心还在跑）；⑤ 每个 core 的日志 UDP socket 绑 `*:0`，所以重启后它的
  fd 与临时端口必然换新（总数不增长），对「既有监听端点」的断言才成立。
- **P2 会让准入白名单从「形同虚设」变成真的生效**（默认行为变更），同现包那条。
- **P3 的热配置值不会跨热重启保留**：重启路径会重读 `node.conf` 并 `memcpy` 覆盖共享内存里的 `conf_st`，
  所以用 `core.log.*` 改过的值热重启后回退到配置文件值（实测 `console_log_level[1]→[2]`）。想让某个值
  长期生效就写进 `node.conf`；当临时排障手段用没关系。
- **P3 换日志目录时目标目录必须已存在且可写**：`open()` 失败时 fd 保持 `-1`，文件日志中断。补丁 0006
  把它从静默改成显式告警（`WARNING log file not opened, log_path[...] std_fd[-1] ...`）；daemon 模式下
  stderr 接到 `/dev/null`，只剩 `GNB_LOG1` 那条通道可见。运维脚本换目录前仍应先 `mkdir -p`。
- **P3 改了归档文件名**（行为变更）：后缀 `.log.arc` → `.arc.log` 且带秒
  （`std_2026_09_21_01.32.36.arc.log`）。仓内无引用，外部按旧名清理日志的脚本要同步。
- **P3 把布局影响扩大到 conf zone**（新增 `log_verbose`/`log_trace`），「四程序必须同一份构建」这条
  比只有 P1 v2 时更容易踩到。

## 接作者新版怎么迭代

不再重基补丁文件。固定动作（全文见 `packages/RELEASE-SOURCES` 注释）：在新快照的纯源码提交上做
基线提交 → 在其上重放 proxy 提交 → `git merge --no-ff` 进 `main` → 更新 `RELEASE-SOURCES` 的两个
钉值 → 重打 tag 让 CI 出包。补丁串作为派生产物重导一次：

```sh
git format-patch <新基线提交>..<新发行提交> --stdout > packages/1.6.8-dev/p1-route-proxy/patches/0001-proxy-conf.patch
sh scripts/apply_packages.sh --packages packages/1.6.8-dev   # 在干净副本上验重放
```

## 记录约定

每个包的 `README.md` 至少写清：基线提交的完整 SHA 与 ZIP SHA256；每个补丁改了什么、为什么；配置项与
可直接复制的例子；**已知限制与行为变更**（尤其会让既有部署行为变化的地方要显式警告）；验证记录
（编译结果、实测做了什么、哪些没做及原因）。
