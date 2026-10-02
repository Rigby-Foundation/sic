/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Intel High Definition Audio: the controller (CORB/RIRB command rings, one
 * output stream with a cyclic buffer of fragments, an interrupt per
 * fragment) and enough of the codec to make sound come out: every output
 * pin wired to a speaker, headphones or a line out gets a path back to a
 * DAC, powered up and unmuted, and every such DAC plays our stream.
 * External amplifiers are switched on (EAPD) on every pin that has one,
 * wired or not: laptops put the speaker amp's switch on odd pins (a Dell
 * STAC9200 has it on pin 8, marked unconnected). Plugging headphones in
 * silences the speakers. The stream is 48 kHz 16-bit stereo; dsp.c feeds
 * the ring. /proc/hda: the codec's widgets, and `echo "nid verb payload"`
 * (hex) sends a verb, for finding what a machine needs. */
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
#include "spinlock.h"
#include "fs/vfs.h"
#include "abi/abi.h"

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
#define V_GET_PIN_CTL   0xF07
#define V_GET_PIN_SENSE 0xF09
#define V_EXEC_SENSE    0x709
#define V_GET_EAPD      0xF0C
#define V_GET_SUBSYS    0xF20
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
#define P_GPIO_COUNT    0x11

#define AW_TYPE(caps)   (((caps) >> 20) & 0xF)
#define AW_OUT          0
#define AW_IN           1
#define AW_MIXER        2
#define AW_SELECTOR     3
#define AW_PIN          4
#define AW_HAS_IN_AMP   (1u << 1)
#define AW_HAS_OUT_AMP  (1u << 2)
#define AW_AMP_OVERRIDE (1u << 3)          /* the widget has its own amp caps; else the function group's apply */
#define AW_CONN_LIST    (1u << 8)
#define AW_POWER_CTL    (1u << 10)
#define PIN_OUT_CAPABLE (1u << 4)
#define PIN_HP_DRIVE    (1u << 6)
#define PIN_SENSE_TRIG  (1u << 1)
#define PIN_PRESENCE    (1u << 2)
#define PIN_EAPD        (1u << 16)
#define MAX_OUTS        6

enum { OUT_LINE, OUT_SPEAKER, OUT_HP };

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
    uint32_t vendor, subsys;
    uint8_t afg;
    spinlock_t cmd_lock;
    uint8_t out_pin[MAX_OUTS], out_kind[MAX_OUTS]; int nouts;   /* the pins we drive */
    int hp_in;                              /* headphones plugged (speakers off); -1 = not known yet */
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

/* Send one verb, wait for its response (polling). */
static int cmd_locked(struct hda *h, uint8_t nid, uint32_t verb, uint32_t payload, uint32_t *resp)
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

