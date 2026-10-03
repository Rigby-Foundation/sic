/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* I2C on a Qualcomm QUP serial engine (GENI), FIFO mode, polled, 100 kHz
 * (Linux's i2c-qcom-geni: its 19.2 MHz timings, a write then a read with a
 * repeated start between them), and the battery's fuel gauge on it.
 *
 * The engines are the device tree's enabled "qcom,i2c-geni" ones of QUP0
 * whose firmware runs I2C; each gets its clock voted on and its pins muxed
 * (function 1, no pull, 2 mA, as the tree's default state says), and is
 * scanned with address-only probes, which the log lists. The "creek"
 * (Redmi 15 4G) has a TI bq28z610 gauge at 0x55, on a bus the board's
 * overlay names; the first one that answers there is the battery.
 *
 * /dev/battery: "capacity 87", "voltage 3987" (mV), "current -312" (mA,
 * negative while discharging), "temp 29.5" (degrees C), "status charging |
 * discharging | full", a line each; or "none" and what the scan found. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "asm/timer.h"
#include "fs/vfs.h"
#include "proc/sched.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"

#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define GENI_CGC_CTRL           0x28
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_FW_REVISION_RO     0x68
#define SE_GENI_CLK_SEL         0x7c
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_I2C_TX_TRANS_LEN     0x26c
#define SE_I2C_RX_TRANS_LEN     0x270
#define SE_I2C_SCL_COUNTERS     0x278
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GSI_EVENT_EN         0xe18
#define SE_HW_PARAM_0           0xe24

#define PROTO_I2C       3
#define I2C_WRITE       1
#define I2C_READ        2
#define I2C_ADDR_ONLY   4
#define STOP_STRETCH    (1u << 2)       /* no stop: a repeated start follows */
#define M_CMD_DONE      (1u << 0)
#define M_CMD_ERRORS    (1u << 1 | 1u << 2 | 1u << 3 | 1u << 10 | 1u << 12 | 1u << 13)   /* overrun, illegal, failure, NACK, bus, arbitration */
#define M_CMD_CANCEL    (1u << 4)
#define M_CMD_ABORT     (1u << 5)
#define CMD_CANCEL      (1u << 2)       /* in M_CMD_CTRL */
#define CMD_ABORT       (1u << 1)

/* KHAJE: QUP0's wrapper clocks and its engines' are votes in one register */
#define GCC_APCS_VOTE_1 0x7900c
#define QUP0_WRAP_VOTES ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9))
#define TLMM_WEST       0x100000

#define GAUGE_ADDR      0x55

struct bus { volatile uint8_t *se; int index; uint32_t fifo; uint8_t found[16]; int nfound; };
static struct bus buses[6];
static int nbuses;
static struct bus *gauge;
static volatile int busy;

static void lock(void) { while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) task_sleep_ms(1); }
static void unlock(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }
static uint32_t rd(struct bus *b, uint32_t o) { return mmio_read32(b->se + o); }
static void wr(struct bus *b, uint32_t o, uint32_t v) { mmio_write32(b->se + o, v); }

