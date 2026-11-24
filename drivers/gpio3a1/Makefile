# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build for the GPIO3_A1 driver.
# The SDK kernel tree is only read; all artifacts stay in this module dir.

obj-m := gpio3a1.o

SDK ?= /build/sdk
KERNEL_DIR ?= $(SDK)/kernel
ARCH ?= arm64
CROSS_COMPILE ?= $(SDK)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

.PHONY: all clean modules

all: modules

modules:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