/* Setup, /proc/hda and the jack thread all talk to the codec. */
static int cmd(struct hda *h, uint8_t nid, uint32_t verb, uint32_t payload, uint32_t *resp)
{
    spin_lock(&h->cmd_lock);
    int r = cmd_locked(h, nid, verb, payload, resp);
    spin_unlock(&h->cmd_lock);
    return r;
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

/* Set the widget's output (and input, if any) amplifier to unmuted, 0 dB
 * (the step the amp's caps call 0 dB; above it is gain, and distorts). */
static void unmute(struct hda *h, uint8_t nid, uint32_t caps)
{
    uint8_t from = (caps & AW_AMP_OVERRIDE) ? nid : h->afg;    /* a STAC9200's master volume has none of its own */
    if (caps & AW_HAS_OUT_AMP) {
        uint32_t ac = param(h, from, P_OUT_AMP_CAPS);
        cmd(h, nid, V_SET_AMP, 0xB000 | (ac & 0x7F), NULL);  /* output, left+right, 0 dB, unmuted */
    }
    if (caps & AW_HAS_IN_AMP) {
        uint32_t ac = param(h, from, P_IN_AMP_CAPS);
        for (uint32_t idx = 0; idx < 16; idx++)               /* every input index; harmless past the end */
            cmd(h, nid, V_SET_AMP, 0x7000 | idx << 8 | (ac & 0x7F), NULL);
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

/* Headphones in: speakers off (line outs stay). Pins that cannot sense
 * leave things as they are. */
static void jack_check(struct hda *h)
{
    int hp = 0, can = 0;
    for (int i = 0; i < h->nouts; i++) {
        if (h->out_kind[i] != OUT_HP) continue;
        uint8_t pin = h->out_pin[i];
        uint32_t pc = param(h, pin, P_PIN_CAPS);
        if (!(pc & PIN_PRESENCE)) continue;
        can = 1;
        if (pc & PIN_SENSE_TRIG) cmd(h, pin, V_EXEC_SENSE, 0, NULL);
        uint32_t sense = 0;
        cmd(h, pin, V_GET_PIN_SENSE, 0, &sense);
        if (sense & (1u << 31)) hp = 1;
    }
    if (!can || hp == h->hp_in) return;
    h->hp_in = hp;
    for (int i = 0; i < h->nouts; i++)
        if (h->out_kind[i] == OUT_SPEAKER)
            cmd(h, h->out_pin[i], V_SET_PIN_CTL, hp ? 0 : 0x40, NULL);
}

static void jack_thread(void *arg)
{
    struct hda *h = arg;
    for (;;) {
        jack_check(h);
        task_sleep_ms(500);
    }
}

static const char *const kind_name[] = { "line out", "speaker", "headphones" };

/* Wire every output: speakers, line outs and headphones, each to a DAC
 * (shared or not), every DAC playing our stream. */
static int codec_setup(struct hda *h)
{
    uint32_t sub = param(h, 0, P_SUB_NODES);
    uint8_t fg_start = (sub >> 16) & 0xFF, fg_count = sub & 0xFF, afg = 0;
    for (uint8_t i = 0; i < fg_count; i++)
        if ((param(h, fg_start + i, P_FG_TYPE) & 0xFF) == 1) { afg = fg_start + i; break; }
    if (!afg) { kprintf("hda: codec %u has no audio function group\n", h->cad); return -1; }
    h->afg = afg;
    cmd(h, afg, V_SET_POWER, 0, NULL);                        /* D0 */
    cmd(h, afg, V_GET_SUBSYS, 0, &h->subsys);
    sub = param(h, afg, P_SUB_NODES);
    uint8_t w_start = (sub >> 16) & 0xFF, w_count = sub & 0xFF;

    h->nouts = 0;
    h->hp_in = -1;
    for (uint8_t i = 0; i < w_count; i++) {
        uint8_t nid = w_start + i;
        uint32_t caps = param(h, nid, P_AW_CAPS);
        if (AW_TYPE(caps) != AW_PIN) continue;
        uint32_t pc = param(h, nid, P_PIN_CAPS);
        if (pc & PIN_EAPD) cmd(h, nid, V_SET_EAPD, 0x2, NULL);   /* external amp on, wired or not */
        if (!(pc & PIN_OUT_CAPABLE)) continue;
        uint32_t cfg = 0;
        cmd(h, nid, V_GET_CONFIG, 0, &cfg);
        if (((cfg >> 30) & 3) == 1) continue;                  /* nothing connected to this port */
        uint32_t dev = (cfg >> 20) & 0xF;
        int kind = dev == 1 ? OUT_SPEAKER : dev == 0 ? OUT_LINE : dev == 2 ? OUT_HP : -1;
        if (kind < 0 || h->nouts == MAX_OUTS) continue;
        h->out_pin[h->nouts] = nid; h->out_kind[h->nouts] = (uint8_t)kind; h->nouts++;
    }
    if (!h->nouts) { kprintf("hda: codec %u: no output pin\n", h->cad); return -1; }

    int wired = 0;
    for (int o = 0; o < h->nouts; o++) {
        uint8_t path[10], sel[10] = { 0 };
        int len = find_dac(h, h->out_pin[o], path, sel, 0);
        if (!len) { kprintf("hda: codec %u: no DAC behind pin %u\n", h->cad, h->out_pin[o]); continue; }
        for (int i = 0; i < len; i++) {
            uint8_t nid = path[i];
            uint32_t caps = param(h, nid, P_AW_CAPS);
            if (caps & AW_POWER_CTL) cmd(h, nid, V_SET_POWER, 0, NULL);
            if (i < len - 1 && (caps & AW_CONN_LIST)) cmd(h, nid, V_SET_CONN_SEL, sel[i], NULL);
            unmute(h, nid, caps);
        }
        uint8_t pin = path[0], dac = path[len - 1];
        uint32_t pincaps = param(h, pin, P_PIN_CAPS);
        cmd(h, pin, V_SET_PIN_CTL, 0x40 | (h->out_kind[o] == OUT_HP && (pincaps & PIN_HP_DRIVE) ? 0x80 : 0), NULL);
        cmd(h, dac, V_SET_STREAM, STREAM_TAG << 4, NULL);                           /* stream 1, channel 0 */
        cmd(h, dac, V_SET_FORMAT, FMT_48K_16_STEREO, NULL);
        kprintf("hda: codec %u (%04x:%04x, subsystem %08x) %s: pin %u -> DAC %u (%d hops)\n", h->cad,
                h->vendor >> 16, h->vendor & 0xFFFF, h->subsys, kind_name[h->out_kind[o]], pin, dac, len - 1);
        wired++;
    }
    return wired ? 0 : -1;
}

/* ---- /proc/hda ------------------------------------------------------------------- */

static char proc_buf[8192];
static size_t proc_len;

static void pp(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pp(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    if (proc_len + 1 < sizeof proc_buf)
        proc_len += (size_t)kvsnprintf(proc_buf + proc_len, sizeof proc_buf - proc_len, fmt, ap);
    if (proc_len >= sizeof proc_buf) proc_len = sizeof proc_buf - 1;
    __builtin_va_end(ap);
}

static const char *const aw_name[16] = { "dac", "adc", "mixer", "selector", "pin", "power", "volume knob", "beep",
                                         "?", "?", "?", "?", "?", "?", "?", "vendor" };

/* Every widget: type, caps, connections, amps; pins with their config,
 * control, sense and EAPD. */
static void proc_fill(struct hda *h)
{
    proc_len = 0;
    pp("codec %u: %08x subsystem %08x, afg %u, gpios %08x\n", h->cad, h->vendor, h->subsys, h->afg, param(h, h->afg, P_GPIO_COUNT));
    uint32_t sub = param(h, h->afg, P_SUB_NODES);
    uint8_t w_start = (sub >> 16) & 0xFF, w_count = sub & 0xFF;
    for (uint8_t i = 0; i < w_count; i++) {
        uint8_t nid = w_start + i;
        uint32_t caps = param(h, nid, P_AW_CAPS);
        pp("%3u %-8s caps %08x", nid, aw_name[AW_TYPE(caps)], caps);
        if (caps & AW_HAS_OUT_AMP) pp(" outamp %08x", param(h, nid, P_OUT_AMP_CAPS));
        if (caps & AW_HAS_IN_AMP) pp(" inamp %08x", param(h, nid, P_IN_AMP_CAPS));
        if (caps & AW_CONN_LIST) {
            uint8_t conns[32]; uint32_t cur = 0;
            int n = conn_list(h, nid, conns, 32);
            cmd(h, nid, V_GET_CONN_SEL, 0, &cur);
            pp(" conn");
            for (int k = 0; k < n; k++) pp("%s%u", (uint32_t)k == cur ? " *" : " ", conns[k]);
        }
        if (AW_TYPE(caps) == AW_PIN) {
            uint32_t pc = param(h, nid, P_PIN_CAPS), cfg = 0, ctl = 0, sense = 0, eapd = 0;
            cmd(h, nid, V_GET_CONFIG, 0, &cfg);
            cmd(h, nid, V_GET_PIN_CTL, 0, &ctl);
            if (pc & PIN_PRESENCE) cmd(h, nid, V_GET_PIN_SENSE, 0, &sense);
            if (pc & PIN_EAPD) cmd(h, nid, V_GET_EAPD, 0, &eapd);
            pp("\n      pincaps %08x config %08x ctl %02x sense %08x eapd %x", pc, cfg, ctl, sense, eapd);
        }
        pp("\n");
    }
    for (int o = 0; o < h->nouts; o++) pp("driving %s on pin %u\n", kind_name[h->out_kind[o]], h->out_pin[o]);
    if (h->hp_in >= 0) pp("headphones %s\n", h->hp_in ? "in: speakers off" : "out");
}

static long proc_read(struct file *f, void *buf, size_t len)
{
    if (f->pos == 0) proc_fill(card);
    if (f->pos >= proc_len) return 0;
    size_t n = proc_len - (size_t)f->pos < len ? proc_len - (size_t)f->pos : len;
    memcpy(buf, proc_buf + f->pos, n);
    f->pos += n;
    return (long)n;
}

static uint32_t hexnum(const char **p, const char *end, int *got)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n')) (*p)++;
    uint32_t v = 0;
    const char *start = *p;
    for (; *p < end; (*p)++) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v << 4 | (uint32_t)d;
    }
    if (*p > start) (*got)++;
    return v;
}

/* "nid verb payload" in hex: a 12-bit verb with an 8-bit payload, or a
 * 4-bit one (2 format, 3 amp, ...) with 16 bits. The answer goes to the log. */
static long proc_write(struct file *f, const void *buf, size_t len)
{
    (void)f;
    const char *p = buf, *end = p + len;
    int got = 0;
    uint32_t nid = hexnum(&p, end, &got), verb = hexnum(&p, end, &got), payload = hexnum(&p, end, &got);
    uint32_t r = 0;
    if (got == 0) return (long)len;                     /* a lone newline (echo writes it apart) */
    if (got < 2) return -EINVAL;
    if (nid > 0x7F || verb > 0xFFF) return -EINVAL;
    if (cmd(card, (uint8_t)nid, verb, payload, &r) != 0) return -EIO;
    kprintf("hda: nid %x verb %x payload %x -> %08x\n", nid, verb, payload, r);
    return (long)len;
}

static const struct dev_ops proc_ops = { .read = proc_read, .write = proc_write };

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
    /* ATI SB450/SB600 and AMD Hudson: their DMA does not snoop the CPU's
     * caches until told to (config 0x42 bit 1), and plays what is in RAM,
     * which is the silence the ring was cleared to. As Linux does. */
    if ((pd->vendor == 0x1002 && (pd->device == 0x437b || pd->device == 0x4383)) ||
        (pd->vendor == 0x1022 && pd->device == 0x780d)) {
        uint32_t v = pci_read32(pd->bus, pd->slot, pd->func, 0x40);
        pci_write32(pd->bus, pd->slot, pd->func, 0x40, (v & ~(0x07u << 16)) | (0x02u << 16));
    }
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
    vfs_mkdev("/proc/hda", &proc_ops, NULL);
    task_create("hdajack", jack_thread, h);
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
