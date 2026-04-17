# vkernel — UEFI Microkernel (C++26)

A minimal UEFI microkernel for x86_64, written in **C++26** as a hobby project.

## Current Status

| Subsystem | Status |
|---|---|
| UEFI boot & entry point | ✅ Working |
| Serial + framebuffer console | ✅ Working |
| Self-relocator (GOT patching) | ✅ Working |
| GDT / TSS (64-bit long mode) | ✅ Working |
| IDT — 256 vectors, runtime addresses | ✅ Working |
| Paging hardening (WP + NXE) | ✅ Working |
| PIC 8259A remapping (IRQ0 → vec 32) | ✅ Working |
| PIT 8254 @ 100 Hz (preemption clock) | ✅ Working |
| Round-robin preemptive scheduler | ✅ Working |
| Kernel heap (64 MB static) | ✅ Working |
| Physical page allocator | ⚠️ Stub (page count tracked, no free list) |
| UEFI Simple File System loader | ✅ Working |
| Ramfs (in-memory flat file table) | ✅ Working |
| Kernel/userspace ABI header | ✅ Working |
| Userspace compat layer (`printf`, `FILE`, stubs) | ✅ Working |
| Kernel input subsystem (PS/2 + serial) | ✅ Working |
| Userspace shell | ✅ Working |
| IPC mechanism | ❌ Not yet implemented |
| Virtual memory / paging rework | ❌ Not yet implemented |
| PCI enumeration | ❌ Not yet implemented |
| SMP / multi-core | ❌ Not yet implemented |

## Architecture

```
┌───────────────────────────────────────────────────┐
│               Kernel Tasks (ring 0)               │
│  ┌─────────────────────┬────────────────────────┐ │
│  │   Built-in Shell    │     Idle Task (hlt)    │ │
│  └─────────────────────┴────────────────────────┘ │
├───────────────────────────────────────────────────┤
│                  Microkernel Core                 │
│  ┌────────────┬──────────────┬──────────────────┐ │
│  │ Scheduler  │ Memory (heap │  Input subsystem │ │
│  │ (PIT/PIC)  │  + phys alloc│  (PS/2 + serial) │ │
│  └────────────┴──────────────┴──────────────────┘ │
│  ┌──────────────────────────────────────────────┐ │
│  │  Ramfs  │  UEFI ESP Loader  │  Console/Log   │ │
│  └──────────────────────────────────────────────┘ │
├───────────────────────────────────────────────────┤
│             x86_64 Hardware Abstraction           │
│   GDT / IDT / TSS  │  PIC  │  PIT  │  Paging      │
├───────────────────────────────────────────────────┤
│                  UEFI Firmware                    │
│        (Boot Services exited after init)          │
└───────────────────────────────────────────────────┘
```

## Source Layout

```
include/vkernel/        — Public kernel headers
    config.h            — Build config, version, feature flags
    types.h             — Freestanding primitive types
    console.h           — Console + log:: namespace
    memory.h            — Heap, physical allocator, memory map
    elf.h               — ELF64 loader/data structures
    pe.h                — PE loader/data structures
    scheduler.h         — Task scheduler API
    input.h             — Unified input (PS/2 + serial)
    fs.h                — Ramfs + UEFI ESP loader
    process.h           — ELF/PE loader entry point
    process_internal.h  — Shared loader internals
    uefi.h              — UEFI protocol bindings
    arch/x86_64/arch.h  — GDT/IDT/TSS/paging/port I/O
    vk.h                — Canonical kernel/userspace ABI header

userspace/include/
    vk.h                — libc-style stdio/FILE compatibility wrapper for freestanding ports
    vkernel/vk.h        — Generated copy of the canonical ABI header

userspace/hello/
    hello.c             — Sample freestanding binary using printf/FILE support
    hello.vcxproj       — MSVC project for the sample

userspace/framebuffer/
    framebuffer.c       — Framebuffer painting demo
    framebuffer.vcxproj  — MSVC project for the demo

userspace/framebuffer_text/
    framebuffer_text.c  — Framebuffer text rendering demo
    framebuffer_text.vcxproj — MSVC project for the demo

userspace/raytracer/
    raytracer.c         — Realtime raytracing demo
    raytracer.vcxproj   — MSVC project for the demo

userspace/shell/
    shell.c             — Userspace shell that launches demos
    shell.vcxproj       — MSVC project for the shell

userspace/ramfs_reader/
    ramfs_reader.c      — Ramfs file-reading demo
    ramfs_reader.vcxproj — MSVC project for the demo

src/boot/
    efi_main.cpp        — UEFI entry point, boot phases, self-relocator
    linker.ld           — Custom linker script (base 0, GOT markers)
    reloc_stub.cpp      — Empty .reloc PE section stub

src/core/
    console.cpp         — Serial + framebuffer console, log levels
    memory.cpp          — Heap allocator, physical allocator, memory map
    scheduler.cpp       — Round-robin preemptive scheduler, PIC/PIT init
    input.cpp           — PS/2 keyboard driver + COM1 serial input
    fs.cpp              — Ramfs + UEFI Simple File System Protocol loader
    elf.cpp             — ELF64 binary loader for ramfs-backed programs
    pe.cpp              — PE binary loader for ramfs-backed programs
    kernel_api.cpp      — Kernel API table, file streams, and kernel-backed stubs
    process.cpp         — ELF/PE process loader and task launch
    uefi.cpp            — UEFI protocol wrappers

src/arch/x86_64/
    arch_init.cpp       — GDT, IDT, TSS, paging, interrupt dispatcher
    interrupts.S        — 256 ISR stubs + isr_common save/restore path
```

