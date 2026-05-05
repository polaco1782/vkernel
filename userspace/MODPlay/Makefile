# vkernel userspace - MODPlay
# Copyright (C) 2026 vkernel authors
#
# Builds a static PIE binary linked against newlib + vkernel CRT.
# Prerequisites: run scripts/setup_newlib.sh once, then make -C ../libc
#
# Usage:
#   make              — release build  → modplay.vbin
#   make DEBUG=1      — debug build
#   make clean

CC      ?= gcc
CXX     ?= g++
AR      ?= ar
OBJCOPY ?= objcopy

USER_INCLUDE   := ../include
SYSROOT        := ../sysroot
LIBC_DIR       := ../libc
CPP_DIR        := ../cpp
CPP_INCLUDE    := $(CPP_DIR)/include
KERNEL_INCLUDE := ../../include

CFLAGS  := -std=c11
CFLAGS  += -ffreestanding
CFLAGS  += -fno-builtin
CFLAGS  += -fno-stack-protector
CFLAGS  += -fpie
CFLAGS  += -m64
CFLAGS  += -mno-red-zone
ifdef DEBUG
CFLAGS  += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CFLAGS  += -O3 -DNDEBUG
endif
CFLAGS  += -Wall -Wextra
CFLAGS  += -isystem $(SYSROOT)/include
CFLAGS  += -I$(USER_INCLUDE)
CFLAGS  += -I$(KERNEL_INCLUDE)

CXXFLAGS := -std=c++17
CXXFLAGS += -ffreestanding
CXXFLAGS += -fno-builtin
CXXFLAGS += -fno-stack-protector
CXXFLAGS += -fpie
CXXFLAGS += -m64
CXXFLAGS += -mno-red-zone
CXXFLAGS += -fno-exceptions
CXXFLAGS += -fno-rtti
CXXFLAGS += -nostdinc++
ifdef DEBUG
CXXFLAGS += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CXXFLAGS += -O3 -DNDEBUG
endif
CXXFLAGS += -Wall -Wextra
CXXFLAGS += -isystem $(SYSROOT)/include
CXXFLAGS += -I$(USER_INCLUDE)
CXXFLAGS += -I$(CPP_INCLUDE)
CXXFLAGS += -I$(KERNEL_INCLUDE)
# modplay.c uses USE_LINEAR_INTERPOLATION by default (comment out for speed)

LDFLAGS := -nostdlib
LDFLAGS += -static-pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -L$(SYSROOT)/lib
LDFLAGS += -L$(LIBC_DIR)
LDFLAGS += -L$(CPP_DIR)

# Link order: crt0 → objects → --start-group (libc, glue, libgcc) --end-group
CRT0   := $(LIBC_DIR)/crt0.o
CPP_RUNTIME_LIB := $(CPP_DIR)/libvkcpp.a
DEBUG_ARG       := $(if $(DEBUG),DEBUG=$(DEBUG),)
# This frontend stays freestanding; libvkcpp provides the in-repo C++ runtime.
LIBS   := -Wl,--start-group -lvkcpp -lvksys -lc -lm -lgcc -Wl,--end-group

TARGET := modplay.vbin

LIBMODPLAY_SRCS := $(wildcard libmodplay/*.c)
CPP_SRCS := main_vk.cpp
OBJS   := $(CPP_SRCS:.cpp=.o) $(LIBMODPLAY_SRCS:.c=.o)

.PHONY: all clean distclean

all: $(CPP_RUNTIME_LIB) $(TARGET)

$(CPP_RUNTIME_LIB):
	@$(MAKE) --no-print-directory -C $(CPP_DIR) CXX=$(CXX) AR=$(AR) $(DEBUG_ARG)

main_vk.o: main_vk.cpp
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(CRT0) $(CPP_RUNTIME_LIB)
	@echo "  LD      $@"
	@$(CC) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBS)
ifndef DEBUG
	@echo "  STRIP   $@"
	@strip --strip-unneeded $@
endif
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(OBJS) $(TARGET)

distclean: clean
