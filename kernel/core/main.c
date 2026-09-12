/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "zaeboot.h"
#include "drivers/fb.h"
#include "drivers/serial.h"
#include "printf.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/smp.h"
#include "proc/syscall.h"
#include "proc/elf.h"
#include "fs/vfs.h"
#include "drivers/pci.h"
#include "drivers/nvme.h"
#include "module.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/timer.h"
#include "drivers/keyboard.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/apic.h"
#include "proc/sched.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "string.h"

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

static void halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

#define CHECK(cond, ...) do { if (!(cond)) { kprintf("  FAIL: " __VA_ARGS__); kprintf("\n"); fails++; } } while (0)

static int fails;

static void test_pmm(void)
{
    kprintf("pmm: %lu MiB total, %lu KiB used\n",
            pmm_total_pages() * 4 / 1024, pmm_used_pages() * 4);

    uint64_t before = pmm_used_pages();
    uint64_t a = pmm_alloc_page(), b = pmm_alloc_page(), run = pmm_alloc_pages(8);
    CHECK(a && b && run, "pmm allocation returned 0");
    CHECK(a != b, "pmm returned the same page twice");
    CHECK((run & 0xFFF) == 0, "pmm run not page aligned");
    pmm_free_page(a);
    pmm_free_page(b);
    pmm_free_pages(run, 8);
    CHECK(pmm_used_pages() == before, "pmm leaked pages (%lu -> %lu)", before, pmm_used_pages());
    kprintf("pmm: alloc/free ok\n");
}

static void test_vmm(void)
{
    uint64_t virt = 0xFFFFD00000000000UL;
    uint64_t phys = pmm_alloc_page();
    CHECK(vmm_map_page(virt, phys, PTE_WRITE | PTE_NX) == 0, "vmm_map_page failed");
    CHECK(vmm_translate(virt) == phys, "vmm_translate mismatch");

    volatile uint64_t *p = (volatile uint64_t *)virt;
    *p = 0xDEADBEEFCAFEBABEUL;
    CHECK(*(volatile uint64_t *)P2V(phys) == 0xDEADBEEFCAFEBABEUL, "write via new mapping not visible via HHDM");

    CHECK(vmm_unmap_page(virt) == phys, "vmm_unmap_page returned wrong phys");
    CHECK(vmm_translate(virt) == 0, "page still mapped after unmap");
    pmm_free_page(phys);

    CHECK(vmm_translate(0x100000) == 0x100000, "identity map broken");
    CHECK(vmm_translate(HHDM_BASE + 0x100000) == 0x100000, "HHDM broken");
    kprintf("vmm: map/unmap/translate ok\n");
}

static void test_heap_worker(void *arg)
{
    (void)arg;
    for (int round = 0; round < 50; round++) {
        void *p[32];
        for (int i = 0; i < 32; i++) {
            p[i] = kmalloc(16 << (i % 8));
            memset(p[i], round, 16);
        }
        void *big = kmalloc(64 * 1024);
        for (int i = 0; i < 32; i++)
            kfree(p[i]);
        kfree(big);
    }
}

static void test_heap(void)
{
    uint64_t pmm_before = pmm_used_pages();
    struct heap_stats base;
    heap_get_stats(&base);              /* the scheduler already owns a few objects */

    /* small objects across classes, with pattern verification */
    void *ptrs[64];
    for (int i = 0; i < 64; i++) {
        size_t sz = 8 << (i % 8);           /* 8..1024 */
        ptrs[i] = kmalloc(sz);
        CHECK(ptrs[i] != NULL, "kmalloc(%lu) returned NULL", sz);
        CHECK(((uint64_t)ptrs[i] & 15) == 0, "kmalloc(%lu) not 16-byte aligned", sz);
        memset(ptrs[i], i, sz);
    }
    for (int i = 0; i < 64; i++) {
        size_t sz = 8 << (i % 8);
        uint8_t *p = ptrs[i];
        int ok = 1;
        for (size_t k = 0; k < sz; k++)
            ok &= p[k] == (uint8_t)i;
        CHECK(ok, "object %d corrupted", i);
    }
    for (int i = 0; i < 64; i += 2)
        kfree(ptrs[i]);

    /* freed slots get reused before a new slab is created */
    struct heap_stats s1, s2;
    heap_get_stats(&s1);
    void *again = kmalloc(8);
    heap_get_stats(&s2);
    CHECK(s2.slabs == s1.slabs, "kmalloc created a new slab despite free objects");
    kfree(again);
    for (int i = 1; i < 64; i += 2)
        kfree(ptrs[i]);

    /* many allocations of one class -> multiple slabs */
    void **many = kmalloc(2000 * sizeof(void *));
    for (int i = 0; i < 2000; i++) {
        many[i] = kmalloc(64);
        *(int *)many[i] = i;
    }
    int ok = 1;
    for (int i = 0; i < 2000; i++)
        ok &= *(int *)many[i] == i;
    CHECK(ok, "64-byte objects corrupted");
    for (int i = 0; i < 2000; i++)
        kfree(many[i]);
    kfree(many);

    /* large allocations + realloc */
    char *big = kmalloc(100 * 1024);
    CHECK(big != NULL, "large kmalloc failed");
    memset(big, 0x5A, 100 * 1024);
    big = krealloc(big, 300 * 1024);
    CHECK(big != NULL && big[100 * 1024 - 1] == 0x5A, "krealloc lost data");
    char *small = krealloc(big, 100);            /* large -> slab */
    CHECK(small != NULL && small[99] == 0x5A, "krealloc large->small lost data");
    kfree(small);

    heap_dump();
    heap_get_stats(&s2);
    CHECK(s2.slab_objects == base.slab_objects && s2.large_allocs == base.large_allocs &&
          s2.bytes_requested == base.bytes_requested,
          "heap leak: %lu objects, %lu large, %lu bytes",
          s2.slab_objects - base.slab_objects, s2.large_allocs - base.large_allocs,
          s2.bytes_requested - base.bytes_requested);
    kprintf("heap: pmm pages in use by heap after tests: %lu (spare slabs kept)\n",
            pmm_used_pages() - pmm_before);
}

