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
#include "fs/fdtable.h"
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
#define PT_INTERP 3
#define PF_X      1
#define PF_W      2
#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 62
#define EM_PPC    20
#define EM_AARCH64 183
#if defined(__x86_64__)
#define EM_NATIVE EM_X86_64
#elif defined(__powerpc__)
#define EM_NATIVE EM_PPC
#elif defined(__aarch64__)
#define EM_NATIVE EM_AARCH64
#endif
typedef uintptr_t elf_word_t;           /* the size of a pointer on the initial stack */

#define MMAP_BASE USER_MMAP_BASE
/* Where a position-independent executable (ET_DYN) goes: where the static
 * ones are linked, so the two kinds share one layout. The interpreter
 * (PT_INTERP, musl's ld.so) goes at the start of the mmap area. */
#if BITS_PER_LONG == 64
#define PIE_BASE 0x8000000000UL
#else
#define PIE_BASE 0x10000000UL
#endif


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
    uint64_t start;                       /* where the thread begins: the entry, or the interpreter's */
    uint64_t interp_base;                 /* AT_BASE: the interpreter's load address, 0 without one */
    uint64_t mmap_next;                   /* the mmap area starts after the interpreter */
    uint64_t arg_start, arg_end;          /* the argv strings, contiguous, argv[0] first */
    char exe[256];                        /* where the file is, absolute */
};

/* One ELF object, as the kernel maps it: an executable, or the dynamic
 * linker named by its PT_INTERP. */
struct loaded {
    uint64_t entry, phdr, phnum, top, bias;
    char interp[128];                     /* PT_INTERP, or "" */
};

/* Map `n` into `pgd`. ET_EXEC goes where it is linked; ET_DYN is moved so
 * its lowest segment starts at `dyn_base`. */
