/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "zaeboot.h"
#include "drivers/fb.h"
#include "drivers/serial.h"
#include "printf.h"
#include "asm/arch.h"
#include "proc/syscall.h"
#include "proc/elf.h"
#include "core/prof.h"
#include "fs/vfs.h"
#include "mm/shm.h"
#include "drivers/pci.h"
#include "drivers/nvme.h"
#include "drivers/ahci.h"
#include "drivers/ide.h"
#include "drivers/sound.h"
#include "net/net.h"
#include "drivers/e1000.h"
#include "module.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/virtio_gpu.h"
#include "proc/sched.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "string.h"
#include "core/selftest.h"
#include "drivers/console.h"

static const char *mem_type_name(uint32_t type)
{
    switch (type) {
    case ZAEBOOT_MEM_USABLE:           return "usable";
    case ZAEBOOT_MEM_RESERVED:         return "reserved";
    case ZAEBOOT_MEM_ACPI_RECLAIMABLE: return "acpi reclaimable";
    case ZAEBOOT_MEM_ACPI_NVS:         return "acpi nvs";
    case ZAEBOOT_MEM_BOOTLOADER:       return "bootloader";
    case ZAEBOOT_MEM_BAD:              return "bad";
    case ZAEBOOT_MEM_BOOT_SERVICES:    return "boot services";
    default:                           return "unknown";
    }
}

/* /dev/initrd: the boot image, read-only, straight from the memory the loader put it in. */
static const uint8_t *initrd_data;
static size_t initrd_len;

static long initrd_read(struct file *f, void *buf, size_t len)
{
    if (f->pos >= initrd_len) return 0;
    if (len > initrd_len - f->pos) len = initrd_len - f->pos;
    memcpy(buf, initrd_data + f->pos, len);
    f->pos += len;
    return (long)len;
}

static const struct dev_ops initrd_ops = { .read = initrd_read };

static void initrd_expose(const void *data, size_t len)
{
    initrd_data = data;
    initrd_len = len;
    vfs_mkdev("/dev/initrd", &initrd_ops, NULL);
    struct vnode *n = vfs_lookup(vfs_root(), "/dev/initrd");
    if (n) { n->size = len; n->seekable = 1; }
}

void kernel_main(struct zaeboot_info *info)
{
#ifdef CONFIG_SERIAL
    serial_init();
#endif

    if (info == NULL || info->magic != ZAEBOOT_MAGIC) {
        kputs("sic: bad boot info magic, halting\n");
        arch_halt_forever();
    }

#ifdef CONFIG_FB_CONSOLE
    if (info->fb.base == 0 && info->size >= sizeof(*info) && info->firmware == ZAEBOOT_FW_BIOS)
        fb_init_text();             /* the loader left the BIOS text screen: keep using it */
    else
        fb_init(&info->fb, (void *)(uintptr_t)info->fb.base, 0);  /* identity / BAT mapped */
    fb_set_color(0xE0E0E0, 0x101018);
    fb_clear();
#endif

    arch_cpu_init_boot();

    kprintf("sic/%s booting (zaeboot protocol v%u)\n", ARCH_NAME, info->version);
    kprintf("framebuffer %ux%u @ %p, rsdp %p\n\n",
            info->fb.width, info->fb.height, (void *)info->fb.base, (void *)info->rsdp);

    kprintf("memory map (%llu entries):\n", info->mmap_count);
    const struct zaeboot_mmap_entry *mm = (const void *)info->mmap;
    for (uint64_t i = 0; i < info->mmap_count; i++)
        kprintf("  %016llx - %016llx  %s\n",
                mm[i].base, mm[i].base + mm[i].length, mem_type_name(mm[i].type));
    kprintf("\n");

    /* Memory: frames first, then our own page tables, then give back what
     * the firmware and loader were using, then the heap on top. */
    pmm_init(info);
    vmm_init();
    int have_initrd = info->size >= sizeof(*info) && info->initrd_addr && info->initrd_size;
    if (have_initrd)
        pmm_keep(info->initrd_addr, info->initrd_size);
    pmm_reclaim_boot_memory();
    heap_init();

    vfs_init();
    vfs_create(vfs_root(), "/dev", VNODE_DIR);
    vfs_create(vfs_root(), "/proc", VNODE_DIR);
    vfs_create(vfs_root(), "/tmp", VNODE_DIR);
    vfs_create(vfs_root(), "/mnt", VNODE_DIR);
    vfs_create(vfs_root(), "/disk", VNODE_DIR);     /* where init mounts the zaefs root partition */
    if (have_initrd) {
        vfs_load_tar(P2V(info->initrd_addr), info->initrd_size);
        /* Keep the image: the installer copies it to the target disk. */
        initrd_expose(P2V(info->initrd_addr), info->initrd_size);
    } else {
        kprintf("vfs: no initrd, /bin will be empty\n");
    }
    {
        const char *mode = info->size >= sizeof(*info) && info->firmware == ZAEBOOT_FW_BIOS ? "bios\n"
                         : info->size >= sizeof(*info) && info->firmware == ZAEBOOT_FW_UEFI ? "uefi\n" : "unknown\n";
        struct file *f = vfs_open(vfs_root(), "/proc/bootmode", O_WRONLY | O_CREAT);
        if (f) { file_write(f, mode, strlen(mode)); file_close(f); }
    }
    vfs_mkdev("/dev/console", &console_ops, NULL);
    shm_init();
#ifdef CONFIG_FB_CONSOLE
    fb_dev_init();
#endif
#ifdef CONFIG_PCI
    pci_init();
#endif
#ifdef CONFIG_MODULES
    module_init_ksyms();
#endif

    arch_init_interrupts(info);
#ifdef CONFIG_KEYBOARD
    keyboard_init();
#endif
#ifdef CONFIG_MOUSE
    mouse_init();
#endif
#ifdef CONFIG_VIRTIO_INPUT
    virtio_input_init();            /* keyboards and mice on the PCI bus */
#endif
    sched_init();
    prof_init();                    /* /proc/prof: where the CPUs spend their ticks */
#if defined(CONFIG_VIRTIO_GPU) && defined(CONFIG_FB_CONSOLE)
    virtio_gpu_init();              /* needs the scheduler (present thread) and the timer */
    fb_dev_init();                  /* /dev/fb0, if the display only appeared now (aarch64: no firmware framebuffer) */
#endif
    interrupts_enable();
    arch_init_smp();
#ifdef CONFIG_NVME
    nvme_init();
#endif
#ifdef CONFIG_AHCI
    ahci_init();
#endif
#ifdef CONFIG_IDE
    ide_init();
#endif
#ifdef CONFIG_HDA
    hda_init();
#endif
#ifdef CONFIG_NET
    net_init();
#ifdef CONFIG_E1000
    e1000_init();
#endif
#endif

#ifdef CONFIG_SELFTEST
    selftest_run();
#endif

    kprintf("\nstarting /bin/init\n");
    if (!process_spawn("/bin/init", NULL, NULL))
        kprintf("could not start init\n");
    /* The boot task is done. It must not stay runnable: as an ordinary task
     * it would be scheduled round-robin and hold the CPU for a full slice
     * doing nothing (that cost every wakeup on a busy CPU up to 10 ms). */
    for (;;)
        task_block();
}
