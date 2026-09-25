/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Intel High Definition Audio: the controller (CORB/RIRB command rings, one
 * output stream with a cyclic buffer of fragments, an interrupt per
 * fragment) and enough of the codec to make sound come out: find an
 * output pin that is wired to something, walk its connection list back to
 * a DAC, power the path up, unmute it, and point the DAC at our stream.
 * The stream is 48 kHz 16-bit stereo; dsp.c feeds the ring. */
#include "drivers/sound.h"
#include "drivers/pci.h"
#include "endian.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "asm/irq.h"
#include "asm/timer.h"
#include "proc/sched.h"
#include "string.h"
#include "printf.h"

/* controller registers */
#define GCAP        0x00
#define GCTL        0x08
#define WAKEEN      0x0C
#define STATESTS    0x0E
#define INTCTL      0x20
#define INTSTS      0x24
#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48
#define CORBRP      0x4A
#define CORBCTL     0x4C
#define CORBSIZE    0x4E
#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58
#define RINTCNT     0x5A
#define RIRBCTL     0x5C
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E
#define DPLBASE     0x70
#define DPUBASE     0x74
/* stream descriptor (offset from its base) */
#define SD_CTL      0x00
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

#define GCTL_CRST   (1u << 0)
#define SD_CTL_SRST (1u << 0)
#define SD_CTL_RUN  (1u << 1)
#define SD_CTL_IOCE (1u << 2)
#define SD_STS_BCIS (1u << 2)
#define FMT_48K_16_STEREO 0x0011            /* base 48 kHz, 16 bit, 2 channels */

#define RING_ENTRIES 256
#define FRAGS        16
#define FRAG_SIZE    4096                   /* 21 ms at 48 kHz stereo 16-bit; the ring is 341 ms */
#define STREAM_TAG   1

/* codec verbs (12-bit) and parameters */
#define V_GET_PARAM     0xF00
#define V_GET_CONN_SEL  0xF01
#define V_SET_CONN_SEL  0x701
#define V_GET_CONN_LIST 0xF02
#define V_SET_POWER     0x705
#define V_SET_STREAM    0x706
#define V_SET_PIN_CTL   0x707
#define V_GET_CONFIG    0xF1C
#define V_SET_EAPD      0x70C
#define V_SET_FORMAT    0x200               /* 4-bit verb 0x2, 16-bit payload */
#define V_SET_AMP       0x300               /* 4-bit verb 0x3, 16-bit payload */
#define P_VENDOR        0x00
#define P_SUB_NODES     0x04
#define P_FG_TYPE       0x05
#define P_AW_CAPS       0x09
#define P_PIN_CAPS      0x0C
#define P_IN_AMP_CAPS   0x0D
#define P_CONN_LEN      0x0E
#define P_OUT_AMP_CAPS  0x12

#define AW_TYPE(caps)   (((caps) >> 20) & 0xF)
#define AW_OUT          0
#define AW_IN           1
#define AW_MIXER        2
#define AW_SELECTOR     3
#define AW_PIN          4
#define AW_HAS_IN_AMP   (1u << 1)
#define AW_HAS_OUT_AMP  (1u << 2)
#define AW_CONN_LIST    (1u << 8)
#define AW_POWER_CTL    (1u << 10)
#define PIN_OUT_CAPABLE (1u << 4)
#define PIN_HP_DRIVE    (1u << 6)

struct bdl_entry { uint64_t addr; uint32_t len; uint32_t ioc; } __attribute__((packed));

struct hda {
    volatile uint8_t *regs;
    uint32_t *corb; uint64_t *rirb;         /* one page each */
    uint64_t corb_phys, rirb_phys;
    uint16_t corb_wp, rirb_rp;
    volatile uint8_t *sd;                   /* our output stream descriptor */
    struct bdl_entry *bdl; uint64_t bdl_phys;
    uint64_t ring_phys;
    uint8_t irq, cad;                       /* codec address */
    uint32_t vendor;
    struct pcm_hw pcm;
    uint32_t lpib_last;         /* the link position at the last interrupt (bytes into the ring) */
    uint64_t lpib_total;        /* bytes the link has played since the stream started */
};

static struct hda *card;

