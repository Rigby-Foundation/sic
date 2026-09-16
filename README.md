# sic

sic is a monolithic x86_64 kernel written in C: SMP, preemptive scheduling of
processes and threads, POSIX signals and pipes, a VFS with tmpfs, its own
on-disk filesystem (zaefs) and FAT, NVMe/AHCI/IDE disks with GPT and MBR
partitions, an IPv4 network stack (Ethernet, ARP, ICMP, UDP, TCP, BSD sockets,
Intel e1000 driver), loadable modules, and a framebuffer/serial console. It
runs on real hardware, booted by zaeboot from UEFI or legacy BIOS.

It is one of four projects that make up the system, each in its own repo:

| Repo | Role |
|------|------|
| **sic** (this one) | the kernel |
| [zaeboot](https://github.com/Rigby-Foundation/zaeboot) | bootloader (UEFI and legacy BIOS) that loads the kernel and the initrd |
| [musl](https://github.com/Rigby-Foundation/musl) | the C library: musl, ported to sic's system call ABI |
| [ZAE](https://github.com/Rigby-Foundation/zae) | userland: init, shell, tools, ports |

The contract between them is sic's system call ABI — deliberately its own
numbering, not Linux's: `abi/syscall.tbl` (generated into
`include/abi/syscall_nr.h` and musl's `bits/syscall.h`) and `include/abi/abi.h`
(structure layouts and constants). sic executables are ELF64 stamped with
OS/ABI byte `0x53`.

Source layout: `kernel/{arch/x86_64,mm,fs,drivers,proc,lib,core}` with matching
`include/` subdirectories, `modules/` for loadable modules, `include/abi/` for
the public ABI.

## Configuring

`configs/defconfig` lists every option (`CONFIG_<NAME>=y|n`): SMP, the
framebuffer/serial consoles and keyboard, PCI, the NVMe/AHCI (SATA)/legacy IDE
disk drivers, the network stack and the e1000 NIC driver, the zaefs and FAT
filesystems, loadable modules, signals, pipes, and the boot-time self tests.

```bash
make defconfig            # copy it to .config, then edit .config
make CONFIG_FAT=n         # or override on the command line
make config               # show the effective configuration
```

Options select which sources are compiled and are visible to the code as
`CONFIG_*` macros (`include/generated/config.h`). Turning a subsystem off
makes its system calls return `ENOSYS`.

## Building

Needs clang and `ld.lld` (on macOS: `brew install llvm lld`). The Makefile finds
them via `brew --prefix`; override with `make CC=... LD=...`.

```bash
make
```

Produces `build/sic.elf`, a static ELF64 linked at physical `0x100000`
(`linker.ld`) and the modules under `build/modules/`.

All four sic projects meet in a **sysroot** rather than knowing each other's
paths: `$SIC_SYSROOT`, default `~/.sic/sysroot` (or `make SYSROOT=...`).
`make install` puts the kernel there for the bootloader, plus what the other
projects build against:

```
$SYSROOT/boot/sic.elf               (zaeboot boots it)
$SYSROOT/usr/include/abi/*.h        public ABI headers  (libc, ZAE, tcc)
$SYSROOT/usr/share/sic/abi/         syscall.tbl + gen.py (libc's `make abi`)
$SYSROOT/lib/modules/*.ko           (ZAE packs them into the initrd)
```

Build order for a full system: sic → [libc](https://github.com/Rigby-Foundation/musl)
→ [ZAE](https://github.com/Rigby-Foundation/zae) (each `make install`), then
`zig build run` in [zaeboot](https://github.com/Rigby-Foundation/zaeboot).

## Contributing & Conduct

Before opening an issue, sending a patch, or proposing changes, read the
[CODE_OF_CONFLICT](./CODE_OF_CONFLICT) and [CONTRIBUTING](./CONTRIBUTING).

Development on this tree is technically uncompromising:
- Code quality and correctness override feelings.
- We do not break userspace. Ever.
- If your patch breaks the build, ABI, or core subsystems, expect direct,
  unfiltered criticism.

Emotional trauma reports and CoC inquiries can be directed to `nobody@nowhere.com`.

## License

Copyright (C) 2026 Rigby Foundation. This project is licensed under the terms of
the GNU General Public License v2 only ([GPL-2.0-only](./LICENSE)); every source
file carries an SPDX tag.

The headers under `include/abi/` additionally carry the `sic-syscall-note`
exception ([LICENSES/exceptions/sic-syscall-note](./LICENSES/exceptions/sic-syscall-note)):
using the kernel through system calls, including compiling against those
headers, does not make a program a derived work of the kernel. Loadable modules
are kernel code and are covered by the GPL like the rest of it.

No GPLv3 anti-tivoization restrictions, no corporate CLA. Read [LICENSE](./LICENSE)
for the full text.