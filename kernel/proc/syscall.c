/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* System call layer: the sic ABI (abi/syscall.tbl) with POSIX semantics as
 * musl expects them. Handlers return a value or -errno. */
#include "proc/syscall.h"
#include "arch/x86_64/cpu.h"
#include "proc/sched.h"
#include "arch/x86_64/timer.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "fs/vfs.h"
#include "proc/elf.h"
#include "mm/heap.h"
#include "module.h"
#include "string.h"
#include "printf.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE   (1 << 0)

extern void syscall_entry(void);

void syscall_init_cpu(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* syscall: CS = 0x08, SS = 0x10; sysret: CS = 0x10+16 | 3, SS = 0x10+8 | 3 */
    wrmsr(MSR_STAR, ((uint64_t)SEL_KDATA << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, (1 << 9) | (1 << 8) | (1 << 10) | (1 << 18));   /* IF, TF, DF, AC */
}

/* ---- user memory access ------------------------------------------------------ */

/* The caller's address space is live, so a range is usable once we've
 * checked it lies in user space and every page of it is mapped. */
static int user_ok(uint64_t ptr, uint64_t len)
{
    if (len == 0)
        return ptr >= USER_BASE && ptr < USER_END;
    if (ptr < USER_BASE || len > USER_END - USER_BASE || ptr + len > USER_END)
        return 0;
    uint64_t pml4 = task_current()->pml4;
    for (uint64_t p = ptr & ~0xFFFUL; p < ptr + len; p += 4096)
        if (!vmm_translate_in(pml4, p))
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

static struct file *fd_get(long fd)
{
    if (fd < 0 || fd >= MAX_FDS)
        return NULL;
    return task_current()->files[fd];
}

static int fd_alloc_from(struct file *f, int from)
{
    struct task *t = task_current();
    for (int i = from; i < MAX_FDS; i++)
        if (!t->files[i]) {
            t->files[i] = f;
            return i;
        }
    return -EMFILE;
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
    return r < 0 ? -EBADF : r;
}

static long sys_read(long fd, uint64_t buf, uint64_t len)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, len)) return -EFAULT;
    long r = file_read(f, (void *)buf, len);
    return r < 0 ? -EBADF : r;
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
    task_current()->files[fd] = NULL;
    file_close(f);
    return 0;
}

static long sys_lseek(long fd, long off, long whence)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->node->type != VNODE_FILE && !(f->node->type == VNODE_DEV && f->node->size))
        return -ESPIPE;                     /* block devices are seekable, the console isn't */
    long r = file_seek(f, off, (int)whence);
    return r < 0 ? -EINVAL : r;
}

static void fill_stat(struct vnode *n, struct abi_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = n->ino;
    st->st_nlink = 1;
    st->st_mode = (n->type == VNODE_DIR ? S_IFDIR | 0755 : n->type == VNODE_DEV ? S_IFCHR | 0666 : S_IFREG | 0755);
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
    struct task *t = task_current();
    if (t->files[nfd])
        file_close(t->files[nfd]);
    t->files[nfd] = file_dup(f);
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
    case F_GETFL: return f->flags & (O_ACCMODE | O_APPEND);
    case F_SETFL: f->flags = (f->flags & O_ACCMODE) | (int)(arg & O_APPEND); return 0;
    }
    return -EINVAL;
}

static long sys_ioctl(long fd, long req, uint64_t arg)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->node->type != VNODE_DEV) return -ENOTTY;
    if (req == TIOCGWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        struct winsize *ws = (void *)arg;
        ws->ws_row = 64; ws->ws_col = 80; ws->ws_xpixel = ws->ws_ypixel = 0;
        return 0;
    }
    return -ENOTTY;
}

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
    (void)addr;
    if (len == 0) return -EINVAL;
    if (flags & MAP_FIXED) return -EINVAL;          /* not supported: we choose the address */
    if (off & 0xFFF) return -EINVAL;

    struct file *f = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(fd);
        if (!f) return -EBADF;
        if (flags & MAP_SHARED) return -ENOSYS;     /* shared file mappings need a page cache */
    }

    size_t pages = PAGE_ALIGN_UP(len) / PAGE_SIZE;
    uint64_t virt = t->mmap_next;
    if (virt + pages * PAGE_SIZE > USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE)
        return -ENOMEM;
    long rc = f ? map_file(t, virt, pages, f, (uint64_t)off, (int)prot)
                : (process_map_anon(t, virt, pages, (int)prot) != 0 ? -ENOMEM : 0);
    if (rc != 0) {
        process_unmap(t, virt, pages);
        return rc;
    }
    t->mmap_next = virt + pages * PAGE_SIZE + PAGE_SIZE;    /* guard page between mappings */
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
    if (addr == 0 || addr < t->brk_start)
        return (long)t->brk_end;
    uint64_t old = PAGE_ALIGN_UP(t->brk_end), new = PAGE_ALIGN_UP(addr);
    if (new >= 0x0000010000000000UL)              /* would run into the mmap area */
        return (long)t->brk_end;
    if (new > old) {
        if (process_map_anon(t, old, (new - old) / PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            process_unmap(t, old, (new - old) / PAGE_SIZE);
            return (long)t->brk_end;
        }
    } else if (new < old) {
        process_unmap(t, new, (old - new) / PAGE_SIZE);
        vmm_flush_range(new, (old - new) / PAGE_SIZE);
    }
    t->brk_end = addr;
    return (long)addr;
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
            if (!user_ok(uv[l] + n * 8, 8)) { rc = -EFAULT; break; }
            uint64_t sp = *(const uint64_t *)(uv[l] + n * 8);
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
        task_sleep_ms(1);
    }
}

