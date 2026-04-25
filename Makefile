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

# Userspace programs
USERSPACE_DIR  := userspace
LIBC_DIR       := $(USERSPACE_DIR)/libc
SYSROOT_DIR    := $(USERSPACE_DIR)/sysroot
HELLO_VBIN      := $(USERSPACE_DIR)/hello/hello.vbin
FRAMEBUFFER_VBIN := $(USERSPACE_DIR)/framebuffer/framebuffer.vbin
FRAMEBUFFER_TEXT_VBIN := $(USERSPACE_DIR)/framebuffer_text/framebuffer_text.vbin
RAYTRACER_VBIN := $(USERSPACE_DIR)/raytracer/raytracer.vbin
RAMFS_READER_VBIN := $(USERSPACE_DIR)/ramfs_reader/ramfs_reader.vbin
SHELL_VBIN      := $(USERSPACE_DIR)/shell/shell.vbin
DOOM_VBIN       := $(USERSPACE_DIR)/doom/doom.vbin
MODPLAY_VBIN    := $(USERSPACE_DIR)/MODPlay/modplay.vbin
ROTOZOOM_VBIN    := $(USERSPACE_DIR)/rotozoom/rotozoom.vbin
USERSPACE_BINARIES := $(HELLO_VBIN) $(FRAMEBUFFER_VBIN) $(FRAMEBUFFER_TEXT_VBIN) $(RAYTRACER_VBIN) $(RAMFS_READER_VBIN) $(SHELL_VBIN) $(DOOM_VBIN) $(MODPLAY_VBIN) $(ROTOZOOM_VBIN)

# Toolchain
CROSS_PREFIX ?= x86_64-redhat-linux-
CXX := $(CROSS_PREFIX)g++
LD := ld
OBJCOPY := objcopy
OBJDUMP := objdump

# Compiler flags
CXXFLAGS := -Wall -Wextra -Werror
CXXFLAGS += -nostdlib -nostdinc -fno-builtin -fno-stack-protector
CXXFLAGS += -fno-exceptions -fno-rtti
CXXFLAGS += -mno-red-zone -mno-mmx -mno-sse -mno-sse2
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

# Linker flags — static link; -fpic gives RIP-relative code; reloc_stub.cpp
# injects the .reloc section that clears IMAGE_FILE_RELOCS_STRIPPED.
# --no-relax prevents the linker from relaxing RIP-relative references to
# absolute addresses, which is critical for a PE loaded at arbitrary bases.
LDFLAGS := -nostdlib --no-relax
LDFLAGS += -T src/boot/linker.ld

