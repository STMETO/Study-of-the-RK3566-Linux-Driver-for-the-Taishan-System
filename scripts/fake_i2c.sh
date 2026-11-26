#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# fake_i2c.sh - 没有真实 EEPROM 时，用 i2c-stub 造一条假 I2C 总线来跑驱动
#
# 原理：
#   i2c-stub 是内核自带的一个"假 I2C 适配器"。insmod 之后系统里会多出一条
#   I2C 总线（名字叫 i2c-stub），它背后没有真实硬件，只是一个 256 字节的
#   内存数组，所有 SMBus 读写都落在这个数组上。
#
#   然后往这条总线 new_device 一个地址 0x50 的 i2c 设备，
#   内核就会创建 i2c_client，用名字 "at24c02" 去匹配我们的驱动。
#
# 这样 /dev/at24c02、read/write/ioctl、页切分、tWR 延时……整条链路都能跑，
# 唯一"假"的只是最后那几根 I2C 线。
#
# 在板子上运行（需要同目录下有 i2c-stub.ko 和 at24c02.ko）：
#   ./fake_i2c.sh
#
# 清理：
#   rmmod at24c02
#   echo 0x50 > /sys/bus/i2c/devices/i2c-N/delete_device
#   rmmod i2c_stub
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
CHIP=0x50

KO=$DIR/at24c02.ko
STUB=$DIR/i2c-stub.ko

[ -f "$KO" ] || { echo "缺少 $KO（宿主机 make 后拷过来）" >&2; exit 1; }
[ -f "$STUB" ] || { echo "缺少 $STUB（宿主机 make stub 后拷过来）" >&2; exit 1; }

# --- 1. 加载 i2c-stub ---------------------------------------------------------
# 模块名是 i2c_stub（下划线），lsmod 里也显示下划线形式
if ! lsmod | grep -q '^i2c_stub'; then
	echo "==> insmod i2c-stub.ko chip_addr=$CHIP"
	insmod "$STUB" chip_addr="$CHIP"
else
	echo "==> i2c_stub 已在运行，跳过"
fi

# --- 2. 找到 stub 造出来的那条总线号 ------------------------------------------
BUS=""
for d in /sys/class/i2c-adapter/i2c-*; do
	[ -e "$d/name" ] || continue
	if [ "$(cat "$d/name")" = "i2c-stub" ]; then
		BUS=${d##*-}
	fi
done

[ -n "$BUS" ] || { echo "没找到 i2c-stub 适配器" >&2; exit 1; }
echo "==> i2c-stub 总线号: $BUS"

# --- 3. 在这条总线上实例化一颗 0x50 的 EEPROM ---------------------------------
DEVDIR=/sys/bus/i2c/devices/$BUS-0050
if [ ! -e "$DEVDIR" ]; then
	echo "==> new_device: $CHIP on i2c-$BUS"
	echo at24c02 "$CHIP" > "/sys/bus/i2c/devices/i2c-$BUS/new_device"
else
	echo "==> $DEVDIR 已存在，跳过"
fi

# --- 4. 加载我们的驱动 --------------------------------------------------------
if ! lsmod | grep -q '^at24c02'; then
	echo "==> insmod at24c02.ko"
	insmod "$KO"
else
	echo "==> at24c02 已在运行，跳过"
fi

# --- 5. 等 udev/mdev 把 /dev/at24c02 建出来 -----------------------------------
i=0
while [ ! -e /dev/at24c02 ] && [ $i -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done

if [ -e /dev/at24c02 ]; then
	echo "==> 设备节点就绪:"
	ls -l /dev/at24c02
else
	echo "警告：/dev/at24c02 还没出现，检查 dmesg" >&2
fi

echo "==> 最近的 dmesg:"
dmesg | tail -n 15

cat <<EOF

==> 现在可以测试了：
    at24c02 info
    at24c02 write 0x00 11 22 33 44
    at24c02 dump 0 32
    at24c02 bench

注意：i2c-stub 只是内存模拟，数据掉电即失；它也不模拟 tWR 延时，
所以 bench 会比真实芯片快很多，这是正常的。
EOF
