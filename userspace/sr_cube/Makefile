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
CFLAGS  += -O3 -DNDEBUG
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

CRT0   := $(LIBC_DIR)/crt0.o
LIBS   := -Wl,--start-group -lvksys -lc -lm -lgcc -Wl,--end-group

TARGET := sr_cube.vbin

.PHONY: all clean distclean

all: $(TARGET)

$(TARGET): sr_cube.c $(CRT0)
	@echo "  CC      $<  →  $@"
	@$(CC) $(CFLAGS) -c sr_cube.c -o sr_cube.o
	@echo "  LD      $@"
	@$(CC) $(LDFLAGS) -o $@ $(CRT0) sr_cube.o $(LIBS)
ifndef DEBUG
	@strip --strip-unneeded $@
endif
	@rm -f sr_cube.o
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(TARGET) sr_cube.o

distclean: clean