static int load_object(uint64_t pgd, struct vnode *n, uint64_t dyn_base, struct loaded *lo)
{
    uint64_t size = n->size;
    struct elf_ehdr eh;
    memset(lo, 0, sizeof(*lo));
    if (size < sizeof(eh) || vfs_pread(n, &eh, sizeof(eh), 0) != (long)sizeof(eh))
        return -ENOEXEC;
    if (memcmp(eh.ident, "\x7f" "ELF", 4) != 0 || eh.ident[4] != ELFCLASS_NATIVE)
        return -ENOEXEC;
    if (eh.machine != EM_NATIVE || (eh.type != ET_EXEC && eh.type != ET_DYN))
        return -ENOEXEC;
    if (eh.ident[EI_OSABI] != ELFOSABI_SIC || eh.ident[EI_ABIVERSION] != 0) {
        kprintf("exec: not a sic executable (OS/ABI %u)\n", eh.ident[EI_OSABI]);
        return -ENOEXEC;
    }
    if (eh.phnum == 0 || eh.phnum > 128)
        return -ENOEXEC;
    size_t phbytes = (size_t)eh.phnum * sizeof(struct elf_phdr);
    if (eh.phoff + phbytes > size)
        return -ENOEXEC;

    struct elf_phdr *ph = kmalloc(phbytes);
    if (!ph)
        return -ENOMEM;
    if (vfs_pread(n, ph, phbytes, eh.phoff) != (long)phbytes) {
        kfree(ph);
        return -ENOEXEC;
    }

    /* The bias: 0 for ET_EXEC; for ET_DYN, what puts the lowest segment at dyn_base. */
    uint64_t bias = 0, lo_vaddr = ~0ULL;
    for (uint16_t i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_LOAD && ph[i].memsz && PAGE_ALIGN_DOWN(ph[i].vaddr) < lo_vaddr)
            lo_vaddr = PAGE_ALIGN_DOWN(ph[i].vaddr);
    if (lo_vaddr == ~0ULL) {
        kfree(ph);
        return -ENOEXEC;
    }
    if (eh.type == ET_DYN)
        bias = dyn_base - lo_vaddr;
    uint64_t limit = eh.type == ET_DYN ? USER_STACK_TOP - USER_STACK_MAX - PAGE_SIZE : MMAP_BASE;

    uint64_t top = 0;
    int code = 0;
    for (uint16_t i = 0; i < eh.phnum; i++) {
        if (ph[i].type == PT_INTERP) {
            if (ph[i].filesz < 2 || ph[i].filesz > sizeof(lo->interp) || ph[i].offset + ph[i].filesz > size ||
                vfs_pread(n, lo->interp, ph[i].filesz, ph[i].offset) != (long)ph[i].filesz) {
                kfree(ph);
                return -ENOEXEC;
            }
            lo->interp[sizeof(lo->interp) - 1] = 0;
            continue;
        }
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;
        if (ph[i].offset + ph[i].filesz > size) {
            kfree(ph);
            return -ENOEXEC;
        }
        uint64_t seg = ph[i].vaddr + bias;
        if (seg < USER_BASE || seg + ph[i].memsz > limit || seg + ph[i].memsz < seg) {
            kfree(ph);
            return -ENOEXEC;
        }

        uint64_t start = PAGE_ALIGN_DOWN(seg);
        uint64_t end   = PAGE_ALIGN_UP(seg + ph[i].memsz);
        uint64_t flags = 0;
        if (ph[i].flags & PF_W)   flags |= PTE_WRITE;
        if (!(ph[i].flags & PF_X)) flags |= PTE_NX;

        if (map_zeroed(pgd, start, (end - start) / PAGE_SIZE, flags) != 0) {
            kfree(ph);
            return -ENOMEM;
        }

        uint64_t file_off = ph[i].offset;
        uint64_t vaddr = seg;
        size_t remaining = ph[i].filesz;
        while (remaining > 0) {
            uint64_t phys = vmm_translate_in(pgd, vaddr);
            if (!phys) {
                kfree(ph);
                return -ENOMEM;
            }
            size_t chunk = PAGE_SIZE - (vaddr & 0xFFF);
            if (chunk > remaining)
                chunk = remaining;
            long r = vfs_pread(n, P2V(phys), chunk, file_off);
            if (r != (long)chunk) {
                kfree(ph);
                return -EIO;
            }
            vaddr += chunk;
            file_off += chunk;
            remaining -= chunk;
        }

        if (ph[i].flags & PF_X) {
            /* the frames may have held another program's code: make the
             * new code what instruction fetches see */
            for (uint64_t va = start; va < end; va += PAGE_SIZE) {
                uint64_t pa = vmm_translate_in(pgd, va);
                if (pa) arch_dcache_clean(P2V(pa & ~(uint64_t)(PAGE_SIZE - 1)), PAGE_SIZE);
            }
            code = 1;
        }
        if (end > top)
            top = end;
    }
    if (code) arch_icache_invalidate_all();

    /* AT_PHDR must be where the program headers really are in memory: the
     * libc derives the load base from it (AT_PHDR - PT_PHDR.p_vaddr) and
     * finds PT_TLS through it. Normally the first PT_LOAD covers them. */
    uint64_t phdr_addr = 0;
    for (uint16_t i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_LOAD && ph[i].offset <= eh.phoff &&
            eh.phoff + phbytes <= ph[i].offset + ph[i].filesz)
            phdr_addr = ph[i].vaddr + bias + (eh.phoff - ph[i].offset);
    if (!phdr_addr) {
        /* Not loaded: give the libc a private copy after the image. */
        phdr_addr = top + PAGE_SIZE;
        if (map_zeroed(pgd, phdr_addr, PAGE_ALIGN_UP(phbytes) / PAGE_SIZE, PTE_NX) != 0 ||
            copy_to_space(pgd, phdr_addr, ph, phbytes) != 0) {
            kfree(ph);
            return -ENOMEM;
        }
        top = phdr_addr + PAGE_ALIGN_UP(phbytes);
    }

    lo->entry = eh.entry + bias;
    lo->phdr = phdr_addr;
    lo->phnum = eh.phnum;
    lo->top = top;
    lo->bias = bias;
    kfree(ph);
    return 0;
}

/* The program, and its dynamic linker if it names one (PT_INTERP): the
 * thread starts in the linker, which finds the program through AT_PHDR,
 * AT_ENTRY and its own load address in AT_BASE. */
