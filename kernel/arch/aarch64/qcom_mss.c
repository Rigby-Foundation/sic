/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A Qualcomm phone's modem (MPSS, a Hexagon DSP), booted the way Linux's
 * qcom_q6v5_pas does: its signed image (modem.mdt + modem.bNN, from the
 * phone's modem partition, which init mounts on /mnt/modem) goes into its
 * reserved memory, and TrustZone checks it and lets it run. On these SoCs
 * the WLAN firmware runs in it, so it is the Wi-Fi's first step.
 *   echo check > /dev/mss    parse the image, touch nothing
 *   echo start > /dev/mss    load it and start the modem
 *   cat /dev/mss             what it is doing (its SMP2P bits, a crash reason)
 * Nothing starts it at boot. ("check" works anywhere, a VM too.) */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/qcom_scm.h"
#include "asm/qcom_smem.h"
#include "asm/timer.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "proc/sched.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"

#define FW_DIR          "/mnt/modem/image/"
#define MSS_PAS_ID      4
#define CRASH_REASON    421             /* SMEM item: the modem's last words */
#define SMP2P_IN_ITEM   428             /* the modem's SMP2P entries for us */
#define MAX_PHDRS       64

#define PT_LOAD             1
#define MDT_TYPE_MASK       (7u << 24)
#define MDT_TYPE_HASH       (2u << 24)
#define MDT_RELOCATABLE     (1u << 27)

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static uint64_t region, region_size;    /* modem_region */
static uint64_t meta_phys;              /* a no-map page run for the metadata */
static int present;
static char state[96] = "stopped";
static int busy;

static const char *pas_state(void) { return state; }

/* ---- the image ------------------------------------------------------------------------ */

static uint8_t *read_file(const char *name, size_t *len)
{
    char path[96];
    ksnprintf(path, sizeof path, FW_DIR "%s", name);
    struct vnode *v = vfs_lookup(vfs_root(), path);
    return v ? vfs_read_all(v, len) : NULL;
}

static int file_size(const char *path, uint64_t *size)
{
    struct vnode *v = vfs_lookup(vfs_root(), path);
    if (!v) return -1;
    struct stat st;
    vnode_stat(v, &st);
    *size = st.size;
    return 0;
}

static int loadable(const struct phdr *p)
{
    return p->type == PT_LOAD && (p->flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && p->memsz;
}

struct image {
    uint8_t *mdt;
    size_t mdt_len, meta_len;
    struct phdr ph[MAX_PHDRS];
    int nph;
    uint64_t min, max;
    int relocatable;
};

/* The headers, and the metadata TrustZone wants: the ELF header and
 * program headers (the first segment), then the hash segment. A split
 * image's .mdt is exactly those two. */
static int parse(struct image *im)
{
    im->mdt = read_file("modem.mdt", &im->mdt_len);
    if (!im->mdt) { ksnprintf(state, sizeof state, "no " FW_DIR "modem.mdt (is the modem partition mounted?)"); return -1; }
    const uint8_t *e = im->mdt;
    if (im->mdt_len < 52 || memcmp(e, "\177ELF", 4) || e[4] != 1) { ksnprintf(state, sizeof state, "modem.mdt is not a 32-bit ELF"); return -1; }
    uint32_t phoff; uint16_t phentsize, phnum;
    memcpy(&phoff, e + 28, 4); memcpy(&phentsize, e + 42, 2); memcpy(&phnum, e + 44, 2);
    if (phentsize != sizeof(struct phdr) || phnum > MAX_PHDRS || phoff + (uint64_t)phnum * phentsize > im->mdt_len) {
        ksnprintf(state, sizeof state, "modem.mdt: bad program headers"); return -1;
    }
    im->nph = phnum;
    memcpy(im->ph, e + phoff, phnum * sizeof(struct phdr));
    int hash = -1;
    im->min = ~0ull; im->max = 0; im->relocatable = 0;
    for (int i = 0; i < phnum; i++) {
        const struct phdr *p = &im->ph[i];
        if ((p->flags & MDT_TYPE_MASK) == MDT_TYPE_HASH && hash < 0) hash = i;
        if (!loadable(p)) continue;
        if (p->flags & MDT_RELOCATABLE) im->relocatable = 1;
        if (p->paddr < im->min) im->min = p->paddr;
        if (p->paddr + (uint64_t)p->memsz > im->max) im->max = p->paddr + (uint64_t)p->memsz;
    }
    if (hash < 0 || im->min >= im->max) { ksnprintf(state, sizeof state, "modem.mdt: no hash segment or nothing to load"); return -1; }
    im->meta_len = im->ph[0].filesz + im->ph[hash].filesz;
    if (im->meta_len != im->mdt_len) { ksnprintf(state, sizeof state, "modem.mdt: %lu bytes, not headers + hash (%lu)", (unsigned long)im->mdt_len, (unsigned long)im->meta_len); return -1; }
    uint64_t base = im->relocatable ? im->min : region;
    if (region_size && (im->max - base > region_size || (!im->relocatable && im->min < region))) {
        ksnprintf(state, sizeof state, "the image (%lx-%lx) does not fit modem_region", (unsigned long)im->min, (unsigned long)im->max); return -1;
    }
    for (int i = 0; i < phnum; i++) {               /* every segment with bytes has its file, of at least that size */
        if (!loadable(&im->ph[i]) || !im->ph[i].filesz) continue;
        char path[96]; uint64_t sz;
        ksnprintf(path, sizeof path, FW_DIR "modem.b%02d", i);
        if (file_size(path, &sz) || sz < im->ph[i].filesz) { ksnprintf(state, sizeof state, "modem.b%02d missing or short", i); return -1; }
    }
    return 0;
}

/* Device memory takes whole aligned words only. */
static void put_words(volatile uint32_t *dst, const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        memcpy(&w, src + i, len - i < 4 ? len - i : 4);
        dst[i / 4] = w;
    }
}

static int load_segment(volatile uint8_t *mem, const struct image *im, int i, uint8_t *buf, size_t bufsz)
{
    const struct phdr *p = &im->ph[i];
    volatile uint8_t *dst = mem + (p->paddr - (im->relocatable ? im->min : region));
    size_t done = 0;
    if (p->filesz) {
        char path[96];
        ksnprintf(path, sizeof path, FW_DIR "modem.b%02d", i);
        struct file *f = vfs_open(vfs_root(), path, O_RDONLY);
        if (!f) return -1;
        while (done < p->filesz) {
            size_t want = p->filesz - done < bufsz ? p->filesz - done : bufsz;
            long n = file_read(f, buf, want);
            if (n <= 0) { file_close(f); return -1; }
            put_words((volatile uint32_t *)(dst + done), buf, (size_t)n);
            done += (size_t)n;
            if (n & 3) { file_close(f); return -1; }        /* only the last read may end off a word */
        }
        file_close(f);
    }
    for (size_t z = (done + 3) & ~(size_t)3; z < p->memsz; z += 4)
        *(volatile uint32_t *)(dst + z) = 0;
    return 0;
}

static int check(void)
{
    struct image im;
    int rc = parse(&im);
    if (im.mdt) kfree(im.mdt);
    if (rc == 0) {
        int segs = 0;
        uint64_t bytes = 0;
        for (int i = 0; i < im.nph; i++) if (loadable(&im.ph[i])) { segs++; bytes += im.ph[i].filesz; }
        ksnprintf(state, sizeof state, "image ok: %d segments, %lu KiB, %lx-%lx%s", segs, (unsigned long)(bytes >> 10),
                  (unsigned long)im.min, (unsigned long)im.max, im.relocatable ? ", relocatable" : "");
    }
    return rc;
}

/* ---- starting it ------------------------------------------------------------------------ */

static void start_thread(void *arg)
{
    (void)arg;
    struct image im;
    uint8_t *buf = NULL;
    if (parse(&im) != 0) goto out;
    kprintf("mss: %s\n", "loading the modem's image");
    struct scm_res r = qcom_scm_call(SCM_PIL, 7, 1, MSS_PAS_ID, 0, 0);          /* IS_SUPPORTED */
    if (r.a0 || !r.a1) { ksnprintf(state, sizeof state, "TrustZone does not take PAS %d (%ld/%lu)", MSS_PAS_ID, (long)r.a0, (unsigned long)r.a1); goto out; }

    /* The metadata into no-map memory, mapped as device memory: TrustZone
     * locks it while it reads it, and a CPU access then (a speculative one
     * through a cacheable mapping too) is fatal. */
    volatile uint32_t *meta = vmm_map_mmio(meta_phys, PAGE_ALIGN_UP(im.meta_len));
    volatile uint8_t *mem = vmm_map_mmio(region, region_size);
    if (!meta || !mem) { ksnprintf(state, sizeof state, "cannot map the modem's memory"); goto out; }
    put_words(meta, im.mdt, im.meta_len);
    __asm__ volatile("dsb sy" ::: "memory");
    r = qcom_scm_call(SCM_PIL, 1, SCM_ARGS(2, 0, SCM_ARG_RW, 0), MSS_PAS_ID, meta_phys, 0);   /* INIT_IMAGE */
    if (r.a0 || r.a1) { ksnprintf(state, sizeof state, "init image: %ld/%lu", (long)r.a0, (unsigned long)r.a1); goto out; }
    if (im.relocatable) {
        r = qcom_scm_call(SCM_PIL, 2, 3, MSS_PAS_ID, region, im.max - im.min);          /* MEM_SETUP */
        if (r.a0 || r.a1) { ksnprintf(state, sizeof state, "mem setup: %ld/%lu", (long)r.a0, (unsigned long)r.a1); goto shutdown; }
    }

    size_t bufsz = 256 * 1024;
    buf = kmalloc(bufsz);
    if (!buf) goto shutdown;
    uint64_t t0 = timer_ms();
    for (int i = 0; i < im.nph; i++) {
        if (!loadable(&im.ph[i])) continue;
        ksnprintf(state, sizeof state, "loading modem.b%02d", i);
        if (load_segment(mem, &im, i, buf, bufsz) != 0) { ksnprintf(state, sizeof state, "modem.b%02d did not load", i); goto shutdown; }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    kprintf("mss: image loaded in %lu ms; authenticating\n", (unsigned long)(timer_ms() - t0));
    ksnprintf(state, sizeof state, "authenticating");
    r = qcom_scm_call(SCM_PIL, 5, 1, MSS_PAS_ID, 0, 0);                             /* AUTH_AND_RESET */
    if (r.a0 || r.a1) { ksnprintf(state, sizeof state, "auth and reset: %ld/%lu", (long)r.a0, (unsigned long)r.a1); goto shutdown; }
    ksnprintf(state, sizeof state, "running");
    kprintf("mss: the modem runs\n");
    goto out;
shutdown:
    qcom_scm_call(SCM_PIL, 6, 1, MSS_PAS_ID, 0, 0);                                 /* SHUTDOWN: TrustZone lets go of the memory */
    kprintf("mss: %s\n", state);
out:
    if (im.mdt) kfree(im.mdt);
    if (buf) kfree(buf);
    __atomic_store_n(&busy, 0, __ATOMIC_SEQ_CST);
}

/* ---- /dev/mss ------------------------------------------------------------------------------ */

static char report[1024];

static void smp2p_report(size_t *n)
{
    size_t sz;
    const volatile uint8_t *it = smem_get(1, SMP2P_IN_ITEM, &sz);
    if (!it || sz < 20 || mmio_read32(it) != 0x504d5324) { *n += (size_t)ksnprintf(report + *n, sizeof report - *n, "smp2p from the modem: none yet\n"); return; }
    uint32_t w = mmio_read32(it + 12);
    unsigned valid = w >> 16;
    for (unsigned i = 0; i < valid && 20 + 20 * (i + 1) <= sz; i++) {
        char name[17];
        for (int k = 0; k < 16; k += 4) { uint32_t c = mmio_read32(it + 20 + 20 * i + k); memcpy(name + k, &c, 4); }
        name[16] = 0;
        uint32_t v = mmio_read32(it + 20 + 20 * i + 16);
        *n += (size_t)ksnprintf(report + *n, sizeof report - *n, "smp2p %s: %08x%s%s%s%s\n", name, v,
                                strcmp(name, "slave-kernel") ? "" : v & 1 ? " FATAL" : "", strcmp(name, "slave-kernel") ? "" : v & 2 ? " ready" : "",
                                strcmp(name, "slave-kernel") ? "" : v & 4 ? " handover" : "", strcmp(name, "slave-kernel") ? "" : v & 8 ? " stop-ack" : "");
    }
}

static size_t make_report(void)
{
    size_t n = (size_t)ksnprintf(report, sizeof report, "modem: %s\n", pas_state());
    smp2p_report(&n);
    size_t sz;
    const volatile uint8_t *cr = smem_get(SMEM_GLOBAL_HOST, CRASH_REASON, &sz);
    if (cr && sz && n < sizeof report - 8) {
        n += (size_t)ksnprintf(report + n, sizeof report - n, "crash reason: ");
        for (size_t i = 0; i < sz && n < sizeof report - 2; i++) {
            char c = (char)cr[i];
            if (!c) break;
            report[n++] = c >= 32 && c < 127 ? c : '.';
        }
        report[n++] = '\n';
        report[n] = 0;
    }
    return n;
}

static long mss_read(struct file *f, void *buf, size_t len)
{
    if (f->pos == 0) make_report();
    size_t n = strlen(report);
    if (f->pos >= n) return 0;
    if (len > n - f->pos) len = n - f->pos;
    memcpy(buf, report + f->pos, len);
    f->pos += len;
    return (long)len;
}

static long mss_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    char cmd[16] = {0};
    memcpy(cmd, buf, len < sizeof cmd - 1 ? len : sizeof cmd - 1);
    for (int i = 0; cmd[i]; i++) if (cmd[i] == '\n') cmd[i] = 0;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_SEQ_CST)) return -EBUSY;
    if (strcmp(cmd, "check") == 0) {
        check();
        busy = 0;
        return (long)len;
    }
    if (strcmp(cmd, "start") == 0) {
        if (strcmp(state, "running") == 0) { busy = 0; return -EBUSY; }
        if (!present) { ksnprintf(state, sizeof state, "no modem here"); busy = 0; return -ENODEV; }
        ksnprintf(state, sizeof state, "starting");
        task_create("mss-start", start_thread, NULL);
        return (long)len;
    }
    busy = 0;
    return -EINVAL;
}

