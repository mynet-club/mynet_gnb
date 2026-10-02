#!/bin/sh
# gnb-dist.sh — 从「固定发行源码提交」出通道产物（不 apply 补丁）
#
#   sh scripts/gnb-dist.sh --channel main-patched --os darwin --arch arm64
#                          [--make make] [--make-var K=V] [--target install]
#                          [--strip-cmd strip] [--tag v1.6.8]
#                          [--out DIR] [--dist-dir DIR] [--archive] [--keep]
#
# 职责边界（docs/gnb-maintenance-plan.md「开发与发行的职责边界」、§8C）：
#   1. 从 packages/RELEASE-SOURCES 取本通道的 release_source_commit（UNSET 直接失败，不推断）
#   2. git archive 导出那个提交 —— 构建树逐字节等于已固定的源码提交
#   3. 校验源码身份：version 与期望一致、无 src/src 嵌套、proxy 源码与 Features 行的
#      存在性与通道期望一致（Features 行只由源码提交决定，本脚本绝不插入）
#   4. 按平台 Makefile 构建，四程序必须来自这一次构建
#   5. 可选 strip，然后从**构建出的二进制**验收能力输出（present/absent 由通道决定）
#   6. 写 MANIFEST + SHA256SUMS，可选打 tgz（包名规则见打包段注释）
#
# 本脚本不打补丁、不改功能源码、不生成补丁包。任何校验失败都阻断发行。
# CI 各平台 job 与本地共用这一条流水线；本地产物不作为发布证据。

set -e

usage() { awk 'NR==1{next} /^#/{sub(/^# ?/, ""); print; next} {exit}' "$0"; exit 1; }
die() { echo "错误: $*" >&2; exit 1; }

CHANNEL="main-patched"
OS=""
ARCH=""
MAKE="make"
TARGET="install"
STRIP_CMD=""
TAG=""
OUT=""
DIST_DIR="dist"
MAKE_ARCHIVE=0
KEEP=0
STAGE=""

# make 变量逐行存文件，再用 set -- 追加为独立 argv：
# 值里带空格（CC="clang -arch arm64"、CLI_LDFLAGS="-static -pg"）也不会被拆开，
# 也避免对 CI 传入的字符串做 eval。
MAKE_VARS="$(mktemp "${TMPDIR:-/tmp}/gnb-make-vars.XXXXXX")"
cleanup() {
    rm -f "$MAKE_VARS"
    [ -n "$STAGE" ] && rm -rf "$STAGE"
    if [ "$KEEP" -eq 1 ]; then
        echo "（--keep）构建树保留在 $WORK"
    else
        [ -n "${WORK:-}" ] && rm -rf "$WORK"
    fi
    return 0
}
trap cleanup EXIT INT TERM

while [ $# -gt 0 ]; do
    case "$1" in
        --channel)   CHANNEL="$2"; shift 2 ;;
        --os)        OS="$2"; shift 2 ;;
        --arch)      ARCH="$2"; shift 2 ;;
        --make)      MAKE="$2"; shift 2 ;;
        --target)    TARGET="$2"; shift 2 ;;
        --make-var)  printf '%s\n' "$2" >> "$MAKE_VARS"; shift 2 ;;
        --strip-cmd) STRIP_CMD="$2"; shift 2 ;;
        --tag)       TAG="$2"; shift 2 ;;
        --out)       OUT="$2"; shift 2 ;;
        --dist-dir)  DIST_DIR="$2"; shift 2 ;;
        --archive)   MAKE_ARCHIVE=1; shift ;;
        --keep)      KEEP=1; shift ;;
        -h|--help)   usage ;;
        *) echo "未知参数: $1" >&2; usage ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
RS_SH="$REPO_ROOT/scripts/release-sources.sh"
rs_get()    { sh "$RS_SH" get "$CHANNEL" "$1"; }
rs_require() { sh "$RS_SH" require "$CHANNEL" "$1"; }

# 四程序是全局口径（计划§五）：空格列表用于循环，逗号列表写进 MANIFEST
PROGS="gnb gnb_ctl gnb_es gnb_crypto"
PROGRAMS=""
for _p in $PROGS; do PROGRAMS="${PROGRAMS:+$PROGRAMS,}$_p"; done

