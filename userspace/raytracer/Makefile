# vkernel userspace - raytracer demo
# Copyright (C) 2026 vkernel authors
#
# Builds a static PIE binary linked against newlib + vkernel CRT.
# Prerequisites: run scripts/setup_newlib.sh once, then make -C ../libc

CC      ?= gcc
OBJCOPY ?= objcopy

USER_INCLUDE   := ../include
SYSROOT        := ../sysroot
LIBC_DIR       := ../libc
KERNEL_INCLUDE := ../../include

CFLAGS  := -std=c11
CFLAGS  += -ffreestanding
CFLAGS  += -fno-builtin
CFLAGS  += -fno-stack-protector
CFLAGS  += -fpie
CFLAGS  += -m64
ifdef DEBUG
CFLAGS  += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CFLAGS  += -Ofast -funroll-loops -march=native -DNDEBUG
endif
CFLAGS  += -Wall -Wextra
CFLAGS  += -isystem $(SYSROOT)/include
CFLAGS  += -I$(USER_INCLUDE)
CFLAGS  += -I$(KERNEL_INCLUDE)

LDFLAGS := -nostdlib
LDFLAGS += -static-pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -L$(SYSROOT)/lib
LDFLAGS += -L$(LIBC_DIR)

# Link order: crt0 → program objects → --start-group (libc, glue, libgcc) --end-group
CRT0   := $(LIBC_DIR)/crt0.o
LIBS   := -Wl,--start-group -lvksys -lc -lm -lgcc -Wl,--end-group

TARGET := raytracer.vbin

.PHONY: all clean distclean

all: $(TARGET)

$(TARGET): raytracer.c $(CRT0)
	@echo "  CC      $<  →  $@"
	@$(CC) $(CFLAGS) -c raytracer.c -o raytracer.o
	@echo "  LD      $@"
	@$(CC) $(LDFLAGS) -o $@ $(CRT0) raytracer.o $(LIBS)
ifndef DEBUG
	@strip --strip-unneeded $@
endif
	@rm -f raytracer.o
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(TARGET) raytracer.o

distclean: clean
