/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* OpenFirmware client interface (IEEE 1275). Used only while booting: the
 * device tree tells us where RAM, the framebuffer, the PCI bridges and the
 * interrupt controller are; the firmware console prints until our own
 * drivers are up. Every pointer handed to the firmware is a physical
 * address: it runs with its own 1:1 translations, we run in the direct map. */
#include "asm/of.h"
#include "asm/memlayout.h"
#include "string.h"

typedef int (*of_entry_t)(void *args);
static of_entry_t of_entry;

static uint32_t phys(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return v >= HHDM_BASE ? (uint32_t)(v - HHDM_BASE) : (uint32_t)v;
}

/* One call: service name, argument cells, result cells. */
static int of_call(const char *service, int nargs, int nret, const uint32_t *args, uint32_t *rets)
{
    struct { uint32_t service, nargs, nret, a[12]; } cell;
    cell.service = phys(service);
    cell.nargs = (uint32_t)nargs;
    cell.nret = (uint32_t)nret;
    for (int i = 0; i < nargs; i++) cell.a[i] = args[i];
    for (int i = 0; i < nret; i++) cell.a[nargs + i] = 0;
    if (!of_entry || of_entry((void *)phys(&cell)) != 0)
        return -1;
    for (int i = 0; i < nret; i++) rets[i] = cell.a[nargs + i];
    return 0;
}

void of_init(void *entry) { of_entry = entry; }

uint32_t of_finddevice(const char *path)
{
    uint32_t a[1] = { phys(path) }, r[1];
    if (of_call("finddevice", 1, 1, a, r) != 0) return 0;
    return r[0] == 0xFFFFFFFF ? 0 : r[0];
}

int of_getprop(uint32_t ph, const char *name, void *buf, uint32_t len)
{
    uint32_t a[4] = { ph, phys(name), phys(buf), len }, r[1];
    if (of_call("getprop", 4, 1, a, r) != 0) return -1;
    return (int)r[0];
}

uint32_t of_getprop_u32(uint32_t ph, const char *name, uint32_t dflt)
{
    uint32_t v;
    return of_getprop(ph, name, &v, 4) == 4 ? v : dflt;
}

uint32_t of_child(uint32_t ph) { uint32_t a[1] = { ph }, r[1]; return of_call("child", 1, 1, a, r) == 0 ? r[0] : 0; }
uint32_t of_peer(uint32_t ph)  { uint32_t a[1] = { ph }, r[1]; return of_call("peer", 1, 1, a, r) == 0 ? r[0] : 0; }
uint32_t of_parent(uint32_t ph) { uint32_t a[1] = { ph }, r[1]; return of_call("parent", 1, 1, a, r) == 0 ? r[0] : 0; }

uint32_t of_instance_to_package(uint32_t ih)
{
    uint32_t a[1] = { ih }, r[1];
    if (of_call("instance-to-package", 1, 1, a, r) != 0) return 0;
    return r[0] == 0xFFFFFFFF ? 0 : r[0];
}

int of_package_to_path(uint32_t ph, char *buf, uint32_t len)
{
    uint32_t a[3] = { ph, phys(buf), len }, r[1];
    if (of_call("package-to-path", 3, 1, a, r) != 0) return -1;
    return (int)r[0];
}

int of_nextprop(uint32_t ph, const char *prev, char *buf)
{
    uint32_t a[3] = { ph, phys(prev), phys(buf) }, r[1];
    if (of_call("nextprop", 3, 1, a, r) != 0) return -1;
    return (int)r[0];
}

void of_write(uint32_t ih, const char *s, uint32_t len)
{
    uint32_t a[3] = { ih, phys(s), len }, r[1];
    of_call("write", 3, 1, a, r);
}

/* Firmware memory: claim `size` bytes at physical `addr` (0 = anywhere). */
uint32_t of_claim(uint32_t addr, uint32_t size, uint32_t align)
{
    uint32_t a[3] = { addr, size, align }, r[1];
    if (of_call("claim", 3, 1, a, r) != 0) return 0xFFFFFFFF;
    return r[0];
}

void of_quiesce(void)
{
    uint32_t r[1];
    of_call("quiesce", 0, 0, NULL, r);
}

void of_exit(void)
{
    uint32_t r[1];
    of_call("exit", 0, 0, NULL, r);
    for (;;) ;
}

/* Find the first node named `name` (property "name") below `root`, depth first. */
uint32_t of_find_by_name(uint32_t root, const char *name)
{
    for (uint32_t n = of_child(root); n; n = of_peer(n)) {
        char buf[32] = { 0 };
        if (of_getprop(n, "name", buf, sizeof buf - 1) > 0 && strcmp(buf, name) == 0)
            return n;
        uint32_t deep = of_find_by_name(n, name);
        if (deep)
            return deep;
    }
    return 0;
}

uint32_t of_find_by_type(uint32_t root, const char *type)
{
    for (uint32_t n = of_child(root); n; n = of_peer(n)) {
        char buf[32] = { 0 };
        if (of_getprop(n, "device_type", buf, sizeof buf - 1) > 0 && strcmp(buf, type) == 0)
            return n;
        uint32_t deep = of_find_by_type(n, type);
        if (deep)
            return deep;
    }
    return 0;
}
