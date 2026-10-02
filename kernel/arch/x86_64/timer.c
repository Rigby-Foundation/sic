/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "asm/timer.h"
#include "asm/idt.h"
#include "asm/apic.h"
#include "asm/pit.h"
#include "proc/sched.h"
#include "core/prof.h"
#include "printf.h"
#include "asm/cpu.h"
#include "asm/io.h"

static volatile uint64_t ticks;

static void timer_irq(struct interrupt_frame *f)
{
    (void)f;
    if (this_cpu()->index == 0)
        ticks++;
    prof_tick(f);
    sched_tick();
}

/* ---- the CMOS clock: the date at boot --------------------------------------------- */

static uint64_t boot_epoch;             /* seconds since 1970 when timer_ms() was 0 */

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

static uint64_t days_from_civil(int y, unsigned m, unsigned d)     /* Howard Hinnant's */
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)((int64_t)era * 146097 + (int64_t)doe - 719468);
}

/* The time as the firmware keeps it (local time on a PC that ran Windows;
 * sic has no time zones, so that is what it shows). Read until two reads
 * agree, outside an update. */
static void rtc_read(void)
{
    uint8_t r[6], again[6];
    for (int tries = 0; tries < 10; tries++) {
        for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) ;
        r[0] = cmos(0x00); r[1] = cmos(0x02); r[2] = cmos(0x04); r[3] = cmos(0x07); r[4] = cmos(0x08); r[5] = cmos(0x09);
        for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) ;
        again[0] = cmos(0x00); again[1] = cmos(0x02); again[2] = cmos(0x04); again[3] = cmos(0x07); again[4] = cmos(0x08); again[5] = cmos(0x09);
        int same = 1;
        for (int i = 0; i < 6; i++) if (r[i] != again[i]) same = 0;
        if (same) break;
    }
    uint8_t b = cmos(0x0B);
    int pm = r[2] & 0x80;
    r[2] &= 0x7F;
    if (!(b & 0x04))                                        /* BCD */
        for (int i = 0; i < 6; i++) r[i] = (uint8_t)((r[i] & 0x0F) + (r[i] >> 4) * 10);
    if (!(b & 0x02) && pm) r[2] = (uint8_t)((r[2] + 12) % 24);   /* 12-hour clock */
    if (r[4] < 1 || r[4] > 12 || r[3] < 1 || r[3] > 31) return;  /* no sensible clock */
    uint64_t days = days_from_civil(2000 + r[5], r[4], r[3]);
    uint64_t now = days * 86400 + (uint64_t)r[2] * 3600 + (uint64_t)r[1] * 60 + r[0];
    boot_epoch = now - timer_ms() / 1000;
    kprintf("rtc: 20%02u-%02u-%02u %02u:%02u:%02u\n", r[5], r[4], r[3], r[2], r[1], r[0]);
}

uint64_t timer_boot_epoch(void) { return boot_epoch; }

void timer_init(void)
{
    rtc_read();
    irq_install(0, timer_irq);
    if (apic_enabled()) {
        lapic_timer_init(IRQ_BASE + 0, TIMER_HZ);
    } else {
        pit_start_periodic(TIMER_HZ);
        irq_unmask(0);
    }
    kprintf("timer: %u Hz via %s\n", TIMER_HZ, apic_enabled() ? "lapic" : "pit");
}

uint64_t timer_ticks(void) { return ticks; }
uint64_t timer_ms(void)    { return ticks * 1000 / TIMER_HZ; }

const char *timer_source(void)
{
    return apic_enabled() ? "lapic" : "pit";
}
