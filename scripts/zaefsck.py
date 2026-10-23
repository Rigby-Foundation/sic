#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""zaefsck: consistency check of a zaefs image (sic/include/abi/zaefs.h).
Walks the tree from the root; reports block pointers outside the data area,
blocks owned twice, unreadable directories, and bitmaps that disagree with
the tree. With --fix, rewrites both bitmaps and the superblock's free
counts from the tree (in-use blocks marked free get marked used, leaked
ones freed); file contents are not touched.

    zaefsck.py [--fix] <image>"""
import struct, sys
from collections import defaultdict
BS = 4096
fix = '--fix' in sys.argv
args = [a for a in sys.argv[1:] if a != '--fix']
img = open(args[0], 'r+b' if fix else 'rb')
def blk(n): img.seek(n * BS); return img.read(BS)
sb = blk(0)
magic, ver, bsz, total, icount, bbs, bbn, ibs, ibn, its, itn, data_start = struct.unpack_from('<QIIQQQQQQQQQ', sb, 0)
assert magic == 0x0100736665617A, 'not zaefs'
free_blocks, free_inodes, root = struct.unpack_from('<QQQ', sb, 8 + 4 + 4 + 8 * 9)
print(f'blocks {total} data_start {data_start} inodes {icount} root {root}')
def inode(i):
    b, off = divmod(i * 128, BS)
    d = blk(its + b)[off:off + 128]
    t, links, mode, size, ct, mt, nb = struct.unpack_from('<HHIQQQI', d, 0)
    ptrs = struct.unpack_from('<14I', d, 36)
    return t, links, size, nb, ptrs[:12], ptrs[12], ptrs[13]
owner = {}
problems = []
def bad(p): return p != 0 and not (data_start <= p < total)
def blocks_of(path, ptrs, ind, dind):
    out = []
    for p in ptrs: out.append(p)
    def idx(b, depth):
        if b == 0: return
        if bad(b): problems.append(f'{path}: index block {b:#x} out of range'); return
        claim(path, b, 'index')
        for q in struct.unpack('<1024I', blk(b)):
            if depth: idx(q, depth - 1)
            else: out.append(q)
    idx(ind, 0); idx(dind, 1)
    return out
def claim(path, b, what):
    if b in owner and owner[b] != path: problems.append(f'{path}: {what} block {b} also owned by {owner[b]}')
    owner[b] = path
seen = set()
def walk(ino, path):
    if ino in seen: return
    seen.add(ino)
    t, links, size, nb, ptrs, ind, dind = inode(ino)
    bl = blocks_of(path, ptrs, ind, dind)
    for i, b in enumerate(bl):
        if b == 0: continue
        if bad(b): problems.append(f'{path}: data block #{i} = {b:#x} out of range (size {size})'); continue
        claim(path, b, 'data')
    nused = (size + BS - 1) // BS
    if t == 2:
        for i in range(min(nused, len(bl))):
            b = bl[i]
            if b == 0 or bad(b): continue
            d = blk(b); off = 0
            while off + 8 <= BS:
                cino, rec, nl, ct = struct.unpack_from('<IHBB', d, off)
                if rec < 8 or off + rec > BS: problems.append(f'{path}: dirent chain broken in block {b}'); break
                if cino:
                    name = d[off + 8:off + 8 + nl].decode(errors='replace')
                    if cino >= icount: problems.append(f'{path}/{name}: inode {cino} out of range')
                    else: walk(cino, path.rstrip('/') + '/' + name)
                off += rec
walk(root, '/')
print(f'{len(seen)} inodes walked, {len(owner)} blocks owned, {len(problems)} problems')
for p in problems[:200]: print(' ', p)

files = sorted({p.split(': ')[0] for p in problems} | {p.split(' also owned by ')[1] for p in problems if ' also owned by ' in p})
if files:
    print(f'{len(files)} files with damaged blocks:')
    for f in files: print('  ', f)

def bitmap(start, n): return bytearray(b''.join(blk(start + i) for i in range(n)))
def isset(bm, i): return bm[i >> 3] >> (i & 7) & 1
bb, ib = bitmap(bbs, bbn), bitmap(ibs, ibn)
want_b = set(range(data_start)) | {b for b in owner if data_start <= b < total}
want_i = {0} | seen
used_free = sum(1 for b in want_b if not isset(bb, b))
leaked = sum(1 for b in range(total) if isset(bb, b) and b not in want_b)
ino_free = sum(1 for i in want_i if not isset(ib, i))
ino_leak = sum(1 for i in range(icount) if isset(ib, i) and i not in want_i)
print(f'block bitmap: {used_free} in-use blocks marked free, {leaked} leaked; '
      f'inode bitmap: {ino_free} in-use inodes marked free, {ino_leak} leaked')
print(f'superblock free blocks {free_blocks} (tree says {total - len(want_b)}), free inodes {free_inodes} (tree says {icount - len(want_i)})')
if fix:
    nb, ni = bytearray(len(bb)), bytearray(len(ib))
    for b in want_b: nb[b >> 3] |= 1 << (b & 7)
    for i in want_i: ni[i >> 3] |= 1 << (i & 7)
    img.seek(bbs * BS); img.write(nb)
    img.seek(ibs * BS); img.write(ni)
    sbw = bytearray(sb)
    struct.pack_into('<QQ', sbw, 88, total - len(want_b), icount - len(want_i))
    img.seek(0); img.write(sbw)
    img.flush()
    print('fixed: bitmaps and free counts rewritten from the tree')
