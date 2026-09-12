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

BUILD   := build
TARGET  := $(BUILD)/sic.elf

# Where the other projects (libc, ZAE, zaeboot) find what the kernel provides.
SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)

CFLAGS  := --target=x86_64-elf -std=c11 -ffreestanding -fno-stack-protector \
           -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=small \
           -fno-asynchronous-unwind-tables -fno-builtin -nostdlib \
           -O2 -g -Wall -Wextra -Iinclude -DSIC_KERNEL
ASFLAGS := --target=x86_64-elf -g
LDFLAGS := -T linker.ld -nostdlib -static -z max-page-size=0x1000

# Sources live in kernel/<subsystem>/ (arch/x86_64, mm, fs, drivers, proc, lib, core).
CSRC := $(shell find kernel -name '*.c')
SSRC := $(shell find kernel -name '*.S')
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CSRC)) $(patsubst %.S,$(BUILD)/%.o,$(SSRC))

# Loadable modules: modules/<name>/*.c -> build/modules/<name>.ko (ET_REL, ld -r).
MODULES  := $(patsubst modules/%,%,$(wildcard modules/*))
MOD_KOS  := $(patsubst %,$(BUILD)/modules/%.ko,$(MODULES))
MOD_CFLAGS := $(CFLAGS) -fno-common

.PHONY: all clean modules install

all: $(TARGET) modules
modules: $(MOD_KOS)

# install: the kernel image, its loadable modules, the public ABI headers and
# the syscall table (for the libc's generator) into the sysroot.
install: all
	@mkdir -p $(SYSROOT)/boot $(SYSROOT)/lib/modules $(SYSROOT)/usr/include/abi $(SYSROOT)/usr/share/sic/abi
	cp $(TARGET) $(SYSROOT)/boot/sic.elf
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

$(BUILD)/modules/%.o: modules/%.c include/module.h
	@mkdir -p $(dir $@)
	$(CC) $(MOD_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