# Source files
CXX_SRCS := $(wildcard src/boot/*.cpp)
CXX_SRCS += $(wildcard src/core/*.cpp)
CXX_SRCS += $(wildcard src/arch/x86_64/*.cpp)

# Object files
CXX_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/obj/%.o,$(CXX_SRCS))

# Assembly files
ASM_SRCS := $(wildcard src/arch/x86_64/*.S)
ASM_OBJS := $(patsubst src/%.S,$(BUILD_DIR)/obj/%.o,$(ASM_SRCS))

ALL_OBJS := $(CXX_OBJS) $(ASM_OBJS)

# Default target
all: $(EFI_FILE)

# Create build directories
$(BUILD_DIR) $(BUILD_DIR)/obj $(BUILD_DIR)/obj/boot $(BUILD_DIR)/obj/core $(BUILD_DIR)/obj/arch $(BUILD_DIR)/obj/arch/x86_64:
	@mkdir -p $@

# Compile C++ files
$(BUILD_DIR)/obj/%.o: src/%.cpp | $(BUILD_DIR) $(BUILD_DIR)/obj $(BUILD_DIR)/obj/boot $(BUILD_DIR)/obj/core $(BUILD_DIR)/obj/arch $(BUILD_DIR)/obj/arch/x86_64
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile assembly files
$(BUILD_DIR)/obj/%.o: src/%.S | $(BUILD_DIR) $(BUILD_DIR)/obj $(BUILD_DIR)/obj/boot $(BUILD_DIR)/obj/core $(BUILD_DIR)/obj/arch $(BUILD_DIR)/obj/arch/x86_64
	@echo "  ASM     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Link ELF kernel
$(BUILD_DIR)/$(KERNEL_NAME).elf: $(ALL_OBJS)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $^

# Convert to PE/COFF EFI application and stage into ESP directory
# DWARF debug sections (VMA=0) must be stripped from the PE image or the
# UEFI firmware loader will reject / misparse the binary.  The unstripped
# .elf is kept alongside for GDB / QEMU symbol loading.
$(EFI_FILE): $(BUILD_DIR)/$(KERNEL_NAME).elf
	@echo "  OBJCOPY $@"
	@$(OBJCOPY) -O efi-app-x86_64 \
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
		$< $@
	@mkdir -p $(ESP_DIR)/EFI/BOOT
	@cp $@ $(ESP_DIR)/EFI/BOOT/bootx64.efi
	@echo ""
	@echo "Build complete: $@"
	@ls -lh $@

# Create bootable GPT + EFI System Partition disk image
$(BOOT_IMG): $(EFI_FILE) $(USERSPACE_BINARIES) scripts/make_disk.sh
	@echo "  DISK    $@"
	@bash scripts/make_disk.sh $(EFI_FILE) $@ $(USERSPACE_BINARIES)

# Build all userspace binaries
.PHONY: userspace libc-glue newlib-setup
userspace: $(USERSPACE_BINARIES)

# newlib sysroot (headers + libc.a/libm.a) — run once
newlib-setup:
	@if [ ! -f $(SYSROOT_DIR)/lib/libc.a ]; then \
		echo "  NEWLIB  Building sysroot..."; \
		bash scripts/setup_newlib.sh; \
	else \
		echo "  NEWLIB  sysroot already built"; \
	fi

# Pass DEBUG down to sub-makes so userspace also gets debug/release flags.
_DEBUG_FLAG := $(if $(DEBUG),DEBUG=$(DEBUG),)

# CRT glue library (crt0.o + libvksys.a) — depends on sysroot
libc-glue: newlib-setup
	@$(MAKE) --no-print-directory -C $(LIBC_DIR) CC=$(CROSS_PREFIX)gcc $(_DEBUG_FLAG)

# hello.vbin depends on the CRT glue (newlib-based program)
$(HELLO_VBIN): $(USERSPACE_DIR)/hello/hello.c $(USERSPACE_DIR)/hello/Makefile libc-glue
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/hello CC=$(CROSS_PREFIX)gcc $(_DEBUG_FLAG)

$(FRAMEBUFFER_VBIN): $(USERSPACE_DIR)/framebuffer/framebuffer.c $(USERSPACE_DIR)/framebuffer/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer $(_DEBUG_FLAG)

$(FRAMEBUFFER_TEXT_VBIN): $(USERSPACE_DIR)/framebuffer_text/framebuffer_text.c $(USERSPACE_DIR)/framebuffer_text/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer_text $(_DEBUG_FLAG)

$(RAYTRACER_VBIN): $(USERSPACE_DIR)/raytracer/raytracer.c $(USERSPACE_DIR)/raytracer/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/raytracer $(_DEBUG_FLAG)

$(RAMFS_READER_VBIN): $(USERSPACE_DIR)/ramfs_reader/ramfs_reader.cpp $(USERSPACE_DIR)/ramfs_reader/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/ramfs_reader $(_DEBUG_FLAG)

$(SHELL_VBIN): $(USERSPACE_DIR)/shell/shell.c $(USERSPACE_DIR)/shell/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/shell $(_DEBUG_FLAG)

$(DOOM_VBIN): $(USERSPACE_DIR)/doom/Makefile libc-glue
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/doom CC=$(CROSS_PREFIX)gcc $(_DEBUG_FLAG)

$(MODPLAY_VBIN): $(USERSPACE_DIR)/MODPlay/Makefile libc-glue
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/MODPlay CC=$(CROSS_PREFIX)gcc $(_DEBUG_FLAG)

$(ROTOZOOM_VBIN): $(USERSPACE_DIR)/rotozoom/Makefile libc-glue
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/rotozoom CC=$(CROSS_PREFIX)gcc $(_DEBUG_FLAG)

# Disassembly for debugging
disasm: $(BUILD_DIR)/$(KERNEL_NAME).elf
	@$(OBJDUMP) -d $< > $(BUILD_DIR)/$(KERNEL_NAME).dis
	@echo "Disassembly written to $(BUILD_DIR)/$(KERNEL_NAME).dis"

# Clean build artifacts
clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@$(MAKE) --no-print-directory -C $(LIBC_DIR) clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/hello clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer_text clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/raytracer clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/ramfs_reader clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/shell clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/doom clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/MODPlay clean

# Deep clean — also remove newlib sysroot and source (requires re-running setup_newlib.sh)
distclean: clean
	@echo "Removing newlib sysroot and build..."
	@$(MAKE) --no-print-directory -C $(LIBC_DIR) distclean
	@bash scripts/setup_newlib.sh clean 2>/dev/null || true
	@rm -rf $(SYSROOT_DIR)

# QEMU test
qemu: $(BOOT_IMG)
	@echo "Running in QEMU..."
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
		-drive if=ide,format=raw,file=$(BOOT_IMG) \
		-m 512M \
		-net none \
		-serial stdio \
		-display default,show-cursor=off \
		-no-reboot \
		-no-shutdown

# QEMU with debug
qemu-debug: $(BOOT_IMG)
	@echo "Running in QEMU with GDB server..."
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
		-drive if=ide,format=raw,file=$(BOOT_IMG) \
		-m 512M \
		-net none \
		-serial stdio \
		-display default,show-cursor=off \
		-no-reboot \
		-no-shutdown \
		-s -S

# Show build info
info:
	@echo "Build Configuration:"
	@echo "  CXX:          $(CXX)"
	@echo "  CXXFLAGS:     $(CXXFLAGS)"
	@echo "  LDFLAGS:      $(LDFLAGS)"
	@echo "  Sources:      $(CXX_SRCS)"
	@echo "  Objects:      $(ALL_OBJS)"

# Phony targets
.PHONY: all clean distclean disasm qemu qemu-debug info userspace disk newlib-setup libc-glue

disk: $(BOOT_IMG)
