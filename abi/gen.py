#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Generate syscall number headers from abi/syscall.tbl.

  gen.py kernel  > sic/include/abi/syscall_nr.h     (#define SYS_read 64)
  gen.py musl    > arch/x86_64/bits/syscall.h.in    (#define __NR_read 64)
"""
import sys, os

def load():
    here = os.path.dirname(os.path.abspath(__file__))
    rows = []
    for line in open(os.path.join(here, "syscall.tbl")):
        line = line.split("#", 1)[0].strip()
        if line:
            num, name = line.split()
            rows.append((int(num), name))
    return rows

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "kernel"
    rows = load()
    if mode == "kernel":
        print("/* SPDX-License-Identifier: GPL-2.0-only WITH sic-syscall-note */")
        print("/* Copyright (C) 2026 Rigby Foundation */")
        print("/* Generated from abi/syscall.tbl by abi/gen.py. Do not edit. */")
        print("#pragma once")
        print("#define SYS_sic_max %d" % max(n for n, _ in rows))
        for n, name in rows:
            print("#define SYS_%s %d" % (name, n))
    elif mode == "musl":
        print("/* SPDX-License-Identifier: LGPL-2.1-or-later */")
        print("/* Copyright (C) 2026 Rigby Foundation. Generated from sic/abi/syscall.tbl. */")
        for n, name in rows:
            print("#define __NR_%s\t%d" % (name, n))
    else:
        sys.exit("usage: gen.py kernel|musl")

if __name__ == "__main__":
    main()
