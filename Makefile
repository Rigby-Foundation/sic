# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# sic - a small x86_64 kernel, booted by zaeboot. Userland is the separate ZAE project.
#
# Builds with clang + ld.lld (Homebrew LLVM on macOS, or any recent LLVM).

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))

ifeq ($(origin CC),default)
CC := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/clang,clang)
endif
ifeq ($(origin LD),default)
LD := $(if $(LLD_PREFIX),$(LLD_PREFIX)/bin/ld.lld,ld.lld)
endif

# Target architecture: x86_64 (default), powerpc (32-bit big-endian, G4) or aarch64.
# Each has kernel/arch/<arch>/ (code, linker script, arch.mk with the flags)
# and include/arch/<arch>/asm/ (the headers the generic code includes as asm/*).
ARCH    ?= x86_64
BUILD   := build/$(ARCH)
TARGET  := $(BUILD)/sic.elf
include kernel/arch/$(ARCH)/arch.mk

# --- configuration ------------------------------------------------------------
# .config holds CONFIG_<NAME>=y/n (see defconfig for the list). Any CONFIG_*
# variable given on the command line overrides it. include/generated/config.h
# is produced from the result and included by every kernel source.
CONFIG_FILE := $(wildcard .config.$(ARCH))
ifeq ($(CONFIG_FILE),)
ifeq ($(ARCH),x86_64)
CONFIG_FILE := $(wildcard .config)
endif
endif
ifeq ($(CONFIG_FILE),)
CONFIG_FILE := $(firstword $(wildcard configs/defconfig.$(ARCH)) configs/defconfig)
endif
CONFIG_OVERRIDES := $(foreach v,$(filter CONFIG_%,$(.VARIABLES)),$(if $(filter command line,$(origin $(v))),$(v)=$($(v))))
cfg = $(strip $(shell python3 scripts/genconfig.py $(CONFIG_FILE) $(CONFIG_OVERRIDES) | grep -q "define $(1) " && echo y))
CONFIG_SMP        := $(call cfg,CONFIG_SMP)
CONFIG_FB_CONSOLE := $(call cfg,CONFIG_FB_CONSOLE)
CONFIG_SERIAL     := $(call cfg,CONFIG_SERIAL)
CONFIG_KEYBOARD   := $(call cfg,CONFIG_KEYBOARD)
CONFIG_MOUSE      := $(call cfg,CONFIG_MOUSE)
CONFIG_VIRTIO_GPU := $(call cfg,CONFIG_VIRTIO_GPU)
CONFIG_VIRTIO_INPUT := $(call cfg,CONFIG_VIRTIO_INPUT)
CONFIG_PCI        := $(call cfg,CONFIG_PCI)
CONFIG_NVME       := $(call cfg,CONFIG_NVME)
CONFIG_AHCI       := $(call cfg,CONFIG_AHCI)
CONFIG_IDE        := $(call cfg,CONFIG_IDE)
CONFIG_NET        := $(call cfg,CONFIG_NET)
CONFIG_E1000      := $(call cfg,CONFIG_E1000)
CONFIG_HDA        := $(call cfg,CONFIG_HDA)
CONFIG_ZAEFS      := $(call cfg,CONFIG_ZAEFS)
CONFIG_FAT        := $(call cfg,CONFIG_FAT)
CONFIG_MODULES    := $(call cfg,CONFIG_MODULES)
CONFIG_SIGNALS    := $(call cfg,CONFIG_SIGNALS)
CONFIG_PIPES      := $(call cfg,CONFIG_PIPES)
CONFIG_SELFTEST   := $(call cfg,CONFIG_SELFTEST)

# Where the other projects (libc, ZAE, zaeboot) find what the kernel provides.
SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)

CFLAGS  := $(ARCH_CFLAGS) -std=c11 -ffreestanding -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-builtin -nostdlib -fno-omit-frame-pointer \
           -O2 -g -Wall -Wextra -Iinclude -Iinclude/arch/$(ARCH) -DSIC_KERNEL -include generated/config.h $(CFLAGS_EXTRA)
ASFLAGS := $(ARCH_ASFLAGS) -g -Iinclude -Iinclude/arch/$(ARCH)
LINKER_SCRIPT := kernel/arch/$(ARCH)/linker.ld
LDFLAGS := $(ARCH_LDFLAGS) -T $(LINKER_SCRIPT) -nostdlib -static -z max-page-size=0x1000

