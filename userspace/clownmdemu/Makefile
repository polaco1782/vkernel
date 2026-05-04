CC      ?= gcc
CXX     ?= g++
OBJCOPY ?= objcopy

USER_INCLUDE   := ../include
SYSROOT        := ../sysroot
LIBC_DIR       := ../libc
KERNEL_INCLUDE := ../../include
VENDOR_DIR     := vendor/clownmdemu-core

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
CXXFLAGS += -I$(KERNEL_INCLUDE)

LDFLAGS := -nostdlib
LDFLAGS += -static-pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -L$(SYSROOT)/lib
LDFLAGS += -L$(LIBC_DIR)

CRT0   := $(LIBC_DIR)/crt0.o
LIBS   := -Wl,--start-group -lvksys -lc -lstdc++ -lm -lgcc -Wl,--end-group

TARGET := clownmdemu.vbin

CPP_SRCS := main.cpp
C_SRCS   := $(VENDOR_DIR)/unity.c
OBJS     := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

.PHONY: all clean distclean

all: $(TARGET)

main.o: main.cpp
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(CRT0)
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