#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# gen_compile_commands.sh - 为 clangd 生成 compile_commands.json
#
# 为什么需要它：
#   clangd 要靠"编译数据库"才知道一个文件到底是用什么参数编译的
#   （-I 内核头文件路径、-D__KERNEL__、-mgeneral-regs-only ... 一大堆）。
#   没有数据库时，clangd 把 led8.c 当普通 C 文件解析，
#   #include <linux/fs.h> 全部变成红色，跳转、补全自然也就没了。
#
# 做法：
#   1. 把驱动源码拷进临时目录，用 V=1 编一次模块，拿到内核真正用的那条 gcc 命令；
#   2. 把命令里的临时路径改回工程里的真实路径；
#   3. 再补一条 CLI 的编译命令，一起写进 compile_commands.json。
#
# 用法：
#   ./scripts/gen_compile_commands.sh      # 或 make compdb
#
# 生成的文件被 .gitignore 忽略（里面有绝对路径），换机器/换 SDK 路径后重跑即可。

set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

SDK=${SDK:-/build/sdk}
KERNEL_DIR=${KERNEL_DIR:-$SDK/kernel}
ARCH=${ARCH:-arm64}
CROSS_COMPILE=${CROSS_COMPILE:-$SDK/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/led8-cdb-XXXXXX")
trap 'rm -rf "$STAGE"' EXIT

# ---- 1. 在临时目录里编一次，顺便拿到真实编译命令 ----
cp "$WORK/drivers/led8/led8.c" "$WORK/drivers/led8/Kbuild" \
   "$WORK/include/led8_uapi.h" "$STAGE/"

LOG=$STAGE/build.log
if ! make -C "$KERNEL_DIR" M="$STAGE" ARCH="$ARCH" \
	CROSS_COMPILE="$CROSS_COMPILE" V=1 modules > "$LOG" 2>&1; then
	echo "module build failed, see $LOG" >&2
	tail -20 "$LOG" >&2
	exit 1
fi

# 从 V=1 的输出里挑出编译 led8.o 的那一行（含 -c）
LINE=$(grep -F "$STAGE/led8.c" "$LOG" | grep -F ' -c ' | head -1 || true)
if [ -z "$LINE" ]; then
	echo "cannot find compile command for led8.c in $LOG" >&2
	exit 1
fi

# ---- 2/3. 改写路径并生成 JSON（用 python3 保证转义正确） ----
python3 - "$LINE" "$STAGE" "$WORK" "$KERNEL_DIR" <<'PY'
import json
import os
import sys

line, stage, work, kdir = sys.argv[1:5]

cmd = " ".join(line.split())

# 依赖文件路径里带临时目录，clangd 用不到，直接去掉
cmd = cmd.replace(f"-Wp,-MMD,{stage}/.led8.o.d", "")
# 输出文件也换成不落盘的位置
cmd = cmd.replace(f"-o {stage}/led8.o", "-o /dev/null")
# 输入文件改回工程里的真实路径（临时目录马上就被删了）
cmd = cmd.replace(f"{stage}/led8.c", f"{work}/drivers/led8/led8.c")
# 公共头文件在 include/ 下，补一个 -I
cmd = cmd + f" -I{work}/include"
cmd = " ".join(cmd.split())

entries = [
    {
        "directory": kdir,
        "file": f"{work}/drivers/led8/led8.c",
        "command": cmd,
    },
    {
        "directory": work,
        "file": f"{work}/apps/led8_cli/led8_cli.c",
        "command": (
            "/usr/bin/gcc -O2 -Wall -Wextra "
            f"-I{work}/include -c -o /dev/null "
            f"{work}/apps/led8_cli/led8_cli.c"
        ),
    },
]

out = os.path.join(work, "compile_commands.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(entries, f, indent=1)
    f.write("\n")
print(f"wrote {out}")
PY

echo "clangd: reopen/reload the file (or restart the language server) to pick it up"
