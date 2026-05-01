# vkernel — UEFI Microkernel (C++26)

A minimal UEFI microkernel for x86_64, written in C++26 as a hobby project.
Boots directly from UEFI firmware, runs freestanding userspace binaries out of
a ramfs, and includes a working preemptive round-robin scheduler, PCI bus
enumeration, ACPI table parsing, SMP bringup infrastructure, a loadable driver
framework with SB16 and AC97 sound drivers, and ports of Doom and Dear ImGui.

## Current Status

| Subsystem | Status |
|---|---|
| UEFI boot & self-relocator (GOT patching) | ✅ Working |
| Serial + GOP framebuffer console | ✅ Working |
| GDT / TSS / IDT (256 vectors, runtime addresses) | ✅ Working |
| Paging hardening (WP + NXE) | ✅ Working |
| PIC 8259A remapping (IRQ0 → vec 32) | ✅ Working |
| PIT 8254 @ 100 Hz (preemption clock) | ✅ Working |
| Round-robin preemptive scheduler | ✅ Working |
| Kernel heap (64 MB static) | ✅ Working |
| Physical page allocator | ⚠️ Stub (page count tracked; no free list) |
| ACPI RSDP/RSDT/XSDT/MADT parser | ✅ Working |
| PCI bus enumeration (I/O config space) | ✅ Working |
| Loadable driver framework | ✅ Working |
| Sound subsystem (vtable + active driver) | ✅ Working |
| SB16 sound driver | ✅ Working |
| AC97 sound driver | ✅ Working |
| Bochs VBE display driver | ✅ Working |
| SMP bringup (INIT-SIPI-SIPI + AP trampoline) | ✅ Infrastructure complete; disabled at boot |
| UEFI Simple File System loader → ramfs | ✅ Working |
| Ramfs (in-memory flat file table, read-only) | ✅ Working |
| ELF64 + PE/COFF userspace loader | ✅ Working |
| Kernel API (`vk_api_t`, ABI version 12) | ✅ Working |
| Userspace libc (newlib sysroot + CRT glue) | ✅ Working |
| Unified input subsystem (PS/2 + COM1 serial) | ✅ Working |
| Userspace shell | ✅ Working |
| Panic handler (`vk_panic()` + halt) | ✅ Working |
| IPC mechanism | ❌ Not yet implemented |
| Virtual memory / demand paging | ❌ Not yet implemented |

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                    Userspace programs  (ring 0)                  │
│  shell  hello  framebuffer  raytracer  rotozoom  MODPlay  doom   │
│  framebuffer_text  ramfs_reader  vgui (Dear ImGui)               │
│                   freestanding C/C++ · .vbin ELF64               │
├──────────────────────────────────────────────────────────────────┤
│               Userspace libc  (newlib sysroot + CRT glue)        │
│          crt0.c · syscalls.c · userspace/include/vk.h            │
│     printf · FILE · fopen/fread · malloc · memcpy wrappers       │
├──────────────────────────────────────────────────────────────────┤
│                    Kernel API  (vk_api_t v12)                    │
│   console · input · memory · file I/O · process · sound ·        │
│   framebuffer · raw key/mouse events · driver load/unload        │
├──────────────────────────────────────────────────────────────────┤
│                      Microkernel Core  (ring 0)                  │
│  ┌────────────┬──────────────┬──────────┬──────────────────────┐ │
│  │ Scheduler  │ Memory       │ Input    │ Console / log        │ │
│  │ PIT 100 Hz │ 64 MB heap   │ PS/2 +   │ serial + GOP fb      │ │
│  │ round-robin│ phys alloc   │ COM1     │                      │ │
│  ├────────────┴──────────────┴──────────┴──────────────────────┤ │
│  │ Filesystem          │ Process loader  │ Panic               │ │
│  │ ramfs + UEFI ESP    │ ELF64 · PE/COFF │ vk_panic() · halt   │ │
│  ├────────────┬────────┴───────────────────────────────────────┤ │
│  │ ACPI       │ PCI bus                                        │ │
│  │ RSDP·MADT  │ I/O 0xCF8/0xCFC config space                   │ │
│  ├────────────┴────────────────────────────────────────────────┤ │
│  │ Driver framework                                            │ │
│  │  SB16 · AC97 · Bochs VBE · (future: net, block)             │ │
│  ├──────────────────────────────┬──────────────────────────────┤ │
│  │ SMP (infrastructure ready)   │ Sound subsystem (vtable)     │ │
│  │ INIT-SIPI-SIPI · AP trampo.  │ active driver forwarding     │ │
│  └──────────────────────────────┴──────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│              x86_64 Hardware Abstraction                         │
│   GDT · IDT · TSS · PIC · PIT · Paging · LAPIC · AP trampoline   │
├──────────────────────────────────────────────────────────────────┤
│                      UEFI Firmware                               │
│     Boot services · GOP · config table (RSDP) · Simple FS        │
└──────────────────────────────────────────────────────────────────┘
```

## Source Layout

```
include/vkernel/                — Public kernel headers
    config.h                    — Build config, version, feature flags
    types.h                     — Freestanding primitive types
    console.h                   — Console + log:: namespace
    memory.h                    — Heap, physical allocator, memory map
    elf.h                       — ELF64 loader / data structures
    pe.h                        — PE/COFF loader / data structures
    scheduler.h                 — Task scheduler API
    input.h                     — Unified input (PS/2 + serial)
    fs.h                        — Ramfs + UEFI ESP loader
    process.h                   — ELF/PE loader entry point
    process_internal.h          — Shared loader internals
    acpi.h                      — ACPI RSDP/RSDT/XSDT/MADT structures + API
    pci.h                       — PCI bus enumeration API
    driver.h                    — Loadable driver framework
    sound.h                     — Sound subsystem vtable + management API
    smp.h                       — SMP bringup API (cpu_info, init, queries)
    panic.h                     — vk_panic() declaration
    uefi.h                      — UEFI protocol bindings
    vk.h                        — Canonical kernel/userspace ABI (vk_api_t v12)
    arch/x86_64/arch.h          — GDT/IDT/TSS/paging/port I/O/LAPIC
    arch/x86_64/gcc_asm.h       — GCC inline-asm helpers
    arch/x86_64/msvc_asm.h      — MSVC intrinsic helpers

