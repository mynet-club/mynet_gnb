#!/usr/bin/env bash
set -euo pipefail

# 把某个补丁包的补丁串重基到新的上游基线 commit。
#
# 用法:
#   packages/rebase.sh <新上游ref> <包目录> [分支名]
#
# 例:
#   packages/rebase.sh 1.6.9 packages/1.6.8-c411f14ee647/p1-route-proxy
#
# 做的事: 在临时目录 clone 上游 -> checkout 新 ref -> git am -3 依次应用补丁串。
# 冲突时停下并打印处理提示; 全部成功后打印如何把新补丁串导出到新基线目录。
#
# 环境变量:
#   UPSTREAM_URL  上游仓库地址, 默认 https://github.com/opengnb/opengnb.git

usage() {
  echo "用法: $0 <新上游ref> <包目录> [分支名]" 1>&2
  echo "例:   $0 1.6.9 packages/1.6.8-c411f14ee647/p1-hot-restart" 1>&2
  exit 1
}

[[ $# -ge 2 ]] || usage

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
NEW_REF="$1"
PKG_DIR="$(cd "$2" && pwd)"
BRANCH="${3:-}"
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/opengnb/opengnb.git}"

PKG_NAME="$(basename "$PKG_DIR")"

[[ -d "$PKG_DIR/patches" ]] || { echo "错误: $PKG_DIR/patches 不存在" 2>&1; exit 1; }

PATCHES=()
while IFS= read -r p; do
  [[ -n "$p" ]] && PATCHES+=("$p")
done < <(ls -1 "$PKG_DIR"/patches/*.patch 2>/dev/null || true)

if [[ ${#PATCHES[@]} -eq 0 ]]; then
  echo "错误: $PKG_DIR/patches 下没有 .patch 文件" 2>&1
  exit 1
fi

if [[ -z "$BRANCH" ]]; then
  BRANCH="rebase/${PKG_NAME}-$(date +%Y%m%d-%H%M%S)"
fi

WORK="$(mktemp -d)"
echo "临时工作区: $WORK"
echo "（应用失败时会保留它以便排查；成功后可直接删除）"

echo "clone 上游: $UPSTREAM_URL"
git clone --quiet --filter=blob:none "$UPSTREAM_URL" "$WORK/upstream"
cd "$WORK/upstream"

echo "fetch 并 checkout 新基线: $NEW_REF"
git -c advice.detachedHead=false fetch --quiet --tags origin 2>/dev/null || true
git checkout --quiet -b "$BRANCH" "$NEW_REF"
NEW_SHA="$(git rev-parse --short=12 HEAD)"
NEW_DESC="$(git describe --tags --always "$NEW_SHA" 2>/dev/null || echo "$NEW_SHA")"
echo "新基线 commit: $NEW_SHA  ($NEW_DESC)"
echo

echo "依次应用 ${#PATCHES[@]} 个补丁 ..."
if git am -3 "${PATCHES[@]}"; then
  SUGGEST_DIR="$REPO_ROOT/packages/${NEW_DESC}-${NEW_SHA}/${PKG_NAME}"
  echo
  echo "=========================================================="
  echo "全部应用成功（基线 ${NEW_SHA}，分支 ${BRANCH}）。"
  echo
  echo "导出为以新基线归档的补丁串："
  echo "  mkdir -p \"$SUGGEST_DIR/patches\""
  echo "  git format-patch -o \"$SUGGEST_DIR/patches\" $NEW_SHA..HEAD"
  echo "  cp \"$PKG_DIR\"/{README.md,apply.sh,revert.sh} \"$SUGGEST_DIR\"/"
  echo
  echo "然后必须重新做验证："
  echo "  1) 在该 clone 里 make -f Makefile.Darwin clean && make -f Makefile.Darwin install（0 error / 0 warning）"
  echo "  2) 按 $PKG_NAME/README.md 里记录的实测步骤重跑一遍"
  echo "  3) README.md 里把基线 commit 改成 ${NEW_SHA}，更新验证结论"
  echo "  4) 在 packages/README.md 的表格里加一行，旧目录保留"
  echo "=========================================================="
else
  echo
  echo "应用过程中出现冲突，已停在冲突处。" 2>&1
  echo "处理方式：" 2>&1
  echo "  cd $WORK/upstream" 2>&1
  echo "  # 编辑 git status 里列出的冲突文件，解决后：git add <文件> && git am --continue" 2>&1
  echo "  # 想跳过这一条补丁：git am --skip" 2>&1
  echo "  # 想整体放弃：git am --abort" 2>&1
  exit 1
fi
