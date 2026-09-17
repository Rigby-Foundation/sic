/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* System call layer: the sic ABI (abi/syscall.tbl) with POSIX semantics as
 * musl expects them. Handlers return a value or -errno. */
#include "proc/syscall.h"
#include "asm/cpu.h"
#include "proc/sched.h"
#include "asm/timer.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "fs/vfs.h"
#include "proc/elf.h"
#include "mm/heap.h"
#include "module.h"
#include "fs/pipe.h"
#include "fs/blkdev.h"
#include "proc/signal.h"
#include "proc/futex.h"
#include "proc/wait.h"
#include "asm/arch.h"
#ifdef CONFIG_NET
#include "net/net.h"
#include "net/socket.h"
#endif
#include "asm/smp.h"
#ifdef CONFIG_KEYBOARD
#include "drivers/keyboard.h"
#endif
#include "string.h"
#include "printf.h"

/* ---- user memory access ------------------------------------------------------ */

/* The caller's address space is live, so a range is usable once we've
 * checked it lies in user space and every page of it is mapped. */
int user_ok(uint64_t ptr, uint64_t len)
{
    if (len == 0)
        return ptr >= USER_BASE && ptr < USER_END;
    if (ptr < USER_BASE || len > USER_END - USER_BASE || ptr + len > USER_END)
        return 0;
    uint64_t pgd = task_current()->mm->pgd;
    for (uint64_t p = ptr & ~0xFFFULL; p < ptr + len; p += 4096)
        if (!vmm_translate_in(pgd, p))
            return 0;
    return 1;
}

/* Copy a NUL-terminated user string; -EFAULT if unmapped, -ENAMETOOLONG if too long. */
static long user_str(uint64_t ptr, char *dst, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if (!user_ok(ptr + i, 1))
            return -EFAULT;
        dst[i] = *(const char *)(ptr + i);
        if (!dst[i])
            return 0;
    }
    return -ENAMETOOLONG;
}

#define FILES (task_current()->fdt->files)

struct file *fd_get(long fd)
{
    if (fd < 0 || fd >= MAX_FDS)
        return NULL;
    return FILES[fd];
}

/* Install f in the first free slot >= from. The table lock only orders slot
 * claims between threads; the file objects themselves are refcounted. */
static int fd_alloc_from(struct file *f, int from)
{
    struct fdtable *t = task_current()->fdt;
    spin_lock(&t->lock);
    for (int i = from; i < MAX_FDS; i++)
        if (!t->files[i]) {
            t->files[i] = f;
            spin_unlock(&t->lock);
            return i;
        }
    spin_unlock(&t->lock);
    return -EMFILE;
}

int fd_install(struct file *f)
{
    return fd_alloc_from(f, 0);
}

/* Resolve a *at() directory fd to a vnode (AT_FDCWD = the cwd). */
static struct vnode *at_dir(long dirfd)
{
    if (dirfd == AT_FDCWD)
        return task_current()->cwd;
    struct file *f = fd_get(dirfd);
    return f && f->node->type == VNODE_DIR ? f->node : NULL;
}

/* ---- files ------------------------------------------------------------------------ */

static long sys_write(long fd, uint64_t buf, uint64_t len)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, len)) return -EFAULT;
    long r = file_write(f, (const void *)buf, len);
    return r == -EPIPE || r == -EINTR ? r : r < 0 ? -EBADF : r;
}

static long sys_read(long fd, uint64_t buf, uint64_t len)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, len)) return -EFAULT;
    long r = file_read(f, (void *)buf, len);
    return r == -EINTR ? -EINTR : r < 0 ? -EBADF : r;
}

static long sys_writev(long fd, uint64_t uiov, long cnt)
{
    if (cnt < 0 || !user_ok(uiov, (uint64_t)cnt * sizeof(struct abi_iovec)))
        return -EFAULT;
    const struct abi_iovec *iov = (const void *)uiov;
    long total = 0;
    for (long i = 0; i < cnt; i++) {
        long r = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

static long sys_readv(long fd, uint64_t uiov, long cnt)
{
    if (cnt < 0 || !user_ok(uiov, (uint64_t)cnt * sizeof(struct abi_iovec)))
        return -EFAULT;
    const struct abi_iovec *iov = (const void *)uiov;
    long total = 0;
    for (long i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        long r = sys_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len)
            break;                          /* short read: don't block for more */
    }
    return total;
}

static long sys_openat(long dirfd, uint64_t upath, long flags)
{
    char path[VFS_PATH_MAX];
    long rc = user_str(upath, path, sizeof(path));
    if (rc) return rc;
    struct vnode *dir = at_dir(dirfd);
    if (!dir) return -EBADF;

    struct vnode *n = vfs_lookup(dir, path);
    if (n && (flags & O_EXCL) && (flags & O_CREAT)) return -EEXIST;
    if (!n && !(flags & O_CREAT)) return -ENOENT;
    if (n && (flags & O_DIRECTORY) && n->type != VNODE_DIR) return -ENOTDIR;
    if (n && n->type == VNODE_DIR && (flags & O_ACCMODE) != O_RDONLY) return -EISDIR;

    struct file *f = vfs_open(dir, path, (int)(flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)));
    if (!f) return -ENOENT;
    int fd = fd_alloc_from(f, 0);
    if (fd < 0) file_close(f);
    return fd;
}

static long sys_close(long fd)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    FILES[fd] = NULL;
    file_close(f);
    return 0;
}

