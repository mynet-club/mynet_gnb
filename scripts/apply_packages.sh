#!/bin/sh
# 把 packages/ 下的补丁包应用到**纯上游**源码树，产出一份可编译的功能源码。
#
# 这是**源码准备线**的工具，不是发行工具：CI 不再 apply 补丁，它按
# packages/RELEASE-SOURCES 里已固定的 release_source_commit 直接出包（见 gnb-dist.sh）。
# 本脚本的用途是：给定一个基线，验证「纯上游 + 补丁串」仍然成立，
# 以及为下一个 release_source_commit 准备内容。
#
#   热更新（p1-hot-restart / p1-hot-restart-v2）与旧 p2-route-proxy 均已退位并删除：
#   上游 1.6.8 自带 gnb_hot_restart_prepare（由同步提交 2313dea 引入），我们的
#   热重启包是冗余的；proxy 也已从「叠加在 P1 v2 之上」改为独立成包。
#
# 用法:
#   scripts/apply_packages.sh [--src DIR] [--packages DIR]
#
#   默认 --src .，--packages 自动挑选：packages/ 下版本前缀与 src/version 相符的
#   基线目录（即 packages/<版本>-<上游commit>/）。有多个候选（例如 1.6.8-c411f14ee647
#   与 1.6.8-dev-74414d1 都以 1.6.8 开头）时必须用 --packages 显式指定。
#
# 只有一个包：p1-route-proxy，无前置依赖。
#
# 只依赖 git apply：不用 bash、也不调用各包自带的 apply.sh（那些脚本要 bash，
# 而 FreeBSD/OpenBSD 的 CI 虚拟机里未必装了 bash）。
#
# 幂等：已应用的包会被识别并跳过，重复执行不会重复打补丁。
#
# 不提供 --dry-run：补丁串是一个整体，逐条 `git apply --check` 会因为在未改动的树上
# 校验后续补丁而给出假否定，没有参考价值。要看会不会成功，就用一个干净副本实跑。

set -e

SRC="."
PACKAGES=""

while [ $# -gt 0 ]; do
    case "$1" in
        --src)      SRC="$2"; shift 2 ;;
        --packages) PACKAGES="$2"; shift 2 ;;
        -h|--help)  sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 1 ;;
    esac
done

[ -d "$SRC" ] || { echo "错误: 源码树目录不存在: $SRC" >&2; exit 1; }
[ -f "$SRC/Makefile.inc" ] || { echo "错误: $SRC 看起来不是 opengnb 源码树（缺 Makefile.inc）" >&2; exit 1; }
[ -f "$SRC/src/gnb_ctl_block.h" ] || { echo "错误: 找不到 $SRC/src/gnb_ctl_block.h" >&2; exit 1; }

command -v git >/dev/null 2>&1 || { echo "错误: 需要 git" >&2; exit 1; }

SRC_VERSION="$(cat "$SRC/version" 2>/dev/null || echo "")"
[ -n "$SRC_VERSION" ] || { echo "错误: 读不到 $SRC/version" >&2; exit 1; }

