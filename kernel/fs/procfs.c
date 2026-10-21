/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The Linux-compatible part of /proc: the text files that ps, top, btop,
 * free, uptime and psutil read, in Linux's formats.
 *
 *   /proc/stat /meminfo /uptime /loadavg /cpuinfo /version
 *   /proc/<pid>/stat /statm /status /comm /cmdline, and /proc/self
 *
 * /proc itself stays the in-memory directory it was (kmsg, prof, sysfail, ...
 * are device files in it); procfs_init() gives it a lookup and a readdir that
 * also answer numbers and "self". Process directories are not entries in the
 * directory cache, so a dead process cannot linger there: they are kept in a
 * table, reused per pid, and freed once the process is gone and nothing uses
 * them. Every file is generated on read; times are in clock ticks of 100 Hz
 * (AT_CLKTCK, sysconf(_SC_CLK_TCK)). */
#include "fs/vfs.h"
#include "proc/sched.h"
#include "proc/mm.h"
#include "proc/elf.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "asm/timer.h"
#include "asm/smp.h"
#include "asm/cpu.h"
#include "string.h"
#include "printf.h"

#define USER_HZ       100
#define TO_CLK(ticks) ((ticks) / (TIMER_HZ / USER_HZ))
#define PROC_MAX      512                 /* processes listed at once */
#define TEXT_MAX      16384               /* the longest generated file */

static struct vnode *proc_root;
static struct vnode_ops proc_root_ops;    /* tmpfs's, with lookup and readdir replaced */
static const struct vnode_ops *tmpfs_ops;

/* ---- text generation --------------------------------------------------------- */

struct text { char *buf; size_t len, cap; };

static void put(struct text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(struct text *t, const char *fmt, ...)
{
    if (t->len >= t->cap) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = kvsnprintf(t->buf + t->len, t->cap - t->len, fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) t->len += (size_t)n < t->cap - t->len ? (size_t)n : t->cap - t->len - 1;
}

/* A read of a generated file: build the whole text, hand out the part at f->pos. */
static long serve(struct file *f, void *buf, size_t len, void (*gen)(struct text *, uint32_t), uint32_t pid)
{
    struct text t = { kmalloc(TEXT_MAX), 0, TEXT_MAX };
    if (!t.buf) return -ENOMEM;
    gen(&t, pid);
    long n = 0;
    if (f->pos < t.len) {
        n = (long)(t.len - f->pos < len ? t.len - f->pos : len);
        memcpy(buf, t.buf + f->pos, (size_t)n);
        f->pos += (uint64_t)n;
    }
    kfree(t.buf);
    return n;
}

static void gen_stat(struct text *t, uint32_t unused)
{
    (void)unused;
    int ncpu = (int)smp_cpu_count();
    struct cpu_times c[MAX_CPUS], sum = { 0, 0, 0 };
    sched_cpu_times(c, ncpu);
    for (int i = 0; i < ncpu; i++) { sum.user += c[i].user; sum.system += c[i].system; sum.idle += c[i].idle; }
    put(t, "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n", (unsigned long long)TO_CLK(sum.user),
        (unsigned long long)TO_CLK(sum.system), (unsigned long long)TO_CLK(sum.idle));
    for (int i = 0; i < ncpu; i++)
        put(t, "cpu%d %llu 0 %llu %llu 0 0 0 0 0 0\n", i, (unsigned long long)TO_CLK(c[i].user),
            (unsigned long long)TO_CLK(c[i].system), (unsigned long long)TO_CLK(c[i].idle));
    put(t, "intr 0\nctxt 0\nbtime %llu\nprocesses %llu\nprocs_running %d\nprocs_blocked 0\n",
        (unsigned long long)timer_boot_epoch(), (unsigned long long)sched_forks(), sched_nr_running());
}

static void gen_meminfo(struct text *t, uint32_t unused)
{
    (void)unused;
    unsigned long long total = pmm_total_pages() * (PAGE_SIZE / 1024);
    unsigned long long used = pmm_used_pages() * (PAGE_SIZE / 1024);
    unsigned long long free = total > used ? total - used : 0;
    put(t, "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\n"
           "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n"
           "Active:         %8llu kB\nInactive:              0 kB\nSwapTotal:             0 kB\n"
           "SwapFree:              0 kB\nDirty:                 0 kB\nShmem:                 0 kB\n"
           "SReclaimable:          0 kB\n", total, free, free, used);
}

static void gen_uptime(struct text *t, uint32_t unused)
{
    (void)unused;
    int ncpu = (int)smp_cpu_count();
    struct cpu_times c[MAX_CPUS];
    sched_cpu_times(c, ncpu);
    uint64_t idle = 0;
    for (int i = 0; i < ncpu; i++) idle += c[i].idle;
    uint64_t ms = timer_ms(), idle_cs = idle * 100 / TIMER_HZ;
    put(t, "%llu.%02llu %llu.%02llu\n", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000 / 10),
        (unsigned long long)(idle_cs / 100), (unsigned long long)(idle_cs % 100));
}

