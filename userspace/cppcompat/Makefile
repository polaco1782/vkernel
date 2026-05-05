CXX   ?= g++
AR    ?= ar
STRIP ?= strip

USER_INCLUDE   := ../include
SYSROOT        := ../sysroot
LIBC_DIR       := ../libc
CPP_DIR        := ../cpp
CPP_INCLUDE    := $(CPP_DIR)/include
KERNEL_INCLUDE := ../../include

CXXFLAGS  := -std=c++17
CXXFLAGS  += -ffreestanding
CXXFLAGS  += -fno-builtin
CXXFLAGS  += -fno-stack-protector
CXXFLAGS  += -fpie
CXXFLAGS  += -m64
CXXFLAGS  += -mno-red-zone
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
CXXFLAGS  += -I$(CPP_INCLUDE)

LDFLAGS  := -nostdlib
LDFLAGS  += -static-pie
LDFLAGS  += -e _start
LDFLAGS  += -Wl,--build-id=none
LDFLAGS  += -Wl,-z,norelro
LDFLAGS  += -Wl,--gc-sections
LDFLAGS  += -L$(SYSROOT)/lib
LDFLAGS  += -L$(LIBC_DIR)
LDFLAGS  += -L$(CPP_DIR)

CRT0            := $(LIBC_DIR)/crt0.o
CPP_RUNTIME_LIB := $(CPP_DIR)/libvkcpp.a
LIBS            := -Wl,--start-group -lvkcpp -lvksys -lc -lm -lgcc -Wl,--end-group
DEBUG_ARG       := $(if $(DEBUG),DEBUG=$(DEBUG),)

TARGET := cppcompat.vbin
OBJS   := main.o

.PHONY: all clean distclean

all: _check_sysroot $(CPP_RUNTIME_LIB) $(TARGET)

_check_sysroot:
	@test -f $(SYSROOT)/include/stdio.h || \
	    { echo ""; \
	      echo "  ERROR: newlib sysroot not found at $(SYSROOT)"; \
	      echo "  Run:  bash ../../scripts/setup_newlib.sh"; \
	      echo ""; exit 1; }

$(CPP_RUNTIME_LIB):
	@$(MAKE) --no-print-directory -C $(CPP_DIR) CXX=$(CXX) AR=$(AR) $(DEBUG_ARG)

%.o: %.cpp
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(CRT0) $(CPP_RUNTIME_LIB)
	@echo "  LD      $@"
	@$(CXX) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBS)
ifndef DEBUG
	@echo "  STRIP   $@"
	@$(STRIP) --strip-unneeded $@
endif
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -f $(OBJS) $(TARGET)

distclean: clean