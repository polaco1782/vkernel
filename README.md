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
| Kernel input subsystem (PS/2 + serial) | ✅ Working |
| Built-in kernel shell | ✅ Working |
| IPC mechanism | ❌ Not yet implemented |
| Virtual memory / paging rework | ❌ Not yet implemented |
| PCI enumeration | ❌ Not yet implemented |
| SMP / multi-core | ❌ Not yet implemented |

## Architecture

```
┌──────────────────────────────────────────────────┐
│               Kernel Tasks (ring 0)               │
│  ┌─────────────────────┬────────────────────────┐ │
│  │   Built-in Shell    │     Idle Task (hlt)    │ │
│  └─────────────────────┴────────────────────────┘ │
├──────────────────────────────────────────────────┤
│                  Microkernel Core                  │
│  ┌────────────┬──────────────┬──────────────────┐ │
│  │ Scheduler  │ Memory (heap │  Input subsystem  │ │
│  │ (PIT/PIC)  │  + phys alloc│  (PS/2 + serial) │ │
│  └────────────┴──────────────┴──────────────────┘ │
│  ┌──────────────────────────────────────────────┐ │
│  │  Ramfs  │  UEFI ESP Loader  │  Console/Log   │ │
│  └──────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────┤
│             x86_64 Hardware Abstraction            │
│   GDT / IDT / TSS  │  PIC  │  PIT  │  Paging     │
├──────────────────────────────────────────────────┤
│                  UEFI Firmware                     │
│        (Boot Services exited after init)           │
└──────────────────────────────────────────────────┘
```

## Source Layout

```
include/vkernel/        — Public kernel headers
    config.h            — Build config, version, feature flags
    types.h             — Freestanding primitive types
    console.h           — Console + log:: namespace
    memory.h            — Heap, physical allocator, memory map
    scheduler.h         — Task scheduler API
    input.h             — Unified input (PS/2 + serial)
    fs.h                — Ramfs + UEFI ESP loader
    shell.h             — Built-in shell entry point
    uefi.h              — UEFI protocol bindings
    arch/x86_64/arch.h  — GDT/IDT/TSS/paging/port I/O

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
    shell.cpp           — Built-in kernel shell with commands
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
6. **Phase 4**: init scheduler (PIC remap + PIT @ 100 Hz), create idle task + shell task, `sched::start()` — does not return

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

### Build Commands

```bash
make              # Release build
make DEBUG=1      # Debug build (VK_DEBUG_LEVEL=5, extra [DEBUG] output)
make clean        # Remove build artefacts
make info         # Print build configuration
make disasm       # Generate build/vkernel.disasm
```

### Visual Studio / MSVC

Open `vkernel.sln` and build the `hello` project. It uses `cl` and writes
`hello.exe` under `build_vs\hello\<Configuration>\`.

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

## Shell Commands

The built-in kernel shell accepts input from both the PS/2 keyboard (QEMU window) and the serial port simultaneously.

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

## Key Design Notes

**Position-Independent PE**: The kernel is compiled with `-fpic` and linked at base 0. UEFI loads it at an arbitrary address. Because the `.reloc` section is an empty stub (PE loader applies zero fixups), `self_relocate()` in `efi_main.cpp` manually walks `.data` and adjusts all pointer-sized values that fall within the link-time image range.

**IDT runtime addresses**: ISR stub addresses are computed at runtime via `lea isr_stub_0(%rip)` + stride × vector, avoiding the broken absolute addresses that a `.rodata` table would contain.

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