static void gen_loadavg(struct text *t, uint32_t unused)
{
    (void)unused;
    unsigned long a[3];
    sched_loadavg(a);
    struct proc_info *list = kmalloc(sizeof(*list) * PROC_MAX);
    int n = list ? sched_proc_list(list, PROC_MAX) : 0;
    kfree(list);
    put(t, "%lu.%02lu %lu.%02lu %lu.%02lu %d/%d %u\n",
        a[0] >> 11, (a[0] & 2047) * 100 >> 11, a[1] >> 11, (a[1] & 2047) * 100 >> 11,
        a[2] >> 11, (a[2] & 2047) * 100 >> 11, sched_nr_running(), n, sched_last_pid());
}

static void gen_cpuinfo(struct text *t, uint32_t unused)
{
    (void)unused;
#if defined(__x86_64__)
    const char *arch = "x86_64";
#elif defined(__aarch64__)
    const char *arch = "aarch64";
#else
    const char *arch = "powerpc";
#endif
    for (uint32_t i = 0; i < smp_cpu_count(); i++)
        put(t, "processor\t: %u\nmodel name\t: %s processor (sic)\ncpu MHz\t\t: 0.000\nBogoMIPS\t: 0.00\n\n", i, arch);
}

static void gen_version(struct text *t, uint32_t unused)
{
    (void)unused;
    put(t, "sic version 0.1 (Rigby Foundation) #1 SMP\n");
}

/* ---- /proc/<pid>/... ---------------------------------------------------------------- */

static void gen_pid_stat(struct text *t, uint32_t pid)
{
    struct proc_info p;
    if (sched_proc_get(pid, &p) != 0) return;
    uint64_t rss = 0, vsize = 0;
    struct mm *mm = sched_proc_mm(pid);
    if (mm) { process_mem_stats(mm, &rss, &vsize); mm_put(mm); }
    /* pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt majflt
     * cmajflt utime stime cutime cstime priority nice threads itrealvalue
     * starttime vsize rss rsslim ... (52 fields in all) */
    put(t, "%u (%s) %c %u %u %u 0 -1 0 0 0 0 0 %llu %llu 0 0 20 0 %u 0 %llu %llu %llu "
           "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        p.pid, p.name, p.state, p.ppid, p.pid, p.pid,
        (unsigned long long)TO_CLK(p.utime), (unsigned long long)TO_CLK(p.stime), p.threads,
        (unsigned long long)TO_CLK(p.start_tick), (unsigned long long)vsize, (unsigned long long)rss);
}

static void gen_pid_statm(struct text *t, uint32_t pid)
{
    uint64_t rss = 0, vsize = 0;
    struct mm *mm = sched_proc_mm(pid);
    if (mm) { process_mem_stats(mm, &rss, &vsize); mm_put(mm); }
    put(t, "%llu %llu 0 0 0 %llu 0\n", (unsigned long long)(vsize / PAGE_SIZE),
        (unsigned long long)rss, (unsigned long long)rss);
}

static void gen_pid_status(struct text *t, uint32_t pid)
{
    struct proc_info p;
    if (sched_proc_get(pid, &p) != 0) return;
    uint64_t rss = 0, vsize = 0;
    struct mm *mm = sched_proc_mm(pid);
    if (mm) { process_mem_stats(mm, &rss, &vsize); mm_put(mm); }
    const char *state = p.state == 'R' ? "R (running)" : p.state == 'T' ? "T (stopped)"
                      : p.state == 'Z' ? "Z (zombie)" : "S (sleeping)";
    put(t, "Name:\t%s\nState:\t%s\nTgid:\t%u\nPid:\t%u\nPPid:\t%u\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n"
           "VmSize:\t%8llu kB\nVmRSS:\t%8llu kB\nThreads:\t%u\n",
        p.name, state, p.pid, p.pid, p.ppid, (unsigned long long)(vsize / 1024),
        (unsigned long long)(rss * (PAGE_SIZE / 1024)), p.threads);
}

static void gen_pid_comm(struct text *t, uint32_t pid)
{
    struct proc_info p;
    if (sched_proc_get(pid, &p) == 0) put(t, "%s\n", p.name);
}