## Boot Sequence

1. UEFI loads the PE image at an arbitrary base address
2. `efi_main` runs `self_relocate()` — scans `.data` and patches all link-time absolute pointers by adding the load delta (GOT + data pointer tables)
3. **Phase 1** (boot services active): query memory map, query GOP framebuffer, load files from ESP into ramfs
4. **Phase 2**: `ExitBootServices` — switches console to serial + framebuffer
5. **Phase 3**: load GDT/IDT/TSS, harden paging (WP + NXE), init kernel heap, init input subsystem
6. **Phase 4**: init scheduler (PIC remap + PIT @ 100 Hz), create idle task, launch userspace shell, `sched::start()` — does not return

## Building

### Prerequisites (Linux)

```bash
# Fedora
sudo dnf install gcc-c++ make qemu-system-x86-core edk2-ovmf mtools

# Ubuntu/Debian
sudo apt install build-essential qemu-system-x86 ovmf mtools
```

The Makefile uses `x86_64-redhat-linux-g++` by default. Override with:

```bash
make CROSS_PREFIX=x86_64-linux-gnu-
```

### Userspace Shell

The shell now runs as a userspace program launched by the kernel. It still reads from the unified keyboard/serial input path and writes to the same console surfaces.
make              # Release build
make DEBUG=1      # Debug build (VK_DEBUG_LEVEL=5, extra [DEBUG] output)
make userspace    # Build all staged userspace ELF examples
| `help` | List available commands |
| `version` | Show shell and ABI version |
| `mem` | Show memory info |
| `tasks` | Show scheduler tasks |
| `ls` | Show staged files |
| `cat <file>` | Print a ramfs file |
| `clear` | Clear the screen |
| `uptime` | Show tick count |
| `reboot` | Reboot the machine |
| `idt` | Dump interrupt descriptor table |
| `alloc` | Allocate and free a test block |
| `run <file>` | Launch a staged userspace program |
| `panic` | Trigger a userspace fault |
| `exit` | Exit the shell |
`ramfs_reader`). They use `cl` and write their outputs under
`build_vs\<project>\<Configuration>\`.

`run_qemu.bat` stages the matching `.exe` files into the Windows ESP image,
copying `hello.exe`, `framebuffer.exe`, `framebuffer_text.exe`, `raytracer.exe`,
`ramfs_reader.exe`, and `shell.exe` from the selected configuration directory.

Pass `Debug` or `Release` to `run_qemu.bat` to choose which MSVC output
directory gets staged; `Debug` is the default.

Output: `build/vkernel.efi` — a UEFI PE application (~43 KB).

## Running

```bash
# Build a bootable disk image and launch QEMU (GTK display + serial stdio)
./run_qemu.sh

