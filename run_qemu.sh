#!/bin/bash
# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# run_qemu.sh - Run vkernel in QEMU

BUILD_DIR="build"
EFI_FILE="${BUILD_DIR}/vkernel.efi"
BOOT_IMG="${BUILD_DIR}/vkernel_boot.img"
NVRAM_FILE="ovmf_vars.fd"
TAP_IF="tap0"
DEBUG_QEMU=0
VERBOSE=0
NET_MODE="tap"

set -x
set -e

# Parse args
for arg in "$@"; do
    case "$arg" in
        --debug|-d) DEBUG_QEMU=1 ;;
        --verbose|-v) VERBOSE=1 ;;
        --keep-disk) KEEP_DISK=1 ;;
        --tap) NET_MODE="tap" ;;
        --usernet) NET_MODE="user" ;;
    esac
done

make clean

if [ -z "${KEEP_DISK}" ]; then
    if [ "${DEBUG_QEMU}" -eq 1 ]; then
        make DEBUG=1 GDB_WAIT=1 disk
    elif [ "${VERBOSE}" -eq 1 ]; then
        make DEBUG=1 disk
    else
        make disk
    fi
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
    echo "  2. continue once; the debug build will trap at kernel entry"
    echo "  3. symbols auto-load on that first stop"
    echo "  4. Set breakpoints by name or step from vk::efi_main"
fi

echo ""
echo "Running vkernel in QEMU..."
echo "Press Ctrl+Alt+2 to switch to QEMU monitor"
echo "Press Ctrl+Alt+1 to switch back to VM"
echo "Type 'quit' in QEMU monitor to exit"
echo ""
echo "Mouse: press Ctrl+Alt+G to grab/release the mouse inside the VM."
echo ""

NETDEV_ARGS="-netdev user,id=net0"
if [ "${NET_MODE}" = "tap" ]; then
    if ! ip link show "${TAP_IF}" >/dev/null 2>&1; then
        echo "Error: TAP interface '${TAP_IF}' not found."
        echo "Create it with:"
        echo "  sudo ip tuntap add dev ${TAP_IF} mode tap user \"$USER\""
        echo "  sudo ip link set ${TAP_IF} up"
        echo ""
        echo "Or run this script with --usernet to use QEMU user networking."
        exit 1
    fi

    NETDEV_ARGS="-netdev tap,id=net0,ifname=${TAP_IF},script=no,downscript=no"
fi

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
    ${NETDEV_ARGS} \
    -device virtio-net-pci,netdev=net0,disable-modern=off,disable-legacy=off \
    -m 512M \
    -device AC97 \
    -serial stdio \
    -no-reboot \
    -no-shutdown \
    ${DEBUG_ARGS}