# ── 1. 通道口径（唯一事实源：packages/RELEASE-SOURCES） ──────────
AUTHOR_IDENTITY="$(rs_require author_identity)"
LOCAL_SOURCE_COMMIT="$(rs_require local_source_commit)"
RELEASE_SOURCE_COMMIT="$(rs_require release_source_commit)"
PATCH_PACKAGE="$(rs_get patch_package)"
SRC_VERSION="$(rs_require src_version)"
FEATURES_MARK="$(rs_require features_mark)"
PUBLISH="$(rs_require publish)"
case "$FEATURES_MARK" in present|absent) : ;; *) die "features_mark 只支持 present / absent，得到 '$FEATURES_MARK'" ;; esac
case "$PUBLISH" in production|internal) : ;; *) die "publish 只支持 production / internal，得到 '$PUBLISH'" ;; esac

# ── 2. 平台 → Makefile（仓库根即上游根布局） ─────────────────────
UNAME="$(uname -s)"
if [ -z "$OS" ]; then
    case "$UNAME" in
        Darwin)  OS=darwin ;;
        Linux)   OS=linux ;;
        FreeBSD) OS=freebsd ;;
        OpenBSD) OS=openbsd ;;
        NetBSD)  OS=netbsd ;;
        *) die "无法自动识别 OS=$UNAME，请传 --os" ;;
    esac
fi
case "$OS" in
    darwin)   MF=Makefile.Darwin ;;
    linux)    MF=Makefile.linux ;;
    openwrt)  MF=Makefile.openwrt ;;
    mingw)    MF=Makefile.mingw_x86_64 ;;
    freebsd)  MF=Makefile.freebsd ;;
    openbsd)  MF=Makefile.openbsd ;;
    netbsd)   MF=Makefile.netbsd ;;
    *) die "不支持的 --os $OS（darwin|linux|openwrt|mingw|freebsd|openbsd|netbsd）" ;;
esac

if [ -z "$TAG" ]; then
    TAG="v$(cat version 2>/dev/null || echo unknown)"
fi
# 开发包快照的 version 文件仍写 1.6.8，不标记就会和真正从已发行 1.6.8 出的包同名，
# 而发布表按文件名 {name}_{platform}_{arch}_{version} 解析，两者会并成同一版本。
# 判据只认 author_identity（已发行上游 vs 作者 ZIP），未识别的新形态一律带 -dev：
# 宁可多标，不可漏标 —— 漏标是错的包名，多标只是一个丑点的包名。
case "$AUTHOR_IDENTITY" in
    opengnb/opengnb@*) : ;;
    *)  case "$TAG" in
            *-dev*) : ;;
            *) TAG="$TAG-dev" ;;
        esac ;;
esac
if [ "$MAKE_ARCHIVE" -eq 1 ] && [ -z "$ARCH" ]; then
    # 不带 arch 的包名会被 ctl 的严格正则误解析（arch 段会吃掉 version），出包必须显式给 arch
    die "打 tgz 必须传 --arch（发行注册表按 {name}_{platform}_{arch}_{version} 解析）"
fi
ARCH_TAG="${ARCH:+_$ARCH}"
# 交叉编译要用同工具链的 strip；命令不存在就早失败，不要白编一遍才发现。
# 晚一步的 strip 循环里用 `|| true` 容忍单文件失败（如只读产物），但工具本身必须在。
if [ -n "$STRIP_CMD" ]; then
    command -v "$(printf '%s' "$STRIP_CMD" | cut -d' ' -f1)" >/dev/null 2>&1 \
        || die "--strip-cmd '$STRIP_CMD' 不可用 —— 宁可不发，也不发声称已 strip 却没 strip 的包"
fi
STAMP="$(date +%Y%m%d-%H%M%S)"
if [ -z "$OUT" ]; then
    OUT="dist/gnb-${CHANNEL}_${OS}${ARCH_TAG}_${TAG}-${STAMP}"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/gnb-dist.XXXXXX")"

echo "▸ gnb 通道出包"
echo "  通道     : $CHANNEL（publish=$PUBLISH）"
echo "  作者标识 : $AUTHOR_IDENTITY"
echo "  纯源码   : $LOCAL_SOURCE_COMMIT"
echo "  发行源码 : $RELEASE_SOURCE_COMMIT"
echo "  补丁包   : $PATCH_PACKAGE（仅溯源，本流程不 apply）"
echo "  平台     : $OS${ARCH:+ ($ARCH)} → $MF"
echo "  构建树   : $WORK"
echo "  产物     : $OUT"
echo