# Sources live in kernel/<subsystem>/ (arch/x86_64, mm, fs, drivers, proc, lib, core).
# Optional pieces are listed per config option; everything else is always built.
OPTIONAL := kernel/arch/x86_64/smp.c kernel/arch/x86_64/ap_trampoline.S kernel/arch/aarch64/smp.c kernel/arch/powerpc/escc.c kernel/arch/aarch64/pl011.c \
            kernel/arch/x86_64/module.c kernel/arch/powerpc/module.c kernel/arch/aarch64/module.c \
            kernel/drivers/fb.c kernel/drivers/font.c kernel/drivers/serial.c kernel/drivers/keyboard.c kernel/drivers/mouse.c \
            kernel/drivers/pci.c kernel/drivers/nvme.c kernel/drivers/ahci.c kernel/drivers/ide.c \
            kernel/drivers/virtio.c kernel/drivers/virtio_gpu.c kernel/drivers/virtio_input.c \
            kernel/fs/zaefs.c kernel/fs/fat.c \
            kernel/net/core.c kernel/net/arp.c kernel/net/ip.c kernel/net/udp.c kernel/net/tcp.c kernel/net/socket.c kernel/net/unix.c \
            kernel/drivers/e1000.c kernel/drivers/hda.c kernel/drivers/dsp.c \
            kernel/core/module.c kernel/core/ksyms.c kernel/proc/signal.c kernel/fs/pipe.c \
            kernel/core/selftest.c
