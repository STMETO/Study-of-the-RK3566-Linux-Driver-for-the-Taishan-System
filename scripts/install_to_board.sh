#!/bin/bash
# Install the GPIO3_A1 demo to a running board over SSH.
#
#   BOARD=root@192.168.1.100 ./scripts/install_to_board.sh
#
# It copies the .ko/.dtbo/cli, registers the overlay in /boot/ubootEnv.txt and
# installs the CLI to /usr/bin. Reboot is required for the overlay to apply.
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BOARD=${BOARD:-root@192.168.1.100}
REMOTE_DIR=${REMOTE_DIR:-/root/gpio3a1}

for f in "$WORK/drivers/gpio3a1/gpio3a1.ko" \
	 "$WORK/dts/gpio3a1.dtbo" \
	 "$WORK/apps/gpio3a1_cli/gpio3a1_cli"; do
	[ -f "$f" ] || { echo "missing $f, run scripts/build_all.sh" >&2; exit 1; }
done

ssh "$BOARD" "mkdir -p $REMOTE_DIR /boot/overlays"
scp "$WORK/drivers/gpio3a1/gpio3a1.ko" \
    "$WORK/dts/gpio3a1.dtbo" \
    "$WORK/apps/gpio3a1_cli/gpio3a1_cli" \
    "$BOARD:$REMOTE_DIR/"

ssh "$BOARD" "REMOTE_DIR=$REMOTE_DIR sh -s" <<'EOF'
set -e
install -m 644 "$REMOTE_DIR/gpio3a1.dtbo" /boot/overlays/gpio3a1.dtbo
install -m 755 "$REMOTE_DIR/gpio3a1_cli" /usr/bin/gpio3a1
touch /boot/ubootEnv.txt
if grep -q '^overlays=' /boot/ubootEnv.txt; then
	if ! grep -q 'gpio3a1.dtbo' /boot/ubootEnv.txt; then
		sed -i 's|^overlays=\(.*\)|overlays=\1 gpio3a1.dtbo|' /boot/ubootEnv.txt
	fi
else
	echo 'overlays=gpio3a1.dtbo' >> /boot/ubootEnv.txt
fi
sync
echo "installed. /boot/ubootEnv.txt:"
grep '^overlays=' /boot/ubootEnv.txt
EOF

cat <<EOF

Next steps on the board:
  reboot
  insmod $REMOTE_DIR/gpio3a1.ko
  gpio3a1 get
  gpio3a1 set 1
  gpio3a1 toggle
EOF
