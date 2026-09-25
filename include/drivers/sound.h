/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Sound: a playback device (a DMA ring the hardware reads in fragments)
 * and /dev/dsp on top of it (dsp.c). One format: 48 kHz, 16-bit signed
 * little-endian, stereo. */
#pragma once
#include "types.h"
#include "proc/wait.h"

#define PCM_RATE      48000
#define PCM_CHANNELS  2
#define PCM_FRAME     4                     /* bytes per frame */

struct pcm_hw {
    const char *name;
    uint8_t *ring;                          /* the DMA buffer, frags * frag_size bytes */
    uint32_t frags, frag_size;
    int  (*start)(struct pcm_hw *hw);       /* begin playing the ring from fragment 0 */
    void (*stop)(struct pcm_hw *hw);
    /* filled by the driver: fragments played since start (dsp.c reads it) */
    volatile uint64_t played_frags;
    struct waitqueue wq;                    /* woken per fragment */
    void *priv;
};

void pcm_register(struct pcm_hw *hw);       /* dsp.c: makes /dev/dsp */
void pcm_fragment_done(struct pcm_hw *hw);  /* driver, from its interrupt: one more fragment played */

void hda_init(void);                        /* Intel HD Audio controllers (PCI) */
