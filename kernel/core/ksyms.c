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
#include "arch/x86_64/timer.h"
#include "arch/x86_64/idt.h"
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
