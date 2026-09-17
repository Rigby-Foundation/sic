/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include "asm/pit.h"
#include "asm/io.h"

#define PIT_CH0  0x40
#define PIT_CH2  0x42
#define PIT_CMD  0x43
#define PIT_FREQ 1193182
#define SPEAKER  0x61

void pit_start_periodic(uint32_t hz)
{
    uint16_t divisor = (uint16_t)(PIT_FREQ / hz);
    outb(PIT_CMD, 0x36);                    /* channel 0, lo/hi, mode 3 */
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, divisor >> 8);
}

/* Channel 2 in one-shot mode, gated by port 0x61; OUT goes high when the count ends. */
void pit_wait_ms(uint32_t ms)
{
    while (ms) {
        uint32_t chunk = ms > 50 ? 50 : ms;     /* keep the divisor in 16 bits */
        ms -= chunk;
        uint16_t count = (uint16_t)(PIT_FREQ * chunk / 1000);

        outb(SPEAKER, (inb(SPEAKER) & ~0x02) | 0x01);   /* gate on, speaker off */
        outb(PIT_CMD, 0xB0);                            /* channel 2, lo/hi, mode 0 */
        outb(PIT_CH2, count & 0xFF);
        outb(PIT_CH2, count >> 8);
        while (!(inb(SPEAKER) & 0x20))
            ;
        outb(SPEAKER, inb(SPEAKER) & ~0x01);
    }
}
