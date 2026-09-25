#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""mkzaefs.py: build a zaefs disk image from a directory, on the host.

    mkzaefs.py [-L label] [-s size] [-a name=file]... [-x name]... <directory> <image>

The image gets the whole tree (regular files and directories; symlinks are
followed; executable files stay executable), plus each `-a` file at the root
under `name`, minus the top-level entries named with `-x`. Its size is `-s`
(e.g. 512M) or the tree plus a quarter, rounded up to a megabyte. Same
layout as ZAE's mkfs.zaefs (include/abi/zaefs.h)."""
import os, struct, sys, time

BS = 4096
INODE_SIZE = 128
NDIRECT = 12
PTRS = BS // 4
MAGIC = 0x0100736665617A
SB_FMT = "<QIIQQQQQQQQQQQQ32s"      # magic version block_size total inodes bb_start bb_blocks ib_start ib_blocks it_start it_blocks data_start free_blocks free_inodes root label
INODE_FMT = "<HHIQQQI" + "I" * NDIRECT + "II"

def parse_size(s):
    m = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    return int(s[:-1]) * m[s[-1].upper()] if s[-1].upper() in m else int(s)

class Image:
    def __init__(self, path, total_blocks, label):
        self.f = open(path, "wb")
        self.total = total_blocks
        self.inode_count = max(64, total_blocks // 4)
        self.bb_start = 1
        self.bb_blocks = (total_blocks + BS * 8 - 1) // (BS * 8)
        self.ib_start = self.bb_start + self.bb_blocks
        self.ib_blocks = (self.inode_count + BS * 8 - 1) // (BS * 8)
        self.it_start = self.ib_start + self.ib_blocks
        self.it_blocks = (self.inode_count * INODE_SIZE + BS - 1) // BS
        self.data_start = self.it_start + self.it_blocks
        self.label = label
        self.bbitmap = bytearray(self.bb_blocks * BS)
        self.ibitmap = bytearray(self.ib_blocks * BS)
        self.inodes = {}                # ino -> (type, size, blocks list) filled at the end
        self.next_block = self.data_start
        self.next_ino = 2               # 0 unused, 1 root
        for b in range(self.data_start): self.set_bit(self.bbitmap, b)
        self.set_bit(self.ibitmap, 0); self.set_bit(self.ibitmap, 1)
        self.f.truncate(total_blocks * BS)

    @staticmethod
    def set_bit(bm, i): bm[i // 8] |= 1 << (i % 8)

    def alloc_block(self):
        if self.next_block >= self.total: sys.exit("mkzaefs: image full; use -s for a bigger one")
        b = self.next_block; self.next_block += 1
        self.set_bit(self.bbitmap, b)
        return b

    def put(self, blk, data):
        self.f.seek(blk * BS); self.f.write(data)

    def alloc_ino(self):
        if self.next_ino >= self.inode_count: sys.exit("mkzaefs: out of inodes")
        i = self.next_ino; self.next_ino += 1
        self.set_bit(self.ibitmap, i)
        return i

    def write_data(self, ino, typ, data, exec_=False):
        """Store `data` as the contents of inode `ino`: direct, indirect, double-indirect pointers."""
        nblk = (len(data) + BS - 1) // BS
        blocks = []
        for i in range(nblk):
            b = self.alloc_block(); blocks.append(b)
            self.put(b, data[i * BS:(i + 1) * BS])
        direct = blocks[:NDIRECT] + [0] * (NDIRECT - min(len(blocks), NDIRECT))
        rest = blocks[NDIRECT:]
        owned = nblk
        indirect = 0
        if rest[:PTRS]:
            indirect = self.alloc_block(); owned += 1
            self.put(indirect, struct.pack("<%dI" % PTRS, *(rest[:PTRS] + [0] * (PTRS - len(rest[:PTRS])))))
        rest = rest[PTRS:]
        dindirect = 0
        if rest:
            if len(rest) > PTRS * PTRS: sys.exit("mkzaefs: a file is too big for zaefs (max %d MB)" % ((NDIRECT + PTRS + PTRS * PTRS) * BS >> 20))
            dindirect = self.alloc_block(); owned += 1
            table = []
            for i in range(0, len(rest), PTRS):
                chunk = rest[i:i + PTRS]
                ib = self.alloc_block(); owned += 1
                self.put(ib, struct.pack("<%dI" % PTRS, *(chunk + [0] * (PTRS - len(chunk)))))
                table.append(ib)
            self.put(dindirect, struct.pack("<%dI" % PTRS, *(table + [0] * (PTRS - len(table)))))
        self.inodes[ino] = (typ, len(data), owned, direct, indirect, dindirect, exec_)

    def finish(self):
        now = int(time.time())
        table = bytearray(self.it_blocks * BS)
        for ino, (typ, size, owned, direct, indirect, dindirect, exec_) in self.inodes.items():
            mode = 0o755 if typ == 2 or exec_ else 0o644
            rec = struct.pack(INODE_FMT, typ, 1, mode, size, now, now, owned, *direct, indirect, dindirect)
            table[ino * INODE_SIZE:ino * INODE_SIZE + len(rec)] = rec
        self.put(self.it_start, bytes(table))
        for i in range(self.bb_blocks): self.put(self.bb_start + i, bytes(self.bbitmap[i * BS:(i + 1) * BS]))
        for i in range(self.ib_blocks): self.put(self.ib_start + i, bytes(self.ibitmap[i * BS:(i + 1) * BS]))
        sb = struct.pack(SB_FMT, MAGIC, 1, BS, self.total, self.inode_count, self.bb_start, self.bb_blocks, self.ib_start,
                         self.ib_blocks, self.it_start, self.it_blocks, self.data_start, self.total - self.next_block,
                         self.inode_count - self.next_ino, 1, self.label.encode()[:31])
        self.put(0, sb + bytes(BS - len(sb)))
        self.f.close()

def dirent(ino, name, typ):
    n = name.encode()
    rec = struct.pack("<IHBB", ino, 0, len(n), typ) + n
    pad = (-len(rec)) % 8
    return bytearray(rec + bytes(pad))

def build_dir(img, ino, path, extra={}, exclude=()):
    """Directory blocks: records padded to 8, the last one's rec_len runs to the block end."""
    entries = []
    names = {n: os.path.join(path, n) for n in os.listdir(path) if n not in exclude}
    names.update(extra)
    for name in sorted(names):
        if name.startswith(".DS_Store"): continue
        full = names[name]
        if len(name.encode()) > 255: sys.exit("mkzaefs: name too long: " + full)
        child = img.alloc_ino()
        if os.path.isdir(full):
            entries.append(dirent(child, name, 2)); build_dir(img, child, full)
        elif os.path.isfile(full):
            entries.append(dirent(child, name, 1))
            with open(full, "rb") as f: img.write_data(child, 1, f.read(), os.access(full, os.X_OK))
    blocks = bytearray()
    cur = bytearray()
    def flush():
        if cur:
            struct.pack_into("<H", cur, len(cur) - last_len + 4, BS - (len(cur) - last_len))    # rec_len of the last record runs to the block end
            blocks.extend(cur + bytes(BS - len(cur)))
    last_len = 0
    for e in entries:
        if len(cur) + len(e) > BS:
            flush(); cur = bytearray(); last_len = 0
        struct.pack_into("<H", e, 4, len(e))
        cur.extend(e); last_len = len(e)
    flush()
    img.write_data(ino, 2, bytes(blocks))

def tree_size(path):
    total = 0
    for root, dirs, files in os.walk(path):
        total += BS
        for f in files:
            try: total += (os.path.getsize(os.path.join(root, f)) + BS - 1) // BS * BS + BS // 64
            except OSError: pass
    return total

def main():
    args = sys.argv[1:]
    label, size, extra, exclude = "zaefs", None, {}, set()
    while args and args[0].startswith("-"):
        if args[0] == "-L": label = args[1]; args = args[2:]
        elif args[0] == "-s": size = parse_size(args[1]); args = args[2:]
        elif args[0] == "-a": name, _, file = args[1].partition("="); extra[name] = file; args = args[2:]
        elif args[0] == "-x": exclude.add(args[1]); args = args[2:]
        else: sys.exit(__doc__)
    if len(args) != 2: sys.exit(__doc__)
    src, out = args
    if size is None:
        size = (tree_size(src) + sum(os.path.getsize(f) for f in extra.values())) * 5 // 4 + 64 * BS
        size = (size + (1 << 20) - 1) & ~((1 << 20) - 1)
    img = Image(out, size // BS, label)
    build_dir(img, 1, src, extra, exclude)
    img.finish()
    print("mkzaefs: %s: %d MB, %d inodes used, label %r" % (out, size >> 20, img.next_ino - 2, label))

if __name__ == "__main__":
    main()
