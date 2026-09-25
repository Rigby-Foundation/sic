/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/dsp: OSS-style PCM playback over a pcm_hw ring. write() copies
 * frames into the ring ahead of the hardware's position and blocks when it
 * is full; fragments the writer did not refill are silenced so an underrun
 * plays quiet, not stale sound. One opener at a time. */
#include "drivers/sound.h"
#include "abi/soundcard.h"
#include "fs/vfs.h"
#include "proc/sched.h"
#include "proc/syscall.h"
#include "spinlock.h"
#include "string.h"
#include "printf.h"

static struct pcm_hw *hw;
static spinlock_t dsp_lock;
static int dsp_open;
static int running;
static uint64_t wr;                         /* bytes written since start */
static uint64_t rd;                         /* bytes the hardware has finished */

static uint64_t ring_bytes(void) { return (uint64_t)hw->frags * hw->frag_size; }

/* How far ahead a writer may queue: the whole ring is 340 ms of sound,
 * which is how late everything would play. Four fragments (85 ms) is
 * plenty to ride out a slow tick; SNDCTL_DSP_SETFRAGMENT's count adjusts it. */
static uint32_t queue_frags = 4;
static uint64_t queue_bytes(void) { uint64_t q = (uint64_t)queue_frags * hw->frag_size; return q < ring_bytes() ? q : ring_bytes(); }

/* The driver's interrupt: fragment `played_frags - 1` is done. */
void pcm_fragment_done(struct pcm_hw *h)
{
    uint64_t f = spin_lock_irqsave(&dsp_lock);
    rd = h->played_frags * h->frag_size;
    /* the fragment just consumed will come round again: silence it unless
     * the writer has already put new data there */
    uint64_t consumed = (h->played_frags - 1) % h->frags;
    if (wr < rd + ring_bytes() - h->frag_size || wr <= rd)      /* not refilled */
        memset(h->ring + consumed * h->frag_size, 0, h->frag_size);
    if (wr < rd) {                                              /* underrun: new data goes ahead of the head */
        static int reported;
        if (reported < 3) { reported++; kprintf("dsp: underrun (%llu bytes late)\n", rd - wr); }
        wr = rd;
    }
    spin_unlock_irqrestore(&dsp_lock, f);
    waitqueue_wake_all(&h->wq);
}

static int space_locked(void) { return (int)(queue_bytes() - (wr - rd)); }

static int dsp_space(void)
{
    uint64_t f = spin_lock_irqsave(&dsp_lock);
    int s = space_locked();
    spin_unlock_irqrestore(&dsp_lock, f);
    return s;
}

/* Not under dsp_lock: the driver's start may wait on the hardware. Only
 * the one opener writes, so nothing races it. */
static void dsp_start(void)
{
    if (running) return;
    memset(hw->ring, 0, ring_bytes());
    wr = rd = 0;
    hw->played_frags = 0;
    if (hw->start(hw) == 0) running = 1;
}

static void dsp_stop(void)
{
    if (!running) return;
    hw->stop(hw);
    running = 0;
}

static long dsp_write(struct file *f, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    len &= ~(size_t)(PCM_FRAME - 1);
    dsp_start();
    while (done < len) {
        uint64_t fl = spin_lock_irqsave(&dsp_lock);
        int space = space_locked();
        if (space > 0) {
            size_t n = (size_t)space < len - done ? (size_t)space : len - done;
            uint64_t off = wr % ring_bytes();
            size_t first = ring_bytes() - off < n ? (size_t)(ring_bytes() - off) : n;
            memcpy(hw->ring + off, p + done, first);
            if (n > first) memcpy(hw->ring, p + done + first, n - first);
            wr += n;
            done += n;
        }
        spin_unlock_irqrestore(&dsp_lock, fl);
        if (done == len) break;
        if (f->flags & O_NONBLOCK) return done ? (long)done : -EAGAIN;
        /* A queue of a few fragments drains in well under a second; if it
         * does not, the hardware stopped telling us (a lost interrupt, a
         * stalled link): restart the stream rather than hang the writer. */
        int rc = wait_event_timeout(&hw->wq, dsp_space() >= (int)hw->frag_size, 1000);
        if (rc < 0) return done ? (long)done : -EINTR;
        if (rc == 0 && dsp_space() < (int)hw->frag_size) {
            kprintf("dsp: no progress for a second, restarting the stream\n");
            dsp_stop();
            dsp_start();
        }
    }
    return (long)done;
}