static const struct dev_ops mss_ops = { .read = mss_read, .write = mss_write };

static uint64_t phandle_region(int node, const char *prop, uint64_t *size)
{
    uint32_t ph = node >= 0 ? fdt_prop_u32(node, prop, 0, 0) : 0;
    uint64_t base = 0;
    for (int m = fdt_path("/"); ph && m >= 0; m = fdt_walk_next(m))
        if (fdt_prop_u32(m, "phandle", 0, 0) == ph) { fdt_reg(m, 0, &base, size); break; }
    return base;
}

void qcom_mss_init(void)
{
    int n = fdt_find_compatible("qcom,khaje-modem-pas");
    if (n < 0) n = fdt_find_compatible("qcom,bengal-modem-pas");
    if (n >= 0) region = phandle_region(n, "memory-region", &region_size);
    /* The metadata (9 KiB) goes at 1 MiB into the video firmware's no-map
     * region, which sic never uses (the zap shader's metadata is at its start). */
    for (int m = fdt_path("/"); m >= 0; m = fdt_walk_next(m))
        if (memcmp(fdt_name(m), "video_region@", 13) == 0 && fdt_prop(m, "no-map", NULL)) {
            uint64_t b, s;
            if (fdt_reg(m, 0, &b, &s) == 0 && s >= 0x200000) meta_phys = b + 0x100000;
            break;
        }
    present = region && region_size && meta_phys && !(meta_phys & (PAGE_SIZE - 1));
    vfs_mkdev("/dev/mss", &mss_ops, NULL);
    kprintf("mss: modem_region %lx+%lx%s\n", (unsigned long)region, (unsigned long)region_size, present ? "" : " (incomplete: it cannot start)");
}
