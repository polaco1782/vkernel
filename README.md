# vkernel - UEFI Hobby Kernel (C++26)

vkernel is a small x86_64 kernel and userspace playground written in
freestanding C++26. It boots directly from UEFI, mounts its boot disk through a
virtio block device, runs freestanding ELF64 `.vbin` programs, and includes a
Dear ImGui based graphical shell/window manager.

The project is intentionally experimental: userspace is still ring 0, but
processes now run with their own virtual address spaces and isolated image,
heap, and shared framebuffer mappings.

## Current Status

| Subsystem | Status |
|---|---|
| UEFI boot and self-relocation | Working |
| Serial logging and framebuffer console | Working; startup logs default to serial |
| GDT, TSS, IDT, interrupt entry/exit | Working |
| Paging hardening | Working; WP and NXE enabled |
| Kernel heap | Working; boot heap plus expandable subheaps |
| Physical page allocator | Working; free list with page release/coalescing |
| Per-process virtual address spaces | Working; image, heap, and shared framebuffer regions |
| Demand paging | Not implemented |
| Ring 3 userspace isolation | Not implemented |
| PIC, PIT, LAPIC timers | Working |
| SMP bringup | Working under QEMU with 4 CPUs |
| Preemptive round-robin scheduler | Working |
| ACPI and PCI enumeration | Working |
| Driver framework | Working |
| virtio-blk block device | Working; read/write boot disk access |
| AC97 audio | Working |
| Software mixer | Working; 8 channels |
| FAT32 runtime filesystem | Working; read, write, list, seek, remove |
| UEFI-loaded ramfs fallback | Working |
| ELF64 and PE/COFF loaders | Working |
| Kernel API (`vk_api_t`, ABI v39) | Working |
| musl-backed userspace libc | Working |
| libc++ userspace runtime | Working |
| Keyboard, mouse, and serial input | Working |
| vkGUI compositor/window manager | Working |
| KObj typed kernel object tree | Working; JSON RPC from userspace |
| Ports and demos | Doom, Quake, ClownMDEmu, MOD, MP3, raytracer, rotozoom, cube |
| IPC | Not implemented |

## Architecture

```text
┌─────────────────────────────────────────────────────────────────┐
│                 Userspace programs (.vbin ELF64)                │
│  shell  vkgui  hello  doom  quake  clownmdemu  modplay  minimp3 │
│  raytracer  rotozoom  sr_cube  cppcompat  vkobj  vnes  snes9x   │
│                                                                 │
│  Runs in ring 0, but each process has its own CR3/address space │
├─────────────────────────────────────────────────────────────────┤
│                  Userspace runtime and libraries                │
│  crt0 · runtime_glue · musl libc · libc++/libc++abi · vkrt.h    │
├─────────────────────────────────────────────────────────────────┤
│                    Kernel ABI (vk_api_t v39)                    │
│ api table bootstrap · raw vk_syscall · files · process · fb     │
│ input · sound mixer · compositor · task snapshot · kobj RPC     │
├─────────────────────────────────────────────────────────────────┤
│                         Kernel core                             │
│ scheduler · VM · heap · phys pages · FAT32 · ramfs · loaders    │
│ ACPI · PCI · drivers · sound · input · console · kobj · panic   │
├─────────────────────────────────────────────────────────────────┤
│                         Hardware layer                          │
│ GDT · IDT · TSS · paging · PIC · PIT · LAPIC · SMP trampoline   │
├─────────────────────────────────────────────────────────────────┤
│                            UEFI                                 │
│ GOP framebuffer · Simple FS boot staging · ACPI config table    │
└─────────────────────────────────────────────────────────────────┘
```

## Source Layout

