/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A reader for the flattened device tree (big-endian, version 17 layout):
 * enough to find nodes by path or "compatible" and read their properties. */
#include "asm/fdt.h"
#include "string.h"

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

static const uint8_t *blob;
static const uint32_t *structs;     /* the structure block, as big-endian words */
static const char *strings;
static uint32_t struct_words, total_size;

static uint32_t be32(const void *p) { const uint8_t *b = p; return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint32_t word(int i) { return (uint32_t)i < struct_words ? be32(&structs[i]) : FDT_END; }

int fdt_init(const void *b)
{
    blob = b;
    if (!b || be32(blob) != FDT_MAGIC) return -1;
    total_size = be32(blob + 4);
    uint32_t off_struct = be32(blob + 8), off_strings = be32(blob + 12), size_struct = be32(blob + 36);
    structs = (const uint32_t *)(blob + off_struct);
    strings = (const char *)(blob + off_strings);
    struct_words = size_struct / 4;
    return 0;
}

const void *fdt_blob(void) { return blob; }
size_t fdt_size(void) { return total_size; }

/* `node` is the index of its FDT_BEGIN_NODE word. The name follows, padded to 4. */
const char *fdt_name(int node)
{
    return node < 0 ? "" : (const char *)&structs[node + 1];
}

static int after_name(int node)
{
    const char *n = fdt_name(node);
    size_t len = strlen(n) + 1;
    return node + 1 + (int)((len + 3) / 4);
}

/* Skip one property record starting at word i (which is FDT_PROP). */
static int skip_prop(int i)
{
    uint32_t len = word(i + 1);
    return i + 3 + (int)((len + 3) / 4);
}

/* Walk from the first word inside `node` (after its name) to the next
 * FDT_BEGIN_NODE at depth 1 (a child), or -1 at the node's end. */
int fdt_first_child(int node)
{
    if (node < 0) return -1;
    int i = after_name(node);
    for (;;) {
        uint32_t t = word(i);
        if (t == FDT_PROP) i = skip_prop(i);
        else if (t == FDT_NOP) i++;
        else if (t == FDT_BEGIN_NODE) return i;
        else return -1;
    }
}

/* Past `node` and everything in it. */
static int skip_node(int node)
{
    int i = after_name(node), depth = 1;
    while (depth > 0) {
        uint32_t t = word(i);
        if (t == FDT_PROP) i = skip_prop(i);
        else if (t == FDT_NOP) i++;
        else if (t == FDT_BEGIN_NODE) { i = after_name(i); depth++; }
        else if (t == FDT_END_NODE) { i++; depth--; }
        else return -1;
    }
    return i;
}

int fdt_next_sibling(int node)
{
    int i = skip_node(node);
    if (i < 0) return -1;
    for (;;) {
        uint32_t t = word(i);
        if (t == FDT_NOP) i++;
        else if (t == FDT_BEGIN_NODE) return i;
        else return -1;
    }
}

const void *fdt_prop(int node, const char *name, int *len)
{
    if (node < 0) return NULL;
    int i = after_name(node);
    for (;;) {
        uint32_t t = word(i);
        if (t == FDT_PROP) {
            uint32_t l = word(i + 1), off = word(i + 2);
            if (strcmp(strings + off, name) == 0) { if (len) *len = (int)l; return &structs[i + 3]; }
            i = skip_prop(i);
        } else if (t == FDT_NOP) i++;
        else return NULL;
    }
}

uint32_t fdt_prop_u32(int node, const char *name, int index, uint32_t dflt)
{
    int len;
    const uint8_t *p = fdt_prop(node, name, &len);
    if (!p || len < (index + 1) * 4) return dflt;
    return be32(p + index * 4);
}

uint64_t fdt_prop_u64(int node, const char *name, int index, uint64_t dflt)
{
    int len;
    const uint8_t *p = fdt_prop(node, name, &len);
    if (!p || len < (index + 2) * 4) return dflt;
    return (uint64_t)be32(p + index * 4) << 32 | be32(p + index * 4 + 4);
}

/* Node names carry a unit address ("pcie@10000000"); match up to the '@'. */
static int name_is(const char *name, const char *want, size_t wlen)
{
    return memcmp(name, want, wlen) == 0 && (name[wlen] == 0 || name[wlen] == '@');
}

int fdt_path(const char *path)
{
    if (!structs || word(0) != FDT_BEGIN_NODE) return -1;
    int node = 0;
    while (*path == '/') path++;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/') end++;
        int found = -1;
        for (int c = fdt_first_child(node); c >= 0; c = fdt_next_sibling(c))
            if (name_is(fdt_name(c), path, (size_t)(end - path))) { found = c; break; }
        if (found < 0) return -1;
        node = found;
        path = end;
        while (*path == '/') path++;
    }
    return node;
}

static int compatible(int node, const char *compat)
{
    int len;
    const char *p = fdt_prop(node, "compatible", &len);
    if (!p) return 0;
    for (int i = 0; i < len;) {
        if (strcmp(p + i, compat) == 0) return 1;
        i += (int)strlen(p + i) + 1;
    }
    return 0;
}

/* Depth-first over the whole tree from word `from`. */
static int find_compat_from(int from, const char *compat)
{
    for (int i = from; ; ) {
        uint32_t t = word(i);
        if (t == FDT_BEGIN_NODE) {
            if (compatible(i, compat)) return i;
            i = after_name(i);
        } else if (t == FDT_PROP) i = skip_prop(i);
        else if (t == FDT_NOP || t == FDT_END_NODE) i++;
        else return -1;
    }
}

int fdt_find_compatible(const char *compat) { return structs ? find_compat_from(0, compat) : -1; }
int fdt_find_compatible_after(int after, const char *compat) { return after < 0 ? -1 : find_compat_from(after_name(after), compat); }

/* The next node in structure order, at any depth (children, then what
 * follows), or -1 at the end of the tree. */
int fdt_walk_next(int node)
{
    if (node < 0) return -1;
    int i = after_name(node);
    for (;;) {
        uint32_t t = word(i);
        if (t == FDT_PROP) i = skip_prop(i);
        else if (t == FDT_NOP || t == FDT_END_NODE) i++;
        else if (t == FDT_BEGIN_NODE) return i;
        else return -1;
    }
}
