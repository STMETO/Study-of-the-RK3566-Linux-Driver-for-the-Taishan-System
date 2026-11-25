#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# install_to_board.sh - 通过 SSH 把 led8 工程部署到已经启动的板子上
#
# 用法（在容器里执行，板子要能 ssh 登录）：
#   BOARD=root@192.168.1.100 ./scripts/install_to_board.sh
#
# 做了什么：
#   1. 检查 out/ 下三个产物是否存在（不存在就提示先 make）；
#   2. 把 led8.ko / led8.dtbo / led8_cli 拷到板子 /root/led8/；
#   3. 把 dtbo 放到 /boot/overlays/，并在 /boot/ubootEnv.txt 里登记 overlays=；
#   4. 把 CLI 装到 /usr/bin/led8，方便直接敲命令。
#
# 注意：overlay 必须在启动阶段由 U-Boot 合并，所以装完要重启一次板子。

set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BOARD=${BOARD:-root@192.168.1.100}
REMOTE_DIR=${REMOTE_DIR:-/root/led8}

# ---- 1. 检查本地产物 ----
for f in "$WORK/out/led8.ko" \
	 "$WORK/out/led8.dtbo" \
	 "$WORK/out/led8_cli"; do
	[ -f "$f" ] || { echo "missing $f, run 'make' in $WORK" >&2; exit 1; }
done

echo "[1/3] copy artifacts to $BOARD:$REMOTE_DIR"
ssh "$BOARD" "mkdir -p $REMOTE_DIR /boot/overlays"
scp -q "$WORK/out/led8.ko" \
       "$WORK/out/led8.dtbo" \
       "$WORK/out/led8_cli" \
       "$BOARD:$REMOTE_DIR/"

# ---- 2. 在板子上安装 dtbo、登记 overlay、安装 CLI ----
echo "[2/3] install dtbo / register overlay / install cli"
ssh "$BOARD" "REMOTE_DIR=$REMOTE_DIR sh -s" <<'EOF'
set -e
install -m 644 "$REMOTE_DIR/led8.dtbo" /boot/overlays/led8.dtbo
install -m 755 "$REMOTE_DIR/led8_cli" /usr/bin/led8

touch /boot/ubootEnv.txt
if grep -q '^overlays=' /boot/ubootEnv.txt; then
	if ! grep -q 'led8.dtbo' /boot/ubootEnv.txt; then
		sed -i 's|^overlays=\(.*\)|overlays=\1 led8.dtbo|' /boot/ubootEnv.txt
	fi
else
	echo 'overlays=led8.dtbo' >> /boot/ubootEnv.txt
fi
sync
echo "---- /boot/ubootEnv.txt ----"
grep '^overlays=' /boot/ubootEnv.txt
EOF

# ---- 3. 打印后续手动步骤 ----
echo "[3/3] done. next steps on the board:"
cat <<EOF

  reboot                                  # 让 U-Boot 合并 led8.dtbo
  insmod $REMOTE_DIR/led8.ko              # 加载驱动
  dmesg | tail                            # 应看到 "ready: major=... "
  cat /proc/devices | grep led8           # 确认设备号已注册
  ls -l /dev/led8                         # 确认设备节点
  led8 info                               # 打印通道数
  led8 dump                               # 读全部通道
  led8 set 0 200 && led8 dump             # 写 0 号通道
  led8 demo                               # 流水灯演示
  rmmod led8                              # 卸载，/dev/led8 消失
EOF
