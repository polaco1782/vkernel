#!/bin/bash
# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# make_disk.sh - Create a bootable UEFI disk image
#
# Creates a 512 MiB raw GPT disk image with:
#   - EFI System Partition (126 MiB, FAT32) for EFI/BOOT and /boot
#   - FAT32 data partition (384 MiB) for /bin and /data
#
# Usage: make_disk.sh <efi_file> <output_image> [elf_file ...]
# Requires: truncate, parted, mkfs.fat, mmd, mcopy  (dosfstools + mtools)

set -e

shopt -s nullglob

EFI_FILE="$1"
OUTPUT="$2"
shift 2

IMAGE_PATH="${OUTPUT}"
TEMP_IMAGE=""

if [ -z "${EFI_FILE}" ] || [ -z "${OUTPUT}" ]; then
    echo "Usage: $0 <efi_file> <output_image> [elf_file ...]"
    exit 1
fi

if [ ! -f "${EFI_FILE}" ]; then
    echo "Error: EFI file not found: ${EFI_FILE}"
    exit 1
fi

for tool in truncate parted mkfs.fat mdir mmd mcopy; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Error: '${tool}' not found."
        echo "Install with: dnf install parted dosfstools mtools  # or: apt install parted dosfstools mtools"
        exit 1
    fi
done

cleanup_temp_image() {
    if [ -n "${TEMP_IMAGE}" ] && [ -f "${TEMP_IMAGE}" ]; then
        rm -f "${TEMP_IMAGE}"
    fi
}

trap cleanup_temp_image EXIT

# ── Disk layout ────────────────────────────────────────────────────────────
#  Total disk  : 512 MiB
#  ESP         : 1 MiB → 127 MiB  (126 MiB)
#  Data FAT32  : 127 MiB → 511 MiB (384 MiB)
#  GPT backup  : last 33 sectors
#
#  FAT32 geometry uses 64 heads × 32 sectors/track, so tracks == partition MiB.
# ───────────────────────────────────────────────────────────────────────────

DISK_MB=512
ESP_START_MiB=1
ESP_SIZE_MiB=126
ESP_END_MiB=$((ESP_START_MiB + ESP_SIZE_MiB))
DATA_START_MiB=${ESP_END_MiB}
DATA_END_MiB=511
DATA_SIZE_MiB=$((DATA_END_MiB - DATA_START_MiB))

SECTORS_PER_MiB=2048
ESP_BYTE_OFFSET=$((ESP_START_MiB * 1024 * 1024))   # 1 048 576
DATA_BYTE_OFFSET=$((DATA_START_MiB * 1024 * 1024))
FAT_HEADS=64
FAT_SECS=32
ESP_START_SECTOR=$((ESP_START_MiB * SECTORS_PER_MiB))
ESP_SECTORS=$((ESP_SIZE_MiB * SECTORS_PER_MiB))
ESP_END_SECTOR=$((ESP_START_SECTOR + ESP_SECTORS - 1))
DATA_START_SECTOR=$((DATA_START_MiB * SECTORS_PER_MiB))
DATA_SECTORS=$((DATA_SIZE_MiB * SECTORS_PER_MiB))
DATA_END_SECTOR=$((DATA_START_SECTOR + DATA_SECTORS - 1))

esp_image_spec() {
    printf '%s@@%s' "${IMAGE_PATH}" "${ESP_BYTE_OFFSET}"
}

data_image_spec() {
    printf '%s@@%s' "${IMAGE_PATH}" "${DATA_BYTE_OFFSET}"
}

format_fat_partition() {
    local start_sector="$1"
    local size_mib="$2"
    local fat_bits="$3"
    local image_path="$4"
    local volume_label="$5"
    local sectors_per_cluster="$6"

    # mkfs.fat writes a partition-sized filesystem at the given sector offset,
    # which keeps the BPB length consistent with the GPT entry.
    mkfs.fat -F "${fat_bits}" \
        --offset "${start_sector}" \
        -h "${start_sector}" \
        -g "${FAT_HEADS}/${FAT_SECS}" \
        -n "${volume_label}" \
        -s "${sectors_per_cluster}" \
        "${image_path}" \
        "$((size_mib * 1024))" \
        >/dev/null 2>&1
}