static long sys_lseek(long fd, long off, long whence)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->node->type != VNODE_FILE && !(f->node->type == VNODE_DEV && (f->node->is_block || f->node->seekable)))
        return -ESPIPE;                     /* block devices are seekable, the console isn't */
    long r = file_seek(f, off, (int)whence);
    return r < 0 ? -EINVAL : r;
}

static void fill_stat(struct vnode *n, struct abi_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = n->ino;
    st->st_nlink = 1;
    st->st_mode = n->type == VNODE_DIR ? S_IFDIR | 0755
                : n->type == VNODE_DEV ? (n->is_block ? S_IFBLK : n->is_sock ? S_IFSOCK : S_IFCHR) | 0666
                : S_IFREG | 0755;
    st->st_size = (int64_t)n->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((n->size + 511) / 512);
}

static long sys_newfstatat(long dirfd, uint64_t upath, uint64_t ust, long flags)
{
    if (!user_ok(ust, sizeof(struct abi_stat))) return -EFAULT;
    char path[VFS_PATH_MAX];
    long rc = user_str(upath, path, sizeof(path));
    if (rc) return rc;

    struct vnode *n;
    if ((flags & AT_EMPTY_PATH) && !path[0]) {
        struct file *f = fd_get(dirfd);
        if (!f) return -EBADF;
        n = f->node;
    } else {
        struct vnode *dir = at_dir(dirfd);
        if (!dir) return -EBADF;
        n = vfs_lookup(dir, path);
        if (!n) return -ENOENT;
    }
    fill_stat(n, (struct abi_stat *)ust);
    return 0;
}

static long sys_fstat(long fd, uint64_t ust)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(ust, sizeof(struct abi_stat))) return -EFAULT;
    fill_stat(f->node, (struct abi_stat *)ust);
    return 0;
}

static long sys_getdents64(long fd, uint64_t ubuf, uint64_t len)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->node->type != VNODE_DIR) return -ENOTDIR;
    if (!user_ok(ubuf, len)) return -EFAULT;

    uint8_t *out = (uint8_t *)ubuf;
    uint64_t used = 0;
    struct dirent d;
    while (file_readdir(f, f->pos, &d) == 1) {
        size_t namelen = strlen(d.name);
        uint16_t reclen = (uint16_t)((sizeof(struct abi_dirent64) + namelen + 1 + 7) & ~7UL);
        if (used + reclen > len)
            break;
        struct abi_dirent64 *e = (void *)(out + used);
        e->d_ino = d.ino;
        e->d_off = (int64_t)f->pos + 1;
        e->d_reclen = reclen;
        e->d_type = d.type == VNODE_DIR ? DT_DIR : d.type == VNODE_DEV ? DT_CHR : DT_REG;
        memcpy(e->d_name, d.name, namelen + 1);
        used += reclen;
        f->pos++;
    }
    if (used == 0 && file_readdir(f, f->pos, &d) == 1)
        return -EINVAL;                     /* buffer too small for one entry */
    return (long)used;
}

static long path_op(long dirfd, uint64_t upath, int op)
{
    char path[VFS_PATH_MAX];
    long rc = user_str(upath, path, sizeof(path));
    if (rc) return rc;
    struct vnode *dir = at_dir(dirfd);
    if (!dir) return -EBADF;
    struct vnode *n = vfs_lookup(dir, path);
    switch (op) {
    case 0: /* mkdir */
        if (n) return -EEXIST;
        return vfs_create(dir, path, VNODE_DIR) ? 0 : -ENOENT;
    case 1: /* unlink */
        if (!n) return -ENOENT;
        if (n->type == VNODE_DIR) return -EISDIR;
        return vfs_unlink(dir, path) == 0 ? 0 : -EACCES;
    case 2: /* rmdir */
        if (!n) return -ENOENT;
        if (n->type != VNODE_DIR) return -ENOTDIR;
        if (n->children) return -ENOTEMPTY;
        return vfs_unlink(dir, path) == 0 ? 0 : -EACCES;
    case 3: /* access */
        return n ? 0 : -ENOENT;
    case 4: /* chdir */
        if (!n) return -ENOENT;
        if (n->type != VNODE_DIR) return -ENOTDIR;
        task_current()->cwd = n;
        return 0;
    }
    return -EINVAL;
}

static long sys_getcwd(uint64_t ubuf, uint64_t len)
{
    if (!user_ok(ubuf, len)) return -EFAULT;
    char tmp[VFS_PATH_MAX];
    if (vfs_path_of(task_current()->cwd, tmp, sizeof(tmp)) != 0) return -ENOENT;
    if (strlen(tmp) + 1 > len) return -ERANGE;
    memcpy((void *)ubuf, tmp, strlen(tmp) + 1);
    return (long)(strlen(tmp) + 1);
}

static long sys_dup(long fd)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    int nfd = fd_alloc_from(file_dup(f), 0);
    if (nfd < 0) file_close(f);
    return nfd;
}

static long sys_dup3(long fd, long nfd, long flags)
{
    (void)flags;
    struct file *f = fd_get(fd);
    if (!f || nfd < 0 || nfd >= MAX_FDS) return -EBADF;
    if (fd == nfd) return nfd;
    struct file *old = FILES[nfd];
    FILES[nfd] = file_dup(f);
    if (old)
        file_close(old);
    return nfd;
}

