/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Broadcom BCM4401 (b44) 10/100 Ethernet, as in the Dell Vostro 1000 and
 * other laptops of its time. After Linux's b44 and the parts of its SSB
 * bus driver a PCI 440x needs.
 *
 * The chip is a small Sonics Silicon Backplane: an Ethernet core, a PCI
 * core and a modem core, each a 4 KiB register block. PCI BAR 0 is an
 * 8 KiB window: the first 4 KiB shows whichever core the config register
 * BAR0_WIN selects (the Ethernet core, once set up), the second the SPROM
 * with the MAC address. The DMA engine reaches only the first GiB of
 * memory, through the PCI core's translation at 0x40000000, so rings and
 * buffers are allocated below 1 GiB. One receive and one transmit ring of
 * 8-byte descriptors, each with its own buffer; frames are copied in and
 * out. The netrx thread polls every 20 ms as well, so the link works
 * even if the interrupt is not routed where config space says. */
#include "drivers/b44.h"
#include "drivers/pci.h"
#include "net/net.h"
#include "endian.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "asm/irq.h"
#include "asm/timer.h"
#include "string.h"
#include "printf.h"

/* PCI config space and the backplane */
#define SSB_BAR0_WIN        0x80            /* which core the first 4 KiB of BAR 0 shows */
#define SSB_PCI_IRQMASK     0x94            /* PCI core rev >= 6: per-core interrupt enables at bit 8 */
#define SSB_ENUM_BASE       0x18000000u
#define SSB_CORE_SIZE       0x1000
#define SSB_SPROM_BASE      0x1000          /* in BAR 0 */
#define SSB_PCI_DMA         0x40000000u     /* what the chip adds to a physical address */
#define SSB_DEV_PCI         0x804
#define SSB_DEV_ETHERNET    0x806

#define SSB_TPSFLAG         0x0F18
#define  SSB_TPSFLAG_BPFLAG 0x0000003F
#define SSB_IMSTATE         0x0F90
#define  SSB_IMSTATE_IBE    0x00020000
#define  SSB_IMSTATE_TO     0x00040000
#define  SSB_IMSTATE_BUSY   0x01800000
#define  SSB_IMSTATE_REJECT 0x02000000
#define SSB_INTVEC          0x0F94
#define SSB_TMSLOW          0x0F98
#define  SSB_TMSLOW_RESET   0x00000001
#define  SSB_TMSLOW_REJECT  0x00000002
#define  SSB_TMSLOW_REJECT_23 0x00000004
#define  SSB_TMSLOW_CLOCK   0x00010000
#define  SSB_TMSLOW_FGC     0x00020000
#define SSB_TMSHIGH         0x0F9C
#define  SSB_TMSHIGH_SERR   0x00000001
#define  SSB_TMSHIGH_BUSY   0x00000004
#define SSB_IMCFGLO         0x0FA8
#define  SSB_IMCFGLO_SERTO  0x00000007
#define  SSB_IMCFGLO_REQTO  0x00000070
#define SSB_IDLOW           0x0FF8
#define  SSB_IDLOW_INITIATOR 0x00000080
#define  SSB_IDLOW_SSBREV   0xF0000000
#define SSB_IDHIGH          0x0FFC

#define PCICORE_SBTOPCI2    0x0108
#define  SBTOPCI_PREF       0x00000004
#define  SBTOPCI_BURST      0x00000008
#define  SBTOPCI_MRM        0x00000020
#define PCICORE_SPROM0      0x0800          /* the SPROM shadow: word 0 holds the PCI core's index */

#define SPROM1_ET0MAC       0x4E
#define SPROM1_ETHPHY       0x5A

