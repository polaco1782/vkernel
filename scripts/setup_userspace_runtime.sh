#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USERSPACE_DIR="$ROOT_DIR/userspace"
RUNTIME_DIR="$USERSPACE_DIR/runtime"
VENDOR_DIR="$RUNTIME_DIR/vendor"
PATCH_DIR="$RUNTIME_DIR/patches"
BUILD_DIR="$RUNTIME_DIR/build"
SYSROOT_DIR="$RUNTIME_DIR/sysroot"
MUSL_VENDOR_DIR="$VENDOR_DIR/musl"
MUSL_BUILD_DIR="$BUILD_DIR/musl"
MUSL_PATCH_SERIES="$PATCH_DIR/musl/series"
MUSL_STAMP="$SYSROOT_DIR/.musl-headers.stamp"
LLVM_PROJECT_VENDOR_DIR="$VENDOR_DIR/llvm-project"
LIBCXX_BUILD_DIR="$BUILD_DIR/libcxx"
LIBCXX_INCLUDE_DIR="$SYSROOT_DIR/include/c++/v1"
LIBCXX_HEADER_STAMP="$SYSROOT_DIR/.libcxx-headers.stamp"
LIBCXX_STAMP="$SYSROOT_DIR/.libcxx-build.stamp"
USERSPACE_CXX_RUNTIME="${USERSPACE_CXX_RUNTIME:-libcxx}"

