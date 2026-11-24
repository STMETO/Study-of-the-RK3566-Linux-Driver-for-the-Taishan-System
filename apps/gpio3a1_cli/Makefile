# SPDX-License-Identifier: GPL-2.0
# Cross-compile the GPIO3_A1 command line tool for the TaishanPi (aarch64).

SDK ?= /build/sdk
ARCH ?= arm64
CROSS_COMPILE ?= $(SDK)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

CC := $(CROSS_COMPILE)gcc
TARGET := gpio3a1_cli

all: $(TARGET)

$(TARGET): gpio3a1_cli.c
	$(CC) -O2 -Wall -Wextra -static -s -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
