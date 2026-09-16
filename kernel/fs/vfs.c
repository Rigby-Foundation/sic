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
    if (parent) {
        n->mnt = parent->mnt;
        n->ops = parent->ops;
        n->sibling = parent->children;
        parent->children = n;
    }
    return n;
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

/* Walk all components but the last; returns the parent dir and points
 * last/lastlen at the final component (may be empty for "/"). */
static struct vnode *walk_parent(struct vnode *cwd, const char *path, const char **last, size_t *lastlen)
{
    struct vnode *dir = (*path == '/') ? root : (cwd ? cwd : root);
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
        dir = dir_find(dir, seg, len);
        if (!dir)
            return NULL;
    }
}

static struct vnode *lookup_locked(struct vnode *cwd, const char *path)
{
    const char *last;
    size_t len;
    struct vnode *dir = walk_parent(cwd, path, &last, &len);
    if (!dir)
        return NULL;
    if (len == 0)
        return dir;
    if (dir->type != VNODE_DIR)
        return NULL;
    return dir_find(dir, last, len);
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
    root_mount.type = &tmpfs_type;
    root = tmpfs_type.mount(&root_mount, NULL, NULL);
    root_mount.root = root;
    kprintf("vfs: tmpfs root mounted\n");
}

struct vnode *vfs_root(void) { return root; }

int vfs_mount(const char *fstype, struct vnode *dev, struct vnode *dir, const char *opts)
{
    struct fs_type *fs = find_fs(fstype);
    if (!fs || !dir || dir->type != VNODE_DIR)
        return -1;
    if (dir->mounted || dir->mountpoint || dir == root)
        return -2;                          /* already a mount point / a mounted root */

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
    struct vnode *n = lookup_locked(cwd, path);
    int rc = -1;
    if (n && n->parent && !n->mountpoint && !n->mounted &&
        (n->type != VNODE_DIR || !n->children) &&
        (n->type != VNODE_DIR || !n->ops->readdir || n->ops->readdir(n, 0, &(struct dirent){0}) == 0)) {
        rc = n->ops && n->ops->unlink ? n->ops->unlink(n->parent, n) : 0;
        if (rc == 0) {
            detach(n);
            vnode_free(n);      /* leaks the node if still open; fine for now */
        }
    }
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
    if (n && n->type == VNODE_FILE && (flags & O_TRUNC) && n->ops->truncate)
        n->ops->truncate(n, 0);
    spin_unlock(&vfs_lock);
    if (!n)
        return NULL;

    struct file *f = kzalloc(sizeof(*f));
    if (!f)
        return NULL;
    f->node = n;
    f->flags = flags;
    f->refs = 1;
    if ((flags & O_APPEND) && n->type == VNODE_FILE)
        f->pos = n->size;
    return f;
}

struct file *file_dup(struct file *f)
{
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
    return f;
}

void file_close(struct file *f)
{
    if (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        if (f->fops && f->fops->release)
            f->fops->release(f);
        else if (f->node->type == VNODE_DEV && f->node->dev && f->node->dev->release)
            f->node->dev->release(f);
        else if (f->node->ops && f->node->ops->sync)
            f->node->ops->sync(f->node);
        kfree(f);
    }
}

long file_read(struct file *f, void *buf, size_t len)
{
    struct vnode *n = f->node;
    if (n->type == VNODE_DEV)
        return n->dev && n->dev->read ? n->dev->read(f, buf, len) : -1;
    if (n->type != VNODE_FILE || (f->flags & 3) == O_WRONLY || !n->ops->read)
        return -1;

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
        return n->dev && n->dev->write ? n->dev->write(f, buf, len) : -1;
    if (n->type != VNODE_FILE || (f->flags & 3) == O_RDONLY || !n->ops->write)
        return -1;

    spin_lock(&vfs_lock);
    if (f->flags & O_APPEND)
        f->pos = n->size;
    long r = n->ops->write(n, f->pos, buf, len);
    if (r > 0)
        f->pos += (uint64_t)r;
    spin_unlock(&vfs_lock);
    return r;
}

long file_seek(struct file *f, long off, int whence)
{
    if (f->node->type != VNODE_FILE && f->node->type != VNODE_DEV)
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

int file_readdir(struct file *f, size_t index, struct dirent *out)
{
    struct vnode *d = f->node;
    if (d->type != VNODE_DIR || !d->ops->readdir)
        return -1;
    spin_lock(&vfs_lock);
    int r = d->ops->readdir(d, index, out);
    spin_unlock(&vfs_lock);
    return r;
}

void vnode_stat(struct vnode *n, struct stat *st)
{
    st->type = n->type;
    st->ino = (uint32_t)n->ino;
    st->size = n->size;
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