static long sys_fcntl(long fd, long cmd, long arg)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (arg < 0 || arg >= MAX_FDS) return -EINVAL;
        int nfd = fd_alloc_from(file_dup(f), (int)arg);
        if (nfd < 0) file_close(f);
        return nfd;
    }
    case F_GETFD: return 0;
    case F_SETFD: return 0;
    case F_GETFL: return f->flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
    case F_SETFL: f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (int)(arg & (O_APPEND | O_NONBLOCK)); return 0;
    }
    return -EINVAL;
}

static long sys_ioctl(long fd, long ureq, uint64_t arg)
{
    struct file *f = fd_get(fd);
    uint32_t req = (uint32_t)ureq;          /* the libc passes an int: strip sign extension */
    if (!f) return -EBADF;
    if (f->node->type != VNODE_DEV) return -ENOTTY;
    if (f->node->dev && f->node->dev->ioctl) {
        long r = f->node->dev->ioctl(f, req, arg);
        if (r != -ENOTTY)
            return r;
    }
#ifdef CONFIG_NET
    if (f->node->is_sock) {
        if ((req & 0xFF00) != 0x8900) return -ENOTTY;
        if (!user_ok(arg, sizeof(struct abi_ifreq))) return -EFAULT;
        return net_ioctl(req, (void *)arg);
    }
#endif
    if (f->node->is_block) {
        struct blkdev *bd = blkdev_from_vnode(f->node);
        if (req == BLKGETSIZE64) {
            if (!user_ok(arg, 8)) return -EFAULT;
            *(uint64_t *)arg = f->node->size;
            return 0;
        }
        if (req == BLKRRPART && bd) {
            blkdev_rescan(bd);
            return 0;
        }
        return -ENOTTY;
    }
    if (req == TIOCGWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        struct winsize *ws = (void *)arg;
        ws->ws_row = 64; ws->ws_col = 80; ws->ws_xpixel = ws->ws_ypixel = 0;
        return 0;
    }
#ifdef CONFIG_KEYBOARD
    if (req == TIOCSPGRP) {
        if (!user_ok(arg, 4)) return -EFAULT;
        keyboard_set_foreground(*(const uint32_t *)arg);
        return 0;
    }
    if (req == TIOCGPGRP) {
        if (!user_ok(arg, 4)) return -EFAULT;
        *(uint32_t *)arg = keyboard_get_foreground();
        return 0;
    }
#endif
    return -ENOTTY;
}

/* ---- poll / select ------------------------------------------------------------ */

#define POLL_MAX 64

/* Wait until one of the descriptors is ready or `timeout_ms` passes (-1: forever).
 * The wait entries go onto every object's queue before the state is
 * re-checked, so a wakeup between the check and the sleep is not lost. */
static long do_poll(struct abi_pollfd *fds, int n, long timeout_ms)
{
    struct waitqueue *wqs[POLL_MAX];
    struct wait_entry entries[POLL_MAX];
    uint64_t deadline = timeout_ms < 0 ? ~0ULL : timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000;

    for (;;) {
        int ready = 0;
        for (int i = 0; i < n; i++) {
            fds[i].revents = 0;
            wqs[i] = NULL;
            if (fds[i].fd < 0) continue;
            struct file *f = fd_get(fds[i].fd);
            if (!f) { fds[i].revents = POLLNVAL; ready++; continue; }
            int mask = file_poll(f, &wqs[i]);
            fds[i].revents = (int16_t)(mask & (fds[i].events | POLLERR | POLLHUP));
            if (fds[i].revents) ready++;
        }
        if (ready || timeout_ms == 0)
            return ready;
        uint64_t now = timer_ticks();
        if (now >= deadline)
            return 0;

        for (int i = 0; i < n; i++) {
            entries[i].task = task_current();
            if (wqs[i]) __wait_add(wqs[i], &entries[i]);
        }
        int changed = 0;
        for (int i = 0; i < n && !changed; i++) {
            struct file *f = fds[i].fd >= 0 ? fd_get(fds[i].fd) : NULL;
            struct waitqueue *dummy;
            if (f && (file_poll(f, &dummy) & (fds[i].events | POLLERR | POLLHUP)))
                changed = 1;
        }
        int sig = task_signal_pending(task_current());
        if (!changed && !sig) {
            if (deadline == ~0ULL)
                task_block();
            else
                task_sleep_ms((deadline - now) * 1000 / TIMER_HZ + 1);
        }
        for (int i = 0; i < n; i++)
            if (wqs[i]) __wait_remove(wqs[i], &entries[i]);
        if (sig)
            return -EINTR;
    }
}

static long sys_poll(uint64_t ufds, long n, long timeout_ms)
{
    if (n < 0 || n > POLL_MAX) return -EINVAL;
    if (!user_ok(ufds, (uint64_t)n * sizeof(struct abi_pollfd))) return -EFAULT;
    return do_poll((struct abi_pollfd *)ufds, (int)n, timeout_ms);
}

static long sys_ppoll(uint64_t ufds, long n, uint64_t uts)
{
    long timeout = -1;
    if (uts) {
        if (!user_ok(uts, sizeof(struct abi_timespec))) return -EFAULT;
        const struct abi_timespec *ts = (const void *)uts;
        timeout = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
    }
    return sys_poll(ufds, n, timeout);
}