apply_patch_series() {
    local tree_dir="$1"
    local series_file="$2"

    if [ ! -f "$series_file" ]; then
        return 0
    fi

    while IFS= read -r patch_name; do
        if [ -z "$patch_name" ] || [[ "$patch_name" == \#* ]]; then
            continue
        fi

        patch -d "$tree_dir" -p1 < "$PATCH_DIR/musl/$patch_name"
    done < "$series_file"
}

sync_musl_headers() {
    if [ ! -f "$MUSL_VENDOR_DIR/Makefile" ]; then
        cat <<EOF
ERROR: musl vendor tree not found at:
  $MUSL_VENDOR_DIR

Run:
  bash scripts/import_musl.sh
EOF
        exit 1
    fi

    rm -rf "$MUSL_BUILD_DIR"
    cp -a "$MUSL_VENDOR_DIR" "$MUSL_BUILD_DIR"
    rm -rf "$MUSL_BUILD_DIR/.git"

    apply_patch_series "$MUSL_BUILD_DIR" "$MUSL_PATCH_SERIES"

    cat > "$MUSL_BUILD_DIR/config.mak" <<EOF
ARCH = x86_64
CC = ${CC:-gcc}
AR = ${AR:-ar}
RANLIB = ${RANLIB:-ranlib}
prefix = /
includedir = /include
libdir = /lib
SHARED_LIBS =
TOOL_LIBS =
ALL_TOOLS =
CFLAGS += -I$USERSPACE_DIR/include -m64 -mno-red-zone -fno-stack-protector -fPIC
EOF

    rm -rf "$SYSROOT_DIR/include" "$SYSROOT_DIR/lib"
    mkdir -p "$SYSROOT_DIR/include" "$SYSROOT_DIR/lib"

    make -C "$MUSL_BUILD_DIR" \
        DESTDIR="$SYSROOT_DIR" \
        install-headers install-libs

    git -C "$MUSL_VENDOR_DIR" rev-parse HEAD > "$MUSL_STAMP"
}

sync_libcxx_headers() {
    if [ ! -d "$LLVM_PROJECT_VENDOR_DIR/libcxx/include" ]; then
        if [ "$USERSPACE_CXX_RUNTIME" = "libcxx" ]; then
            cat <<EOF
ERROR: llvm-project vendor tree not found at:
  $LLVM_PROJECT_VENDOR_DIR

Run:
  bash scripts/import_llvm_project.sh
EOF
            exit 1
        fi
        return 0
    fi

    rm -rf "$LIBCXX_INCLUDE_DIR"
    mkdir -p "$LIBCXX_INCLUDE_DIR"
    cp -a "$LLVM_PROJECT_VENDOR_DIR/libcxx/include/." "$LIBCXX_INCLUDE_DIR/"

    if [ -d "$LLVM_PROJECT_VENDOR_DIR/libcxxabi/include" ]; then
        cp -a "$LLVM_PROJECT_VENDOR_DIR/libcxxabi/include/." "$SYSROOT_DIR/include/"
    fi

    cat > "$LIBCXX_INCLUDE_DIR/__config_site" <<'EOF'
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_HAS_THREADS 0
#define _LIBCPP_HAS_MONOTONIC_CLOCK 0
#define _LIBCPP_HAS_MUSL_LIBC 1
#define _LIBCPP_HAS_THREAD_API_PTHREAD 0
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 1
#define _LIBCPP_HAS_RANDOM_DEVICE 0
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_UNICODE 0
#define _LIBCPP_HAS_WIDE_CHARACTERS 0
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT _LIBCPP_ASSERTION_SEMANTIC_IGNORE

#endif
EOF

    mkdir -p "$LIBCXX_INCLUDE_DIR/__cxx03"
    cp "$LIBCXX_INCLUDE_DIR/__config_site" "$LIBCXX_INCLUDE_DIR/__cxx03/__config_site"
    cp "$LLVM_PROJECT_VENDOR_DIR/libcxx/vendor/llvm/default_assertion_handler.in" \
        "$LIBCXX_INCLUDE_DIR/__assertion_handler"

    git -C "$LLVM_PROJECT_VENDOR_DIR" rev-parse HEAD > "$LIBCXX_HEADER_STAMP"
}

build_libcxx_runtime() {
    if [ ! -d "$LLVM_PROJECT_VENDOR_DIR/libcxx" ] || [ ! -d "$LLVM_PROJECT_VENDOR_DIR/libcxxabi" ]; then
        if [ "$USERSPACE_CXX_RUNTIME" = "libcxx" ]; then
            cat <<EOF
ERROR: llvm-project runtime sources not found at:
  $LLVM_PROJECT_VENDOR_DIR

Run:
  bash scripts/import_llvm_project.sh
EOF
            exit 1
        fi
        return 0
    fi

    mkdir -p "$LIBCXX_BUILD_DIR"

    local cmake_args=(
        -S "$LLVM_PROJECT_VENDOR_DIR/runtimes"
        -B "$LIBCXX_BUILD_DIR"
        -G Ninja
        -DLLVM_ENABLE_RUNTIMES=libcxxabi\;libcxx
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX="$SYSROOT_DIR"
        -DCMAKE_SYSROOT="$SYSROOT_DIR"
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
        -DCMAKE_C_COMPILER=clang
        -DCMAKE_CXX_COMPILER=clang++
        "-DCMAKE_C_FLAGS=-ffreestanding -fno-builtin -fno-stack-protector -fPIC -m64 -mno-red-zone -D_GNU_SOURCE"
        "-DCMAKE_CXX_FLAGS=-ffreestanding -fno-builtin -fno-stack-protector -fPIC -m64 -mno-red-zone -D_GNU_SOURCE"
        -DLIBCXX_ENABLE_SHARED=OFF
        -DLIBCXX_ENABLE_STATIC=ON
        -DLIBCXX_INSTALL_LIBRARY=ON
        -DLIBCXX_INSTALL_STATIC_LIBRARY=ON
        -DLIBCXX_INSTALL_SHARED_LIBRARY=OFF
        -DLIBCXX_ENABLE_EXCEPTIONS=OFF
        -DLIBCXX_ENABLE_RTTI=OFF
        -DLIBCXX_ENABLE_THREADS=OFF
        -DLIBCXX_ENABLE_MONOTONIC_CLOCK=OFF
        -DLIBCXX_ENABLE_FILESYSTEM=ON
        -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF
        -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF
        -DLIBCXX_ENABLE_LOCALIZATION=ON
        -DLIBCXX_ENABLE_UNICODE=ON
        -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON
        -DLIBCXX_INCLUDE_TESTS=OFF
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF
        -DLIBCXX_INCLUDE_DOCS=OFF
        -DLIBCXX_HAS_MUSL_LIBC=ON
        -DLIBCXX_CXX_ABI=libcxxabi
        -DLIBCXXABI_ENABLE_SHARED=OFF
        -DLIBCXXABI_ENABLE_STATIC=ON
        -DLIBCXXABI_INSTALL_LIBRARY=ON
        -DLIBCXXABI_INSTALL_STATIC_LIBRARY=ON
        -DLIBCXXABI_INSTALL_SHARED_LIBRARY=OFF
        -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF
        -DLIBCXXABI_ENABLE_THREADS=OFF
        -DLIBCXXABI_USE_LLVM_UNWINDER=OFF
        -DLIBCXXABI_INCLUDE_TESTS=OFF
        -DLIBCXXABI_ENABLE_ASSERTIONS=OFF
    )

    CCACHE_DISABLE=1 cmake "${cmake_args[@]}"
    CCACHE_DISABLE=1 ninja -C "$LIBCXX_BUILD_DIR" libc++abi.a libc++.a libc++experimental.a
    cmake --install "$LIBCXX_BUILD_DIR"

    git -C "$LLVM_PROJECT_VENDOR_DIR" rev-parse HEAD > "$LIBCXX_STAMP"
}

if [ "${1:-}" = "clean" ]; then
    rm -rf "$BUILD_DIR" "$SYSROOT_DIR"
    exit 0
fi

mkdir -p \
    "$VENDOR_DIR" \
    "$PATCH_DIR/musl" \
    "$PATCH_DIR/libcxx" \
    "$PATCH_DIR/libcxxabi" \
    "$BUILD_DIR" \
    "$SYSROOT_DIR" \
    "$SYSROOT_DIR/include" \
    "$SYSROOT_DIR/lib"

sync_musl_headers
sync_libcxx_headers
build_libcxx_runtime

cat <<'EOF'
userspace runtime tree initialized.

This seeds the musl/libc++ migration layout and installs musl libc plus any
available libc++ runtime artifacts into:
  userspace/runtime/vendor
  userspace/runtime/patches/{musl,libcxx,libcxxabi}
  userspace/runtime/build
  userspace/runtime/sysroot

Runtime selector:
  make -C userspace USERSPACE_RUNTIME=musl runtime-setup

Current note:
  USERSPACE_RUNTIME=musl now stages musl libc into userspace/runtime/sysroot.
  USERSPACE_CXX_RUNTIME=libcxx stages libc++ headers, libc++abi, and libc++
  when llvm-project is vendored.
  crt0 still comes from userspace/libc while the syscall/POSIX surface comes
  from the musl archive.
EOF
