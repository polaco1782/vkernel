# vkernel userspace - vgui (Dear ImGui demo)
# Copyright (C) 2026 vkernel authors
#
# Builds a static PIE binary linked against newlib + vkernel CRT.
#
# Prerequisites (one-time setup):
#   1.  Run  scripts/setup_newlib.sh   from the project root.
#   2.  Run  make -C ../libc           to build crt0 + libvksys.
#   3.  Run  bash setup_imgui.sh       to download Dear ImGui sources.
#   4.  Run  bash setup_implot.sh      to download ImPlot sources.
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
CPP_INCLUDE     := ../cpp/include
KERNEL_INCLUDE  := $(ROOT_DIR)/include
IMGUI_DIR       := ./imgui
IMPLOT_DIR      := ./implot

# ---- Common flags -------------------------------------------------------

DEPFLAGS := -MMD -MP

CXXFLAGS  := -std=c++20
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
CXXFLAGS  += -I$(CPP_INCLUDE)
CXXFLAGS  += -I$(KERNEL_INCLUDE)
CXXFLAGS  += -I$(IMGUI_DIR)
CXXFLAGS  += -I$(IMPLOT_DIR)
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

# ImPlot is another third-party Dear ImGui extension; build it with the
# same relaxed warning profile we use for vendored ImGui sources.
IMPLOT_CXXFLAGS := $(IMGUI_CXXFLAGS)

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
LIBVKSYS := $(LIBC_DIR)/libvksys.a
LIBS     := -Wl,--start-group -lvksys -lc -lm -lgcc -Wl,--end-group

LIBC_MAKE_ARGS :=
ifdef DEBUG
LIBC_MAKE_ARGS += DEBUG=$(DEBUG)
endif

# ---- Sources & objects --------------------------------------------------

IMGUI_SRCS :=                          \
    $(IMGUI_DIR)/imgui.cpp             \
    $(IMGUI_DIR)/imgui_draw.cpp        \
    $(IMGUI_DIR)/imgui_tables.cpp      \
    $(IMGUI_DIR)/imgui_widgets.cpp     \
    $(IMGUI_DIR)/imgui_demo.cpp

IMGUI_OBJS := $(IMGUI_SRCS:.cpp=.o)
IMGUI_DEPS := $(IMGUI_OBJS:.o=.d)

IMPLOT_SRCS :=                         \
	$(IMPLOT_DIR)/implot.cpp          \
	$(IMPLOT_DIR)/implot_items.cpp

IMPLOT_OBJS := $(IMPLOT_SRCS:.cpp=.o)
IMPLOT_DEPS := $(IMPLOT_OBJS:.o=.d)

OUR_SRCS   := cxxabi_stubs.cpp      \
			  console_log.cpp       \
			  imgui_impl_vk.cpp     \
			  kobj_panel.cpp        \
			  launch_registry.cpp   \
			  main.cpp              \
			  shell_ui.cpp          \
			  task_manager_panel.cpp \
			  window_manager.cpp
OUR_OBJS   := $(OUR_SRCS:.cpp=.o)
OUR_DEPS   := $(OUR_OBJS:.o=.d)

ALL_OBJS   := $(IMGUI_OBJS) $(IMPLOT_OBJS) $(OUR_OBJS)
ALL_DEPS   := $(IMGUI_DEPS) $(IMPLOT_DEPS) $(OUR_DEPS)

TARGET     := vgui.vbin

# ---- Phony targets ------------------------------------------------------

.PHONY: all clean distclean help libc_build

all: _check_imgui _check_implot _check_sysroot $(TARGET)

help:
	@echo "vgui build targets:"
	@echo "  make              – release build"
	@echo "  make DEBUG=1      – debug build"
	@echo "  make clean        – remove objects and binary"
	@echo "  make distclean    – also remove downloaded ImGui sources"
	@echo ""
	@echo "First-time setup:"
	@echo "  bash setup_imgui.sh   – download Dear ImGui sources"
	@echo "  bash setup_implot.sh  – download ImPlot sources"
	@echo "  make -C ../libc       – build newlib glue (crt0, libvksys)"

# ---- Dependency guards --------------------------------------------------

_check_imgui:
	@test -f $(IMGUI_DIR)/imgui.h || \
	    { echo ""; \
	      echo "  ERROR: ImGui sources not found in $(IMGUI_DIR)/"; \
	      echo "  Run:  bash setup_imgui.sh"; \
	      echo ""; exit 1; }

_check_implot:
	@test -f $(IMPLOT_DIR)/implot.h || \
	    { echo ""; \
	      echo "  ERROR: ImPlot sources not found in $(IMPLOT_DIR)/"; \
	      echo "  Run:  bash setup_implot.sh"; \
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
	@$(CXX) $(IMGUI_CXXFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(IMPLOT_DIR)/%.o: $(IMPLOT_DIR)/%.cpp
	@echo "  CXX [implot] $(<F)"
	@$(CXX) $(IMPLOT_CXXFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

%.o: %.cpp
	@echo "  CXX         $<"
	@$(CXX) $(CXXFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

libc_build:
	@$(MAKE) -C $(LIBC_DIR) $(LIBC_MAKE_ARGS)

$(CRT0) $(LIBVKSYS): libc_build
	@:

# ---- Link ---------------------------------------------------------------

$(TARGET): $(CRT0) $(LIBVKSYS) $(ALL_OBJS)
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
	@rm -f $(TARGET) $(ALL_OBJS) $(ALL_DEPS)
	@echo "  cleaned."

distclean: clean
	@rm -rf imgui/
	@rm -rf implot/
	@echo "  imgui/ and implot/ removed."

-include $(ALL_DEPS)
