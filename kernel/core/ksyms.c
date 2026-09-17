/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The kernel's exported API for loadable modules. Keep this list curated:
 * everything here is a stable-ish contract with out-of-tree code. */
#include "module.h"
#include "printf.h"
#include "mm/heap.h"
#include "string.h"
#include "fs/vfs.h"
#include "fs/blkdev.h"
#include "proc/sched.h"
#include "asm/timer.h"
#include "asm/irq.h"
#include "drivers/pci.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "proc/wait.h"
#include "proc/signal.h"

EXPORT_SYMBOL(kprintf);
EXPORT_SYMBOL(kputc);
EXPORT_SYMBOL(kputs);
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kzalloc);
EXPORT_SYMBOL(krealloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(vfs_root);
EXPORT_SYMBOL(vfs_lookup);
EXPORT_SYMBOL(vfs_create);
EXPORT_SYMBOL(vfs_unlink);
EXPORT_SYMBOL(vfs_mkdev);
EXPORT_SYMBOL(vfs_register_fs);
EXPORT_SYMBOL(vfs_mount);
EXPORT_SYMBOL(vnode_alloc);
EXPORT_SYMBOL(vnode_free);
EXPORT_SYMBOL(blkdev_register);
EXPORT_SYMBOL(blkdev_find);
EXPORT_SYMBOL(blkdev_from_vnode);
EXPORT_SYMBOL(task_create);
EXPORT_SYMBOL(task_current);
EXPORT_SYMBOL(task_sleep_ms);
EXPORT_SYMBOL(task_yield);
EXPORT_SYMBOL(task_exit);
EXPORT_SYMBOL(timer_ticks);
EXPORT_SYMBOL(timer_ms);
EXPORT_SYMBOL(irq_install);
EXPORT_SYMBOL(irq_mask);
EXPORT_SYMBOL(irq_unmask);
#ifdef CONFIG_PCI
EXPORT_SYMBOL(pci_find_class);
EXPORT_SYMBOL(pci_read32);
EXPORT_SYMBOL(pci_write32);
EXPORT_SYMBOL(pci_enable_busmaster);
#endif
#ifdef CONFIG_SIGNALS
EXPORT_SYMBOL(task_send_signal);
#endif
EXPORT_SYMBOL(waitqueue_wake_all);
EXPORT_SYMBOL(__wait_add);
EXPORT_SYMBOL(__wait_remove);
EXPORT_SYMBOL(task_block);
EXPORT_SYMBOL(task_wake);
EXPORT_SYMBOL(vmm_map_mmio);
EXPORT_SYMBOL(pmm_alloc_page);
EXPORT_SYMBOL(pmm_free_page);
EXPORT_SYMBOL(module_load);
EXPORT_SYMBOL(ksym_lookup);

#if BITS_PER_LONG == 32
/* What the compiler emits calls to on a 32-bit target (kernel/lib/int64.c). */
uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);
int64_t  __divdi3(int64_t n, int64_t d);
int64_t  __moddi3(int64_t n, int64_t d);
uint64_t __atomic_fetch_or_8(volatile void *p, uint64_t v, int order);
uint64_t __atomic_fetch_and_8(volatile void *p, uint64_t v, int order);
uint64_t __atomic_load_8(const volatile void *p, int order);
void     __atomic_store_8(volatile void *p, uint64_t v, int order);
EXPORT_SYMBOL(__udivdi3);
EXPORT_SYMBOL(__umoddi3);
EXPORT_SYMBOL(__divdi3);
EXPORT_SYMBOL(__moddi3);
EXPORT_SYMBOL(__atomic_fetch_or_8);
EXPORT_SYMBOL(__atomic_fetch_and_8);
EXPORT_SYMBOL(__atomic_load_8);
EXPORT_SYMBOL(__atomic_store_8);
#endif