/* pselect6 on top of poll: fd_sets in, fd_sets out. */
static long sys_pselect6(long n, uint64_t urd, uint64_t uwr, uint64_t uex, uint64_t uts)
{
    if (n < 0 || n > POLL_MAX) return -EINVAL;
    size_t bytes = ((size_t)n + 7) / 8;
    if ((urd && !user_ok(urd, bytes)) || (uwr && !user_ok(uwr, bytes)) || (uex && !user_ok(uex, bytes)))
        return -EFAULT;
    long timeout = -1;
    if (uts) {
        if (!user_ok(uts, sizeof(struct abi_timespec))) return -EFAULT;
        const struct abi_timespec *ts = (const void *)uts;
        timeout = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
    }
    uint8_t *rd = (uint8_t *)urd, *wr = (uint8_t *)uwr, *ex = (uint8_t *)uex;
    struct abi_pollfd fds[POLL_MAX];
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        int ev = 0;
        if (rd && (rd[i / 8] >> (i % 8) & 1)) ev |= POLLIN;
        if (wr && (wr[i / 8] >> (i % 8) & 1)) ev |= POLLOUT;
        if (ex && (ex[i / 8] >> (i % 8) & 1)) ev |= POLLPRI;
        if (!ev) continue;
        fds[cnt].fd = i;
        fds[cnt].events = (int16_t)ev;
        cnt++;
    }
    long r = do_poll(fds, cnt, timeout);
    if (r < 0) return r;
    if (rd) memset(rd, 0, bytes);
    if (wr) memset(wr, 0, bytes);
    if (ex) memset(ex, 0, bytes);
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        int fd = fds[i].fd;
        if (fds[i].revents & POLLNVAL) return -EBADF;
        if (rd && (fds[i].events & POLLIN) && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) { rd[fd / 8] |= 1 << (fd % 8); total++; }
        if (wr && (fds[i].events & POLLOUT) && (fds[i].revents & (POLLOUT | POLLERR))) { wr[fd / 8] |= 1 << (fd % 8); total++; }
        if (ex && (fds[i].events & POLLPRI) && (fds[i].revents & POLLPRI)) { ex[fd / 8] |= 1 << (fd % 8); total++; }
    }
    return total;
}

#ifdef CONFIG_PIPES
static long sys_pipe2(uint64_t ufds, long flags)
{
    (void)flags;
    if (!user_ok(ufds, 8)) return -EFAULT;
    struct file *r, *w;
    long rc = pipe_create(&r, &w);
    if (rc) return rc;
    int rfd = fd_alloc_from(r, 0);
    int wfd = rfd >= 0 ? fd_alloc_from(w, 0) : -EMFILE;
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) { FILES[rfd] = NULL; }
        file_close(r);
        file_close(w);
        return -EMFILE;
    }
    ((int *)ufds)[0] = rfd;
    ((int *)ufds)[1] = wfd;
    return 0;
}
#endif

/* ---- memory --------------------------------------------------------------------- */

/* Copy the contents of `f` at `off` into a freshly mapped (writable) range,
 * then apply the requested protection: MAP_PRIVATE file mappings are plain
 * copies here (no page cache, no sharing). */
static long map_file(struct task *t, uint64_t virt, size_t pages, struct file *f, uint64_t off, int prot)
{
    if (f->node->type != VNODE_FILE) return -EACCES;
    if (process_map_anon(t, virt, pages, PROT_READ | PROT_WRITE) != 0) return -ENOMEM;
    uint64_t save = f->pos;
    f->pos = off;
    uint8_t *dst = (uint8_t *)virt;
    size_t left = pages * PAGE_SIZE;
    while (left) {
        long r = file_read(f, dst, left);
        if (r <= 0) break;
        dst += r;
        left -= (size_t)r;
    }
    f->pos = save;
    process_protect(t, virt, pages, prot);
    return 0;
}

static long sys_mmap(uint64_t addr, uint64_t len, long prot, long flags, long fd, long off)
{
    struct task *t = task_current();
    struct mm *mm = t->mm;
    (void)addr;
    if (len == 0) return -EINVAL;
    if (flags & MAP_FIXED) return -EINVAL;          /* not supported: we choose the address */
    if (off & 0xFFF) return -EINVAL;

    struct file *f = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(fd);
        if (!f) return -EBADF;
        if (f->node->type == VNODE_DEV) {
            if (!f->node->dev || !f->node->dev->mmap)
                return -ENODEV;
        } else {
            if (flags & MAP_SHARED) return -ENOSYS;     /* shared file mappings need a page cache */
        }
    }

    size_t pages = PAGE_ALIGN_UP(len) / PAGE_SIZE;
    spin_lock(&mm->lock);
    uint64_t virt = mm->mmap_next;
    if (virt + pages * PAGE_SIZE > USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE) {
        spin_unlock(&mm->lock);
        return -ENOMEM;
    }
    mm->mmap_next = virt + pages * PAGE_SIZE + PAGE_SIZE;    /* guard page between mappings */
    spin_unlock(&mm->lock);
    long rc = 0;
    if (f) {
        if (f->node->type == VNODE_DEV && f->node->dev && f->node->dev->mmap)
            rc = f->node->dev->mmap(f, virt, pages, (uint64_t)off, (int)prot);
        else
            rc = map_file(t, virt, pages, f, (uint64_t)off, (int)prot);
    } else {
        rc = (process_map_anon(t, virt, pages, (int)prot) != 0 ? -ENOMEM : 0);
    }
    if (rc != 0) {
        process_unmap(t, virt, pages);
        return rc;
    }
    return (long)virt;
}

