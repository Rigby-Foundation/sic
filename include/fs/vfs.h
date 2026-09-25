/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#pragma once
#include "types.h"

#define VFS_NAME_MAX 64
#define VFS_PATH_MAX 256
#define MAX_FDS      64

enum vnode_type { VNODE_FILE = 1, VNODE_DIR = 2, VNODE_DEV = 3 };

struct vnode;
struct file;
struct vfs_mount;
struct dirent;
struct waitqueue;

/* Character/block device hooks (VNODE_DEV). */
struct dev_ops {
    long (*read)(struct file *f, void *buf, size_t len);
    long (*write)(struct file *f, const void *buf, size_t len);
    int  (*poll)(struct file *f, struct waitqueue **wq);   /* optional, see file_ops */
    long (*ioctl)(struct file *f, long req, uint64_t arg);  /* optional */
    int  (*mmap)(struct file *f, uint64_t virt, size_t pages, uint64_t off, int prot); /* optional */
    void (*release)(struct file *f);   /* optional: last reference to an open file dropped */
    long (*open)(struct file *f);      /* optional: a new open file; < 0 (-errno) refuses it */
};

/* Per-filesystem operations. Directory entries are cached as child vnodes
 * (children/sibling); `lookup` is only asked for names not in the cache. */
struct vnode_ops {
    struct vnode *(*lookup)(struct vnode *dir, const char *name, size_t len);
    struct vnode *(*create)(struct vnode *dir, const char *name, size_t len, enum vnode_type type);
    int  (*unlink)(struct vnode *dir, struct vnode *n);           /* n has no children/open users */
    int  (*rename)(struct vnode *olddir, struct vnode *n, struct vnode *newdir, const char *name, size_t len);
                                    /* move the on-disk entry; optional (an in-memory fs needs none) */
    long (*read)(struct vnode *n, uint64_t pos, void *buf, size_t len);
    long (*write)(struct vnode *n, uint64_t pos, const void *buf, size_t len);
    int  (*truncate)(struct vnode *n, uint64_t size);
    int  (*readdir)(struct vnode *dir, size_t index, struct dirent *out);   /* 1 = entry, 0 = end */
    int  (*sync)(struct vnode *n);
};

struct vnode {
    char     name[VFS_NAME_MAX];
    enum vnode_type type;
    struct vnode *parent;
    struct vnode *children;         /* cached/authoritative directory entries */
    struct vnode *sibling;
    struct vfs_mount *mnt;          /* filesystem instance this node belongs to */
    const struct vnode_ops *ops;
    const struct dev_ops *dev;      /* devices */
    uint64_t ino;
    uint64_t size;
    void    *priv;                  /* filesystem private data (tmpfs buffer, on-disk inode, ...) */
    int      is_block;              /* VNODE_DEV backed by a block device */
    int      is_sock;               /* VNODE_DEV that is a socket (priv = struct socket) */
    int      seekable;              /* VNODE_DEV with a position (framebuffer, initrd) */
    struct vnode *mounted;          /* a filesystem's root mounted on this directory */
    struct file *lock_owner;        /* fcntl record lock: one whole-file lock per file, held by an open file */
    uint32_t lock_pid;
    struct vnode *mountpoint;       /* for a mounted root: the directory it sits on */
};

struct vfs_mount {
    const struct fs_type *type;
    struct vnode *root;
    void *priv;
};

/* A filesystem driver. `mount` gets the device vnode (may be NULL for
 * memory filesystems) and returns the root vnode, or NULL. */
struct fs_type {
    const char *name;
    struct vnode *(*mount)(struct vfs_mount *mnt, struct vnode *dev, const char *opts);
    int (*unmount)(struct vfs_mount *mnt);
    struct fs_type *next;
};

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

/* Per-file hooks for objects that aren't plain vnodes (pipes, sockets). */
struct file_ops {
    void (*release)(struct file *f);    /* last reference dropped */
    /* poll: return the POLL* bits that are ready now and, in *wq, the wait
     * queue that is woken when they change (NULL if the state never changes). */
    int  (*poll)(struct file *f, struct waitqueue **wq);
};

struct file {
    struct vnode *node;
    uint64_t pos;
    int      flags;
    int      refs;
    const struct file_ops *fops;
    void    *priv_gpu;              /* a device's per-open state (virtio-gpu contexts, shm segments) */
    uint32_t fb_gen;                /* /dev/fb0: the mode generation this file last read */
};

struct stat {
    uint32_t type;      /* enum vnode_type */
    uint32_t ino;
    uint64_t size;
};

struct dirent {
    char     name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t ino;
    uint64_t size;
};

void          vfs_init(void);
struct vnode *vfs_root(void);

/* Filesystem drivers and mounting. */
void          vfs_register_fs(struct fs_type *fs);
int           vfs_mount(const char *fstype, struct vnode *dev, struct vnode *dir, const char *opts);
int           vfs_umount(struct vnode *dir);

/* Helpers for filesystem drivers. */
struct vnode *vnode_alloc(const char *name, size_t namelen, enum vnode_type type, struct vnode *parent);
void          vnode_free(struct vnode *n);

/* Path operations, relative to `cwd` when the path isn't absolute. */
struct vnode *vfs_lookup(struct vnode *cwd, const char *path);
struct vnode *vfs_create(struct vnode *cwd, const char *path, enum vnode_type type);
int           vfs_mkdev(const char *path, const struct dev_ops *ops, void *priv);
int           vfs_unlink(struct vnode *cwd, const char *path);
int           vfs_rename(struct vnode *cwd, const char *oldpath, struct vnode *newcwd, const char *newpath);   /* -errno */
int           vfs_path_of(struct vnode *n, char *buf, size_t len);
void          vfs_load_tar(const void *tar, size_t size);

/* File objects (refcounted, shared across fork). */
struct file  *vfs_open(struct vnode *cwd, const char *path, int flags);
struct file  *file_dup(struct file *f);
void          file_close(struct file *f);
long          file_read(struct file *f, void *buf, size_t len);
long          file_write(struct file *f, const void *buf, size_t len);
long          file_seek(struct file *f, long off, int whence);
int           file_poll(struct file *f, struct waitqueue **wq);   /* POLL* bits ready now */
int           file_readdir(struct file *f, size_t index, struct dirent *out);
void          vnode_stat(struct vnode *n, struct stat *st);

/* Whole-file read into a fresh kmalloc buffer (for exec). */
void         *vfs_read_all(struct vnode *n, size_t *size);
