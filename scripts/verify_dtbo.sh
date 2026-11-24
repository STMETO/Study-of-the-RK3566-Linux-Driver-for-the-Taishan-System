#!/bin/bash
# Verify gpio3a1.dtbo against the SDK-built base DTB with fdtoverlay.
# No board needed. Requires "cd /build/sdk && ./build.sh kernel" once.
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${SDK:-/build/sdk}
BASE_DTB="${BASE_DTB:-$SDK/kernel/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dtb}"
OUT=${OUT:-$WORK/out/merged-gpio3a1.dtb}

if [ ! -f "$BASE_DTB" ]; then
	echo "base dtb not found: $BASE_DTB" >&2
	echo "run 'cd $SDK && ./build.sh kernel' first" >&2
	exit 1
fi
if [ ! -f "$WORK/dts/gpio3a1.dtbo" ]; then
	echo "gpio3a1.dtbo not found, run scripts/build_dtbo.sh first" >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT")"
fdtoverlay -i "$BASE_DTB" -o "$OUT" "$WORK/dts/gpio3a1.dtbo"
echo "OK: overlay applied -> $OUT"

echo "--- merged node ---"
dtc -I dtb -O dts "$OUT" 2>/dev/null | grep -B2 -A9 "gpio3a1" || {
	echo "node gpio3a1 not found in merged dtb" >&2
	exit 1
}