static void test_timer(void)
{
    uint64_t t0 = timer_ticks();
    task_sleep_ms(200);
    uint64_t dt = timer_ticks() - t0;
    CHECK(dt >= 200 * TIMER_HZ / 1000 && dt <= 220 * TIMER_HZ / 1000,
          "timer off: %lu ticks in 200 ms", dt);
    kprintf("timer: %lu ticks in 200 ms @ %u Hz (%s)\n", dt, TIMER_HZ, apic_enabled() ? "lapic" : "pit");
}

/* Workers: one spins (proves preemption), the others sleep-loop. */
static volatile uint64_t spin_counter, spin_done;
static volatile int order[8], order_n;
static volatile uint32_t cpus_seen;

static void spinner(void *arg)
{
    (void)arg;
    while (!spin_done) {
        spin_counter++;
        __atomic_fetch_or(&cpus_seen, 1u << this_cpu()->index, __ATOMIC_RELAXED);
    }
}

static void sleeper(void *arg)
{
    int id = (int)(uint64_t)arg;
    for (int i = 0; i < 3; i++)
        task_sleep_ms(30 * id);
    order[order_n++] = id;
}

static void test_sched(void)
{
    struct task *sp = task_create("spinner", spinner, NULL);
    uint32_t sleepers[3];
    for (int i = 0; i < 3; i++) {
        char name[16] = "sleeper0";
        name[7] = '1' + (char)i;
        sleepers[i] = task_create(name, sleeper, (void *)(uint64_t)(3 - i))->id;   /* 3,2,1 */
    }

    for (int i = 0; i < 3; i++)
        task_join(sleepers[i]);
    CHECK(order_n == 3, "not all sleepers finished (%d)", order_n);
    CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3,
          "sleep ordering wrong: %d %d %d", order[0], order[1], order[2]);
    CHECK(spin_counter > 0, "spinner never ran while main was sleeping");
    CHECK(task_current()->id == 0, "main is not task 0?!");

    uint64_t c1 = spin_counter;
    task_yield();
    task_sleep_ms(20);
    CHECK(spin_counter > c1, "spinner starved while main slept");

    spin_done = 1;
    task_join(sp->id);
    task_sleep_ms(5);                       /* let idle reap the zombie */
    CHECK(!task_alive(sp->id), "spinner still alive after join");
    /* A lone spinner may legitimately stay on one CPU; test_smp checks spreading. */
    kprintf("sched: preemption/sleep/join ok, spinner ran on cpu mask %x\n", cpus_seen);
}

/* SMP: several spinners at once must spread across CPUs. */
static void test_smp(void)
{
    if (smp_cpu_count() < 2) {
        kprintf("smp: only one cpu, skipping\n");
        return;
    }
    cpus_seen = 0;
    spin_done = 0;
    struct task *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = task_create("smp-spin", spinner, NULL);
    task_sleep_ms(100);
    spin_done = 1;
    for (int i = 0; i < 4; i++)
        task_join(t[i]->id);
    CHECK(cpus_seen == (1u << smp_cpu_count()) - 1, "not all cpus ran spinners: mask %x", cpus_seen);

    /* Heap traffic from two CPUs at once, plus TLB shootdowns from frees. */
    struct task *a = task_create("heap-a", (task_entry_t)test_heap_worker, NULL);
    struct task *b = task_create("heap-b", (task_entry_t)test_heap_worker, NULL);
    task_join(a->id);
    task_join(b->id);
    kprintf("smp: %u cpus exercised, cpu mask %x\n", smp_cpu_count(), cpus_seen);
}

