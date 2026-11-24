# SPDX-License-Identifier: GPL-2.0
#
# Unified build for the GPIO3_A1 project (TaishanPi RK3566).
#
# Final artifacts are written to $(OUT) (default: ./out):
#   out/gpio3a1.ko                 kernel module
#   out/gpio3a1.dtbo               device tree overlay
#   out/gpio3a1_cli                userspace CLI
#   out/tspi-rk3566-gpio3a1.dtb    full board DTB (only with "make dtb")
#
# Kbuild, cpp and dtc intermediates live in a throw-away directory under
# $(BUILD_DIR) (default: ./build) and are removed when the target finishes,
# so only the final artifacts remain in out/.

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

DRIVER_DIR := $(CURDIR)/drivers/gpio3a1
DTS_DIR := $(CURDIR)/dts
APP_DIR := $(CURDIR)/apps/gpio3a1_cli

KO := $(OUT)/gpio3a1.ko
DTBO := $(OUT)/gpio3a1.dtbo
CLI := $(OUT)/gpio3a1_cli
DTB := $(OUT)/tspi-rk3566-gpio3a1.dtb

DTS_CPPFLAGS := -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I$(KERNEL_DIR)/include \
	-I$(KERNEL_DIR)/arch/arm64/boot/dts \
	-I$(KERNEL_DIR)/arch/arm64/boot/dts/rockchip

.PHONY: all dtb verify clean help

all: $(KO) $(DTBO) $(CLI)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

dtb: $(DTB)
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

# --- kernel module -----------------------------------------------------------
# Kbuild cannot separate its output from its source directory, so the source
# and Kbuild fragment are copied into a temporary staging directory, built
# there, and the resulting .ko is moved to out/.
$(KO): $(DRIVER_DIR)/gpio3a1.c $(DRIVER_DIR)/Kbuild
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/drv.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	cp "$(DRIVER_DIR)/gpio3a1.c" "$(DRIVER_DIR)/Kbuild" "$$stage/"; \
	$(MAKE) -C "$(KERNEL_DIR)" M="$$stage" ARCH="$(ARCH)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" modules; \
	cp "$$stage/gpio3a1.ko" "$@"
	@echo "  KO    $@"

# --- device tree overlay -----------------------------------------------------
$(DTBO): $(DTS_DIR)/gpio3a1-overlay.dts
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/dtbo.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(DTS_CPP) $(DTS_CPPFLAGS) -o "$$stage/gpio3a1-overlay.dts" "$<"; \
	$(DTC) -@ -I dts -O dtb -o "$$stage/gpio3a1.dtbo" \
		"$$stage/gpio3a1-overlay.dts"; \
	cp "$$stage/gpio3a1.dtbo" "$@"
	@echo "  DTBO  $@"

# --- full board DTB (backup variant) ----------------------------------------
$(DTB): $(DTS_DIR)/tspi-rk3566-gpio3a1.dts
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/dtb.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(DTS_CPP) $(DTS_CPPFLAGS) -o "$$stage/tspi-rk3566-gpio3a1.dts" "$<"; \
	$(DTC) -@ -I dts -O dtb -o "$$stage/tspi-rk3566-gpio3a1.dtb" \
		"$$stage/tspi-rk3566-gpio3a1.dts"; \
	cp "$$stage/tspi-rk3566-gpio3a1.dtb" "$@"
	@echo "  DTB   $@"

# --- userspace CLI -----------------------------------------------------------
$(CLI): $(APP_DIR)/gpio3a1_cli.c
	@mkdir -p "$(OUT)" "$(BUILD_DIR)"
	@set -eu; \
	stage=$$(mktemp -d "$(BUILD_DIR)/app.XXXXXX"); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(CC) -O2 -Wall -Wextra -static -s -o "$$stage/gpio3a1_cli" "$<"; \
	cp "$$stage/gpio3a1_cli" "$@"
	@echo "  CLI   $@"

# --- verification / housekeeping --------------------------------------------
verify: $(DTBO)
	@BASE_DTB="$(BASE_DTB)" DTBO="$(DTBO)" $(CURDIR)/scripts/verify_dtbo.sh
	@rmdir "$(BUILD_DIR)" 2>/dev/null || true

clean:
	rm -rf -- "$(OUT)" "$(BUILD_DIR)"

help:
	@echo "Usage: make [target] [VAR=value]"
	@echo
	@echo "Targets:"
	@echo "  all      build out/gpio3a1.ko, out/gpio3a1.dtbo, out/gpio3a1_cli (default)"
	@echo "  dtb      build out/tspi-rk3566-gpio3a1.dtb (optional full board DTB)"
	@echo "  verify   apply out/gpio3a1.dtbo to the SDK base DTB with fdtoverlay"
	@echo "  clean    remove out/ and build/"
	@echo
	@echo "Variables: SDK, KERNEL_DIR, ARCH, CROSS_COMPILE, OUT, BUILD_DIR, BASE_DTB"
