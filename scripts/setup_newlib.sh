#!/bin/bash
# vkernel - setup_newlib.sh
# Copyright (C) 2026 vkernel authors
#
# Download, configure and cross-compile newlib for vkernel userspace.
#
# Prerequisites (Fedora/RHEL):
#   dnf install gcc make texinfo
#   The cross-compiler used by the kernel build (x86_64-redhat-linux-gcc)
#   is reused here with bare-metal flags.
#
# Usage:
#   ./scripts/setup_newlib.sh          # full build
#   ./scripts/setup_newlib.sh clean    # remove build artifacts
#
# The script produces:
#   userspace/sysroot/include/         # newlib headers
#   userspace/sysroot/lib/libc.a       # C library
#   userspace/sysroot/lib/libm.a       # math library

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
USERSPACE_DIR="$ROOT_DIR/userspace"
NEWLIB_DIR="$USERSPACE_DIR/newlib"
SYSROOT_DIR="$USERSPACE_DIR/sysroot"
BUILD_DIR="$USERSPACE_DIR/newlib-build"

NEWLIB_VERSION="4.4.0.20231231"
NEWLIB_TARBALL="newlib-${NEWLIB_VERSION}.tar.gz"
NEWLIB_URL="https://sourceware.org/pub/newlib/${NEWLIB_TARBALL}"

# Cross-compiler prefix – must match the kernel Makefile.
# Override with: CROSS_PREFIX=x86_64-elf- ./scripts/setup_newlib.sh
CROSS_PREFIX="${CROSS_PREFIX:-x86_64-redhat-linux-}"

# Target triplet for newlib's configure.
# We use a generic ELF target; our own syscalls replace libgloss.
# NOTE: must differ from the build host (x86_64-redhat-linux) so newlib
# treats this as a cross build and actually compiles its libraries.
NEWLIB_TARGET="x86_64-elf"

# Compiler/flags for the target code inside newlib.
CFLAGS_FOR_TARGET="-O2 -g"
CFLAGS_FOR_TARGET+=" -fpie -mcmodel=small"
CFLAGS_FOR_TARGET+=" -fno-stack-protector"
CFLAGS_FOR_TARGET+=" -mno-red-zone"
CFLAGS_FOR_TARGET+=" -ffunction-sections -fdata-sections"

# ─────────────────────────────────────────────────────────────
# Clean target
# ─────────────────────────────────────────────────────────────
if [ "${1:-}" = "clean" ]; then
    echo "Cleaning newlib build artifacts..."
    rm -rf "$BUILD_DIR" "$SYSROOT_DIR"
    echo "Done.  (Source in $NEWLIB_DIR is kept; delete manually if desired.)"
    exit 0
fi

# ─────────────────────────────────────────────────────────────
# 1. Download newlib source
# ─────────────────────────────────────────────────────────────
if [ ! -d "$NEWLIB_DIR/newlib" ]; then
    echo "==> Downloading newlib ${NEWLIB_VERSION}..."
    mkdir -p "$NEWLIB_DIR"
    if command -v wget &>/dev/null; then
        wget -q --show-progress -O "$NEWLIB_DIR/$NEWLIB_TARBALL" "$NEWLIB_URL"
    elif command -v curl &>/dev/null; then
        curl -L -o "$NEWLIB_DIR/$NEWLIB_TARBALL" "$NEWLIB_URL"
    else
        echo "Error: wget or curl is required." >&2
        exit 1
    fi

    echo "==> Extracting..."
    tar xf "$NEWLIB_DIR/$NEWLIB_TARBALL" -C "$NEWLIB_DIR" --strip-components=1
    rm -f "$NEWLIB_DIR/$NEWLIB_TARBALL"
else
    echo "==> newlib source already present in $NEWLIB_DIR"
fi

# ─────────────────────────────────────────────────────────────
# 2. Configure
# ─────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -f Makefile ]; then
    echo "==> Configuring newlib for ${NEWLIB_TARGET}..."

    # newlib configure expects $target-gcc in PATH.  Create thin
    # wrapper symlinks so it finds our cross-compiler.
    WRAPPER_DIR="$BUILD_DIR/.wrappers"
    mkdir -p "$WRAPPER_DIR"
    for tool in gcc g++ ar as ranlib strip nm objcopy objdump; do
        src="${CROSS_PREFIX}${tool}"
        dst="$WRAPPER_DIR/${NEWLIB_TARGET}-${tool}"
        if [ ! -e "$dst" ]; then
            # Try prefixed tool first, then fall back to native
            if command -v "$src" &>/dev/null; then
                real_path="$(command -v "$src")"
            elif command -v "$tool" &>/dev/null; then
                real_path="$(command -v "$tool")"
            else
                continue
            fi
            # If the resolved path goes through ccache, find the real binary.
            case "$real_path" in
                */ccache/*)
                    real_binary="$(PATH="$(echo "$PATH" | tr ':' '\n' | grep -v ccache | tr '\n' ':')" command -v "$src" 2>/dev/null || echo "$real_path")"
                    ;;
                *)
                    real_binary="$real_path"
                    ;;
            esac
            printf '#!/bin/sh\nexec "%s" "$@"\n' "$real_binary" > "$dst"
            chmod +x "$dst"
        fi
    done
    export PATH="$WRAPPER_DIR:$PATH"

    "$NEWLIB_DIR/configure" \
        --target="$NEWLIB_TARGET" \
        --prefix="$SYSROOT_DIR" \
        --disable-multilib \
        --disable-newlib-supplied-syscalls \
        --disable-newlib-io-float \
        --enable-newlib-io-long-long \
        --enable-newlib-io-c99-formats \
        --enable-newlib-mb \
        --enable-newlib-reent-small \
        CFLAGS_FOR_TARGET="$CFLAGS_FOR_TARGET"
else
    echo "==> newlib already configured"
fi

# ─────────────────────────────────────────────────────────────
# 3. Build
# ─────────────────────────────────────────────────────────────
# Re-export wrappers (in case configure already ran in a prior invocation)
WRAPPER_DIR="$BUILD_DIR/.wrappers"
export PATH="$WRAPPER_DIR:$(echo "$PATH" | tr ':' '\n' | grep -v ccache | tr '\n' ':')"

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "==> Building newlib (${NPROC} jobs)..."
make -j"$NPROC"

# ─────────────────────────────────────────────────────────────
# 4. Install into sysroot
# ─────────────────────────────────────────────────────────────
echo "==> Installing into $SYSROOT_DIR..."
make install

# Flatten the directory layout for convenience:
#   sysroot/x86_64-elf/include → sysroot/include
#   sysroot/x86_64-elf/lib     → sysroot/lib
if [ -d "$SYSROOT_DIR/$NEWLIB_TARGET" ]; then
    cp -a "$SYSROOT_DIR/$NEWLIB_TARGET/include" "$SYSROOT_DIR/include" 2>/dev/null || true
    cp -a "$SYSROOT_DIR/$NEWLIB_TARGET/lib"     "$SYSROOT_DIR/lib"     2>/dev/null || true
fi

echo ""
echo "=== newlib build complete ==="
echo "  Headers : $SYSROOT_DIR/include/"
echo "  libc.a  : $SYSROOT_DIR/lib/libc.a"
echo "  libm.a  : $SYSROOT_DIR/lib/libm.a"
echo ""
echo "Userspace Makefiles should use:"
echo "  CFLAGS  += -isystem $SYSROOT_DIR/include"
echo "  LDFLAGS += -L$SYSROOT_DIR/lib -lc -lm"
