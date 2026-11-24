#!/bin/bash
# Verify out/gpio3a1.dtbo against the SDK-built base DTB with fdtoverlay.
# No board needed. Requires "cd /build/sdk && ./build.sh kernel" once.
# The merged DTB is a temporary verification artifact and is discarded.
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${SDK:-/build/sdk}
BASE_DTB="${BASE_DTB:-$SDK/kernel/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dtb}"
DTBO="${DTBO:-$WORK/out/gpio3a1.dtbo}"

if [ ! -f "$BASE_DTB" ]; then
	echo "base dtb not found: $BASE_DTB" >&2
	echo "run 'cd $SDK && ./build.sh kernel' first" >&2
	exit 1
fi
if [ ! -f "$DTBO" ]; then
	echo "gpio3a1.dtbo not found: $DTBO" >&2
	echo "run 'make' in $WORK first" >&2
	exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/gpio3a1-verify-XXXXXX")
trap 'rm -rf "$TMP"' EXIT

MERGED=$TMP/merged-gpio3a1.dtb
fdtoverlay -i "$BASE_DTB" -o "$MERGED" "$DTBO"
echo "OK: overlay applied (temporary merged dtb discarded)"

echo "--- merged node ---"
dtc -I dtb -O dts "$MERGED" 2>/dev/null | grep -B2 -A9 "gpio3a1" || {
	echo "node gpio3a1 not found in merged dtb" >&2
	exit 1
}
