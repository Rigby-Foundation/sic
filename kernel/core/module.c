/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Loadable kernel modules: an ELF64 relocatable-object linker. */
#include "module.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "abi/abi.h"

extern const struct ksym __ksymtab_start[], __ksymtab_end[];

/* Modules must sit within ±2 GiB of the kernel (linked at 1 MiB) for the
 * small code model's 32-bit relocations; the low identity map is RWX. */
#define MODULE_ADDR_LIMIT 0x7FFF0000UL

struct elf_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_shdr {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} __attribute__((packed));

struct elf_sym {
    uint32_t name;
    uint8_t  info, other;
    uint16_t shndx;
    uint64_t value, size;
} __attribute__((packed));

struct elf_rela {
    uint64_t offset;
    uint64_t info;
    int64_t  addend;
} __attribute__((packed));

#define ET_REL       1
#define EM_X86_64    62
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_NOBITS   8
#define SHT_RELA     4
#define SHF_ALLOC    2
#define SHN_UNDEF    0
#define SHN_ABS      0xFFF1
#define SHN_COMMON   0xFFF2

#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11
#define R_X86_64_PC64  24

static struct module *modules;
static spinlock_t module_lock = SPINLOCK_INIT;

const struct ksym *ksym_lookup(const char *name)
{
    for (const struct ksym *k = __ksymtab_start; k < __ksymtab_end; k++)
        if (strcmp(k->name, name) == 0)
            return k;
    return NULL;
}

void module_init_ksyms(void)
{
    kprintf("modules: %lu exported kernel symbols\n", (unsigned long)(__ksymtab_end - __ksymtab_start));
}

static int apply_rela(uint8_t *base_of_target, const struct elf_rela *r, uint64_t symval, const char *symname)
{
    uint8_t *P = base_of_target + r->offset;
    uint64_t S = symval;
    int64_t  A = r->addend;
    uint32_t type = (uint32_t)(r->info & 0xFFFFFFFF);
    int64_t val;

    switch (type) {
    case R_X86_64_NONE:
        return 0;
    case R_X86_64_64:
        *(uint64_t *)P = S + (uint64_t)A;
        return 0;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        val = (int64_t)(S + (uint64_t)A) - (int64_t)(uint64_t)P;
        if (val != (int32_t)val) goto range;
        *(int32_t *)P = (int32_t)val;
        return 0;
    case R_X86_64_32:
        val = (int64_t)(S + (uint64_t)A);
        if ((uint64_t)val >> 32) goto range;
        *(uint32_t *)P = (uint32_t)val;
        return 0;
    case R_X86_64_32S:
        val = (int64_t)(S + (uint64_t)A);
        if (val != (int32_t)val) goto range;
        *(int32_t *)P = (int32_t)val;
        return 0;
    case R_X86_64_PC64:
        *(uint64_t *)P = S + (uint64_t)A - (uint64_t)P;
        return 0;
    default:
        kprintf("module: unsupported relocation type %u against %s\n", type, symname);
        return -ENOEXEC;
    }
range:
    kprintf("module: relocation against %s out of range\n", symname);
    return -ENOEXEC;
}

