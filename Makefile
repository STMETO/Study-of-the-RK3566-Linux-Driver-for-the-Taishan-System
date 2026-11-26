# SPDX-License-Identifier: GPL-2.0
#
# w25q64 工程统一构建入口（泰山派 RK3566）
#
# 最终产物统一写到 $(OUT)（默认 ./out）：
#   out/w25q64.ko                 内核模块
#   out/w25q64.dtbo               设备树 overlay
#   out/w25q64_cli                用户态命令行工具（C++17）
#   out/fake-spi-flash.ko         假 SPI 控制器（make fake，无硬件测试用）
#   out/tspi-rk3566-w25q64.dtb    整颗板级 DTB（只在 make dtb 时生成）
#
# Kbuild / cpp / dtc / g++ 的中间文件都放在 $(BUILD_DIR) 下的临时目录里，
# 每个目标结束时自动删除，所以源码目录不会留下 .o、.cmd、.mod.c、.dtbo。

SDK ?= /build/sdk
KERNEL_DIR ?= $(SDK)/kernel
ARCH ?= arm64
CROSS_COMPILE ?= $(SDK)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

CXX := $(CROSS_COMPILE)g++
DTS_CPP ?= gcc
DTC ?= dtc
FDTOVERLAY ?= fdtoverlay

BASE_DTB ?= $(KERNEL_DIR)/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dtb

OUT ?= $(CURDIR)/out
BUILD_DIR ?= $(CURDIR)/build

DRIVER_DIR := $(CURDIR)/drivers/w25q64
FAKE_DIR := $(CURDIR)/harness
INCLUDE_DIR := $(CURDIR)/include
DTS_DIR := $(CURDIR)/dts
APP_DIR := $(CURDIR)/apps/w25q64_cli

KO := $(OUT)/w25q64.ko
DTBO := $(OUT)/w25q64.dtbo
CLI := $(OUT)/w25q64_cli
FAKE := $(OUT)/fake-spi-flash.ko
DTB := $(OUT)/tspi-rk3566-w25q64.dtb

APP_SRCS := $(APP_DIR)/main.cpp $(APP_DIR)/spi_flash.cpp
APP_HDRS := $(APP_DIR)/spi_flash.hpp $(INCLUDE_DIR)/w25q64_uapi.h

# 设备树预处理：overlay 里会 #include <dt-bindings/...>，所以要带上内核头文件路径
DTS_CPPFLAGS := -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I$(KERNEL_DIR)/include \
	-I$(KERNEL_DIR)/arch/arm64/boot/dts \
	-I$(KERNEL_DIR)/arch/arm64/boot/dts/rockchip

.PHONY: all dtb fake compdb verify clean help

all: $(KO) $(DTBO) $(CLI)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

dtb: $(DTB)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

fake: $(FAKE)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

# --- 内核模块 -----------------------------------------------------------------
# Kbuild 无法把输出目录和源码目录分开，所以把 w25q64.c、Kbuild 片段和
# 公共头文件一起拷进临时 staging 目录，在那里编译，最后只把 .ko 搬到 out/。
$(KO): $(DRIVER_DIR)/w25q64.c $(DRIVER_DIR)/Kbuild $(INCLUDE_DIR)/w25q64_uapi.h
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/drv.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	cp "$(DRIVER_DIR)/w25q64.c" "$(DRIVER_DIR)/Kbuild" "$(INCLUDE_DIR)/w25q64_uapi.h" "$$stage/"; \
	$(MAKE) -C "$(KERNEL_DIR)" M="$$stage" ARCH="$(ARCH)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" modules; \
	cp "$$stage/w25q64.ko" "$@"
	@echo "  KO    $@"

