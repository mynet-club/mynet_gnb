#!/bin/sh
set -eu
TARGET=${1:-.}
cd "$TARGET"
git apply -R -p1 "$(CDPATH= cd -- "$(dirname "$0")/patches" && pwd)/0001-proxy-conf.patch"
echo 'p1-route-proxy reverted'