int module_load(const void *image, size_t len, const char *params)
{
    (void)params;
    const uint8_t *img = image;
    const struct elf_ehdr *eh = image;

    if (len < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != 2)
        return -ENOEXEC;
    if (eh->type != ET_REL || eh->machine != EM_X86_64 || eh->shentsize != sizeof(struct elf_shdr))
        return -ENOEXEC;
    if (eh->shoff + (uint64_t)eh->shnum * sizeof(struct elf_shdr) > len)
        return -ENOEXEC;

    const struct elf_shdr *sh = (const void *)(img + eh->shoff);
    for (uint16_t i = 0; i < eh->shnum; i++)
        if (sh[i].type != SHT_NOBITS && sh[i].offset + sh[i].size > len)
            return -ENOEXEC;

    /* Lay out the allocatable sections back to back. */
    uint64_t *secaddr = kzalloc(sizeof(uint64_t) * eh->shnum);
    if (!secaddr)
        return -ENOMEM;
    uint64_t total = 0;
    for (uint16_t i = 0; i < eh->shnum; i++) {
        if (!(sh[i].flags & SHF_ALLOC) || sh[i].size == 0)
            continue;
        uint64_t align = sh[i].addralign ? sh[i].addralign : 1;
        total = (total + align - 1) & ~(align - 1);
        secaddr[i] = total;                 /* offset for now */
        total += sh[i].size;
    }
    size_t pages = PAGE_ALIGN_UP(total) / PAGE_SIZE;
    uint64_t base = pages ? pmm_alloc_pages_below(pages, MODULE_ADDR_LIMIT) : 0;
    if (pages && !base) {
        kfree(secaddr);
        return -ENOMEM;
    }
    uint8_t *mem = (uint8_t *)base;         /* identity mapped */
    memset(mem, 0, pages * PAGE_SIZE);
    for (uint16_t i = 0; i < eh->shnum; i++) {
        if (!(sh[i].flags & SHF_ALLOC) || sh[i].size == 0)
            continue;
        secaddr[i] += base;
        if (sh[i].type != SHT_NOBITS)
            memcpy((void *)secaddr[i], img + sh[i].offset, sh[i].size);
    }

    /* Resolve symbols. */
    int rc = -ENOEXEC;
    const struct elf_shdr *symtab = NULL;
    for (uint16_t i = 0; i < eh->shnum; i++)
        if (sh[i].type == SHT_SYMTAB)
            symtab = &sh[i];
    if (!symtab || symtab->link >= eh->shnum) {
        kprintf("module: no symbol table\n");
        goto fail;
    }
    const struct elf_sym *syms = (const void *)(img + symtab->offset);
    size_t nsyms = symtab->size / sizeof(struct elf_sym);
    const char *strtab = (const char *)(img + sh[symtab->link].offset);
    uint64_t *symval = kzalloc(sizeof(uint64_t) * nsyms);
    if (!symval) {
        rc = -ENOMEM;
        goto fail;
    }
    const char *modname = NULL;
    int (*init)(void) = NULL;
    void (*exit)(void) = NULL;
    for (size_t i = 1; i < nsyms; i++) {
        const char *name = strtab + syms[i].name;
        if (syms[i].shndx == SHN_UNDEF) {
            const struct ksym *k = ksym_lookup(name);
            if (!k) {
                kprintf("module: unresolved symbol '%s'\n", name);
                kfree(symval);
                goto fail;
            }
            symval[i] = (uint64_t)k->addr;
        } else if (syms[i].shndx == SHN_ABS) {
            symval[i] = syms[i].value;
        } else if (syms[i].shndx == SHN_COMMON || syms[i].shndx >= eh->shnum) {
            kprintf("module: unsupported symbol '%s' (shndx %u); build with -fno-common\n", name, syms[i].shndx);
            kfree(symval);
            goto fail;
        } else {
            symval[i] = secaddr[syms[i].shndx] + syms[i].value;
        }
        if (strcmp(name, "module_name") == 0) modname = (const char *)symval[i];
        if (strcmp(name, "init_module") == 0) init = (int (*)(void))symval[i];
        if (strcmp(name, "cleanup_module") == 0) exit = (void (*)(void))symval[i];
    }

    /* Relocate. */
    for (uint16_t i = 0; i < eh->shnum; i++) {
        if (sh[i].type != SHT_RELA || sh[i].info >= eh->shnum)
            continue;
        if (!(sh[sh[i].info].flags & SHF_ALLOC))
            continue;                       /* .rela.debug_* and friends */
        const struct elf_rela *rel = (const void *)(img + sh[i].offset);
        size_t n = sh[i].size / sizeof(*rel);
        for (size_t j = 0; j < n; j++) {
            uint32_t si = (uint32_t)(rel[j].info >> 32);
            if (si >= nsyms || rel[j].offset + 8 > sh[sh[i].info].size + 4) {
                kfree(symval);
                goto fail;
            }
            if (apply_rela((uint8_t *)secaddr[sh[i].info], &rel[j], symval[si], strtab + syms[si].name) != 0) {
                kfree(symval);
                goto fail;
            }
        }
    }
    kfree(symval);

    if (!modname || !init) {
        kprintf("module: missing module_name or init_module\n");
        goto fail;
    }

    struct module *m = kzalloc(sizeof(*m));
    if (!m) {
        rc = -ENOMEM;
        goto fail;
    }
    size_t nl = strlen(modname);
    memcpy(m->name, modname, nl < sizeof(m->name) - 1 ? nl : sizeof(m->name) - 1);
    m->base = mem;
    m->size = pages * PAGE_SIZE;
    m->init = init;
    m->exit = exit;

    spin_lock(&module_lock);
    struct module *dup = modules;
    while (dup && strcmp(dup->name, m->name) != 0)
        dup = dup->next;
    if (dup) {
        spin_unlock(&module_lock);
        kfree(m);
        rc = -EEXIST;
        goto fail;
    }
    m->next = modules;
    modules = m;
    spin_unlock(&module_lock);

    rc = m->init();
    if (rc != 0) {
        kprintf("module: %s: init failed (%d)\n", m->name, rc);
        module_unload(m->name);
        kfree(secaddr);
        return rc < 0 ? rc : -EINVAL;
    }
    kprintf("module: loaded %s at %p (%lu KiB)\n", m->name, mem, m->size / 1024);
    kfree(secaddr);
    return 0;

fail:
    if (pages)
        pmm_free_pages(base, pages);
    kfree(secaddr);
    return rc;
}

int module_unload(const char *name)
{
    spin_lock(&module_lock);
    struct module **pp = &modules;
    while (*pp && strcmp((*pp)->name, name) != 0)
        pp = &(*pp)->next;
    struct module *m = *pp;
    if (m)
        *pp = m->next;
    spin_unlock(&module_lock);
    if (!m)
        return -ENOENT;
    if (m->exit)
        m->exit();
    pmm_free_pages((uint64_t)m->base, m->size / PAGE_SIZE);
    kprintf("module: unloaded %s\n", m->name);
    kfree(m);
    return 0;
}

long module_list(char *buf, size_t len)
{
    size_t used = 0;
    spin_lock(&module_lock);
    for (struct module *m = modules; m; m = m->next) {
        char line[96];
        size_t n = 0;
        const char *s = m->name;
        while (*s) line[n++] = *s++;
        line[n++] = ' ';
        static const char hex[] = "0123456789abcdef";
        line[n++] = '0'; line[n++] = 'x';
        for (int i = 7; i >= 0; i--) line[n++] = hex[((uint64_t)m->base >> (i * 4)) & 0xF];
        line[n++] = ' ';
        char num[24]; int k = 0; size_t v = m->size;
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) line[n++] = num[--k];
        line[n++] = '\n';
        if (used + n > len) break;
        memcpy(buf + used, line, n);
        used += n;
    }
    spin_unlock(&module_lock);
    return (long)used;
}