src/boot/
    efi_main.cpp                — UEFI entry point, 4-phase boot, self-relocator
    linker.ld                   — Custom linker script (base 0, GOT markers)
    reloc_stub.cpp              — Empty .reloc PE section stub

src/core/
    console.cpp                 — Serial + framebuffer console, log levels
    memory.cpp                  — Heap allocator, physical allocator, memory map
    scheduler.cpp               — Round-robin preemptive scheduler, PIC/PIT init
    input.cpp                   — PS/2 keyboard driver + COM1 serial input
    fs.cpp                      — Ramfs + UEFI Simple File System loader
    elf.cpp                     — ELF64 binary loader
    pe.cpp                      — PE/COFF binary loader
    kernel_api.cpp              — vk_api_t table, file streams, syscall stubs
    process.cpp                 — ELF/PE process loader and task launch
    acpi.cpp                    — ACPI RSDP/RSDT/XSDT/MADT parser
    pci.cpp                     — PCI bus enumeration via I/O ports 0xCF8/0xCFC
    driver.cpp                  — Loadable driver registry (load/unload/list)
    sound.cpp                   — Sound subsystem management (active driver)
    sound_sb16.cpp              — Sound Blaster 16 driver
    sound_ac97.cpp              — AC97 audio driver
    bochs_vbe.cpp               — Bochs VBE display driver
    panic.cpp                   — vk_panic() — dump context and halt
    uefi.cpp                    — UEFI protocol wrappers

src/arch/x86_64/
    arch_init.cpp               — GDT, IDT, TSS, paging, interrupt dispatcher
    interrupts.S                — 256 ISR stubs + isr_common save/restore path
    ap_trampoline.S             — 16-bit AP startup blob (real → PM → LM)
    smp.cpp                     — SMP bringup: MADT scan, INIT-SIPI-SIPI, APs
    gcc_asm.S                   — ap_entry_64 (stack load → ap_init_secondary)
    interrupts.asm              — MSVC MASM ISR stubs
    msvc_asm.asm                — MSVC MASM helpers

userspace/include/
    vk.h                        — libc-style stdio/FILE compat wrapper

userspace/libc/
    crt0.c                      — freestanding C runtime start (_start → main)
    syscalls.c                  — newlib syscall stubs backed by vk_api_t
    Makefile                    — builds crt0.o + libvksys.a into sysroot