image_has_expected_layout() {
    local image_path="$1"
    local layout

    layout=$(parted -s -m "${image_path}" unit s print 2>/dev/null) || return 1

    grep -Fq ':gpt::;' <<< "${layout}" || return 1
    grep -Fqx "1:${ESP_START_SECTOR}s:${ESP_END_SECTOR}s:${ESP_SECTORS}s:fat32:ESP:boot, esp;" <<< "${layout}" || return 1
    grep -Fqx "2:${DATA_START_SECTOR}s:${DATA_END_SECTOR}s:${DATA_SECTORS}s:fat32:DATA:msftdata;" <<< "${layout}" || return 1
}

ensure_fat_dir() {
    local image_spec="$1"
    local rel_dir="$2"

    [ -n "${rel_dir}" ] || return 0

    local fat_path="::"
    local component
    local -a components

    IFS='/' read -ra components <<< "${rel_dir}"
    for component in "${components[@]}"; do
        [ -n "${component}" ] || continue
        fat_path="${fat_path}/${component}"
        mmd -D s -i "${image_spec}" "${fat_path}" 2>/dev/null || true
    done
}

ensure_esp_dir() {
    ensure_fat_dir "$(esp_image_spec)" "$1"
}

ensure_data_dir() {
    ensure_fat_dir "$(data_image_spec)" "$1"
}

copy_into_fat() {
    local image_spec="$1"
    local src="$2"
    local dest_path="$3"

    if [ ! -f "${src}" ]; then
        return 0
    fi

    local dest_dir
    dest_dir="$(dirname "${dest_path}")"
    if [ "${dest_dir}" != "." ]; then
        ensure_fat_dir "${image_spec}" "${dest_dir}"
    fi

    echo "    ${dest_path}"
    mcopy -o -i "${image_spec}" "${src}" "::/${dest_path}"
}

copy_into_esp() {
    copy_into_fat "$(esp_image_spec)" "$1" "$2"
}

copy_into_data() {
    copy_into_fat "$(data_image_spec)" "$1" "$2"
}

copy_dir_into_fat() {
    local image_spec="$1"
    local src_dir="$2"
    local dest_dir="$3"   # relative to ::/

    [ -d "${src_dir}" ] || return 0

    ensure_fat_dir "${image_spec}" "${dest_dir}"

    local f rel dir fat_path
    while IFS= read -r -d '' f; do
        rel="${f#${src_dir}/}"
        dir="$(dirname "${rel}")"

        if [ "${dir}" != "." ]; then
            ensure_fat_dir "${image_spec}" "${dest_dir}/${dir}"
        fi

        echo "    ${dest_dir}/${rel}"
        mcopy -o -i "${image_spec}" "${f}" "::/${dest_dir}/${rel}"
    done < <(find "${src_dir}" -type f -print0)
}

copy_dir_into_data() {
    copy_dir_into_fat "$(data_image_spec)" "$1" "$2"
}

copy_filtered_tree_into_data() {
    local src_dir="$1"
    local dest_dir="$2"
    shift 2

    [ -d "${src_dir}" ] || return 0

    ensure_data_dir "${dest_dir}"

    local src_file rel_path ext allowed_ext
    while IFS= read -r -d '' src_file; do
        rel_path="${src_file#${src_dir}/}"
        ext="${src_file##*.}"
        ext="${ext,,}"

        for allowed_ext in "$@"; do
            if [ "${ext}" = "${allowed_ext}" ]; then
                copy_into_data "${src_file}" "${dest_dir}/${rel_path}"
                break
            fi
        done
    done < <(find "${src_dir}" -type f -print0)
}

