/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Intel 8254x (e1000) Ethernet: the classic QEMU NIC and a whole family of
 * real PCI/PCIe parts. Legacy descriptors, one receive and one transmit
 * ring, interrupts on the PCI INTx line from config space. */
#include "drivers/e1000.h"
#include "endian.h"
#include "drivers/pci.h"
#include "net/net.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "asm/irq.h"
#include "asm/timer.h"
#include "string.h"
#include "printf.h"

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL     0x5400
#define REG_RAH     0x5404

#define CTRL_SLU    (1 << 6)
#define CTRL_RST    (1 << 26)
#define STATUS_LU   (1 << 1)
#define RCTL_EN     (1 << 1)
#define RCTL_SBP    (1 << 2)
#define RCTL_UPE    (1 << 3)
#define RCTL_MPE    (1 << 4)
#define RCTL_BAM    (1 << 15)
#define RCTL_BSIZE_2048 0
#define RCTL_SECRC  (1 << 26)
#define TCTL_EN     (1 << 1)
#define TCTL_PSP    (1 << 3)
#define TCTL_CT(n)  ((n) << 4)
#define TCTL_COLD(n) ((n) << 12)
#define ICR_TXDW    (1 << 0)
#define ICR_LSC     (1 << 2)
#define ICR_RXDMT0  (1 << 4)
#define ICR_RXO     (1 << 6)
#define ICR_RXT0    (1 << 7)

#define RX_DESCS 64
#define TX_DESCS 64
#define BUF_SIZE 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length, checksum;
    uint8_t  status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

#define RXD_DD  1
#define RXD_EOP 2
#define TXD_EOP 1
#define TXD_IFCS 2
#define TXD_RS  8
#define TXD_DD  1

struct e1000 {
    volatile uint32_t *regs;
    struct rx_desc *rx;             /* virtual (HHDM) */
    struct tx_desc *tx;
    uint64_t rx_phys, tx_phys;
    uint8_t *rx_buf[RX_DESCS];      /* virtual */
    uint8_t *tx_buf[TX_DESCS];
    uint32_t rx_next, tx_next;
    spinlock_t tx_lock;
    struct netdev dev;
    uint8_t irq;
};

static struct e1000 *nics[4];
static int nic_count;

static inline uint32_t rd(struct e1000 *n, uint32_t reg) { return mmio_read32((const volatile uint8_t *)n->regs + reg); }
static inline void wr(struct e1000 *n, uint32_t reg, uint32_t v) { mmio_write32((volatile uint8_t *)n->regs + reg, v); }

static int eeprom_read(struct e1000 *n, uint8_t addr, uint16_t *out)
{
    wr(n, REG_EERD, 1 | ((uint32_t)addr << 8));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(n, REG_EERD);
        if (v & (1 << 4)) { *out = (uint16_t)(v >> 16); return 0; }
    }
    wr(n, REG_EERD, 1 | ((uint32_t)addr << 2));     /* newer parts: address at bit 2, done at bit 1 */
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(n, REG_EERD);
        if (v & (1 << 1)) { *out = (uint16_t)(v >> 16); return 0; }
    }
    return -1;
}

static void read_mac(struct e1000 *n)
{
    uint16_t w[3];
    if (eeprom_read(n, 0, &w[0]) == 0 && eeprom_read(n, 1, &w[1]) == 0 && eeprom_read(n, 2, &w[2]) == 0 &&
        (w[0] | w[1] | w[2]) != 0) {
        for (int i = 0; i < 3; i++) {
            n->dev.mac[2 * i] = (uint8_t)w[i];
            n->dev.mac[2 * i + 1] = (uint8_t)(w[i] >> 8);
        }
        return;
    }
    uint32_t lo = rd(n, REG_RAL), hi = rd(n, REG_RAH);
    for (int i = 0; i < 4; i++) n->dev.mac[i] = (uint8_t)(lo >> (8 * i));
    n->dev.mac[4] = (uint8_t)hi;
    n->dev.mac[5] = (uint8_t)(hi >> 8);
}

