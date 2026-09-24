# sic

sic is a monolithic kernel written in C for x86_64 and 32-bit big-endian
PowerPC (G4 PowerMacs): SMP, preemptive scheduling of
processes and threads, POSIX signals and pipes, sound (Intel HD Audio,
`/dev/dsp`), a VFS with tmpfs, its own
on-disk filesystem (zaefs) and FAT, NVMe/AHCI/IDE disks with GPT and MBR
partitions (`scripts/mkzaefs.py` builds a zaefs image on the host), an IPv4
network stack (Ethernet, ARP, ICMP, UDP, TCP, BSD sockets, AF_UNIX stream sockets,
Intel e1000 driver), loadable modules, and a framebuffer/serial console. On
x86_64 it runs on real hardware, booted by zaeboot from UEFI or legacy BIOS;
the PowerPC port is loaded by OpenFirmware and so far runs on QEMU's mac99.

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
(structure layouts and constants; a few layouts differ per architecture, as
they do in musl). sic executables are ELF stamped with OS/ABI byte `0x53`.
The ABI is "time64-only" on every width: a 32-bit port speaks 64-bit time to
the kernel in every call, so there are no `*_time64` or `_llseek` spellings
in the table (`abi/gen.py musl32` emits the aliases musl expects).

Source layout: `kernel/{arch/<arch>,mm,fs,drivers,proc,lib,core}` with matching
`include/` subdirectories, `modules/` for loadable modules, `include/abi/` for
the public ABI. Everything an architecture has to provide is declared in
`include/arch/<arch>/asm/*.h` (included as `"asm/x.h"`): interrupts and
exception frames, the MMU behind `mm/vmm.h`, task switching and user entry,
signal frames, timers, PCI access, the memory layout, and the module
loader's relocations (`module_arch.h`). The generic code is
width- and endian-clean: on-disk and on-wire structures go through
`include/endian.h`.

### Architectures

| `ARCH`    | Target                                       | Boots via                    |
|-----------|----------------------------------------------|------------------------------|
| `x86_64`  | any 64-bit PC (default)                       | zaeboot (UEFI, legacy BIOS)  |
| `powerpc` | 32-bit big-endian, G4 (7450); QEMU `-M mac99` | OpenFirmware `-kernel`       |
| `aarch64` | ARMv8-A, EL1; QEMU `-M virt`                  | a Linux-style Image, `-kernel` |

`make ARCH=powerpc` builds into `build/powerpc/` with
`configs/defconfig.powerpc`. The PowerPC kernel is linked at `0xC1000000`
(load address `0x01000000`), maps the kernel and PCI space with BATs, user
space with the hash page table from its own two-level page tables, takes
interrupts from the mac-io OpenPIC and time from the decrementer, talks on the
ESCC serial port (console input too), to PCI through Uni-North and to disks
on the mac-io ATA cells or NVMe. FP and AltiVec state are enabled lazily per
task and carried across switches, fork and signal frames. Modules are ELF32
objects with branch stubs for the ±32 MiB `bl` reach. Not yet: ADB/USB
input, SMP.

`make ARCH=aarch64` builds `build/aarch64/sic.elf` and `sic.img`, the latter
a Linux arm64 Image (the header is in `head.S`) so QEMU's `-kernel` loads it
with the initrd and hands over a flattened device tree in `x0`
(`kernel/arch/aarch64/fdt.c` reads it: memory, initrd, PL011, GIC, the PCI
host bridge's ECAM, windows and interrupt map). The kernel lives in a direct
map at `0xffff000000000000` (the first 4 GiB of physical space, 1 GiB
blocks, device attributes below RAM), user space in TTBR0 with 4 KiB pages,
takes interrupts from a GICv2, time from the generic timer, talks on the
PL011 (console input too) and to PCI through ECAM; with no firmware before
it, `drivers/pci.c` assigns the BARs itself. The SIMD/FP file is saved
around switches and in signal frames (the kernel is `-mgeneral-regs-only`).
Modules are ELF64 objects with `ldr/br` stubs for `bl` beyond 128 MiB. The
other CPUs come up through PSCI `CPU_ON` (`smp.c`; the GIC's banked part
and the timer are per CPU, a reschedule is an SGI, TLB invalidation is
broadcast so no shootdown IPI is needed). Input is virtio: `-device
virtio-keyboard-pci -device virtio-mouse-pci` feed the console and
`/dev/mouse` (`drivers/virtio_input.c`, Linux key codes are scancode set 1
for the main block), the display a `virtio-gpu-pci`. It runs unchanged under Apple's
hypervisor (`-accel hvf -cpu host`), which the top-level `run-arm64`
targets use on an Apple Silicon host. Not yet: USB, real boards.

## Configuring

`configs/defconfig` lists every option (`CONFIG_<NAME>=y|n`): SMP, the
framebuffer/serial consoles, keyboard and mouse (`/dev/mouse`), shared memory
between processes (`/dev/shmem`, `abi/shm.h`), PCI, the
virtio-gpu display (QEMU `-vga virtio`: a framebuffer in RAM pushed to the
host by a kernel thread and on `FBIOPRESENT`, following the host window's
size -- `/dev/fb0` reports the new mode and polls `POLLPRI`; with a virgl host,
`-device virtio-vga-gl`, also `/dev/gpu0`: contexts, resources and command
submission to the host GPU for [zgl](../zgl)), Intel HD Audio (`/dev/dsp`:
OSS-style playback, 48 kHz 16-bit stereo, `beep` to try it), the NVMe/AHCI (SATA)/legacy IDE
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

Produces `build/<arch>/sic.elf`, a static ELF (x86_64: linked at physical
`0x100000`; `kernel/arch/<arch>/linker.ld`) and the modules under
`build/<arch>/modules/`.

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

When a user process dies of a fault the kernel prints a post-mortem on the
console: the registers, the top of its stack and its last sixteen system
calls (number, first argument, result) — usually enough to see what it was
doing without a debugger.

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