static uint32_t rd32(struct hda *h, uint32_t r) { return mmio_read32(h->regs + r); }
static uint16_t rd16(struct hda *h, uint32_t r) { return mmio_read16(h->regs + r); }
static uint8_t  rd8(struct hda *h, uint32_t r)  { return mmio_read8(h->regs + r); }
static void wr32(struct hda *h, uint32_t r, uint32_t v) { mmio_write32(h->regs + r, v); }
static void wr16(struct hda *h, uint32_t r, uint16_t v) { mmio_write16(h->regs + r, v); }
static void wr8(struct hda *h, uint32_t r, uint8_t v)   { mmio_write8(h->regs + r, v); }

static void delay_us(uint32_t us)
{
    uint64_t end = timer_ticks() + us / 1000 + 2;
    while (timer_ticks() < end) __asm__ volatile("" ::: "memory");
}

/* ---- codec commands ---------------------------------------------------------- */

/* Send one verb, wait for its response (polling: this runs at init only). */
static int cmd(struct hda *h, uint8_t nid, uint32_t verb, uint32_t payload, uint32_t *resp)
{
    uint32_t v = (uint32_t)h->cad << 28 | (uint32_t)nid << 20 | (verb << 8 & 0xFFF00) | payload;
    if (verb < 0x10) v = (uint32_t)h->cad << 28 | (uint32_t)nid << 20 | verb << 16 | (payload & 0xFFFF);
    h->corb_wp = (h->corb_wp + 1) % RING_ENTRIES;
    h->corb[h->corb_wp] = htole32(v);
    dma_wmb();
    wr16(h, CORBWP, h->corb_wp);
    for (int i = 0; i < 200000; i++) {                 /* MMIO reads pace this: ~100 ms */
        uint16_t wp = rd16(h, RIRBWP) & 0xFF;
        if (wp != h->rirb_rp) {
            h->rirb_rp = (h->rirb_rp + 1) % RING_ENTRIES;
            uint64_t r = le64toh(h->rirb[h->rirb_rp]);
            if (resp) *resp = (uint32_t)r;
            wr8(h, RIRBSTS, 0x5);                   /* ack response interrupt bits */
            return 0;
        }
    }
    kprintf("hda: codec %u nid %u verb %x: no response (corbwp %x corbrp %x rirbwp %x corbctl %x)\n", h->cad, nid, verb, rd16(h, CORBWP), rd16(h, CORBRP), rd16(h, RIRBWP), rd8(h, CORBCTL));
    return -1;
}

static uint32_t param(struct hda *h, uint8_t nid, uint32_t p)
{
    uint32_t r = 0;
    cmd(h, nid, V_GET_PARAM, p, &r);
    return r;
}

/* The connection list of a widget: up to `max` node ids. */
static int conn_list(struct hda *h, uint8_t nid, uint8_t *out, int max)
{
    uint32_t len = param(h, nid, P_CONN_LEN);
    int n = (int)(len & 0x7F), longform = (len >> 7) & 1, count = 0;
    for (int i = 0; i < n && count < max; i += longform ? 2 : 4) {
        uint32_t r = 0;
        cmd(h, nid, V_GET_CONN_LIST, (uint32_t)i, &r);
        int per = longform ? 2 : 4;
        for (int k = 0; k < per && i + k < n && count < max; k++) {
            uint32_t e = longform ? (r >> (16 * k)) & 0xFFFF : (r >> (8 * k)) & 0xFF;
            int range = longform ? (e >> 15) & 1 : (e >> 7) & 1;         /* "up to" the previous entry */
            uint8_t id = (uint8_t)(longform ? e & 0x7FFF : e & 0x7F);
            if (range && count) {
                for (uint8_t x = out[count - 1] + 1; x <= id && count < max; x++) out[count++] = x;
            } else {
                out[count++] = id;
            }
        }
    }
    return count;
}

/* Set the widget's output (and input, if any) amplifier to unmuted, full gain. */
static void unmute(struct hda *h, uint8_t nid, uint32_t caps)
{
    if (caps & AW_HAS_OUT_AMP) {
        uint32_t ac = param(h, nid, P_OUT_AMP_CAPS);
        uint32_t steps = (ac >> 8) & 0x7F;
        cmd(h, nid, V_SET_AMP, 0xB000 | steps, NULL);        /* output, left+right, gain = max, unmuted */
    }
    if (caps & AW_HAS_IN_AMP) {
        uint32_t ac = param(h, nid, P_IN_AMP_CAPS);
        uint32_t steps = (ac >> 8) & 0x7F;
        for (uint32_t idx = 0; idx < 16; idx++)               /* every input index; harmless past the end */
            cmd(h, nid, V_SET_AMP, 0x7000 | idx << 8 | steps, NULL);
    }
}

