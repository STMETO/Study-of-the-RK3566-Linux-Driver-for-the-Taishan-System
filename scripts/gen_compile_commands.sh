#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# gen_compile_commands.sh - 生成 clangd 用的 compile_commands.json
#
# 为什么需要它：
#   clangd 不是"猜"代码怎么编译的，它需要一个编译数据库，里面记录每个源文件
#   真实的编译命令（-I 头文件路径、-D 宏定义、-include 强制包含等）。
#   内核模块的编译参数有几百个，手写不现实，所以这里玩了个小技巧：
#     1. 把源码拷进临时目录
#     2. 用 V=1 真正编译一次，把完整命令行打印到日志
#     3. 从日志里把 at24c02.c 那条 gcc 命令抠出来
#     4. 把临时路径替换成真实路径，写进 JSON
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

STAGE=$WORK/stage
mkdir -p "$STAGE"

# 和顶层 Makefile 里的 KO 目标做同样的准备：源码 + Kbuild + 公共头文件
cp "$ROOT/drivers/at24c02/at24c02.c" \
   "$ROOT/drivers/at24c02/Kbuild" \
   "$ROOT/include/at24c02_uapi.h" "$STAGE/"

LOG=$WORK/build.log

echo "==> 编译一次并抓取命令行（V=1）"
if ! make -C "$KERNEL_DIR" M="$STAGE" ARCH="$ARCH" \
	CROSS_COMPILE="$CROSS_COMPILE" V=1 modules >"$LOG" 2>&1; then
	echo "编译失败，日志尾部：" >&2
	tail -n 40 "$LOG" >&2
	exit 1
fi

# 日志里会有很多行，挑出真正编译 at24c02.c 的那一条
CMD_LINE=$(grep -E '[[:space:]]-c[[:space:]]' "$LOG" | grep 'at24c02\.c' | tail -n 1)
if [ -z "$CMD_LINE" ]; then
	echo "没能从编译日志里找到 at24c02.c 的编译命令" >&2
	exit 1
fi

# 清理命令开头可能存在的 make 前缀（"make[1]: ... gcc ..."之类）
CMD_LINE=${CMD_LINE#*: }

# 把临时 staging 路径换成真实源码目录，把输出目标改成 /dev/null，
# 再补上公共头文件目录 include/。
CMD=$(printf '%s' "$CMD_LINE" \
	| sed "s#$STAGE#$ROOT/drivers/at24c02#g" \
	| sed "s#-o $ROOT/drivers/at24c02/at24c02.o#-o /dev/null#" \
	| sed "s#-o $STAGE/at24c02.o#-o /dev/null#")

# 兜底：如果上面的 -o 替换没命中，就把任何 "-o xxx.o" 改成 "-o /dev/null"
CMD=$(printf '%s' "$CMD" | sed -E 's#-o [^ ]+\.o#-o /dev/null#')

CMD="$CMD -I$ROOT/include"

# CLI 那条记录：普通 gcc 编译，参数很少，手写即可
CLI_CMD="gcc -O2 -Wall -Wextra -I$ROOT/include -c -o /dev/null $ROOT/apps/at24c02_cli/at24c02_cli.c"

# 用 python3 写 JSON：命令里可能有引号、反斜杠，手工拼容易出错
python3 - "$ROOT" "$CMD" "$CLI_CMD" <<'PY'
import json
import sys

root, driver_cmd, cli_cmd = sys.argv[1], sys.argv[2], sys.argv[3]

entries = [
    {
        "directory": "/build/sdk/kernel",
        "file": f"{root}/drivers/at24c02/at24c02.c",
        "command": driver_cmd,
    },
    {
        "directory": f"{root}/apps/at24c02_cli",
        "file": f"{root}/apps/at24c02_cli/at24c02_cli.c",
        "command": cli_cmd,
    },
]

with open(f"{root}/compile_commands.json", "w", encoding="utf-8") as f:
    json.dump(entries, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY

echo "==> 已生成 $ROOT/compile_commands.json"