static long sys_mprotect(uint64_t addr, uint64_t len, long prot)
{
    if (addr & 0xFFF) return -EINVAL;
    if (addr < USER_BASE || addr + len > USER_END) return -ENOMEM;
    if (process_protect(task_current(), addr, PAGE_ALIGN_UP(len) / PAGE_SIZE, (int)prot) != 0)
        return -ENOMEM;
    return 0;
}

static long sys_munmap(uint64_t addr, uint64_t len)
{
    if (addr & 0xFFF || !len) return -EINVAL;
    if (addr < USER_BASE || addr + len > USER_END) return -EINVAL;
    process_unmap(task_current(), addr, PAGE_ALIGN_UP(len) / PAGE_SIZE);
    vmm_flush_range(addr, PAGE_ALIGN_UP(len) / PAGE_SIZE);
    return 0;
}

static long sys_brk(uint64_t addr)
{
    struct task *t = task_current();
    struct mm *mm = t->mm;
    spin_lock(&mm->lock);
    long ret = (long)mm->brk_end;
    if (addr == 0 || addr < mm->brk_start)
        goto out;
    uint64_t old = PAGE_ALIGN_UP(mm->brk_end), new = PAGE_ALIGN_UP(addr);
    if (new >= USER_MMAP_BASE)                     /* would run into the mmap area */
        goto out;
    if (new > old) {
        if (process_map_anon(t, old, (new - old) / PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            process_unmap(t, old, (new - old) / PAGE_SIZE);
            goto out;
        }
    } else if (new < old) {
        process_unmap(t, new, (old - new) / PAGE_SIZE);
        vmm_flush_range(new, (old - new) / PAGE_SIZE);
    }
    mm->brk_end = addr;
    ret = (long)addr;
out:
    spin_unlock(&mm->lock);
    return ret;
}

/* ---- process ---------------------------------------------------------------------- */

static long sys_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp)
{
    char path[VFS_PATH_MAX];
    static const int cap = ARG_BYTES;
    char *data = kmalloc(cap);
    const char *argv[ARG_MAX + 1], *envp[ARG_MAX + 1];
    size_t used = 0;
    long rc = user_str(upath, path, sizeof(path));
    if (!data) return -ENOMEM;

    /* Pull the vectors into a kernel buffer; process_exec copies them again
     * into its own arena, so we can free this on any path. */
    const char **lists[2] = { argv, envp };
    uint64_t uv[2] = { uargv, uenvp };
    for (int l = 0; rc == 0 && l < 2; l++) {
        int n = 0;
        for (; uv[l] && n < ARG_MAX; n++) {
            uint64_t slot = uv[l] + (uint64_t)n * sizeof(unsigned long);
            if (!user_ok(slot, sizeof(unsigned long))) { rc = -EFAULT; break; }
            uint64_t sp = *(const unsigned long *)(uintptr_t)slot;
            if (!sp) break;
            rc = user_str(sp, data + used, cap - used);
            if (rc == -ENAMETOOLONG) rc = -E2BIG;
            if (rc) break;
            lists[l][n] = data + used;
            used += strlen(data + used) + 1;
        }
        lists[l][n] = NULL;
    }
    if (rc == 0)
        rc = process_exec(path, argv, envp);        /* only returns on failure */
    kfree(data);
    return rc;
}

static long sys_wait4(long pid, uint64_t ustatus, long options, uint64_t rusage)
{
    (void)rusage;
    if (ustatus && !user_ok(ustatus, sizeof(int))) return -EFAULT;
    if (pid == 0 || pid < -1) pid = -1;             /* no process groups: treat as any child */
    for (;;) {
        int code = 0;
        long r = task_collect_child(pid, &code);
        if (r > 0) {
            if (ustatus)
                *(int *)ustatus = code >= 128 && code < 128 + 32 ? W_SIGNALED(code - 128) : W_EXITCODE(code);
            return r;
        }
        if (r < 0) return -ECHILD;
        if (options & WNOHANG) return 0;
        if (task_signal_pending(task_current())) return -EINTR;
        task_sleep_ms(1);
    }
}

#ifdef __x86_64__
static long sys_arch_prctl(long code, uint64_t addr)
{
    struct task *t = task_current();
    switch (code) {
    case ARCH_SET_FS:
        t->arch.fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!user_ok(addr, 8)) return -EFAULT;
        *(uint64_t *)addr = t->arch.fs_base;
        return 0;
    }
    return -EINVAL;
}
#endif

/* ---- mounting ----------------------------------------------------------------------- */

static long sys_mount(uint64_t usrc, uint64_t utarget, uint64_t utype, long flags, uint64_t udata)
{
    (void)flags; (void)udata;
    char src[VFS_PATH_MAX], target[VFS_PATH_MAX], type[32];
    long rc;
    if ((rc = user_str(utarget, target, sizeof(target))) != 0) return rc;
    if ((rc = user_str(utype, type, sizeof(type))) != 0) return rc;
    struct vnode *dev = NULL;
    if (usrc) {
        if ((rc = user_str(usrc, src, sizeof(src))) != 0) return rc;
        if (src[0] && strcmp(src, "none") != 0) {
            dev = vfs_lookup(task_current()->cwd, src);
            if (!dev) return -ENOENT;
        }
    }
    struct vnode *dir = vfs_lookup(task_current()->cwd, target);
    if (!dir) return -ENOENT;
    if (dir->type != VNODE_DIR) return -ENOTDIR;
    switch (vfs_mount(type, dev, dir, NULL)) {
    case 0:  return 0;
    case -2: return -EBUSY;
    case -3: return -EINVAL;        /* driver refused (bad superblock, wrong device) */
    default: return -ENODEV;
    }
}

