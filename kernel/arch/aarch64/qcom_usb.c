/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* USB on a Qualcomm phone: the DWC3 controller behind "qcom,dwc-usb3-msm",
 * as a USB serial device (CDC ACM) the computer at the other end of the
 * cable sees as a modem port (/dev/cu.usbmodem* on a Mac, /dev/ttyACM* on
 * Linux). Here it is /dev/ttyGS0, with echo and line editing, and init
 * keeps a shell on it.
 *
 * The bootloader leaves the controller powered, clocked and its PHY set
 * up (fastboot ran on it); this takes it from there: a soft reset, high
 * speed device mode, VBUS reported valid through the Qualcomm glue, an
 * event buffer and the TRB rings in memory, and a kernel thread that polls
 * the events. The controller does not snoop the CPU's caches: everything
 * it reads is cleaned to memory first, everything it writes invalidated
 * before it is read. DMA goes through the SMMU as the bootloader left it. */
#include "asm/fdt.h"
#include "asm/memlayout.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "proc/sched.h"
#include "proc/wait.h"
#include "spinlock.h"
#include "endian.h"
#include "string.h"
#include "printf.h"
#include "abi/abi.h"

/* The clock controller (SM6115/KHAJE offsets). */
#define GCC_USB30_PRIM_GDSC     0x1a004
#define GCC_USB30_MASTER_CBCR   0x1a010
#define CBCR_OFF                (1u << 31)

/* Qualcomm glue (QSCRATCH, at the controller + 0xf8800). */
#define QSCRATCH                0xf8800
#define QS_HS_PHY_CTRL          0x10
#define QS_SS_PHY_CTRL          0x30
#define UTMI_OTG_VBUS_VALID     (1u << 20)
#define SW_SESSVLD_SEL          (1u << 28)
#define LANE0_PWR_PRESENT       (1u << 24)

/* DWC3 */
#define GCTL            0xc110
#define GSNPSID         0xc120
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTCOUNT      0xc40c
#define DCFG            0xc700
#define DCTL            0xc704
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DALEPENA        0xc720
#define DEP(n)          (0xc800 + (n) * 0x10)
#define DEPCMDPAR2      0x00
#define DEPCMDPAR1      0x04
#define DEPCMDPAR0      0x08
#define DEPCMD          0x0c

#define DCTL_RUN_STOP   (1u << 31)
#define DCTL_CSFTRST    (1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)

#define CMD_SETEPCONFIG     1
#define CMD_SETXFERRES      2
#define CMD_SETSTALL        4
#define CMD_STARTXFER       6
#define CMD_ENDXFER         8
#define CMD_STARTCFG        9
#define CMD_ACT             (1u << 10)
#define CMD_IOC             (1u << 8)

#define TRB_HWO         (1u << 0)
#define TRB_LST         (1u << 1)
#define TRB_ISP_IMI     (1u << 10)
#define TRB_IOC         (1u << 11)
#define TRBCTL_NORMAL   (1u << 4)
#define TRBCTL_SETUP    (2u << 4)
#define TRBCTL_STATUS2  (3u << 4)
#define TRBCTL_STATUS3  (4u << 4)
#define TRBCTL_DATA     (5u << 4)

#define EVT_XFERCOMPLETE 1
#define EVT_XFERNOTREADY 3
#define DEV_DISCONNECT   0
#define DEV_RESET        1
#define DEV_CONNECTDONE  2

/* Physical endpoints: 0/1 control, 2/3 bulk out/in (EP1), 5 interrupt in (EP2). */
#define EP0_OUT 0
#define EP0_IN  1
#define EP_BOUT 2
#define EP_BIN  3
#define EP_NOTIFY 5

struct trb { uint32_t bpl, bph, size, ctrl; };

static volatile uint8_t *dwc;
static uint64_t dma_phys;          /* one page each: events, TRBs, ep0 buffer, bulk out, bulk in */
static uint8_t *dma;
#define OFF_EVT     0x0000
#define OFF_TRB     0x1000          /* one TRB per physical endpoint, 16 bytes apart */
#define OFF_EP0BUF  0x2000
#define OFF_OUTBUF  0x3000
#define OFF_INBUF   0x4000
#define DMA_PAGES   5
#define EVT_SIZE    4096
static uint32_t evt_pos;
static uint32_t rsc[8];            /* each endpoint's transfer resource index */

