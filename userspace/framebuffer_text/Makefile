# vkernel userspace - framebuffer text demo
# Copyright (C) 2026 vkernel authors
#
# Builds a freestanding static binary suitable for loading
# with the vkernel process loader.

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

TARGET  := framebuffer_text.vbin

.PHONY: all clean

all: $(TARGET)

$(TARGET): framebuffer_text.c
	@echo "  CC      $<  ->  $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(TARGET)
