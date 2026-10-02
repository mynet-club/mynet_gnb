#!/bin/sh
# check.sh — mynet_gnb 的真检查（lint / test）
#
# 本仓是 opengnb fork，核心不变量（2026-10-02 开发者裁定）：
#   **改动以提交为真相 —— 基线提交 + proxy 提交压在作者快照上；packages/ 里的补丁串
#   是按需从提交导出的溯源件，不是真相源。**
# 所以这里最有价值的检查不是「编译过不过」，而是**补丁串还能不能在纯基线树上干净重放** ——
# 它一旦失败，导出的补丁串和提交就已经不是同一份东西，下一个基线也切不出来。
# 注意：CI 出包**不再** apply 补丁（它按 packages/RELEASE-SOURCES 里已固定的提交构建），
# 所以这条检查是源码准备线的守护，而不是发版前的一步。
#
# apply_packages.sh 自己写明「不提供 --dry-run：逐条 git apply --check 会在未改动的树上
# 校验后续补丁而给出假否定」。所以本检查按它的建议做：**在干净副本上实跑**。
#
# 用法：sh scripts/check.sh [lint|test|all]
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-all}
pass=0; fail=0; skip=0
FAILED=""

ok()      { printf '  ✓ %s\n' "$1"; pass=$((pass+1)); }
bad()     { printf '  ✗ %s\n' "$1"; fail=$((fail+1)); FAILED="${FAILED} $1"; }
skipped() { printf '  ⚠ 已跳过：%s\n' "$1"; skip=$((skip+1)); }

