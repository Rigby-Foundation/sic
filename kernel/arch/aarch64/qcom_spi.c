/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* SPI on a Qualcomm QUP serial engine (GENI), FIFO mode, polled: the bus
 * a phone's touch controller sits on. The engine's clock is voted on, its
 * pins muxed to it (the TLMM, which on KHAJE has its pins in tiles: WEST
 * at +0x100000, SOUTH +0x500000, EAST +0x900000 into its region), and it
 * is set up the way Linux's spi-geni-qcom does: 8-bit words packed four
 * to a FIFO entry (each byte MSB first on the wire), one chip select, the mode the device tree asks for.
 * /dev/spi0 is the raw bus: write() sends, read() clocks in (sending 0xff).
 * For now this knows the one engine a "creek" uses for touch (SE2). */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "fs/vfs.h"
#include "proc/sched.h"
#include "spinlock.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"

/* GENI */
#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define GENI_CGC_CTRL           0x28
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_SER_S_CLK_CFG      0x4c
#define GENI_FW_REVISION_RO     0x68
#define SE_GENI_CLK_SEL         0x7c
#define SE_SPI_CPHA             0x224
#define SE_SPI_LOOPBACK         0x22c
#define SE_SPI_CPOL             0x230
#define SE_SPI_DEMUX_OUTPUT_INV 0x24c
#define SE_SPI_DEMUX_SEL        0x250
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_SPI_TRANS_CFG        0x25c
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_SPI_WORD_LEN         0x268
#define SE_SPI_TX_TRANS_LEN     0x26c
#define SE_SPI_RX_TRANS_LEN     0x270
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
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
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR          0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c

#define SPI_TX_ONLY     1
#define SPI_RX_ONLY     2
#define SPI_TX_RX       7
#define FRAGMENTATION   (1u << 2)       /* keep the chip select asserted after this command */
#define M_CMD_DONE      (1u << 0)

/* KHAJE clock controller: QUP0's SE2 and wrapper clocks are votes in one register. */
#define GCC_APCS_VOTE_1         0x7900c
#define GCC_QUP0_S2_CBCR        0x1f3a4
#define QUP0_VOTES              ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 12))

/* TLMM: each GPIO's control and in/out registers, 0x1000 apart within its tile. */
#define TLMM_WEST       0x100000
#define TLMM_SOUTH      0x500000

static volatile uint8_t *se, *tlmm;
static spinlock_t bus_lock = SPINLOCK_INIT;
static uint32_t fifo_depth = 16;

static uint32_t rd(uint32_t o) { return mmio_read32(se + o); }
static void wr(uint32_t o, uint32_t v) { mmio_write32(se + o, v); }

/* A pin's control register: pull (0 none, 1 down, 3 up), function, drive (mA). */
static void pin_mux(uint32_t tile, int gpio, int func, int pull, int ma)
{
    volatile uint8_t *ctl = tlmm + tile + 0x1000u * (uint32_t)gpio;
    mmio_write32(ctl, (uint32_t)pull | (uint32_t)func << 2 | (uint32_t)(ma / 2 - 1) << 6);
}

void qcom_gpio_set(uint32_t tile, int gpio, int value)     /* an output, driven */
{
    volatile uint8_t *ctl = tlmm + tile + 0x1000u * (uint32_t)gpio;
    mmio_write32(ctl + 4, value ? 2 : 0);
    mmio_write32(ctl, (mmio_read32(ctl) & ~0x3fcu) | (1u << 9) | (3u << 6));   /* gpio function, output, 8 mA */
}

int qcom_gpio_get(uint32_t tile, int gpio)
{
    return mmio_read32(tlmm + tile + 0x1000u * (uint32_t)gpio + 4) & 1;
}

/* One transfer: send `tx` (or 0xff when NULL), keep what comes back in `rx`
 * (when not NULL). `more`: keep the chip select asserted afterwards. */