/* The Ethernet core */
#define B44_DEVCTRL         0x0000
#define  DEVCTRL_IPP        0x00000400      /* internal PHY present */
#define  DEVCTRL_EPR        0x00008000      /* PHY in reset */
#define B44_ISTAT           0x0020
#define  ISTAT_TO           0x00000080
#define  ISTAT_DSCE         0x00000400
#define  ISTAT_DATAE        0x00000800
#define  ISTAT_DPE          0x00001000
#define  ISTAT_RDU          0x00002000
#define  ISTAT_RFO          0x00004000
#define  ISTAT_TFU          0x00008000
#define  ISTAT_RX           0x00010000
#define  ISTAT_TX           0x01000000
#define  ISTAT_EMAC         0x04000000
#define  ISTAT_ERRORS       (ISTAT_DSCE | ISTAT_DATAE | ISTAT_DPE | ISTAT_RFO | ISTAT_TFU)
#define B44_IMASK           0x0024
#define B44_MAC_CTRL        0x00A8
#define  MAC_CTRL_CRC32_ENAB 0x00000001
#define  MAC_CTRL_PHY_PDOWN 0x00000004
#define  MAC_CTRL_PHY_LEDCTRL 0x000000E0
#define B44_RCV_LAZY        0x0100
#define B44_DMATX_CTRL      0x0200
#define  DMATX_CTRL_ENABLE  0x00000001
#define B44_DMATX_ADDR      0x0204
#define B44_DMATX_PTR       0x0208
#define B44_DMATX_STAT      0x020C
#define  DMA_STAT_CDMASK    0x00000FFF
#define B44_DMARX_CTRL      0x0210
#define  DMARX_CTRL_ENABLE  0x00000001
#define  DMARX_CTRL_ROSHIFT 1
#define B44_DMARX_ADDR      0x0214
#define B44_DMARX_PTR       0x0218
#define B44_DMARX_STAT      0x021C
#define  DMARX_STAT_EMASK   0x000F0000
#define  DMARX_STAT_SIDLE   0x00002000
#define B44_RXCONFIG        0x0400
#define  RXCONFIG_ALLMULTI  0x00000002
#define  RXCONFIG_PROMISC   0x00000008
#define  RXCONFIG_CAM_ABSENT 0x00000100
#define B44_RXMAXLEN        0x0404
#define B44_TXMAXLEN        0x0408
#define B44_MDIO_CTRL       0x0410
#define  MDIO_CTRL_PREAMBLE 0x00000080
#define B44_MDIO_DATA       0x0414
#define B44_EMAC_IMASK      0x0418
#define B44_EMAC_ISTAT      0x041C
#define  EMAC_INT_MII       0x00000001
#define B44_CAM_DATA_LO     0x0420
#define B44_CAM_DATA_HI     0x0424
#define  CAM_DATA_HI_VALID  0x00010000
#define B44_CAM_CTRL        0x0428
#define  CAM_CTRL_ENABLE    0x00000001
#define  CAM_CTRL_WRITE     0x00000008
#define  CAM_CTRL_BUSY      0x80000000
#define B44_ENET_CTRL       0x042C
#define  ENET_CTRL_ENABLE   0x00000001
#define  ENET_CTRL_DISABLE  0x00000002
#define  ENET_CTRL_EPSEL    0x00000008
#define B44_TX_CTRL         0x0430
#define  TX_CTRL_DUPLEX     0x00000001
#define B44_TX_WMARK        0x0434
#define B44_MIB_CTRL        0x0438
#define  MIB_CTRL_CLR_ON_READ 0x00000001
#define B44_TX_GOOD_O       0x0500
#define B44_TX_PAUSE        0x055C
#define B44_RX_GOOD_O       0x0580
#define B44_RX_NPAUSE       0x05D8

/* The PHY, over MDIO */
#define MII_BMCR            0
#define  BMCR_ANRESTART     0x0200
#define  BMCR_ANENABLE      0x1000
#define  BMCR_RESET         0x8000
#define MII_BMSR            1
#define  BMSR_LSTATUS       0x0004
#define MII_ADVERTISE       4
#define  ADV_ALL_10_100     0x01E1          /* CSMA, 10/100 half/full */
#define B44_MII_AUXCTRL     24
#define  AUXCTRL_DUPLEX     0x0001
#define  AUXCTRL_SPEED      0x0002
#define B44_MII_ALEDCTRL    26
#define B44_MII_TLEDCTRL    27
#define  TLEDCTRL_ENABLE    0x0040

struct dma_desc { uint32_t ctrl, addr; };
#define DESC_CTRL_LEN       0x00001FFF
#define DESC_CTRL_EOT       0x10000000
#define DESC_CTRL_IOC       0x20000000
#define DESC_CTRL_EOF       0x40000000
#define DESC_CTRL_SOF       0x80000000

/* The chip writes this header in front of every received frame. */
struct rx_header { uint16_t len, flags; uint16_t pad[12]; };
#define RX_HEADER_LEN       28
#define RX_PKT_OFFSET       (RX_HEADER_LEN + 2)
#define RX_FLAG_ERRORS      0x000F          /* odd nibbles, symbol, CRC, FIFO overflow */

#define RX_DESCS    128
#define TX_DESCS    64
#define BUF_SIZE    2048
#define RX_BUF_LEN  (1536 + RX_PKT_OFFSET)
#define DMA_LIMIT   (1ull << 30)

