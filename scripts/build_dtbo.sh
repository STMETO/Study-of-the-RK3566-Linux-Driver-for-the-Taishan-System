#!/bin/bash
# Build dts/gpio3a1-overlay.dts -> dts/gpio3a1.dtbo
# Out-of-tree: only the SDK kernel headers/DTS include paths are read.
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${SDK:-/build/sdk}
KERNEL_DIR=${KERNEL_DIR:-$SDK/kernel}

CPP=${CPP:-gcc}
DTC=${DTC:-dtc}

TMP=$(mktemp "${TMPDIR:-/tmp}/gpio3a1-XXXXXX.dts")
trap 'rm -f "$TMP"' EXIT

"$CPP" -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I"$KERNEL_DIR/include" \
	-I"$KERNEL_DIR/arch/arm64/boot/dts" \
	-I"$KERNEL_DIR/arch/arm64/boot/dts/rockchip" \
	-o "$TMP" "$WORK/dts/gpio3a1-overlay.dts"

"$DTC" -@ -I dts -O dtb -o "$WORK/dts/gpio3a1.dtbo" "$TMP"
echo "OK: $WORK/dts/gpio3a1.dtbo"
