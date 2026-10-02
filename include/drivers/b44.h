/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
void b44_init(void);        /* probe PCI for Broadcom 440x NICs and register them */
void b44_dump(void);        /* the chip's DMA and interrupt state, to the console */