# ── lint ─────────────────────────────────────────────────────
lint_scripts() {
  echo "▸ lint · 脚本语法"
  # 按 shebang 选检查器。⚠️ 一律用 sh -n 会**假失败**：bash 脚本里的 process
  # substitution `done < <(…)` 是 bashism，sh -n 直接报 syntax error（实测撞到过）。
  for f in "$ROOT"/scripts/*.sh; do
    [ -f "$f" ] || continue
    _rel=${f#"$ROOT"/}
    case "$(head -1 "$f")" in
      *bash*) _i="bash" ;;
      *)      _i="sh" ;;
    esac
    if ! command -v "$_i" >/dev/null 2>&1; then
      skipped "${_i} 不可用 —— ${_rel} 的语法本次未检查"
      continue
    fi
    if _err=$("$_i" -n "$f" 2>&1); then
      ok "${_i} -n ${_rel}"
    else
      bad "${_i} -n ${_rel}：$(printf '%s\n' "$_err" | head -1)"
    fi
  done

  if command -v shellcheck >/dev/null 2>&1; then
    _sc=$(shellcheck "$ROOT"/scripts/*.sh 2>&1 || true)
    _n=$(printf '%s\n' "$_sc" | grep -c '^In ' || true)
    if [ "$_n" -eq 0 ]; then
      ok "shellcheck 无告警"
    else
      # 只报数量等于没报：CI 的 shellcheck 与本地的版本不同（实测 0.9.0 vs 0.11.0），
      # 同一份脚本一边绿一边红，没有明细就只能盲改。连版本号一起打出来。
      bad "shellcheck 有 ${_n} 处告警（shellcheck $(shellcheck --version | awk '/^version:/{print $2}')）"
      printf '%s\n' "$_sc" | head -24 | sed 's/^/      /'
    fi
  else
    skipped "shellcheck 未安装 —— 语法已由上面的 -n 覆盖，引用/风格类问题本次未检查"
  fi
}

# 包索引一致性：packages/README.md 是补丁包的 SSOT 索引（含「当前采用」标记），
# 漏登记一个包，升级重基时就会漏掉它 —— 静默丢功能。
lint_index() {
  echo "▸ lint · 补丁包索引一致性"
  _readme="$ROOT/packages/README.md"
  [ -f "$_readme" ] || { bad "缺 packages/README.md（补丁包索引）"; return; }

  _actual=$(cd "$ROOT" && find packages -mindepth 2 -maxdepth 2 -type d -name 'p*' 2>/dev/null | sed 's:.*/::' | sort -u)
  _n=$(printf '%s\n' "$_actual" | grep -c . || true)
  [ "$_n" -gt 0 ] || { bad "packages/ 下找不到任何包目录（p*）"; return; }

  _missing=0
  for p in $_actual; do
    if grep -qF "$p" "$_readme"; then :; else
      bad "包 ${p} 未在 packages/README.md 登记"; _missing=1
    fi
  done
  [ "$_missing" -eq 0 ] && ok "实际存在的 ${_n} 个包全部已登记（$(printf '%s' "$_actual" | tr '\n' ' ')）"

  # 每个包必须有 patches/ 与 README.md（目录约定见 packages/README.md）
  _bad=0
  for d in $(cd "$ROOT" && find packages -mindepth 2 -maxdepth 2 -type d -name 'p*'); do
    [ -d "$ROOT/$d/patches" ] || { bad "${d} 缺 patches/"; _bad=1; }
    [ -f "$ROOT/$d/README.md" ] || { bad "${d} 缺 README.md（包的语义/限制/验证记录）"; _bad=1; }
    ls "$ROOT/$d"/patches/*.patch >/dev/null 2>&1 || { bad "${d}/patches 下没有 .patch"; _bad=1; }
  done
  [ "$_bad" -eq 0 ] && ok "每个包都有 patches/*.patch 与 README.md"
}

# ── test ─────────────────────────────────────────────────────
# 每个「带补丁包的通道」在**纯上游导出树**上实跑补丁链，再在同一棵树里跑包内单测。
#
# 为什么导出 local_source_commit 而不是拷工作区：HEAD 的 src/ 是已含 proxy 代码的
# 根布局树，拷过来只能证明「重复打补丁会跳过」，证明不了
# **「纯上游 + 补丁串仍然成立」** —— 后者一旦破了，发行产物就静默缺功能。
#
# 布局归一：老提交把上游整棵树放在仓库的 src/ 下（嵌套布局），新提交直接用上游根布局。
# 所以 `<sha>:src` 下有 Makefile.inc 时取它，否则整棵 `<sha>` 就是上游根。
#
# 与 verify.yml 的分工：这里证明补丁串**还能重导出**（源码准备线的不变量），
# verify.yml 在 release_source_commit（已固定的发行树）里跑同一套单测，
# 证明**实际出包的那份字节**。两者都要，缺一不可。
test_release_channels() {
  echo "▸ test · 各通道补丁链 + 包内单测（纯上游导出树实跑）"
  command -v git >/dev/null 2>&1 || { skipped "git 不可用 —— 补丁链本次未检查"; return; }
  _rs="$ROOT/scripts/release-sources.sh"
  [ -f "$_rs" ] || { bad "缺 scripts/release-sources.sh —— 无法枚举通道"; return; }

  _tested=0
  for _ch in $(sh "$_rs" channels all); do
    _pkg=$(sh "$_rs" get "$_ch" patch_package)
    if [ "$_pkg" = none ]; then
      skipped "通道 ${_ch} 是纯上游通道（无补丁包），补丁链不适用"
      continue
    fi
    _lsc=$(sh "$_rs" get "$_ch" local_source_commit)
    if ! git cat-file -e "${_lsc}^{commit}" 2>/dev/null; then
      bad "通道 ${_ch} 的 local_source_commit '${_lsc}' 不在对象库（浅克隆？CI 需 fetch-depth: 0）"
      continue
    fi

    _t=$(mktemp -d) || { bad "通道 ${_ch} 建临时目录失败"; continue; }
    if git cat-file -e "${_lsc}:src/Makefile.inc" 2>/dev/null; then
      _export="${_lsc}:src"
    else
      _export="$_lsc"
    fi
    if ! git archive "$_export" | tar -xf - -C "$_t"; then
      bad "通道 ${_ch} 导出纯上游失败（$_export）"; rm -rf "$_t"; continue
    fi
    _base=$(dirname "$_pkg")
    mkdir -p "$_t/$_base" "$_t/scripts"
    cp -R "$ROOT/$_pkg" "$_t/$_pkg"
    cp "$ROOT/scripts/apply_packages.sh" "$_t/scripts/"
    ( cd "$_t" && git init -q . )

    _log="$_t/apply.log"
    ( cd "$_t" && sh scripts/apply_packages.sh --src . --packages "$_base" ) >"$_log" 2>&1
    _rc=$?
    if [ "$_rc" -eq 0 ]; then
      ok "通道 ${_ch}：补丁串在纯上游上干净应用"
    else
      bad "通道 ${_ch}：apply_packages.sh 失败（退出码 ${_rc}）"
      tail -6 "$_log" | sed 's/^/      /'
      rm -rf "$_t"; continue
    fi

    if grep -qF 'GNB_PROXY_CONF_H' "$_t/src/gnb_proxy.h" 2>/dev/null; then
      ok "通道 ${_ch}：proxy 标记已落地（src/gnb_proxy.h 含 include guard）"
    else
      bad "通道 ${_ch}：src/gnb_proxy.h 里没有 proxy 标记，补丁未真正改动源码树"
    fi

    case "$(cat "$_log")" in
      *"应用失败"*) bad "通道 ${_ch}：日志里出现「应用失败」" ;;
      *"Skipped patch"*) bad "通道 ${_ch}：有补丁被 git apply 静默跳过" ;;
      *) ok "通道 ${_ch}：日志无「应用失败」/「Skipped patch」" ;;
    esac

    ( cd "$_t" && sh scripts/apply_packages.sh --src . --packages "$_base" ) >"$_log.2" 2>&1
    _rc2=$?
    if [ "$_rc2" -eq 0 ] && grep -q "所有包都已应用过" "$_log.2"; then
      ok "通道 ${_ch}：复跑幂等（识别为已应用并跳过）"
    else
      bad "通道 ${_ch}：复跑不幂等（退出码 ${_rc2}）：$(tail -2 "$_log.2" | head -1)"
    fi

    if [ -d "$_t/$_pkg/tests" ]; then
      _tlog="$_t/tests.log"
      if ( cd "$_t/$_pkg/tests" && make ) >"$_tlog" 2>&1; then
        ok "通道 ${_ch}：包内单测通过（$(grep -c '✓' "$_tlog") 项）"
      else
        bad "通道 ${_ch}：包内单测失败"
        tail -8 "$_tlog" | sed 's/^/      /'
      fi
    else
      bad "通道 ${_ch}：${_pkg}/tests 不存在 —— 补丁串没有可跑的单元测试"
    fi

    _tested=$((_tested+1))
    rm -rf "$_t"
  done

  # 「没东西可查」型门禁等于没有门禁：一个通道都没测到时必须红，不能绿着放行。
  [ "$_tested" -gt 0 ] || bad "没有任何带补丁包的通道被实测（SSOT 全为 none 或读不到提交？）"
}

# ── 调度 ─────────────────────────────────────────────────────
case "$MODE" in
  lint) lint_scripts; lint_index ;;
  test) test_release_channels ;;
  all)  lint_scripts; lint_index; test_release_channels ;;
  *)    echo "用法：sh scripts/check.sh [lint|test|all]"; exit 2 ;;
esac

echo
printf 'gnb 检查：%s 通过 / %s 失败 / %s 已跳过\n' "$pass" "$fail" "$skip"
if [ "$skip" -ne 0 ]; then
  echo "  ⚠ 有 ${skip} 项被跳过 —— 跳过不是通过，缺的工具装上后需重跑"
fi
[ "$fail" -eq 0 ] || { printf '失败项:%s\n' "$FAILED"; exit 1; }
exit 0
