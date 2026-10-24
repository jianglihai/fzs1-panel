#!/bin/bash
# ============================================================================
#  build-engine.sh —— 交叉编译 IMS 引擎（Go）→ engine/board-files/usr/bin/imsengine
#
#  目标平台：ramips/mt76x8（MIBOX-668M2）= MIPS 24Kc little-endian，无硬件浮点
#      GOOS=linux  GOARCH=mipsle  GOMIPS=softfloat  CGO_ENABLED=0
#
#  用法：
#      bash engine/build-engine.sh
#
#  说明：平时**不需要**跑这个脚本 —— 仓库里已经带着编译好的二进制，
#        mkpkg.py 直接取用。只有改动 engine/imsengine/*.go 之后才需要重编，
#        而且必须在 Linux / WSL2 / Git Bash 下（Windows 原生 cmd 不行）。
# ============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/imsengine"
OUT="$HERE/board-files/usr/bin/imsengine"

command -v go >/dev/null 2>&1 || {
    echo "!! 未找到 go，请先装 Go 1.21+（https://go.dev/dl/）" >&2
    exit 1
}

echo ">>> 编译 imsengine（linux/mipsle, softfloat）"
cd "$SRC"
GOOS=linux GOARCH=mipsle GOMIPS=softfloat CGO_ENABLED=0 \
    go build -trimpath -ldflags "-s -w" -o "$OUT" .
echo ">>> 完成：$OUT（$(du -h "$OUT" | cut -f1)）"
