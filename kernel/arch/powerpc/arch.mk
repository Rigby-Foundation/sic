# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# powerpc: 32-bit big-endian, G4 (7450) class, soft-float in the kernel.
# QEMU mac99 / PowerMac: OpenFirmware loads the ELF and enters _start.
# 64-bit atomics (signal masks) are lock-based here (kernel/lib/int64.c); that is the plan, not a mistake.
ARCH_CFLAGS  := --target=powerpc-unknown-elf -mcpu=7450 -msoft-float -fno-pic -fno-pie -mno-altivec -Wno-atomic-alignment
ARCH_ASFLAGS := --target=powerpc-unknown-elf -mcpu=7450
ARCH_LDFLAGS := -m elf32ppc
ARCH_SERIAL_SRC := kernel/arch/powerpc/escc.c   # mac-io ESCC
