/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* ELF loading and the process lifecycle: spawn, exec, fork.
 *
 * The initial user stack follows the System V / musl convention:
 *   rsp -> argc, argv[0..], NULL, envp[0..], NULL, auxv pairs..., AT_NULL
 * with the strings, a copy of the program headers (AT_PHDR) and 16 random
 * bytes (AT_RANDOM) above them. */
#include "proc/elf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "drivers/keyboard.h"
#include "asm/timer.h"
#include "asm/cpu.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

/* The native ELF class: ELF64 on x86_64, ELF32 (big-endian) on powerpc.
 * Fields are in the CPU's byte order, so plain struct access is right. */
#if BITS_PER_LONG == 64
struct elf_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));
#define ELFCLASS_NATIVE 2
#else
struct elf_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint32_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));
#define ELFCLASS_NATIVE 1
#endif

#define PT_LOAD   1
#define PF_X      1
#define PF_W      2
#define ET_EXEC   2
#define EM_X86_64 62
#define EM_PPC    20
#if defined(__x86_64__)
#define EM_NATIVE EM_X86_64
#elif defined(__powerpc__)
#define EM_NATIVE EM_PPC
#endif
typedef uintptr_t elf_word_t;           /* the size of a pointer on the initial stack */

#define MMAP_BASE USER_MMAP_BASE


/* ---- address-space helpers ------------------------------------------------- */

static int map_zeroed(uint64_t pgd, uint64_t virt, size_t pages, uint64_t flags)
{
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            kprintf("%s: out of physical memory (%llu/%llu pages used)\n", task_current()->name, pmm_used_pages(), pmm_total_pages());
            return -1;
        }
        memset(P2V(phys), 0, PAGE_SIZE);
        int rc = vmm_map_user_page(pgd, virt + i * PAGE_SIZE, phys, flags);
        if (rc == -2)                       /* already mapped: keep the existing page */
            pmm_free_page(phys);
        else if (rc != 0) {
            pmm_free_page(phys);
            return -1;
        }
    }
    return 0;
}

static uint64_t prot_to_pte(int prot)
{
    return ((prot & PROT_WRITE) ? PTE_WRITE : 0) | ((prot & PROT_EXEC) ? 0 : PTE_NX);
}

int process_map_anon(struct task *t, uint64_t virt, size_t pages, int prot)
{
    return map_zeroed(t->mm->pgd, virt, pages, prot_to_pte(prot));
}

int process_protect(struct task *t, uint64_t virt, size_t pages, int prot)
{
    for (size_t i = 0; i < pages; i++)
        if (vmm_protect_user_page(t->mm->pgd, virt + i * PAGE_SIZE, prot_to_pte(prot)) != 0)
            return -1;
    return 0;
}

void process_unmap(struct task *t, uint64_t virt, size_t pages)
{
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = vmm_unmap_user_page(t->mm->pgd, virt + i * PAGE_SIZE);
        if (phys)
            pmm_free_page(phys);
    }
}

/* Copy into a (not necessarily current) user address space through the HHDM. */
static int copy_to_space(uint64_t pgd, uint64_t virt, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t phys = vmm_translate_in(pgd, virt);
        if (!phys)
            return -1;
        size_t chunk = PAGE_SIZE - (virt & 0xFFF);
        if (chunk > len)
            chunk = len;
        memcpy(P2V(phys), s, chunk);
        virt += chunk;
        s += chunk;
        len -= chunk;
    }
    return 0;
}

struct image {
    uint64_t pgd, entry, brk;
    uint64_t phdr_addr, phnum, phent;     /* for auxv */
};

