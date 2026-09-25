/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* What an architecture provides to the module loader (kernel/core/module.c):
 * memory the kernel can call into, and the relocation types of its object
 * format. Implemented in kernel/arch/<arch>/module.c. */
#pragma once
#include "types.h"

/* Bytes of scratch per relocation entry for out-of-range branches (PowerPC
 * puts a `lis/ori/mtctr/bctr` stub there; x86_64 needs none). */
#if defined(__powerpc__) || defined(__aarch64__)
#define MODULE_STUB_SIZE 16
#else
#define MODULE_STUB_SIZE 0
#endif

/* Page-granular memory that is executable and within reach of the kernel's
 * own relocations. Returns a kernel virtual address or NULL. */
void *arch_module_alloc(size_t pages);
void  arch_module_free(void *base, size_t pages);

/* Apply one relocation of `type` at P: S is the symbol's address, A the
 * addend. `stubs` points at the module's stub area, *stub_used counts the
 * bytes taken from it so far. Returns 0 or -ENOEXEC (having printed why). */
int arch_module_reloc(uint8_t *P, uint32_t type, unsigned long S, long A,
                      uint8_t *stubs, size_t *stub_used, const char *symname);

/* Code was written through the data side: make it fetchable. */
void arch_module_flush(void *base, size_t size);
