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
#include "arch/x86_64/timer.h"
#include "arch/x86_64/cpu.h"
#include "string.h"
#include "printf.h"

struct elf64_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

#define PT_LOAD   1
#define PF_X      1
#define PF_W      2
#define ET_EXEC   2
#define EM_X86_64 62

#define MMAP_BASE 0x0000010000000000UL      /* 1 TiB: anonymous mappings grow up from here */

extern void enter_usermode(uint64_t rip, uint64_t rsp, uint64_t argc, uint64_t argv) __attribute__((noreturn));
extern void fork_return(struct syscall_frame *f);

/* ---- address-space helpers ------------------------------------------------- */

static int map_zeroed(uint64_t pml4, uint64_t virt, size_t pages, uint64_t flags)
{
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys)
            return -1;
        memset(P2V(phys), 0, PAGE_SIZE);
        int rc = vmm_map_user_page(pml4, virt + i * PAGE_SIZE, phys, flags);
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
    return map_zeroed(t->pml4, virt, pages, prot_to_pte(prot));
}

int process_protect(struct task *t, uint64_t virt, size_t pages, int prot)
{
    for (size_t i = 0; i < pages; i++)
        if (vmm_protect_user_page(t->pml4, virt + i * PAGE_SIZE, prot_to_pte(prot)) != 0)
            return -1;
    return 0;
}

void process_unmap(struct task *t, uint64_t virt, size_t pages)
{
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = vmm_unmap_user_page(t->pml4, virt + i * PAGE_SIZE);
        if (phys)
            pmm_free_page(phys);
    }
}

/* Copy into a (not necessarily current) user address space through the HHDM. */
static int copy_to_space(uint64_t pml4, uint64_t virt, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t phys = vmm_translate_in(pml4, virt);
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
    uint64_t pml4, entry, brk;
    uint64_t phdr_addr, phnum, phent;     /* for auxv */
};

static int elf_load(struct image *im, const void *data, size_t size)
{
    const struct elf64_ehdr *eh = data;
    if (size < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != 2)
        return -ENOEXEC;
    if (eh->machine != EM_X86_64 || eh->type != ET_EXEC)
        return -ENOEXEC;
    if (eh->ident[EI_OSABI] != ELFOSABI_SIC || eh->ident[EI_ABIVERSION] != 0) {
        kprintf("exec: not a sic executable (OS/ABI %u)\n", eh->ident[EI_OSABI]);
        return -ENOEXEC;
    }
    if (eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > size)
        return -ENOEXEC;

    const struct elf64_phdr *ph = (const void *)((const uint8_t *)data + eh->phoff);
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

        if (map_zeroed(im->pml4, start, (end - start) / PAGE_SIZE, flags) != 0)
            return -ENOMEM;
        if (copy_to_space(im->pml4, ph[i].vaddr, (const uint8_t *)data + ph[i].offset, ph[i].filesz) != 0)
            return -ENOMEM;
        if (end > top)
            top = end;
    }

    /* AT_PHDR must be where the program headers really are in memory: the
     * libc derives the load base from it (AT_PHDR - PT_PHDR.p_vaddr) and
     * finds PT_TLS through it. Normally the first PT_LOAD covers them. */
    size_t phbytes = (size_t)eh->phnum * sizeof(struct elf64_phdr);
    uint64_t phdr_addr = 0;
    for (uint16_t i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD && ph[i].offset <= eh->phoff &&
            eh->phoff + phbytes <= ph[i].offset + ph[i].filesz)
            phdr_addr = ph[i].vaddr + (eh->phoff - ph[i].offset);
    if (!phdr_addr) {
        /* Not loaded: give the libc a private copy after the image. */
        phdr_addr = top + PAGE_SIZE;
        if (map_zeroed(im->pml4, phdr_addr, PAGE_ALIGN_UP(phbytes) / PAGE_SIZE, PTE_NX) != 0 ||
            copy_to_space(im->pml4, phdr_addr, ph, phbytes) != 0)
            return -ENOMEM;
        top = phdr_addr + PAGE_ALIGN_UP(phbytes);
    }

    im->entry = eh->entry;
    im->phdr_addr = phdr_addr;
    im->phnum = eh->phnum;
    im->phent = sizeof(struct elf64_phdr);
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
    if (map_zeroed(im->pml4, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE / PAGE_SIZE,
                   PTE_WRITE | PTE_NX) != 0)
        return -ENOMEM;

    uint64_t sp = USER_STACK_TOP;
    uint64_t argv_ptrs[ARG_MAX + 1], envp_ptrs[ARG_MAX + 1];