SRC-y :=
SRC-$(CONFIG_SMP)        += $(ARCH_SMP_SRC)
SRC-$(CONFIG_FB_CONSOLE) += kernel/drivers/fb.c kernel/drivers/font.c
SRC-$(CONFIG_SERIAL)     += $(ARCH_SERIAL_SRC)
SRC-$(CONFIG_KEYBOARD)   += kernel/drivers/keyboard.c
SRC-$(CONFIG_MOUSE)      += kernel/drivers/mouse.c
SRC-$(CONFIG_VIRTIO_GPU) += kernel/drivers/virtio_gpu.c
SRC-$(CONFIG_VIRTIO_INPUT) += kernel/drivers/virtio_input.c
SRC-y += $(if $(CONFIG_VIRTIO_GPU)$(CONFIG_VIRTIO_INPUT),kernel/drivers/virtio.c,)
SRC-$(CONFIG_PCI)        += kernel/drivers/pci.c
SRC-$(CONFIG_NVME)       += kernel/drivers/nvme.c
SRC-$(CONFIG_AHCI)       += kernel/drivers/ahci.c
SRC-$(CONFIG_IDE)        += kernel/drivers/ide.c
SRC-$(CONFIG_NET)        += kernel/net/core.c kernel/net/arp.c kernel/net/ip.c kernel/net/udp.c kernel/net/tcp.c kernel/net/socket.c kernel/net/unix.c
SRC-$(CONFIG_E1000)      += kernel/drivers/e1000.c
SRC-$(CONFIG_HDA)        += kernel/drivers/hda.c kernel/drivers/dsp.c
SRC-$(CONFIG_ZAEFS)      += kernel/fs/zaefs.c
SRC-$(CONFIG_FAT)        += kernel/fs/fat.c
SRC-$(CONFIG_MODULES)    += kernel/core/module.c kernel/core/ksyms.c kernel/arch/$(ARCH)/module.c
SRC-$(CONFIG_SIGNALS)    += kernel/proc/signal.c
SRC-$(CONFIG_PIPES)      += kernel/fs/pipe.c
SRC-$(CONFIG_SELFTEST)   += kernel/core/selftest.c
# Everything under kernel/ except other architectures' directories.
ALL_SRC := $(shell find kernel -not -path 'kernel/arch/*' \( -name '*.c' -o -name '*.S' \)) \
           $(wildcard kernel/arch/$(ARCH)/*.c kernel/arch/$(ARCH)/*.S)
ALWAYS := $(filter-out $(OPTIONAL),$(ALL_SRC))
CSRC := $(filter %.c,$(ALWAYS) $(SRC-y))
SSRC := $(filter %.S,$(ALWAYS) $(SRC-y))
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CSRC)) $(patsubst %.S,$(BUILD)/%.o,$(SSRC))
CONFIG_H := include/generated/config.h

# Loadable modules: modules/<name>/*.c -> build/modules/<name>.ko (ET_REL, ld -r).
MODULES  := $(if $(CONFIG_MODULES),$(patsubst modules/%,%,$(wildcard modules/*)),)
MOD_KOS  := $(patsubst %,$(BUILD)/modules/%.ko,$(MODULES))
MOD_CFLAGS := $(CFLAGS) -fno-common

.PHONY: all clean modules install defconfig config

all: $(TARGET) $(ARCH_IMAGE) modules
modules: $(MOD_KOS)

# A raw image beside the ELF where the boot protocol wants one (aarch64: a
# Linux-style Image that QEMU's -kernel loads with the initrd and the device tree).
OBJCOPY := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-objcopy,llvm-objcopy)
$(BUILD)/sic.img: $(TARGET)
	$(OBJCOPY) -O binary $< $@

defconfig:
	cp configs/defconfig .config
	@echo "wrote .config from configs/defconfig"

# Show the effective configuration.
config: $(CONFIG_H)
	@grep -E "define|not set" $(CONFIG_H) | sed 's|/\* \(.*\) is not set \*/|\1=n|; s|#define \(.*\) 1|\1=y|'

# Regenerated on every make (overrides on the command line don't touch any
# file), but only rewritten when the content changes so objects stay fresh.
$(CONFIG_H): $(CONFIG_FILE) scripts/genconfig.py Makefile FORCE
	@mkdir -p $(dir $@)
	@python3 scripts/genconfig.py $(CONFIG_FILE) $(CONFIG_OVERRIDES) > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@ && echo "config: regenerated $@"; fi

FORCE:
.PHONY: FORCE

# install: the kernel image, its loadable modules, the public ABI headers and
# the syscall table (for the libc's generator) into the sysroot.
install: all
	@mkdir -p $(SYSROOT)/boot $(SYSROOT)/lib/modules $(SYSROOT)/usr/include/abi $(SYSROOT)/usr/share/sic/abi
	cp $(TARGET) $(SYSROOT)/boot/sic.elf
	@[ -z "$(ARCH_IMAGE)" ] || cp $(ARCH_IMAGE) $(SYSROOT)/boot/sic.img
	cp include/abi/*.h $(SYSROOT)/usr/include/abi/
	cp abi/syscall.tbl abi/gen.py $(SYSROOT)/usr/share/sic/abi/
	@rm -f $(SYSROOT)/lib/modules/*.ko; [ -z "$(MOD_KOS)" ] || cp $(MOD_KOS) $(SYSROOT)/lib/modules/
	@echo "installed into $(SYSROOT)"

define MODULE_RULE
$(BUILD)/modules/$(1).ko: $(patsubst modules/%.c,$(BUILD)/modules/%.o,$(wildcard modules/$(1)/*.c))
	@mkdir -p $$(dir $$@)
	$$(LD) -r -o $$@ $$^
endef
$(foreach m,$(MODULES),$(eval $(call MODULE_RULE,$(m))))

$(BUILD)/modules/%.o: modules/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MOD_CFLAGS) -MMD -MP -c $< -o $@

# Two-pass link: the first pass with an empty symbol table fixes every text
# address (the table itself is data and lands after .text), the second links
# in the table generated from the first image so crash dumps can name code.
NM := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-nm,llvm-nm)
$(BUILD)/syms0.c:
	@mkdir -p $(dir $@)
	@echo '/* empty first-pass table */' > $@
	@echo '#include "types.h"' >> $@
	@echo 'const struct kernel_sym { uint64_t addr; const char *name; } kernel_syms[] = { { 0, 0 } };' >> $@
	@echo 'const unsigned kernel_sym_count = 0;' >> $@
$(BUILD)/syms0.o: $(BUILD)/syms0.c $(CONFIG_H)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/sic-pass1.elf: $(OBJS) $(BUILD)/syms0.o $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/syms0.o
$(BUILD)/syms.c: $(BUILD)/sic-pass1.elf scripts/gensyms.py
	$(NM) -n $< | python3 scripts/gensyms.py > $@
$(BUILD)/syms.o: $(BUILD)/syms.c
	$(CC) $(CFLAGS) -c -o $@ $<
$(TARGET): $(OBJS) $(BUILD)/syms.o $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/syms.o

# -MMD writes build/x.d next to each object so header changes (struct
# layouts!) recompile everything that includes them.
$(BUILD)/%.o: %.c $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d) $(patsubst %.ko,%.d,$(MOD_KOS)) $(shell find $(BUILD)/modules -name '*.d' 2>/dev/null)

clean:
	rm -rf $(BUILD) include/generated
