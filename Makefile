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
HELLO_ELF      := $(USERSPACE_DIR)/hello/hello.elf
FRAMEBUFFER_ELF := $(USERSPACE_DIR)/framebuffer/framebuffer.elf
FRAMEBUFFER_TEXT_ELF := $(USERSPACE_DIR)/framebuffer_text/framebuffer_text.elf
RAYTRACER_ELF := $(USERSPACE_DIR)/raytracer/raytracer.elf
RAMFS_READER_ELF := $(USERSPACE_DIR)/ramfs_reader/ramfs_reader.elf
SHELL_ELF      := $(USERSPACE_DIR)/shell/shell.elf
USERSPACE_ELFS := $(HELLO_ELF) $(FRAMEBUFFER_ELF) $(FRAMEBUFFER_TEXT_ELF) $(RAYTRACER_ELF) $(RAMFS_READER_ELF) $(SHELL_ELF)

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
CXXFLAGS += -O2
CXXFLAGS += -Wno-unused-parameter
CXXFLAGS += -Wno-unused-variable

# Debug flags
ifdef DEBUG
    CXXFLAGS += -g -DDEBUG -DKERNEL_DEBUG=1
else
    CXXFLAGS += -DNDEBUG
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
$(EFI_FILE): $(BUILD_DIR)/$(KERNEL_NAME).elf
	@echo "  OBJCOPY $@"
	@$(OBJCOPY) -O efi-app-x86_64 $< $@
	@mkdir -p $(ESP_DIR)/EFI/BOOT
	@cp $@ $(ESP_DIR)/EFI/BOOT/bootx64.efi
	@echo ""
	@echo "Build complete: $@"
	@ls -lh $@

# Create bootable GPT + EFI System Partition disk image
$(BOOT_IMG): $(EFI_FILE) $(USERSPACE_ELFS) scripts/make_disk.sh
	@echo "  DISK    $@"
	@bash scripts/make_disk.sh $(EFI_FILE) $@ $(USERSPACE_ELFS)

# Build all userspace ELF programs
.PHONY: userspace
userspace: $(USERSPACE_ELFS)

$(HELLO_ELF): $(USERSPACE_DIR)/hello/hello.c $(USERSPACE_DIR)/hello/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/hello

$(FRAMEBUFFER_ELF): $(USERSPACE_DIR)/framebuffer/framebuffer.c $(USERSPACE_DIR)/framebuffer/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer

$(FRAMEBUFFER_TEXT_ELF): $(USERSPACE_DIR)/framebuffer_text/framebuffer_text.c $(USERSPACE_DIR)/framebuffer_text/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer_text

$(RAYTRACER_ELF): $(USERSPACE_DIR)/raytracer/raytracer.c $(USERSPACE_DIR)/raytracer/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/raytracer

$(RAMFS_READER_ELF): $(USERSPACE_DIR)/ramfs_reader/ramfs_reader.c $(USERSPACE_DIR)/ramfs_reader/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/ramfs_reader

$(SHELL_ELF): $(USERSPACE_DIR)/shell/shell.c $(USERSPACE_DIR)/shell/Makefile
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/shell

# Disassembly for debugging
disasm: $(BUILD_DIR)/$(KERNEL_NAME).elf
	@$(OBJDUMP) -d $< > $(BUILD_DIR)/$(KERNEL_NAME).dis
	@echo "Disassembly written to $(BUILD_DIR)/$(KERNEL_NAME).dis"

# Clean build artifacts
clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/hello clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/framebuffer_text clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/raytracer clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/ramfs_reader clean
	@$(MAKE) --no-print-directory -C $(USERSPACE_DIR)/shell clean

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
.PHONY: all clean disasm qemu qemu-debug info userspace disk

disk: $(BOOT_IMG)
