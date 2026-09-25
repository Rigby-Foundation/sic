/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/dsp: the OSS digital audio interface, the part of it programs use
 * (SDL's "dsp" backend, plain write() of PCM). The request numbers are
 * Linux's so <sys/soundcard.h> code compiles unchanged. The hardware plays
 * one format; SETFMT/CHANNELS/SPEED report what it is, the caller
 * converts (SDL does). */
#pragma once
#ifdef SIC_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

#define SNDCTL_DSP_RESET        0x00005000
#define SNDCTL_DSP_SYNC         0x00005001
#define SNDCTL_DSP_SPEED        0xC0045002
#define SNDCTL_DSP_STEREO       0xC0045003
#define SNDCTL_DSP_GETBLKSIZE   0xC0045004
#define SNDCTL_DSP_SETFMT       0xC0045005
#define SNDCTL_DSP_CHANNELS     0xC0045006
#define SNDCTL_DSP_POST         0x00005008
#define SNDCTL_DSP_SUBDIVIDE    0xC0045009
#define SNDCTL_DSP_SETFRAGMENT  0xC004500A
#define SNDCTL_DSP_GETFMTS      0x8004500B
#define SNDCTL_DSP_GETOSPACE    0x8010500C
#define SNDCTL_DSP_GETISPACE    0x8010500D
#define SNDCTL_DSP_NONBLOCK     0x0000500E
#define SNDCTL_DSP_GETCAPS      0x8004500F
#define SNDCTL_DSP_GETTRIGGER   0x80045010
#define SNDCTL_DSP_SETTRIGGER   0x40045010
#define SNDCTL_DSP_GETOPTR      0x800C5012
#define SNDCTL_DSP_GETODELAY    0x80045017
#define SOUND_PCM_WRITE_RATE    SNDCTL_DSP_SPEED
#define SOUND_PCM_WRITE_CHANNELS SNDCTL_DSP_CHANNELS
#define SOUND_PCM_WRITE_BITS    SNDCTL_DSP_SETFMT
#define SOUND_PCM_READ_RATE     0x80045002
#define SOUND_PCM_READ_CHANNELS 0x80045006
#define SOUND_PCM_READ_BITS     0x80045005

#define AFMT_QUERY      0x00000000
#define AFMT_MU_LAW     0x00000001
#define AFMT_A_LAW      0x00000002
#define AFMT_IMA_ADPCM  0x00000004
#define AFMT_U8         0x00000008
#define AFMT_S16_LE     0x00000010
#define AFMT_S16_BE     0x00000020
#define AFMT_S8         0x00000040
#define AFMT_U16_LE     0x00000080
#define AFMT_U16_BE     0x00000100
#define AFMT_MPEG       0x00000200
#define AFMT_AC3        0x00000400
#define AFMT_S16_NE     AFMT_S16_LE

#define DSP_CAP_REVISION 0x000000ff
#define DSP_CAP_DUPLEX   0x00000100
#define DSP_CAP_REALTIME 0x00000200
#define DSP_CAP_BATCH    0x00000400
#define DSP_CAP_COPROC   0x00000800
#define DSP_CAP_TRIGGER  0x00001000
#define DSP_CAP_MMAP     0x00002000
#define PCM_ENABLE_INPUT  1
#define PCM_ENABLE_OUTPUT 2

typedef struct audio_buf_info {
    int32_t fragments;          /* fragments that can be written without blocking */
    int32_t fragstotal;
    int32_t fragsize;           /* bytes */
    int32_t bytes;              /* writable now */
} audio_buf_info;

typedef struct count_info {
    int32_t bytes;              /* played since open */
    int32_t blocks;             /* fragments since the last call */
    int32_t ptr;                /* position in the buffer */
} count_info;
