/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
/* A UFS host controller at regs: its logical units become sda, sdb, ...
 * phone: one the bootloader left running is taken over as it is, and only
 * LUN 0 is writable. */
int  ufs_attach(volatile void *regs, int phone);
void ufs_init(void);        /* the ones on PCI */