static int elf_load(struct image *im, struct vnode *n)
{
    struct loaded prog;
    int rc = load_object(im->pgd, n, PIE_BASE, &prog);
    if (rc != 0)
        return rc;
    im->entry = prog.entry;
    im->phdr_addr = prog.phdr;
    im->phnum = prog.phnum;
    im->phent = sizeof(struct elf_phdr);
    im->brk = prog.top + PAGE_SIZE;                         /* guard page, then the heap */
    im->start = prog.entry;
    im->interp_base = 0;
    im->mmap_next = MMAP_BASE;
    if (!prog.interp[0])
        return 0;

    struct vnode *ld = vfs_lookup(task_current()->cwd, prog.interp);
    if (!ld || ld->type != VNODE_FILE) {
        kprintf("exec: %s: no dynamic linker %s\n", im->exe, prog.interp);
        return -ENOENT;
    }
    struct loaded interp;
    rc = load_object(im->pgd, ld, MMAP_BASE, &interp);
    if (rc != 0)
        return rc;
    if (interp.interp[0] || interp.bias == 0) {             /* a linker must be ET_DYN and need none */
        kprintf("exec: %s: %s is not a dynamic linker\n", im->exe, prog.interp);
        return -ENOEXEC;
    }
    im->start = interp.entry;
    im->interp_base = interp.bias;
    im->mmap_next = interp.top + PAGE_SIZE;
    return 0;
}

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

/* A fault below the mapped stack but inside its limit: map the page. */
int process_grow_stack(struct task *t, uint64_t addr)
{
    if (!t || !t->mm || addr >= USER_STACK_TOP || addr < USER_STACK_TOP - USER_STACK_MAX) return -1;
    uint64_t page = addr & ~(uint64_t)0xFFF;
    if (vmm_translate_in(t->mm->pgd, page)) return -1;         /* mapped: a real fault */
    return map_zeroed(t->mm->pgd, page, 1, PTE_WRITE | PTE_NX);
}