static uint32_t rd(uint32_t off) { return mmio_read32(dwc + off); }
static void wr(uint32_t off, uint32_t v) { mmio_write32(dwc + off, v); }

static void cache_clean(void *p, size_t len)
{
    for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + len; a += 64) __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void cache_inval(void *p, size_t len)
{
    for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + len; a += 64) __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static int ep_cmd(int ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
    wr(DEP(ep) + DEPCMDPAR0, p0);
    wr(DEP(ep) + DEPCMDPAR1, p1);
    wr(DEP(ep) + DEPCMDPAR2, p2);
    wr(DEP(ep) + DEPCMD, cmd | CMD_ACT);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(DEP(ep) + DEPCMD);
        if (!(v & CMD_ACT)) {
            if ((cmd & 0xf) == CMD_STARTXFER) rsc[ep] = (v >> 16) & 0x7f;
            return (v >> 12) & 0xf ? -1 : 0;
        }
    }
    return -1;
}

/* One TRB on an endpoint and start it. */
static int start_xfer(int ep, uint64_t buf, uint32_t len, uint32_t type)
{
    struct trb *t = (struct trb *)(dma + OFF_TRB + ep * 16);
    t->bpl = (uint32_t)buf; t->bph = (uint32_t)(buf >> 32);
    t->size = len;
    t->ctrl = type | TRB_LST | TRB_IOC | TRB_ISP_IMI | TRB_HWO;
    cache_clean(t, sizeof *t);
    uint64_t tp = dma_phys + OFF_TRB + ep * 16;
    return ep_cmd(ep, CMD_STARTXFER, (uint32_t)(tp >> 32), (uint32_t)tp, 0);
}

static uint32_t trb_left(int ep)
{
    struct trb *t = (struct trb *)(dma + OFF_TRB + ep * 16);
    cache_inval(t, sizeof *t);
    return t->size & 0xffffff;
}

/* ---- descriptors (a CDC ACM function) ------------------------------------------- */

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,    /* USB 2.0, class CDC, ep0 64 */
    0x25, 0x05, 0xa7, 0xa4, 0x00, 0x01,         /* 0525:a4a7 (the Linux gadget serial ids), bcdDevice 1.00 */
    1, 2, 3, 1,
};
static const uint8_t qual_desc[10] = { 10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0 };
static const uint8_t cfg_desc[] = {
    9, 2, 67, 0, 2, 1, 0, 0x80, 250,            /* 2 interfaces, bus powered, 500 mA */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,         /* interface 0: communications, ACM, AT commands */
    5, 0x24, 0x00, 0x10, 0x01,                  /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 1,                     /* call management: data on interface 1 */
    4, 0x24, 0x02, 0x02,                        /* ACM: line coding and serial state */
    5, 0x24, 0x06, 0, 1,                        /* union: 0 controls 1 */
    7, 5, 0x82, 0x03, 16, 0, 9,                 /* EP2 IN interrupt, 16 bytes */
    9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,         /* interface 1: data */
    7, 5, 0x01, 0x02, 0x00, 0x02, 0,            /* EP1 OUT bulk 512 */
    7, 5, 0x81, 0x02, 0x00, 0x02, 0,            /* EP1 IN bulk 512 */
};
static const char *const strings[] = { NULL, "Rigby Foundation", "sic", "0001" };
static uint8_t line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 */

/* ---- the serial line --------------------------------------------------------------- */

#define RING 8192
static uint8_t txring[RING]; static uint32_t tx_head, tx_tail;   /* to the host */
static uint8_t rxline[512]; static uint32_t rx_len;              /* being edited */
static uint8_t rxring[RING]; static uint32_t rx_head, rx_tail;   /* finished lines, for readers */
static spinlock_t lock = SPINLOCK_INIT;
static struct waitqueue rx_wq = WAITQUEUE_INIT, tx_wq = WAITQUEUE_INIT;
static int configured, in_busy;

static void tx_put_locked(uint8_t c)
{
    if (tx_head - tx_tail < RING) txring[tx_head++ % RING] = c;
}

static void tx_put(const uint8_t *p, size_t n)
{
    uint64_t f = spin_lock_irqsave(&lock);
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') tx_put_locked('\r');
        tx_put_locked(p[i]);
    }
    spin_unlock_irqrestore(&lock, f);
}