struct b44 {
    const struct pci_dev *pd;
    volatile uint8_t *regs;                 /* BAR 0: the window, then the SPROM */
    int eth_core, pci_core;
    uint32_t pci_rev;
    uint8_t phy_addr;
    struct dma_desc *rx, *tx;
    uint64_t rx_phys, tx_phys;
    uint8_t *rx_buf[RX_DESCS], *tx_buf[TX_DESCS];
    uint64_t rx_buf_phys[RX_DESCS], tx_buf_phys[TX_DESCS];
    uint32_t rx_cons, tx_prod, tx_cons;
    spinlock_t lock;
    struct netdev dev;
    uint64_t last_phy_ms;
    int link, irq;
    uint32_t resets;
};

static inline uint32_t br32(struct b44 *b, uint32_t reg) { return mmio_read32(b->regs + reg); }
static inline void bw32(struct b44 *b, uint32_t reg, uint32_t v) { mmio_write32(b->regs + reg, v); }
static inline uint16_t sprom16(struct b44 *b, uint32_t off) { return mmio_read16(b->regs + SSB_SPROM_BASE + off); }

static void cfg_write(struct b44 *b, uint8_t off, uint32_t v) { pci_write32(b->pd->bus, b->pd->slot, b->pd->func, off, v); }
static uint32_t cfg_read(struct b44 *b, uint8_t off) { return pci_read32(b->pd->bus, b->pd->slot, b->pd->func, off); }

static void switch_core(struct b44 *b, int idx)
{
    cfg_write(b, SSB_BAR0_WIN, SSB_ENUM_BASE + (uint32_t)idx * SSB_CORE_SIZE);
    (void)cfg_read(b, SSB_BAR0_WIN);
}

/* A PCI read takes about a microsecond. */
static void udelay_(struct b44 *b, unsigned us)
{
    for (unsigned i = 0; i < us; i++) (void)br32(b, SSB_IDHIGH);
}

static int wait_bit(struct b44 *b, uint32_t reg, uint32_t bit, unsigned tries, int clear)
{
    for (unsigned i = 0; i < tries; i++) {
        uint32_t v = br32(b, reg);
        if (clear ? !(v & bit) : (v & bit) == bit) return 0;
        udelay_(b, 10);
    }
    return -1;
}

/* ---- the backplane: core reset (ssb_device_enable/disable) --------------------- */

static uint32_t reject_bits(struct b44 *b)
{
    switch (br32(b, SSB_IDLOW) & SSB_IDLOW_SSBREV) {
    case 0x00000000: case 0x40000000: case 0x60000000: return SSB_TMSLOW_REJECT;
    case 0x10000000: return SSB_TMSLOW_REJECT_23;
    default: return SSB_TMSLOW_REJECT | SSB_TMSLOW_REJECT_23;
    }
}

static void flush_tmslow(struct b44 *b) { (void)br32(b, SSB_TMSLOW); udelay_(b, 1); }

static void core_disable(struct b44 *b)
{
    if (br32(b, SSB_TMSLOW) & SSB_TMSLOW_RESET) return;
    uint32_t reject = reject_bits(b);
    if (br32(b, SSB_TMSLOW) & SSB_TMSLOW_CLOCK) {
        bw32(b, SSB_TMSLOW, reject | SSB_TMSLOW_CLOCK);
        wait_bit(b, SSB_TMSLOW, reject, 1000, 0);
        wait_bit(b, SSB_TMSHIGH, SSB_TMSHIGH_BUSY, 1000, 1);
        if (br32(b, SSB_IDLOW) & SSB_IDLOW_INITIATOR) {
            bw32(b, SSB_IMSTATE, br32(b, SSB_IMSTATE) | SSB_IMSTATE_REJECT);
            wait_bit(b, SSB_IMSTATE, SSB_IMSTATE_BUSY, 1000, 1);
        }
        bw32(b, SSB_TMSLOW, SSB_TMSLOW_FGC | SSB_TMSLOW_CLOCK | reject | SSB_TMSLOW_RESET);
        flush_tmslow(b);
        if (br32(b, SSB_IDLOW) & SSB_IDLOW_INITIATOR)
            bw32(b, SSB_IMSTATE, br32(b, SSB_IMSTATE) & ~SSB_IMSTATE_REJECT);
    }
    bw32(b, SSB_TMSLOW, reject | SSB_TMSLOW_RESET);
    flush_tmslow(b);
}