```text
include/vkernel/
    vk.h                    Canonical kernel/userspace ABI
    scheduler.h             Task scheduler API
    memory.h                Kernel heap and physical allocator
    virtual_memory.h        Per-process address space API
    framebuffer.h           Shared framebuffer helpers
    fs.h                    Filesystem facade
    fs/fat32.h              FAT32 backend definitions
    fs/ramfs.h              Ramfs fallback definitions
    elf.h, pe.h             Binary loader structures
    process.h               Process launch API
    process_internal.h      Loader/task context internals
    acpi.h, pci.h           Platform discovery
    driver.h, block.h       Driver and block device interfaces
    sound.h                 Sound driver and mixer API
    input.h                 Keyboard, mouse, and serial input
    kobj.h                  Typed kernel object tree
    log.h, console.h        Logging and console output
    arch/x86_64/arch.h      x86_64 arch services

src/boot/
    efi_main.cpp            UEFI entry point and boot sequencing
    linker.ld               Kernel linker script
    reloc_stub.cpp          Empty PE relocation section

src/core/
    scheduler.cpp           Preemptive scheduler and timer dispatch
    virtual_memory.cpp      Address-space creation, mapping, teardown
    memory.cpp              Heap and physical page allocator
    process.cpp             Binary loading and task creation
    kernel_api.cpp          vk_api_t implementation
    fs.cpp                  Filesystem facade and stream handles
    elf.cpp, pe.cpp         ELF64 and PE/COFF loaders
    kobj.cpp                KObj tree and JSON RPC
    acpi.cpp, pci.cpp       ACPI and PCI support
    driver.cpp, block.cpp   Driver registry and block devices
    sound.cpp               Sound subsystem and software mixer
    input.cpp               PS/2, mouse, serial input
    console.cpp, log.cpp    Console and leveled logging
    panic.cpp               Fault reporting and halt path

src/fs/
    fat32.cpp               FAT32 read/write/list/remove backend
    ramfs.cpp               UEFI-loaded fallback file table
    uefi_loader.cpp         Pre-ExitBootServices ESP loader

src/drivers/
    virtio_blk.cpp          Legacy PCI virtio block driver
    sound_ac97.cpp          AC97 audio driver
    bochs_vbe.cpp           Bochs/QEMU display fallback probe

src/arch/x86_64/
    arch_init.cpp           GDT, IDT, TSS, paging, exception handling
    interrupts.S           256 ISR stubs and common save/restore path
    ap_trampoline.S        AP startup trampoline
    smp.cpp                 AP discovery and INIT-SIPI-SIPI bringup

userspace/               Git submodule with runtime, apps, ports, demos
    libc/                   crt0, syscall glue, C/C++ main bridges
    include/vk.h            raw ABI wrapper used by the runtime
    include/vkrt.h          stable userspace runtime services layer
    runtime/                vendored musl/libc++ sysroot staging tree
    shell/                  Interactive shell
    vkgui/                  Dear ImGui shell/window manager
    doom/, quake/           Game ports through vkernel runtime shims
    clownmdemu/             Sega Mega Drive emulator port
    MODPlay/, minimp3/      Audio demos
    raytracer/, rotozoom/   Graphics demos
    sr_cube/, cppcompat/    C/C++ demos
    vkobj/                  KObj CLI
    vnes/, snes9x/          Emulator frontends
    vspcplay/               SPC player frontend
```

## Boot Flow

1. UEFI loads `build/vkernel.efi` from `\EFI\BOOT\bootx64.efi`.
2. The kernel self-relocates because the PE `.reloc` section is intentionally
   empty and the image is linked at base 0.
3. While UEFI boot services are still available, the loader captures the memory
    map, GOP framebuffer, ACPI RSDP, and a fallback ramfs snapshot from
    `\bin` and `\boot` on the ESP.
4. After `ExitBootServices`, the kernel initializes architecture state,
   logging, heap/physical memory, input, ACPI, PCI, drivers, the block layer,
   and the scheduler.
5. `virtio_blk` is loaded, the FAT32 boot filesystem is mounted, and `ac97` is
   loaded if present.
6. `/bin/shell.vbin` and `/bin/vkgui.vbin` are launched, then the scheduler takes over.

## Virtual Memory

Each process gets a `vm::address_space` with its own PML4. The kernel remains
identity/direct mapped, while userspace-visible process memory is placed in
fixed high virtual regions:

| Region | Base |
|---|---:|
| Program image | `0x0000400000000000` |
| Process heap | `0x0000410000000000` |
| Shared/compositor mappings | `0x00007E0000000000` |
| User map limit | `0x0000800000000000` |

ELF images are loaded at the process image base, the C heap grows through the
userspace `brk` path, and larger runtime allocations can come from `mmap` in
the user mapping region. Graphical tasks can also receive a shared framebuffer
mapping owned by the compositor. This is address-space separation, not
privilege separation: code still executes in ring 0.

## Filesystem and Disk Image

The boot disk is a raw GPT image with a FAT32 EFI System Partition. At runtime,
the kernel mounts that FAT32 volume through `virtio_blk` and exposes it through
the filesystem facade:

| Operation | Status |
|---|---|
| Load file by path | Working |
| Open/read/seek/tell/close streams | Working |
| Write streams | Working on FAT32 |
| Remove files | Working on FAT32 |
| List directories | Working |
| Ramfs fallback | Read-only fallback if FAT32 is unavailable |