/* Build the initial stack; returns the user rsp (pointing at argc). */
static int build_stack(const struct image *im, const struct argpack *ap, uint64_t *rsp_out)
{
    /* what the arguments take (strings, pointers, auxv), on top of the usual stack */
    size_t need = ap->used + (size_t)(ap->argc + ap->envc + 2) * sizeof(elf_word_t) + 1024;
    size_t map = (USER_STACK_SIZE + need + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    if (map_zeroed(im->pgd, USER_STACK_TOP - map, map / PAGE_SIZE, PTE_WRITE | PTE_NX) != 0)
        return -ENOMEM;

    uint64_t sp = USER_STACK_TOP;
    elf_word_t *argv_ptrs = kmalloc((size_t)(ap->argc + 1) * sizeof(elf_word_t));
    elf_word_t *envp_ptrs = kmalloc((size_t)(ap->envc + 1) * sizeof(elf_word_t));
    int rc = -ENOMEM;
    if (!argv_ptrs || !envp_ptrs) goto out;

#define PUSH_BYTES(src, len) do { sp -= (len); if (copy_to_space(im->pgd, sp, (src), (len)) != 0) goto out; } while (0)
    for (int i = 0; i < ap->envc; i++) { PUSH_BYTES(ap->envp[i], strlen(ap->envp[i]) + 1); envp_ptrs[i] = (elf_word_t)sp; }
    /* argv last-to-first, so the strings lie in order (argv[0] lowest) as one
     * block, like Linux's: /proc/<pid>/cmdline is that block. */
    uint64_t args_hi = sp;
    for (int i = ap->argc - 1; i >= 0; i--) { PUSH_BYTES(ap->argv[i], strlen(ap->argv[i]) + 1); argv_ptrs[i] = (elf_word_t)sp; }
    ((struct image *)im)->arg_start = sp;
    ((struct image *)im)->arg_end = args_hi;
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
        AT_BASE,   (elf_word_t)im->interp_base,
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
    if (copy_to_space(im->pgd, p, &argc_w, W) != 0) goto out;
    p += W;
    if (copy_to_space(im->pgd, p, argv_ptrs, (ap->argc + 1) * W) != 0) goto out;
    p += (ap->argc + 1) * W;
    if (copy_to_space(im->pgd, p, envp_ptrs, (ap->envc + 1) * W) != 0) goto out;
    p += (ap->envc + 1) * W;
    if (copy_to_space(im->pgd, p, auxv, sizeof(auxv)) != 0) goto out;
#undef PUSH_BYTES

    *rsp_out = sp;
    rc = 0;
out:
    kfree(argv_ptrs);
    kfree(envp_ptrs);
    return rc;
}

/* Load `path` + args into a brand-new address space. */
static int build_image(const char *path, const struct argpack *ap, struct image *im, uint64_t *rsp)
{
    struct vnode *n = vfs_lookup(task_current()->cwd, path);
    if (!n)
        return -ENOENT;
    if (n->type != VNODE_FILE)
        return -EACCES;

    memset(im, 0, sizeof(*im));
    if (vfs_path_of(n, im->exe, sizeof(im->exe)) != 0) { size_t k = strlen(path); if (k > sizeof(im->exe) - 1) k = sizeof(im->exe) - 1; memcpy(im->exe, path, k); }
    im->pgd = vmm_create_address_space();
    int rc = elf_load(im, n);
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
    mm->mmap_next = im->mmap_next;
    mm->arg_start = im->arg_start;
    mm->arg_end = im->arg_end;
    memcpy(mm->exe, im->exe, sizeof(mm->exe));
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
    sa->entry = im.start;

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

/* A script: "#!interpreter [one argument]" on its first line. The
 * interpreter runs instead, with the argument (if any) and the script's
 * path before the script's own arguments (argv[0] dropped), as on Linux.
 * Returns 1 and rewrites ap/path, 0 if it isn't one, -errno. */
static int script(struct argpack *ap, const char **path)
{
    struct vnode *n = vfs_lookup(task_current()->cwd, *path);
    if (!n || n->type != VNODE_FILE || !n->ops || !n->ops->read) return 0;
    char line[128];
    long got = vfs_pread(n, line, sizeof line - 1, 0);    /* under vfs_lock: the fs block cache is shared */
    if (got < 3 || line[0] != '#' || line[1] != '!') return 0;
    line[got] = 0;
    char *p = line + 2, *end = line;
    while (*end && *end != '\n') end++;
    if (!*end) end = NULL;
    if (!end) return -ENOEXEC;              /* a first line longer than we read */
    *end = 0;
    while (*p == ' ' || *p == '\t') p++;
    char *interp = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    char *arg = NULL;
    if (*p) {
        *p++ = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) {
            arg = p;                        /* the rest of the line is one argument */
            char *t = p + strlen(p);
            while (t > p && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r')) *--t = 0;
        }
    }
    if (!*interp) return -ENOEXEC;

    size_t ilen = strlen(interp) + 1;
    size_t alen = arg ? strlen(arg) + 1 : 0;
    size_t plen = strlen(*path) + 1;
    if (ap->used + ilen + alen + plen > ARG_BYTES) return -E2BIG;

    int prefix_count = arg ? 3 : 2;
    if (ap->argc - 1 + prefix_count > ARG_MAX) return -E2BIG;

    char *new_interp = ap->data + ap->used;
    memcpy(new_interp, interp, ilen);
    ap->used += ilen;

    char *new_arg = NULL;
    if (arg) {
        new_arg = ap->data + ap->used;
        memcpy(new_arg, arg, alen);
        ap->used += alen;
    }

    char *new_path = ap->data + ap->used;
    memcpy(new_path, *path, plen);
    ap->used += plen;

    int shift = prefix_count - 1;
    for (int i = ap->argc - 1; i >= 1; i--)
        ap->argv[i + shift] = ap->argv[i];

    ap->argv[0] = new_interp;
    if (arg) {
        ap->argv[1] = new_arg;
        ap->argv[2] = new_path;
    } else {
        ap->argv[1] = new_path;
    }
    ap->argc += shift;
    ap->argv[ap->argc] = NULL;

    *path = new_interp;
    return 1;
}

long process_exec_ap(const char *path, struct argpack *ap)
{
    struct task *t = task_current();
    int rc = 0;
    for (int depth = 0; rc == 0; depth++) {      /* scripts, maybe run by scripts: a few levels */
        int s = script(ap, &path);
        if (s <= 0) { rc = s; break; }
        if (depth == 4) rc = -ELOOP;
    }
    char exe[VFS_PATH_MAX];                     /* the path may now be in ap, which goes before set_name */
    size_t pl = strlen(path) < sizeof exe - 1 ? strlen(path) : sizeof exe - 1;
    memcpy(exe, path, pl);
    exe[pl] = 0;
    path = exe;

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
    fdt_close_on_exec(t->fdt);
    arch_task_init_user(t);
    mm_put(old);
    arch_exec_enter(t, im.start, rsp);
}

long process_exec(const char *path, const char *const argv[], const char *const envp[])
{
    struct argpack *ap = kzalloc(sizeof(*ap));
    if (!ap)
        return -ENOMEM;
    const char *const dflt_argv[] = { path, NULL };
    int rc = pack_list(ap, argv ? argv : dflt_argv, ap->argv, &ap->argc);
    if (rc == 0)
        rc = pack_list(ap, envp, ap->envp, &ap->envc);
    if (rc != 0) {
        kfree(ap);
        return rc;
    }
    return process_exec_ap(path, ap);
}

/* ---- fork --------------------------------------------------------------------------- */


/* fork, the child resuming on `stack` instead of the parent's stack pointer
 * when it is non-zero (a clone(CLONE_VM) process, see process_clone). */
static long process_fork_on(struct syscall_frame *f, uint64_t stack)
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
    mm->arg_start = parent->mm->arg_start;
    mm->arg_end = parent->mm->arg_end;
    memcpy(mm->exe, parent->mm->exe, sizeof(mm->exe));
    spin_unlock(&parent->mm->lock);

    arch_task_fork(child, parent, f, stack, 0);
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

long process_fork(struct syscall_frame *f)
{
    return process_fork_on(f, 0);
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
    if (!stack)
        return -EINVAL;
    /* A shared-VM process, not a thread: what musl's posix_spawn (and so
     * clang starting the linker) does, CLONE_VM|CLONE_VFORK, to run a stub on
     * `stack` that execs. Like vfork, a copy-on-write fork resumed on that
     * stack is a valid stand-in: the stub only reads what the parent set up
     * and reports back through a pipe, never through the shared memory. */
    if (!(flags & CLONE_THREAD))
        return process_fork_on(f, stack);

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


/* ---- for /proc ------------------------------------------------------------------ */

/* Copy up to `len` bytes at `virt` in another address space (through the
 * direct map); stops at the first unmapped page. Returns the bytes copied. */
size_t process_read_mem(struct mm *mm, uint64_t virt, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        uint64_t phys = vmm_translate_in(mm->pgd, virt + done);
        if (!phys) break;
        size_t chunk = PAGE_SIZE - ((virt + done) & 0xFFF);
        if (chunk > len - done) chunk = len - done;
        memcpy((uint8_t *)buf + done, P2V(phys), chunk);
        done += chunk;
    }
    return done;
}

static uint64_t count_present(struct mm *mm, uint64_t lo, uint64_t hi)
{
    uint64_t n = 0;
    if (hi > lo + (8ULL << 30)) hi = lo + (8ULL << 30);     /* a sanity bound on the walk */
    for (uint64_t va = lo & ~(uint64_t)(PAGE_SIZE - 1); va < hi; va += PAGE_SIZE)
        if (vmm_translate_in(mm->pgd, va)) n++;
    return n;
}

/* Resident pages and address-space size, over the regions a process uses:
 * image and heap, the mmap area (libraries, mappings), the stack. */
void process_mem_stats(struct mm *mm, uint64_t *resident, uint64_t *size)
{
    uint64_t image_lo = PIE_BASE, heap_hi = mm->brk_end > image_lo ? mm->brk_end : image_lo;
    uint64_t mmap_hi = mm->mmap_next > MMAP_BASE ? mm->mmap_next : MMAP_BASE;
    *resident = count_present(mm, image_lo, heap_hi)
              + count_present(mm, MMAP_BASE, mmap_hi)
              + count_present(mm, USER_STACK_TOP - USER_STACK_MAX, USER_STACK_TOP);
    *size = (heap_hi - image_lo) + (mmap_hi - MMAP_BASE) + USER_STACK_SIZE;
}