# ── 3. 导出固定发行源码提交 ──────────────────────────────────────
echo "▸ [1/6] 导出发行源码提交 $RELEASE_SOURCE_COMMIT"
command -v git >/dev/null 2>&1 || die "需要 git 才能导出固定提交"
if ! git rev-parse --verify "${RELEASE_SOURCE_COMMIT}^{commit}" >/dev/null 2>&1; then
    die "本地找不到提交 $RELEASE_SOURCE_COMMIT。CI 必须 fetch-depth: 0 检全历史；
       跨分支固定的提交还要先 fetch 到那个分支。缺提交时宁可失败，不要拿工作区凑数。"
fi
git archive "$RELEASE_SOURCE_COMMIT" | tar -x -C "$WORK"
if [ ! -f "$WORK/$MF" ]; then
    die "导出树缺 $MF —— 该提交不是可构建的根布局"
fi
[ -f "$WORK/src/gnb.h" ] || die "导出树缺 src/gnb.h"

# ── 4. 源码身份校验（只读，不改源码） ────────────────────────────
echo "▸ [2/6] 校验源码身份"
if [ -d "$WORK/src/src" ]; then
    die "导出树里仍有 src/src 嵌套目录 —— 该提交不是上游根布局"
fi
_work_ver="$(cat "$WORK/version" 2>/dev/null || echo '')"
if [ "$_work_ver" != "$SRC_VERSION" ]; then
    die "version=$_work_ver 与 RELEASE-SOURCES src_version=$SRC_VERSION 不符"
fi
echo "  version OK ($_work_ver)"

_has_proxy_src=0
[ -f "$WORK/src/gnb_proxy.c" ] && _has_proxy_src=1
_has_features=0
grep -qF 'Features: proxy' "$WORK/src/cli/gnb.c" 2>/dev/null && _has_features=1

if [ "$FEATURES_MARK" = "present" ]; then
    if [ "$_has_proxy_src" -ne 1 ]; then
        die "通道 $CHANNEL 期望已含 proxy 源码，但 $RELEASE_SOURCE_COMMIT 里没有 src/gnb_proxy.c"
    fi
    if [ "$_has_features" -ne 1 ]; then
        die "通道 $CHANNEL 期望源码里有 Features 行，但 src/cli/gnb.c 没有 ——
       Features 属于补丁源码的一部分，不该由发行流程补插。请补丁线补齐后重指定提交。"
    fi
    echo "  proxy 源码 + Features 行 OK"
else
    if [ "$_has_proxy_src" -eq 1 ] || [ "$_has_features" -eq 1 ]; then
        die "通道 $CHANNEL 是纯上游对照，但 $RELEASE_SOURCE_COMMIT 的 src/ 已含 MyNet proxy 代码/Features 行"
    fi
    echo "  纯上游 src OK（无 proxy 代码）"
fi

# ── 5. 构建四程序 ───────────────────────────────────────────────
echo "▸ [3/6] $MAKE -C <tree> -f $MF $TARGET"
build() {
    _t=${1:-}
    set -- "$MAKE" -C "$WORK" -f "$MF"
    if [ -s "$MAKE_VARS" ]; then
        while IFS= read -r _mv; do
            [ -n "$_mv" ] && set -- "$@" "$_mv"
        done < "$MAKE_VARS"
    fi
    if [ -n "$_t" ]; then
        set -- "$@" "$_t"
    fi
    "$@"
}
build clean >/dev/null 2>&1 || true
build "$TARGET"

BIN=""
for cand in "$WORK/bin" "$WORK" "$WORK/src"; do
    if [ -x "$cand/gnb" ] || [ -x "$cand/gnb.exe" ]; then
        BIN="$cand"
        break
    fi
done
if [ -z "$BIN" ]; then
    echo "      查过 $WORK/bin、$WORK、$WORK/src" >&2
    KEEP=1
    die "编译完成但找不到 gnb 可执行文件（加 --keep 可留工作区排查）"
fi
echo "  二进制目录: $BIN"

MISS=""
for prog in $PROGS; do
    if [ ! -x "$BIN/$prog" ] && [ ! -x "$BIN/$prog.exe" ]; then
        MISS="$MISS $prog"
    fi
done
if [ -n "$MISS" ]; then
    KEEP=1
    die "同一次构建的套件不完整，缺:$MISS —— 四程序必须同 job、同源码、同构建"
fi

mkdir -p "$OUT"
for prog in $PROGS; do
    if   [ -f "$BIN/$prog" ];     then cp "$BIN/$prog" "$OUT/$prog"
    elif [ -f "$BIN/$prog.exe" ]; then cp "$BIN/$prog.exe" "$OUT/$prog.exe"
    fi
done