static long sys_arch_prctl(long code, uint64_t addr)
{
    struct task *t = task_current();
    switch (code) {
    case ARCH_SET_FS:
        t->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!user_ok(addr, 8)) return -EFAULT;
        *(uint64_t *)addr = t->fs_base;
        return 0;
    }
    return -EINVAL;
}

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
    task_sleep_ms(ms ? ms : 1);
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
    memcpy(u->machine, "x86_64", 7);
    return 0;
}

static long sys_getrandom(uint64_t ubuf, uint64_t len)
{
    if (!user_ok(ubuf, len)) return -EFAULT;
    static uint64_t x = 0x2545F4914F6CDD1DUL;
    uint8_t *b = (void *)ubuf;
    for (uint64_t i = 0; i < len; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        b[i] = (uint8_t)(x ^ timer_ticks());
    }
    return (long)len;
}

/* ---- dispatch ---------------------------------------------------------------------- */

void syscall_dispatch(struct syscall_frame *f)
{
    uint64_t a1 = f->rdi, a2 = f->rsi, a3 = f->rdx, a4 = f->r10, a5 = f->r8, a6 = f->r9;
    long ret;

    switch (f->rax) {
    /* process */
    case SYS_exit:
    case SYS_exit_group:    task_exit_code((int)a1 & 0xFF);          /* no return */
    case SYS_fork:          ret = process_fork(f); break;
    case SYS_execve:        ret = sys_execve(a1, a2, a3); break;
    case SYS_wait4:         ret = sys_wait4((long)a1, a2, (long)a3, a4); break;
    case SYS_getpid:        ret = task_current()->id; break;
    case SYS_getppid:       ret = task_current()->parent_id; break;
    case SYS_gettid:        ret = task_current()->id; break;
    case SYS_sched_yield:   task_yield(); ret = 0; break;
    case SYS_set_tid_address: ret = task_current()->id; break;
    case SYS_set_robust_list: ret = 0; break;
    case SYS_arch_prctl:    ret = sys_arch_prctl((long)a1, a2); break;
    case SYS_getcpu:
        if (a1 && user_ok(a1, 4)) *(uint32_t *)a1 = this_cpu()->index;
        if (a2 && user_ok(a2, 4)) *(uint32_t *)a2 = 0;
        ret = 0; break;
    case SYS_kill: case SYS_tkill: case SYS_tgkill:
        ret = -ENOSYS; break;

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
        ret = fd_get((long)a1) || f->rax == SYS_sync ? 0 : -EBADF; break;
    case SYS_umask:         ret = 022; break;
    case SYS_chmod: case SYS_fchmod: case SYS_fchmodat:
        ret = 0; break;                     /* no permissions yet */
    case SYS_poll: case SYS_ppoll:
        ret = 0; break;

    /* sic extensions */
    case SYS_mount:         ret = sys_mount(a1, a2, a3, (long)a4, a5); break;
    case SYS_umount2:       ret = sys_umount2(a1, (long)a2); break;
    case SYS_init_module:   ret = sys_init_module(a1, a2, a3); break;
    case SYS_delete_module: ret = sys_delete_module(a1, (long)a2); break;
    case SYS_query_module:  ret = sys_query_module(a1, a2); break;

    /* memory */
    case SYS_mmap:          ret = sys_mmap(a1, a2, (long)a3, (long)a4, (long)a5, (long)a6); break;
    case SYS_munmap:        ret = sys_munmap(a1, a2); break;
    case SYS_brk:           ret = sys_brk(a1); break;
    case SYS_mprotect:      ret = sys_mprotect(a1, a2, (long)a3); break;
    case SYS_madvise: case SYS_msync:
        ret = 0; break;
    case SYS_mremap:        ret = -ENOMEM; break;

    /* signals: accepted but never delivered (no signal support yet) */
    case SYS_rt_sigaction:  ret = 0; break;
    case SYS_rt_sigprocmask:
        if (a3 && user_ok(a3, 8)) *(uint64_t *)a3 = 0;
        ret = 0; break;
    case SYS_sigaltstack:   ret = 0; break;

    /* credentials & limits: single-user system, everything is root */
    case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
        ret = 0; break;
    case SYS_setuid: case SYS_setgid: case SYS_setpgid: case SYS_setsid:
        ret = 0; break;
    case SYS_getpgid: case SYS_getpgrp: case SYS_getsid:
        ret = task_current()->id; break;
    case SYS_prlimit64: case SYS_getrlimit: case SYS_setrlimit:
        ret = -ENOSYS; break;

    default:
        if (f->rax > SYS_sic_max)
            kprintf("[%s: bad syscall %lu]\n", task_current()->name, f->rax);
        ret = -ENOSYS;
    }
    f->rax = (uint64_t)ret;
}
