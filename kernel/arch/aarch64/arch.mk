# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# aarch64: ARMv8-A, EL1, 4 KiB pages, no SIMD/FP in the kernel.
# QEMU virt (-M virt -cpu cortex-a72): `-kernel build/aarch64/sic.img`, a
# Linux-style Image (head.S carries the header) so QEMU also loads the
# initrd and passes the device tree in x0. The ELF is kept for symbols.
ARCH_CFLAGS  := --target=aarch64-elf -mgeneral-regs-only -fno-pic -fno-pie -mcmodel=small -mno-outline-atomics -mstrict-align
ARCH_ASFLAGS := --target=aarch64-elf
ARCH_LDFLAGS := -m aarch64elf
ARCH_SERIAL_SRC := kernel/arch/aarch64/pl011.c
ARCH_IMAGE := $(BUILD)/sic.img
ARCH_SMP_SRC := kernel/arch/aarch64/smp.c