/* What the host typed: echoed, edited, delivered a line at a time. */
static void rx_char(uint8_t c)
{
    if (c == '\r') c = '\n';
    if (c == 0x7f || c == '\b') {
        if (rx_len) { rx_len--; tx_put((const uint8_t *)"\b \b", 3); }
        return;
    }
    if (c == 0x03) { rx_len = 0; tx_put((const uint8_t *)"^C\n", 3); return; }
    if (c == '\n' || rx_len < sizeof rxline - 1) {
        rxline[rx_len++] = c;
        tx_put(&c, 1);
    }
    if (c == '\n') {
        uint64_t f = spin_lock_irqsave(&lock);
        for (uint32_t i = 0; i < rx_len && rx_head - rx_tail < RING; i++) rxring[rx_head++ % RING] = rxline[i];
        spin_unlock_irqrestore(&lock, f);
        rx_len = 0;
        waitqueue_wake_all(&rx_wq);
    }
}

static long tty_read(struct file *f, void *buf, size_t len)
{
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&lock);
        size_t n = 0;
        while (n < len && rx_tail != rx_head) {
            uint8_t c = rxring[rx_tail++ % RING];
            ((uint8_t *)buf)[n++] = c;
            if (c == '\n') break;
        }
        spin_unlock_irqrestore(&lock, fl);
        if (n) return (long)n;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (wait_event_interruptible(&rx_wq, rx_tail != rx_head) < 0) return -EINTR;
    }
}

static long tty_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < 256 ? len - done : 256;
        wait_event_timeout(&tx_wq, tx_head - tx_tail < RING - 600, 200);   /* a host not reading: drop rather than hang */
        tx_put(p + done, n);
        done += n;
    }
    return (long)len;
}

static int tty_poll(struct file *f, struct waitqueue **wq)
{
    (void)f;
    *wq = &rx_wq;
    return (rx_tail != rx_head ? POLLIN : 0) | POLLOUT;
}

static const struct dev_ops tty_ops = { .read = tty_read, .write = tty_write, .poll = tty_poll };

/* ---- endpoint 0 ------------------------------------------------------------------- */

enum { EP0_SETUP, EP0_DATA, EP0_STATUS, EP0_STATUS_RUN } ep0_state;   /* STATUS: waiting for the host's status phase */
static int ep0_in_dir;             /* the data stage goes to the host */
static int ep0_has_data;           /* three-stage */
static uint8_t pending_addr;
static int pending_config, pending_line_coding;

static void ep0_setup_arm(void)
{
    ep0_state = EP0_SETUP;
    cache_inval(dma + OFF_EP0BUF, 64);
    start_xfer(EP0_OUT, dma_phys + OFF_EP0BUF, 8, TRBCTL_SETUP);
}

static void ep0_stall(void)
{
    ep_cmd(EP0_OUT, CMD_SETSTALL, 0, 0, 0);
    ep0_setup_arm();
}

static void configure_data_eps(void);

static void ep0_data_in(const void *p, uint32_t len, uint32_t want)
{
    if (len > want) len = want;
    memcpy(dma + OFF_EP0BUF, p, len);
    cache_clean(dma + OFF_EP0BUF, 64 + len);
    ep0_state = EP0_DATA; ep0_in_dir = 1; ep0_has_data = 1;
    start_xfer(EP0_IN, dma_phys + OFF_EP0BUF, len, TRBCTL_DATA);
}

