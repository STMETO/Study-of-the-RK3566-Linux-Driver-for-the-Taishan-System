#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# fake_spi.sh - 没有真实 SPI flash 时，用假 SPI 控制器跑通整个驱动
#
# 原理：
#   fake-spi-flash.ko 注册了一个软件模拟的 SPI 控制器，它背后是一个内存数组，
#   能按 W25Q 的时序应答 JEDEC ID / 读 / 页编程 / 扇区擦除 / 读状态。
#   加载后 SPI 核心里会多出一条 Bus 和一颗 "w25q64" 设备，
#   我们的 w25q64.ko 照样 probe，不需要真实芯片也能把
#   read/write/llseek/ioctl 整条链路跑通。
#
#   两个模块互相独立：fake-spi-flash.ko 只是"总线"，w25q64.ko 只是"设备驱动"。
#
# 在板子上运行（需要同目录下有 fake-spi-flash.ko 和 w25q64.ko）：
#   ./fake_spi.sh
#   ./fake_spi.sh 0x100000        # 想模拟 1MB 容量时
#
# 清理：
#   rmmod w25q64
#   rmmod fake-spi-flash
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
SIZE=${1:-}

KO=$DIR/w25q64.ko
FAKE=$DIR/fake-spi-flash.ko

[ -f "$KO" ]   || { echo "缺少 $KO（宿主机 make 后拷过来）" >&2; exit 1; }
[ -f "$FAKE" ] || { echo "缺少 $FAKE（宿主机 make fake 后拷过来）" >&2; exit 1; }

# --- 1. 加载假 SPI 控制器 -----------------------------------------------------
# 模块名是 fake-spi-flash（连字符），lsmod 里显示成 fake_spi_flash
if ! lsmod | grep -q '^fake_spi_flash'; then
	if [ -n "$SIZE" ]; then
		echo "==> insmod fake-spi-flash.ko flash_size=$SIZE"
		insmod "$FAKE" flash_size="$SIZE"
	else
		echo "==> insmod fake-spi-flash.ko"
		insmod "$FAKE"
	fi
else
	echo "==> fake_spi_flash 已在运行，跳过"
fi

# --- 2. 加载我们的驱动 --------------------------------------------------------
if ! lsmod | grep -q '^w25q64'; then
	echo "==> insmod w25q64.ko"
	insmod "$KO"
else
	echo "==> w25q64 已在运行，跳过"
fi

# --- 3. 等 udev/mdev 把 /dev/w25q64 建出来 ------------------------------------
i=0
while [ ! -e /dev/w25q64 ] && [ $i -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done

if [ -e /dev/w25q64 ]; then
	echo "==> 设备节点就绪:"
	ls -l /dev/w25q64
else
	echo "警告：/dev/w25q64 还没出现，检查 dmesg" >&2
fi

echo "==> 最近的 dmesg:"
dmesg | tail -n 15

cat <<EOF

==> 现在可以测试了：
    w25q64 info
    w25q64 write 0x0 --text "hello w25q64"
    w25q64 dump 0x0 64
    w25q64 selftest --size 0x10000
    w25q64 bench --size 0x40000

注意：假 flash 只是内存模拟，数据掉电即失；它也不模拟编程/擦除的真实耗时，
所以 bench 的数字会比真芯片快很多，这是正常的。
EOF