/* One command; 0, or -ENODEV (no ACK), -EIO, -ETIMEDOUT. */
static int command(struct bus *b, uint32_t op, uint8_t addr, uint32_t flags, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    wr(b, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    if (op == I2C_WRITE) wr(b, SE_I2C_TX_TRANS_LEN, len);
    if (op == I2C_READ) wr(b, SE_I2C_RX_TRANS_LEN, len);
    wr(b, SE_GENI_M_CMD0, op << 27 | (uint32_t)addr << 9 | flags);
    uint32_t sent = 0, got = 0, irq = 0;
    uint64_t t0 = timer_ms();
    for (;;) {
        while (op == I2C_WRITE && sent < len && (rd(b, SE_GENI_TX_FIFO_STATUS) & 0x0fffffff) < b->fifo) {
            uint32_t w = 0;
            for (int k = 0; k < 4 && sent + (uint32_t)k < len; k++) w |= (uint32_t)tx[sent + (uint32_t)k] << (8 * k);
            wr(b, SE_GENI_TX_FIFOn, w);
            sent += 4;
        }
        if (op == I2C_READ) {
            uint32_t words = rd(b, SE_GENI_RX_FIFO_STATUS) & 0x1ffffff;
            while (words--) {
                uint32_t w = rd(b, SE_GENI_RX_FIFOn);
                for (int k = 0; k < 4 && got < len; k++, got++) rx[got] = (uint8_t)(w >> (8 * k));
            }
        }
        irq = rd(b, SE_GENI_M_IRQ_STATUS);
        if (irq & M_CMD_ERRORS) break;
        if ((irq & M_CMD_DONE) && (op != I2C_READ || got >= len)) break;
        if (timer_ms() - t0 > 50) { irq = 0; break; }
    }
    int rc = irq & M_CMD_ERRORS ? (irq & (1u << 10) ? -ENODEV : -EIO) : irq & M_CMD_DONE ? 0 : -ETIMEDOUT;
    if (rc) {                                   /* let go of the bus: cancel, else abort */
        wr(b, SE_GENI_M_CMD_CTRL_REG, CMD_CANCEL);
        uint64_t t1 = timer_ms();
        while (!(rd(b, SE_GENI_M_IRQ_STATUS) & (M_CMD_CANCEL | M_CMD_DONE)) && timer_ms() - t1 < 10) ;
        if (!(rd(b, SE_GENI_M_IRQ_STATUS) & (M_CMD_CANCEL | M_CMD_DONE))) {
            wr(b, SE_GENI_M_CMD_CTRL_REG, CMD_ABORT);
            t1 = timer_ms();
            while (!(rd(b, SE_GENI_M_IRQ_STATUS) & M_CMD_ABORT) && timer_ms() - t1 < 10) ;
        }
    }
    wr(b, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    return rc;
}

/* A register read the SMBus way: its number written, then a repeated start and the bytes. */
static int read_reg(struct bus *b, uint8_t addr, uint8_t reg, uint8_t *v, uint32_t len)
{
    lock();
    int rc = command(b, I2C_WRITE, addr, STOP_STRETCH, &reg, NULL, 1);
    if (!rc) rc = command(b, I2C_READ, addr, 0, NULL, v, len);
    unlock();
    return rc;
}

static int read16(struct bus *b, uint8_t reg, int *v)
{
    uint8_t x[2];
    int rc = read_reg(b, GAUGE_ADDR, reg, x, 2);
    if (!rc) *v = x[0] | x[1] << 8;
    return rc;
}

/* ---- /dev/battery ------------------------------------------------------------------ */

static char report[512];

static size_t make_report(void)
{
    size_t n = 0;
#define P(...) n += (size_t)ksnprintf(report + n, sizeof report - n, __VA_ARGS__)
    int soc, mv, ma, dk, st;
    if (gauge && !read16(gauge, 0x2c, &soc) && !read16(gauge, 0x08, &mv) && !read16(gauge, 0x0c, &ma) &&
        !read16(gauge, 0x06, &dk) && !read16(gauge, 0x0a, &st)) {
        ma = (int16_t)ma;
        int dc = dk - 2732;                     /* 0.1 K to 0.1 degrees C */
        P("capacity %d\nvoltage %d\ncurrent %d\ntemp %s%d.%d\n", soc, mv, ma, dc < 0 ? "-" : "", (dc < 0 ? -dc : dc) / 10, (dc < 0 ? -dc : dc) % 10);
        P("status %s\n", st & (1u << 5) ? "full" : ma > 0 ? "charging" : "discharging");   /* BatteryStatus FC */
    } else {
        P("none\n");
    }
    for (int i = 0; i < nbuses; i++) {
        P("i2c SE%d:", buses[i].index);
        for (int k = 0; k < buses[i].nfound; k++) P(" %02x", buses[i].found[k]);
        P("%s\n", &buses[i] == gauge ? " (the gauge)" : "");
    }
#undef P
    return n;
}

static long battery_read(struct file *f, void *buf, size_t len)
{
    if (f->pos == 0) make_report();
    size_t n = strlen(report);
    if (f->pos >= n) return 0;
    if (len > n - f->pos) len = n - f->pos;
    memcpy(buf, report + f->pos, len);
    f->pos += len;
    return (long)len;
}

static const struct dev_ops battery_ops = { .read = battery_read };

/* ---- bring-up ---------------------------------------------------------------------- */

static const struct { uint64_t base; int index, pins[2]; } engines[] = {
    { 0x4a80000, 0, { 0, 1 } }, { 0x4a84000, 1, { 4, 5 } },
};

static int bus_up(struct bus *b)
{
    uint32_t fw = rd(b, GENI_FW_REVISION_RO);
    if (((fw >> 8) & 0xff) != PROTO_I2C) { kprintf("i2c: SE%d runs protocol %u, not I2C\n", b->index, (fw >> 8) & 0xff); return -1; }
    b->fifo = (rd(b, SE_HW_PARAM_0) >> 16) & 0x3f;
    if (!b->fifo) b->fifo = 16;
    wr(b, GENI_CGC_CTRL, rd(b, GENI_CGC_CTRL) | 0x7f);
    wr(b, GENI_OUTPUT_CTRL, 0x7f);
    wr(b, GENI_FORCE_DEFAULT_REG, 1);
    wr(b, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    wr(b, SE_GENI_S_IRQ_CLEAR, 0xffffffff);
    wr(b, SE_GENI_DMA_MODE_EN, 0);
    wr(b, SE_GSI_EVENT_EN, 0);
    wr(b, SE_GENI_M_IRQ_EN, rd(b, SE_GENI_M_IRQ_EN) | M_CMD_DONE | M_CMD_ERRORS | M_CMD_CANCEL | M_CMD_ABORT);
    wr(b, SE_GENI_CLK_SEL, 0);
    wr(b, GENI_SER_M_CLK_CFG, 7u << 4 | 1);                      /* 19.2 MHz / 7 */
    wr(b, SE_I2C_SCL_COUNTERS, 10u << 20 | 12u << 10 | 26);     /* high, low, cycle: 100 kHz */
    return 0;
}

static void probe_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < nbuses; i++) {
        struct bus *b = &buses[i];
        for (int a = 0x08; a < 0x78 && b->nfound < 16; a++) {
            lock();
            int rc = command(b, I2C_ADDR_ONLY, (uint8_t)a, 0, NULL, NULL, 0);
            unlock();
            if (!rc) b->found[b->nfound++] = (uint8_t)a;
        }
        int soc;
        if (!gauge && read16(b, 0x2c, &soc) == 0 && soc <= 100) gauge = b;
    }
    make_report();
    kprintf("battery: %s", report);
}

void qcom_i2c_init(void)
{
    int gccn = fdt_find_compatible("qcom,khaje-gcc"), tlmmn = fdt_find_compatible("qcom,khaje-pinctrl");
    uint64_t gcc_phys, tlmm_phys;
    if (gccn < 0 || tlmmn < 0 || fdt_reg(gccn, 0, &gcc_phys, NULL) || fdt_reg(tlmmn, 0, &tlmm_phys, NULL)) return;
    volatile uint8_t *gcc = P2V(gcc_phys), *tlmm = P2V(tlmm_phys);
    for (int n = fdt_find_compatible("qcom,i2c-geni"); n >= 0; n = fdt_find_compatible_after(n, "qcom,i2c-geni")) {
        uint64_t base;
        int len;
        const char *status = fdt_prop(n, "status", &len);
        if (fdt_reg(n, 0, &base, NULL) || (status && strcmp(status, "ok") && strcmp(status, "okay"))) continue;
        for (size_t e = 0; e < sizeof engines / sizeof engines[0]; e++) {
            if (engines[e].base != base || nbuses >= 6) continue;
            mmio_write32(gcc + GCC_APCS_VOTE_1, mmio_read32(gcc + GCC_APCS_VOTE_1) | QUP0_WRAP_VOTES | 1u << (10 + engines[e].index));
            for (int k = 0; k < 2; k++)                         /* function 1 (qupN), no pull, 2 mA */
                mmio_write32(tlmm + TLMM_WEST + 0x1000u * (uint32_t)engines[e].pins[k], 1u << 2 | 0u << 6);
            struct bus *b = &buses[nbuses];
            b->se = P2V(base); b->index = engines[e].index;
            for (int i = 0; i < 1000; i++) (void)rd(b, GENI_FW_REVISION_RO);   /* the clock comes up */
            if (bus_up(b) == 0) nbuses++;
        }
    }
    if (!nbuses) return;
    vfs_mkdev("/dev/battery", &battery_ops, NULL);
    task_create("i2c-probe", probe_thread, NULL);               /* a scan takes a moment: not on the boot path */
}