static void core_enable(struct b44 *b)
{
    core_disable(b);
    bw32(b, SSB_TMSLOW, SSB_TMSLOW_RESET | SSB_TMSLOW_CLOCK | SSB_TMSLOW_FGC);
    flush_tmslow(b);
    if (br32(b, SSB_TMSHIGH) & SSB_TMSHIGH_SERR) bw32(b, SSB_TMSHIGH, 0);
    uint32_t v = br32(b, SSB_IMSTATE);
    if (v & (SSB_IMSTATE_IBE | SSB_IMSTATE_TO)) bw32(b, SSB_IMSTATE, v & ~(SSB_IMSTATE_IBE | SSB_IMSTATE_TO));
    bw32(b, SSB_TMSLOW, SSB_TMSLOW_CLOCK | SSB_TMSLOW_FGC);
    flush_tmslow(b);
    bw32(b, SSB_TMSLOW, SSB_TMSLOW_CLOCK);
    flush_tmslow(b);
}

static int core_enabled(struct b44 *b)
{
    return (br32(b, SSB_TMSLOW) & (SSB_TMSLOW_CLOCK | SSB_TMSLOW_RESET | reject_bits(b))) == SSB_TMSLOW_CLOCK;
}

/* Route the Ethernet core's interrupt through the PCI core, and the PCI
 * core's one-time setup (ssb_pcicore_dev_irqvecs_enable). Leaves the
 * window on the Ethernet core. */
static void pcicore_setup(struct b44 *b)
{
    uint32_t flag = br32(b, SSB_TPSFLAG) & SSB_TPSFLAG_BPFLAG;     /* the Ethernet core's backplane flag */
    if (b->pci_rev >= 6) {
        cfg_write(b, SSB_PCI_IRQMASK, cfg_read(b, SSB_PCI_IRQMASK) | (1u << b->eth_core) << 8);
        switch_core(b, b->pci_core);
    } else {
        switch_core(b, b->pci_core);
        bw32(b, SSB_INTVEC, br32(b, SSB_INTVEC) | 1u << flag);
    }
    bw32(b, PCICORE_SBTOPCI2, br32(b, PCICORE_SBTOPCI2) | SBTOPCI_PREF | SBTOPCI_BURST);
    if (b->pci_rev < 5) {
        uint32_t v = br32(b, SSB_IMCFGLO);
        v = (v & ~SSB_IMCFGLO_SERTO) | 2;
        v = (v & ~SSB_IMCFGLO_REQTO) | 3 << 4;
        bw32(b, SSB_IMCFGLO, v);
    } else if (b->pci_rev >= 11) {
        bw32(b, PCICORE_SBTOPCI2, br32(b, PCICORE_SBTOPCI2) | SBTOPCI_MRM);
    }
    switch_core(b, b->eth_core);
}

/* ---- the PHY ------------------------------------------------------------------ */

static int phy_read(struct b44 *b, int reg, uint32_t *val)
{
    bw32(b, B44_EMAC_ISTAT, EMAC_INT_MII);
    bw32(b, B44_MDIO_DATA, 0x40000000u | 2u << 28 | (uint32_t)b->phy_addr << 23 | (uint32_t)reg << 18 | 2u << 16);
    int err = wait_bit(b, B44_EMAC_ISTAT, EMAC_INT_MII, 100, 0);
    *val = br32(b, B44_MDIO_DATA) & 0xFFFF;
    return err;
}

static int phy_write(struct b44 *b, int reg, uint32_t val)
{
    bw32(b, B44_EMAC_ISTAT, EMAC_INT_MII);
    bw32(b, B44_MDIO_DATA, 0x40000000u | 1u << 28 | (uint32_t)b->phy_addr << 23 | (uint32_t)reg << 18 | 2u << 16 | (val & 0xFFFF));
    return wait_bit(b, B44_EMAC_ISTAT, EMAC_INT_MII, 100, 0);
}

