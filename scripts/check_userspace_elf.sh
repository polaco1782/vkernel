#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <elf-binary>" >&2
    exit 2
fi

INPUT="$1"

if ! command -v readelf >/dev/null 2>&1; then
    echo "check_userspace_elf: readelf not found" >&2
    exit 1
fi

HEADER="$(readelf -h "$INPUT")"
PROGRAM_HEADERS="$(readelf -lW "$INPUT")"

if ! printf '%s\n' "$HEADER" | grep -Eq 'Type:[[:space:]]+DYN'; then
    echo "check_userspace_elf: expected ET_DYN static PIE: $INPUT" >&2
    exit 1
fi

if printf '%s\n' "$PROGRAM_HEADERS" | grep -Eq '(^|[[:space:]])INTERP([[:space:]]|$)|Requesting program interpreter'; then
    echo "check_userspace_elf: PT_INTERP is not supported: $INPUT" >&2
    exit 1
fi