int qcom_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len, int more)
{
    if (!se || !len) return -1;
    uint64_t f = spin_lock_irqsave(&bus_lock);
    /* SPI_TX_RX sends its TX bytes and then receives (len + len clocks);
     * a read is SPI_RX_ONLY, which clocks len bytes and keeps them. */
    uint32_t op = rx ? (tx ? SPI_TX_RX : SPI_RX_ONLY) : SPI_TX_ONLY;
    wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    wr(SE_SPI_TX_TRANS_LEN, op == SPI_RX_ONLY ? 0 : len);
    wr(SE_SPI_RX_TRANS_LEN, rx ? len : 0);
    wr(SE_GENI_M_CMD0, op << 27 | (more ? FRAGMENTATION : 0));
    uint32_t sent = 0, got = 0;
    int rc = 0;
    for (uint32_t spins = 0; ; spins++) {
        /* feed the TX FIFO: four bytes to an entry, low byte first */
        while (op != SPI_RX_ONLY && sent < len && (rd(SE_GENI_TX_FIFO_STATUS) & 0x0fffffff) < fifo_depth) {
            uint32_t w = 0;
            for (int k = 0; k < 4 && sent + (uint32_t)k < len; k++)
                w |= (uint32_t)(tx ? tx[sent + (uint32_t)k] : 0xff) << (8 * k);
            wr(SE_GENI_TX_FIFOn, w);
            sent += 4;
        }
        /* drain the RX FIFO */
        uint32_t st = rd(SE_GENI_RX_FIFO_STATUS), words = st & 0x1ffffff;
        while (words--) {
            uint32_t w = rd(SE_GENI_RX_FIFOn);
            for (int k = 0; k < 4 && got < len; k++, got++)
                if (rx) rx[got] = (uint8_t)(w >> (8 * k));
        }
        uint32_t irq = rd(SE_GENI_M_IRQ_STATUS);
        if ((irq & M_CMD_DONE) && (op == SPI_RX_ONLY || sent >= len) && (!rx || got >= len)) break;
        if (spins > 2000000) { rc = -1; break; }
    }
    wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    spin_unlock_irqrestore(&bus_lock, f);
    return rc;
}

/* ---- /dev/spi0 ------------------------------------------------------------------- */

static uint8_t devbuf[128 * 1024];      /* a whole firmware download goes as one transfer, chip select held */

static long spi_read(struct file *f, void *buf, size_t len)
{
    (void)f;
    if (len > sizeof devbuf) len = sizeof devbuf;
    if (qcom_spi_xfer(NULL, devbuf, (uint32_t)len, 0) != 0) return -EIO;
    memcpy(buf, devbuf, len);
    return (long)len;
}

static long spi_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    if (len > sizeof devbuf) len = sizeof devbuf;
    memcpy(devbuf, buf, len);
    return qcom_spi_xfer(devbuf, NULL, (uint32_t)len, 0) == 0 ? (long)len : -EIO;
}

static const struct dev_ops spi_ops = { .read = spi_read, .write = spi_write };

/* ---- bring-up ---------------------------------------------------------------------- */

