/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A sampling profiler out of the timer tick: which task each CPU was in,
 * in user or kernel mode, and where in the kernel. `cat /proc/prof`
 * prints the counts since the last read and clears them. Cheap enough to
 * leave on: a few stores per tick. */
#include "core/prof.h"
#include "proc/sched.h"
#include "fs/vfs.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"

#define ENTRIES 24
#define PCS     6

struct entry {
    char name[24];
    uint32_t user, kernel, sleeping;
    uint64_t pc[PCS]; uint32_t pc_hits[PCS];
};
static struct entry entries[ENTRIES];
static uint32_t idle_ticks, total_ticks;
static spinlock_t prof_lock = SPINLOCK_INIT;

void prof_tick(const struct interrupt_frame *f)
{
    struct task *t = task_current();
    if (!t) return;
    uint64_t fl = spin_lock_irqsave(&prof_lock);
    total_ticks++;
    if (memcmp(t->name, "idle/", 5) == 0) { idle_ticks++; spin_unlock_irqrestore(&prof_lock, fl); return; }
    struct entry *e = NULL;
    for (int i = 0; i < ENTRIES; i++) {
        if (entries[i].name[0] == 0) { e = &entries[i]; memcpy(e->name, t->name, sizeof e->name); break; }
        if (strcmp(entries[i].name, t->name) == 0) { e = &entries[i]; break; }
    }
    if (e) {
        if (FRAME_FROM_USER(f)) e->user++;
        else {
            e->kernel++;
            uint64_t pc = FRAME_PC(f);
            int slot = -1, least = 0;
            for (int i = 0; i < PCS; i++) {
                if (e->pc_hits[i] && e->pc[i] == pc) { slot = i; break; }
                if (!e->pc_hits[i]) { slot = i; break; }
                if (e->pc_hits[i] < e->pc_hits[least]) least = i;
            }
            if (slot < 0) { slot = least; e->pc_hits[slot] = 0; }
            e->pc[slot] = pc; e->pc_hits[slot]++;
        }
    }
    spin_unlock_irqrestore(&prof_lock, fl);
}

/* No snprintf in the kernel: a few appenders. */
static char text[4096];
static size_t tn;
static void app(const char *s) { while (*s && tn < sizeof text - 1) text[tn++] = *s++; }
static void app_pad(const char *s, size_t w) { size_t k = strlen(s); app(s); while (k++ < w && tn < sizeof text - 1) text[tn++] = ' '; }
static void app_u(uint64_t v, int hex)
{
    char b[24]; int i = 24; b[--i] = 0;
    do { b[--i] = "0123456789abcdef"[v % (hex ? 16 : 10)]; v /= hex ? 16 : 10; } while (v && i);
    app(b + i);
}

static long prof_read(struct file *f, void *buf, size_t len)
{
    if (f->pos > 0) return 0;               /* one report per open */
    tn = 0;
    uint64_t fl = spin_lock_irqsave(&prof_lock);
    app("ticks "); app_u(total_ticks, 0); app(", idle "); app_u(idle_ticks, 0); app(" ("); app_u(total_ticks ? idle_ticks * 100 / total_ticks : 0, 0); app("%)\n");
    extern unsigned long virtio_irqs;
    app("gpu: requests answered "); app_u(virtio_irqs, 0); app("\n");
    virtio_irqs = 0;
    for (int i = 0; i < ENTRIES && entries[i].name[0]; i++) {
        struct entry *e = &entries[i];
        app_pad(e->name, 16); app(" user "); app_u(e->user, 0); app("  kernel "); app_u(e->kernel, 0);
        for (int k = 0; k < PCS; k++)
            if (e->pc_hits[k]) {
                uint64_t off;
                const char *s = ksym_name(e->pc[k], &off);
                app("  "); app(s ? s : "?"); app("+"); app_u(off, 1); app(":"); app_u(e->pc_hits[k], 0);
            }
        app("\n");
        if (tn >= sizeof text - 200) break;
    }
    size_t n = tn;
    memset(entries, 0, sizeof entries);
    idle_ticks = total_ticks = 0;
    spin_unlock_irqrestore(&prof_lock, fl);
    if (n > len) n = len;
    memcpy(buf, text, n);
    f->pos = (int64_t)n;
    return (long)n;
}

/* `echo N > /proc/prof`: a report on the console every N seconds (0 stops),
 * for watching a program that has the screen. */
static volatile int period;
static void prof_thread(void *arg)
{
    (void)arg;
    for (;;) {
        task_sleep_ms(1000);
        if (!period) continue;
        static int left;
        if (++left < period) continue;
        left = 0;
        struct file dummy = { 0 };
        long n = prof_read(&dummy, text, sizeof text - 1);
        if (n > 0) { text[n] = 0; kprintf("--- /proc/prof ---\n%s", text); }
    }
}

static long prof_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    int v = 0, digits = 0;
    for (size_t i = 0; i < len && ((const char *)buf)[i] >= '0' && ((const char *)buf)[i] <= '9'; i++, digits++) v = v * 10 + (((const char *)buf)[i] - '0');
    if (digits) period = v;             /* a lone newline (echo writes it separately) changes nothing */
    return (long)len;
}

static const struct dev_ops prof_ops = { .read = prof_read, .write = prof_write };

void prof_init(void)
{
    vfs_mkdev("/proc/prof", &prof_ops, NULL);
    task_create("prof", prof_thread, NULL);
}
