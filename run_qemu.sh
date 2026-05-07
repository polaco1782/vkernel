#!/bin/bash
# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# run_qemu.sh - Run vkernel in QEMU

BUILD_DIR="build"
EFI_FILE="${BUILD_DIR}/vkernel.efi"
BOOT_IMG="${BUILD_DIR}/vkernel_boot.img"
NVRAM_FILE="ovmf_vars.fd"
DEBUG_QEMU=0
VERBOSE=0

set -x
set -e

# Parse args
for arg in "$@"; do
    case "$arg" in
        --debug|-d) DEBUG_QEMU=1 ;;
        --verbose|-d) VERBOSE=1 ;;
    esac
done

if [ "${DEBUG_QEMU}" -eq 1 ] || [ "${VERBOSE}" -eq 1 ]; then
    make DEBUG=1 disk
else
    make disk
fi

if [ ! -f "${EFI_FILE}" ]; then
    echo "Error: EFI file not found. Run: make"
    exit 1
fi

if [ ! -f "${BOOT_IMG}" ]; then
    echo "Error: boot image not found. Run: make disk"
    exit 1
fi

#QEMU="qemu-system-x86_64"
QEMU="qemu-kvm"
OVMF_CODE="/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2"
OVMF_VARS="/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2"

# Fall back to 2M variant
if [ ! -f "${OVMF_CODE}" ]; then
    OVMF_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
    OVMF_VARS="/usr/share/edk2/ovmf/OVMF_VARS.fd"
fi
if [ ! -f "${OVMF_CODE}" ]; then
    OVMF_CODE="/usr/share/OVMF/OVMF_CODE.fd"
    OVMF_VARS="/usr/share/OVMF/OVMF_VARS.fd"
fi
if [ ! -f "${OVMF_CODE}" ]; then
    echo "Error: OVMF firmware not found. Install: dnf install edk2-ovmf  # or: apt install ovmf"
    exit 1
fi

# Detect pflash format (qcow2 or raw)
PFLASH_FMT="raw"
case "${OVMF_CODE}" in
    *.qcow2) PFLASH_FMT="qcow2" ;;
esac

# Writable OVMF_VARS
cp "${OVMF_VARS}" "${NVRAM_FILE}"

# Debug?
DEBUG_ARGS=""
if [ "${DEBUG_QEMU}" = "1" ]; then
    DEBUG_ARGS="-s -S"
    echo "GDB debug workflow:"
    echo "  1. gdb ${BUILD_DIR}/vkernel.efi \\"
    echo "       -ex 'set confirm off' \\"
    echo "       -ex 'set breakpoint pending on' \\"
    echo "       -ex 'source .vscode/find_kernel.py' \\"
    echo "       -ex 'target remote localhost:1234'"
    echo "  2. (continue) -> let UEFI load the kernel"
    echo "  3. (pause) -> then run: find-kernel"
    echo "  4. Set breakpoints by name (e.g. break vk::efi_main)"
fi

echo ""
echo "Running vkernel in QEMU..."
echo "Press Ctrl+Alt+2 to switch to QEMU monitor"
echo "Press Ctrl+Alt+1 to switch back to VM"
echo "Type 'quit' in QEMU monitor to exit"
echo ""
echo "Mouse: press Ctrl+Alt+G to grab/release the mouse inside the VM."
echo ""

exec ${QEMU} \
    -display sdl \
    -machine q35 \
    -vga virtio \
    -cpu host \
    -smp 4 \
    -drive if=pflash,format=${PFLASH_FMT},readonly=on,file="${OVMF_CODE}" \
    -drive if=pflash,format=${PFLASH_FMT},file="${NVRAM_FILE}" \
    -drive if=none,id=bootdisk,format=raw,file="${BOOT_IMG}" \
    -device virtio-blk-pci,drive=bootdisk,bootindex=0,disable-modern=off,disable-legacy=off \
    -m 512M \
    -net none \
    -device AC97 \
    -serial stdio \
    -no-reboot \
    -no-shutdown \
    ${DEBUG_ARGS}