static void gen_pid_cmdline(struct text *t, uint32_t pid)
{
    struct mm *mm = sched_proc_mm(pid);
    if (!mm) return;
    if (mm->arg_end > mm->arg_start) {
        size_t len = (size_t)(mm->arg_end - mm->arg_start);
        if (len > t->cap) len = t->cap;
        t->len = process_read_mem(mm, mm->arg_start, t->buf, len);   /* NUL-separated, as on Linux */
    }
    mm_put(mm);
}

/* ---- the files --------------------------------------------------------------------- */

#define PROC_FILE(fn)                                                                   \
    static long fn##_read(struct file *f, void *buf, size_t len)                       \
    { return serve(f, buf, len, gen_##fn, 0); }                                       \
    static const struct dev_ops fn##_ops = { .read = fn##_read };
PROC_FILE(stat)
PROC_FILE(meminfo)
PROC_FILE(uptime)
PROC_FILE(loadavg)
PROC_FILE(cpuinfo)
PROC_FILE(version)

/* /proc/taskdump: writing anything prints every task, its syscall and where
 * it sleeps, to the kernel log (what Scroll Lock does on a PS/2 keyboard). */
static long taskdump_write(struct file *f, const void *buf, size_t len)
{
    (void)f; (void)buf;
    sched_dump_request();
    return (long)len;
}
static const struct dev_ops taskdump_ops = { .write = taskdump_write };

static uint32_t file_pid(struct file *f) { return (uint32_t)(uintptr_t)f->node->parent->priv; }

#define PID_FILE(fn)                                                                    \
    static long pid_##fn##_read(struct file *f, void *buf, size_t len)                 \
    { return serve(f, buf, len, gen_pid_##fn, file_pid(f)); }                         \
    static const struct dev_ops pid_##fn##_ops = { .read = pid_##fn##_read };
PID_FILE(stat)
PID_FILE(statm)
PID_FILE(status)
PID_FILE(comm)
PID_FILE(cmdline)

static const struct { const char *name; const struct dev_ops *ops; } pid_files[] = {
    { "stat", &pid_stat_ops }, { "statm", &pid_statm_ops }, { "status", &pid_status_ops },
    { "comm", &pid_comm_ops }, { "cmdline", &pid_cmdline_ops },
};
#define NPID_FILES (sizeof(pid_files) / sizeof(pid_files[0]))

/* ---- process directories ------------------------------------------------------------ */

static struct vnode_ops pid_dir_ops;
static struct pid_dir { uint32_t pid; struct vnode *dir; } *pid_dirs;
static size_t npid_dirs, cap_pid_dirs;

static int alive(uint32_t pid)
{
    struct proc_info p;
    return sched_proc_get(pid, &p) == 0;
}

/* vfs_lock held: forget the directories of processes that are gone, when
 * nothing has them (or a file in them) open or as its working directory. */
static void sweep(void)
{
    for (size_t i = 0; i < npid_dirs; ) {
        struct vnode *d = pid_dirs[i].dir;
        int busy = alive(pid_dirs[i].pid) || d->open_count || sched_vnode_is_cwd(d);
        for (struct vnode *c = d->children; c && !busy; c = c->sibling)
            busy = c->open_count != 0;
        if (busy) { i++; continue; }
        for (struct vnode *c = d->children, *nx; c; c = nx) { nx = c->sibling; vnode_free(c); }
        vnode_free(d);
        pid_dirs[i] = pid_dirs[--npid_dirs];
    }
}

static struct vnode *pid_dir(uint32_t pid)
{
    if (!alive(pid)) return NULL;
    for (size_t i = 0; i < npid_dirs; i++)
        if (pid_dirs[i].pid == pid) return pid_dirs[i].dir;
    sweep();
    if (npid_dirs == cap_pid_dirs) {
        size_t cap = cap_pid_dirs ? cap_pid_dirs * 2 : 64;
        struct pid_dir *n = kmalloc(cap * sizeof(*n));
        if (!n) return NULL;
        if (pid_dirs) { memcpy(n, pid_dirs, npid_dirs * sizeof(*n)); kfree(pid_dirs); }
        pid_dirs = n;
        cap_pid_dirs = cap;
    }
    char name[16];
    int len = ksnprintf(name, sizeof name, "%u", pid);
    struct vnode *d = vnode_alloc(name, (size_t)len, VNODE_DIR, NULL);   /* not a cached entry */
    if (!d) return NULL;
    d->parent = proc_root;
    d->mnt = proc_root->mnt;
    d->ops = &pid_dir_ops;
    d->priv = (void *)(uintptr_t)pid;
    d->ino = 0x10000000ULL + pid;
    pid_dirs[npid_dirs++] = (struct pid_dir){ pid, d };
    return d;
}

static struct vnode *pid_lookup(struct vnode *dir, const char *name, size_t len)
{
    for (size_t i = 0; i < NPID_FILES; i++)
        if (strlen(pid_files[i].name) == len && memcmp(pid_files[i].name, name, len) == 0) {
            struct vnode *n = vnode_alloc(name, len, VNODE_DEV, dir);    /* cached in the pid dir */
            if (!n) return NULL;
            n->dev = pid_files[i].ops;
            n->is_proc = n->seekable = 1;      /* lseek works, as on Linux (libc++ filebuf relies on it) */
            n->ino = dir->ino * 16 + i + 1;
            return n;
        }
    return NULL;
}

static int pid_readdir(struct vnode *dir, uint64_t *pos, struct dirent *out)
{
    (void)dir;
    if (*pos >= NPID_FILES) return 0;
    memset(out, 0, sizeof(*out));
    strcpy(out->name, pid_files[*pos].name);
    out->type = VNODE_DEV;
    out->ino = (uint32_t)(dir->ino * 16 + *pos + 1);
    (*pos)++;
    return 1;
}

/* ---- /proc itself ------------------------------------------------------------------ */

static int parse_pid(const char *name, size_t len, uint32_t *pid)
{
    if (!len || len > 9) return -1;
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
        v = v * 10 + (uint32_t)(name[i] - '0');
    }
    *pid = v;
    return 0;
}

static struct vnode *root_lookup(struct vnode *dir, const char *name, size_t len)
{
    if (dir == proc_root) {
        uint32_t pid;
        if (len == 4 && memcmp(name, "self", 4) == 0)
            return pid_dir(task_current()->tgid);
        if (parse_pid(name, len, &pid) == 0)
            return pid_dir(pid);
    }
    return tmpfs_ops->lookup ? tmpfs_ops->lookup(dir, name, len) : NULL;
}

/* The static files first (tmpfs's own listing), then "self", then a
 * directory per process in pid order (the position carries the last pid). */
#define POS_PIDS (1ULL << 62)

static int root_readdir(struct vnode *dir, uint64_t *pos, struct dirent *out)
{
    if (dir != proc_root)
        return tmpfs_ops->readdir(dir, pos, out);
    if (!(*pos & POS_PIDS)) {
        if (tmpfs_ops->readdir(dir, pos, out) == 1) return 1;
        memset(out, 0, sizeof(*out));
        strcpy(out->name, "self");
        out->type = VNODE_DIR;
        *pos = POS_PIDS;
        return 1;
    }
    uint32_t last = (uint32_t)(*pos & 0xffffffffu), next = 0;
    struct proc_info *list = kmalloc(sizeof(*list) * PROC_MAX);
    if (!list) return 0;
    int n = sched_proc_list(list, PROC_MAX);
    for (int i = 0; i < n; i++)
        if (list[i].pid > last && (!next || list[i].pid < next)) next = list[i].pid;
    kfree(list);
    if (!next) return 0;
    memset(out, 0, sizeof(*out));
    ksnprintf(out->name, sizeof out->name, "%u", next);
    out->type = VNODE_DIR;
    out->ino = 0x10000000u + next;
    *pos = POS_PIDS | next;
    return 1;
}

void procfs_init(void)
{
    proc_root = vfs_lookup(vfs_root(), "/proc");
    if (!proc_root) return;
    tmpfs_ops = proc_root->ops;
    proc_root_ops = *tmpfs_ops;
    proc_root_ops.lookup = root_lookup;
    proc_root_ops.readdir = root_readdir;
    proc_root->ops = &proc_root_ops;
    pid_dir_ops = (struct vnode_ops){ .lookup = pid_lookup, .readdir = pid_readdir };

    static const struct { const char *path; const struct dev_ops *ops; } files[] = {
        { "/proc/stat", &stat_ops }, { "/proc/meminfo", &meminfo_ops }, { "/proc/uptime", &uptime_ops },
        { "/proc/loadavg", &loadavg_ops }, { "/proc/cpuinfo", &cpuinfo_ops }, { "/proc/version", &version_ops },
    };
    vfs_mkdev("/proc/taskdump", &taskdump_ops, NULL);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (vfs_mkdev(files[i].path, files[i].ops, NULL) == 0) {
            struct vnode *n = vfs_lookup(vfs_root(), files[i].path);
            if (n) n->is_proc = n->seekable = 1;
        }
}