/* Depth-first from the pin back to a DAC through the connection lists;
 * records the path (pin first) and the connection index chosen at each hop. */
static int find_dac(struct hda *h, uint8_t nid, uint8_t *path, uint8_t *sel, int depth)
{
    if (depth > 8) return 0;
    uint32_t caps = param(h, nid, P_AW_CAPS);
    path[depth] = nid;
    if (AW_TYPE(caps) == AW_OUT) return depth + 1;
    if (!(caps & AW_CONN_LIST)) return 0;
    uint8_t conns[32];
    int n = conn_list(h, nid, conns, 32);
    for (int i = 0; i < n; i++) {
        int len = find_dac(h, conns[i], path, sel, depth + 1);
        if (len) { sel[depth] = (uint8_t)i; return len; }
    }
    return 0;
}

/* Choose and wire an output: speaker first, then line out, then headphones. */
static int codec_setup(struct hda *h)
{
    uint32_t sub = param(h, 0, P_SUB_NODES);
    uint8_t fg_start = (sub >> 16) & 0xFF, fg_count = sub & 0xFF, afg = 0;
    for (uint8_t i = 0; i < fg_count; i++)
        if ((param(h, fg_start + i, P_FG_TYPE) & 0xFF) == 1) { afg = fg_start + i; break; }
    if (!afg) { kprintf("hda: codec %u has no audio function group\n", h->cad); return -1; }
    cmd(h, afg, V_SET_POWER, 0, NULL);                        /* D0 */
    sub = param(h, afg, P_SUB_NODES);
    uint8_t w_start = (sub >> 16) & 0xFF, w_count = sub & 0xFF;

    uint8_t best = 0; int best_rank = 99;
    for (uint8_t i = 0; i < w_count; i++) {
        uint8_t nid = w_start + i;
        uint32_t caps = param(h, nid, P_AW_CAPS);
        if (AW_TYPE(caps) != AW_PIN) continue;
        if (!(param(h, nid, P_PIN_CAPS) & PIN_OUT_CAPABLE)) continue;
        uint32_t cfg = 0;
        cmd(h, nid, V_GET_CONFIG, 0, &cfg);
        if (((cfg >> 30) & 3) == 1) continue;                  /* nothing connected to this port */
        uint32_t dev = (cfg >> 20) & 0xF;
        int rank = dev == 1 ? 0 : dev == 0 ? 1 : dev == 2 ? 2 : 3;    /* speaker, line out, HP, other */
        if (rank < best_rank) { best_rank = rank; best = nid; }
    }
    if (!best) { kprintf("hda: codec %u: no output pin\n", h->cad); return -1; }

    uint8_t path[10], sel[10] = { 0 };
    int len = find_dac(h, best, path, sel, 0);
    if (!len) { kprintf("hda: codec %u: no DAC behind pin %u\n", h->cad, best); return -1; }
    for (int i = 0; i < len; i++) {
        uint8_t nid = path[i];
        uint32_t caps = param(h, nid, P_AW_CAPS);
        if (caps & AW_POWER_CTL) cmd(h, nid, V_SET_POWER, 0, NULL);
        if (i < len - 1 && (caps & AW_CONN_LIST)) cmd(h, nid, V_SET_CONN_SEL, sel[i], NULL);
        unmute(h, nid, caps);
    }
    uint8_t pin = path[0], dac = path[len - 1];
    uint32_t pincaps = param(h, pin, P_PIN_CAPS);
    cmd(h, pin, V_SET_PIN_CTL, 0x40 | ((pincaps & PIN_HP_DRIVE) ? 0x80 : 0), NULL);   /* output enable (+ headphone amp) */
    cmd(h, pin, V_SET_EAPD, 0x2, NULL);                                             /* external amp on, if there is one */
    cmd(h, dac, V_SET_STREAM, STREAM_TAG << 4, NULL);                               /* stream 1, channel 0 */
    cmd(h, dac, V_SET_FORMAT, FMT_48K_16_STEREO, NULL);
    kprintf("hda: codec %u (%04x:%04x) pin %u -> DAC %u (%d hops)\n", h->cad, h->vendor >> 16, h->vendor & 0xFFFF, pin, dac, len - 1);
    return 0;
}

/* ---- the stream ----------------------------------------------------------------- */