int qcom_spi_init(void)
{
    int node = fdt_find_compatible("qcom,spi-geni");
    int gccn = fdt_find_compatible("qcom,khaje-gcc");
    int tlmmn = fdt_find_compatible("qcom,khaje-pinctrl");
    uint64_t se_phys, gcc_phys, tlmm_phys;
    for (; node >= 0; node = fdt_find_compatible_after(node, "qcom,spi-geni"))
        if (fdt_reg(node, 0, &se_phys, NULL) == 0 && se_phys == 0x4a88000) break;   /* SE2: the touch bus */
    if (node < 0 || gccn < 0 || tlmmn < 0 || fdt_reg(gccn, 0, &gcc_phys, NULL) || fdt_reg(tlmmn, 0, &tlmm_phys, NULL)) return -1;
    volatile uint8_t *gcc = P2V(gcc_phys);
    tlmm = P2V(tlmm_phys);

    mmio_write32(gcc + GCC_APCS_VOTE_1, mmio_read32(gcc + GCC_APCS_VOTE_1) | QUP0_VOTES);
    for (int i = 0; i < 100000 && (mmio_read32(gcc + GCC_QUP0_S2_CBCR) & (1u << 31)); i++) ;
    if (mmio_read32(gcc + GCC_QUP0_S2_CBCR) & (1u << 31)) { kprintf("spi: SE2 clock did not start\n"); return -1; }
    se = P2V(se_phys);
    uint32_t fw = rd(GENI_FW_REVISION_RO);
    if (((fw >> 8) & 0xff) != 1) { kprintf("spi: SE2 runs protocol %u, not SPI\n", (fw >> 8) & 0xff); se = NULL; return -1; }
    fifo_depth = (mmio_read32(se + 0xe24) >> 16) & 0x3f;
    if (!fifo_depth) fifo_depth = 16;

    /* pins: MISO 6 (pull-up), MOSI 7, CLK 71 (pull-down), CS 80 (pull-up); function 1 (qup2), 6 mA */
    pin_mux(TLMM_WEST, 6, 1, 3, 6);
    pin_mux(TLMM_WEST, 7, 1, 1, 6);
    pin_mux(TLMM_WEST, 71, 1, 1, 6);
    pin_mux(TLMM_WEST, 80, 1, 3, 6);

    /* the engine: I/O defaults, FIFO mode, 8-bit words packed four to an entry */
    wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
    wr(GENI_OUTPUT_CTRL, 0x7f);
    wr(GENI_FORCE_DEFAULT_REG, 1);
    wr(SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    wr(SE_GENI_S_IRQ_CLEAR, 0xffffffff);
    wr(SE_GENI_DMA_MODE_EN, 0);
    wr(SE_GSI_EVENT_EN, 0);
    wr(SE_GENI_M_IRQ_EN, rd(SE_GENI_M_IRQ_EN) | M_CMD_DONE);
    wr(SE_GENI_TX_PACKING_CFG0, 0x7f8fe); wr(SE_GENI_TX_PACKING_CFG1, 0xffefe);   /* MSB first, as SPI sends */
    wr(SE_GENI_RX_PACKING_CFG0, 0x7f8fe); wr(SE_GENI_RX_PACKING_CFG1, 0xffefe);
    wr(SE_GENI_BYTE_GRAN, 0);
    wr(SE_GENI_RX_WATERMARK, fifo_depth - 2);
    wr(SE_GENI_RX_RFR, fifo_depth - 2);

    /* SPI: the mode the tree says (3 for the touch chip), CS 0, 8-bit words, 4.8 MHz from 19.2 */
    uint32_t mode = fdt_prop_u32(fdt_first_child(node), "omnivision,spi-mode", 0, 3);
    wr(SE_SPI_LOOPBACK, 0);
    wr(SE_SPI_CPHA, mode & 1);
    wr(SE_SPI_CPOL, mode & 2 ? (1u << 2) : 0);
    wr(SE_SPI_DEMUX_OUTPUT_INV, 0);
    wr(SE_SPI_DEMUX_SEL, 0);
    wr(SE_SPI_WORD_LEN, 8 - 4);
    wr(SE_SPI_TRANS_CFG, 0);                                    /* the chip select stays down for a whole command */
    wr(SE_GENI_CLK_SEL, 0);
    wr(GENI_SER_M_CLK_CFG, 4u << 4 | 1);
    wr(GENI_SER_S_CLK_CFG, 4u << 4 | 1);

    /* the engine itself: TX looped back to RX must come back unchanged */
    {
        static const uint8_t pat[8] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 };
        uint8_t back[8] = { 0 };
        wr(SE_SPI_LOOPBACK, 1);
        int rc = qcom_spi_xfer(pat, back, 8, 0);
        wr(SE_SPI_LOOPBACK, 0);
        kprintf("spi: loopback %s (%d): %02x %02x %02x %02x %02x %02x %02x %02x\n", memcmp(pat, back, 8) ? "FAILED" : "ok", rc,
                back[0], back[1], back[2], back[3], back[4], back[5], back[6], back[7]);
    }
    vfs_mkdev("/dev/spi0", &spi_ops, NULL);
    kprintf("spi: QUP SE2 (firmware %x, %u-word FIFOs), mode %u, 4.8 MHz -> /dev/spi0\n", fw, fifo_depth, mode);
    return 0;
}