# 平台差异取证：链接方式/架构在 CI 日志里一眼可见（musl/uClibc/静态/动态）
if command -v file >/dev/null 2>&1; then
    for prog in $PROGS; do
        for f in "$OUT/$prog" "$OUT/$prog.exe"; do
            [ -f "$f" ] || continue
            echo "  $(basename "$f"): $(file -b "$f")"
        done
    done
fi

# strip 放在能力验收与校验和之前：SHA256SUMS 必须描述实际交付的那份字节
if [ -n "$STRIP_CMD" ]; then
    # 工具可用性已在出包前查过。这里单个文件 strip 失败也一律阻断：MANIFEST 会写
    # strip=<cmd>，出一个「声称已 strip 其实没 strip」的包比不出包更糟。
    echo "  strip: $STRIP_CMD"
    for f in "$OUT"/gnb "$OUT"/gnb_ctl "$OUT"/gnb_es "$OUT"/gnb_crypto \
             "$OUT"/gnb.exe "$OUT"/gnb_ctl.exe "$OUT"/gnb_es.exe "$OUT"/gnb_crypto.exe; do
        [ -f "$f" ] || continue
        # shellcheck disable=SC2086  # strip-cmd 允许带参数（如 mips-linux-gnu-strip --strip-unneeded）
        $STRIP_CMD "$f" || die "strip 失败：$f"
    done
fi

# ── 6. 从构建产物验收能力输出 ───────────────────────────────────
echo "▸ [4/6] 验收构建产物的能力输出"
GNB_OUT="$OUT/gnb"
[ -f "$GNB_OUT" ] || GNB_OUT="$OUT/gnb.exe"
[ -f "$GNB_OUT" ] || die "$OUT 里没有 gnb"

binary_strings() {
    if command -v strings >/dev/null 2>&1; then
        strings "$1" 2>/dev/null || true
    else
        grep -a -o '[ -~]\{4,\}' "$1" 2>/dev/null || true
    fi
}
FEATURES_LINE="$(binary_strings "$GNB_OUT" | grep -F 'Features: proxy' | head -1 || true)"
PROXY_LINE="$(binary_strings "$GNB_OUT" | grep -E 'proxy\.conf|ingress_open' | head -1 || true)"

echo "  Features 行: ${FEATURES_LINE:-<未命中>}"
echo "  proxy 标记 : ${PROXY_LINE:-<未命中>}"
if [ "$FEATURES_MARK" = "present" ]; then
    [ -n "$FEATURES_LINE" ] || die "二进制未报 Features: proxy，但通道 $CHANNEL 必须带该能力行（mynet_agent 的主判据）"
    [ -n "$PROXY_LINE" ]    || die "二进制里没有 proxy.conf/ingress_open 特征串 —— proxy 源码可能没编进去"
else
    [ -z "$FEATURES_LINE" ] || die "纯上游通道 $CHANNEL 的产物却报出 Features: proxy —— 源码身份与产物不一致"
fi

# ── 7. MANIFEST + SHA256SUMS ────────────────────────────────────
echo "▸ [5/6] 写 MANIFEST 与校验和"
PATCH_COMMITS="none"
if [ "$PATCH_PACKAGE" != "none" ] && [ -d "$REPO_ROOT/$PATCH_PACKAGE" ]; then
    PATCH_COMMITS="$PATCH_PACKAGE@$(git log -1 --format='%h' -- "$PATCH_PACKAGE" 2>/dev/null || echo unknown)"
fi
MAKE_VARS_QUOTED=""
if [ -s "$MAKE_VARS" ]; then
    while IFS= read -r _mv; do
        [ -n "$_mv" ] || continue
        MAKE_VARS_QUOTED="$MAKE_VARS_QUOTED '$(printf '%s' "$_mv" | tr -d "\"'")'"
    done < "$MAKE_VARS"
fi
TOOLING_DESC="$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"

cat > "$OUT/MANIFEST" <<EOF
# gnb 通道发包清单 —— 四程序同一次构建，禁止混用异构建产物
channel=$CHANNEL
publish=$PUBLISH
author_identity=$AUTHOR_IDENTITY
local_source_commit=$LOCAL_SOURCE_COMMIT
release_source_commit=$RELEASE_SOURCE_COMMIT
patch_package=$PATCH_PACKAGE
patch_commits=$PATCH_COMMITS
src_version=$SRC_VERSION
programs=$PROGRAMS
os=$OS
arch=${ARCH:-native}
makefile=$MF
build_cmd=$MAKE -f $MF$MAKE_VARS_QUOTED $TARGET
strip=${STRIP_CMD:-none}
mark_features=${FEATURES_LINE:-none}
mark_proxy=${PROXY_LINE:-none}
tooling_commit=$TOOLING_DESC
build_host=$(uname -sr)
built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
ci_ref=${GITHUB_REF:-local}
ci_job=${GITHUB_JOB:-local}
ci_run=${GITHUB_RUN_ID:-local}
EOF