static void test_vfs(void)
{
    struct file *f = vfs_open(vfs_root(), "/tmp/kernel.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(f != NULL, "vfs_open O_CREAT failed");
    if (!f)
        return;
    CHECK(file_write(f, "abc", 3) == 3, "file_write");
    CHECK(file_write(f, "defg", 4) == 4, "file_write append");
    file_close(f);

    char buf[16] = {0};
    f = vfs_open(vfs_root(), "tmp/kernel.txt", O_RDONLY);
    CHECK(f && file_read(f, buf, sizeof(buf)) == 7 && memcmp(buf, "abcdefg", 7) == 0, "file_read");
    file_close(f);

    struct vnode *sh = vfs_lookup(vfs_root(), "/bin/sh");
    CHECK(sh && sh->type == VNODE_FILE && sh->size > 0, "initrd /bin/sh missing");
    CHECK(vfs_lookup(vfs_root(), "/bin/../etc/./motd") != NULL, "dot/dotdot resolution");
    CHECK(vfs_unlink(vfs_root(), "/tmp/kernel.txt") == 0, "vfs_unlink");
    kprintf("vfs: create/write/read/lookup/unlink ok\n");
}

static void test_user(void)
{
    if (!vfs_lookup(vfs_root(), "/bin/test") || !vfs_lookup(vfs_root(), "/bin/crash")) {
        kprintf("user: /bin/test or /bin/crash not in initrd, skipping\n");
        return;
    }
    struct task *crash = process_spawn("/bin/crash", NULL, NULL);
    struct task *test  = process_spawn("/bin/test", NULL, NULL);
    CHECK(crash && test, "process_spawn failed");
    if (!crash || !test)
        return;
    /* We're their parent: collect them like waitpid would. */
    int code = -1, crash_code = 0;
    while (task_collect_child(crash->id, &crash_code) == 0)
        task_sleep_ms(5);
    while (task_collect_child(test->id, &code) == 0)
        task_sleep_ms(5);
    CHECK(code == 0, "userspace test program reported %d failure(s)", code);
    task_sleep_ms(5);
    sched_dump();
    kprintf("user: fork/exec/wait + fs from ring 3 ok, crash was killed\n");
}

void kernel_main(struct zaeboot_info *info)
{
    serial_init();

    if (info == NULL || info->magic != ZAEBOOT_MAGIC) {
        serial_puts("sic: bad boot info magic, halting\n");
        halt_forever();
    }

    fb_init(&info->fb);
    fb_set_color(0xE0E0E0, 0x101018);
    fb_clear();

    cpus[0].index = 0;
    cpu_count = 1;
    cpu_init(&cpus[0], 0);
    cpu_enable_fpu();
    idt_init();
    syscall_init_cpu();

    kprintf("sic kernel booting (zaeboot protocol v%u)\n", info->version);
    kprintf("framebuffer %ux%u @ %p, rsdp %p\n\n",
            info->fb.width, info->fb.height, (void *)info->fb.base, (void *)info->rsdp);

    kprintf("memory map (%lu entries):\n", info->mmap_count);
    const struct zaeboot_mmap_entry *mm = (const void *)info->mmap;
    for (uint64_t i = 0; i < info->mmap_count; i++)
        kprintf("  %016lx - %016lx  %s\n",
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
    if (have_initrd) {
        vfs_load_tar(P2V(info->initrd_addr), info->initrd_size);
        pmm_free_pages(PAGE_ALIGN_DOWN(info->initrd_addr),
                       (PAGE_ALIGN_UP(info->initrd_addr + info->initrd_size) -
                        PAGE_ALIGN_DOWN(info->initrd_addr)) / PAGE_SIZE);
    } else {
        kprintf("vfs: no initrd, /bin will be empty\n");
    }
    vfs_create(vfs_root(), "/dev", VNODE_DIR);
    vfs_create(vfs_root(), "/tmp", VNODE_DIR);
    vfs_create(vfs_root(), "/disk", VNODE_DIR);     /* where userland mounts the zaefs disk */
    vfs_mkdev("/dev/console", &console_ops, NULL);
    pci_init();
    module_init_ksyms();

    /* Interrupts: PIC remapped and masked, then APICs from the MADT if we
     * have them, then the timer (LAPIC or PIT) and the keyboard. */
    pic_init();
    if (acpi_init(info->rsdp) == 0 && apic_init() == 0)
        irq_use_apic();
    timer_init();
    keyboard_init();
    sched_init();
    interrupts_enable();
    smp_init();
    nvme_init();

    kprintf("\nself tests:\n");
    test_pmm();
    test_vmm();
    test_heap();

    test_timer();
    test_sched();
    test_smp();
    test_vfs();
    test_user();

    if (fails)
        kprintf("\n%d self test(s) FAILED\n", fails);
    else
        kprintf("\nall self tests passed\n");

    kprintf("\nstarting /bin/init\n");
    if (!process_spawn("/bin/init", NULL, NULL))
        kprintf("could not start init\n");
    for (;;)
        __asm__ volatile("sti; hlt");
}
