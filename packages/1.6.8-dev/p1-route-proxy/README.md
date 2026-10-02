# 本补丁 route-proxy：`proxy.conf` 出口策略补丁

本包是 MyNet 对 opengnb 1.6.8 的代理数据面改动。入口节点先由系统路由决定哪些流量进入 gnb；gnb 对未命中 `route.conf` 的 IPv4 目的，按 `proxy.conf` 的地区策略选择出口节点。出口节点通过 `ingress_open` 接收公网目的流量，之后由宿主系统转发和 NAT。

完整产品语义和计划验收见 [`mynet_ctl/docs/knowledge-base/spec/proxy-conf-design.md`](../../../../mynet_ctl/docs/knowledge-base/spec/proxy-conf-design.md)；本 README 只记录**这个补丁包实际包含什么、如何验证和如何升级**。设计文档的计划项不能当成本补丁已实现的证据。

## 基线、依赖和文件

| 项目 | 当前值 |
|---|---|
| 基线 | 作者开发快照：当前 ZIP SHA256 `2454c303dabdb5e27a13a3fe19b0c4d5066a845d12b75e80172afa84e990c1ae`，本仓未打 proxy 补丁的基线提交 `98f8b4ef16afc11a0452b5e76b9b0f1ddb84f9c1`。这份补丁串是在上一快照（ZIP `1daa8c78cb07…`，导入 `74414d12787fbd9ab36a486d0a43534f54bee470`）上制作的，proxy 改动本身未变；新快照上已重验干净应用（见下方验证记录）。`version` 仍写 1.6.8，但它**不是**上游 tag 1.6.8 —— 上游发行版基线那条线的同名孪生包已于 2026-10-02 删除（删除前逐文件比对，正文与本包完全相同，只差一行路径注释），两条基线上下文不同、不可互换，本目录只服务开发线。 |
| 前置 | **无**。本包直接打在干净基线上（`apply.sh` / `revert.sh` 里也从来没有 P1 检测）。 |
| 补丁 | `patches/0001-proxy-conf.patch`（proxy.conf 主链），其中 `Features: proxy` 已合并在同一个 patch 内（`Features: proxy` 能力行），为相对**干净基线**的 diff |
| 应用顺序 | 干净基线 → 本补丁；回滚即回滚本补丁 |
| 废弃内容 | 旧包的 `node.conf proxy`、`--proxy`、f 节点兜底补丁和 P3 日志热配置均不在当前包中；热更新整条已退位（上游 1.6.8 自带 `gnb_hot_restart_prepare`，由同步提交 `2313dea` 引入） |

> 「前置 = `p1-hot-restart-v2`」是这份 README 此前的说法，已按 2026-10-02 的实测更正：
> 在 `74414d1:src` 导出的干净基线（根布局）上，`git apply --check` 退出码 0，`apply_packages.sh`
> 干净应用、复跑幂等、包内单测 **52 项通过**。注意这份基线里 `gnb_core_flag` 命中 1、
> `gnb_hot_restart_prepare` 命中 1 —— 那是**作者快照自带**的，不是本补丁引入的，别把它当成
> P1 v2 复活。守护见 `scripts/check.sh` 的 `test` 段（每个带补丁包的通道都实测一遍）。
> 重基时**不要**再去把 P1 v2 打回来。


补丁修改 `Makefile.inc`、`Makefile.openwrt.inc`、`src/gnb_core.c`、`src/gnb_pf.c`、`src/packet_filter/gnb_pf_route.c`、`src/cli/gnb.c``src/cli/gnb.c` 输出机器可读 `Features: proxy` 行，供 agent 探测，新增 `src/gnb_proxy.c/h`。Windows Makefile 复用 `Makefile.inc`。作者基线自带的其它功能不由本补丁重复实现；本补丁 不应复制 P1 或 P3 的日志、socket、ctl block 改动。

## 配置与数据路径

文件位于 `conf/<nodeid>/proxy.conf`，与 `route.conf` 同目录。典型 client：

```ini
role client
proxy on

[default]
exit 4997342800601732 w=20

[group us]
target 8.8.8.0/24
exit 8283289760467957 w=10 order=1
```

纯出口节点：

```ini
role server
proxy on
ingress_open on
```