static long sys_umount2(uint64_t utarget, long flags)
{
    (void)flags;
    char target[VFS_PATH_MAX];
    long rc = user_str(utarget, target, sizeof(target));
    if (rc) return rc;
    struct vnode *dir = vfs_lookup(task_current()->cwd, target);
    if (!dir) return -ENOENT;
    if (dir->mountpoint)
        dir = dir->mountpoint;      /* path resolved into the mounted root */
    return vfs_umount(dir) == 0 ? 0 : -EINVAL;
}

#ifdef CONFIG_MODULES
/* ---- kernel modules ------------------------------------------------------------------ */

static long sys_init_module(uint64_t uimage, uint64_t len, uint64_t uparams)
{
    if (len == 0 || len > 16 * 1024 * 1024 || !user_ok(uimage, len)) return -EFAULT;
    char params[128] = "";
    if (uparams && user_str(uparams, params, sizeof(params)) != 0) return -EFAULT;
    void *copy = kmalloc(len);              /* don't link straight out of user memory */
    if (!copy) return -ENOMEM;
    memcpy(copy, (const void *)uimage, len);
    long rc = module_load(copy, len, params);
    kfree(copy);
    return rc;
}

static long sys_delete_module(uint64_t uname, long flags)
{
    (void)flags;
    char name[32];
    long rc = user_str(uname, name, sizeof(name));
    if (rc) return rc;
    return module_unload(name);
}

static long sys_query_module(uint64_t ubuf, uint64_t len)
{
    if (!user_ok(ubuf, len)) return -EFAULT;
    return module_list((char *)ubuf, len);
}
#endif /* CONFIG_MODULES */

/* ---- time & misc ------------------------------------------------------------------ */

static long sys_clock_gettime(long clk, uint64_t uts)
{
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return -EINVAL;
    if (!user_ok(uts, sizeof(struct abi_timespec))) return -EFAULT;
    uint64_t ms = timer_ms();
    struct abi_timespec *ts = (void *)uts;
    ts->tv_sec = (int64_t)(ms / 1000);
    ts->tv_nsec = (int64_t)(ms % 1000) * 1000000;
    return 0;
}

static long sys_nanosleep(uint64_t ureq, long flags)
{
    if (!user_ok(ureq, sizeof(struct abi_timespec))) return -EFAULT;
    const struct abi_timespec *ts = (const void *)ureq;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0) return -EINVAL;
    uint64_t ms = (uint64_t)ts->tv_sec * 1000 + (uint64_t)ts->tv_nsec / 1000000;
    if (flags & 1)                                  /* TIMER_ABSTIME */
        ms = ms > timer_ms() ? ms - timer_ms() : 0;
    uint64_t until = timer_ticks() + ms * TIMER_HZ / 1000;
    do {
        task_sleep_ms(ms ? ms : 1);
        if (task_signal_pending(task_current()))
            return -EINTR;
        ms = timer_ticks() < until ? (until - timer_ticks()) * 1000 / TIMER_HZ : 0;
    } while (ms);
    return 0;
}

static long sys_uname(uint64_t ubuf)
{
    if (!user_ok(ubuf, sizeof(struct abi_utsname))) return -EFAULT;
    struct abi_utsname *u = (void *)ubuf;
    memset(u, 0, sizeof(*u));
    memcpy(u->sysname, "sic", 4);
    memcpy(u->nodename, "sic", 4);
    memcpy(u->release, "0.1", 4);
    memcpy(u->version, "zaeboot/ZAE", 12);
    memcpy(u->machine, ARCH_NAME, sizeof(ARCH_NAME));
    return 0;
}

static long sys_getrandom(uint64_t ubuf, uint64_t len)
{
    if (!user_ok(ubuf, len)) return -EFAULT;
    static uint64_t x = 0x2545F4914F6CDD1DULL;
    uint8_t *b = (void *)ubuf;
    for (uint64_t i = 0; i < len; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        b[i] = (uint8_t)(x ^ timer_ticks());
    }
    return (long)len;
}

/* ---- dispatch ---------------------------------------------------------------------- */

