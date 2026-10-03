#!/bin/sh
# gnb-dist.sh — 从「固定发行源码提交」出通道产物（不 apply 补丁）
#
#   sh scripts/gnb-dist.sh --channel main --os darwin --arch arm64
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
#   6. 写 MANIFEST + SHA256SUMS，可选打归档包（tgz；windows 为 zip，包名规则见打包段注释）
#
# 本脚本不打补丁、不改功能源码、不生成补丁包。任何校验失败都阻断发行。
# CI 各平台 job 与本地共用这一条流水线；本地产物不作为发布证据。

set -e

usage() { awk 'NR==1{next} /^#/{sub(/^# ?/, ""); print; next} {exit}' "$0"; exit 1; }
die() { echo "错误: $*" >&2; exit 1; }

CHANNEL="main"
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
    die "打归档包必须传 --arch（发行注册表按 {name}_{platform}_{arch}_{version}.{ext} 解析）"
fi
# ── 发行名规范化（只在包名这一层，不动 --os 的 Makefile 选择）─────────
# 包名 {name}_{platform}_{arch}_{version}.{ext} 的三段都必须落在 mynet_ctl
# ReleaseDomainService 的词表里，否则注册表解析不出这条记录：
#   platform  mingw 不在 PLATFORMS 内，windows 才是；
#   arch      ARCHITECTURES 没有 x86_64/aarch64，规范名是 amd64/arm64；
#   ext       PLATFORMS['windows'].ext = zip，其余 tgz —— 记录的扩展名必须和字节的
#             真实格式一致，否则 Windows 端拿到一个叫 .zip 的 tar 包，解包必炸。
# Makefile 仍按 OS 选（Makefile.mingw_x86_64 是作者的文件名，不改它）。
REL_OS="$OS"
ARCH_EXT=tgz
if [ "$OS" = "mingw" ]; then
    REL_OS=windows
    ARCH_EXT=zip
fi
case "$ARCH" in
    x86_64)  ARCH=amd64 ;;
    aarch64) ARCH=arm64 ;;
esac
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
command -v git >/dev/null 2>&1 || die "需要 git 才能导出固定提交（PATH=$PATH）"
if ! _git_err=$(git rev-parse --verify "${RELEASE_SOURCE_COMMIT}^{commit}" 2>&1); then
    # git 的原文必须一起报出来：BSD VM 里工作区是同步进去的，同一条「找不到提交」
    # 可能是仓库没同步、可能是 dubious ownership，只报我们自己的话就得再猜一轮。
    die "本地找不到提交 $RELEASE_SOURCE_COMMIT。CI 必须 fetch-depth: 0 检全历史；
       跨分支固定的提交还要先 fetch 到那个分支。缺提交时宁可失败，不要拿工作区凑数。
       git 说: $_git_err
       仓库: $(git rev-parse --git-dir 2>&1) / $(git --version 2>&1)"
fi
# `-f -` 是必需的：BSD tar（OpenBSD/FreeBSD）在没有 -f 时默认读 /dev/rst0，
# 管道会直接失败（实测 OpenBSD：tar: Failed open to read on /dev/rst0）。
git archive "$RELEASE_SOURCE_COMMIT" | tar -xf - -C "$WORK"
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

# ── 校验和：跨 OS 的探测与统一输出格式 ────────────────────────────
# 探测靠**已知输入算已知结果**（"abc" 的 SHA-256 固定），而不是 `--help`：
# BSD/OpenSSL 那一系（sha256、openssl dgst）不认 --help，会把它当文件名去打开而
# 退出非 0 —— 实测 OpenBSD 7.9 明明能算校验和，却因这个探测报
# 「没有 sha256sum / shasum / sha256」。
# 输出格式一律自己写成 GNU 的 "<hex>  <文件名>"：BSD 默认那行
# "SHA256 (gnb) = <hex>" 在别的机器上 `sha256sum -c` 认不了，而校验和文件是要
# 跨平台核对的，六条线必须字节级同构。
SHA256_ABC="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
SHA256_TOOL=""

