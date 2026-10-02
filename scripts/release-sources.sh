#!/bin/sh
# release-sources.sh — 读 packages/RELEASE-SOURCES（发行源码提交 SSOT）
#
#   sh scripts/release-sources.sh list
#   sh scripts/release-sources.sh get    <channel> <field>   # 原样输出，UNSET 也输出
#   sh scripts/release-sources.sh require <channel> <field>  # UNSET/空 即失败并说明缺什么
#   sh scripts/release-sources.sh channels [publish_filter]  # 通道名，可按 publish 过滤
#
# 字段表头在 SSOT 文件里，这里按表头定位列，避免手写字段序号。
# Actions/脚本只通过本脚本取通道口径，不允许各自硬编码提交号。

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SSOT="$ROOT/packages/RELEASE-SOURCES"

[ -f "$SSOT" ] || { echo "错误: 缺少发行事实源 $SSOT" >&2; exit 1; }

die() { echo "错误: $*" >&2; exit 1; }

# 只把数据行交给 awk：# 注释行（SSOT 里是大段说明）和空行都不能参与解析，
# 否则第一条空行会被当成表头，字段序号全体错位。
data() { grep -v '^[[:space:]]*#' "$SSOT" | grep -v '^[[:space:]]*$'; }

field_index() { # <field> -> 1-based column，按表头行
    _idx=$(data | awk -F'|' -v f="$1" 'NR==1{for(i=1;i<=NF;i++) if($i==f){print i; exit}}')
    [ -n "$_idx" ] || die "RELEASE-SOURCES 表头里没有字段 '$1'"
    printf '%s\n' "$_idx"
}

row() { # <channel> -> 整行
    _row=$(data | awk -F'|' -v c="$1" 'NR>1 && $1==c {print; exit}')
    [ -n "$_row" ] || die "RELEASE-SOURCES 里没有通道 '$1'（可用: $(channels_all)）"
    printf '%s\n' "$_row"
}

channels_all() {
    data | awk -F'|' 'NR>1 && $1!="" {printf "%s ", $1}' | sed 's/ $//'
}

get() { # <channel> <field>
    _row=$(row "$1")
    _idx=$(field_index "$2")
    printf '%s\n' "$_row" | cut -d'|' -f"$_idx"
}

require() { # <channel> <field>
    _val=$(get "$1" "$2")
    case "$_val" in
        ""|UNSET)
            die "通道 '$1' 的 $2 尚未指定（值='$_val'）。
       发行工具不会替你推断或生成源码提交：请由补丁/源码准备线固定该提交后填入
       packages/RELEASE-SOURCES。见 docs/gnb-maintenance-plan.md「开发与发行的职责边界」。"
            ;;
    esac
    printf '%s\n' "$_val"
}

case "${1:-list}" in
    list)
        data | awk -F'|' 'NR==1{for(i=1;i<=NF;i++) printf "%-22s", $i; print ""; next}
                        {for(i=1;i<=NF;i++) printf "%-22s", $i; print ""}'
        ;;
    get)     [ $# -eq 3 ] || die "用法: release-sources.sh get <channel> <field>"; get "$2" "$3" ;;
    require) [ $# -eq 3 ] || die "用法: release-sources.sh require <channel> <field>"; require "$2" "$3" ;;
    channels)
        case "${2:-all}" in
            all) printf '%s\n' "$(channels_all)" ;;
            *)   data | awk -F'|' -v p="$2" 'NR==1{for(i=1;i<=NF;i++) if($i=="publish") pi=i; next}
                                                 p!="" && $pi==p {print $1}' ;;
        esac
        ;;
    -h|--help|usage) sed -n '2,12p' "$0"; exit 1 ;;
    *) die "未知子命令: $1（见 sh scripts/release-sources.sh usage）" ;;
esac
