CXX     ?= g++
AR      ?= ar
STRIP   ?= strip

ROOT_DIR        := $(realpath $(CURDIR)/../..)
USER_INCLUDE    := ../include
SYSROOT         := ../sysroot
LIBC_DIR        := ../libc
CPP_DIR         := ../cpp
CPP_INCLUDE     := $(CPP_DIR)/include
KERNEL_INCLUDE  := $(ROOT_DIR)/include
VGUI_DIR        := ../vgui
IMGUI_DIR       := $(VGUI_DIR)/imgui
BUILD_DIR       := build

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
CXXFLAGS  += -I$(VGUI_DIR)
CXXFLAGS  += -I$(IMGUI_DIR)

IMGUI_CXXFLAGS := $(CXXFLAGS)
IMGUI_CXXFLAGS += -Wno-unused-parameter
IMGUI_CXXFLAGS += -Wno-missing-field-initializers
IMGUI_CXXFLAGS += -Wno-sign-compare
IMGUI_CXXFLAGS += -Wno-type-limits
IMGUI_CXXFLAGS += -Wno-misleading-indentation
IMGUI_CXXFLAGS += -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS

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
LIBVKSYS        := $(LIBC_DIR)/libvksys.a
LIBS            := -Wl,--start-group -lvkcpp -lvksys -lc -lm -lgcc -Wl,--end-group

CPP_MAKE_ARGS :=
ifdef DEBUG
CPP_MAKE_ARGS += DEBUG=$(DEBUG)
endif

LIBC_MAKE_ARGS :=
ifdef DEBUG
LIBC_MAKE_ARGS += DEBUG=$(DEBUG)
endif

CORE_SRCS := apu.cpp       \
			 app.cpp       \
			 browser.cpp   \
			 bus.cpp       \
			 cartridge.cpp \
			 cpu.cpp       \
			 hq2x.cpp      \
			 hqx.cpp       \
			 input.cpp     \
			 main.cpp      \
			 mapper.cpp    \
			 mapper_000.cpp \
			 mapper_001.cpp \
			 mapper_002.cpp \
			 mapper_004.cpp \
			 mapper_009.cpp \
			 ppu.cpp       \
			 sound.cpp     \
			 ui.cpp

IMGUI_SRCS := $(IMGUI_DIR)/imgui.cpp      \
		      $(IMGUI_DIR)/imgui_draw.cpp \
		      $(IMGUI_DIR)/imgui_tables.cpp \
		      $(IMGUI_DIR)/imgui_widgets.cpp

VGUI_SRCS := $(VGUI_DIR)/imgui_impl_vk.cpp

CORE_OBJS  := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CORE_SRCS))
IMGUI_OBJS := $(patsubst $(IMGUI_DIR)/%.cpp,$(BUILD_DIR)/imgui/%.o,$(IMGUI_SRCS))
VGUI_OBJS  := $(patsubst $(VGUI_DIR)/%.cpp,$(BUILD_DIR)/vgui/%.o,$(VGUI_SRCS))

ALL_OBJS := $(CORE_OBJS) $(IMGUI_OBJS) $(VGUI_OBJS)
ALL_DEPS := $(ALL_OBJS:.o=.d)

TARGET := vnes.vbin

.PHONY: all clean libc_build cpp_build

all: _check_imgui _check_sysroot $(TARGET)

_check_imgui:
	@test -f $(IMGUI_DIR)/imgui.h || \
	    { echo ""; \
	      echo "  ERROR: ImGui sources not found in $(IMGUI_DIR)/"; \
	      echo "  Run:  bash ../vgui/setup_imgui.sh"; \
	      echo ""; exit 1; }

_check_sysroot:
	@test -f $(SYSROOT)/include/stdio.h || \
	    { echo ""; \
	      echo "  ERROR: newlib sysroot not found at $(SYSROOT)"; \
	      echo "  Run:  bash ../../scripts/setup_newlib.sh"; \
	      echo ""; exit 1; }

$(BUILD_DIR)/%.o: %.cpp
	@echo "  CXX     $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/imgui/%.o: $(IMGUI_DIR)/%.cpp
	@echo "  CXX [imgui] $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(IMGUI_CXXFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/vgui/%.o: $(VGUI_DIR)/%.cpp
	@echo "  CXX [vgui]  $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(IMGUI_CXXFLAGS) $(DEPFLAGS) -MF $(@:.o=.d) -c $< -o $@

libc_build:
	@$(MAKE) -C $(LIBC_DIR) $(LIBC_MAKE_ARGS)

cpp_build:
	@$(MAKE) --no-print-directory -C $(CPP_DIR) CXX=$(CXX) AR=$(AR) $(CPP_MAKE_ARGS)

$(CRT0) $(LIBVKSYS): libc_build
	@:

$(CPP_RUNTIME_LIB): cpp_build
	@:

$(TARGET): $(CRT0) $(LIBVKSYS) $(CPP_RUNTIME_LIB) $(ALL_OBJS)
	@echo "  LD      $@"
	@$(CXX) $(LDFLAGS) -o $@ $(CRT0) $(ALL_OBJS) $(LIBS)
ifndef DEBUG
	@echo "  STRIP   $@"
	@$(STRIP) --strip-unneeded $@
endif
	@echo "  SIZE    $@"
	@size $@ 2>/dev/null || true

clean:
	@rm -rf $(BUILD_DIR) $(TARGET)

-include $(ALL_DEPS)