/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* tmpfs: files live in kmalloc'd buffers, the directory tree is the vnode
 * cache itself. Also home of the USTAR initrd unpacker. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "string.h"
#include "printf.h"

struct tmpfs_file {
    uint8_t *data;
    size_t   cap;
};

static uint64_t next_ino = 1;

static struct vnode *tmpfs_create(struct vnode *dir, const char *name, size_t len, enum vnode_type type)
{
    struct vnode *n = vnode_alloc(name, len, type, dir);
    if (!n)
        return NULL;
    n->ino = next_ino++;
    if (type == VNODE_FILE) {
        n->priv = kzalloc(sizeof(struct tmpfs_file));
        if (!n->priv) {
            vnode_free(n);
            return NULL;
        }
    }
    return n;
}

static int tmpfs_unlink(struct vnode *dir, struct vnode *n)
{
    (void)dir;
    if (n->type == VNODE_FILE && n->priv) {
        struct tmpfs_file *tf = n->priv;
        kfree(tf->data);
        kfree(tf);
    }
    return 0;
}

static long tmpfs_read(struct vnode *n, uint64_t pos, void *buf, size_t len)
{
    struct tmpfs_file *tf = n->priv;
    size_t avail = pos < n->size ? n->size - pos : 0;
    if (len > avail)
        len = avail;
    memcpy(buf, tf->data + pos, len);
    return (long)len;
}

static long tmpfs_write(struct vnode *n, uint64_t pos, const void *buf, size_t len)
{
    struct tmpfs_file *tf = n->priv;
    size_t end = pos + len;
    if (end > tf->cap) {
        size_t cap = tf->cap ? tf->cap : 64;
        while (cap < end)
            cap *= 2;
        uint8_t *d = krealloc(tf->data, cap);
        if (!d)
            return -1;
        tf->data = d;
        tf->cap = cap;
    }
    if (pos > n->size)
        memset(tf->data + n->size, 0, pos - n->size);
    memcpy(tf->data + pos, buf, len);
    if (end > n->size)
        n->size = end;
    return (long)len;
}

static int tmpfs_truncate(struct vnode *n, uint64_t size)
{
    struct tmpfs_file *tf = n->priv;
    if (size > n->size) {
        uint8_t z = 0;
        return tmpfs_write(n, size - 1, &z, 1) == 1 ? 0 : -1;
    }
    n->size = size;
    (void)tf;
    return 0;
}

static int tmpfs_readdir(struct vnode *dir, size_t index, struct dirent *out)
{
    struct vnode *c = dir->children;
    while (c && index--)
        c = c->sibling;
    if (!c)
        return 0;
    memcpy(out->name, c->name, VFS_NAME_MAX);
    out->type = c->type;
    out->ino = (uint32_t)c->ino;
    out->size = c->size;
    return 1;
}

static const struct vnode_ops tmpfs_ops = {
    .create   = tmpfs_create,
    .unlink   = tmpfs_unlink,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .truncate = tmpfs_truncate,
    .readdir  = tmpfs_readdir,
};

static struct vnode *tmpfs_mount(struct vfs_mount *mnt, struct vnode *dev, const char *opts)
{
    (void)dev; (void)opts;
    struct vnode *r = vnode_alloc("", 0, VNODE_DIR, NULL);
    if (!r)
        return NULL;
    r->mnt = mnt;
    r->ops = &tmpfs_ops;
    r->ino = next_ino++;
    return r;
}

struct fs_type tmpfs_type = { .name = "tmpfs", .mount = tmpfs_mount };

/* ---- USTAR initrd ------------------------------------------------------------- */

struct tar_header {
    char name[100];
    char mode[8], uid[8], gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32], gname[32];
    char devmajor[8], devminor[8];
    char prefix[155];
    char pad[12];
};

static size_t tar_octal(const char *s, size_t n)
{
    size_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (size_t)(s[i] - '0');
    return v;
}

void vfs_load_tar(const void *tar, size_t size)
{
    const uint8_t *p = tar, *end = p + size;
    int files = 0, dirs = 0;

    while (p + 512 <= end) {
        const struct tar_header *h = (const void *)p;
        if (h->name[0] == '\0' || memcmp(h->magic, "ustar", 5) != 0)
            break;

        char path[VFS_PATH_MAX];
        size_t plen = 0;
        if (h->prefix[0]) {
            plen = strlen(h->prefix) < 155 ? strlen(h->prefix) : 155;
            memcpy(path, h->prefix, plen);
            path[plen++] = '/';
        }
        size_t nlen = strlen(h->name) < 100 ? strlen(h->name) : 100;
        memcpy(path + plen, h->name, nlen);
        plen += nlen;
        path[plen] = '\0';
        while (plen && path[plen - 1] == '/')
            path[--plen] = '\0';

        size_t fsize = tar_octal(h->size, sizeof(h->size));
        const uint8_t *data = p + 512;
        p = data + ((fsize + 511) & ~511UL);

        /* Skip "." and make every parent directory exist. */
        const char *rel = path;
        while (rel[0] == '.' && rel[1] == '/') rel += 2;
        while (*rel == '/') rel++;
        if (!*rel || (rel[0] == '.' && !rel[1]))
            continue;
        char tmp[VFS_PATH_MAX];
        memcpy(tmp, rel, strlen(rel) + 1);
        for (char *q = tmp + 1; *q; q++)
            if (*q == '/') {
                *q = '\0';
                vfs_create(vfs_root(), tmp, VNODE_DIR);
                *q = '/';
            }

        if (h->typeflag == '5') {
            vfs_create(vfs_root(), tmp, VNODE_DIR);
            dirs++;
        } else if (h->typeflag == '0' || h->typeflag == '\0') {
            struct vnode *n = vfs_create(vfs_root(), tmp, VNODE_FILE);
            if (n && fsize)
                tmpfs_write(n, 0, data, fsize);
            files++;
        }
    }
    kprintf("vfs: initrd unpacked: %d files, %d dirs\n", files, dirs);
}
