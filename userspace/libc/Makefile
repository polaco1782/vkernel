# vkernel userspace - C runtime glue library
# Copyright (C) 2026 vkernel authors
#
# Builds:
#   crt0.o      — entry-point object (linked first by every program)
#   libvksys.a  — newlib syscall stubs (linked alongside libc.a)
#
# The newlib sysroot must exist before building this.
# Run:  ../scripts/setup_newlib.sh

CC           ?= gcc
AR           ?= ar
RANLIB       ?= ranlib

ROOT_DIR     := $(realpath $(CURDIR)/../..)
SYSROOT      := $(CURDIR)/../sysroot
KERNEL_INC   := $(ROOT_DIR)/include

# We compile against newlib headers from the sysroot.
CFLAGS  := -std=c11
CFLAGS  += -ffreestanding
CFLAGS  += -fno-builtin
CFLAGS  += -fno-stack-protector
CFLAGS  += -fpie
CFLAGS  += -m64
ifdef DEBUG
CFLAGS  += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CFLAGS  += -O2 -DNDEBUG
endif
CFLAGS  += -Wall -Wextra
CFLAGS  += -isystem $(SYSROOT)/include
CFLAGS  += -I$(KERNEL_INC)

OBJS_GLUE := syscalls.o
TARGET_CRT := crt0.o
TARGET_LIB := libvksys.a

.PHONY: all clean distclean

all: $(TARGET_CRT) $(TARGET_LIB)

$(TARGET_CRT): crt0.c
	@echo "  CC      $<  →  $@"
	@$(CC) $(CFLAGS) -c $< -o $@

syscalls.o: syscalls.c
	@echo "  CC      $<  →  $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET_LIB): $(OBJS_GLUE)
	@echo "  AR      $@"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@

clean:
	@rm -f $(TARGET_CRT) $(TARGET_LIB) $(OBJS_GLUE)

distclean: clean
