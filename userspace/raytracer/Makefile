# vkernel userspace - raytracer demo
# Copyright (C) 2026 vkernel authors
#
# Builds a freestanding static ELF64 binary suitable for loading
# with the vkernel ELF loader.

CC      ?= gcc
OBJCOPY ?= objcopy

KERNEL_INCLUDE := ../../include
USER_INCLUDE   := ../include
ABI_HEADER_SRC := $(KERNEL_INCLUDE)/vkernel/vk.h
ABI_HEADER_DST := $(USER_INCLUDE)/vkernel/vk.h

CFLAGS  := -std=c11
CFLAGS  += -nostdlib
CFLAGS  += -ffreestanding
CFLAGS  += -fno-builtin
CFLAGS  += -fno-stack-protector
CFLAGS  += -fpie
CFLAGS  += -m64
CFLAGS  += -O2
CFLAGS  += -Wall -Wextra
CFLAGS  += -I$(KERNEL_INCLUDE)
CFLAGS  += -I$(USER_INCLUDE)

LDFLAGS := -nostdlib
LDFLAGS += -static
LDFLAGS += -pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro

TARGET  := raytracer.elf

.PHONY: all clean

all: $(ABI_HEADER_DST) $(TARGET)

$(ABI_HEADER_DST): $(ABI_HEADER_SRC)
	@mkdir -p $(dir $@)
	@cp $< $@

$(TARGET): raytracer.c $(ABI_HEADER_DST)
	@echo "  CC      $<  ->  $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(TARGET)