static void handle_setup(const uint8_t *s)
{
    uint8_t type = s[0], req = s[1];
    uint16_t val = (uint16_t)(s[2] | s[3] << 8), len = (uint16_t)(s[6] | s[7] << 8);
    ep0_has_data = len != 0;
    ep0_in_dir = (type & 0x80) != 0;
    if ((type & 0x60) == 0) {                                    /* standard */
        switch (req) {
        case 6: {                                                /* GET_DESCRIPTOR */
            uint8_t kind = (uint8_t)(val >> 8), n = (uint8_t)val;
            if (kind == 1) { ep0_data_in(dev_desc, sizeof dev_desc, len); return; }
            if (kind == 2) { ep0_data_in(cfg_desc, sizeof cfg_desc, len); return; }
            if (kind == 6) { ep0_data_in(qual_desc, sizeof qual_desc, len); return; }
            if (kind == 3) {
                uint8_t d[64];
                if (n == 0) { d[0] = 4; d[1] = 3; d[2] = 0x09; d[3] = 0x04; ep0_data_in(d, 4, len); return; }
                if (n < sizeof strings / sizeof strings[0]) {
                    size_t l = strlen(strings[n]);
                    d[0] = (uint8_t)(2 + 2 * l); d[1] = 3;
                    for (size_t i = 0; i < l; i++) { d[2 + 2 * i] = (uint8_t)strings[n][i]; d[3 + 2 * i] = 0; }
                    ep0_data_in(d, d[0], len);
                    return;
                }
            }
            ep0_stall();
            return;
        }
        case 5: pending_addr = (uint8_t)(val & 0x7f); break;    /* SET_ADDRESS: applied at once, before the status stage */
        case 9: pending_config = val; break;                     /* SET_CONFIGURATION */
        case 8: { uint8_t c = configured; ep0_data_in(&c, 1, len); return; }        /* GET_CONFIGURATION */
        case 0: { uint8_t z[2] = { 0, 0 }; ep0_data_in(z, 2, len); return; }        /* GET_STATUS */
        case 1: case 3: case 11: break;                          /* CLEAR/SET_FEATURE, SET_INTERFACE: fine */
        case 10: { uint8_t z = 0; ep0_data_in(&z, 1, len); return; }                /* GET_INTERFACE */
        default: ep0_stall(); return;
        }
        if (req == 5) {
            uint32_t d = rd(DCFG);
            wr(DCFG, (d & ~(0x7fu << 3)) | (uint32_t)pending_addr << 3);
        }
    } else if ((type & 0x60) == 0x20) {                          /* class: ACM */
        switch (req) {
        case 0x20:                                               /* SET_LINE_CODING: 7 bytes follow */
            pending_line_coding = 1;
            ep0_state = EP0_DATA; ep0_in_dir = 0;
            cache_inval(dma + OFF_EP0BUF, 64);
            start_xfer(EP0_OUT, dma_phys + OFF_EP0BUF, 64, TRBCTL_DATA);
            return;
        case 0x21: ep0_data_in(line_coding, 7, len); return;     /* GET_LINE_CODING */
        case 0x22: break;                                        /* SET_CONTROL_LINE_STATE */
        default: ep0_stall(); return;
        }
    } else {
        ep0_stall();
        return;
    }
    if (ep0_has_data) { ep0_stall(); return; }                   /* (an OUT data stage we did not expect) */
    ep0_state = EP0_STATUS;                                      /* two-stage: wait for the host's status phase */
}

static void ep0_event(int ep, int event, int status)
{
    if (event == EVT_XFERNOTREADY) {
        /* The host wants the status phase (status 2): start it on the
         * endpoint it asks on, once we have finished with the request. */
        if ((status & 3) == 2 && ep0_state == EP0_STATUS) {
            start_xfer(ep, dma_phys + OFF_EP0BUF, 0, ep0_has_data ? TRBCTL_STATUS3 : TRBCTL_STATUS2);
            ep0_state = EP0_STATUS_RUN;
        }
        return;
    }
    if (event != EVT_XFERCOMPLETE) return;
    switch (ep0_state) {
    case EP0_SETUP:
        if (ep == EP0_OUT) {
            uint8_t setup[8];
            cache_inval(dma + OFF_EP0BUF, 64);
            memcpy(setup, dma + OFF_EP0BUF, 8);
            handle_setup(setup);
        }
        return;
    case EP0_DATA:
        if (!ep0_in_dir && pending_line_coding) {
            cache_inval(dma + OFF_EP0BUF, 64);
            memcpy(line_coding, dma + OFF_EP0BUF, 7);
            pending_line_coding = 0;
        }
        ep0_state = EP0_STATUS;
        return;
    case EP0_STATUS_RUN:
        if (pending_config >= 0 && pending_config != configured) {
            configured = pending_config;
            if (configured) configure_data_eps();
        }
        ep0_setup_arm();
        return;
    default:
        return;
    }
}

/* ---- the data endpoints ------------------------------------------------------------- */

static void out_arm(void)
{
    cache_inval(dma + OFF_OUTBUF, 512);
    start_xfer(EP_BOUT, dma_phys + OFF_OUTBUF, 512, TRBCTL_NORMAL);
}