stage_clownmdemu_roms() {
    copy_filtered_tree_into_data "userspace/clownmdemu/roms" "data/clownmdemu/roms" \
        bin gen smd 32x md
}

stage_snes9x_roms() {
    copy_filtered_tree_into_data "userspace/snes9x/roms" "data/snes9x/roms" \
        smc sfc
}

stage_vnes_roms() {
    copy_filtered_tree_into_data "userspace/vnes/roms" "data/vnes/roms" \
        nes
}

stage_vspcplay_tracks() {
    copy_filtered_tree_into_data "userspace/vspcplay/tracks" "data/vspcplay/tracks" \
        spc
}

stage_minimp3_tracks() {
    copy_filtered_tree_into_data "userspace/minimp3/tracks" "data/minimp3/tracks" \
        mp3
}

stage_userspace_line_maps() {
    local line_map

    while IFS= read -r -d '' line_map; do
        copy_into_data "${line_map}" "data/debug/lines/$(basename "${line_map}")"
    done < <(find build/symbols/userspace -type f -name '*.vbin.lines' -print0 2>/dev/null)
}

stage_userspace_symbol_maps() {
    local symbol_map

    while IFS= read -r -d '' symbol_map; do
        copy_into_data "${symbol_map}" "data/debug/maps/$(basename "${symbol_map}")"
    done < <(find build/symbols/userspace -type f -name '*.vbin.map' -print0 2>/dev/null)
}

stage_extra_file() {
    local extra_path="$1"
    local extra_name

    [ -f "${extra_path}" ] || return 0

    extra_name=$(basename "${extra_path}")
    case "${extra_name}" in
        vkernel.elf.map|vkernel.elf.lines)
            copy_into_esp "${extra_path}" "boot/${extra_name}"
            ;;
        *.vbin.lines)
            copy_into_data "${extra_path}" "data/debug/lines/${extra_name}"
            ;;
        *.vbin.map)
            copy_into_data "${extra_path}" "data/debug/maps/${extra_name}"
            ;;
        *)
            # Keep the ESP limited to firmware and early-loader inputs.
            copy_into_data "${extra_path}" "data/boot/${extra_name}"
            ;;
    esac
}

TEMP_IMAGE=$(mktemp "${OUTPUT}.tmp.XXXXXX")
IMAGE_PATH="${TEMP_IMAGE}"

if [ -f "${OUTPUT}" ] \
    && image_has_expected_layout "${OUTPUT}" \
    && mdir -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" :: >/dev/null 2>&1 \
    && mdir -i "${OUTPUT}@@${DATA_BYTE_OFFSET}" :: >/dev/null 2>&1; then
    echo "  Refreshing existing disk image..."
    if ! cp --reflink=auto "${OUTPUT}" "${IMAGE_PATH}" 2>/dev/null; then
        cp "${OUTPUT}" "${IMAGE_PATH}"
    fi

    echo "  Reformatting ESP as FAT32..."
    # Use 1 KiB clusters so the 126 MiB ESP stays above FAT32's minimum cluster count
    # while remaining easy for firmware and the in-kernel FAT32 reader to consume.
    format_fat_partition "${ESP_START_SECTOR}" "${ESP_SIZE_MiB}" 32 "${IMAGE_PATH}" "VKEFI" 2

    echo "  Reformatting data partition..."
    format_fat_partition "${DATA_START_SECTOR}" "${DATA_SIZE_MiB}" 32 "${IMAGE_PATH}" "VKDATA" 8
