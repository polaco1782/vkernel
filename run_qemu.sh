#!/bin/bash
# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# run_qemu.sh - Run vkernel in QEMU

set -e

BUILD_DIR="build"
EFI_FILE="${BUILD_DIR}/vkernel.efi"

if [ ! -f "${EFI_FILE}" ]; then
    echo "Error: EFI file not found. Run: make"
    exit 1
fi

QEMU="qemu-system-x86_64"
OVMF_CODE="/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2"
OVMF_VARS="/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2"
BOOT_IMG="${BUILD_DIR}/vkernel_boot.img"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)/scripts"

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

echo "Creating bootable UEFI disk image..."
bash "${SCRIPT_DIR}/make_disk.sh" "${EFI_FILE}" "${BOOT_IMG}"

# Writable OVMF_VARS
TEMP_VARS="/tmp/vkernel_ovmf_vars_$$.fd"
cp "${OVMF_VARS}" "${TEMP_VARS}"

# Detect pflash format (qcow2 or raw)
PFLASH_FMT="raw"
case "${OVMF_CODE}" in
    *.qcow2) PFLASH_FMT="qcow2" ;;
esac

# Debug?
DEBUG_ARGS=""
if [ "$1" = "--debug" ] || [ "$1" = "-d" ]; then
    DEBUG_ARGS="-s -S"
    echo "GDB: gdb -ex 'target remote localhost:1234' ${BUILD_DIR}/vkernel.elf"
fi

# Serial only mode?
DISPLAY_ARGS="-device VGA -display gtk"
if [ "$1" = "--serial-only" ] || [ "$1" = "-s" ]; then
    DISPLAY_ARGS="-nographic"
fi

echo ""
echo "Running vkernel..."
echo "Ctrl+A X to exit"

exec ${QEMU} \
    -drive if=pflash,format=${PFLASH_FMT},readonly=on,file="${OVMF_CODE}" \
    -drive if=pflash,format=${PFLASH_FMT},file="${TEMP_VARS}" \
    -drive if=ide,format=raw,file="${BOOT_IMG}" \
    -m 512M \
    -net none \
    ${DISPLAY_ARGS} \
    -serial mon:stdio \
    -no-reboot \
    -no-shutdown \
    ${DEBUG_ARGS}
