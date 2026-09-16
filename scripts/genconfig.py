#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Turn .config into a C header: CONFIG_FOO=y -> #define CONFIG_FOO 1 (n -> undefined)."""
import sys

def parse(path):
    opts = {}
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        k, _, v = line.partition("=")
        opts[k.strip()] = v.strip()
    return opts

def main():
    opts = parse(sys.argv[1])
    # command-line overrides: CONFIG_X=n given as extra args
    for a in sys.argv[2:]:
        k, _, v = a.partition("=")
        opts[k] = v
    print("/* Generated from .config by scripts/genconfig.py. Do not edit. */")
    print("#pragma once")
    for k in sorted(opts):
        if opts[k] == "y":
            print(f"#define {k} 1")
        else:
            print(f"/* {k} is not set */")

if __name__ == "__main__":
    main()
