# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# Makefile - GCC/Clang C++26 build system

# Configuration
KERNEL_NAME := vkernel
BUILD_DIR := build
EFI_FILE := $(BUILD_DIR)/$(KERNEL_NAME).efi
ESP_DIR   := $(BUILD_DIR)/esp
BOOT_IMG  := $(BUILD_DIR)/$(KERNEL_NAME)_boot.img
SYMBOLS_DIR := $(BUILD_DIR)/symbols

# Userspace build delegation
USERSPACE_DIR := userspace
USERSPACE_STAMP := $(USERSPACE_DIR)/.build/userspace$(if $(DEBUG),-debug,).stamp

include $(USERSPACE_DIR)/artifacts.mk

# Toolchain
CROSS_PREFIX ?= x86_64-redhat-linux-
CXX := $(CROSS_PREFIX)g++
LD := ld
OBJCOPY := objcopy
OBJDUMP := objdump
NM := nm

USERSPACE_MAKE_ARGS := ROOT_DIR=$(CURDIR) ROOT_BUILD_DIR=$(abspath $(BUILD_DIR)) CROSS_PREFIX=$(CROSS_PREFIX) NM=$(NM) $(if $(DEBUG),DEBUG=$(DEBUG),)
USERSPACE_BINARIES := $(addprefix $(USERSPACE_DIR)/,$(USERSPACE_BINARY_RELATIVE))

# Text symbol maps live under build/symbols/<original-path>.map.
symbol_map_target = $(SYMBOLS_DIR)/$(1).map
line_map_target = $(SYMBOLS_DIR)/$(1).lines
KERNEL_SYMBOL_MAP := $(call symbol_map_target,$(BUILD_DIR)/$(KERNEL_NAME).elf)
KERNEL_LINE_MAP := $(call line_map_target,$(BUILD_DIR)/$(KERNEL_NAME).elf)
USERSPACE_LINE_MAPS := $(foreach bin,$(USERSPACE_BINARY_RELATIVE),$(call line_map_target,$(USERSPACE_DIR)/$(bin)))
KERNEL_BUILD_CONFIG_STAMP := $(BUILD_DIR)/.kernel_build_config$(if $(DEBUG),.debug,.release)$(if $(GDB_WAIT),.gdbwait,)

USERSPACE_STAGE_INPUTS := $(USERSPACE_BINARIES)
ifdef DEBUG
USERSPACE_STAGE_INPUTS += $(USERSPACE_LINE_MAPS)
endif

# Compiler flags
CXXFLAGS := -Wall -Wextra -Werror
CXXFLAGS += -nostdlib -nostdinc -fno-builtin -fno-stack-protector
CXXFLAGS += -fno-exceptions -fno-rtti
CXXFLAGS += -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-3dnow -mno-avx
CXXFLAGS += -I$(CURDIR)/include
CXXFLAGS += -I$(CURDIR)/include/vkernel
CXXFLAGS += -ffreestanding
CXXFLAGS += -fpic
CXXFLAGS += -std=c++26
CXXFLAGS += -Wno-unused-parameter
CXXFLAGS += -Wno-unused-variable

# Debug flags
ifdef DEBUG
	CXXFLAGS += -g -O0 -fno-omit-frame-pointer -DDEBUG -DKERNEL_DEBUG=1
else
	CXXFLAGS += -O2 -DNDEBUG
endif

ifdef GDB_WAIT
	CXXFLAGS += -DKERNEL_GDB_WAIT=1
endif

# Linker flags — static link; -fpic gives RIP-relative code; reloc_stub.cpp
# injects the .reloc section that clears IMAGE_FILE_RELOCS_STRIPPED.
# --no-relax prevents the linker from relaxing RIP-relative references to
# absolute addresses, which is critical for a PE loaded at arbitrary bases.
LDFLAGS := -nostdlib --no-relax
LDFLAGS += -T src/boot/linker.ld

