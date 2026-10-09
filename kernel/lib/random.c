/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Random numbers for getrandom(2) and /dev/[u]random: ChaCha20 as a
 * generator, keyed at boot from the CPU's random number instruction where
 * there is one (RDSEED/RDRAND, RNDR) and from timing jitter (the cycle
 * counter around work whose duration wobbles), with the cycle counter
 * mixed in again on every request. After each request the key is replaced
 * by more generator output, so what was handed out cannot be recomputed
 * from a later state. */
#include "types.h"
#include "string.h"
#include "spinlock.h"
#include "printf.h"
#include "asm/random.h"

static uint32_t key[8];
static uint64_t counter;
static spinlock_t lock;
static int seeded, hw;

static inline uint32_t rotl(uint32_t v, int n) { return v << n | v >> (32 - n); }
#define QR(a, b, c, d) (a += b, d = rotl(d ^ a, 16), c += d, b = rotl(b ^ c, 12), a += b, d = rotl(d ^ a, 8), c += d, b = rotl(b ^ c, 7))

/* One ChaCha20 block (RFC 8439 layout, the 96-bit nonce zero but for the counter's top half). */
static void block(uint32_t out[16])
{
    uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                       key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                       (uint32_t)counter, (uint32_t)(counter >> 32), 0, 0 };
    uint32_t x[16];
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + s[i];
    counter++;
}

/* Fold 64 bits into the key and step the generator over it. lock held. */
static void mix(uint64_t v)
{
    static unsigned at;
    key[at++ % 8] ^= (uint32_t)v;
    key[at++ % 8] ^= (uint32_t)(v >> 32);
    uint32_t b[16];
    block(b);
    for (int i = 0; i < 8; i++) key[i] ^= b[i];
}

static void seed(void)
{
    uint64_t v;
    for (int i = 0; i < 16; i++)
        if (arch_hw_random(&v)) { mix(v); hw++; }
    /* jitter: how long a little memory traffic takes, a few thousand times */
    static volatile uint8_t scratch[4096];
    for (int i = 0; i < 4096; i++) {
        uint64_t t0 = arch_cycles();
        for (int j = 0; j < 64; j++) scratch[(i * 67 + j * 131) & 4095] += (uint8_t)j;
        uint64_t d = arch_cycles() - t0;
        if ((i & 31) == 31) mix(d ^ t0 << 17);
        else key[i % 8] ^= (uint32_t)d << (i % 24);
    }
    seeded = 1;
}

/* Something unpredictable happened (an interrupt's timing, a device's noise). */
void random_add(uint64_t v)
{
    uint64_t f = spin_lock_irqsave(&lock);
    mix(v ^ arch_cycles());
    spin_unlock_irqrestore(&lock, f);
}

void random_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    uint64_t f = spin_lock_irqsave(&lock);
    if (!seeded) seed();
    mix(arch_cycles());
    while (len) {
        uint32_t b[16];
        block(b);
        size_t n = len < sizeof b ? len : sizeof b;
        memcpy(p, b, n);
        p += n; len -= n;
    }
    uint32_t b[16];                         /* a fresh key: no going back */
    block(b);
    memcpy(key, b, sizeof key);
    spin_unlock_irqrestore(&lock, f);
}

void random_init(void)
{
    uint64_t f = spin_lock_irqsave(&lock);
    if (!seeded) seed();
    spin_unlock_irqrestore(&lock, f);
    kprintf("random: ChaCha20, seeded from %s\n", hw ? "the CPU's random number instruction and timing jitter" : "timing jitter (no CPU random number instruction)");
}