static int elf_load(struct image *im, const void *data, size_t size)
{
    const struct elf_ehdr *eh = data;
    if (size < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != ELFCLASS_NATIVE)
        return -ENOEXEC;
    if (eh->machine != EM_NATIVE || eh->type != ET_EXEC)
        return -ENOEXEC;
    if (eh->ident[EI_OSABI] != ELFOSABI_SIC || eh->ident[EI_ABIVERSION] != 0) {
        kprintf("exec: not a sic executable (OS/ABI %u)\n", eh->ident[EI_OSABI]);
        return -ENOEXEC;
    }
    if (eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf_phdr) > size)
        return -ENOEXEC;

    const struct elf_phdr *ph = (const void *)((const uint8_t *)data + eh->phoff);
    uint64_t top = 0;
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;
        if (ph[i].offset + ph[i].filesz > size)
            return -ENOEXEC;
        if (ph[i].vaddr < USER_BASE || ph[i].vaddr + ph[i].memsz > MMAP_BASE)
            return -ENOEXEC;

        uint64_t start = PAGE_ALIGN_DOWN(ph[i].vaddr);
        uint64_t end   = PAGE_ALIGN_UP(ph[i].vaddr + ph[i].memsz);
        uint64_t flags = 0;
        if (ph[i].flags & PF_W)   flags |= PTE_WRITE;
        if (!(ph[i].flags & PF_X)) flags |= PTE_NX;

        if (map_zeroed(im->pgd, start, (end - start) / PAGE_SIZE, flags) != 0)
            return -ENOMEM;
        if (copy_to_space(im->pgd, ph[i].vaddr, (const uint8_t *)data + ph[i].offset, ph[i].filesz) != 0)
            return -ENOMEM;
        if (end > top)
            top = end;
    }

    /* AT_PHDR must be where the program headers really are in memory: the
     * libc derives the load base from it (AT_PHDR - PT_PHDR.p_vaddr) and
     * finds PT_TLS through it. Normally the first PT_LOAD covers them. */
    size_t phbytes = (size_t)eh->phnum * sizeof(struct elf_phdr);
    uint64_t phdr_addr = 0;
    for (uint16_t i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD && ph[i].offset <= eh->phoff &&
            eh->phoff + phbytes <= ph[i].offset + ph[i].filesz)
            phdr_addr = ph[i].vaddr + (eh->phoff - ph[i].offset);
    if (!phdr_addr) {
        /* Not loaded: give the libc a private copy after the image. */
        phdr_addr = top + PAGE_SIZE;
        if (map_zeroed(im->pgd, phdr_addr, PAGE_ALIGN_UP(phbytes) / PAGE_SIZE, PTE_NX) != 0 ||
            copy_to_space(im->pgd, phdr_addr, ph, phbytes) != 0)
            return -ENOMEM;
        top = phdr_addr + PAGE_ALIGN_UP(phbytes);
    }

    im->entry = eh->entry;
    im->phdr_addr = phdr_addr;
    im->phnum = eh->phnum;
    im->phent = sizeof(struct elf_phdr);
    im->brk = top + PAGE_SIZE;                              /* guard page, then the heap */
    return 0;
}

/* Kernel-side copy of argv/envp: a fixed arena so nothing is borrowed from
 * the address space we're about to tear down. */
struct argpack {
    int    argc, envc;
    char  *argv[ARG_MAX + 1];
    char  *envp[ARG_MAX + 1];
    char   data[ARG_BYTES];
    size_t used;
};

static int pack_list(struct argpack *ap, const char *const list[], char **out, int *count)
{
    *count = 0;
    for (int i = 0; list && list[i]; i++) {
        if (i >= ARG_MAX)
            return -E2BIG;
        size_t l = strlen(list[i]) + 1;
        if (ap->used + l > ARG_BYTES)
            return -E2BIG;
        memcpy(ap->data + ap->used, list[i], l);
        out[i] = ap->data + ap->used;
        ap->used += l;
        (*count)++;
    }
    out[*count] = NULL;
    return 0;
}

static uint32_t rng_state = 0x9E3779B9;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state ^ (uint32_t)timer_ticks();
}