/* Bounded MMIO polling, no timer: this also runs from paths that must not sleep. */
static int stream_reset(struct hda *h)
{
    uint32_t sd = (uint32_t)(h->sd - h->regs);
    wr8(h, sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (rd8(h, sd + SD_CTL) & SD_CTL_RUN); i++) ;
    wr8(h, sd + SD_CTL, SD_CTL_SRST);
    for (int i = 0; i < 100000 && !(rd8(h, sd + SD_CTL) & SD_CTL_SRST); i++) ;
    wr8(h, sd + SD_CTL, 0);
    for (int i = 0; i < 100000 && (rd8(h, sd + SD_CTL) & SD_CTL_SRST); i++) ;
    return 0;
}

static int hda_start(struct pcm_hw *pcm)
{
    struct hda *h = pcm->priv;
    uint32_t sd = (uint32_t)(h->sd - h->regs);
    stream_reset(h);
    h->lpib_last = 0; h->lpib_total = 0;
    wr32(h, sd + SD_BDPL, (uint32_t)h->bdl_phys);
    wr32(h, sd + SD_BDPU, (uint32_t)(h->bdl_phys >> 32));
    wr32(h, sd + SD_CBL, FRAGS * FRAG_SIZE);
    wr16(h, sd + SD_LVI, FRAGS - 1);
    wr16(h, sd + SD_FMT, FMT_48K_16_STEREO);
    wr8(h, sd + SD_STS, 0x1C);                                 /* clear status */
    /* stream number in bits 20-23 of the 24-bit control register, then run */
    wr8(h, sd + SD_CTL + 2, STREAM_TAG << 4);
    wr8(h, sd + SD_CTL, SD_CTL_RUN | SD_CTL_IOCE);
    return 0;
}

static void hda_stop(struct pcm_hw *pcm)
{
    struct hda *h = pcm->priv;
    uint32_t sd = (uint32_t)(h->sd - h->regs);
    wr8(h, sd + SD_CTL, 0);
    for (int i = 0; i < 100000 && (rd8(h, sd + SD_CTL) & SD_CTL_RUN); i++) ;
}

static void hda_irq(struct interrupt_frame *f)
{
    (void)f;
    struct hda *h = card;
    if (!h) return;
    uint32_t sts = rd32(h, INTSTS);
    if (!(sts & (1u << 31))) return;                           /* not ours */
    uint32_t sd = (uint32_t)(h->sd - h->regs);
    uint8_t ssts = rd8(h, sd + SD_STS);
    if (ssts & 0x1c) wr8(h, sd + SD_STS, ssts & 0x1c);      /* every cause (BCIS, FIFOE, DESE), or the line stays up */
    if (ssts & SD_STS_BCIS) {
        /* Where the link really is, not "one fragment per interrupt": a
         * late handler (another CPU, a long tick) sees two fragments gone
         * in one interrupt, and counting would then lag the hardware for
         * good, with the writer landing behind the head. */
        uint32_t ring = h->pcm.frags * h->pcm.frag_size;
        uint32_t lpib = rd32(h, sd + SD_LPIB) % ring;
        uint32_t advanced = (lpib + ring - h->lpib_last) % ring;
        h->lpib_last = lpib;
        h->lpib_total += advanced;
        uint64_t frags = h->lpib_total / h->pcm.frag_size;
        /* Only what the link position says: an interrupt that shows no
         * progress (a repeat) must not count, or the driver's idea of the
         * head drifts ahead of the hardware and the writer lands on what
         * is still playing. */
        if (frags > h->pcm.played_frags) {
            h->pcm.played_frags = frags;
            pcm_fragment_done(&h->pcm);
        }
    }
    if (sts & (1u << 30)) wr8(h, RIRBSTS, 0x5);               /* a stray controller interrupt */
}

/* ---- controller init ------------------------------------------------------------ */