static int e1000_xmit(struct netdev *d, struct pkt *p)
{
    struct e1000 *n = d->priv;
    if (p->len > BUF_SIZE) {
        pkt_free(p);
        return -EMSGSIZE;
    }
    uint64_t f = spin_lock_irqsave(&n->tx_lock);
    uint32_t i = n->tx_next;
    struct tx_desc *td = &n->tx[i];
    /* the ring is full if the slot we want hasn't been sent yet */
    uint32_t head = rd(n, REG_TDH);
    if ((i + 1) % TX_DESCS == head) {
        spin_unlock_irqrestore(&n->tx_lock, f);
        d->tx_errors++;
        pkt_free(p);
        return -ENOBUFS;
    }
    memcpy(n->tx_buf[i], pkt_data(p), p->len);
    td->length = htole16((uint16_t)p->len);
    td->cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    td->status = 0;
    n->tx_next = (i + 1) % TX_DESCS;
    dma_wmb();                      /* descriptor and buffer before the doorbell */
    wr(n, REG_TDT, n->tx_next);
    spin_unlock_irqrestore(&n->tx_lock, f);
    pkt_free(p);
    return 0;
}

/* Called by the netrx thread with net_lock held. */
static void e1000_poll(struct netdev *d)
{
    struct e1000 *n = d->priv;
    for (;;) {
        uint32_t i = n->rx_next;
        struct rx_desc *rd_ = &n->rx[i];
        if (!(rd_->status & RXD_DD))
            break;
        dma_rmb();                  /* status first, then length and data */
        uint16_t len = le16toh(rd_->length);
        if ((rd_->status & RXD_EOP) && !rd_->errors && len >= 14 && len <= PKT_DATA_MAX + PKT_HEADROOM) {
            struct pkt *p = pkt_alloc();
            if (p) {
                memcpy(pkt_data(p), n->rx_buf[i], len);
                p->len = len;
                net_rx(d, p);
            } else {
                d->rx_dropped++;
            }
        } else {
            d->rx_dropped++;
        }
        rd_->status = 0;
        n->rx_next = (i + 1) % RX_DESCS;
        dma_wmb();
        wr(n, REG_RDT, i);          /* give this slot back */
    }
    uint32_t st = rd(n, REG_STATUS);
    if (st & STATUS_LU) d->flags |= IFF_RUNNING; else d->flags &= ~IFF_RUNNING;
}

static void e1000_irq(struct interrupt_frame *f)
{
    (void)f;
    for (int k = 0; k < nic_count; k++) {
        struct e1000 *n = nics[k];
        uint32_t icr = rd(n, REG_ICR);      /* reading acknowledges */
        if (icr & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO | ICR_LSC))
            netdev_kick(&n->dev);
    }
}

