# Newlib on MSVC — Integration Plan

## Problem
newlib's build system (autotools) doesn't support MSVC. The standard
`configure --target=x86_64-elf` workflow only works with GCC/Clang.

## Recommended Approach: Compile newlib sources directly in a vcxproj

### Phase 1: Create `userspace/libc/libc.vcxproj`
1. Add a new static library project `libc.vcxproj` to `vkernel.sln`
2. Include `crt0.c` and `syscalls.c` (already MSVC-compatible)
3. Cherry-pick newlib source files for the functions we need:
   - `newlib/libc/string/` — memcpy, memset, strlen, strcmp, etc.
   - `newlib/libc/stdlib/` — malloc (uses _sbrk), atoi, strtol, etc.
   - `newlib/libc/stdio/`  — printf, fprintf, fopen, fclose, etc.
   - `newlib/libc/ctype/`  — isalpha, isdigit, etc.
   - `newlib/libc/misc/`   — __libc_init_array, __libc_fini_array
4. Add newlib's internal headers to the include path
5. Provide a `newlib_config.h` to satisfy `_REENT` and config macros

### Phase 2: Handle MSVC-specific issues
- Replace `__attribute__((weak))` with `__declspec(selectany)`
- Handle `__asm__` inline assembly (replace or stub)
- Provide `<sys/config.h>` shim for MSVC
- Define `_REENT_ONLY` to avoid reentrancy overhead

### Phase 3: Link userspace programs
- Each userspace .vcxproj adds `libc.vcxproj` as a project reference
- Link order: crt0.obj → program.obj → libc.lib

### Alternative: Use clang-cl
MSVC supports `clang-cl` as a drop-in compiler with GCC extension
support. This would let us compile newlib sources with fewer
modifications. Set in vcxproj:
```xml
<PlatformToolset>ClangCL</PlatformToolset>
```

### Files needed
- `userspace/libc/libc.vcxproj` — static library project
- `userspace/libc/newlib_msvc_config.h` — config overrides for MSVC
- Modifications to each userspace `.vcxproj` to reference libc