/* Build the initial stack; returns the user rsp (pointing at argc). */
static int build_stack(const struct image *im, const struct argpack *ap, uint64_t *rsp_out)
{
    if (map_zeroed(im->pgd, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE / PAGE_SIZE,
                   PTE_WRITE | PTE_NX) != 0)
        return -ENOMEM;

    uint64_t sp = USER_STACK_TOP;
    elf_word_t argv_ptrs[ARG_MAX + 1], envp_ptrs[ARG_MAX + 1];

#define PUSH_BYTES(src, len) do { sp -= (len); if (copy_to_space(im->pgd, sp, (src), (len)) != 0) return -ENOMEM; } while (0)
    for (int i = 0; i < ap->envc; i++) { PUSH_BYTES(ap->envp[i], strlen(ap->envp[i]) + 1); envp_ptrs[i] = (elf_word_t)sp; }
    for (int i = 0; i < ap->argc; i++) { PUSH_BYTES(ap->argv[i], strlen(ap->argv[i]) + 1); argv_ptrs[i] = (elf_word_t)sp; }
    argv_ptrs[ap->argc] = 0;
    envp_ptrs[ap->envc] = 0;

    uint32_t rnd[4] = { rng(), rng(), rng(), rng() };
    sp &= ~15ULL;
    PUSH_BYTES(rnd, 16);
    elf_word_t random_addr = (elf_word_t)sp;

    elf_word_t auxv[] = {
        AT_PHDR,   (elf_word_t)im->phdr_addr,
        AT_PHENT,  (elf_word_t)im->phent,
        AT_PHNUM,  (elf_word_t)im->phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_ENTRY,  (elf_word_t)im->entry,
        AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,
        AT_SECURE, 0,
        AT_HWCAP,  0,
        AT_CLKTCK, 100,
        AT_RANDOM, random_addr,
        AT_NULL,   0,
    };

    /* Vector area: argc, argv[], NULL, envp[], NULL, auxv. Keep the stack 16-aligned. */
    const size_t W = sizeof(elf_word_t);
    size_t vec_bytes = W + (ap->argc + 1) * W + (ap->envc + 1) * W + sizeof(auxv);
    sp = (sp - vec_bytes) & ~15ULL;
    uint64_t p = sp;
    elf_word_t argc_w = (elf_word_t)ap->argc;
    if (copy_to_space(im->pgd, p, &argc_w, W) != 0) return -ENOMEM;
    p += W;
    if (copy_to_space(im->pgd, p, argv_ptrs, (ap->argc + 1) * W) != 0) return -ENOMEM;
    p += (ap->argc + 1) * W;
    if (copy_to_space(im->pgd, p, envp_ptrs, (ap->envc + 1) * W) != 0) return -ENOMEM;
    p += (ap->envc + 1) * W;
    if (copy_to_space(im->pgd, p, auxv, sizeof(auxv)) != 0) return -ENOMEM;
#undef PUSH_BYTES

    *rsp_out = sp;
    return 0;
}

/* Load `path` + args into a brand-new address space. */
static int build_image(const char *path, const struct argpack *ap, struct image *im, uint64_t *rsp)
{
    struct vnode *n = vfs_lookup(task_current()->cwd, path);
    if (!n)
        return -ENOENT;
    if (n->type != VNODE_FILE)
        return -EACCES;
    size_t size;
    void *data = vfs_read_all(n, &size);
    if (!data)
        return -ENOMEM;

    memset(im, 0, sizeof(*im));
    im->pgd = vmm_create_address_space();
    int rc = elf_load(im, data, size);
    kfree(data);
    if (rc == 0)
        rc = build_stack(im, ap, rsp);
    if (rc != 0) {
        vmm_destroy_address_space(im->pgd);
        return rc;
    }
    return 0;
}

static void set_name(struct task *t, const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1])
            base = p + 1;
    size_t n = strlen(base);
    if (n >= sizeof(t->name))
        n = sizeof(t->name) - 1;
    memset(t->name, 0, sizeof(t->name));
    memcpy(t->name, base, n);
}