static void ep_config(int ep, int type, int mps, int fifo)
{
    uint32_t p0 = (uint32_t)type << 1 | (uint32_t)mps << 3 | (uint32_t)fifo << 17;
    uint32_t p1 = (1u << 8) | (1u << 10) | (uint32_t)ep << 25;   /* XferComplete, XferNotReady events; its number */
    ep_cmd(ep, CMD_SETEPCONFIG, p0, p1, 0);
    ep_cmd(ep, CMD_SETXFERRES, 1, 0, 0);
    wr(DALEPENA, rd(DALEPENA) | 1u << ep);
}

static void configure_data_eps(void)
{
    ep_cmd(EP0_OUT, CMD_STARTCFG | (2u << 16), 0, 0, 0);       /* resources from index 2 on */
    ep_config(EP_BOUT, 2, 512, 0);
    ep_config(EP_BIN, 2, 512, 1);
    ep_config(EP_NOTIFY, 3, 16, 2);
    in_busy = 0;
    out_arm();
    kprintf("usb: configured: /dev/ttyGS0 is up\n");
}

static void in_kick(void)
{
    if (!configured || in_busy) return;
    uint64_t f = spin_lock_irqsave(&lock);
    uint32_t n = 0;
    while (tx_tail != tx_head && n < 4096) dma[OFF_INBUF + n++] = txring[tx_tail++ % RING];
    spin_unlock_irqrestore(&lock, f);
    if (!n) return;
    cache_clean(dma + OFF_INBUF, n);
    in_busy = 1;
    start_xfer(EP_BIN, dma_phys + OFF_INBUF, n, TRBCTL_NORMAL);
    waitqueue_wake_all(&tx_wq);
}

static void data_event(int ep, int event)
{
    if (event != EVT_XFERCOMPLETE) return;
    if (ep == EP_BOUT) {
        uint32_t got = 512 - trb_left(EP_BOUT);
        cache_inval(dma + OFF_OUTBUF, 512);
        for (uint32_t i = 0; i < got; i++) rx_char(dma[OFF_OUTBUF + i]);
        out_arm();
    } else if (ep == EP_BIN) {
        in_busy = 0;
    }
}

/* ---- events ------------------------------------------------------------------------- */

static void end_xfer(int ep)
{
    if (rsc[ep]) ep_cmd(ep, CMD_ENDXFER | (1u << 11) | rsc[ep] << 16, 0, 0, 0);
    rsc[ep] = 0;
}

static void reset_state(void)
{
    if (configured) { end_xfer(EP_BOUT); end_xfer(EP_BIN); }
    configured = 0; pending_config = -1; in_busy = 0;
    wr(DCFG, rd(DCFG) & ~(0x7fu << 3));
    wr(DALEPENA, 3);
}

static void handle_event(uint32_t e)
{
    if (e & 1) {                                                 /* a device event */
        uint32_t type = (e >> 8) & 0xf;
        if (type == DEV_RESET) reset_state();
        else if (type == DEV_CONNECTDONE) {
            uint32_t mps = 64;                                   /* high or full speed alike */
            ep_cmd(EP0_OUT, CMD_SETEPCONFIG, mps << 3 | (2u << 30), (1u << 8) | (1u << 10) | (0u << 25), 0);
            ep_cmd(EP0_IN, CMD_SETEPCONFIG, mps << 3 | (2u << 30), (1u << 8) | (1u << 10) | (1u << 25), 0);
        } else if (type == DEV_DISCONNECT) reset_state();
        return;
    }
    int ep = (int)((e >> 1) & 0x1f), event = (int)((e >> 6) & 0xf), status = (int)((e >> 12) & 0xf);
    if (ep <= 1) ep0_event(ep, event, status);
    else data_event(ep, event);
}

static void usb_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t n = rd(GEVNTCOUNT) & 0xfffc;
        if (n) {
            cache_inval(dma + OFF_EVT, EVT_SIZE);
            for (uint32_t done = 0; done < n; done += 4) {
                uint32_t e = *(volatile uint32_t *)(dma + OFF_EVT + evt_pos);
                evt_pos = (evt_pos + 4) % EVT_SIZE;
                handle_event(e);
            }
            wr(GEVNTCOUNT, n);
        }
        in_kick();
        task_sleep_ms(1);
    }
}

/* ---- bring-up ---------------------------------------------------------------------- */