int syscall_dispatch(struct syscall_frame *f)
{
    unsigned long a1 = SYSCALL_ARG1(f), a2 = SYSCALL_ARG2(f), a3 = SYSCALL_ARG3(f),
                  a4 = SYSCALL_ARG4(f), a5 = SYSCALL_ARG5(f), a6 = SYSCALL_ARG6(f);
    long ret;

#ifdef CONFIG_SIGNALS
    if (SYSCALL_NR(f) == SYS_rt_sigreturn) {    /* restores the whole frame, result register included */
        sys_rt_sigreturn(f);
        signal_deliver_syscall(f);
        return 1;                               /* full frame restore: every register must survive */
    }
#endif

    switch (SYSCALL_NR(f)) {
    /* process */
    case SYS_exit:          task_exit_code((int)a1 & 0xFF);          /* a thread; the leader ends the process */
    case SYS_exit_group:    task_exit_group((int)a1 & 0xFF);
    case SYS_fork:          ret = process_fork(f); break;
    case SYS_clone:         ret = process_clone(f, a1, a2, a3, a4, a5); break;
    case SYS_futex:         ret = sys_futex(a1, (int)a2, (uint32_t)a3, a4, a5, (uint32_t)a6); break;
    case SYS_execve:        ret = sys_execve(a1, a2, a3); break;
    case SYS_wait4:         ret = sys_wait4((long)a1, a2, (long)a3, a4); break;
    case SYS_getpid:        ret = task_current()->tgid; break;
    case SYS_getppid:       ret = task_current()->parent_id; break;
    case SYS_gettid:        ret = task_current()->id; break;
    case SYS_sched_yield:   task_yield(); ret = 0; break;
    case SYS_set_tid_address:
        task_current()->clear_child_tid = a1;
        ret = task_current()->id; break;
    case SYS_set_robust_list: ret = 0; break;
#ifdef __x86_64__
    case SYS_arch_prctl:    ret = sys_arch_prctl((long)a1, a2); break;
#endif
    case SYS_getcpu:
        if (a1 && user_ok(a1, 4)) *(uint32_t *)a1 = this_cpu()->index;
        if (a2 && user_ok(a2, 4)) *(uint32_t *)a2 = 0;
        ret = 0; break;
#ifdef CONFIG_SIGNALS
    case SYS_kill:          ret = sys_kill((long)a1, (int)a2); break;
    case SYS_tkill:         ret = sys_tkill((long)a1, (int)a2); break;
    case SYS_tgkill:        ret = sys_tkill((long)a2, (int)a3); break;
#else
    case SYS_kill: case SYS_tkill: case SYS_tgkill:
        ret = -ENOSYS; break;
#endif

    /* time & system */
    case SYS_nanosleep:     ret = sys_nanosleep(a1, 0); break;
    case SYS_clock_nanosleep: ret = sys_nanosleep(a3, (long)a2); break;
    case SYS_clock_gettime: ret = sys_clock_gettime((long)a1, a2); break;
    case SYS_clock_getres:
        if (a2 && user_ok(a2, 16)) { ((struct abi_timespec *)a2)->tv_sec = 0; ((struct abi_timespec *)a2)->tv_nsec = 1000000; }
        ret = 0; break;
    case SYS_uname:         ret = sys_uname(a1); break;
    case SYS_getrandom:     ret = sys_getrandom(a1, a2); break;

    /* files */
    case SYS_read:          ret = sys_read((long)a1, a2, a3); break;
    case SYS_write:         ret = sys_write((long)a1, a2, a3); break;
    case SYS_readv:         ret = sys_readv((long)a1, a2, (long)a3); break;
    case SYS_writev:        ret = sys_writev((long)a1, a2, (long)a3); break;
    case SYS_open:          ret = sys_openat(AT_FDCWD, a1, (long)a2); break;
    case SYS_openat:        ret = sys_openat((long)a1, a2, (long)a3); break;
    case SYS_close:         ret = sys_close((long)a1); break;
    case SYS_lseek:         ret = sys_lseek((long)a1, (long)a2, (long)a3); break;
    case SYS_stat:          ret = sys_newfstatat(AT_FDCWD, a1, a2, 0); break;
    case SYS_lstat:         ret = sys_newfstatat(AT_FDCWD, a1, a2, 0); break;
    case SYS_fstat:         ret = sys_fstat((long)a1, a2); break;
    case SYS_newfstatat:    ret = sys_newfstatat((long)a1, a2, a3, (long)a4); break;
    case SYS_getdents64:    ret = sys_getdents64((long)a1, a2, a3); break;
    case SYS_mkdir:         ret = path_op(AT_FDCWD, a1, 0); break;
    case SYS_mkdirat:       ret = path_op((long)a1, a2, 0); break;
    case SYS_unlink:        ret = path_op(AT_FDCWD, a1, 1); break;
    case SYS_unlinkat:      ret = path_op((long)a1, a2, (a3 & 0x200) ? 2 : 1); break;   /* AT_REMOVEDIR */
    case SYS_rmdir:         ret = path_op(AT_FDCWD, a1, 2); break;
    case SYS_access:        ret = path_op(AT_FDCWD, a1, 3); break;
    case SYS_faccessat:     ret = path_op((long)a1, a2, 3); break;
    case SYS_chdir:         ret = path_op(AT_FDCWD, a1, 4); break;
    case SYS_getcwd:        ret = sys_getcwd(a1, a2); break;
    case SYS_dup:           ret = sys_dup((long)a1); break;
    case SYS_dup2:          ret = sys_dup3((long)a1, (long)a2, 0); break;
    case SYS_dup3:          ret = sys_dup3((long)a1, (long)a2, (long)a3); break;
    case SYS_fcntl:         ret = sys_fcntl((long)a1, (long)a2, (long)a3); break;
    case SYS_ioctl:         ret = sys_ioctl((long)a1, (long)a2, a3); break;
    case SYS_fsync: case SYS_fdatasync: case SYS_sync:
        ret = fd_get((long)a1) || SYSCALL_NR(f) == SYS_sync ? 0 : -EBADF; break;
    case SYS_umask:         ret = 022; break;
#ifdef CONFIG_PIPES
    case SYS_pipe:          ret = sys_pipe2(a1, 0); break;
    case SYS_pipe2:         ret = sys_pipe2(a1, (long)a2); break;
#endif
    case SYS_chmod: case SYS_fchmod: case SYS_fchmodat:
        ret = 0; break;                     /* no permissions yet */
    case SYS_poll:          ret = sys_poll(a1, (long)a2, (long)(int)a3); break;
    case SYS_ppoll:         ret = sys_ppoll(a1, (long)a2, a3); break;
    case SYS_pselect6:      ret = sys_pselect6((long)a1, a2, a3, a4, a5); break;

#ifdef CONFIG_NET
    case SYS_socket:      ret = sys_socket((long)a1, (long)a2, (long)a3); break;
    case SYS_bind:        ret = sys_bind((long)a1, a2, a3); break;
    case SYS_listen:      ret = sys_listen((long)a1, (long)a2); break;
    case SYS_connect:     ret = sys_connect((long)a1, a2, a3); break;
    case SYS_accept:      ret = sys_accept4((long)a1, a2, a3, 0); break;
    case SYS_accept4:     ret = sys_accept4((long)a1, a2, a3, (long)a4); break;
    case SYS_shutdown:    ret = sys_shutdown((long)a1, (long)a2); break;
    case SYS_getsockname: ret = sys_getsockname((long)a1, a2, a3); break;
    case SYS_getpeername: ret = sys_getpeername((long)a1, a2, a3); break;
    case SYS_setsockopt:  ret = sys_setsockopt((long)a1, (long)a2, (long)a3, a4, a5); break;
    case SYS_getsockopt:  ret = sys_getsockopt((long)a1, (long)a2, (long)a3, a4, a5); break;
    case SYS_sendto:      ret = sys_sendto((long)a1, a2, a3, (long)a4, a5, a6); break;
    case SYS_recvfrom:    ret = sys_recvfrom((long)a1, a2, a3, (long)a4, a5, a6); break;
    case SYS_sendmsg:     ret = sys_sendmsg((long)a1, a2, (long)a3); break;
    case SYS_recvmsg:     ret = sys_recvmsg((long)a1, a2, (long)a3); break;
    case SYS_socketpair:  ret = -EOPNOTSUPP; break;
#endif

    /* sic extensions */
    case SYS_mount:         ret = sys_mount(a1, a2, a3, (long)a4, a5); break;
    case SYS_umount2:       ret = sys_umount2(a1, (long)a2); break;
#ifdef CONFIG_MODULES
    case SYS_init_module:   ret = sys_init_module(a1, a2, a3); break;
    case SYS_delete_module: ret = sys_delete_module(a1, (long)a2); break;
    case SYS_query_module:  ret = sys_query_module(a1, a2); break;
#endif

    /* memory */
    case SYS_mmap:          ret = sys_mmap(a1, a2, (long)a3, (long)a4, (long)a5, (long)a6); break;
    case SYS_munmap:        ret = sys_munmap(a1, a2); break;
    case SYS_brk:           ret = sys_brk(a1); break;
    case SYS_mprotect:      ret = sys_mprotect(a1, a2, (long)a3); break;
    case SYS_madvise: case SYS_msync:
        ret = 0; break;
    case SYS_mremap:        ret = -ENOMEM; break;

#ifdef CONFIG_SIGNALS
    case SYS_rt_sigaction:   ret = sys_rt_sigaction((int)a1, a2, a3, a4); break;
    case SYS_rt_sigprocmask: ret = sys_rt_sigprocmask((int)a1, a2, a3, a4); break;
    case SYS_rt_sigpending:  ret = sys_rt_sigpending(a1, a2); break;
    case SYS_rt_sigsuspend:  ret = sys_rt_sigsuspend(a1, a2); break;
    case SYS_sigaltstack:    ret = 0; break;
    case SYS_pause:          ret = sys_pause(); break;
    case SYS_alarm:          ret = sys_alarm(a1); break;
    case SYS_setitimer:      ret = sys_setitimer((int)a1, a2, a3); break;
    case SYS_getitimer:      ret = sys_getitimer((int)a1, a2); break;
#else
    /* signals configured out: pretend to accept, never deliver */
    case SYS_rt_sigaction:  ret = 0; break;
    case SYS_rt_sigprocmask:
        if (a3 && user_ok(a3, 8)) *(uint64_t *)a3 = 0;
        ret = 0; break;
    case SYS_sigaltstack:   ret = 0; break;
#endif

    /* credentials & limits: single-user system, everything is root */
    case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
        ret = 0; break;
    case SYS_setuid: case SYS_setgid: case SYS_setpgid: case SYS_setsid:
        ret = 0; break;
    case SYS_getpgid: case SYS_getpgrp: case SYS_getsid:
        ret = task_current()->tgid; break;
    case SYS_sched_getaffinity:
        if (a3 && user_ok(a3, sizeof(unsigned long))) {
            *(unsigned long *)a3 = (1UL << smp_cpu_count()) - 1;
            ret = sizeof(unsigned long);
        } else ret = -EFAULT;
        break;
    case SYS_membarrier:    ret = 0; break;
    case SYS_prlimit64: case SYS_getrlimit: case SYS_setrlimit:
        ret = -ENOSYS; break;

    default:
        if (SYSCALL_NR(f) > SYS_sic_max)
            kprintf("[%s: bad syscall %lu]\n", task_current()->name, (unsigned long)SYSCALL_NR(f));
        ret = -ENOSYS;
    }
    SYSCALL_SET_RET(f, ret);
    signal_deliver_syscall(f);              /* may redirect the return into a handler */
    return 0;
}
