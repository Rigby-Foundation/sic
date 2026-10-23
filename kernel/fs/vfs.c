/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Virtual filesystem: a tree of vnodes, filesystem drivers behind vnode_ops,
 * mount points, and refcounted open files. One big lock for the tree. */
#include "fs/vfs.h"
#include "abi/abi.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "fs/blkdev.h"
#include "asm/timer.h"
#include "abi/zaefs.h"

static struct vnode *root;
static struct vfs_mount root_mount;
static struct fs_type *fs_types;
static spinlock_t vfs_lock = SPINLOCK_INIT;

extern struct fs_type tmpfs_type, zaefs_type, fat_type;

/* ---- vnode helpers ------------------------------------------------------------ */

struct vnode *vnode_alloc(const char *name, size_t namelen, enum vnode_type type, struct vnode *parent)
{
    struct vnode *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    if (namelen >= VFS_NAME_MAX)
        namelen = VFS_NAME_MAX - 1;
    memcpy(n->name, name, namelen);
    n->type = type;
    n->parent = parent;
    n->mtime_ms = n->ctime_ms = vfs_now_ms();
    if (parent) {
        n->mnt = parent->mnt;
        n->ops = parent->ops;
        n->sibling = parent->children;
        parent->children = n;
    }
    return n;
}

uint64_t vfs_now_ms(void)
{
    return timer_boot_epoch() * 1000 + timer_ms();
}

void vfs_touch(struct vnode *n)
{
    n->mtime_ms = n->ctime_ms = vfs_now_ms();
}

void vnode_free(struct vnode *n)
{
    kfree(n);
}

static void detach(struct vnode *n)
{
    if (!n->parent)
        return;
    struct vnode **pp = &n->parent->children;
    while (*pp && *pp != n)
        pp = &(*pp)->sibling;
    if (*pp)
        *pp = n->sibling;
}

/* Directory lookup with the entry cache; follows mounts. */
static struct vnode *dir_find(struct vnode *dir, const char *name, size_t len)
{
    if (len == 1 && name[0] == '.')
        return dir;
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        struct vnode *up = dir->mountpoint ? dir->mountpoint : dir;
        return up->parent ? up->parent : up;
    }
    struct vnode *c;
    for (c = dir->children; c; c = c->sibling)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
            break;
    if (!c && dir->ops && dir->ops->lookup)
        c = dir->ops->lookup(dir, name, len);
    if (c && c->mounted)
        c = c->mounted;
    return c;
}

#define SYMLOOP_MAX 16                      /* links followed in one lookup, at most */

static struct vnode *resolve(struct vnode *cwd, const char *path, int follow_last, int *depth);

/* n, or where it leads if it is a symbolic link (a relative target counts
 * from dir, the directory holding the link). NULL: dangling, or a loop. */
static struct vnode *follow(struct vnode *dir, struct vnode *n, int *depth)
{
    while (n && n->type == VNODE_LINK) {
        if (++*depth > SYMLOOP_MAX || !n->link) return NULL;
        n = resolve(dir, n->link, 1, depth);
    }
    return n;
}

/* Walk all components but the last (following links on the way); returns
 * the parent dir and points last/lastlen at the final component (may be
 * empty for "/"). */
static struct vnode *walk_parent_d(struct vnode *cwd, const char *path, const char **last, size_t *lastlen, int *depth)
{
    struct vnode *dir = (*path == '/') ? vfs_root() : (cwd ? cwd : vfs_root());
    while (*path == '/')
        path++;

    for (;;) {
        const char *seg = path;
        while (*path && *path != '/')
            path++;
        size_t len = path - seg;
        while (*path == '/')
            path++;
        if (*path == '\0') {
            *last = seg;
            *lastlen = len;
            return dir;
        }
        if (dir->type != VNODE_DIR)
            return NULL;
        struct vnode *next = dir_find(dir, seg, len);
        dir = follow(dir, next, depth);
        if (!dir)
            return NULL;
    }
}