userspace/hello/                — Runtime info + ramfs + stdio demo
userspace/shell/                — Interactive shell (help/run/ls/cat/mem/tasks…)
userspace/framebuffer/          — Direct GOP pixel painting demo
userspace/framebuffer_text/     — Text rendering into framebuffer demo
userspace/raytracer/            — Realtime raytraced scene demo
userspace/rotozoom/             — Rotate-zoom effect demo
userspace/ramfs_reader/         — Inspect ramfs files, print ELF header bytes
userspace/MODPlay/              — MOD tracker audio player (PCM via sound API)
userspace/doom/                 — Chocolate Doom port (SDL shim → vk_api_t)
userspace/vgui/                 — Dear ImGui frontend (imgui_impl_vk backend)
```

## Boot Sequence

1. UEFI loads the PE image at an arbitrary base address.
2. `efi_main` runs `self_relocate()` — scans `.data` and patches all
   link-time absolute pointers by adding the load delta (GOT + data pointer
   tables). No `.reloc` fixups are used.
3. **Phase 1** (boot services active): query memory map, query GOP
   framebuffer, locate ACPI RSDP via UEFI configuration table, load
   `\EFI\vkernel\*` files from the ESP into ramfs.
4. **Phase 2**: `ExitBootServices` — switches console to serial + framebuffer.
5. **Phase 3**: `arch::activate()` — load GDT/IDT/TSS, harden paging
   (WP + NXE), init kernel heap, init input subsystem.
6. **Phase 4**: `acpi::init()` → `pci::init()` → `driver::init()` →
   sound driver probe (SB16 or AC97) → `sched::init()` (PIC remap + PIT @
   100 Hz) → create idle task → launch `shell.vbin` → `sched::start()`.
   Does not return. SMP bringup (`smp::init()`) is implemented but currently
   commented out in this phase.

## Kernel API (`vk_api_t`, ABI version 12)

Every userspace binary receives a `const vk_api_t*` as its first argument.
There are no syscall instructions — it is a plain function-pointer table
populated once at boot by `kernel_api.cpp`.

| Group | Functions |
|---|---|
| Console output | `vk_puts`, `vk_putc`, `vk_put_hex`, `vk_put_dec`, `vk_clear` |
| Console input | `vk_getc` (blocking), `vk_try_getc` (non-blocking) |
| Memory | `vk_malloc`, `vk_free`, `vk_realloc`, `vk_memset`, `vk_memcpy`, `vk_memmove`, `vk_memchr`, `vk_memcmp` |
| File I/O (direct) | `vk_file_exists`, `vk_file_size`, `vk_file_read` |
| File I/O (stream) | `vk_file_open`, `vk_file_close`, `vk_file_read_handle`, `vk_file_write_handle`, `vk_file_seek`, `vk_file_tell`, `vk_file_eof`, `vk_file_error`, `vk_file_flush` |
| Process | `vk_exit`, `vk_yield`, `vk_sleep`, `vk_run`, `vk_wait_task`, `vk_tick_count`, `vk_ticks_per_sec`, `vk_reboot` |
| Diagnostics | `vk_dump_memory`, `vk_dump_tasks`, `vk_dump_idt` |
| Framebuffer | `vk_framebuffer_info` → base address, width, height, stride, pixel format |
| Raw input | `vk_poll_key` (scancode + modifier bits), `vk_poll_mouse` (Δx/Δy + buttons) |
| Sound | `vk_snd_play`, `vk_snd_stop`, `vk_snd_is_playing`, `vk_snd_set_sample_rate`, `vk_snd_set_volume` |
| Drivers | `vk_drv_load`, `vk_drv_unload` |

Ramfs is read-only — `vk_file_write_handle`, `vk_file_remove`, and
`vk_file_rename` are stubbed and always return `-1`. New fields may only be
appended to the end of `vk_api_t`; `api_version` must be bumped on any
breaking layout change.

## SMP

The full SMP bringup infrastructure is implemented in
`src/arch/x86_64/smp.cpp` and `src/arch/x86_64/ap_trampoline.S` but is
currently disabled (commented out in `efi_main.cpp`).

**What is implemented:**

- ACPI MADT scan (`foreach_madt_entry(lapic, …)`) to discover all CPUs.
- LAPIC base address read from `MSR_IA32_APIC_BASE` (MSR `0x1B`).
- Per-AP 64 KB private kernel stacks (`s_ap_stacks[MAX_CPUS][65536]`).
- 16-bit AP trampoline blob copied to physical `0x8000`, with a data area
  written by the BSP at `0x8100–0x8154`: temporary 5-entry GDT, BSP's CR3,
  per-AP stack pointer, and the 64-bit far-jump descriptor.
- INIT-SIPI-SIPI IPI sequence per AP (10 ms after INIT; two SIPIs 200 µs
  apart; up to 1 s poll on ready flag at `0x8150`).
- AP mode-switch: real mode → 32-bit PM (`ap_pm32` at `0x8040`) → 64-bit
  long mode via PAE + `EFER.LME` + `CR0.PG`, landing in `ap_entry_64` →
  `ap_init_secondary()`.
- `ap_init_secondary()` calls `arch::ap_activate()` (reload real GDT/IDT),
  `lapic_init_local()` (enable LAPIC SVR), sets the ready flag, then enters
  `sched::start_ap()` which arms the LAPIC timer and begins the `hlt` loop.
- Public API: `smp::cpu_count()`, `smp::current_cpu_apic_id()`,
  `smp::get_cpu_info(idx)`, `smp::dump_cpus()`.

## Driver Framework

Drivers are compiled into the kernel image and activated on demand. Each
driver provides a `driver_descriptor` with a name (e.g. `"sb16"`), type
(`driver_type::sound`), and a vtable pointer (`sound_driver_t*`). Registration
happens at boot via `driver::register_driver()`.

Runtime management:

- Shell command: `drvload <name>`
- Kernel API: `vk_drv_load` / `vk_drv_unload`
- Direct: `driver::load("sb16")` / `driver::unload("sb16")`

Currently registered drivers: `sb16`, `ac97`, `bochs_vbe`.

## Userspace Shell

```
vk> help
```

| Command | Description |
|---|---|
| `help` | List available commands |
| `version` | Kernel version and build info |
| `mem` | Heap and physical allocator stats |
| `tasks` | Scheduler task list |
| `ls` | List ramfs files |
| `cat <file>` | Print a ramfs file |
| `clear` | Clear the screen |
| `uptime` | Tick count since scheduler start |
| `reboot` | Reboot the machine |
| `idt` | Dump interrupt descriptor table |
| `alloc` | Allocate and free a test heap block |
| `drvload <name>` | Load a kernel driver by name |
| `run <file>` | Launch a `.vbin` userspace program |
| `panic` | Trigger a userspace fault |
| `exit` | Exit the shell |

### Userspace Programs

All programs are freestanding ELF64 binaries (`.vbin`) loaded from ramfs.

| Binary | Description |
|---|---|
| `shell.vbin` | Interactive shell (launched automatically at boot) |
| `hello.vbin` | Runtime info, ramfs test, stdio demo |
| `framebuffer.vbin` | Direct GOP pixel painting |
| `framebuffer_text.vbin` | Text rendering into framebuffer |
| `raytracer.vbin` | Realtime raytraced scene |
| `rotozoom.vbin` | Rotate-zoom effect |
| `ramfs_reader.vbin` | Inspect ramfs files and ELF headers |
| `modplay.vbin` | MOD tracker audio player (uses sound API) |
| `doom.vbin` | Chocolate Doom port (SDL shim → vk_api_t) |
| `vgui.vbin` | Dear ImGui frontend (run `setup_imgui.sh` first) |

## Building

### Prerequisites (Linux)

```bash
# Fedora
sudo dnf install gcc-c++ make qemu-system-x86-core edk2-ovmf mtools

