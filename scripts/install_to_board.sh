#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# install_to_board.sh - 把驱动、overlay、CLI 传到板子上
#
# 用法：
#   BOARD=root@192.168.1.100 ./scripts/install_to_board.sh
#
# 做的事：
#   1. scp w25q64.ko / w25q64.dtbo / w25q64_cli（以及可选的 fake-spi-flash.ko）
#      到板子的 /root/w25q64
#   2. 把 dtbo 放进 /boot/overlays/
#   3. 把 CLI 装到 /usr/bin/w25q64（方便直接敲）
#   4. 提示你改 /boot/ubootEnv.txt 并重启
set -euo pipefail

BOARD=${BOARD:-root@192.168.1.100}
DEST=${DEST:-/root/w25q64}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out

KO=$OUT/w25q64.ko
DTBO=$OUT/w25q64.dtbo
CLI=$OUT/w25q64_cli
FAKE=$OUT/fake-spi-flash.ko

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
# fake-spi-flash.ko 是可选的：只有做"无硬件测试"时才需要
if [ -f "$FAKE" ]; then
	scp -q "$FAKE" "$BOARD:$DEST/"
else
	echo "    （没有 out/fake-spi-flash.ko，跳过；需要无硬件测试请先 make fake）"
fi
scp -q "$ROOT/scripts/set_overlay.sh" "$ROOT/scripts/fake_spi.sh" "$BOARD:$DEST/"

echo "==> 板端安装"
ssh "$BOARD" "
	set -e
	mkdir -p /boot/overlays
	cp '$DEST/w25q64.dtbo' /boot/overlays/
	cp '$DEST/w25q64_cli' /usr/bin/w25q64
	chmod +x /usr/bin/w25q64 '$DEST/set_overlay.sh' '$DEST/fake_spi.sh'
	echo '  /boot/overlays/w25q64.dtbo 已就位'
	echo '  /usr/bin/w25q64 已就位'
"

cat <<EOF

==> 完成。接下来在板子上做：

  1) 让 U-Boot 启动时叠加 overlay（二选一）：
       $DEST/set_overlay.sh            # 自动改 /boot/ubootEnv.txt
     或者把 w25q64.dtbo 烘进整颗 dtb（make dtb 后替换 boot.img 里的 dtb）。
     overlay 会把板级 dts 自带的 spidev 节点 spi_test@0 设成 disabled，
     这样 CS0 才空出来给 flash@0。改完重启。

  2) 重启后确认设备树里有节点：
       ls /proc/device-tree/spi@fe640000/ | grep flash
       cat /proc/device-tree/spi@fe640000/flash@0/compatible

  3) 加载驱动并测试：
       insmod $DEST/w25q64.ko
       w25q64 info
       w25q64 write 0x0 --text "hello w25q64"
       w25q64 dump 0x0 64
       w25q64 selftest --size 0x10000

  4) 没有真实 flash 时用假 SPI 控制器：
       $DEST/fake_spi.sh

  卸载：
       rmmod w25q64
EOF