`scripts/make_disk.sh` builds a GPT image with a small FAT32 ESP plus a
separate FAT32 data partition. The ESP stays limited to firmware and early
loader files while preserving the removable-media boot path at
`\EFI\BOOT\bootx64.efi`:

- The ESP contains `\EFI\BOOT\bootx64.efi` plus `/boot/vkernel.elf.map` and
  `/boot/vkernel.elf.lines`.
- The data partition carries the runtime tree with `/bin`, `/data`, userspace
  debug maps under `/data/debug/maps`, userspace line maps under
  `/data/debug/lines`, vkGUI settings/plugins, game data, emulator ROMs,
  audio tracks, and demo assets.

Representative paths include `/data/shell/shell.txt`,
`/data/debug/maps/*.vbin.map`, `/data/debug/lines/*.vbin.lines`,
`/data/vkgui/plugins/*.vplg`,
`/data/doom/doom1.wad`, `/data/quake/id1/pak0.pak`,
`/data/quake/zeusbot/progs.dat`, `/data/modplay/makemove.mod`, and
`/data/minimp3/tracks/*.mp3`.

## Userspace Runtime and ABI

Every userspace binary still enters at `_start(const vk_api_t* api)`. The
kernel passes a bootstrap `vk_api_t` pointer, `crt0` stores it, builds a
musl-style `argc/argv/envp/auxv` block, and then transfers control into libc
startup.

The ABI now has two layers:

- `include/vkernel/vk.h` is the canonical kernel ABI shared by kernel and
  runtime code.
- `userspace/include/vk.h` is the userspace-facing raw wrapper around that ABI.
- `userspace/include/vkrt.h` is the stable runtime services layer that app code
  should prefer over direct kernel calls.

The raw ABI still includes the historical `vk_api_t` function table, but it
also exposes `vk_syscall(...)` through `vk_api_t::vk_syscall`. musl uses that
raw syscall path, while app code is being moved toward libc and `vkrt_*`
helpers instead of talking to the kernel ABI directly.

Representative runtime surface today:

| Layer | Examples |
|---|---|
| libc / libc++ | `printf`, `malloc`, `free`, `open`, `read`, `write`, `std::string`, `std::vector` |
| `vkrt_*` services | `vkrt_framebuffer_info`, `vkrt_poll_key`, `vkrt_run_cmdline`, `vkrt_task_snapshot`, `vkrt_kobj_rpc_json` |
| raw ABI | `vk_syscall`, `vk_api_t`, KObj/process/framebuffer primitives used by runtime glue |

`VK_API_VERSION` is currently `39`.

## Userspace Linking

Userspace programs are built as freestanding static PIE ELF64 binaries:

- `-nostdlib`
- `-static-pie`
- entrypoint `-e _start`
- no interpreter (`PT_INTERP` is rejected by the loader and by
  `scripts/check_userspace_elf.sh`)

The staged runtime sysroot lives under `userspace/runtime/sysroot` and is
prepared by `scripts/setup_userspace_runtime.sh`. It installs the headers and
archives used by app builds:

- musl C headers and `libc.a`
- `libm.a`
- libc++ headers plus `libc++.a` and `libc++abi.a`
- the runtime glue objects built from `userspace/libc/`

`userspace/libc/Makefile` builds four always-reused runtime objects:

| Object | Purpose |
|---|---|
| `crt0.o` | Entry bridge from `_start` into musl startup |
| `runtime_glue.o` | Shared `__dso_handle`, syscall dispatch, and `vkrt_*` wrappers |
| `c_main_bridge.o` | Maps the runtime entry to a normal C `main(int, char**)` |
| `cxx_main_bridge.o` | Maps the runtime entry to a normal C++ `main(int, char**)` |

### C program link flow

For a C target such as `hello.vbin`, the final link is conceptually:

```text
cc -nostdlib -static-pie -e _start \
  crt0.o runtime_glue.o c_main_bridge.o app.o \
  -Wl,--start-group -lc -lm -lgcc -Wl,--end-group
```

That gives the binary one shared entry path while keeping the app source
conventional: the program just defines `int main(int argc, char** argv)`.

### C++ program link flow

For a C++ target such as `shell.vbin` or `vkgui.vbin`, the final link is
conceptually:

```text
clang++ -nostdlib -static-pie -e _start \
  crt0.o runtime_glue.o cxx_main_bridge.o app_objects... \
  -Wl,--start-group -lc++ -lc++abi -lc -lm -lgcc -Wl,--end-group
```

The C++ build injects `-Dmain=__vkernel_cpp_main` through
`userspace/cxx_runtime_config.mk`. That lets application source keep writing a
normal `int main(...)`, while `cxx_main_bridge.o` provides the C-linkage bridge
that `crt0` calls. App code no longer needs to spell `extern "C" int main(...)`
just to satisfy the freestanding startup ABI.

## vkGUI

`vkgui.vbin` is the graphical shell. It uses Dear ImGui and a small
`imgui_impl_vk` backend over the kernel framebuffer/compositor API.

Current panels and features:

- Launch menu populated by scanning `/bin` for `.vbin` apps
- Window manager for graphical `.vbin` tasks
- Per-window framebuffer routing and input routing
- Console log window
- Task Manager panel
- KObj Navigator panel
- `vkfm` file manager and file preview/launch panel
- Info/settings panels and optional ImGui demo window

vkGUI depends on the tracked `userspace/vkgui/deps/` tree; the build expects
those vendored sources to already be present.

## Userspace Programs

| Binary | Description |
|---|---|
| `shell.vbin` | Interactive command shell |
| `vkgui.vbin` | Dear ImGui graphical shell/window manager |
| `hello.vbin` | Runtime, file, and stdio smoke test |
| `raytracer.vbin` | Realtime software raytracer |
| `rotozoom.vbin` | Rotate/zoom graphics effect |
| `sr_cube.vbin` | Software-rendered 3D cube |
| `cppcompat.vbin` | libc++ runtime smoke test |
| `vkobj.vbin` | KObj CLI and JSON RPC helper |
| `modplay.vbin` | MOD/S3M tracker playback |
| `minimp3.vbin` | MP3 playback through minimp3 |
| `vnes.vbin` | NES emulator frontend |
| `snes9x.vbin` | SNES emulator frontend |
| `vspcplay.vbin` | SPC music player frontend |
| `doom.vbin` | Chocolate Doom port |
| `quake.vbin` | Quake port |
| `clownmdemu.vbin` | Sega Mega Drive emulator |

## Shell Commands

```text
vk> help
```

| Command | Description |
|---|---|
| `help` or `?` | List commands |
| `version` | Kernel version/build info |
| `mem` | Heap and physical allocator stats |
| `tasks` | Scheduler task list |
| `top [once]` | Live CPU usage view; `once` exits after one sample |
| `ls [path]` | List KObj path children |
| `cat <file>` | Print a text file |
| `get <path>` | Read a KObj node |
| `set <path> <value>` | Write a writable KObj node |
| `watch <path> [interval_ms]` | Poll a KObj node |
| `describe <path>` | Show KObj metadata |
| `run <program> [args...]` | Launch a userspace program |
| `drvload <name>` | Load a registered driver |
| `drvunload <name>` | Unload a driver |
| `clear` | Clear console |
| `uptime` | Show scheduler uptime |
| `reboot` | Reboot |
| `idt` | Dump interrupt descriptor table |
| `alloc` | Heap allocation smoke test |
| `panic` | Trigger a test fault |
| `exit` | Exit shell |

Entering a staged `.vbin` name directly also attempts to launch it.

## Build

`userspace/` now lives in its own Git repository and is checked out here as a
submodule. Clone with `--recurse-submodules`, or run
`git submodule update --init --recursive` after cloning.

### Prerequisites

```bash
# Fedora
sudo dnf install gcc-c++ make qemu-system-x86-core edk2-ovmf mtools parted

# Ubuntu / Debian
sudo apt install build-essential qemu-system-x86 ovmf mtools parted
```

The top-level Makefile currently defaults to `x86_64-redhat-linux-`.
Override it if your cross toolchain uses another prefix:

```bash
make CROSS_PREFIX=x86_64-linux-gnu-
```

### Common targets

```bash
make                  # Build build/vkernel.efi
make DEBUG=1          # Debug build
make userspace        # Build all userspace .vbin programs
make disk             # Build build/vkernel_boot.img
make disasm           # Write build/vkernel.dis
make clean            # Remove build artifacts
make distclean        # Also remove generated runtime sysroot/build state
```

`make userspace` builds the libc glue and calls
`scripts/setup_userspace_runtime.sh` if the musl/libc++ sysroot is missing.
That runtime setup stages musl, libc++, libc++abi, and the userspace startup
objects before app links begin.

## Run

