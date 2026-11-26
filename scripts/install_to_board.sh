#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# install_to_board.sh - 把驱动、overlay、CLI 传到板子上
#
# 用法：
#   BOARD=root@192.168.1.100 ./scripts/install_to_board.sh
#
# 做的事：
#   1. scp at24c02.ko / at24c02.dtbo / at24c02_cli 到板子的 /root/at24c02
#   2. 把 dtbo 放进 /boot/overlays/
#   3. 把 CLI 装到 /usr/bin/at24c02（方便直接敲）
#   4. 提示你改 /boot/ubootEnv.txt 并重启
set -euo pipefail

BOARD=${BOARD:-root@192.168.1.100}
DEST=${DEST:-/root/at24c02}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out

KO=$OUT/at24c02.ko
DTBO=$OUT/at24c02.dtbo
CLI=$OUT/at24c02_cli
STUB=$OUT/i2c-stub.ko

die() {
	echo "install: $*" >&2
	exit 1
}

[ -f "$KO" ]   || die "找不到 $KO，先在宿主机上 make"
[ -f "$DTBO" ] || die "找不到 $DTBO，先在宿主机上 make"
[ -f "$CLI" ]  || die "找不到 $CLI，先在宿主机上 make"

echo "==> 目标板: $BOARD"
echo "==> 设备端目录: $DEST"

ssh "$BOARD" "mkdir -p '$DEST'"

echo "==> 拷贝文件"
scp -q "$KO" "$DTBO" "$CLI" "$BOARD:$DEST/"
# i2c-stub.ko 是可选的：只有做"无硬件测试"时才需要
if [ -f "$STUB" ]; then
	scp -q "$STUB" "$BOARD:$DEST/"
else
	echo "    （没有 out/i2c-stub.ko，跳过；需要无硬件测试请先 make stub）"
fi
scp -q "$ROOT/scripts/set_overlay.sh" "$ROOT/scripts/fake_i2c.sh" "$BOARD:$DEST/"

echo "==> 板端安装"
ssh "$BOARD" "
	set -e
	mkdir -p /boot/overlays
	cp '$DEST/at24c02.dtbo' /boot/overlays/
	cp '$DEST/at24c02_cli' /usr/bin/at24c02
	chmod +x /usr/bin/at24c02 '$DEST/set_overlay.sh' '$DEST/fake_i2c.sh'
	echo '  /boot/overlays/at24c02.dtbo 已就位'
	echo '  /usr/bin/at24c02 已就位'
"

cat <<EOF

==> 完成。接下来在板子上做：

  1) 让 U-Boot 启动时叠加 overlay（二选一）：
       $DEST/set_overlay.sh            # 自动改 /boot/ubootEnv.txt
     或者把 at24c02.dtbo 烘进整颗 dtb（make dtb 后替换 boot.img 里的 dtb）。
     改完重启。

  2) 重启后确认设备树里有节点：
       ls /proc/device-tree/i2c@fe5b0000/ | grep eeprom
       cat /proc/device-tree/i2c@fe5b0000/eeprom@50/compatible

  3) 加载驱动并测试：
       insmod $DEST/at24c02.ko
       at24c02 info
       at24c02 write 0x10 41 42 43 44
       at24c02 read  0x10 8
       at24c02 bench

  4) 没有真实 EEPROM 时用 i2c-stub 假总线：
       $DEST/fake_i2c.sh

  卸载：
       rmmod at24c02
EOF
