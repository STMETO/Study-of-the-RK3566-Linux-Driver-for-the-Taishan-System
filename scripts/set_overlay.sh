#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# set_overlay.sh - 在板子上登记 led8 overlay（修改 /boot/ubootEnv.txt）
#
# 前提：led8.dtbo 已经放到 /boot/overlays/led8.dtbo
# 效果：下次重启时 U-Boot 会把 led8.dtbo 合并进设备树，内核就能看到 led8 节点。
#
# 用法（在板子上执行）：
#   sh set_overlay.sh
#
# 说明：
#   /boot/ubootEnv.txt 是瑞芯微 U-Boot 读取的环境变量文件，
#   其中 overlays= 后面跟一串 dtbo 文件名（空格分隔），
#   本脚本只负责"确保 led8.dtbo 在列表里"，不重复添加。

set -e

CFG=/boot/ubootEnv.txt

mkdir -p /boot/overlays
[ -f "$CFG" ] || touch "$CFG"

if grep -q '^overlays=' "$CFG"; then
	# 已经有 overlays 行：只在里面没有 led8.dtbo 时追加
	if ! grep -q 'led8.dtbo' "$CFG"; then
		sed -i 's|^overlays=\(.*\)|overlays=\1 led8.dtbo|' "$CFG"
	fi
else
	# 还没有 overlays 行：新建一行
	echo 'overlays=led8.dtbo' >> "$CFG"
fi

sync

echo "---- $CFG ----"
cat "$CFG"
echo "----------------"
echo "reboot to apply the overlay"