(
    cd "$OUT" || exit 1
    _sum=""
    for _tool in "sha256sum" "shasum -a 256" "sha256"; do
        # shellcheck disable=SC2086  # 探测本机可用的校验工具（名字带参数）
        if $_tool --help >/dev/null 2>&1; then _sum="$_tool"; break; fi
    done
    if [ -z "$_sum" ]; then
        echo "错误: 没有 sha256sum / shasum / sha256 —— 写不出校验和就不出包" >&2
        exit 1
    fi
    # Windows 交叉构建产物带 .exe；同一批四程序，扩展名一致
    _ext=""
    [ -f gnb ] || _ext=".exe"
    # shellcheck disable=SC2086
    if ! $_sum $PROGS$_ext > SHA256SUMS; then
        echo "错误: 算不出四程序 SHA256SUMS（缺哪个？）" >&2
        exit 1
    fi
)

# ── 8. 打包（可选） ─────────────────────────────────────────────
echo "▸ [6/6] 打包"
ARCHIVE_PATH=""
ARCHIVE_SHA=""
if [ "$MAKE_ARCHIVE" -eq 1 ]; then
    # 包名规则（跨仓契约，改动前先想清楚谁能解析它）：
    #   production 通道沿用 mynet_ctl 注册用的
    #     {name}_{platform}_{arch}_{version}.{ext}
    #   —— ReleaseDomainService::parseFilename 是严格正则，多插一段会让下载记录
    #      解析失败（静默 SKIP），通道信息在包内 MANIFEST 与产物目录名里。
    #   internal（x-dev-patched）在 arch 与 version 之间插通道名：arch 段
    #     [a-z0-9-]+ 不含下划线，这类包在 ctl 侧必然解析失败而被跳过 —— 这正是
    #     「开发包不能进生产下载目录、也不能覆盖生产包」的强制形式（计划§8C）。
    if [ "$PUBLISH" = "production" ]; then
        ARCHIVE_NAME="gnb_${OS}${ARCH_TAG}_${TAG}.tgz"
    else
        ARCHIVE_NAME="gnb_${OS}${ARCH_TAG}_${CHANNEL}_${TAG}.tgz"
    fi
    mkdir -p "$DIST_DIR"
    STAGE="$(mktemp -d "${TMPDIR:-/tmp}/gnb-stage.XXXXXX")"
    mkdir -p "$STAGE/bin"
    for prog in $PROGS; do
        [ -f "$OUT/$prog" ]     && cp "$OUT/$prog" "$STAGE/bin/$prog"
        [ -f "$OUT/$prog.exe" ] && cp "$OUT/$prog.exe" "$STAGE/bin/$prog.exe"
    done
    cp "$OUT/MANIFEST" "$OUT/SHA256SUMS" "$STAGE/"
    tar -czf "$DIST_DIR/$ARCHIVE_NAME" -C "$STAGE" bin MANIFEST SHA256SUMS
    rm -rf "$STAGE"
    STAGE=""
    ARCHIVE_PATH="$DIST_DIR/$ARCHIVE_NAME"
    ARCHIVE_SHA="$( cd "$DIST_DIR" && (sha256sum "$ARCHIVE_NAME" 2>/dev/null || shasum -a 256 "$ARCHIVE_NAME") | awk '{print $1}' )"
    if [ -z "$ARCHIVE_SHA" ]; then
        die "归档 $ARCHIVE_NAME 算不出 SHA256 —— 不能发一个没有校验值的包"
    fi
    printf 'archive=%s\narchive_sha256=%s\n' "$ARCHIVE_NAME" "$ARCHIVE_SHA" >> "$OUT/MANIFEST"
    echo "  归档: $ARCHIVE_PATH"
    echo "ARCHIVE=$ARCHIVE_PATH"
fi

echo
echo "✓ 出包完成: $OUT（channel=$CHANNEL）"
echo "  MANIFEST + 四程序 + SHA256SUMS"
echo "  装机时整目录落位，禁止只拷 gnb 单文件。"
cat "$OUT/MANIFEST"