#define PUSH_BYTES(src, len) do { sp -= (len); if (copy_to_space(im->pml4, sp, (src), (len)) != 0) return -ENOMEM; } while (0)
    for (int i = 0; i < ap->envc; i++) { PUSH_BYTES(ap->envp[i], strlen(ap->envp[i]) + 1); envp_ptrs[i] = sp; }
    for (int i = 0; i < ap->argc; i++) { PUSH_BYTES(ap->argv[i], strlen(ap->argv[i]) + 1); argv_ptrs[i] = sp; }
    argv_ptrs[ap->argc] = 0;
    envp_ptrs[ap->envc] = 0;

    uint32_t rnd[4] = { rng(), rng(), rng(), rng() };
    sp &= ~15UL;
    PUSH_BYTES(rnd, 16);
    uint64_t random_addr = sp;

    uint64_t auxv[] = {
        AT_PHDR,   im->phdr_addr,
        AT_PHENT,  im->phent,
        AT_PHNUM,  im->phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_ENTRY,  im->entry,
        AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,
        AT_SECURE, 0,
        AT_HWCAP,  0,
        AT_CLKTCK, 100,
        AT_RANDOM, random_addr,
        AT_NULL,   0,
    };

    /* Vector area: argc, argv[], NULL, envp[], NULL, auxv. Keep rsp 16-aligned. */
    size_t vec_bytes = 8 + (ap->argc + 1) * 8 + (ap->envc + 1) * 8 + sizeof(auxv);
    sp = (sp - vec_bytes) & ~15UL;
    uint64_t p = sp;
    uint64_t argc64 = (uint64_t)ap->argc;
    if (copy_to_space(im->pml4, p, &argc64, 8) != 0) return -ENOMEM;
    p += 8;
    if (copy_to_space(im->pml4, p, argv_ptrs, (ap->argc + 1) * 8) != 0) return -ENOMEM;
    p += (ap->argc + 1) * 8;
    if (copy_to_space(im->pml4, p, envp_ptrs, (ap->envc + 1) * 8) != 0) return -ENOMEM;
    p += (ap->envc + 1) * 8;
    if (copy_to_space(im->pml4, p, auxv, sizeof(auxv)) != 0) return -ENOMEM;
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
    im->pml4 = vmm_create_address_space();
    int rc = elf_load(im, data, size);
    kfree(data);
    if (rc == 0)
        rc = build_stack(im, ap, rsp);
    if (rc != 0) {
        vmm_destroy_address_space(im->pml4);
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

static void apply_image(struct task *t, const struct image *im)
{
    t->pml4 = im->pml4;
    t->brk_start = t->brk_end = im->brk;
    t->mmap_next = MMAP_BASE;
    t->fs_base = 0;
    memcpy(t->fpu, fpu_initial_state, sizeof(t->fpu));
}

/* ---- spawn -------------------------------------------------------------------------- */

struct spawn_args { uint64_t entry, rsp; };

static void spawn_thunk(void *arg)
{
    struct spawn_args a = *(struct spawn_args *)arg;
    kfree(arg);
    enter_usermode(a.entry, a.rsp, 0, 0);
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
        vmm_destroy_address_space(im.pml4);
        kfree(sa);
        return NULL;
    }
    set_name(t, path);
    apply_image(t, &im);
    t->is_user = 1;
    t->parent_id = task_current()->id;
    t->cwd = vfs_root();
    t->files[0] = vfs_open(vfs_root(), "/dev/console", O_RDONLY);
    t->files[1] = vfs_open(vfs_root(), "/dev/console", O_WRONLY);
    t->files[2] = vfs_open(vfs_root(), "/dev/console", O_WRONLY);
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

    /* Point of no return: swap address spaces and restart in user mode. */
    uint64_t old = t->pml4;
    set_name(t, path);
    apply_image(t, &im);
    write_cr3(im.pml4);
    wrmsr(MSR_FS_BASE, 0);
    vmm_destroy_address_space(old);
    enter_usermode(im.entry, rsp, 0, 0);
}

/* ---- fork --------------------------------------------------------------------------- */

static void fork_thunk(void *arg)
{
    (void)arg;
    fork_return((struct syscall_frame *)(task_current()->kstack_top - sizeof(struct syscall_frame)));
}

long process_fork(struct syscall_frame *f)
{
    struct task *parent = task_current();

    uint64_t pml4 = vmm_clone_address_space(parent->pml4);
    if (!pml4)
        return -ENOMEM;

    struct task *child = task_alloc_reserve(parent->name, fork_thunk, NULL, sizeof(struct syscall_frame));
    if (!child) {
        vmm_destroy_address_space(pml4);
        return -ENOMEM;
    }

    /* The child resumes right after the syscall instruction with rax = 0. */
    struct syscall_frame *cf = (struct syscall_frame *)(child->kstack_top - sizeof(*cf));
    *cf = *f;
    cf->rax = 0;

    child->pml4 = pml4;
    child->is_user = 1;
    child->parent_id = parent->id;
    child->cwd = parent->cwd;
    child->fs_base = parent->fs_base;
    child->brk_start = parent->brk_start;
    child->brk_end = parent->brk_end;
    child->mmap_next = parent->mmap_next;
    fpu_save(parent->fpu);                  /* the parent's live state is in the registers */
    memcpy(child->fpu, parent->fpu, sizeof(child->fpu));
    for (int i = 0; i < MAX_FDS; i++)
        if (parent->files[i])
            child->files[i] = file_dup(parent->files[i]);

    task_start(child);
    return child->id;
}