# Ubuntu / Debian
sudo apt install build-essential qemu-system-x86 ovmf mtools
```

The Makefile uses `x86_64-redhat-linux-g++` by default. Override with:

```bash
make CROSS_PREFIX=x86_64-linux-gnu-
```

### Makefile targets

```bash
make                  # Release build → build/vkernel.efi
make DEBUG=1          # Debug build (-g -O0, KERNEL_DEBUG=1)
make userspace        # Build all userspace .vbin binaries
make disk             # Build bootable GPT/FAT32 disk image
make disasm           # Disassemble kernel ELF → build/vkernel.dis
make clean            # Remove build artifacts
make distclean        # Also remove newlib sysroot
```

### Newlib sysroot (required for hello, doom, MODPlay, rotozoom, vgui)

Some userspace programs link against newlib. Run once before building:

```bash
bash scripts/setup_newlib.sh
```

The Makefile calls this automatically if the sysroot is missing.

### Dear ImGui (required for vgui)

```bash
bash userspace/vgui/setup_imgui.sh
```

The Makefile calls this automatically if `imgui.h` is missing.

### VS Code tasks

- `build vkernel (Linux)` — runs `make DEBUG=1 disk`
- `build vkernel (Windows)` — runs `msbuild vkernel.sln /m /p:Configuration=Debug /p:Platform=x64`

Windows MSVC builds produce `.vbin` outputs under `build_vs\<project>\<Config>\`.
`run_qemu.bat [Debug|Release] [--debug]` stages the matching binaries into the
ESP image and launches QEMU.

### Output

`build/vkernel.efi` — UEFI PE application (~43 KB, DWARF sections stripped).  
`build/vkernel.elf` — unstripped ELF kept for GDB / QEMU symbol loading.

## Running

```bash
# Build disk image and launch QEMU (GTK display + serial stdio)
./run_qemu.sh

