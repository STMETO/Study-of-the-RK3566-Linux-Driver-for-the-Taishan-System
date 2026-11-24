#!/bin/sh
# Run ON THE BOARD: enable gpio3a1.dtbo in /boot/ubootEnv.txt.
# The dtbo itself must already be at /boot/overlays/gpio3a1.dtbo.
set -e

CFG=/boot/ubootEnv.txt
mkdir -p /boot/overlays
[ -f "$CFG" ] || touch "$CFG"

if grep -q '^overlays=' "$CFG"; then
	grep -q 'gpio3a1.dtbo' "$CFG" || \
		sed -i 's|^overlays=\(.*\)|overlays=\1 gpio3a1.dtbo|' "$CFG"
else
	echo 'overlays=gpio3a1.dtbo' >> "$CFG"
fi

sync
echo "---- $CFG ----"
cat "$CFG"