static struct vnode *walk_parent(struct vnode *cwd, const char *path, const char **last, size_t *lastlen)
{
    int depth = 0;
    return walk_parent_d(cwd, path, last, lastlen, &depth);
}

static struct vnode *resolve(struct vnode *cwd, const char *path, int follow_last, int *depth)
{
    if (!path || !*path)
        return NULL;
    const char *last;
    size_t len;
    struct vnode *dir = walk_parent_d(cwd, path, &last, &len, depth);
    if (!dir)
        return NULL;
    if (len == 0)
        return dir;
    if (dir->type != VNODE_DIR)
        return NULL;
    struct vnode *n = dir_find(dir, last, len);
    return follow_last ? follow(dir, n, depth) : n;
}

static struct vnode *lookup_locked(struct vnode *cwd, const char *path)
{
    int depth = 0;
    return resolve(cwd, path, 1, &depth);
}

static struct vnode *lookup_nofollow_locked(struct vnode *cwd, const char *path)
{
    int depth = 0;
    return resolve(cwd, path, 0, &depth);
}

static struct vnode *create_locked(struct vnode *cwd, const char *path, enum vnode_type type)
{
    const char *last;
    size_t len;
    struct vnode *dir = walk_parent(cwd, path, &last, &len);
    if (!dir || dir->type != VNODE_DIR || len == 0)
        return NULL;
    if (len == 1 && last[0] == '.')
        return NULL;
    struct vnode *existing = dir_find(dir, last, len);
    if (existing)
        return existing->type == type ? existing : NULL;
    if (!dir->ops || !dir->ops->create)
        return NULL;
    return dir->ops->create(dir, last, len, type);
}

/* ---- init, filesystems, mounts --------------------------------------------------- */

void vfs_register_fs(struct fs_type *fs)
{
    spin_lock(&vfs_lock);
    fs->next = fs_types;
    fs_types = fs;
    spin_unlock(&vfs_lock);
}

