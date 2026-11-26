# SPDX-License-Identifier: GPL-2.0
#
# at24c02 工程统一构建入口（泰山派 RK3566）
#
# 最终产物统一写到 $(OUT)（默认 ./out）：
#   out/at24c02.ko               内核模块
#   out/at24c02.dtbo             设备树 overlay
#   out/at24c02_cli              用户态命令行工具
#   out/i2c-stub.ko              假 I2C 总线（make stub，板端无硬件测试用）
#   out/tspi-rk3566-at24c02.dtb  整颗板级 DTB（只在 make dtb 时生成）
#
# Kbuild / cpp / dtc 的中间文件都放在 $(BUILD_DIR) 下的临时目录里，
# 每个目标结束时自动删除，所以源码目录不会留下 .o、.cmd、.mod.c、.dtbo。

SDK ?= /build/sdk
KERNEL_DIR ?= $(SDK)/kernel
ARCH ?= arm64
CROSS_COMPILE ?= $(SDK)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

CC := $(CROSS_COMPILE)gcc
DTS_CPP ?= gcc
DTC ?= dtc
FDTOVERLAY ?= fdtoverlay

BASE_DTB ?= $(KERNEL_DIR)/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dtb

OUT ?= $(CURDIR)/out
BUILD_DIR ?= $(CURDIR)/build

DRIVER_DIR := $(CURDIR)/drivers/at24c02
INCLUDE_DIR := $(CURDIR)/include
DTS_DIR := $(CURDIR)/dts
APP_DIR := $(CURDIR)/apps/at24c02_cli

KO := $(OUT)/at24c02.ko
DTBO := $(OUT)/at24c02.dtbo
CLI := $(OUT)/at24c02_cli
STUB := $(OUT)/i2c-stub.ko
DTB := $(OUT)/tspi-rk3566-at24c02.dtb

# 设备树预处理：overlay 里会 #include <dt-bindings/...>，所以要带上内核头文件路径
DTS_CPPFLAGS := -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I$(KERNEL_DIR)/include \
	-I$(KERNEL_DIR)/arch/arm64/boot/dts \
	-I$(KERNEL_DIR)/arch/arm64/boot/dts/rockchip

.PHONY: all dtb compdb stub verify clean help

all: $(KO) $(DTBO) $(CLI)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

dtb: $(DTB)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

# --- 内核模块 -----------------------------------------------------------------
# Kbuild 无法把输出目录和源码目录分开，所以把 at24c02.c、Kbuild 片段和
# 公共头文件一起拷进临时 staging 目录，在那里编译，最后只把 .ko 搬到 out/。
$(KO): $(DRIVER_DIR)/at24c02.c $(DRIVER_DIR)/Kbuild $(INCLUDE_DIR)/at24c02_uapi.h
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/drv.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	cp "$(DRIVER_DIR)/at24c02.c" "$(DRIVER_DIR)/Kbuild" "$(INCLUDE_DIR)/at24c02_uapi.h" "$$stage/"; \
	$(MAKE) -C "$(KERNEL_DIR)" M="$$stage" ARCH="$(ARCH)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" modules; \
	cp "$$stage/at24c02.ko" "$@"
	@echo "  KO    $@"

# --- i2c-stub：假的 I2C 总线（无硬件测试的关键） ------------------------------
# 内核源码里有 drivers/i2c/i2c-stub.c，但 SDK 的 .config 没开 CONFIG_I2C_STUB，
# 所以内核里没有 i2c-stub.ko。好在这个文件不依赖任何内核配置，
# 我们把它当"外部模块"单独编出来，板端 insmod 后就能凭空造出一条 I2C 总线，
# 再用 new_device 在 0x50 上挂一颗假的 EEPROM。
$(STUB): $(KERNEL_DIR)/drivers/i2c/i2c-stub.c
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/stub.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	cp "$(KERNEL_DIR)/drivers/i2c/i2c-stub.c" "$$stage/"; \
	printf 'obj-m := i2c-stub.o\n' > "$$stage/Kbuild"; \
	$(MAKE) -C "$(KERNEL_DIR)" M="$$stage" ARCH="$(ARCH)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" modules; \
	cp "$$stage/i2c-stub.ko" "$@"
	@echo "  STUB  $@"

