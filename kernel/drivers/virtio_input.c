/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* virtio-input (QEMU -device virtio-keyboard-pci / virtio-mouse-pci):
 * Linux evdev events off the event queue, turned into what the console
 * and /dev/mouse already speak: scancode-set-1 bytes and mouse records.
 * One driver instance per device, up to four. */
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "asm/irq.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "endian.h"
#include "string.h"
#include "printf.h"

#define VIRTIO_ID_INPUT 18
#define NBUF 64

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define REL_X 0
#define REL_Y 1
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct virtio_input_event { uint16_t type, code; uint32_t value; } __attribute__((packed));

struct vinput {
    struct virtio_dev dev;
    struct virtq q;
    struct virtio_input_event *ev;      /* NBUF of them, one page */
    int dx, dy; unsigned buttons, sent_buttons;   /* the mouse state between EV_SYNs */
    char name[32];
};
static struct vinput inputs[4];
static int ninputs;

/* Linux key codes 1..88 are scancode set 1; the rest of the keys the
 * console cares about are the E0-prefixed ones. */
static int key_to_scancode(uint16_t code, uint8_t *e0)
{
    *e0 = 0;
    if (code >= 1 && code <= 88) return code;
    *e0 = 1;
    switch (code) {
    case 96:  return 0x1C;      /* KP enter */
    case 97:  return 0x1D;      /* right ctrl */
    case 98:  return 0x35;      /* KP slash */
    case 100: return 0x38;      /* right alt */
    case 102: return 0x47;      /* home */
    case 103: return 0x48;      /* up */
    case 104: return 0x49;      /* page up */
    case 105: return 0x4B;      /* left */
    case 106: return 0x4D;      /* right */
    case 107: return 0x4F;      /* end */
    case 108: return 0x50;      /* down */
    case 109: return 0x51;      /* page down */
    case 110: return 0x52;      /* insert */
    case 111: return 0x53;      /* delete */
    case 125: return 0x5B;      /* left meta */
    case 126: return 0x5C;      /* right meta */
    }
    return -1;
}

static void handle(struct vinput *in, const struct virtio_input_event *e)
{
    uint16_t type = le16toh(e->type), code = le16toh(e->code);
    int32_t value = (int32_t)le32toh(e->value);
    switch (type) {
    case EV_KEY:
        if (code >= BTN_LEFT && code <= BTN_MIDDLE) {
            unsigned bit = code == BTN_LEFT ? 1 : code == BTN_RIGHT ? 2 : 4;
            if (value) in->buttons |= bit; else in->buttons &= ~bit;
        } else {
            uint8_t e0;
            int sc = key_to_scancode(code, &e0);
            if (sc < 0 || value == 2) return;               /* unknown, or autorepeat */
            if (e0) keyboard_scancode(0xE0);
            keyboard_scancode((uint8_t)(value ? sc : sc | 0x80));
        }
        break;
    case EV_REL:
        if (code == REL_X) in->dx += value;
        else if (code == REL_Y) in->dy += value;
        break;
    case EV_SYN:
        if (in->dx || in->dy || in->buttons != in->sent_buttons) {
            mouse_push(in->dx, in->dy, in->buttons);
            in->dx = in->dy = 0;
            in->sent_buttons = in->buttons;
        }
        break;
    default:
        break;                                              /* EV_ABS (a tablet): not yet */
    }
}

/* Hand buffer i back to the device. */
static void post(struct vinput *in, uint16_t i)
{
    struct virtq *q = &in->q;
    q->desc[i].addr = htole64(V2P(&in->ev[i]));
    q->desc[i].len = htole32(sizeof in->ev[i]);
    q->desc[i].flags = htole16(VIRTQ_DESC_F_WRITE);
    q->desc[i].next = 0;
    uint16_t idx = le16toh(q->avail->idx);
    q->avail->ring[idx % q->size] = htole16(i);
    dma_wmb();
    q->avail->idx = htole16((uint16_t)(idx + 1));
}

static void drain(struct vinput *in)
{
    struct virtq *q = &in->q;
    int any = 0;
    for (;;) {
        dma_rmb();
        if (le16toh(q->used->idx) == q->last_used) break;
        struct virtq_used_elem *e = &q->used->ring[q->last_used % q->size];
        q->last_used++;
        uint16_t i = (uint16_t)le32toh(e->id);
        if (i < NBUF) { handle(in, &in->ev[i]); post(in, i); }
        any = 1;
    }
    if (any) { dma_wmb(); mmio_write16(q->notify, q->index); }
}

static void input_irq(struct interrupt_frame *f)
{
    (void)f;
    for (int k = 0; k < ninputs; k++) {
        struct vinput *in = &inputs[k];
        (void)mmio_read8(in->dev.isr);              /* acknowledges the line */
        drain(in);
    }
}

static void probe(const struct pci_dev *pd)
{
    if (ninputs >= 4) return;
    struct vinput *in = &inputs[ninputs];
    memset(in, 0, sizeof *in);
    if (virtio_init(&in->dev, pd, 0) != 0) return;
    if (virtio_queue_setup(&in->dev, &in->q, 0) != 0) return;
    uint64_t page = pmm_alloc_page();
    if (!page) return;
    in->ev = P2V(page);
    memset(in->ev, 0, PAGE_SIZE);
    /* the device's name: config select 1 (ID_NAME) */
    mmio_write8(in->dev.device_cfg + 0, 1);
    mmio_write8(in->dev.device_cfg + 1, 0);
    uint8_t n = mmio_read8(in->dev.device_cfg + 2);
    for (uint8_t i = 0; i < n && i < sizeof in->name - 1; i++) in->name[i] = (char)mmio_read8(in->dev.device_cfg + 8 + i);
    mmio_write8(in->dev.device_cfg + 0, 0);
    for (uint16_t i = 0; i < NBUF && i < in->q.size; i++) post(in, i);   /* the queue is ours alone: descriptors by index */
    virtio_driver_ok(&in->dev);
    dma_wmb();
    mmio_write16(in->q.notify, in->q.index);
    int irq = pci_irq(pd);
    if (irq >= 0) { irq_install((uint8_t)irq, input_irq); irq_unmask_pci((uint8_t)irq); }
    kprintf("virtio-input: %s at %02x:%02x.%u, irq %d\n", in->name[0] ? in->name : "input", pd->bus, pd->slot, pd->func, irq);
    ninputs++;
}

void virtio_input_init(void)
{
    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_dev *pd = pci_get(i);
        if (pd->vendor == 0x1AF4 && (pd->device == 0x1040 + VIRTIO_ID_INPUT))
            probe(pd);
    }
}
