# vkernel userspace - ramfs_reader
# Copyright (C) 2026 vkernel authors
#
# Builds a static PIE binary linked against newlib + vkernel CRT.
# Prerequisites: run scripts/setup_newlib.sh once, then make -C ../libc

CXX     ?= g++
OBJCOPY ?= objcopy

USER_INCLUDE   := ../include
SYSROOT        := ../sysroot
LIBC_DIR       := ../libc
KERNEL_INCLUDE := ../../include

CXXFLAGS  := -std=c++17
CXXFLAGS  += -ffreestanding
CXXFLAGS  += -fno-builtin
CXXFLAGS  += -fno-stack-protector
CXXFLAGS  += -fpie
CXXFLAGS  += -m64
CXXFLAGS  += -fno-exceptions
CXXFLAGS  += -fno-rtti
CXXFLAGS  += -nostdinc++
ifdef DEBUG
CXXFLAGS  += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CXXFLAGS  += -O2 -DNDEBUG
endif
CXXFLAGS  += -Wall -Wextra
CXXFLAGS  += -isystem $(SYSROOT)/include
CXXFLAGS  += -I$(USER_INCLUDE)
CXXFLAGS  += -I$(KERNEL_INCLUDE)

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
LIBS   := -Wl,--start-group -lvksys -lc -lstdc++ -lm -lgcc -Wl,--end-group

TARGET := ramfs_reader.vbin

.PHONY: all clean distclean

all: $(TARGET)

$(TARGET): ramfs_reader.cpp $(CRT0)
	@echo "  CXX     $<  →  $@"
	@$(CXX) $(CXXFLAGS) -c ramfs_reader.cpp -o ramfs_reader.o
	@echo "  LD      $@"
	@$(CXX) $(LDFLAGS) -o $@ $(CRT0) ramfs_reader.o $(LIBS)
ifndef DEBUG
	@strip --strip-unneeded $@
endif
	@rm -f ramfs_reader.o
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(TARGET) ramfs_reader.o

distclean: clean
