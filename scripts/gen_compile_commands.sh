#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# gen_compile_commands.sh - 生成 clangd 用的 compile_commands.json
#
# 为什么需要它：
#   clangd 不是"猜"代码怎么编译的，它需要一个编译数据库，里面记录每个源文件
#   真实的编译命令（-I 头文件路径、-D 宏定义、-include 强制包含、-std=...）。
#   内核模块的编译参数有几百个，手写不现实，所以这里玩了个小技巧：
#     1. 把源码拷进临时目录
#     2. 用 V=1 真正编译一次，把完整命令行打印到日志
#     3. 从日志里把 w25q64.c / fake_spi_flash.c 那两条 gcc 命令抠出来
#     4. 把临时路径替换成真实路径，写进 JSON
#   C++ 那两个文件参数很简单，直接手写。
#
# 生成的 compile_commands.json 是绝对路径，换机器/换 SDK 路径后要重跑
# （make compdb）。它已被 .gitignore 忽略，不提交。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

SDK=${SDK:-/build/sdk}
KERNEL_DIR=${KERNEL_DIR:-$SDK/kernel}
ARCH=${ARCH:-arm64}
CROSS_COMPILE=${CROSS_COMPILE:-$SDK/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}

[ -f "$KERNEL_DIR/Makefile" ] || { echo "找不到内核源码: $KERNEL_DIR" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# 从一份编译日志里抠出编译某个源文件的命令行，
# 并把临时的 staging 路径换成真实源码目录
grab_cmd() {
	local log=$1 src=$2 stage=$3 real_dir=$4

	local line
	line=$(grep -E '[[:space:]]-c[[:space:]]' "$log" | grep -- "$src" | tail -n 1) || true
	[ -n "$line" ] || { echo "没能从编译日志里找到 $src 的编译命令" >&2; exit 1; }

	# 去掉可能存在的 "make[1]: " 前缀
	line=${line#*: }

	printf '%s' "$line" \
		| sed "s#$stage#$real_dir#g" \
		| sed -E 's#-o [^ ]+\.o#-o /dev/null#'
}

# --- 1. 驱动模块 ---------------------------------------------------------------
STAGE1=$WORK/stage1
mkdir -p "$STAGE1"
cp "$ROOT/drivers/w25q64/w25q64.c" \
   "$ROOT/drivers/w25q64/Kbuild" \
   "$ROOT/include/w25q64_uapi.h" "$STAGE1/"

echo "==> 编译驱动并抓取命令行（V=1）"
LOG1=$WORK/driver.log
if ! make -C "$KERNEL_DIR" M="$STAGE1" ARCH="$ARCH" \
	CROSS_COMPILE="$CROSS_COMPILE" V=1 modules >"$LOG1" 2>&1; then
	echo "编译失败，日志尾部：" >&2
	tail -n 40 "$LOG1" >&2
	exit 1
fi
DRIVER_CMD=$(grab_cmd "$LOG1" 'w25q64\.c' "$STAGE1" "$ROOT/drivers/w25q64")
DRIVER_CMD="$DRIVER_CMD -I$ROOT/include"

# --- 2. 假 SPI 控制器 ---------------------------------------------------------
STAGE2=$WORK/stage2
mkdir -p "$STAGE2"
cp "$ROOT/harness/fake_spi_flash.c" "$ROOT/harness/Kbuild" "$STAGE2/"

echo "==> 编译假 SPI 控制器并抓取命令行（V=1）"
LOG2=$WORK/fake.log
if ! make -C "$KERNEL_DIR" M="$STAGE2" ARCH="$ARCH" \
	CROSS_COMPILE="$CROSS_COMPILE" V=1 modules >"$LOG2" 2>&1; then
	echo "编译失败，日志尾部：" >&2
	tail -n 40 "$LOG2" >&2
	exit 1
fi
FAKE_CMD=$(grab_cmd "$LOG2" 'fake_spi_flash\.c' "$STAGE2" "$ROOT/harness")
FAKE_CMD="$FAKE_CMD -I$ROOT/include"

# --- 3. C++ 命令行工具（参数简单，手写） --------------------------------------
CLI_CMD="g++ -O2 -Wall -Wextra -std=c++17 -I$ROOT/include -c -o /dev/null"
MAIN_CMD="$CLI_CMD $ROOT/apps/w25q64_cli/main.cpp"
FLASH_CMD="$CLI_CMD $ROOT/apps/w25q64_cli/spi_flash.cpp"

# 用 python3 写 JSON：命令里可能有引号、反斜杠，手工拼容易出错
python3 - "$ROOT" "$DRIVER_CMD" "$FAKE_CMD" "$MAIN_CMD" "$FLASH_CMD" <<'PY'
import json
import sys

root, driver_cmd, fake_cmd, main_cmd, flash_cmd = sys.argv[1:6]

entries = [
    {
        "directory": "/build/sdk/kernel",
        "file": f"{root}/drivers/w25q64/w25q64.c",
        "command": driver_cmd,
    },
    {
        "directory": "/build/sdk/kernel",
        "file": f"{root}/harness/fake_spi_flash.c",
        "command": fake_cmd,
    },
    {
        "directory": f"{root}/apps/w25q64_cli",
        "file": f"{root}/apps/w25q64_cli/main.cpp",
        "command": main_cmd,
    },
    {
        "directory": f"{root}/apps/w25q64_cli",
        "file": f"{root}/apps/w25q64_cli/spi_flash.cpp",
        "command": flash_cmd,
    },
]

with open(f"{root}/compile_commands.json", "w", encoding="utf-8") as f:
    json.dump(entries, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY

echo "==> 已生成 $ROOT/compile_commands.json"