# Same, but pause at startup for GDB
./run_qemu.sh --debug
# then in another terminal:
gdb build/vkernel.elf -ex 'target remote localhost:1234'
```

QEMU is launched with:
- `IDE` disk holding a GPT/FAT32 ESP with the EFI binary at `\EFI\BOOT\bootx64.efi`
- OVMF firmware (4M or 2M variant, auto-detected)
- 512 MB RAM
- VGA + GTK display (keyboard works)
- `serial mon:stdio` (serial input/output in the terminal)

### ESP Files

Files placed under `\EFI\vkernel\` on the ESP are loaded into the ramfs before `ExitBootServices` and are accessible via the shell `cat` command:

```
\EFI\vkernel\motd.txt
\EFI\vkernel\hello.txt
\EFI\vkernel\shell.txt
```

## Userspace Shell

The shell accepts input from both the PS/2 keyboard (QEMU window) and the serial port simultaneously.

| Command | Description |
|---|---|
| `help` | List available commands |
| `version` | Kernel version and build info |
| `mem` | Physical allocator stats |
| `tasks` | List scheduler tasks and states |
| `ls` | List ramfs files |
| `cat <file>` | Print a ramfs file |
| `clear` | Clear the screen |
| `uptime` | Tick count since scheduler start |

### Userspace Examples

The boot image now stages multiple freestanding binaries into ramfs. The shell itself is one of those binaries, and once it is running you can launch the other demos with `run`:

```text
vk> run hello.elf
vk> run framebuffer.elf
vk> run framebuffer_text.elf
vk> run raytracer.elf
vk> run ramfs_reader.elf
```

On Windows/MSVC builds, the same examples are staged as `.exe` files, so the shell names are `hello.exe`, `framebuffer.exe`, `framebuffer_text.exe`, `raytracer.exe`, `ramfs_reader.exe`, and `shell.exe`.

## Key Design Notes

**Position-Independent PE**: The kernel is compiled with `-fpic` and linked at base 0. UEFI loads it at an arbitrary address. Because the `.reloc` section is an empty stub (PE loader applies zero fixups), `self_relocate()` in `efi_main.cpp` manually walks `.data` and adjusts all pointer-sized values that fall within the link-time image range.

**Runtime binary loader**: `process.cpp` accepts ramfs files in either ELF or PE format. It detects the format from the magic bytes (`0x7F 'E' 'L' 'F'` or `MZ`), dispatches to `elf::load()` or `pe::load()`, and then starts the loaded image as a task with the kernel API pointer as the first argument.

**IDT runtime addresses**: ISR stub addresses are computed at runtime via `lea isr_stub_0(%rip)` + stride × vector, avoiding the broken absolute addresses that a `.rodata` table would contain.

**Userspace ABI split**: `include/vkernel/vk.h` is the canonical ABI. Shared helpers that do not need kernel state stay as inline `vk_*` functions there or in the userspace compat wrapper, while kernel-only services remain in `vk_api_t`.

**Userspace stdio layer**: `userspace/include/vk.h` provides a freestanding C-style wrapper with `printf`, `FILE`, `fopen`, `fread`, `fwrite`, and the usual string/memory shims on top of the kernel ABI.

**Userspace shell**: `userspace/shell` is launched by the kernel at boot and provides the interactive prompt, file inspection, and `run` command in userspace.

**Userspace examples**: `userspace/hello` prints runtime and file-system information, `userspace/framebuffer` paints the GOP framebuffer directly, `userspace/framebuffer_text` renders text into the framebuffer, `userspace/raytracer` draws a realtime raytraced scene, and `userspace/ramfs_reader` reads back a staged ELF from ramfs and prints its header bytes.

**Exception handling**: CPU exceptions in userspace terminate only the faulting task when a process context exists. Kernel-mode exceptions still panic the kernel.

**No context-switch assembly file**: `sched_switch_to` is a `[[gnu::naked]]` C++ function in `scheduler.cpp`. The only assembly file is `interrupts.S` (256 macro-generated ISR stubs + the common save/restore/iretq path).

**Unified input**: `input::getc()` polls PS/2 and COM1 in a tight loop, yielding between polls. Any kernel task or future user-space server calls this without knowing the physical source.

## C++26 Compiler Requirements

- **GCC**: 14+ (`-std=c++26`)
- **Clang**: 17+ (`-std=c++2b`)

Freestanding flags: `-ffreestanding -nostdlib -fno-exceptions -fno-rtti -mno-red-zone`

## License

MIT License — see `LICENSE` for details.

## References

- [OSDev Wiki](https://wiki.osdev.org/)
- [UEFI Specification](https://uefi.org/specifications)
- [Intel 64 SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Tianocore EDK II](https://github.com/tianocore/edk2)