static struct fs_type *find_fs(const char *name)
{
    for (struct fs_type *f = fs_types; f; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

void vfs_init(void)
{
    vfs_register_fs(&tmpfs_type);
#ifdef CONFIG_ZAEFS
    vfs_register_fs(&zaefs_type);
#endif
#ifdef CONFIG_FAT
    vfs_register_fs(&fat_type);
#endif
#ifdef CONFIG_EXT4
    extern struct fs_type ext4_type, ext3_type, ext2_type;
    vfs_register_fs(&ext4_type);
    vfs_register_fs(&ext3_type);
    vfs_register_fs(&ext2_type);
#endif
    root_mount.type = &tmpfs_type;
    root = tmpfs_type.mount(&root_mount, NULL, NULL);
    root_mount.root = root;
    kprintf("vfs: tmpfs root mounted\n");
}

struct vnode *vfs_root(void) { return (root && root->mounted) ? root->mounted : root; }

int vfs_mount(const char *fstype, struct vnode *dev, struct vnode *dir, const char *opts)
{
    struct fs_type *fs = find_fs(fstype);
    if (!fs || !dir || dir->type != VNODE_DIR)
        return -1;
    if (dir->mounted || dir->mountpoint)
        return -2;                          /* already a mount point */

    struct vfs_mount *mnt = kzalloc(sizeof(*mnt));
    if (!mnt)
        return -1;
    mnt->type = fs;
    struct vnode *r = fs->mount(mnt, dev, opts);
    if (!r) {
        kfree(mnt);
        return -3;
    }
    mnt->root = r;
    spin_lock(&vfs_lock);
    r->mountpoint = dir;
    dir->mounted = r;
    if (dir == root) {
        /* When mounting over root, mount old /dev and /proc onto new /dev and /proc */
        struct vnode *old_dev = dir_find(root, "dev", 3);
        struct vnode *old_proc = dir_find(root, "proc", 4);
        if (old_dev) {
            struct vnode *new_dev = dir_find(r, "dev", 3);
            if (!new_dev && r->ops && r->ops->create)
                new_dev = r->ops->create(r, "dev", 3, VNODE_DIR);
            if (new_dev && !new_dev->mounted) {
                new_dev->mounted = old_dev;
                old_dev->mountpoint = new_dev;
            }
        }
        if (old_proc) {
            struct vnode *new_proc = dir_find(r, "proc", 4);
            if (!new_proc && r->ops && r->ops->create)
                new_proc = r->ops->create(r, "proc", 4, VNODE_DIR);
            if (new_proc && !new_proc->mounted) {
                new_proc->mounted = old_proc;
                old_proc->mountpoint = new_proc;
            }
        }
    }
    spin_unlock(&vfs_lock);
    return 0;
}

int vfs_umount(struct vnode *dir)
{
    if (!dir || !dir->mounted)
        return -1;
    struct vnode *r = dir->mounted;
    struct vfs_mount *mnt = r->mnt;
    spin_lock(&vfs_lock);
    dir->mounted = NULL;
    r->mountpoint = NULL;
    spin_unlock(&vfs_lock);
    if (mnt->type->unmount)
        mnt->type->unmount(mnt);
    kfree(mnt);
    blkdev_flush_all();
    return 0;
}

/* ---- public: namespace ----------------------------------------------------------- */

struct vnode *vfs_lookup(struct vnode *cwd, const char *path)
{
    spin_lock(&vfs_lock);
    struct vnode *n = lookup_locked(cwd, path);
    spin_unlock(&vfs_lock);
    return n;
}

struct vnode *vfs_lookup_nofollow(struct vnode *cwd, const char *path)
{
    spin_lock(&vfs_lock);
    struct vnode *n = lookup_nofollow_locked(cwd, path);
    spin_unlock(&vfs_lock);
    return n;
}

int vfs_symlink(struct vnode *cwd, const char *target, const char *path)
{
    size_t tl = strlen(target);
    if (!tl || tl >= VFS_PATH_MAX) return -ENOENT;
    char *copy = kmalloc(tl + 1);
    if (!copy) return -ENOMEM;
    memcpy(copy, target, tl + 1);
    spin_lock(&vfs_lock);
    int rc = 0;
    const char *last; size_t len;
    struct vnode *dir = walk_parent(cwd, path, &last, &len);
    if (!dir || dir->type != VNODE_DIR || len == 0) rc = -ENOENT;
    else if (dir_find(dir, last, len)) rc = -EEXIST;
    else if (!dir->ops || !dir->ops->create) rc = -EPERM;
    else {
        struct vnode *n = dir->ops->create(dir, last, len, VNODE_LINK);
        if (!n) rc = -EPERM;                /* a filesystem without links */
        else {
            n->link = copy; n->size = tl; copy = NULL;
            if (n->mnt && n->mnt->type && strcmp(n->mnt->type->name, "tmpfs") != 0) {
                if (n->ops && n->ops->write) {
                    n->ops->write(n, 0, target, tl);
                    if (n->ops->sync) n->ops->sync(n);
                }
            }
        }
    }
    spin_unlock(&vfs_lock);
    kfree(copy);
    return rc;
}

int vfs_link(struct vnode *cwd, const char *oldpath, struct vnode *newcwd, const char *newpath)
{
    spin_lock(&vfs_lock);
    int rc = 0;
    struct vnode *target = lookup_nofollow_locked(cwd, oldpath);
    const char *last; size_t len;
    struct vnode *newdir = walk_parent(newcwd, newpath, &last, &len);
    if (!target) rc = -ENOENT;
    else if (target->type == VNODE_DIR) rc = -EPERM;
    else if (!newdir || newdir->type != VNODE_DIR || len == 0 || len >= VFS_NAME_MAX) rc = -ENOENT;
    else if (newdir->mnt != target->mnt) rc = -EXDEV;
    else if (dir_find(newdir, last, len)) rc = -EEXIST;
    else if (!newdir->ops || !newdir->ops->link) rc = -EPERM;
    else rc = newdir->ops->link(newdir, target, last, len);
    spin_unlock(&vfs_lock);
    return rc;
}

int vfs_readlink(struct vnode *n, char *buf, size_t len)
{
    if (!n || n->type != VNODE_LINK || !n->link) return -EINVAL;
    size_t l = strlen(n->link);
    if (l > len) l = len;
    memcpy(buf, n->link, l);
    return (int)l;
}

struct vnode *vfs_create(struct vnode *cwd, const char *path, enum vnode_type type)
{
    spin_lock(&vfs_lock);
    struct vnode *n = create_locked(cwd, path, type);
    spin_unlock(&vfs_lock);
    return n;
}

int vfs_mkdev(const char *path, const struct dev_ops *ops, void *priv)
{
    struct vnode *n = vfs_create(root, path, VNODE_DEV);
    if (!n)
        return -1;
    n->dev = ops;
    n->priv = priv;
    return 0;
}

int vfs_unlink(struct vnode *cwd, const char *path)
{
    spin_lock(&vfs_lock);
    struct vnode *n = lookup_nofollow_locked(cwd, path);
    int rc = -1;
    uint64_t pos = 0;
    if (n && n->parent && !n->mountpoint && !n->mounted &&
        (n->type != VNODE_DIR || !n->children) &&
        (n->type != VNODE_DIR || !n->ops->readdir || n->ops->readdir(n, &pos, &(struct dirent){0}) == 0)) {
        rc = n->ops && n->ops->unlink ? n->ops->unlink(n->parent, n) : 0;
        if (rc == 0) {
            detach(n);
            kfree(n->link);
            n->link = NULL;
            n->unlinked = 1;
            if (n->open_count == 0) {
                if (n->ops && n->ops->release)
                    n->ops->release(n);
                vnode_free(n);
            }
        }
    }
    spin_unlock(&vfs_lock);
    return rc;
}

/* rename(2): within one filesystem. An existing target file is replaced.
 * The entry cache is the truth for tmpfs; a disk filesystem moves its
 * directory entry through its rename op. */
int vfs_rename(struct vnode *cwd, const char *oldpath, struct vnode *newcwd, const char *newpath)
{
    spin_lock(&vfs_lock);
    int rc = 0;
    struct vnode *n = lookup_nofollow_locked(cwd, oldpath);
    const char *last; size_t len;
    struct vnode *newdir = walk_parent(newcwd, newpath, &last, &len);
    if (!n || !n->parent) rc = -ENOENT;
    else if (!newdir || newdir->type != VNODE_DIR || len == 0 || len >= VFS_NAME_MAX) rc = -ENOENT;
    else if (n->mountpoint || n->mounted) rc = -EBUSY;
    else if (newdir->mnt != n->mnt) rc = -EXDEV;
    else if (!n->ops->rename && !(n->mnt && n->mnt->type && strcmp(n->mnt->type->name, "tmpfs") == 0)) rc = -ENOSYS;
    if (rc) goto out;
    struct vnode *target = dir_find(newdir, last, len);
    if (target == n) goto out;
    if (target) {
        if (target->type == VNODE_DIR && (target->children || n->type != VNODE_DIR)) { rc = target->children ? -ENOTEMPTY : -EISDIR; goto out; }
        if (target->type != VNODE_DIR && n->type == VNODE_DIR) { rc = -ENOTDIR; goto out; }
        if (target->ops && target->ops->unlink && target->ops->unlink(newdir, target) != 0) { rc = -EIO; goto out; }
        detach(target);
        kfree(target->link);
        target->link = NULL;
        target->unlinked = 1;
        if (target->open_count == 0) {
            if (target->ops && target->ops->release)
                target->ops->release(target);
            vnode_free(target);
        }
    }
    if (n->ops->rename && n->ops->rename(n->parent, n, newdir, last, len) != 0) { rc = -EIO; goto out; }
    detach(n);
    memset(n->name, 0, sizeof n->name);
    memcpy(n->name, last, len);
    n->parent = newdir;
    n->sibling = newdir->children;
    newdir->children = n;
out:
    spin_unlock(&vfs_lock);
    return rc;
}

int vfs_path_of(struct vnode *n, char *buf, size_t len)
{
    char tmp[VFS_PATH_MAX];
    size_t pos = sizeof(tmp) - 1;
    tmp[pos] = '\0';
    if (n == root) {
        if (len < 2) return -1;
        buf[0] = '/'; buf[1] = '\0';
        return 0;
    }
    for (;;) {
        if (n->mountpoint)
            n = n->mountpoint;
        if (!n->parent)
            break;
        size_t l = strlen(n->name);
        if (pos < l + 1)
            return -1;
        pos -= l;
        memcpy(tmp + pos, n->name, l);
        tmp[--pos] = '/';
        n = n->parent;
    }
    if (strlen(tmp + pos) + 1 > len)
        return -1;
    memcpy(buf, tmp + pos, strlen(tmp + pos) + 1);
    return 0;
}

/* ---- public: files ------------------------------------------------------------------ */

struct file *vfs_open(struct vnode *cwd, const char *path, int flags)
{
    spin_lock(&vfs_lock);
    struct vnode *n = lookup_locked(cwd, path);
    if (!n && (flags & O_CREAT))
        n = create_locked(cwd, path, VNODE_FILE);
    if (n && n->type == VNODE_FILE && (flags & O_TRUNC) && n->ops->truncate) {
        n->ops->truncate(n, 0);
        vfs_touch(n);
    }
    if (n)
        __atomic_add_fetch(&n->open_count, 1, __ATOMIC_SEQ_CST);
    spin_unlock(&vfs_lock);
    if (!n)
        return NULL;

    struct file *f = kzalloc(sizeof(*f));
    if (!f) {
        spin_lock(&vfs_lock);
        if (__atomic_sub_fetch(&n->open_count, 1, __ATOMIC_SEQ_CST) == 0 && n->unlinked) {
            if (n->ops && n->ops->release)
                n->ops->release(n);
            vnode_free(n);
        }
        spin_unlock(&vfs_lock);
        return NULL;
    }
    f->node = n;
    f->flags = flags;
    f->refs = 1;
    if ((flags & O_APPEND) && n->type == VNODE_FILE)
        f->pos = n->size;
    if (n->type == VNODE_DEV && n->dev && n->dev->open && n->dev->open(f) < 0) {
        spin_lock(&vfs_lock);
        if (__atomic_sub_fetch(&n->open_count, 1, __ATOMIC_SEQ_CST) == 0 && n->unlinked) {
            if (n->ops && n->ops->release)
                n->ops->release(n);
            vnode_free(n);
        }
        spin_unlock(&vfs_lock);
        kfree(f);                       /* the device said no (busy) */
        return NULL;
    }
    return f;
}

struct file *file_dup(struct file *f)
{
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
    return f;
}

/* flock(2) state, for all files */
spinlock_t flock_lock = SPINLOCK_INIT;

void flock_drop(struct vnode *n, struct file *f)
{
    spin_lock(&flock_lock);
    if (f->flock_mode == 2 && n->flock_ex == f) n->flock_ex = NULL;
    else if (f->flock_mode == 1 && n->flock_sh) n->flock_sh--;
    f->flock_mode = 0;
    spin_unlock(&flock_lock);
}

void file_close(struct file *f)
{
    if (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        struct vnode *n = f->node;
        if (n && n->lock_owner == f) n->lock_owner = NULL;   /* a record lock dies with its file */
        if (n && f->flock_mode) flock_drop(n, f);           /* and so does a flock(2) one */
        if (f->fops && f->fops->release) {
            f->fops->release(f);
        } else {
            if (n && n->type == VNODE_DEV && n->dev && n->dev->release)
                n->dev->release(f);
            if (n) {
                spin_lock(&vfs_lock);
                if (n->type != VNODE_DEV && n->ops && n->ops->sync)
                    n->ops->sync(n);
                if (__atomic_sub_fetch(&n->open_count, 1, __ATOMIC_SEQ_CST) == 0 && n->unlinked) {
                    if (n->ops && n->ops->release)
                        n->ops->release(n);
                    vnode_free(n);
                }
                spin_unlock(&vfs_lock);
            }
        }
        kfree(f);
    }
}

long file_read(struct file *f, void *buf, size_t len)
{
    struct vnode *n = f->node;
    if (n->type == VNODE_DEV)
        return n->dev && n->dev->read ? n->dev->read(f, buf, len) : -EINVAL;
    if (n->type == VNODE_DIR)
        return -EISDIR;
    if (n->type != VNODE_FILE || (f->flags & 3) == O_WRONLY || !n->ops->read)
        return -EBADF;

    spin_lock(&vfs_lock);
    long r = n->ops->read(n, f->pos, buf, len);
    if (r > 0)
        f->pos += (uint64_t)r;
    spin_unlock(&vfs_lock);
    return r;
}

long file_write(struct file *f, const void *buf, size_t len)
{
    struct vnode *n = f->node;
    if (n->type == VNODE_DEV)
        return n->dev && n->dev->write ? n->dev->write(f, buf, len) : -EINVAL;
    if (n->type == VNODE_DIR)
        return -EISDIR;
    if (n->type != VNODE_FILE || (f->flags & 3) == O_RDONLY || !n->ops->write)
        return -EBADF;

    spin_lock(&vfs_lock);
    if (f->flags & O_APPEND)
        f->pos = n->size;
    long r = n->ops->write(n, f->pos, buf, len);
    if (r > 0) {
        f->pos += (uint64_t)r;
        vfs_touch(n);
    }
    spin_unlock(&vfs_lock);
    return r;
}

long file_pread(struct file *f, void *buf, size_t len, uint64_t offset)
{
    if (!f) return -EBADF;
    struct vnode *n = f->node;
    if (!n) return -EBADF;
    if (n->type == VNODE_DIR)
        return -EISDIR;
    struct blkdev *b = blkdev_from_vnode(n);
    if (b) {
        if ((f->flags & 3) == O_WRONLY)
            return -EBADF;
        return blkdev_read_bytes(b, offset, buf, len);
    }
    if (n->type == VNODE_DEV)
        return -ESPIPE;
    if (n->type != VNODE_FILE || (f->flags & 3) == O_WRONLY || !n->ops || !n->ops->read)
        return -EBADF;

    spin_lock(&vfs_lock);
    long r = n->ops->read(n, offset, buf, len);
    spin_unlock(&vfs_lock);
    return r;
}

long file_pwrite(struct file *f, const void *buf, size_t len, uint64_t offset)
{
    if (!f) return -EBADF;
    struct vnode *n = f->node;
    if (!n) return -EBADF;
    if (n->type == VNODE_DIR)
        return -EISDIR;
    struct blkdev *b = blkdev_from_vnode(n);
    if (b) {
        if ((f->flags & 3) == O_RDONLY)
            return -EBADF;
        return blkdev_write_bytes(b, offset, buf, len);
    }
    if (n->type == VNODE_DEV)
        return -ESPIPE;
    if (n->type != VNODE_FILE || (f->flags & 3) == O_RDONLY || !n->ops || !n->ops->write)
        return -EBADF;

    spin_lock(&vfs_lock);
    long r = n->ops->write(n, offset, buf, len);
    if (r > 0)
        vfs_touch(n);
    spin_unlock(&vfs_lock);
    return r;
}

long file_seek(struct file *f, long off, int whence)
{
    if (f->node->type != VNODE_FILE && f->node->type != VNODE_DEV && f->node->type != VNODE_DIR)
        return -1;
    long base = whence == 0 ? 0 : whence == 1 ? (long)f->pos : (long)f->node->size;
    if (base + off < 0)
        return -1;
    f->pos = (uint64_t)(base + off);
    return (long)f->pos;
}

/* Readiness for poll(). Plain files and directories never block; devices
 * with a dev_ops.poll hook report their own state; everything else is
 * always readable and writable. */
int file_poll(struct file *f, struct waitqueue **wq)
{
    *wq = NULL;
    if (f->fops && f->fops->poll)
        return f->fops->poll(f, wq);
    struct vnode *n = f->node;
    if (n->type == VNODE_DEV && n->dev && n->dev->poll)
        return n->dev->poll(f, wq);
    return POLLIN | POLLOUT;
}

/* A disk filesystem's inode and block cache are shared by every CPU: what
 * reaches them goes through vfs_lock, these two included. */
int vfs_sync(struct vnode *n)
{
    if (!n->ops || !n->ops->sync) return 0;
    spin_lock(&vfs_lock);
    int r = n->ops->sync(n);
    spin_unlock(&vfs_lock);
    return r;
}

int vfs_truncate(struct vnode *n, uint64_t len)
{
    if (!n->ops || !n->ops->truncate) return -1;
    spin_lock(&vfs_lock);
    int r = n->ops->truncate(n, len);
    spin_unlock(&vfs_lock);
    return r;
}

int file_readdir(struct file *f, uint64_t *pos, struct dirent *out)
{
    struct vnode *d = f->node;
    if (d->type != VNODE_DIR || !d->ops->readdir)
        return -1;
    spin_lock(&vfs_lock);
    int r = d->ops->readdir(d, pos, out);
    spin_unlock(&vfs_lock);
    return r;
}

void vnode_stat(struct vnode *n, struct stat *st)
{
    st->type = n->type;
    st->ino = (uint32_t)n->ino;
    st->size = n->size;
}

uint16_t vfs_nlink(struct vnode *n)
{
    if (!n) return 0;
    if (n->type == VNODE_DIR) return 2;
    if (n->type == VNODE_DEV) return 1;
    if (n->mnt && n->mnt->type) {
        if (strcmp(n->mnt->type->name, "zaefs") == 0 && n->priv) {
            struct { uint32_t ino; struct zaefs_inode di; int dirty; } *zi = n->priv;
            return zi->di.links ? zi->di.links : 1;
        }
        if (strcmp(n->mnt->type->name, "tmpfs") == 0 && n->priv) {
            struct { uint8_t *data; size_t cap; int borrowed; int links; } *tf = n->priv;
            return tf->links > 0 ? (uint16_t)tf->links : 1;
        }
    }
    return 1;
}

void *vfs_read_all(struct vnode *n, size_t *size)
{
    if (n->type != VNODE_FILE)
        return NULL;
    spin_lock(&vfs_lock);
    uint64_t sz = n->size;
    void *buf = kmalloc(sz ? sz : 1);
    long r = buf ? n->ops->read(n, 0, buf, sz) : -1;
    spin_unlock(&vfs_lock);
    if (r < 0 || (uint64_t)r != sz) {
        kfree(buf);
        return NULL;
    }
    *size = sz;
    return buf;
}

long vfs_pread(struct vnode *n, void *buf, size_t len, uint64_t offset)
{
    if (!n || n->type != VNODE_FILE || !n->ops || !n->ops->read)
        return -1;
    spin_lock(&vfs_lock);
    long r = n->ops->read(n, offset, buf, len);
    spin_unlock(&vfs_lock);
    return r;
}