```bash
./run_qemu.sh
```

The QEMU launcher builds the disk image unless `--keep-disk` is provided. It
uses OVMF, Q35, 512 MiB RAM, 4 CPUs, SDL display, virtio VGA, virtio block,
AC97, and serial stdio.

Useful run modes:

```bash
./run_qemu.sh --debug      # Start QEMU paused with a GDB server on :1234
./run_qemu.sh --keep-disk  # Reuse build/vkernel_boot.img
```

## Debugging

```bash
make DEBUG=1 disk
./run_qemu.sh --debug
```

In another terminal:

```bash
gdb build/vkernel.efi \
  -ex 'set confirm off' \
  -ex 'set breakpoint pending on' \
  -ex 'source .vscode/find_kernel.py' \
  -ex 'target remote localhost:1234'
```

After UEFI loads the image, interrupt QEMU/GDB and run `find-kernel` to reload
symbols at the relocated runtime address.

Useful breakpoints include `vk::efi_main`, `sched::start`, `ap_init_secondary`,
`vk::panic`, `elf::load_into_address_space`, and `process::create_task`.

For userspace `.vbin` programs, the same helper script can load symbols at the
fixed process image base:

```gdb
find-process userspace/shell/shell.vbin
```

That lets GDB resolve userspace names and source lines using the binary you
built locally. For the first stop inside a userspace task, prefer hardware
breakpoints because software breakpoints depend on the task address space being
active:

```gdb
hbreak main
continue
```

VS Code also has a dedicated launch profile named
`Attach to vkernel userspace shell (QEMU/GDB, Linux)`. It auto-continues to a
temporary breakpoint at `vk::process::process_task_main`, then the Debug
Console only needs:

```text
-exec find-process userspace/shell/shell.vbin
-exec hbreak main
-exec continue
```

For arbitrary apps, use `Attach to vkernel userspace app (QEMU/GDB, Linux)`.
It prompts for a `.vbin` path such as `userspace/snes9x/snes9x.vbin`, arms
automatic symbol loading for that target, and stops at
`vk::process::process_task_main` when that specific app is launched inside the
guest.

If you want to stop before the app runs, a useful sequence is:

```gdb
b vk::process::process_task_main
continue
find-process userspace/shell/shell.vbin
hbreak main
continue
```

## Design Notes

**Position-independent PE.** The kernel is linked at base 0 and loaded by UEFI
as a PE image. Because the relocation section is intentionally empty,
`self_relocate()` patches pointer-sized values in `.data` using the load delta.

**Bootstrap table plus raw syscall ABI.** Userspace still receives `vk_api_t`
at entry, but musl now uses the raw `vk_syscall(...)` entry carried inside that
table. The long-term direction is libc and `vkrt_*` for apps, with raw ABI use
kept inside runtime glue.

**Address-space separation without privilege separation.** Each process has a
private CR3 and fixed virtual layout. This prevents raw heap/image virtual
addresses from colliding across processes, but it is not a security boundary
until ring transitions and real syscalls exist.

**FAT32 first, ramfs fallback.** Boot services still preload files from the ESP
before `ExitBootServices`, but normal runtime file access goes through the
mounted FAT32 volume when available.

**Graphical task routing.** vkGUI launches graphical apps with private backing
buffers, then asks the kernel to remap each task framebuffer into the
compositor shared region and routes keyboard/mouse events to the focused task.

**Serial-first logging.** Kernel logs start on serial output to avoid tearing
the graphical framebuffer. The log route can still be changed through KObj.

## Screenshots

### Doom

![Doom running on vkernel](doom.webp)

### Quake

![Quake running on vkernel](quake.webp)

The Quake base data is staged as `/data/quake/id1/pak0.pak`, and the Zeusbot
mod is staged as `/data/quake/zeusbot/progs.dat`. Launch it from the shell as
`quake -game zeusbot`, or start `/bin/quake_zeusbot.vbin` from vkGUI.

## Compiler Requirements

- GCC 14+ with `-std=c++26`
- Clang 17+ with `-std=c++2b`
- MSVC VS 2022 17.8+ with `/std:c++latest`

Kernel freestanding flags include:

```text
-ffreestanding -nostdlib -fno-exceptions -fno-rtti -mno-red-zone
```

## License

MIT License. See `LICENSE` for details.

## References

- [OSDev Wiki](https://wiki.osdev.org/)
- [UEFI Specification](https://uefi.org/specifications)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Tianocore EDK II](https://github.com/tianocore/edk2)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom)
