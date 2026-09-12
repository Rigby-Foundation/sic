# sic

A small x86_64 kernel in C, booted by [zaeboot](https://github.com/Rigby-Foundation/zaeboot) over the
`zaeboot` boot protocol (`include/zaeboot.h`). The C library ([libc](https://github.com/Rigby-Foundation/musl),
a musl port) and the userland ([ZAE](https://github.com/Rigby-Foundation/zae)) are separate projects; what ties
them together is the sic system call ABI: `abi/syscall.tbl` (numbers, generated
into `include/abi/syscall_nr.h` and musl's `bits/syscall.h`) and
`include/abi/abi.h` (struct layouts and constants).

Layout: `kernel/{arch/x86_64,mm,fs,drivers,proc,lib,core}` with matching
`include/` subdirectories; `include/abi/` is the public ABI (syscall table,
struct layouts, zaefs format) that libc and userland build against.

What it does today:

- 64-bit entry (`kernel/arch/x86_64/entry.S`) that sets up its own stack and calls `kernel_main`
- Framebuffer text console (`kernel/drivers/fb.c`, scaled 8x8 font) + COM1 serial
- `kprintf` (`%d %u %x %X %p %c %s`, `l` modifier, zero padding)
- Its own GDT (`kernel/gdt.c`) and IDT with all 32 exception handlers (`kernel/arch/x86_64/idt.c`, `kernel/arch/x86_64/isr.S`)
- ACPI (`kernel/arch/x86_64/acpi.c`): RSDP -> XSDT/RSDT with checksums, `acpi_find_table()`, MADT
  parsing (LAPIC address/override, CPUs, IOAPICs, interrupt source overrides)
- APIC (`kernel/arch/x86_64/apic.c`): LAPIC enabled with spurious vector 0xFF, IOAPIC redirection
  honouring MADT overrides, LAPIC timer calibrated against the PIT; the 8259 PIC
  (`kernel/arch/x86_64/pic.c`) is remapped then fully masked, and stays as the fallback path
- IRQ layer (`kernel/arch/x86_64/idt.c`): `irq_install/irq_mask/irq_unmask` work the same on either
  controller; 1000 Hz tick (`kernel/arch/x86_64/timer.c`), PS/2 keyboard with echo (`kernel/drivers/keyboard.c`)
- SMP (`kernel/arch/x86_64/smp.c`, `kernel/arch/x86_64/ap_trampoline.S`, `kernel/arch/x86_64/cpu.c`): APs from the MADT are
  started with INIT/SIPI through a real-mode trampoline at 0x8000; each CPU has its own
  GDT/TSS and a per-CPU block reachable via `%gs`; TLB shootdowns via IPI
- Scheduler (`kernel/proc/sched.c`, `kernel/arch/x86_64/switch.S`): preemptive round-robin over one global
  run queue shared by all CPUs, 10 ms slices, `task_create/yield/sleep_ms/exit/join/block/wake`,
  per-CPU idle tasks that reap zombies; preemption happens in the IRQ epilogue after EOI
- VFS (`kernel/fs/vfs.c`): vnode tree with per-filesystem `vnode_ops`, mount points,
  refcounted file objects shared across fork, per-process fd tables and cwd. Filesystems:
  `tmpfs` (`kernel/fs/tmpfs.c`, the root, populated from the USTAR initrd zaeboot loads) and
  **zaefs** (`kernel/fs/zaefs.c`, sic's on-disk filesystem: 4 KiB blocks, bitmaps, 128-byte
  inodes with direct/indirect/double-indirect pointers, ext2-style directories, write-through
  block cache; format in `include/abi/zaefs.h`, shared with `mkfs.zaefs`). `/dev/console`
  is the keyboard (blocking reads, wake-on-IRQ) + framebuffer/serial output
- Storage: PCI enumeration (`kernel/drivers/pci.c`), a polled NVMe driver (`kernel/drivers/nvme.c`, admin +
  one I/O queue pair), and a block layer (`kernel/fs/blkdev.c`) exposing `/dev/nvme0n1` with
  byte-addressed, seekable reads/writes for user-space tools
- Loadable kernel modules (`kernel/core/module.c`): an in-kernel ELF64 relocatable linker
  (`R_X86_64_64/PC32/PLT32/32/32S/PC64`) resolving against the exported symbol table
  (`EXPORT_SYMBOL` in `kernel/core/ksyms.c`, collected into `.ksymtab` by the linker script);
  modules live in low identity-mapped memory within ±2 GiB of the kernel. Modules are
  `modules/<name>/*.c` built to `build/modules/<name>.ko` (`ld.lld -r`) and must define
  `module_name`, `init_module` and optionally `cleanup_module`. `modules/hello` is the example
- Processes (`kernel/proc/elf.c`, `kernel/proc/syscall.c`, `kernel/arch/x86_64/syscall_entry.S`): per-process
  address spaces (user half at `0x8000000000+`, kernel mappings shared), static ELF64 loader
  with argc/argv on the user stack, `fork` (address-space copy, fd inheritance, child
  returns through a copied syscall frame), `exec` (image replaced in place), `waitpid`
  (zombies held until collected), ring 3 entry via `iretq`, syscalls via `syscall`/`sysret`;
  a faulting process is killed, not the kernel
- System calls (`kernel/proc/syscall.c`): sic's own numbering with POSIX semantics as musl
  expects them — read/write/readv/writev, open(at)/close/lseek/(new)fstat(at)/getdents64,
  mkdir/unlink/rmdir/chdir/getcwd/dup/dup2/dup3/fcntl/ioctl(TIOCGWINSZ)/access, mmap
  (anonymous)/munmap/brk, fork/execve/wait4/exit_group/getpid/getppid, arch_prctl (TLS),
  clock_gettime/nanosleep, uname/getrandom, mprotect (real, incl. PROT_EXEC), private file
  mmap; sic extensions: mount/umount2, init_module/delete_module/query_module. Signals
  are accepted but not delivered.
  The initial process stack carries argc/argv/envp/auxv (AT_PHDR, AT_RANDOM, ...).
  Per-task x87/SSE state (fxsave) and FS_BASE are switched with the task
- The userland itself (libc, `/bin/sh` and friends) is [ZAE](https://github.com/Rigby-Foundation/zae); the kernel only
  needs `/bin/init` to exist in the initrd and will run `/bin/test` + `/bin/crash` as a
  self test when they're present
- PMM (`kernel/mm/pmm.c`): frame bitmap built from the boot memory map; firmware/loader
  memory is reclaimed once the kernel is off UEFI's page tables
- VMM (`kernel/mm/vmm.c`): own 4-level page tables, 2 MiB identity map + higher-half
  direct map (`HHDM_BASE`) of all RAM (min 4 GiB for MMIO), NX enabled,
  `vmm_map_page` / `vmm_unmap_page` / `vmm_translate`
- Heap (`kernel/mm/heap.c`): slab allocator with size classes 16..2048 on 16 KiB slabs,
  large allocations straight from pages, per-page owner table for O(1) `kfree`;
  `kmalloc` / `kzalloc` / `krealloc` / `kfree`, plus `heap_alloc_pages` for page-granular needs
- Boot-time self tests for PMM, VMM, heap, timer, scheduler, SMP, VFS and userspace,
  then `/bin/init` starts a shell on the console

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