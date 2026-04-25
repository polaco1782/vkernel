# vkernel userspace - vgui (Dear ImGui demo)
# Copyright (C) 2026 vkernel authors
#
# Builds a static PIE binary linked against newlib + vkernel CRT.
#
# Prerequisites (one-time setup):
#   1.  Run  scripts/setup_newlib.sh   from the project root.
#   2.  Run  make -C ../libc           to build crt0 + libvksys.
#   3.  Run  bash setup_imgui.sh       to download Dear ImGui sources.
#
# Then just:   make          (release)
#              make DEBUG=1  (debug, keeps symbols and -Og)

CXX     ?= g++
AR      ?= ar
STRIP   ?= strip

ROOT_DIR        := $(realpath $(CURDIR)/../..)
USER_INCLUDE    := ../include
SYSROOT         := ../sysroot
LIBC_DIR        := ../libc
KERNEL_INCLUDE  := $(ROOT_DIR)/include
IMGUI_DIR       := ./imgui

# ---- Common flags -------------------------------------------------------

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
CXXFLAGS  += -I$(IMGUI_DIR)
CXXFLAGS  += -I.
# Tell every TU (including ImGui) to pick up our config header
CXXFLAGS  += -DIMGUI_USER_CONFIG=\"imgui_config.h\"

# ImGui gets slightly relaxed warnings (it was not written with our flags)
IMGUI_CXXFLAGS  := $(CXXFLAGS)
IMGUI_CXXFLAGS  += -Wno-unused-parameter
IMGUI_CXXFLAGS  += -Wno-missing-field-initializers
IMGUI_CXXFLAGS  += -Wno-sign-compare
IMGUI_CXXFLAGS  += -Wno-type-limits
# Disable the default shell opener — it references execvp/waitpid which
# are not available in our newlib/vkernel syscall stubs.
IMGUI_CXXFLAGS  += -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS

# ---- Linker flags -------------------------------------------------------

LDFLAGS  := -nostdlib
LDFLAGS  += -static-pie
LDFLAGS  += -e _start
LDFLAGS  += -Wl,--build-id=none
LDFLAGS  += -Wl,-z,norelro
LDFLAGS  += -Wl,--gc-sections
LDFLAGS  += -L$(SYSROOT)/lib
LDFLAGS  += -L$(LIBC_DIR)

# No -lstdc++ — cxxabi_stubs.cpp provides all the C++ ABI we need
# without pulling in the host's glibc-dependent libstdc++.a.
CRT0     := $(LIBC_DIR)/crt0.o
LIBS     := -Wl,--start-group -lvksys -lc -lm -lgcc -Wl,--end-group

# ---- Sources & objects --------------------------------------------------

IMGUI_SRCS :=                          \
    $(IMGUI_DIR)/imgui.cpp             \
    $(IMGUI_DIR)/imgui_draw.cpp        \
    $(IMGUI_DIR)/imgui_tables.cpp      \
    $(IMGUI_DIR)/imgui_widgets.cpp     \
    $(IMGUI_DIR)/imgui_demo.cpp

IMGUI_OBJS := $(IMGUI_SRCS:.cpp=.o)

OUR_SRCS   := cxxabi_stubs.cpp imgui_impl_vk.cpp main.cpp
OUR_OBJS   := $(OUR_SRCS:.cpp=.o)

ALL_OBJS   := $(IMGUI_OBJS) $(OUR_OBJS)

TARGET     := vgui.vbin

# ---- Phony targets ------------------------------------------------------

.PHONY: all clean distclean help

all: _check_imgui _check_sysroot $(TARGET)

help:
	@echo "vgui build targets:"
	@echo "  make              – release build"
	@echo "  make DEBUG=1      – debug build"
	@echo "  make clean        – remove objects and binary"
	@echo "  make distclean    – also remove downloaded ImGui sources"
	@echo ""
	@echo "First-time setup:"
	@echo "  bash setup_imgui.sh   – download Dear ImGui $(IMGUI_TAG)"
	@echo "  make -C ../libc       – build newlib glue (crt0, libvksys)"

# ---- Dependency guards --------------------------------------------------

_check_imgui:
	@test -f $(IMGUI_DIR)/imgui.h || \
	    { echo ""; \
	      echo "  ERROR: ImGui sources not found in $(IMGUI_DIR)/"; \
	      echo "  Run:  bash setup_imgui.sh"; \
	      echo ""; exit 1; }

_check_sysroot:
	@test -f $(SYSROOT)/include/stdio.h || \
	    { echo ""; \
	      echo "  ERROR: newlib sysroot not found at $(SYSROOT)"; \
	      echo "  Run:  bash ../../scripts/setup_newlib.sh"; \
	      echo ""; exit 1; }

# ---- Compilation rules --------------------------------------------------

$(IMGUI_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	@echo "  CXX [imgui] $(<F)"
	@$(CXX) $(IMGUI_CXXFLAGS) -c $< -o $@

%.o: %.cpp
	@echo "  CXX         $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- Link ---------------------------------------------------------------

$(TARGET): $(CRT0) $(ALL_OBJS)
	@echo "  LD          $@"
	@$(CXX) $(LDFLAGS) -o $@ $(CRT0) $(ALL_OBJS) $(LIBS)
ifndef DEBUG
	@echo "  STRIP       $@"
	@$(STRIP) --strip-unneeded $@
endif
	@echo "  SIZE        $@"
	@size $@ 2>/dev/null || true
	@echo "  OK →  $@"

# ---- Cleanup ------------------------------------------------------------

clean:
	@rm -f $(TARGET) $(ALL_OBJS)
	@echo "  cleaned."

distclean: clean
	@rm -rf imgui/
	@echo "  imgui/ removed."
