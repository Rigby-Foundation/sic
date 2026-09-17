# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# x86_64: clang + ld.lld, kernel at 1 MiB, no red zone / FPU in the kernel.
ARCH_CFLAGS  := --target=x86_64-elf -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small
ARCH_ASFLAGS := --target=x86_64-elf
ARCH_LDFLAGS := -m elf_x86_64
ARCH_SERIAL_SRC := kernel/drivers/serial.c      # COM1
