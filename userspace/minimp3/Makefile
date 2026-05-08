CC      ?= gcc
CXX     ?= g++
AR      ?= ar

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

LDFLAGS := -nostdlib
LDFLAGS += -static-pie
LDFLAGS += -e _start
LDFLAGS += -Wl,--build-id=none
LDFLAGS += -Wl,-z,norelro
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -L$(SYSROOT)/lib
LDFLAGS += -L$(LIBC_DIR)
LDFLAGS += -L$(CPP_DIR)

CRT0            := $(LIBC_DIR)/crt0.o
CPP_RUNTIME_LIB := $(CPP_DIR)/libvkcpp.a
DEBUG_ARG       := $(if $(DEBUG),DEBUG=$(DEBUG),)
LIBS            := -Wl,--start-group -lvkcpp -lvksys -lc -lm -lgcc -Wl,--end-group

TARGET   := minimp3.vbin

CPP_SRCS := main.cpp
C_SRCS   := minimp3_impl.c
OBJS     := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

.PHONY: all clean distclean

all: $(CPP_RUNTIME_LIB) $(TARGET)

$(CPP_RUNTIME_LIB):
	@$(MAKE) --no-print-directory -C $(CPP_DIR) CXX=$(CXX) AR=$(AR) $(DEBUG_ARG)

main.o: main.cpp
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