void qcom_usb_probe(void)
{
    int gccn = fdt_find_compatible("qcom,khaje-gcc");
    if (gccn < 0) gccn = fdt_find_compatible("qcom,gcc-sm6115");
    int usbn = fdt_find_compatible("qcom,dwc-usb3-msm");
    uint64_t gcc_phys, usb_phys;
    if (gccn < 0 || usbn < 0 || fdt_reg(gccn, 0, &gcc_phys, NULL) != 0 || fdt_reg(usbn, 0, &usb_phys, NULL) != 0) return;
    volatile uint8_t *gcc = P2V(gcc_phys);
    uint32_t gdsc = mmio_read32(gcc + GCC_USB30_PRIM_GDSC);
    uint32_t m = mmio_read32(gcc + GCC_USB30_MASTER_CBCR);
    if (!(gdsc & (1u << 31)) || (m & CBCR_OFF)) {
        kprintf("usb: power domain %08x, master clock %08x: the controller is not running, leaving it alone\n", gdsc, m);
        return;
    }
    dwc = P2V(usb_phys);
    uint32_t id = rd(GSNPSID);
    if ((id & 0xffff0000) != 0x55330000) { kprintf("usb: no DWC3 (id %08x)\n", id); return; }

    dma_phys = pmm_alloc_pages_below(DMA_PAGES, 0x100000000ULL);
    if (!dma_phys) return;
    dma = P2V(dma_phys);
    memset(dma, 0, DMA_PAGES * 4096);
    cache_clean(dma, DMA_PAGES * 4096);

    /* VBUS is there as far as the controller is concerned (the PMIC sees the cable, not it). */
    volatile uint8_t *qs = P2V(usb_phys + QSCRATCH);
    mmio_write32(qs + QS_HS_PHY_CTRL, mmio_read32(qs + QS_HS_PHY_CTRL) | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
    mmio_write32(qs + QS_SS_PHY_CTRL, mmio_read32(qs + QS_SS_PHY_CTRL) | LANE0_PWR_PRESENT);

    wr(DCTL, rd(DCTL) & ~DCTL_RUN_STOP);
    for (int i = 0; i < 100000 && !(rd(DSTS) & DSTS_DEVCTRLHLT); i++) ;
    wr(DCTL, DCTL_CSFTRST);
    for (int i = 0; i < 1000000 && (rd(DCTL) & DCTL_CSFTRST); i++) ;
    wr(GCTL, (rd(GCTL) & ~(3u << 12)) | (2u << 12));            /* device */

    wr(GEVNTADRLO, (uint32_t)(dma_phys + OFF_EVT));
    wr(GEVNTADRHI, (uint32_t)((dma_phys + OFF_EVT) >> 32));
    wr(GEVNTSIZ, EVT_SIZE | (1u << 31));                         /* masked: we poll */
    wr(GEVNTCOUNT, rd(GEVNTCOUNT) & 0xfffc);
    evt_pos = 0;
    wr(DCFG, (rd(DCFG) & ~(7u | (0x7fu << 3) | (1u << 22))) | 0);   /* high speed, address 0, no LPM */
    wr(DEVTEN, (1u << 0) | (1u << 1) | (1u << 2));               /* disconnect, reset, connect done */

    ep_cmd(EP0_OUT, CMD_STARTCFG, 0, 0, 0);
    ep_cmd(EP0_OUT, CMD_SETEPCONFIG, 64u << 3, (1u << 8) | (1u << 10) | (0u << 25), 0);
    ep_cmd(EP0_IN, CMD_SETEPCONFIG, 64u << 3, (1u << 8) | (1u << 10) | (1u << 25), 0);
    ep_cmd(EP0_OUT, CMD_SETXFERRES, 1, 0, 0);
    ep_cmd(EP0_IN, CMD_SETXFERRES, 1, 0, 0);
    wr(DALEPENA, 3);
    pending_config = -1;
    ep0_setup_arm();

    vfs_mkdev("/dev/ttyGS0", &tty_ops, NULL);
    wr(DCTL, rd(DCTL) | DCTL_RUN_STOP);
    for (int i = 0; i < 100000 && (rd(DSTS) & DSTS_DEVCTRLHLT); i++) ;
    kprintf("usb: dwc3 %08x running as a USB serial device (dsts %08x)\n", id, rd(DSTS));
    task_create("usb", usb_thread, NULL);
}