# make stub：只编 i2c-stub.ko（第一次板端测试前跑一次）
stub: $(STUB)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

# --- 设备树 overlay -----------------------------------------------------------
$(DTBO): $(DTS_DIR)/at24c02-overlay.dts
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/dtbo.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(DTS_CPP) $(DTS_CPPFLAGS) -o "$$stage/at24c02-overlay.dts" "$<"; \
	$(DTC) -@ -I dts -O dtb -o "$$stage/at24c02.dtbo" "$$stage/at24c02-overlay.dts"; \
	cp "$$stage/at24c02.dtbo" "$@"
	@echo "  DTBO  $@"

# --- 整颗板级 DTB（备用方案，覆盖 boot.img 里的 dtb 时用） --------------------
$(DTB): $(DTS_DIR)/tspi-rk3566-at24c02.dts
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/dtb.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(DTS_CPP) $(DTS_CPPFLAGS) -o "$$stage/tspi-rk3566-at24c02.dts" "$<"; \
	$(DTC) -@ -I dts -O dtb -o "$$stage/tspi-rk3566-at24c02.dtb" "$$stage/tspi-rk3566-at24c02.dts"; \
	cp "$$stage/tspi-rk3566-at24c02.dtb" "$@"
	@echo "  DTB   $@"

# --- 用户态命令行工具 ---------------------------------------------------------
# -I$(INCLUDE_DIR)：让 CLI 和驱动共用同一份 ioctl 定义（include/at24c02_uapi.h）
$(CLI): $(APP_DIR)/at24c02_cli.c $(INCLUDE_DIR)/at24c02_uapi.h
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/app.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(CC) -O2 -Wall -Wextra -static -s -I"$(INCLUDE_DIR)" \
		-o "$$stage/at24c02_cli" "$<"; \
	cp "$$stage/at24c02_cli" "$@"
	@echo "  CLI   $@"

# --- clangd 编译数据库 ---------------------------------------------------------
# 生成 compile_commands.json：VSCode + clangd 靠它拿到内核真实的编译参数，
# 才能解析 <linux/...> 头文件、支持 Ctrl+点击跳转和补全。
# 文件里是绝对路径，已被 .gitignore 忽略；换机器或换 SDK 路径后重跑一次。
compdb:
	@$(CURDIR)/scripts/gen_compile_commands.sh
	@echo "  CDB   $(CURDIR)/compile_commands.json"

# --- 验证 / 清理 --------------------------------------------------------------
# 无需板子：把 out/at24c02.dtbo 用 fdtoverlay 合并进 SDK 编出来的基准 dtb，
# 再反编译检查 eeprom@50 是否真的挂到了 i2c2 底下。
verify: $(DTBO)
	@BASE_DTB="$(BASE_DTB)" DTBO="$(DTBO)" $(CURDIR)/scripts/verify_dtbo.sh
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

clean:
	rm -rf -- "$(OUT)" "$(BUILD_DIR)"

help:
	@echo "Usage: make [target] [VAR=value]"
	@echo
	@echo "Targets:"
	@echo "  all      build out/at24c02.ko, out/at24c02.dtbo, out/at24c02_cli (default)"
	@echo "  stub     build out/i2c-stub.ko (fake I2C bus for board test without hardware)"
	@echo "  dtb      build out/tspi-rk3566-at24c02.dtb (optional full board DTB)"
	@echo "  verify   apply out/at24c02.dtbo to the SDK base DTB with fdtoverlay"
	@echo "  compdb   generate compile_commands.json for clangd"
	@echo "  clean    remove out/ and build/"
	@echo
	@echo "Variables: SDK, KERNEL_DIR, ARCH, CROSS_COMPILE, OUT, BUILD_DIR, BASE_DTB"
