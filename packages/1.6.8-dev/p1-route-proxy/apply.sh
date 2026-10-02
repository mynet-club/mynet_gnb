#!/bin/sh
set -eu
TARGET=${1:-.}
PATCH_DIR=$(CDPATH= cd -- "$(dirname "$0")/patches" && pwd)
cd "$TARGET"
for p in "$PATCH_DIR"/*.patch; do git apply --check -p1 "$p"; done
for p in "$PATCH_DIR"/*.patch; do git apply -p1 "$p"; done
grep -q 'gnb_proxy_cfg_t' src/gnb_proxy.h
grep -q 'gnb_proxy_cfg_load' src/gnb_core.c
grep -q 'gnb_proxy_pick_flow' src/packet_filter/gnb_pf_route.c
echo 'p1-route-proxy applied'
