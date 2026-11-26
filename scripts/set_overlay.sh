#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# set_overlay.sh - 在板子上把 at24c02.dtbo 加进 U-Boot 的 overlays 列表
#
# 泰山派 SDK 的 U-Boot 会读 /boot/ubootEnv.txt，其中：
#   overlays=xxx.dtbo,yyy.dtbo
# 这一行里的每个 dtbo 都会在启动时被 fdtoverlay 叠加到基准 dtb 上。
#
# 本脚本是幂等的：重复执行不会把同一条 overlay 加两遍。
set -eu

OVERLAY=${1:-at24c02.dtbo}
ENV_FILE=${ENV_FILE:-/boot/ubootEnv.txt}

[ -f "$ENV_FILE" ] || {
	echo "找不到 $ENV_FILE" >&2
	exit 1
}

cp -a "$ENV_FILE" "$ENV_FILE.bak"

if grep -q '^overlays=' "$ENV_FILE"; then
	# 取出当前的 overlays 值（可能有多个 dtbo，用逗号分隔）
	CUR=$(sed -n 's/^overlays=//p' "$ENV_FILE" | tail -n 1)

	case ",$CUR," in
	*",$OVERLAY,"*)
		echo "overlays 里已经包含 $OVERLAY，无需修改"
		exit 0
		;;
	esac

	if [ -n "$CUR" ]; then
		NEW="$CUR,$OVERLAY"
	else
		NEW="$OVERLAY"
	fi
	# 用 | 作分隔符，避免值里的 / 干扰
	sed -i "s|^overlays=.*|overlays=$NEW|" "$ENV_FILE"
else
	printf 'overlays=%s\n' "$OVERLAY" >> "$ENV_FILE"
fi

echo "已更新 $ENV_FILE："
grep '^overlays=' "$ENV_FILE" | sed 's/^/  /'
echo "原文件备份在 $ENV_FILE.bak"
echo "重启后 overlay 才会生效："
echo "  sync && reboot"