/* Give `t` a fresh mm for `im`; returns the old mm (to be put) or NULL. */
static struct mm *apply_image(struct task *t, const struct image *im)
{
    struct mm *mm = mm_create(im->pgd);
    if (!mm)
        return NULL;
    mm->brk_start = mm->brk_end = im->brk;
    mm->mmap_next = MMAP_BASE;
    struct mm *old = t->mm;
    t->mm = mm;
    t->pgd = mm->pgd;
    arch_task_init_user(t);
    t->clear_child_tid = 0;
    return old;
}

/* ---- spawn -------------------------------------------------------------------------- */

struct spawn_args { uint64_t entry, rsp; };

static void spawn_thunk(void *arg)
{
    struct spawn_args a = *(struct spawn_args *)arg;
    kfree(arg);
    arch_exec_enter(task_current(), a.entry, a.rsp);
}

struct task *process_spawn(const char *path, const char *const argv[], const char *const envp[])
{
    struct argpack *ap = kzalloc(sizeof(*ap));
    if (!ap)
        return NULL;
    const char *const dflt_argv[] = { path, NULL };
    const char *const dflt_envp[] = { "PATH=/bin", NULL };
    if (pack_list(ap, argv ? argv : dflt_argv, ap->argv, &ap->argc) != 0 ||
        pack_list(ap, envp ? envp : dflt_envp, ap->envp, &ap->envc) != 0) {
        kfree(ap);
        return NULL;
    }

    struct spawn_args *sa = kzalloc(sizeof(*sa));
    struct image im;
    int rc = sa ? build_image(path, ap, &im, &sa->rsp) : -ENOMEM;
    kfree(ap);
    if (rc != 0) {
        kprintf("spawn(%s): error %d\n", path, -rc);
        kfree(sa);
        return NULL;
    }
    sa->entry = im.entry;

    struct task *t = task_alloc(path, spawn_thunk, sa);
    if (!t) {
        vmm_destroy_address_space(im.pgd);
        kfree(sa);
        return NULL;
    }
    set_name(t, path);
    t->fdt = fdt_create();
    apply_image(t, &im);                /* a fresh task has no old mm to return */
    if (!t->fdt || !t->mm) {
        if (!t->mm) vmm_destroy_address_space(im.pgd); else mm_put(t->mm);
        fdt_put(t->fdt);
        signal_release(t);
        kstack_free(t->stack, t->stack_pages);
        kfree(t);
        kfree(sa);
        return NULL;
    }
    t->is_user = 1;
    t->parent_id = task_current()->tgid;
    t->cwd = vfs_root();
    t->fdt->files[0] = vfs_open(vfs_root(), "/dev/console", O_RDONLY);
    t->fdt->files[1] = vfs_open(vfs_root(), "/dev/console", O_WRONLY);
    t->fdt->files[2] = vfs_open(vfs_root(), "/dev/console", O_WRONLY);
    task_start(t);
    return t;
}

/* ---- exec --------------------------------------------------------------------------- */

long process_exec(const char *path, const char *const argv[], const char *const envp[])
{
    struct task *t = task_current();
    struct argpack *ap = kzalloc(sizeof(*ap));
    if (!ap)
        return -ENOMEM;
    const char *const dflt_argv[] = { path, NULL };
    int rc = pack_list(ap, argv ? argv : dflt_argv, ap->argv, &ap->argc);
    if (rc == 0)
        rc = pack_list(ap, envp, ap->envp, &ap->envc);

    struct image im;
    uint64_t rsp;
    if (rc == 0)
        rc = build_image(path, ap, &im, &rsp);
    kfree(ap);
    if (rc != 0)
        return rc;

    /* Point of no return: swap address spaces and restart in user mode.
     * Other threads of the process, if any, are told to exit. */
    set_name(t, path);
    struct mm *old = apply_image(t, &im);
    if (!t->mm || t->mm->pgd != im.pgd) {
        vmm_destroy_address_space(im.pgd);
        return -ENOMEM;
    }
    task_kill_other_threads();
    if (t->is_thread) {
        t->is_thread = 0;               /* the exec'ing thread becomes the process */
        t->tgid = t->id;
    }
    signal_exec(t);
    arch_task_init_user(t);
    mm_put(old);
    arch_exec_enter(t, im.entry, rsp);
}