# 从任意格式的输出里挑出那个 64 位十六进制串（不用 {64} 区间：老 awk 的 ERE 不支持）
_hex64() {
    awk '{ for (i = 1; i <= NF; i++)
              if (length($i) == 64 && $i ~ /^[0-9a-fA-F]+$/) { print tolower($i); exit } }'
}

# 成功时把可用工具写进 SHA256_TOOL
pick_sha256_tool() {
    _probe="$(mktemp "${TMPDIR:-/tmp}/gnb-sha-probe.XXXXXX")" || return 1
    printf 'abc' > "$_probe" || { rm -f "$_probe"; return 1; }
    for _cand in "sha256sum" "shasum -a 256" "sha256" "openssl dgst -sha256" "digest -a sha256" "rhash --digest=sha256"; do
        # shellcheck disable=SC2086  # 候选项本身就是「命令+参数」，必须分词
        _h="$( $_cand "$_probe" 2>/dev/null | _hex64 )"
        if [ "$_h" = "$SHA256_ABC" ]; then
            rm -f "$_probe"
            SHA256_TOOL="$_cand"
            return 0
        fi
    done
    rm -f "$_probe"
    return 1
}

sha256_hex() {  # $1=文件 → stdout 纯 hex；算不出返回 1
    [ -f "$1" ] || return 1
    # shellcheck disable=SC2086
    _h="$( $SHA256_TOOL "$1" 2>/dev/null | _hex64 )"
    [ -n "$_h" ] || return 1
    printf '%s\n' "$_h"
}

# 在**父 shell**里挑一次：SHA256_TOOL 是全局状态，写进命令替换/子 shell 的话打包段
# （归档校验和）拿不到，六条线会退化成「包能出、MANIFEST 里 archive_sha256 空」。
if pick_sha256_tool; then
    echo "  校验和工具: $SHA256_TOOL"
else
    die "本机算不出 SHA-256（依次试过 sha256sum / shasum -a 256 / sha256 / openssl dgst -sha256 / digest / rhash，没有一个能对已知输入给出正确结果）—— 写不出校验和就不出包"
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
EOF

# 易变字段一律**不进包**：同一个 tag 重跑 CI 时，built_at/ci_run 会让归档字节变掉，
# 而 ctl-publish 的幂等预检正是拿归档 sha 判「同版本内容是否变了」——包内只要有一个
# 与构建无关的时间戳，重跑就永远是「同版本内容冲突」，红得毫无意义。这些溯源信息改记
# 在归档旁边的 BUILDINFO（产物目录里能看到，但不进 tgz/zip）。
cat > "$OUT/BUILDINFO" <<EOF
# 本次构建的易变溯源信息（不进归档包，见上面 MANIFEST 的理由）
built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
build_host=$(uname -sr)
ci_ref=${GITHUB_REF:-local}
ci_job=${GITHUB_JOB:-local}
ci_run=${GITHUB_RUN_ID:-local}
EOF