static int dsp_poll(struct file *f, struct waitqueue **wq)
{
    (void)f;
    *wq = &hw->wq;
    return dsp_space() >= (int)hw->frag_size ? POLLOUT : 0;
}

static long dsp_ioctl(struct file *f, long req, uint64_t arg)
{
    (void)f;
    int *ip = (int *)arg;
    switch ((uint32_t)req) {
    case SNDCTL_DSP_RESET:
        dsp_stop();
        return 0;
    case SNDCTL_DSP_SYNC:
    case SNDCTL_DSP_POST:
        if (running) {
            /* let what is queued play out, then stop so the ring does not loop */
            wait_event_interruptible(&hw->wq, rd >= wr || !running);
            dsp_stop();
        }
        return 0;
    case SNDCTL_DSP_SPEED:
    case SOUND_PCM_READ_RATE:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = PCM_RATE;
        return 0;
    case SNDCTL_DSP_STEREO:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = 1;
        return 0;
    case SNDCTL_DSP_CHANNELS:
    case SOUND_PCM_READ_CHANNELS:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = PCM_CHANNELS;
        return 0;
    case SNDCTL_DSP_SETFMT:
    case SOUND_PCM_READ_BITS:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = AFMT_S16_LE;
        return 0;
    case SNDCTL_DSP_GETFMTS:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = AFMT_S16_LE;
        return 0;
    case SNDCTL_DSP_GETBLKSIZE:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = (int)hw->frag_size;
        return 0;
    case SNDCTL_DSP_SETFRAGMENT: {              /* the fragments are what they are; the count says how far ahead to queue */
        if (!user_ok(arg, 4)) return -EFAULT;
        uint32_t count = (uint32_t)*ip >> 16;
        if (count < 3) count = 3;                 /* under 64 ms and a slow tick would underrun */
        if (count <= hw->frags) queue_frags = count;
        return 0;
    }
    case SNDCTL_DSP_SUBDIVIDE:
    case SNDCTL_DSP_NONBLOCK:
    case SNDCTL_DSP_SETTRIGGER:
        return 0;
    case SNDCTL_DSP_GETTRIGGER:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = PCM_ENABLE_OUTPUT;
        return 0;
    case SNDCTL_DSP_GETCAPS:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = DSP_CAP_REALTIME | DSP_CAP_BATCH;
        return 0;
    case SNDCTL_DSP_GETOSPACE: {
        if (!user_ok(arg, sizeof(audio_buf_info))) return -EFAULT;
        audio_buf_info *bi = (void *)arg;
        int s = dsp_space();
        bi->fragments = s / (int)hw->frag_size;
        bi->fragstotal = (int)queue_frags;
        bi->fragsize = (int)hw->frag_size;
        bi->bytes = s;
        return 0;
    }
    case SNDCTL_DSP_GETODELAY:
        if (!user_ok(arg, 4)) return -EFAULT;
        *ip = (int)(wr - rd);
        return 0;
    case SNDCTL_DSP_GETOPTR: {
        if (!user_ok(arg, sizeof(count_info))) return -EFAULT;
        count_info *ci = (void *)arg;
        ci->bytes = (int)rd;
        ci->blocks = 0;
        ci->ptr = (int)(rd % ring_bytes());
        return 0;
    }
    case SNDCTL_DSP_GETISPACE:
        return -EINVAL;                         /* no capture */
    }
    return -ENOTTY;
}

static void dsp_release(struct file *f)
{
    (void)f;
    if (running) {
        wait_event_interruptible(&hw->wq, rd >= wr || !running);   /* drain */
        dsp_stop();
    }
    dsp_open = 0;
}

static long dsp_do_open(struct file *f)
{
    (void)f;
    uint64_t fl = spin_lock_irqsave(&dsp_lock);
    int busy = dsp_open;
    if (!busy) dsp_open = 1;
    spin_unlock_irqrestore(&dsp_lock, fl);
    return busy ? -EBUSY : 0;
}

static const struct dev_ops dsp_ops = { .write = dsp_write, .poll = dsp_poll, .ioctl = dsp_ioctl,
                                        .release = dsp_release, .open = dsp_do_open };

void pcm_register(struct pcm_hw *h)
{
    if (hw) return;                             /* one card is plenty */
    hw = h;
    h->wq = (struct waitqueue)WAITQUEUE_INIT;
    vfs_mkdev("/dev/dsp", &dsp_ops, NULL);
    kprintf("dsp: /dev/dsp on %s, %u Hz s16le stereo, %u x %u byte fragments\n", h->name, PCM_RATE, h->frags, h->frag_size);
}
