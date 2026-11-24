#!/bin/bash
# Build dts/tspi-rk3566-gpio3a1.dts -> dts/tspi-rk3566-gpio3a1.dtb
# Full board DTB variant (includes the unmodified SDK board DTS).
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${SDK:-/build/sdk}
KERNEL_DIR=${KERNEL_DIR:-$SDK/kernel}

CPP=${CPP:-gcc}
DTC=${DTC:-dtc}

TMP=$(mktemp "${TMPDIR:-/tmp}/gpio3a1-dtb-XXXXXX.dts")
trap 'rm -f "$TMP"' EXIT

"$CPP" -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I"$KERNEL_DIR/include" \
	-I"$KERNEL_DIR/arch/arm64/boot/dts" \
	-I"$KERNEL_DIR/arch/arm64/boot/dts/rockchip" \
	-o "$TMP" "$WORK/dts/tspi-rk3566-gpio3a1.dts"

"$DTC" -@ -I dts -O dtb -o "$WORK/dts/tspi-rk3566-gpio3a1.dtb" "$TMP"
echo "OK: $WORK/dts/tspi-rk3566-gpio3a1.dtb"