static void phy_setup(struct b44 *b)
{
    uint32_t v;
    phy_write(b, MII_BMCR, BMCR_RESET);
    udelay_(b, 100);
    if (phy_read(b, B44_MII_ALEDCTRL, &v) == 0) phy_write(b, B44_MII_ALEDCTRL, v & 0x7FFF);
    if (phy_read(b, B44_MII_TLEDCTRL, &v) == 0) phy_write(b, B44_MII_TLEDCTRL, v | TLEDCTRL_ENABLE);
    phy_write(b, MII_ADVERTISE, ADV_ALL_10_100);
    phy_write(b, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
}

/* Link state from the PHY; the MAC's duplex follows it. */
static void phy_check(struct b44 *b)
{
    uint32_t bmsr, aux;
    if (phy_read(b, MII_BMSR, &bmsr) || phy_read(b, B44_MII_AUXCTRL, &aux) || bmsr == 0xFFFF) return;
    int up = (bmsr & BMSR_LSTATUS) != 0;
    if (up && !b->link) {
        uint32_t tx = br32(b, B44_TX_CTRL);
        bw32(b, B44_TX_CTRL, (aux & AUXCTRL_DUPLEX) ? tx | TX_CTRL_DUPLEX : tx & ~TX_CTRL_DUPLEX);
        kprintf("b44: %s: link up, %s Mbit/s, %s duplex\n", b->dev.name, (aux & AUXCTRL_SPEED) ? "100" : "10",
                (aux & AUXCTRL_DUPLEX) ? "full" : "half");
    } else if (!up && b->link) {
        kprintf("b44: %s: link down\n", b->dev.name);
    }
    b->link = up;
    if (up) b->dev.flags |= IFF_RUNNING; else b->dev.flags &= ~IFF_RUNNING;
}

/* ---- the MAC ------------------------------------------------------------------ */

static void cam_write(struct b44 *b, const uint8_t *mac, int index)
{
    bw32(b, B44_CAM_DATA_LO, (uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]);
    bw32(b, B44_CAM_DATA_HI, CAM_DATA_HI_VALID | (uint32_t)mac[0] << 8 | mac[1]);
    bw32(b, B44_CAM_CTRL, CAM_CTRL_WRITE | (uint32_t)index << 16);
    wait_bit(b, B44_CAM_CTRL, CAM_CTRL_BUSY, 100, 1);
}

/* Our address in the CAM (receive filter); promiscuous if the chip has none. */
static void set_rx_mode(struct b44 *b)
{
    uint32_t v = br32(b, B44_RXCONFIG) & ~(RXCONFIG_PROMISC | RXCONFIG_ALLMULTI);
    if (v & RXCONFIG_CAM_ABSENT) { bw32(b, B44_RXCONFIG, v | RXCONFIG_PROMISC); return; }
    static const uint8_t zero[6];
    bw32(b, B44_CAM_CTRL, 0);
    cam_write(b, b->dev.mac, 0);
    for (int i = 1; i < 64; i++) cam_write(b, zero, i);
    bw32(b, B44_RXCONFIG, v | RXCONFIG_ALLMULTI);
    bw32(b, B44_CAM_CTRL, br32(b, B44_CAM_CTRL) | CAM_CTRL_ENABLE);
}

/* b44_chip_reset(FULL): the core out of reset, DMA stopped, MIB cleared,
 * MDIO clocked, the internal PHY out of reset. */
static void chip_reset(struct b44 *b)
{
    int was = core_enabled(b);
    core_enable(b);
    pcicore_setup(b);
    if (was) {
        bw32(b, B44_RCV_LAZY, 0);
        bw32(b, B44_ENET_CTRL, ENET_CTRL_DISABLE);
        wait_bit(b, B44_ENET_CTRL, ENET_CTRL_DISABLE, 200, 1);
        bw32(b, B44_DMATX_CTRL, 0);
        if (br32(b, B44_DMARX_STAT) & DMARX_STAT_EMASK) wait_bit(b, B44_DMARX_STAT, DMARX_STAT_SIDLE, 100, 0);
        bw32(b, B44_DMARX_CTRL, 0);
    }
    bw32(b, B44_MIB_CTRL, MIB_CTRL_CLR_ON_READ);
    for (uint32_t r = B44_TX_GOOD_O; r <= B44_TX_PAUSE; r += 4) (void)br32(b, r);
    for (uint32_t r = B44_RX_GOOD_O; r <= B44_RX_NPAUSE; r += 4) (void)br32(b, r);
    bw32(b, B44_MDIO_CTRL, MDIO_CTRL_PREAMBLE | 0x0D);
    (void)br32(b, B44_MDIO_CTRL);
    if (!(br32(b, B44_DEVCTRL) & DEVCTRL_IPP)) {
        bw32(b, B44_ENET_CTRL, ENET_CTRL_EPSEL);        /* external PHY: not on these laptops */
        (void)br32(b, B44_ENET_CTRL);
    } else {
        uint32_t v = br32(b, B44_DEVCTRL);
        if (v & DEVCTRL_EPR) { bw32(b, B44_DEVCTRL, v & ~DEVCTRL_EPR); (void)br32(b, B44_DEVCTRL); udelay_(b, 100); }
    }
}

/* Fresh rings: every receive descriptor owns its buffer for good. */
static void init_rings(struct b44 *b)
{
    memset(b->rx, 0, 4096);
    memset(b->tx, 0, 4096);
    for (int i = 0; i < RX_DESCS; i++) {
        memset(b->rx_buf[i], 0, RX_HEADER_LEN);
        b->rx[i].ctrl = htole32(RX_BUF_LEN | (i == RX_DESCS - 1 ? DESC_CTRL_EOT : 0));
        b->rx[i].addr = htole32((uint32_t)b->rx_buf_phys[i] + SSB_PCI_DMA);
    }
    b->rx_cons = b->tx_prod = b->tx_cons = 0;
    dma_wmb();
}

/* b44_init_hw(FULL_RESET) */
static void init_hw(struct b44 *b, int reset_phy)
{
    chip_reset(b);
    if (reset_phy) phy_setup(b);
    bw32(b, B44_MAC_CTRL, MAC_CTRL_CRC32_ENAB | MAC_CTRL_PHY_LEDCTRL);
    bw32(b, B44_RCV_LAZY, 1u << 24);                    /* an interrupt per frame */
    set_rx_mode(b);
    bw32(b, B44_RXMAXLEN, 1500 + 14 + 8 + RX_HEADER_LEN);
    bw32(b, B44_TXMAXLEN, 1500 + 14 + 8 + RX_HEADER_LEN);
    bw32(b, B44_TX_WMARK, 56);
    bw32(b, B44_DMATX_CTRL, DMATX_CTRL_ENABLE);
    bw32(b, B44_DMATX_ADDR, (uint32_t)b->tx_phys + SSB_PCI_DMA);
    bw32(b, B44_DMARX_CTRL, DMARX_CTRL_ENABLE | RX_PKT_OFFSET << DMARX_CTRL_ROSHIFT);
    bw32(b, B44_DMARX_ADDR, (uint32_t)b->rx_phys + SSB_PCI_DMA);
    /* the engine stops short of this descriptor: all but one are the chip's */
    bw32(b, B44_DMARX_PTR, (RX_DESCS - 1) * sizeof(struct dma_desc));
    bw32(b, B44_MIB_CTRL, MIB_CTRL_CLR_ON_READ);
    bw32(b, B44_ENET_CTRL, br32(b, B44_ENET_CTRL) | ENET_CTRL_ENABLE);
    b->link = 0;
}

static void restart(struct b44 *b, const char *why, uint32_t istat)
{
    kprintf("b44: %s: %s (istat %x, rx stat %x, tx stat %x), resetting\n", b->dev.name, why, istat,
            br32(b, B44_DMARX_STAT), br32(b, B44_DMATX_STAT));
    b->resets++;
    bw32(b, B44_IMASK, 0);
    init_rings(b);
    init_hw(b, 0);
    phy_check(b);
    bw32(b, B44_IMASK, ISTAT_ERRORS | ISTAT_RX | ISTAT_TX);
}

/* ---- the data path ------------------------------------------------------------ */

static void tx_reap(struct b44 *b)
{
    uint32_t cur = (br32(b, B44_DMATX_STAT) & DMA_STAT_CDMASK) / sizeof(struct dma_desc);
    b->tx_cons = cur % TX_DESCS;
}

static int b44_xmit(struct netdev *d, struct pkt *p)
{
    struct b44 *b = d->priv;
    if (p->len > BUF_SIZE || p->len > DESC_CTRL_LEN) { pkt_free(p); return -EMSGSIZE; }
    uint64_t f = spin_lock_irqsave(&b->lock);
    uint32_t i = b->tx_prod, next = (i + 1) % TX_DESCS;
    if (next == b->tx_cons) tx_reap(b);
    if (next == b->tx_cons) {
        spin_unlock_irqrestore(&b->lock, f);
        d->tx_errors++;
        pkt_free(p);
        return -ENOBUFS;
    }
    uint32_t len = p->len < 60 ? 60 : p->len;           /* the chip does not pad runts */
    memcpy(b->tx_buf[i], pkt_data(p), p->len);
    if (len > p->len) memset(b->tx_buf[i] + p->len, 0, len - p->len);
    b->tx[i].addr = htole32((uint32_t)b->tx_buf_phys[i] + SSB_PCI_DMA);
    b->tx[i].ctrl = htole32(len | DESC_CTRL_IOC | DESC_CTRL_SOF | DESC_CTRL_EOF | (i == TX_DESCS - 1 ? DESC_CTRL_EOT : 0));
    b->tx_prod = next;
    dma_wmb();                                          /* descriptor and buffer before the doorbell */
    bw32(b, B44_DMATX_PTR, next * sizeof(struct dma_desc));
    spin_unlock_irqrestore(&b->lock, f);
    pkt_free(p);
    return 0;
}

/* Called by the netrx thread with net_lock held (on an interrupt, and
 * every 20 ms regardless). */
static void b44_poll(struct netdev *d)
{
    struct b44 *b = d->priv;
    uint32_t istat = br32(b, B44_ISTAT);
    if (istat) bw32(b, B44_ISTAT, istat);
    if (istat & ISTAT_ERRORS) { restart(b, "DMA error", istat); return; }

    uint32_t cur = (br32(b, B44_DMARX_STAT) & DMA_STAT_CDMASK) / sizeof(struct dma_desc);
    int n = 0;
    while (b->rx_cons != cur && n++ < RX_DESCS) {
        uint32_t i = b->rx_cons;
        struct rx_header *rh = (struct rx_header *)b->rx_buf[i];
        dma_rmb();
        uint16_t len = le16toh(rh->len);
        for (int t = 0; len == 0 && t < 5; t++) { udelay_(b, 2); len = le16toh(((volatile struct rx_header *)rh)->len); }
        uint16_t flags = le16toh(rh->flags);
        if (len >= 14 + 4 && len <= RX_BUF_LEN - RX_PKT_OFFSET && !(flags & RX_FLAG_ERRORS)) {
            struct pkt *p = pkt_alloc();
            if (p) {
                memcpy(pkt_data(p), b->rx_buf[i] + RX_PKT_OFFSET, len - 4u);     /* without the CRC */
                p->len = len - 4u;
                net_rx(d, p);
            } else {
                d->rx_dropped++;
            }
        } else {
            d->rx_dropped++;
        }
        rh->len = 0;
        rh->flags = 0;
        b->rx_cons = (i + 1) % RX_DESCS;
        dma_wmb();
        bw32(b, B44_DMARX_PTR, ((b->rx_cons + RX_DESCS - 1) % RX_DESCS) * sizeof(struct dma_desc));
        cur = (br32(b, B44_DMARX_STAT) & DMA_STAT_CDMASK) / sizeof(struct dma_desc);
    }
    if (istat & ISTAT_RDU) restart(b, "receive ring ran dry", istat);

    uint64_t now = timer_ms();
    if (now - b->last_phy_ms >= 1000) { b->last_phy_ms = now; phy_check(b); }
}

static struct b44 *nics[2];
static int nic_count;

static void b44_irq(struct interrupt_frame *f)
{
    (void)f;
    for (int k = 0; k < nic_count; k++) {
        struct b44 *b = nics[k];
        if (br32(b, B44_ISTAT) & br32(b, B44_IMASK)) {
            bw32(b, B44_IMASK, 0);                      /* poll acknowledges; unmasked again below */
            b->dev.rx_pending = 1;
            netdev_kick(&b->dev);
        }
    }
}

/* ---- probe -------------------------------------------------------------------- */

static uint64_t alloc_low(size_t pages)
{
    uint64_t p = pmm_alloc_pages_below(pages, DMA_LIMIT);
    if (p) memset(P2V(p), 0, pages * 4096);
    return p;
}

static int b44_setup(const struct pci_dev *pd)
{
    struct b44 *b = kzalloc(sizeof(*b));
    if (!b) return -1;
    b->pd = pd;
    pci_enable_busmaster(pd);
    b->regs = vmm_map_mmio(pd->bar[0], 0x2000);

    /* the cores: Ethernet, PCI and a modem, at the start of the enumeration space */
    b->eth_core = b->pci_core = -1;
    for (int i = 0; i < 3; i++) {
        switch_core(b, i);
        uint32_t idh = br32(b, SSB_IDHIGH);
        uint32_t id = (idh & 0x8FF0) >> 4;
        uint32_t rev = (idh & 0x000F) | ((idh & 0x7000) >> 8);
        if (id == SSB_DEV_ETHERNET) b->eth_core = i;
        if (id == SSB_DEV_PCI) { b->pci_core = i; b->pci_rev = rev; }
    }
    if (b->eth_core < 0 || b->pci_core < 0) {
        kprintf("b44: %02x:%02x.%u: no Ethernet/PCI core on the backplane\n", pd->bus, pd->slot, pd->func);
        return -1;
    }
    /* ssb_pcicore_init_clientmode: the SPROM shadow's core index, PCI interrupts off */
    switch_core(b, b->pci_core);
    uint16_t sp0 = mmio_read16(b->regs + PCICORE_SPROM0);
    if (((sp0 & 0xF000) >> 12) != (uint32_t)b->pci_core)
        mmio_write16(b->regs + PCICORE_SPROM0, (uint16_t)((sp0 & ~0xF000) | b->pci_core << 12));
    bw32(b, SSB_INTVEC, 0);
    switch_core(b, b->eth_core);

    /* SPROM rev 1-2: the MAC as three big-endian words, the PHY address */
    for (int i = 0; i < 3; i++) {
        uint16_t w = sprom16(b, SPROM1_ET0MAC + 2 * i);
        b->dev.mac[2 * i] = (uint8_t)(w >> 8);
        b->dev.mac[2 * i + 1] = (uint8_t)w;
    }
    b->phy_addr = (uint8_t)(sprom16(b, SPROM1_ETHPHY) & 0x1F);
    if ((b->dev.mac[0] & 1) || !(b->dev.mac[0] | b->dev.mac[1] | b->dev.mac[2] | b->dev.mac[3] | b->dev.mac[4] | b->dev.mac[5])) {
        kprintf("b44: no valid MAC address in the SPROM\n");
        return -1;
    }

    /* rings and buffers below 1 GiB */
    b->rx_phys = alloc_low(1);
    b->tx_phys = alloc_low(1);
    uint64_t rxb = alloc_low(RX_DESCS / 2), txb = alloc_low(TX_DESCS / 2);
    if (!b->rx_phys || !b->tx_phys || !rxb || !txb) { kprintf("b44: no memory below 1 GiB for DMA\n"); return -1; }
    b->rx = P2V(b->rx_phys);
    b->tx = P2V(b->tx_phys);
    for (int i = 0; i < RX_DESCS; i++) { b->rx_buf_phys[i] = rxb + (uint64_t)i * BUF_SIZE; b->rx_buf[i] = P2V(b->rx_buf_phys[i]); }
    for (int i = 0; i < TX_DESCS; i++) { b->tx_buf_phys[i] = txb + (uint64_t)i * BUF_SIZE; b->tx_buf[i] = P2V(b->tx_buf_phys[i]); }

    char name[IFNAMSIZ] = "eth0";
    for (int i = 0; i < 10 && netdev_by_name(name); i++) name[3] = (char)('1' + i);
    strcpy(b->dev.name, name);

    init_rings(b);
    init_hw(b, 1);

    b->dev.priv = b;
    b->dev.xmit = b44_xmit;
    b->dev.poll = b44_poll;
    b->dev.poll_always = 1;
    b->dev.mtu = 1500;
    b->dev.flags = IFF_UP | IFF_BROADCAST;
    nics[nic_count++] = b;
    netdev_register(&b->dev);

    b->irq = pci_irq(pd);
    kprintf("b44: %s at %02x:%02x.%u, %02x:%02x:%02x:%02x:%02x:%02x, phy %u, pci core rev %u, irq %d\n",
            b->dev.name, pd->bus, pd->slot, pd->func, b->dev.mac[0], b->dev.mac[1], b->dev.mac[2], b->dev.mac[3],
            b->dev.mac[4], b->dev.mac[5], b->phy_addr, b->pci_rev, b->irq);
    if (b->irq >= 0) {
        irq_install((uint8_t)b->irq, b44_irq);
        irq_unmask_pci((uint8_t)b->irq);
    }
    bw32(b, B44_IMASK, ISTAT_ERRORS | ISTAT_RX | ISTAT_TX);
    return 0;
}

/* The poll unmasks what the interrupt masked. */
static void b44_poll_unmask(struct netdev *d)
{
    struct b44 *b = d->priv;
    b44_poll(d);
    bw32(b, B44_IMASK, ISTAT_ERRORS | ISTAT_RX | ISTAT_TX);
}

void b44_init(void)
{
    for (size_t i = 0; i < pci_count() && nic_count < 2; i++) {
        const struct pci_dev *pd = pci_get(i);
        if (pd->vendor != 0x14E4 || (pd->device != 0x4401 && pd->device != 0x4402 && pd->device != 0x170C))
            continue;
        if (b44_setup(pd) == 0) nics[nic_count - 1]->dev.poll = b44_poll_unmask;
    }
}

/* For `cat /proc/net/b44`-style debugging from the shell: what the chip says. */
void b44_dump(void)
{
    for (int k = 0; k < nic_count; k++) {
        struct b44 *b = nics[k];
        kprintf("b44: %s: istat %x imask %x rx stat %x (cons %u) tx stat %x (prod %u) enet %x rxcfg %x link %d resets %u\n",
                b->dev.name, br32(b, B44_ISTAT), br32(b, B44_IMASK), br32(b, B44_DMARX_STAT), b->rx_cons,
                br32(b, B44_DMATX_STAT), b->tx_prod, br32(b, B44_ENET_CTRL), br32(b, B44_RXCONFIG), b->link, b->resets);
    }
}
