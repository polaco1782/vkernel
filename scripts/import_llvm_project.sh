#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_DIR="$ROOT_DIR/userspace/runtime/vendor/llvm-project"
LLVM_URL="https://github.com/llvm/llvm-project"

if [ -d "$LLVM_DIR/.git" ] && ! git -C "$LLVM_DIR" rev-parse --verify refs/remotes/origin/main >/dev/null 2>&1; then
    cat <<EOF
ERROR: llvm-project vendor tree exists but does not have a usable origin/main ref:
  $LLVM_DIR

Please remove the incomplete checkout and rerun:
  bash scripts/import_llvm_project.sh
EOF
    exit 1
fi

if [ ! -d "$LLVM_DIR/.git" ]; then
    mkdir -p "$(dirname "$LLVM_DIR")"
    git clone --depth 1 "$LLVM_URL" "$LLVM_DIR"
fi

if [ "${1:-}" = "update" ]; then
    git -C "$LLVM_DIR" fetch --depth 1 origin main
fi

git -C "$LLVM_DIR" checkout -B main origin/main >/dev/null 2>&1

printf 'llvm-project vendor tree: %s\n' "$LLVM_DIR"
printf 'llvm-project commit: %s\n' "$(git -C "$LLVM_DIR" rev-parse HEAD)"
