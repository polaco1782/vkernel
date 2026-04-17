# vkernel userspace - ramfs reader demo
# Copyright (C) 2026 vkernel authors
#
# Builds a freestanding static ELF64 binary suitable for loading
# with the vkernel ELF loader.

CC      ?= gcc
OBJCOPY ?= objcopy

USER_INCLUDE   := ../include

CFLAGS  := -std=c11
CFLAGS  += -nostdlib
CFLAGS  += -ffreestanding
CFLAGS  += -fno-builtin
CFLAGS  += -fno-stack-protector
CFLAGS  += -fpie
CFLAGS  += -m64
CFLAGS  += -O2
CFLAGS  += -Wall -Wextra
CFLAGS  += -I$(USER_INCLUDE)

LDFLAGS := -nostdlib
LDFLAGS += -static
LDFLAGS += -pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro

TARGET  := ramfs_reader.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): ramfs_reader.c
	@echo "  CC      $<  ->  $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(TARGET)
