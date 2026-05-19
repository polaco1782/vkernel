#!/bin/bash
# vkernel - UEFI Microkernel
#
# generate_line_map.sh - Build a compact userspace file/line sidecar.

set -euo pipefail

INPUT="${1:-}"
OUTPUT="${2:-}"

if [ -z "${INPUT}" ] || [ -z "${OUTPUT}" ]; then
    echo "Usage: $0 <input-elf> <output-lines>" >&2
    exit 1
fi

if [ ! -f "${INPUT}" ]; then
    echo "Input ELF not found: ${INPUT}" >&2
    exit 1
fi

for tool in readelf addr2line awk sort mktemp paste; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Required tool not found: ${tool}" >&2
        exit 1
    fi
done

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP_ADDRS="$(mktemp)"
TMP_LOCS="$(mktemp)"
TMP_OUT="$(mktemp)"

cleanup() {
    rm -f "${TMP_ADDRS}" "${TMP_LOCS}" "${TMP_OUT}"
}
trap cleanup EXIT

readelf --debug-dump=decodedline "${INPUT}" | awk '
NF >= 3 && $2 != "number" && $2 != "-" && $3 ~ /^0x[0-9a-fA-F]+$/ {
    key = $3
    sub(/^0x/, "", key)
    if (length(key) > 16) {
        next
    }
    key = sprintf("%016s", key)
    gsub(/ /, "0", key)
    key = "0x" key
    seen[key] = 1
}
END {
    for (key in seen) {
        print key
    }
}
' | sort > "${TMP_ADDRS}"

{
    echo "vklines1"
    if [ -s "${TMP_ADDRS}" ]; then
        addr2line -e "${INPUT}" -C -f < "${TMP_ADDRS}" | awk 'NR % 2 == 0' > "${TMP_LOCS}"
        paste "${TMP_ADDRS}" "${TMP_LOCS}" | awk -v root="${ROOT_DIR}/" '
        function normalize_path(path) {
            if (index(path, root) == 1) {
                return substr(path, length(root) + 1)
            }
            return path
        }
        {
            tab = index($0, "\t")
            if (tab == 0) {
                next
            }

            address = substr($0, 1, tab - 1)
            location = substr($0, tab + 1)
            sub(/ \(discriminator [0-9]+\)$/, "", location)

            if (location == "??:?" || location == "??:0") {
                next
            }

            split_at = 0
            for (i = 1; i <= length(location); ++i) {
                if (substr(location, i, 1) == ":") {
                    split_at = i
                }
            }
            if (split_at == 0) {
                next
            }

            path = substr(location, 1, split_at - 1)
            line = substr(location, split_at + 1)
            if (line !~ /^[0-9]+$/) {
                next
            }

            sub(/^0x/, "", address)
            print substr(address, 1, 16) "\t" line "\t" normalize_path(path)
        }'
    fi
} > "${TMP_OUT}"

mkdir -p "$(dirname "${OUTPUT}")"
mv "${TMP_OUT}" "${OUTPUT}"