`role both` 同时承担两种角色。group 描述目的地区域策略，**不表示** client/server 角色。多数已由系统路由送入 gnb 的流量可落到 `[default]`；group 用于少数需要不同地区出口的目的。`route.conf` 命中优先于 proxy 策略。`target` 当前只支持 IPv4 CIDR，`/0` 被拒载；组内无出口时丢包，不回落 default。`ingress_open` 缺省为 off。`proxy.conf` 删除时发布 disabled 快照，语法错误时保留上一版。

### 选路语义（已实现）

| 规则 | 行为 |
|---|---|
| 匹配 | 组内最长前缀 → 组间最长前缀（同前缀取书写序先者）→ 无 target 命中落 `[default]` |
| order | **最小 order 层优先**（`order=0` 是合法最低层）；该层无可用出口才降到下一层 |
| weight | 同层内按 `w=` 加权（线程安全伪随机） |
| 可用 | 在 `uuid_node_map` + 有 tun 地址 + `GNB_NODE_STATUS_IPV*_PONG` 且时间戳 ≤ `GNB_PROXY_PONG_TTL_SEC`（60s） |
| 粘性 | 按连接（src/dst/proto/sport/dport）粘住出口；TCP 300s / UDP 180s / ICMP 60s idle |
| 分片 | 独立分片缓存，按 `(src,dst,proto,ip_id)` 关联出口，30s 过期，256 条 |
| 容量 | 连接表扫描上限按 `node.conf` 的 `memory`：tiny 512 / small 1024 / large·huge 2048（数组静态预留 2048） |
| 锁 | 配置 `pthread_rwlock`（Windows `CRITICAL_SECTION`）；**锁内只读快照**，节点/PONG 在锁外查 |

`ingress_open` 用于 `pf_inet_fwd_cb`：开启后跳过 `route.conf` 转发白名单。`client` 的 `proxy on` **不**自动开启准入。

## 应用、回滚和构建

在 `mynet_gnb` 仓库根目录使用统一入口（它是**源码准备线**的工具；CI 出包不再 apply 补丁，
而是按 `packages/RELEASE-SOURCES` 里已固定的 `release_source_commit` 直接构建）：

```sh
sh scripts/apply_packages.sh                       # 只有一个包，无前置
make -f Makefile.Darwin install
```

单独操作本补丁时，目标只要是一棵干净的上游树即可：

```sh
bash packages/1.6.8-dev/p1-route-proxy/apply.sh .
bash packages/1.6.8-dev/p1-route-proxy/revert.sh .
```

`apply.sh`/`revert.sh` 会处理路径前缀；不要在仓库里直接跑未经调整的 `git apply -p1`。
准备下一个发行源码提交时，必须从**干净上游树**应用本补丁，不要拿本地已经改过的 `src/` 当基线。

## 测试

单元测试在 `tests/`，**链接真实的 `src/gnb_proxy.c`**（不复制算法）：

```sh
make -C packages/1.6.8-dev/p1-route-proxy/tests
```

覆盖：role/ingress 解析与拒载、`/0` 与残缺段头、删文件→disabled、语法错保留上一版、最长前缀与 default 兜底、组内无出口不落 default、order 降层、tun/PONG/过期/未注册可用性、五元组粘性、分片 ip_id 跟随、client 不自动放宽准入。

## 已验证与待验证

**下表除标明日期的两行外，全部是在**上游 1.6.8 基线**（同功能的孪生包
`packages/1.6.8-c411f14ee647/`，2026-10-02 已删除，正文与本包逐字节相同）上实测的结果，
不是本开发线基线的**：本线 2026-10-02 起才有 `release_source_commit`（`packages/RELEASE-SOURCES`
里现在钉着 `e0228d9`），各平台完整构建仍要等 CI 在那个提交上真跑一次才算数；
下面这两行是本地与测试机跑的证据，说明改动成立，不替代 CI。

截至 2026-10-02：

| 项 | 结果 |
|---|---|
| `patch --dry-run` / `git apply --check` 在**干净基线**副本 | ✅ 退出码 0（2026-10-02，基线提交 `98f8b4e` 的导出树；更早一次在上一快照 `74414d1:src` 的根布局树上同样过） |
| `scripts/check.sh` 补丁链实跑 + 幂等 + 单元测试 | ✅ 本基线 2026-10-02 实测：干净应用、复跑幂等、单测 52/52 |