static int e1000_setup(const struct pci_dev *pd)
{
    int irq = pci_irq(pd);
    if (irq < 0) {
        kprintf("e1000: no interrupt line for %02x:%02x.%u\n", pd->bus, pd->slot, pd->func);
        return -1;
    }
    struct e1000 *n = kzalloc(sizeof(*n));
    if (!n)
        return -1;
    pci_enable_busmaster(pd);
    n->regs = vmm_map_mmio(pd->bar[0], 0x20000);
    n->irq = (uint8_t)irq;

    wr(n, REG_IMC, 0xFFFFFFFF);
    wr(n, REG_CTRL, rd(n, REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(n, REG_CTRL) & CTRL_RST); i++)
        ;
    wr(n, REG_IMC, 0xFFFFFFFF);
    rd(n, REG_ICR);
    read_mac(n);
    wr(n, REG_CTRL, (rd(n, REG_CTRL) | CTRL_SLU) & ~(1u << 3));    /* link up, no LRST */
    for (int i = 0; i < 128; i++)
        wr(n, REG_MTA + 4 * i, 0);
    wr(n, REG_RAL, (uint32_t)n->dev.mac[0] | (uint32_t)n->dev.mac[1] << 8 | (uint32_t)n->dev.mac[2] << 16 | (uint32_t)n->dev.mac[3] << 24);
    wr(n, REG_RAH, (uint32_t)n->dev.mac[4] | (uint32_t)n->dev.mac[5] << 8 | (1u << 31));

    /* rings: one page each (64 x 16 bytes), buffers two per page */
    n->rx_phys = pmm_alloc_page();
    n->tx_phys = pmm_alloc_page();
    n->rx = P2V(n->rx_phys);
    n->tx = P2V(n->tx_phys);
    memset(n->rx, 0, 4096);
    memset(n->tx, 0, 4096);
    for (int i = 0; i < RX_DESCS; i += 2) {
        uint64_t ph = pmm_alloc_page();
        n->rx_buf[i] = P2V(ph);
        n->rx_buf[i + 1] = P2V(ph + BUF_SIZE);
        n->rx[i].addr = htole64(ph);
        n->rx[i + 1].addr = htole64(ph + BUF_SIZE);
    }
    for (int i = 0; i < TX_DESCS; i += 2) {
        uint64_t ph = pmm_alloc_page();
        n->tx_buf[i] = P2V(ph);
        n->tx_buf[i + 1] = P2V(ph + BUF_SIZE);
        n->tx[i].addr = htole64(ph);
        n->tx[i + 1].addr = htole64(ph + BUF_SIZE);
        n->tx[i].status = n->tx[i + 1].status = TXD_DD;
    }
    dma_wmb();
    wr(n, REG_RDBAL, (uint32_t)n->rx_phys);
    wr(n, REG_RDBAH, (uint32_t)(n->rx_phys >> 32));
    wr(n, REG_RDLEN, RX_DESCS * sizeof(struct rx_desc));
    wr(n, REG_RDH, 0);
    wr(n, REG_RDT, RX_DESCS - 1);
    wr(n, REG_TDBAL, (uint32_t)n->tx_phys);
    wr(n, REG_TDBAH, (uint32_t)(n->tx_phys >> 32));
    wr(n, REG_TDLEN, TX_DESCS * sizeof(struct tx_desc));
    wr(n, REG_TDH, 0);
    wr(n, REG_TDT, 0);
    wr(n, REG_TIPG, 10 | (8 << 10) | (6 << 20));
    wr(n, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT(0x10) | TCTL_COLD(0x40));
    wr(n, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    n->dev.priv = n;
    n->dev.xmit = e1000_xmit;
    n->dev.poll = e1000_poll;
    n->dev.mtu = 1500;
    n->dev.flags = IFF_UP | IFF_BROADCAST | ((rd(n, REG_STATUS) & STATUS_LU) ? IFF_RUNNING : 0);
    strcpy(n->dev.name, "eth0");
    n->dev.name[3] = (char)('0' + nic_count);
    nics[nic_count++] = n;
    netdev_register(&n->dev);
    kprintf("e1000: %s at %02x:%02x.%u, irq %u, link %s\n", n->dev.name, pd->bus, pd->slot, pd->func, n->irq,
            (n->dev.flags & IFF_RUNNING) ? "up" : "down");

    irq_install(n->irq, e1000_irq);
    irq_unmask_pci(n->irq);
    wr(n, REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO | ICR_LSC);
    rd(n, REG_ICR);
    return 0;
}

void e1000_init(void)
{
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x1010, 0x1011, 0x1012, 0x1013, 0x1015, 0x1016, 0x1017,
                                    0x1018, 0x1019, 0x101A, 0x101D, 0x1026, 0x1027, 0x1028, 0x1075, 0x1076,
                                    0x1077, 0x1078, 0x1079, 0x107A, 0x107B, 0x107C, 0x108A, 0x1099, 0x10B5,
                                    0x10D3, 0x10F5, 0x1502, 0x1533, 0x153A, 0x15A2 };
    for (size_t i = 0; i < pci_count() && nic_count < 4; i++) {
        const struct pci_dev *pd = pci_get(i);
        if (pd->vendor != 0x8086 || pd->class != 2 || pd->subclass != 0)
            continue;
        int known = 0;
        for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++)
            if (pd->device == ids[k]) known = 1;
        if (!known)
            continue;
        e1000_setup(pd);
    }
}
