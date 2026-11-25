#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# verify_dtbo.sh - 不需要板子，验证 out/led8.dtbo 能不能正确合并进基准 dtb
#
# 原理：
#   fdtoverlay 是 libfdt 自带的小工具，行为和 U-Boot 启动时合并 overlay 一样：
#   把 overlay 的 fragment 按 target 打进基础 dtb，得到一棵新的 dtb。
#   如果 overlay 里引用了不存在的标签、或者片段结构写错，这一步就会报错。
#
# 用法（一般由顶层 Makefile 的 "make verify" 调用）：
#   ./scripts/verify_dtbo.sh
#   BASE_DTB=/path/to/base.dtb DTBO=/path/to/led8.dtbo ./scripts/verify_dtbo.sh
#
# 前提：SDK 内核已经编译过一次，生成了
#   kernel/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dtb
# 合并出来的临时 dtb 用完即删，不会留在 out/ 里。

set -euo pipefail

# 工程根目录（本脚本在 scripts/ 下）
WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

SDK=${SDK:-/build/sdk}
BASE_DTB="${BASE_DTB:-$SDK/kernel/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dtb}"
DTBO="${DTBO:-$WORK/out/led8.dtbo}"

# ---- 前置检查：文件在不在 ----
if [ ! -f "$BASE_DTB" ]; then
	echo "base dtb not found: $BASE_DTB" >&2
	echo "run 'cd $SDK && ./build.sh kernel' once to generate it" >&2
	exit 1
fi
if [ ! -f "$DTBO" ]; then
	echo "led8.dtbo not found: $DTBO" >&2
	echo "run 'make' in $WORK first" >&2
	exit 1
fi

# ---- 用临时目录做合并，退出时自动清理 ----
TMP=$(mktemp -d "${TMPDIR:-/tmp}/led8-verify-XXXXXX")
trap 'rm -rf "$TMP"' EXIT

MERGED=$TMP/merged-led8.dtb

echo "[1/3] fdtoverlay: $DTBO -> $BASE_DTB"
fdtoverlay -i "$BASE_DTB" -o "$MERGED" "$DTBO"
echo "      OK, overlay applied"

# ---- 把合并后的 dtb 反编译回 dts，检查节点内容和属性 ----
echo "[2/3] dump merged led8 node"
dtc -I dtb -O dts "$MERGED" 2>/dev/null > "$TMP/merged.dts"

if ! grep -q 'work,led8' "$TMP/merged.dts"; then
	echo "ERROR: compatible \"work,led8\" not found in merged dtb" >&2
	exit 1
fi

# 打印 led8 节点前后几行，人工确认 channels / default-brightness
grep -B1 -A8 'led8 {' "$TMP/merged.dts" || {
	echo "ERROR: led8 node not found in merged dtb" >&2
	exit 1
}

echo
echo "[3/3] property check"
grep -q 'channels = <0x08>' "$TMP/merged.dts" \
	&& echo "      channels = <8>            OK" \
	|| { echo "ERROR: channels property missing" >&2; exit 1; }
grep -q 'default-brightness = <0x00>' "$TMP/merged.dts" \
	&& echo "      default-brightness = <0> OK" \
	|| { echo "ERROR: default-brightness property missing" >&2; exit 1; }

echo
echo "verify passed: overlay merges cleanly, temporary dtb discarded"
