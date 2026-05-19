#!/bin/bash
# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# make_disk.sh - Create a bootable UEFI disk image
#
# Creates a 128 MiB raw disk image with:
#   - GPT partition table
#   - EFI System Partition (126 MiB, FAT32)
#   - EFI/BOOT/bootx64.efi at the default removable-media boot path
#
# Usage: make_disk.sh <efi_file> <output_image> [elf_file ...]
# Requires: truncate, parted, mformat, mmd, mcopy  (mtools)

set -e

shopt -s nullglob

EFI_FILE="$1"
OUTPUT="$2"
shift 2

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
#  Total disk  : 128 MiB = 262144 sectors
#  ESP         : 1 MiB → 127 MiB (126 MiB, starts at sector 2048)
#  GPT backup  : last 33 sectors
#
#  FAT32 geometry for 126 MiB partition (126 × 64 × 32 = 258 048 sectors):
#    tracks=126  heads=64  sectors/track=32
# ───────────────────────────────────────────────────────────────────────────

DISK_MB=512
ESP_START_MiB=1
ESP_END_MiB=127
ESP_BYTE_OFFSET=$((ESP_START_MiB * 1024 * 1024))   # 1 048 576
ESP_TRACKS=126
ESP_HEADS=64
ESP_SECS=32

copy_into_esp() {
    local src="$1"
    local dest_name="$2"

    if [ ! -f "${src}" ]; then
        return 0
    fi

    echo "    ${dest_name}"
    mcopy -o -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" "${src}" "::/EFI/vkernel/${dest_name}"
}

# Recursively copy a source directory into ::/EFI/vkernel/<dest_dir>.
# Creates intermediate FAT directories as needed then bulk-copies each file.
copy_dir_into_esp() {
    local src_dir="$1"
    local dest_dir="$2"   # relative to ::/EFI/vkernel/

    [ -d "${src_dir}" ] || return 0

    local f rel dir fat_path
    while IFS= read -r -d '' f; do
        rel="${f#${src_dir}/}"
        dir="$(dirname "${rel}")"
        fat_path="::/EFI/vkernel/${dest_dir}"

        # Create any intermediate subdirectories (mmd is idempotent with -D).
        if [ "${dir}" != "." ]; then
            local part=""
            IFS='/' read -ra parts <<< "${dir}"
            for part in "${parts[@]}"; do
                fat_path="${fat_path}/${part}"
                mmd -D s -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" "${fat_path}" 2>/dev/null || true
            done
            fat_path="::/EFI/vkernel/${dest_dir}"
        fi

        echo "    ${dest_dir}/${rel}"
        mcopy -o -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" "${f}" \
              "::/EFI/vkernel/${dest_dir}/${rel}"
    done < <(find "${src_dir}" -type f -print0)
}

stage_clownmdemu_roms() {
    local rom
    for rom in userspace/clownmdemu/roms/*; do
        local base
        local ext

        [ -f "${rom}" ] || continue
        base=$(basename "${rom}")
        ext="${base##*.}"
        ext="${ext,,}"

        case "${ext}" in
            bin|gen|smd|32x|md)
                copy_into_esp "${rom}" "${base}"
                ;;
        esac
    done
}

stage_vnes_roms() {
    local rom
    for rom in userspace/vnes/roms/*; do
        local base
        local ext

        [ -f "${rom}" ] || continue
        base=$(basename "${rom}")
        ext="${base##*.}"
        ext="${ext,,}"

        case "${ext}" in
            nes)
                copy_into_esp "${rom}" "${base}"
                ;;
        esac
    done
}

stage_minimp3_tracks() {
    local track
    for track in userspace/minimp3/tracks/*; do
        local base
        local ext

        [ -f "${track}" ] || continue
        base=$(basename "${track}")
        ext="${base##*.}"
        ext="${ext,,}"

        case "${ext}" in
            mp3)
                copy_into_esp "${track}" "${base}"
                ;;
        esac
    done
}

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

echo "  Staging userspace binaries..."
mmd -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" ::/EFI/vkernel ::/EFI/vkernel/id1 ::/EFI/vkernel/zeusbot ::/EFI/vkernel/reaperfx

manifest_file=$(mktemp)
while IFS= read -r -d '' vbin; do
    name=$(basename "${vbin}")
    copy_into_esp "${vbin}" "${name}"
    printf '%s\n' "${name}" >> "${manifest_file}"
done < <(find userspace -name "*.vbin" -print0)

sort -u "${manifest_file}" -o "${manifest_file}"
copy_into_esp "${manifest_file}" "vgui_apps.txt"
rm -f "${manifest_file}"

copy_into_esp "userspace/doom/doom1.wad" "doom1.wad"
copy_into_esp "userspace/doom/doom2.wad" "doom2.wad"
copy_into_esp "userspace/shell/shell_exec.txt" "shell.txt"
copy_into_esp "userspace/MODPlay/makemove.mod" "makemove.mod"
copy_into_esp "userspace/MODPlay/UNREALPM.S3M" "UNREALPM.S3M"
copy_into_esp "userspace/rotozoom/head.bmp" "head.bmp"
copy_into_esp "userspace/quake/pak0.pak" "id1/pak0.pak"
copy_into_esp "userspace/quake/progs.dat" "zeusbot/progs.dat"
copy_into_esp "userspace/quake/zeus_pak0.pak" "zeusbot/pak0.pak"

for extra in "$@"; do
    [ -f "${extra}" ] || continue
    copy_into_esp "${extra}" "$(basename "${extra}")"
done

echo "  Staging reaperfx..."
copy_dir_into_esp "userspace/quake/reaperfx" "reaperfx"
stage_clownmdemu_roms
stage_vnes_roms
stage_minimp3_tracks

echo "  Done: ${OUTPUT}"
mdir -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" ::/EFI/BOOT 2>/dev/null || true