| `tests/` 单元测试 | ✅ 52/52 |
| macOS `Makefile.Darwin`（arm64） | ✅ `gnb` `gnb_ctl` `gnb_crypto` `gnb_es` |
| Windows `Makefile.mingw_x86_64`（x86_64-w64-mingw32） | ✅ `gnb.exe` `gnb_ctl.exe` `gnb_crypto.exe` `gnb_es.exe` |
| Linux `Makefile.linux`（x86_64 glibc，192.168.0.32） | ✅ 四程序，`ELF 64-bit x86-64` |
| OpenWrt `Makefile.openwrt` x86-64 musl（SDK 24.10） | ✅ 四程序，`ld-musl-x86_64` |
| OpenWrt armsr-armv8 musl（SDK 24.10） | ✅ `gnb`，`ELF aarch64` |
| OpenWrt ramips-mt7621 musl（SDK 24.10） | ✅ 四程序，`ELF MIPS32` |

以下仍需实机数据面验收（配置热更新并发、真实 NAT 回程、表满淘汰、多 worker）：

- `proxy.conf` 的启动加载、修改、删除、错误保持、client/server/both 准出与准入，以及真实 NAT 回程。
- 单连接跨热更新保持出口、IPv4 分片跟随、表满淘汰、多 worker 并发。

### v3 优化（2026-09-30 真双机后）

1. **`gnb_proxy_is_client()`**：server/both 之外不进准出选路——server 上组播/广播不再刷 `no usable proxy exit`（实测噪声 38→0）。
2. **失败日志限速**：pick 失败每秒最多 1 条，避免 DROP 风暴。
3. **order 分层**：唯一 order 一次排序，再按层扫描（去掉原先每层重算）。

### v3 相对旧实现的修正（已进补丁）

1. **Windows 锁**：全部走 `proxy_rdlock/wrlock/unlock` 宏；Unix 用 `PTHREAD_RWLOCK_INITIALIZER`，Windows 在首次 load 时 `InitializeCriticalSection`。
2. **日志时序**：`gnb_proxy_cfg_load` 移到 `gnb_log_ctx_init` 之后，加载/解析失败日志不再被丢弃。
3. **出口可用性**：`uuid_node_map` + tun 地址 + PONG 位 + 60s 时间戳；**锁外**查节点，锁内只读配置快照。
4. **order 分层**：`order=0` 是最低层；最小层全不可用才降层；不再把 0 当成「永远可选」。
5. **分片**：独立 256 条分片缓存（30s），与五元组连接表分离。
6. **解析**：残缺段头（缺 `]`）拒载；`proxy`/`role`/`exit`/`target` 整词匹配，避免 `proxy_foo` 误命中。

## 接作者新版（快照或上游发行版）

1. 把新版源码**原样**导入成一个纯源码提交（作者 ZIP 只取 `gnb/` 内容，丢掉 macOS 元数据条目；
   ZIP SHA256 记进 `sources/author-dev/<版本>-<短SHA>/SOURCE.yaml`）。这个提交就是新的基线候选。
2. 在它之上重放 proxy 改动，自成**一个提交**。检查上述目标文件的每个 hunk，尤其
   `gnb_core_create`、`pf_tun_frame_cb`、`pf_inet_fwd_cb` 和两个 Makefile；不要从含其他实验改动的
   工作树直接导出 diff。热更新若在新版上有变化，那是作者侧的事，本包不重复实现。
3. `git diff <新基线>..<新发行提交>` 必须还是那 8 个文件 —— 第 9 个文件就是混入别的东西。
   然后把补丁串重新导出到本目录（目录按线命名，不必跟着快照改名）：
   `git format-patch <新基线>..<新发行提交> --stdout > patches/0001-proxy-conf.patch`。
4. 更新 `packages/RELEASE-SOURCES` 的 `local_source_commit` / `release_source_commit` 与本 README 的
   基线、文件清单、验证记录，`make -f Makefile.guard check` 全绿后合进 `main`、重打 tag 让 CI 出包。
   未验证通过就不改钉值：发行工具只认那个提交，不会替你生成，也不会替你兜底。

事实来源分三层，别混：`main` 的**提交历史**保存源码改动（基线提交 + proxy 提交），
本目录的补丁串只是从提交导出的载体，`RELEASE-SOURCES` 保存「哪个已固定的提交可以出包」。
工作区 `src/` 的当前状态既不是下一个版本的基线，也不是出包依据。
