#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# verify_dtbo.sh - 不需要板子就能验证 overlay 是否正确
#
# 做法：把 out/w25q64.dtbo 用 fdtoverlay 合并进 SDK 编出来的基准 dtb，
# 再用 dtc 反编译成文本，检查：
#   1. flash@0 节点真的出现在 spi@fe640000（也就是 spi3）下面
#   2. compatible 是 work,w25q64
#   3. reg = 0（用 CS0）
#   4. 板级 dts 自带的 spi_test@0 已经被 disabled（CS0 没被 spidev 占着）
#
# 这样在烧板子之前就能发现"符号没解析""节点挂错父节点""属性打错字"这类问题。
set -euo pipefail

BASE_DTB=${BASE_DTB:-/build/sdk/kernel/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dtb}
DTBO=${DTBO:-$(cd "$(dirname "$0")/.." && pwd)/out/w25q64.dtbo}

die() {
	echo "verify: $*" >&2
	exit 1
}

[ -f "$BASE_DTB" ] || die "基准 dtb 不存在: $BASE_DTB（先整体编译一次 SDK）"
[ -f "$DTBO" ] || die "overlay 不存在: $DTBO（先 make）"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "==> base dtb : $BASE_DTB"
echo "==> overlay  : $DTBO"

# fdtoverlay 要求输入输出都是 dtb 文件；这里先拷一份基准 dtb 再叠加
cp "$BASE_DTB" "$TMP/base.dtb"
fdtoverlay -i "$TMP/base.dtb" -o "$TMP/merged.dtb" "$DTBO"

# 反编译成可读的 dts 文本
dtc -I dtb -O dts -o "$TMP/merged.dts" "$TMP/merged.dtb" 2>/dev/null

# --- 检查 1：compatible 是否正确 ------------------------------------------------
if ! grep -q 'compatible = "work,w25q64";' "$TMP/merged.dts"; then
	die "合并后的 dts 里找不到 compatible = \"work,w25q64\""
fi
echo "  [ok] compatible = work,w25q64"

# --- 检查 2：flash@0 是否挂在 spi@fe640000 下面 --------------------------------
# dtc 输出的 dts 靠缩进表示层级：先遇到 spi@fe640000 这一行，之后遇到的
# flash@0 就认为是它的子节点。因为整份 dts 里只有 spi3 下面有这个节点，
# 这种"顺序 + 标志位"的检查足够可靠。
if ! awk '
	/^[[:space:]]*spi@fe640000[[:space:]]*\{/ { in_spi3 = 1 }
	in_spi3 && /^[[:space:]]*flash@0[[:space:]]*\{/ { found = 1 }
	END { exit(found ? 0 : 1) }
' "$TMP/merged.dts"; then
	die "spi@fe640000 下面没有 flash@0（overlay 没挂上？）"
fi
echo "  [ok] flash@0 位于 spi@fe640000 (spi3) 之下"

# --- 检查 3：reg 是否等于 0 ----------------------------------------------------
if ! awk '
	/^[[:space:]]*flash@0[[:space:]]*\{/ { in_node = 1 }
	in_node && /reg = <0x00>/ { found = 1 }
	in_node && /^[[:space:]]*\};/ { in_node = 0 }
	END { exit(found ? 0 : 1) }
' "$TMP/merged.dts"; then
	die "flash@0 节点的 reg 不是 <0x00>"
fi
echo "  [ok] reg = <0x00> (CS0)"

# --- 检查 4：spi_test@0 是否已 disabled ----------------------------------------
if ! awk '
	/^[[:space:]]*spi_test@0[[:space:]]*\{/ { in_node = 1 }
	in_node && /status = "disabled"/ { found = 1 }
	in_node && /^[[:space:]]*\};/ { in_node = 0 }
	END { exit(found ? 0 : 1) }
' "$TMP/merged.dts"; then
	die "spi_test@0 没有被 disabled，CS0 会被 spidev 占着"
fi
echo "  [ok] spi_test@0 已让出 CS0"

# --- 打印合并后的节点内容，方便肉眼确认 ----------------------------------------
echo "==> merged node:"
awk '
	/^[[:space:]]*spi@fe640000[[:space:]]*\{/ { in_spi3 = 1 }
	in_spi3 && /^[[:space:]]*(flash@0|spi_test@0)[[:space:]]*\{/ { in_node = 1 }
	in_node { print }
	in_node && /^[[:space:]]*\};/ { in_node = 0 }
' "$TMP/merged.dts" | sed 's/^/    /'

echo "verify: all checks passed"