/* ---- fork --------------------------------------------------------------------------- */

long process_fork(struct syscall_frame *f)
{
    struct task *parent = task_current();

    uint64_t pgd = vmm_clone_address_space(parent->mm->pgd);
    if (!pgd)
        return -ENOMEM;
    struct mm *mm = mm_create(pgd);
    struct task *child = mm ? task_alloc(parent->name, NULL, NULL) : NULL;
    struct fdtable *fdt = child ? fdt_clone(parent->fdt) : NULL;
    if (!mm || !child || !fdt || signal_fork(child, parent) != 0) {
        if (fdt) fdt_put(fdt);
        if (child) { signal_release(child); kstack_free(child->stack, child->stack_pages); kfree(child); }
        if (mm) mm_put(mm); else vmm_destroy_address_space(pgd);
        return -ENOMEM;
    }
    spin_lock(&parent->mm->lock);
    mm->brk_start = parent->mm->brk_start;
    mm->brk_end = parent->mm->brk_end;
    mm->mmap_next = parent->mm->mmap_next;
    spin_unlock(&parent->mm->lock);

    arch_task_fork(child, parent, f, 0, 0);
    child->mm = mm;
    child->pgd = pgd;
    child->fdt = fdt;
    child->is_user = 1;
    child->parent_id = parent->tgid;
    child->cwd = parent->cwd;
    child->clear_child_tid = 0;

    task_start(child);
    return child->id;
}

/* ---- clone (threads) ---------------------------------------------------------------- */

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

long process_clone(struct syscall_frame *f, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    struct task *parent = task_current();
    if (!(flags & CLONE_VM))
        return process_fork(f);         /* plain clone(SIGCHLD) is fork */
    if (!(flags & CLONE_THREAD) || !stack)
        return -EINVAL;                 /* shared-VM non-thread clones: not supported */

    struct task *t = task_alloc(parent->name, NULL, NULL);
    if (!t)
        return -ENOMEM;
    signal_release(t);                  /* task_alloc gave it a private sighand */
    arch_task_fork(t, parent, f, stack, (flags & CLONE_SETTLS) ? tls : 0);

    t->mm = mm_get(parent->mm);
    t->pgd = t->mm->pgd;
    t->fdt = (flags & CLONE_FILES) ? fdt_get(parent->fdt) : fdt_clone(parent->fdt);
    if (flags & CLONE_SIGHAND)
        signal_clone(t, parent);
    else if (signal_fork(t, parent) != 0) {
        mm_put(t->mm); fdt_put(t->fdt); kstack_free(t->stack, t->stack_pages); kfree(t);
        return -ENOMEM;
    }
    t->is_user = 1;
    t->is_thread = 1;
    t->tgid = parent->tgid;
    t->parent_id = parent->parent_id;
    t->cwd = parent->cwd;
    t->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;

    task_start(t);                      /* assigns the tid */
    if ((flags & CLONE_PARENT_SETTID) && ptid >= USER_BASE && ptid + 4 <= USER_END &&
        vmm_translate_in(parent->mm->pgd, ptid))
        *(volatile uint32_t *)ptid = t->id;
    if ((flags & CLONE_CHILD_SETTID) && ctid >= USER_BASE && ctid + 4 <= USER_END &&
        vmm_translate_in(parent->mm->pgd, ctid))
        *(volatile uint32_t *)ctid = t->id;
    return t->id;
}