# --- 假 SPI 控制器（没有真实 flash 时的测试通道） -----------------------------
# 这是一个**独立的模块**：它实现一个假的 spi_controller，背后是一个内存数组，
# 能应答 W25Q 的 JEDEC ID / 读 / 页编程 / 扇区擦除 / 状态寄存器。
# 加载后 SPI 核心里会多出一条总线和一颗 "w25q64" 设备，我们的驱动照样 probe，
# 不需要真实芯片也能把 read/write/ioctl 整条链路跑通。
$(FAKE): $(FAKE_DIR)/fake_spi_flash.c $(FAKE_DIR)/Kbuild
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/fake.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	cp "$(FAKE_DIR)/fake_spi_flash.c" "$(FAKE_DIR)/Kbuild" "$$stage/"; \
	$(MAKE) -C "$(KERNEL_DIR)" M="$$stage" ARCH="$(ARCH)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" modules; \
	cp "$$stage/fake-spi-flash.ko" "$@"
	@echo "  FAKE  $@"

# --- 设备树 overlay -----------------------------------------------------------
$(DTBO): $(DTS_DIR)/w25q64-overlay.dts
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/dtbo.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(DTS_CPP) $(DTS_CPPFLAGS) -o "$$stage/w25q64-overlay.dts" "$<"; \
	$(DTC) -@ -I dts -O dtb -o "$$stage/w25q64.dtbo" "$$stage/w25q64-overlay.dts"; \
	cp "$$stage/w25q64.dtbo" "$@"
	@echo "  DTBO  $@"

# --- 整颗板级 DTB（备用方案，覆盖 boot.img 里的 dtb 时用） --------------------
$(DTB): $(DTS_DIR)/tspi-rk3566-w25q64.dts
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/dtb.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(DTS_CPP) $(DTS_CPPFLAGS) -o "$$stage/tspi-rk3566-w25q64.dts" "$<"; \
	$(DTC) -@ -I dts -O dtb -o "$$stage/tspi-rk3566-w25q64.dtb" "$$stage/tspi-rk3566-w25q64.dts"; \
	cp "$$stage/tspi-rk3566-w25q64.dtb" "$@"
	@echo "  DTB   $@"

# --- 用户态命令行工具（C++17） ------------------------------------------------
# -I$(INCLUDE_DIR)：让 CLI 和驱动共用同一份 ioctl 定义（include/w25q64_uapi.h）
# -static：板子上不一定带 libstdc++.so，静态链接最省事（约 2MB）
$(CLI): $(APP_SRCS) $(APP_HDRS)
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/app.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(CXX) -O2 -Wall -Wextra -std=c++17 -static -s -I"$(INCLUDE_DIR)" \
		-o "$$stage/w25q64_cli" $(APP_SRCS); \
	cp "$$stage/w25q64_cli" "$@"
	@echo "  CLI   $@"

# --- clangd 编译数据库 ---------------------------------------------------------
# 生成 compile_commands.json：VSCode + clangd 靠它拿到内核真实的编译参数
# 和 C++ 的 -std=c++17，才能解析 <linux/...>、std:: 并支持跳转和补全。
compdb:
	@$(CURDIR)/scripts/gen_compile_commands.sh
	@echo "  CDB   $(CURDIR)/compile_commands.json"

# --- 验证 / 清理 --------------------------------------------------------------
# 无需板子：把 out/w25q64.dtbo 用 fdtoverlay 合并进 SDK 编出来的基准 dtb，
# 再反编译检查 flash@0 是否挂到 spi3 底下、spi_test@0 是否已经让出 CS0。
verify: $(DTBO)
	@BASE_DTB="$(BASE_DTB)" DTBO="$(DTBO)" $(CURDIR)/scripts/verify_dtbo.sh
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

clean:
	rm -rf -- "$(OUT)" "$(BUILD_DIR)"

help:
	@echo "Usage: make [target] [VAR=value]"
	@echo
	@echo "Targets:"
	@echo "  all      build out/w25q64.ko, out/w25q64.dtbo, out/w25q64_cli (default)"
	@echo "  fake     build out/fake-spi-flash.ko (fake SPI controller, no hardware needed)"
	@echo "  dtb      build out/tspi-rk3566-w25q64.dtb (optional full board DTB)"
	@echo "  verify   apply out/w25q64.dtbo to the SDK base DTB with fdtoverlay"
	@echo "  compdb   generate compile_commands.json for clangd"
	@echo "  clean    remove out/ and build/"
	@echo
	@echo "Variables: SDK, KERNEL_DIR, ARCH, CROSS_COMPILE, OUT, BUILD_DIR, BASE_DTB"