static int hda_setup(const struct pci_dev *pd)
{
    int irq = pci_irq(pd);
    if (irq < 0) { kprintf("hda: no interrupt line for %02x:%02x.%u\n", pd->bus, pd->slot, pd->func); return -1; }
    struct hda *h = kzalloc(sizeof(*h));
    if (!h) return -1;
    pci_enable_busmaster(pd);
    h->regs = vmm_map_mmio(pd->bar[0], 0x4000);
    h->irq = (uint8_t)irq;

    /* reset: CRST low, then high, wait for it to read back; codecs announce themselves */
    wr32(h, GCTL, 0);
    for (int i = 0; i < 1000 && (rd32(h, GCTL) & GCTL_CRST); i++) delay_us(10);
    delay_us(200);
    wr32(h, GCTL, GCTL_CRST);
    for (int i = 0; i < 1000 && !(rd32(h, GCTL) & GCTL_CRST); i++) delay_us(10);
    delay_us(1000);
    uint16_t gcap = rd16(h, GCAP);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    uint16_t codecs = rd16(h, STATESTS);
    if (!oss || !codecs) { kprintf("hda: %02x:%02x.%u: %d output streams, codecs %x: unusable\n", pd->bus, pd->slot, pd->func, oss, codecs); return -1; }
    h->sd = h->regs + 0x80 + 0x20 * iss;                       /* the first output stream */

    /* CORB and RIRB: 256 entries each, one page each */
    h->corb_phys = pmm_alloc_page(); h->rirb_phys = pmm_alloc_page();
    h->corb = P2V(h->corb_phys); h->rirb = P2V(h->rirb_phys);
    memset(h->corb, 0, 4096); memset(h->rirb, 0, 4096);
    wr8(h, CORBCTL, 0); wr8(h, RIRBCTL, 0);
    for (int i = 0; i < 1000 && ((rd8(h, CORBCTL) | rd8(h, RIRBCTL)) & 2); i++) delay_us(10);
    wr32(h, CORBLBASE, (uint32_t)h->corb_phys); wr32(h, CORBUBASE, (uint32_t)(h->corb_phys >> 32));
    wr32(h, RIRBLBASE, (uint32_t)h->rirb_phys); wr32(h, RIRBUBASE, (uint32_t)(h->rirb_phys >> 32));
    wr8(h, CORBSIZE, 2); wr8(h, RIRBSIZE, 2);                  /* 256 entries */
    wr16(h, CORBRP, 0x8000);                                   /* reset the read pointer */
    for (int i = 0; i < 1000 && !(rd16(h, CORBRP) & 0x8000); i++) delay_us(10);
    wr16(h, CORBRP, 0);
    for (int i = 0; i < 1000 && (rd16(h, CORBRP) & 0x8000); i++) delay_us(10);
    wr16(h, CORBWP, 0);
    wr16(h, RIRBWP, 0x8000);
    wr16(h, RINTCNT, 1);
    h->corb_wp = 0; h->rirb_rp = 0;
    /* run both; the response interrupt is enabled because a controller (QEMU's
     * at least) holds the CORB after RINTCNT responses until RIRBSTS is
     * acknowledged, and only counts when the interrupt is on. We poll and ack. */
    wr8(h, CORBCTL, 2); wr8(h, RIRBCTL, 3);
    wr32(h, DPLBASE, 0); wr32(h, DPUBASE, 0);

    /* the first codec that answers */
    int ok = -1;
    for (uint8_t cad = 0; cad < 15 && ok; cad++) {
        if (!(codecs & (1u << cad))) continue;
        h->cad = cad;
        if (cmd(h, 0, V_GET_PARAM, P_VENDOR, &h->vendor) == 0)
            ok = codec_setup(h);
    }
    if (ok) return -1;

    /* the buffer descriptor list and the ring */
    h->bdl_phys = pmm_alloc_page();
    h->bdl = P2V(h->bdl_phys);
    memset(h->bdl, 0, 4096);
    h->ring_phys = pmm_alloc_pages(FRAGS * FRAG_SIZE / 4096);
    if (!h->ring_phys) return -1;
    for (int i = 0; i < FRAGS; i++) {
        h->bdl[i].addr = htole64(h->ring_phys + (uint64_t)i * FRAG_SIZE);
        h->bdl[i].len = htole32(FRAG_SIZE);
        h->bdl[i].ioc = htole32(1);
    }
    h->pcm.name = "hda";
    h->pcm.ring = P2V(h->ring_phys);
    h->pcm.frags = FRAGS; h->pcm.frag_size = FRAG_SIZE;
    h->pcm.start = hda_start; h->pcm.stop = hda_stop;
    h->pcm.priv = h;
    card = h;
    irq_install(h->irq, hda_irq);
    irq_unmask_pci(h->irq);
    wr32(h, INTCTL, (1u << 31) | (1u << iss));                  /* global + our stream */
    kprintf("hda: controller at %02x:%02x.%u, irq %u, %d in/%d out streams\n", pd->bus, pd->slot, pd->func, h->irq, iss, oss);
    pcm_register(&h->pcm);
    return 0;
}

void hda_init(void)
{
    for (size_t i = 0; i < pci_count() && !card; i++) {
        const struct pci_dev *pd = pci_get(i);
        if (pd->class == 0x04 && pd->subclass == 0x03)        /* multimedia: HD audio */
            hda_setup(pd);
    }
}