# ---- 挑选基线目录 ----
if [ -z "$PACKAGES" ]; then
    for d in packages/*/; do
        [ -d "$d" ] || continue
        case "$(basename "$d")" in
            "$SRC_VERSION"-*) ;;
            *) continue ;;
        esac
        ls "$d"p1-route-proxy/patches/*.patch >/dev/null 2>&1 || continue
        if [ -n "$PACKAGES" ]; then
            echo "错误: 有多个基线目录匹配版本 ${SRC_VERSION}（$(basename "$PACKAGES")、$(basename "$d")），请用 --packages 指定" >&2
            exit 1
        fi
        PACKAGES="$d"
    done
fi

[ -n "$PACKAGES" ] || {
    echo "错误: 在 packages/ 下找不到版本为 $SRC_VERSION 的基线目录。" >&2
    echo "      src/ 可能刚同步了上游新版，而补丁包还没重基到新 commit。" >&2
    echo "      处理办法（二选一）：" >&2
    echo "        1) 先按 packages/README.md 用 packages/rebase.sh 把补丁串重基到新基线并重新归档；" >&2
    echo "        2) 若本次确实要构建不带补丁的上游版，跳过本脚本（发行线不走这里）。" >&2
    echo "      也可用 --packages 显式指定基线目录，或 --src 指定源码树。" >&2
    exit 1
}
PACKAGES="${PACKAGES%/}"
[ -d "$PACKAGES" ] || { echo "错误: 基线目录不存在: $PACKAGES" >&2; exit 1; }

# ---- 计算 git apply 的 --directory ----
# 补丁路径形如 a/src/cli/gnb.c，-p1 之后是 src/cli/gnb.c，相对「源码树根」解析；
# 但 git apply 在 git 仓库内是按仓库根解析的，所以目标树嵌在仓库子目录（本仓库的
# src/、CI 的 workspace/src）时必须补上「仓库根 -> 源码树根」的相对路径。
DIR_ARGS=""
if git -C "$SRC" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    PREFIX="$(git -C "$SRC" rev-parse --show-prefix 2>/dev/null | sed 's:/*$::')"
    if [ -n "$PREFIX" ]; then
        DIR_ARGS="--directory=$PREFIX"
    fi
fi

echo "源码树   : $SRC (version $SRC_VERSION)"
echo "基线目录 : $PACKAGES"
echo "应用顺序 : p1-route-proxy（单包，无前置）"
[ -n "$DIR_ARGS" ] && echo "git apply: -p1 $DIR_ARGS"
echo

applied_any=0

apply_package() {
    pkg_name="$1"
    marker_file="$2"
    marker_text="$3"
    pkg_dir="$PACKAGES/$pkg_name"

    if [ ! -d "$pkg_dir/patches" ]; then
        echo "错误: 找不到 $pkg_dir/patches" >&2
        exit 1
    fi

    if [ -n "$marker_text" ] && grep -q "$marker_text" "$SRC/src/${marker_file}" 2>/dev/null; then
        echo "== $pkg_name: 已应用，跳过"
        return 0
    fi

    n=0
    for p in "$pkg_dir"/patches/*.patch; do
        [ -e "$p" ] || { echo "错误: $pkg_dir/patches 下没有 .patch" >&2; exit 1; }
        # shellcheck disable=SC2086  # DIR_ARGS 为空时必须展开为零个参数，不能被当成空字符串参数
        out="$(git apply -p1 $DIR_ARGS --whitespace=nowarn "$p" 2>&1)" || {
            echo "$out" >&2
            echo "错误: ${p##*/} 应用失败（源码树版本可能与基线不符）" >&2
            exit 1
        }
        case "$out" in
            *"Skipped patch"*)
                echo "错误: ${p##*/} 被 git apply 静默跳过（--directory 或上下文不匹配）" >&2
                exit 1 ;;
        esac
        echo "   [OK] $(basename "$p")"
        n=$((n+1))
    done
    echo "== $pkg_name: $n 个补丁 已应用"
    applied_any=1
}

# 只有一个包。marker 取 src/gnb_proxy.h 的 include guard —— 该文件在纯上游里不存在，
# 只有打上补丁才会有，所以「文件里有这个串」等价于「补丁已落地」。
apply_package p1-route-proxy gnb_proxy.h "GNB_PROXY_CONF_H"

echo
if [ "$applied_any" -eq 0 ]; then
    echo "所有包都已应用过，无需改动。"
else
    echo "补丁应用完成。下一步（以 Darwin 为例）："
    echo "    make -C . -f Makefile.Darwin clean && make -C . -f Makefile.Darwin install"
fi
# 热更新退位后本补丁不碰 ctl block 共享内存布局（实测纯上游 + 本补丁的树里
# gnb_core_flag 命中数为 0）。但「gnb / gnb_ctl / gnb_es 必须同一份构建」仍然成立：
# ctl 键集与结构体定义都来自同一份源码，混用不同通道的二进制会解析错位。
echo "注意：gnb / gnb_ctl / gnb_es / gnb_crypto 必须来自同一份构建，不要跨通道混用。"
