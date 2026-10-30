#!/bin/bash
# ============================================================================
#  build-all.sh —— 一条命令产出两个 ipk（面板 + IMS 引擎）
#
#  用法：
#      bash build-all.sh                 # 用脚本里的默认版本号
#      bash build-all.sh 0.3.6 0.1.14    # 指定 面板版本 引擎版本
#
#  产物：
#      src/dist/luci-app-fzs_<面板版本>_all.ipk          面板包
#      build/packages/imsengine_<引擎版本>-1_mipsel_24kc.ipk  引擎包
#      build/packages/luci-app-fzs_<面板版本>-1_all.ipk       面板包（重封，带 release 号）
#      build/packages/fzs-ims-suite-<引擎版本>.tar.gz        恢复包
#      build/packages/sha256sums                             校验值
#
#  顺序很重要：先面板、后引擎。引擎包的 postinst 要检测/补齐面板文件，
#  且 mkpkg.py 默认从 src/dist/ 取面板 ipk 重封。
# ============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"
PANEL_VER="${1:-0.3.7}"

command -v "$PY" >/dev/null 2>&1 || PY=python

echo ">>> [1/2] 打包面板 luci-app-fzs $PANEL_VER"
( cd "$HERE/src" && "$PY" build_ipk.py "$PANEL_VER" )

echo ">>> [2/2] 打包引擎 imsengine（含面板重封 + 恢复包）"
"$PY" "$HERE/engine/mkpkg.py"

echo
echo ">>> 全部完成，产物在 build/packages/："
ls -l "$HERE/build/packages/"