# Source files
CXX_SRCS := $(wildcard src/boot/*.cpp)
CXX_SRCS += $(wildcard src/core/*.cpp)
CXX_SRCS += $(wildcard src/core/kobj/*.cpp)
CXX_SRCS += $(wildcard src/fs/*.cpp)
CXX_SRCS += $(wildcard src/net/*.cpp)
CXX_SRCS += $(wildcard src/drivers/*.cpp)
CXX_SRCS += $(wildcard src/arch/x86_64/*.cpp)

# Object files
CXX_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/obj/%.o,$(CXX_SRCS))

# Assembly files
ASM_SRCS := $(wildcard src/arch/x86_64/*.S)
ASM_OBJS := $(patsubst src/%.S,$(BUILD_DIR)/obj/%.o,$(ASM_SRCS))

ALL_OBJS := $(CXX_OBJS) $(ASM_OBJS)

.DELETE_ON_ERROR:

# Default target
all: $(EFI_FILE)

# Create build directories
$(BUILD_DIR) $(BUILD_DIR)/obj $(BUILD_DIR)/obj/boot $(BUILD_DIR)/obj/core $(BUILD_DIR)/obj/fs $(BUILD_DIR)/obj/drivers $(BUILD_DIR)/obj/arch $(BUILD_DIR)/obj/arch/x86_64:
	@mkdir -p $@

$(KERNEL_BUILD_CONFIG_STAMP): Makefile | $(BUILD_DIR)
	@tmp="$@.tmp"; rm -f "$$tmp"; \
	{ \
		printf 'CXX=%s\n' '$(CXX)'; \
		printf 'CXXFLAGS=%s\n' '$(CXXFLAGS)'; \
		printf 'LDFLAGS=%s\n' '$(LDFLAGS)'; \
		printf 'DEBUG=%s\n' '$(if $(DEBUG),$(DEBUG),0)'; \
		printf 'GDB_WAIT=%s\n' '$(if $(GDB_WAIT),$(GDB_WAIT),0)'; \
	} > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@" 2>/dev/null; then \
		mv -f "$$tmp" "$@"; \
	else \
		rm -f "$$tmp"; \
	fi

# Compile C++ files
$(BUILD_DIR)/obj/%.o: src/%.cpp $(KERNEL_BUILD_CONFIG_STAMP) | $(BUILD_DIR) $(BUILD_DIR)/obj $(BUILD_DIR)/obj/boot $(BUILD_DIR)/obj/core $(BUILD_DIR)/obj/fs $(BUILD_DIR)/obj/drivers $(BUILD_DIR)/obj/arch $(BUILD_DIR)/obj/arch/x86_64
	@echo "  CXX     $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile assembly files
$(BUILD_DIR)/obj/%.o: src/%.S $(KERNEL_BUILD_CONFIG_STAMP) | $(BUILD_DIR) $(BUILD_DIR)/obj $(BUILD_DIR)/obj/boot $(BUILD_DIR)/obj/core $(BUILD_DIR)/obj/fs $(BUILD_DIR)/obj/drivers $(BUILD_DIR)/obj/arch $(BUILD_DIR)/obj/arch/x86_64
	@echo "  ASM     $<"
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Link ELF kernel
$(BUILD_DIR)/$(KERNEL_NAME).elf: $(ALL_OBJS)
	@echo "  LD      $@"
	@tmp="$@.tmp"; rm -f "$$tmp"; \
	trap 'rm -f "$$tmp"' EXIT INT TERM; \
	$(LD) $(LDFLAGS) -o "$$tmp" $^; \
	mv -f "$$tmp" "$@"; \
	trap - EXIT INT TERM

# Emit a sorted text symbol map for addr2line / post-mortem work.
$(SYMBOLS_DIR)/%.map: %
	@echo "  MAP     $@"
	@mkdir -p $(dir $@)
	@$(NM) -n -C --defined-only $< > $@

# Emit a compact userspace file/line lookup table for crash backtraces.
$(SYMBOLS_DIR)/%.lines: % scripts/generate_line_map.sh
	@echo "  LINES   $@"
	@mkdir -p $(dir $@)
	@bash scripts/generate_line_map.sh $< $@

# Convert to PE/COFF EFI application and stage into ESP directory
# DWARF debug sections (VMA=0) must be stripped from the PE image or the
# UEFI firmware loader will reject / misparse the binary.  The unstripped
# .elf is kept alongside for GDB / QEMU symbol loading.
ifdef DEBUG
$(EFI_FILE): | $(KERNEL_SYMBOL_MAP) $(KERNEL_LINE_MAP)
endif

$(EFI_FILE): $(BUILD_DIR)/$(KERNEL_NAME).elf
	@echo "  OBJCOPY $@"
	@tmp="$@.tmp"; rm -f "$$tmp"; \
	trap 'rm -f "$$tmp"' EXIT INT TERM; \
	$(OBJCOPY) -O efi-app-x86_64 \
		--remove-section=.debug_info \
		--remove-section=.debug_abbrev \
		--remove-section=.debug_aranges \
		--remove-section=.debug_ranges \
		--remove-section=.debug_rnglists \
		--remove-section=.debug_loclists \
		--remove-section=.debug_line \
		--remove-section=.debug_line_str \
		--remove-section=.debug_str \
		--remove-section=.debug_macro \
		--remove-section=.debug_loc \
		--remove-section=.debug_frame \
		$< "$$tmp"; \
	mv -f "$$tmp" "$@"; \
	trap - EXIT INT TERM
	@mkdir -p $(ESP_DIR)/EFI/BOOT
	@cp $@ $(ESP_DIR)/EFI/BOOT/bootx64.efi
	@echo ""
	@echo "Build complete: $@"
	@ls -lh $@

# Create bootable GPT + EFI System Partition disk image
BOOT_DEBUG_FILES := $(KERNEL_SYMBOL_MAP)
ifdef DEBUG
BOOT_DEBUG_FILES += $(KERNEL_LINE_MAP)
endif

$(USERSPACE_STAMP): | userspace
	@test -f $@

$(USERSPACE_STAGE_INPUTS): $(USERSPACE_STAMP)
	@test -e $@

$(BOOT_IMG): $(EFI_FILE) scripts/make_disk.sh $(USERSPACE_STAMP) $(USERSPACE_STAGE_INPUTS) $(BOOT_DEBUG_FILES)
	@echo "  DISK    $@"
	@bash scripts/make_disk.sh $(EFI_FILE) $@ $(BOOT_DEBUG_FILES)

# Delegate userspace builds to userspace/Makefile
userspace:
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR) $(USERSPACE_MAKE_ARGS) all

# Disassembly for debugging
disasm: $(BUILD_DIR)/$(KERNEL_NAME).elf
	@$(OBJDUMP) -d $< > $(BUILD_DIR)/$(KERNEL_NAME).dis
	@echo "Disassembly written to $(BUILD_DIR)/$(KERNEL_NAME).dis"

userspace-clean:
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR) $(USERSPACE_MAKE_ARGS) clean

# Clean build artifacts
clean: userspace-clean
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)

# Deep clean — also remove newlib sysroot and source (requires re-running setup_newlib.sh)
distclean: clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR) $(USERSPACE_MAKE_ARGS) distclean

# Show build info
info:
	@echo "Build Configuration:"
	@echo "  CXX:          $(CXX)"
	@echo "  CXXFLAGS:     $(CXXFLAGS)"
	@echo "  LDFLAGS:      $(LDFLAGS)"
	@echo "  Symbol maps:  $(if $(DEBUG),$(SYMBOLS_DIR),disabled)"
	@echo "  Sources:      $(CXX_SRCS)"
	@echo "  Objects:      $(ALL_OBJS)"

# Phony targets
.PHONY: all clean distclean disasm qemu qemu-debug info userspace userspace-clean disk

disk: $(BOOT_IMG)
