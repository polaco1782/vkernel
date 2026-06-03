#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MUSL_DIR="$ROOT_DIR/userspace/runtime/vendor/musl"
MUSL_URL="https://git.musl-libc.org/git/musl"

if [ ! -d "$MUSL_DIR/.git" ]; then
    mkdir -p "$(dirname "$MUSL_DIR")"
    git clone --depth 1 "$MUSL_URL" "$MUSL_DIR"
fi

if [ "${1:-}" = "update" ]; then
    git -C "$MUSL_DIR" fetch --depth 1 origin master
    git -C "$MUSL_DIR" checkout -B master FETCH_HEAD
fi

printf 'musl vendor tree: %s\n' "$MUSL_DIR"
printf 'musl commit: %s\n' "$(git -C "$MUSL_DIR" rev-parse HEAD)"