(
    cd "$OUT" || exit 1
    # Windows 交叉构建产物带 .exe；同一批四程序，扩展名一致。
    # 后缀必须在逐名循环里拼：`$_sum $PROGS$_ext` 只会给最后一个名字带上 .exe
    # （字面展开成 "gnb gnb_ctl gnb_es gnb_crypto.exe"），Windows 出包就是这么
    # 报 gnb/gnb_ctl/gnb_es 三个 No such file 的。
    _ext=""
    [ -f gnb ] || _ext=".exe"
    : > SHA256SUMS
    for _p in $PROGS; do
        _f="$_p$_ext"
        _h="$(sha256_hex "$_f")" || { rm -f SHA256SUMS; die "算不出 $_f 的 SHA256SUMS（在 $OUT）"; }
        printf '%s  %s\n' "$_h" "$_f" >> SHA256SUMS
    done
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
    #   internal（publish 非 production 的通道）在 arch 与 version 之间插通道名：arch 段
    #     [a-z0-9-]+ 不含下划线，这类包在 ctl 侧必然解析失败而被跳过 —— 这正是
    #     「开发包不能进生产下载目录、也不能覆盖生产包」的强制形式（计划§8C）。
    if [ "$PUBLISH" = "production" ]; then
        ARCHIVE_NAME="gnb_${REL_OS}${ARCH_TAG}_${TAG}.${ARCH_EXT}"
    else
        ARCHIVE_NAME="gnb_${REL_OS}${ARCH_TAG}_${CHANNEL}_${TAG}.${ARCH_EXT}"
    fi
    mkdir -p "$DIST_DIR"
    # zip 的 -r 会把条目记成相对当前目录的路径，所以取绝对路径再进 STAGE 打，
    # 否则包里的 bin/ 会落到 .pkg 的相对位置上去（DIST_DIR 在 CI 里传的是相对路径）。
    DIST_ABS="$(cd "$DIST_DIR" && pwd)"
    STAGE="$(mktemp -d "${TMPDIR:-/tmp}/gnb-stage.XXXXXX")"
    mkdir -p "$STAGE/bin"
    # 条目逐个显式列出（不整目录交给 tar）：目录内顺序由文件系统决定，同一份字节
    # 也可能被写成不同顺序，从而破坏下面要的可复现性。
    STAGE_ENTRIES=""
    for prog in $PROGS; do
        if [ -f "$OUT/$prog" ]; then
            cp "$OUT/$prog" "$STAGE/bin/$prog"
            STAGE_ENTRIES="$STAGE_ENTRIES bin/$prog"
        fi
        if [ -f "$OUT/$prog.exe" ]; then
            cp "$OUT/$prog.exe" "$STAGE/bin/$prog.exe"
            STAGE_ENTRIES="$STAGE_ENTRIES bin/$prog.exe"
        fi
    done
    cp "$OUT/MANIFEST" "$OUT/SHA256SUMS" "$STAGE/"
    STAGE_ENTRIES="$STAGE_ENTRIES MANIFEST SHA256SUMS"
    # 可复现归档（10-03 定）：同一个 tag 重跑必须产出**逐字节相同**的包。实测两轮 CI
    # 的四个二进制 sha 逐条相同，差异全在容器层（MANIFEST 里的 built_at/ci_run 已挪到
    # BUILDINFO，剩下就是 mtime）—— 而 ctl-publish 的幂等预检正是拿归档 sha 判
    # 「同版本内容是否变了」，包内只要带构建时刻，重跑就永远是「同版本内容冲突」，
    # 整轴红得毫无意义。做法两步：
    #   1) 归档内所有条目 mtime 钉到 release 源码提交的 commit 时间（与本次构建无关）。
    #      cp 不带 -p 时 mtime 就是当前时间，正是差异来源；tar 与 zip 都读文件系统
    #      mtime，所以这一步同时管住两种容器，比堆 GNU tar 长选项可移植得多（mac 轴是 bsdtar）。
    #   2) gzip 头里另有一个 mtime ⇒ 走 `tar -cf - | gzip -n`；zip 加 -X 去掉额外属性。
    STAGE_EPOCH="$(git log -1 --format=%ct "$RELEASE_SOURCE_COMMIT" 2>/dev/null || true)"
    [ -n "$STAGE_EPOCH" ] \
        || die "拿不到 release_source_commit 的提交时间，无法产出可复现归档"
    STAGE_MTIME="$(date -u -r "$STAGE_EPOCH" +%Y%m%d%H%M.%S 2>/dev/null \
        || date -u -d "@$STAGE_EPOCH" +%Y%m%d%H%M.%S 2>/dev/null || true)"
    [ -n "$STAGE_MTIME" ] \
        || die "date 既不支持 -r 也不支持 -d @epoch，钉不住归档 mtime"
    find "$STAGE" -exec touch -t "$STAGE_MTIME" {} +
    if [ "$ARCH_EXT" = "zip" ]; then
        command -v zip >/dev/null 2>&1 || die "出 zip 需要 zip 命令（Windows 包按 ctl 的 windows 档必须是 zip）"
        # shellcheck disable=SC2086  # 条目列表由上面按 $PROGS 拼出，必须按空白展开
        (cd "$STAGE" && zip -q -X -r "$DIST_ABS/$ARCHIVE_NAME" $STAGE_ENTRIES)
    else
        # shellcheck disable=SC2086
        (cd "$STAGE" && tar -cf - $STAGE_ENTRIES) | gzip -n -9 > "$DIST_ABS/$ARCHIVE_NAME"
    fi
    rm -rf "$STAGE"
    STAGE=""
    ARCHIVE_PATH="$DIST_ABS/$ARCHIVE_NAME"
    ARCHIVE_SHA="$(sha256_hex "$DIST_ABS/$ARCHIVE_NAME" || true)"
    if [ -z "$ARCHIVE_SHA" ]; then
        die "归档 $ARCHIVE_NAME 算不出 SHA256 —— 不能发一个没有校验值的包"
    fi
    # 一次构建、两个名字（openwrt 的 mips 轴有两套互不相通的设备侧词表）：
    #   · ctl 取件脚本 `/api/v2/detect/openwrt?distrib_arch=mipsel_24kc` 解析成 **mipsel**
    #     （arch_map 里没有任何 distrib_arch 映射到 mips*-softfp；DetectController.php:196-203
    #     的 softfpFallback 只在上报 arch 一条记录都没有时才触发，裸名有行就永远不回退）；
    #   · WebUI 安装器自己读 FPU（gnb_installer.lua:193/199）⇒ 软浮点算出 **mipsel-softfp**
    #     / **mips-softfp**，而它的别名表（:240-244）里不含裸名。
    # 只发一个名字必然漏掉一侧。历史 v1.6.0.a 的 manifest 里 mipsel+mipsel-softfp、
    # mips+mips-softfp 四行俱在 —— 那就是既有契约的形状，这里只是不再重编、直接复制字节。
    ARCH_TWINS=""
    if [ "$REL_OS" = "openwrt" ]; then
        case "$ARCH" in
            mipsel) ARCH_TWINS=mipsel-softfp ;;
            mips)   ARCH_TWINS=mips-softfp   ;;
        esac
    fi
    for twin in $ARCH_TWINS; do
        TWIN_NAME="gnb_${REL_OS}_${twin}_${TAG}.${ARCH_EXT}"
        if [ "$PUBLISH" != "production" ]; then
            TWIN_NAME="gnb_${REL_OS}_${twin}_${CHANNEL}_${TAG}.${ARCH_EXT}"
        fi
        cp "$ARCHIVE_PATH" "$DIST_ABS/$TWIN_NAME"
        # 包内 MANIFEST 的 arch= 仍是构建时那个名（同一份字节，不改包里内容）；
        # 双胞胎名与 sha 记在归档旁边的 BUILDINFO 里。
        printf 'twin=%s sha256=%s（与 %s 同一份字节，供 WebUI 安装器的 -softfp 词表命中）\n' \
            "$TWIN_NAME" "$ARCHIVE_SHA" "$ARCHIVE_NAME" >> "$OUT/BUILDINFO"
    done
    printf 'archive=%s\narchive_sha256=%s\n' "$ARCHIVE_NAME" "$ARCHIVE_SHA" >> "$OUT/MANIFEST"
    echo "  归档: $ARCHIVE_PATH"
    echo "ARCHIVE=$ARCHIVE_PATH"
fi

echo
echo "✓ 出包完成: $OUT（channel=$CHANNEL）"
echo "  MANIFEST + 四程序 + SHA256SUMS"
echo "  装机时整目录落位，禁止只拷 gnb 单文件。"
cat "$OUT/MANIFEST"