# Pause at startup for GDB
./run_qemu.sh --debug
# In another terminal:
gdb build/vkernel.elf -ex 'target remote localhost:1234'
```

QEMU is configured with:
- IDE disk — GPT/FAT32 ESP with `\EFI\BOOT\bootx64.efi`
- OVMF firmware (4 M or 2 M variant, auto-detected)
- 512 MB RAM
- VGA + GTK display (keyboard input works)
- `serial mon:stdio` (serial I/O in the terminal)

### ESP data files

Files placed under `\EFI\vkernel\` on the ESP are loaded into ramfs before
`ExitBootServices` and can be read via the shell `cat` command:

```
\EFI\vkernel\motd.txt
\EFI\vkernel\hello.txt
\EFI\vkernel\shell.txt
```

## Debugging

```bash
make DEBUG=1 disk
./run_qemu.sh --debug
gdb build/vkernel.elf -ex 'target remote localhost:1234'
```

In VS Code: run the `qemu debug (Linux)` task, then attach
`Attach to vkernel (QEMU/GDB, Linux)` from the Run and Debug panel.

Useful breakpoints: `efi_main`, `self_relocate`, `sched::start`,
`ap_init_secondary`, `vk::panic`.

On Windows: `run_qemu.bat Debug --debug` pauses the VM before boot.


## but does it runs... DOOM?

![Doom running on vkernel](doom.webp)


## Key Design Notes

**Position-independent PE.** The kernel is compiled with `-fpic` and linked
at base 0. UEFI loads it at an arbitrary address. Because the `.reloc`
section is an empty stub (PE loader applies zero fixups), `self_relocate()`
in `efi_main.cpp` manually walks `.data` and adjusts all pointer-sized values
that fall within the link-time image range.

**No ring transitions.** Everything runs at ring 0. The kernel API is a plain
`const vk_api_t*` function-pointer table passed on the stack to each process
entry point. No `syscall` or `sysenter` instructions are used.

**Runtime binary loader.** `process.cpp` detects ELF64 (`\x7FELF`) or PE
(`MZ`) from magic bytes, dispatches to `elf::load()` or `pe::load()`, and
starts the result as a new scheduler task with the kernel API pointer as its
first argument.

**IDT runtime addresses.** ISR stub addresses are computed at runtime via
`lea isr_stub_0(%rip)` + stride × vector, avoiding broken absolute addresses
that a `.rodata` table would contain after relocation.

**Shared page tables across cores.** APs reuse the BSP's CR3. The UEFI
identity-mapped layout covers the full address space, so no per-AP paging
setup is needed.

**`SCHED_NO_TASK` sentinel.** Set to `SIZE_MAX`. Prevents the scheduler's
preempt handler from treating an uninitialized AP task slot as task 0, which
would corrupt task 0's saved RSP if both the BSP and an AP fired a timer
interrupt simultaneously before the AP's slot was assigned.

**Unified input.** `input::getc()` polls PS/2 and COM1 in a tight loop,
yielding between polls. Any kernel task or userspace program calls this
without knowing the physical source.

**Console routing per process.** Each process has a `process_task_context`
with a `console_interface` flag (`graphical` or `serial`), so `vk_putc`
automatically goes to the right output sink.

**No context-switch assembly file.** `sched_switch_to` is a
`[[gnu::naked]]` C++ function in `scheduler.cpp`. The only assembly files
are `interrupts.S` (256 macro-generated ISR stubs + common save/restore path)
and `ap_trampoline.S` (AP 16-bit startup blob).

**Ramfs is read-only.** Stream write, remove, and rename calls in `vk_api_t`
are stubbed and always return `-1`.

## C++26 Compiler Requirements

- **GCC**: 14+ (`-std=c++26`)
- **Clang**: 17+ (`-std=c++2b`)
- **MSVC**: VS 2022 17.8+ (`/std:c++latest`)

Freestanding flags: `-ffreestanding -nostdlib -fno-exceptions -fno-rtti -mno-red-zone`

## License

MIT License — see `LICENSE` for details.

## References

- [OSDev Wiki](https://wiki.osdev.org/)
- [UEFI Specification](https://uefi.org/specifications)
- [Intel 64 SDM Vol. 3A §8.4 — MP Initialization Protocol](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Tianocore EDK II](https://github.com/tianocore/edk2)
- [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom)
- [Dear ImGui](https://github.com/ocornut/imgui)
