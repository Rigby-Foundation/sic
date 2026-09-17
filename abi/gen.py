#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
"""Generate syscall number headers from abi/syscall.tbl.

  gen.py kernel  > sic/include/abi/syscall_nr.h     (#define SYS_read 64)
  gen.py musl    > arch/x86_64/bits/syscall.h.in    (#define __NR_read 64)
  gen.py musl32  > arch/powerpc/bits/syscall.h.in   (+ the *_time64 aliases)

sic has one time64-only ABI on every width: timespecs are 64-bit seconds
everywhere. musl on 32-bit targets only uses that layout for the calls whose
"*_time64" spelling exists, so the musl32 flavour defines those as aliases of
the plain numbers (musl's syscall.h then maps plain -> time64 itself).
"""

TIME64_ALIASES = {
    "clock_gettime": "clock_gettime64", "clock_settime": "clock_settime64",
    "clock_adjtime": "clock_adjtime64", "clock_getres": "clock_getres_time64",
    "clock_nanosleep": "clock_nanosleep_time64", "timer_gettime": "timer_gettime64",
    "timer_settime": "timer_settime64", "timerfd_gettime": "timerfd_gettime64",
    "timerfd_settime": "timerfd_settime64", "utimensat": "utimensat_time64",
    "pselect6": "pselect6_time64", "ppoll": "ppoll_time64", "recvmmsg": "recvmmsg_time64",
    "mq_timedsend": "mq_timedsend_time64", "mq_timedreceive": "mq_timedreceive_time64",
    "rt_sigtimedwait": "rt_sigtimedwait_time64", "futex": "futex_time64",
    "sched_rr_get_interval": "sched_rr_get_interval_time64",
    "semtimedop": "semtimedop_time64", "io_pgetevents": "io_pgetevents_time64",
}
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
    elif mode in ("musl", "musl32"):
        print("/* SPDX-License-Identifier: LGPL-2.1-or-later */")
        print("/* Copyright (C) 2026 Rigby Foundation. Generated from sic/abi/syscall.tbl. */")
        for n, name in rows:
            print("#define __NR_%s\t%d" % (name, n))
        if mode == "musl32":
            print("/* time64 spellings: the same calls, so musl uses 64-bit time everywhere */")
            for n, name in rows:
                if name in TIME64_ALIASES:
                    print("#define __NR_%s\t%d" % (TIME64_ALIASES[name], n))
    else:
        sys.exit("usage: gen.py kernel|musl|musl32")

if __name__ == "__main__":
    main()
