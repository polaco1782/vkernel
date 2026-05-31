#!/bin/bash
# vkernel - UEFI Microkernel
# Copyright (C) 2026 vkernel authors
#
# make_disk.sh - Create a bootable UEFI disk image
#
# Creates a 512 MiB raw GPT disk image with:
#   - EFI System Partition (64 MiB, FAT32) for EFI/BOOT and /boot
#   - FAT32 data partition (446 MiB) for /bin and /data
#
# Usage: make_disk.sh <efi_file> <output_image> [elf_file ...]
# Requires: truncate, parted, mformat, mmd, mcopy  (mtools)

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

for tool in truncate parted mdir mformat mmd mcopy; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Error: '${tool}' not found."
        echo "Install with: dnf install parted mtools  # or: apt install parted mtools"
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
#  ESP         : 1 MiB → 65 MiB  (64 MiB)
#  Data FAT32  : 65 MiB → 511 MiB (446 MiB)
#  GPT backup  : last 33 sectors
#
#  FAT32 geometry uses 64 heads × 32 sectors/track, so tracks == partition MiB.
# ───────────────────────────────────────────────────────────────────────────

DISK_MB=512
ESP_START_MiB=1
ESP_SIZE_MiB=64
ESP_END_MiB=$((ESP_START_MiB + ESP_SIZE_MiB))
DATA_START_MiB=${ESP_END_MiB}
DATA_END_MiB=511
DATA_SIZE_MiB=$((DATA_END_MiB - DATA_START_MiB))

ESP_BYTE_OFFSET=$((ESP_START_MiB * 1024 * 1024))   # 1 048 576
DATA_BYTE_OFFSET=$((DATA_START_MiB * 1024 * 1024))
FAT_HEADS=64
FAT_SECS=32
ESP_TRACKS=${ESP_SIZE_MiB}
DATA_TRACKS=${DATA_SIZE_MiB}

esp_image_spec() {
    printf '%s@@%s' "${IMAGE_PATH}" "${ESP_BYTE_OFFSET}"
}

data_image_spec() {
    printf '%s@@%s' "${IMAGE_PATH}" "${DATA_BYTE_OFFSET}"
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

# Recursively copy a source directory into ::/<dest_dir>.
# Creates intermediate FAT directories as needed then bulk-copies each file.
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
                copy_into_data "${rom}" "data/clownmdemu/roms/${base}"
                ;;
        esac
    done
}

stage_snes9x_roms() {
    local rom
    for rom in userspace/snes9x/roms/*; do
        local base
        local ext

        [ -f "${rom}" ] || continue
        base=$(basename "${rom}")
        ext="${base##*.}"
        ext="${ext,,}"

        case "${ext}" in
            smc|sfc)
                copy_into_data "${rom}" "data/snes9x/roms/${base}"
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
                copy_into_data "${rom}" "data/vnes/roms/${base}"
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
                copy_into_data "${track}" "data/minimp3/tracks/${base}"
                ;;
        esac
    done
}

stage_userspace_line_maps() {
    local line_map

    while IFS= read -r -d '' line_map; do
        copy_into_data "${line_map}" "bin/$(basename "${line_map}")"
    done < <(find build/symbols/userspace -type f -name '*.vbin.lines' -print0 2>/dev/null)
}

TEMP_IMAGE=$(mktemp "${OUTPUT}.tmp.XXXXXX")
IMAGE_PATH="${TEMP_IMAGE}"

if [ -f "${OUTPUT}" ] \
    && mdir -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" :: >/dev/null 2>&1 \
    && mdir -i "${OUTPUT}@@${DATA_BYTE_OFFSET}" :: >/dev/null 2>&1; then
    echo "  Refreshing existing disk image..."
    if ! cp --reflink=auto "${OUTPUT}" "${IMAGE_PATH}" 2>/dev/null; then
        cp "${OUTPUT}" "${IMAGE_PATH}"
    fi

    echo "  Reformatting ESP..."
    mformat -i "${IMAGE_PATH}@@${ESP_BYTE_OFFSET}" -F \
        -h ${FAT_HEADS} -s ${FAT_SECS} -t ${ESP_TRACKS} ::

    echo "  Reformatting data partition..."
    mformat -i "${IMAGE_PATH}@@${DATA_BYTE_OFFSET}" -F \
        -h ${FAT_HEADS} -s ${FAT_SECS} -t ${DATA_TRACKS} ::
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
    mformat -i "${IMAGE_PATH}@@${ESP_BYTE_OFFSET}" -F \
        -h ${FAT_HEADS} -s ${FAT_SECS} -t ${ESP_TRACKS} ::

    echo "  Formatting data partition as FAT32..."
    mformat -i "${IMAGE_PATH}@@${DATA_BYTE_OFFSET}" -F \
        -h ${FAT_HEADS} -s ${FAT_SECS} -t ${DATA_TRACKS} ::
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
ensure_data_dir "data/doom"
ensure_data_dir "data/quake/id1"
ensure_data_dir "data/quake/zeusbot"
ensure_data_dir "data/modplay"
ensure_data_dir "data/rotozoom"
ensure_data_dir "data/clownmdemu/roms"
ensure_data_dir "data/vnes/roms"
ensure_data_dir "data/snes9x/roms"
ensure_data_dir "data/minimp3/tracks"

manifest_file=$(mktemp)
while IFS= read -r -d '' vbin; do
    name=$(basename "${vbin}")
    copy_into_data "${vbin}" "bin/${name}"
    printf '/bin/%s\n' "${name}" >> "${manifest_file}"
done < <(find userspace -name "*.vbin" -print0)

sort -u "${manifest_file}" -o "${manifest_file}"
copy_into_data "${manifest_file}" "data/vkgui/vkgui_apps.txt"
rm -f "${manifest_file}"

stage_userspace_line_maps

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
    local_dest=""
    [ -f "${extra}" ] || continue
    extra_name=$(basename "${extra}")
    case "${extra_name}" in
        vkernel.elf.map|vkernel.elf.lines)
            local_dest="boot/${extra_name}"
            copy_into_esp "${extra}" "${local_dest}"
            ;;
        *.vbin.lines)
            local_dest="bin/${extra_name}"
            copy_into_data "${extra}" "${local_dest}"
            ;;
        *)
            local_dest="boot/${extra_name}"
            copy_into_esp "${extra}" "${local_dest}"
            ;;
    esac
done

echo "  Staging reaperfx..."
copy_dir_into_data "userspace/quake/reaperfx" "data/quake/reaperfx"
stage_clownmdemu_roms
stage_vnes_roms
stage_snes9x_roms
stage_minimp3_tracks

mv -f "${IMAGE_PATH}" "${OUTPUT}"
TEMP_IMAGE=""

echo "  Done: ${OUTPUT}"
mdir -i "${OUTPUT}@@${ESP_BYTE_OFFSET}" ::/EFI/BOOT 2>/dev/null || true