else
    rm -f "${IMAGE_PATH}"

    echo "  Creating ${DISK_MB} MiB blank disk..."
    truncate -s "${DISK_MB}M" "${IMAGE_PATH}"

    echo "  Writing GPT + ESP + data FAT32 partitions..."
    parted -s "${IMAGE_PATH}" mktable gpt
    parted -s "${IMAGE_PATH}" mkpart ESP fat32 "${ESP_START_MiB}MiB" "${ESP_END_MiB}MiB"
    parted -s "${IMAGE_PATH}" set 1 esp on
    parted -s "${IMAGE_PATH}" mkpart DATA fat32 "${DATA_START_MiB}MiB" "${DATA_END_MiB}MiB"

    echo "  Formatting ESP as FAT32..."
    format_fat_partition "${ESP_START_SECTOR}" "${ESP_SIZE_MiB}" 32 "${IMAGE_PATH}" "VKEFI" 2

    echo "  Formatting data partition as FAT32..."
    format_fat_partition "${DATA_START_SECTOR}" "${DATA_SIZE_MiB}" 32 "${IMAGE_PATH}" "VKDATA" 8
fi

echo "  Staging EFI application..."
ensure_esp_dir "EFI/BOOT"
ensure_esp_dir "boot"
mcopy  -o -i "${IMAGE_PATH}@@${ESP_BYTE_OFFSET}" "${EFI_FILE}" ::/EFI/BOOT/bootx64.efi

echo "  Staging data partition..."
ensure_data_dir "bin"
ensure_data_dir "data/shell"
ensure_data_dir "data/vkgui"
ensure_data_dir "data/vkgui/plugins"
ensure_data_dir "data/debug/maps"
ensure_data_dir "data/debug/lines"
ensure_data_dir "data/boot"
ensure_data_dir "data/doom"
ensure_data_dir "data/quake/id1"
ensure_data_dir "data/quake/zeusbot"
ensure_data_dir "data/modplay"
ensure_data_dir "data/rotozoom"
ensure_data_dir "data/clownmdemu/roms"
ensure_data_dir "data/vnes/roms"
ensure_data_dir "data/snes9x/roms"
ensure_data_dir "data/minimp3/tracks"
ensure_data_dir "data/vspcplay/tracks"

while IFS= read -r -d '' vbin; do
    name=$(basename "${vbin}")
    copy_into_data "${vbin}" "bin/${name}"
done < <(find userspace -name "*.vbin" -print0)

stage_userspace_line_maps
stage_userspace_symbol_maps

copy_into_data "userspace/doom/doom1.wad" "data/doom/doom1.wad"
copy_into_data "userspace/doom/doom2.wad" "data/doom/doom2.wad"
copy_into_data "userspace/shell/shell_exec.txt" "data/shell/shell.txt"
copy_into_data "userspace/MODPlay/makemove.mod" "data/modplay/makemove.mod"
copy_into_data "userspace/MODPlay/UNREALPM.S3M" "data/modplay/UNREALPM.S3M"
copy_into_data "userspace/rotozoom/head.bmp" "data/rotozoom/head.bmp"
copy_into_data "userspace/quake/pak0.pak" "data/quake/id1/pak0.pak"
copy_into_data "userspace/quake/progs.dat" "data/quake/zeusbot/progs.dat"
copy_into_data "userspace/quake/zeus_pak0.pak" "data/quake/zeusbot/pak0.pak"

for plugin in userspace/vkgui/runtime_plugins/*.vplg; do
    [ -f "${plugin}" ] || continue
    copy_into_data "${plugin}" "data/vkgui/plugins/$(basename "${plugin}")"
done

for extra in "$@"; do
    stage_extra_file "${extra}"
done

echo "  Staging reaperfx..."
copy_dir_into_data "userspace/quake/reaperfx" "data/quake/reaperfx"
stage_clownmdemu_roms
stage_vnes_roms
stage_vspcplay_tracks
stage_snes9x_roms
stage_minimp3_tracks

mv -f "${IMAGE_PATH}" "${OUTPUT}"
TEMP_IMAGE=""

echo "  Done: ${OUTPUT}"
mdir -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" ::/EFI/BOOT 2>/dev/null || true
