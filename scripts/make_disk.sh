#!/bin/bash
# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# make_disk.sh - Create a bootable UEFI disk image
#
# Creates a 64 MiB raw disk image with:
#   - GPT partition table
#   - EFI System Partition (62 MiB, FAT32)
#   - EFI/BOOT/bootx64.efi at the default removable-media boot path
#
# Usage: make_disk.sh <efi_file> <output_image> [elf_file ...]
# Requires: truncate, parted, mformat, mmd, mcopy  (mtools)

set -e

EFI_FILE="$1"
OUTPUT="$2"
shift 2
EXTRA_ELFS=("$@")   # remaining args are ELF binaries to stage under EFI/vkernel/

if [ -z "${EFI_FILE}" ] || [ -z "${OUTPUT}" ]; then
    echo "Usage: $0 <efi_file> <output_image> [elf_file ...]"
    exit 1
fi

if [ ! -f "${EFI_FILE}" ]; then
    echo "Error: EFI file not found: ${EFI_FILE}"
    exit 1
fi

for tool in truncate parted mformat mmd mcopy; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Error: '${tool}' not found."
        echo "Install with: dnf install parted mtools  # or: apt install parted mtools"
        exit 1
    fi
done

# ── Disk layout ────────────────────────────────────────────────────────────
#  Total disk  : 64 MiB = 131072 sectors
#  ESP         : 1 MiB → 63 MiB  (62 MiB, starts at sector 2048)
#  GPT backup  : last 33 sectors
#
#  FAT32 geometry for 62 MiB partition (62 × 64 × 32 = 126 976 sectors):
#    tracks=62  heads=64  sectors/track=32
# ───────────────────────────────────────────────────────────────────────────

DISK_MB=64
ESP_START_MiB=1
ESP_END_MiB=63
ESP_BYTE_OFFSET=$((ESP_START_MiB * 1024 * 1024))   # 1 048 576
ESP_TRACKS=62
ESP_HEADS=64
ESP_SECS=32

rm -f "${OUTPUT}"

echo "  Creating ${DISK_MB} MiB blank disk..."
truncate -s "${DISK_MB}M" "${OUTPUT}"

echo "  Writing GPT + EFI System Partition..."
parted -s "${OUTPUT}" mktable gpt
parted -s "${OUTPUT}" mkpart ESP fat32 "${ESP_START_MiB}MiB" "${ESP_END_MiB}MiB"
parted -s "${OUTPUT}" set 1 esp on

echo "  Formatting ESP as FAT32..."
mformat -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" -F \
    -h ${ESP_HEADS} -s ${ESP_SECS} -t ${ESP_TRACKS} ::

echo "  Staging EFI application..."
mmd    -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" ::/EFI ::/EFI/BOOT
mcopy  -o -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" "${EFI_FILE}" ::/EFI/BOOT/bootx64.efi

# add doom2.wad to extra ELF files to stage it as well
EXTRA_ELFS+=("userspace/doom/doom2.wad")

if [ ${#EXTRA_ELFS[@]} -gt 0 ]; then
    echo "  Staging userspace binaries... (${#EXTRA_ELFS[@]} ELF files)"
    mmd -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" ::/EFI/vkernel
    for elf in "${EXTRA_ELFS[@]}"; do
        name=$(basename "${elf}")
        echo "    ${name}"
        mcopy -o -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" "${elf}" "::/EFI/vkernel/${name}"
    done
fi

echo "  Done: ${OUTPUT}"
mdir -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" ::/EFI/BOOT 2>/dev/null || true
