"""
find_kernel.py - GDB Python script to dynamically locate vkernel.efi in memory
and load symbols from vkernel.elf at the correct relocated address.

Usage (from GDB):
  source .vscode/find_kernel.py
  find-kernel
"""

import gdb
import struct
import os

try:
    _SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _SCRIPT_DIR = os.getcwd()

# ---- constants baked from the ELF/PE at build time ----
TEXT_RVA      = 0x1000      # .text section RVA within the PE image (linker.ld: .text at 0x1000)
PE_E_LFANEW   = 0x80        # offset of "PE\0\0" signature in our EFI image

# Memory scan parameters
SCAN_START    = 0x10000000  # typical UEFI runtime region start
SCAN_END      = 0x30000000  # upper bound
SCAN_STEP     = 0x1000      # 4 KB pages

GDB_MAILBOX_IMAGE_BASE = 0x8160
GDB_MAILBOX_RELEASE = 0x8168
GDB_RELEASE_MAGIC = 0x564B4442474F  # "VKDBGO"

_AUTO_LOAD_IN_PROGRESS = False
_LOADED_TEXT_ADDR = None
_STOP_HANDLER_CONNECTED = False
_MAILBOX_WATCHPOINT_SET = False


def get_entry_rva_from_file(elf_dir):
    """Read the EFI PE AddressOfEntryPoint RVA from the built vkernel.efi."""
    efi_path = os.path.join(elf_dir, "vkernel.efi")
    try:
        with open(efi_path, "rb") as f:
            data = f.read(512)  # Only need the headers
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        # AddressOfEntryPoint is at OptionalHeader + 16, OptionalHeader starts at PE + 24
        entry_rva = struct.unpack_from("<I", data, e_lfanew + 24 + 16)[0]
        return entry_rva
    except Exception:
        return None


def read_phys_u16(addr):
    """Read a 16-bit value from target memory, return None on error."""
    try:
        inf = gdb.selected_inferior()
        data = inf.read_memory(addr, 2)
        return struct.unpack_from('<H', bytes(data))[0]
    except Exception:
        return None


def read_phys_u32(addr):
    """Read a 32-bit value from target memory, return None on error."""
    try:
        inf = gdb.selected_inferior()
        data = inf.read_memory(addr, 4)
        return struct.unpack_from('<I', bytes(data))[0]
    except Exception:
        return None


def read_phys_u64(addr):
    """Read a 64-bit value from target memory, return None on error."""
    try:
        inf = gdb.selected_inferior()
        data = inf.read_memory(addr, 8)
        return struct.unpack_from('<Q', bytes(data))[0]
    except Exception:
        return None


def write_phys_u64(addr, value):
    """Write a 64-bit value to target memory."""
    inf = gdb.selected_inferior()
    inf.write_memory(addr, struct.pack('<Q', value))


def find_kernel_base(quiet=False):
    """
    Scan physical memory for the vkernel PE image.
    Returns the image base address or None if not found.
    Identifies the image by: MZ magic + PE signature + matching entry point RVA.
    """
    # Determine expected entry RVA from the local EFI file
    script_dir = _SCRIPT_DIR
    build_dir = os.path.join(script_dir, "..", "build")
    expected_entry_rva = get_entry_rva_from_file(build_dir)
    if expected_entry_rva is None and not quiet:
        gdb.write("[find-kernel] WARNING: Could not read vkernel.efi, "
                  "falling back to e_lfanew-only matching.\n")

    if not quiet:
        gdb.write(f"[find-kernel] Scanning {hex(SCAN_START)}..{hex(SCAN_END)} "
                  f"in {hex(SCAN_STEP)}-byte steps...\n")
    if expected_entry_rva and not quiet:
        gdb.write(f"[find-kernel] Matching entry RVA: {hex(expected_entry_rva)}\n")

    addr = SCAN_START
    while addr < SCAN_END:
        # Check for MZ magic
        mz = read_phys_u16(addr)
        if mz == 0x5A4D:  # 'MZ'
            # Check e_lfanew matches our expected value
            e_lfanew = read_phys_u32(addr + 0x3C)
            if e_lfanew == PE_E_LFANEW:
                # Check PE signature
                pe_sig = read_phys_u32(addr + PE_E_LFANEW)
                if pe_sig == 0x00004550:  # 'PE\0\0'
                    # Verify entry point RVA matches
                    entry_rva = read_phys_u32(addr + PE_E_LFANEW + 24 + 16)
                    if expected_entry_rva is None or entry_rva == expected_entry_rva:
                        if not quiet:
                            gdb.write(f"[find-kernel] Found vkernel.efi at {hex(addr)}\n")
                        return addr
        addr += SCAN_STEP

    return None


def get_kernel_elf_path():
    script_dir = _SCRIPT_DIR
    elf_path = os.path.join(script_dir, "..", "build", "vkernel.elf")
    return os.path.normpath(elf_path)


def load_kernel_symbols(quiet_scan=False, announce_mode="manual", image_base=None):
    global _LOADED_TEXT_ADDR

    elf_path = get_kernel_elf_path()
    if not os.path.exists(elf_path):
        if not quiet_scan:
            gdb.write(f"[find-kernel] ERROR: {elf_path} not found. "
                      "Run: make DEBUG=1\n")
        return False

    if image_base is None:
        image_base = find_kernel_base(quiet=quiet_scan)
    if image_base is None:
        return False

    text_addr = image_base + TEXT_RVA
    if _LOADED_TEXT_ADDR == text_addr:
        if not quiet_scan:
            gdb.write(f"[find-kernel] Symbols already loaded at .text={hex(text_addr)}\n")
        return True

    if announce_mode == "auto":
        gdb.write(f"[find-kernel] Auto-loading symbols: image_base={hex(image_base)} "
                  f".text={hex(text_addr)}\n")
    else:
        gdb.write(f"[find-kernel] image_base={hex(image_base)} "
                  f".text={hex(text_addr)}\n")
        gdb.write(f"[find-kernel] Loading symbols from {elf_path}\n")

    cmd = f'add-symbol-file "{elf_path}" {hex(text_addr)}'
    if announce_mode != "auto":
        gdb.write(f"[find-kernel] Executing: {cmd}\n")
    gdb.execute(cmd, to_string=False)
    _LOADED_TEXT_ADDR = text_addr
    gdb.write("[find-kernel] Done. You can now set breakpoints.\n")
    return True


def ensure_mailbox_watchpoint():
    global _MAILBOX_WATCHPOINT_SET

    if _MAILBOX_WATCHPOINT_SET:
        return

    try:
        gdb.execute(
            f"watch -location *(unsigned long long*){hex(GDB_MAILBOX_IMAGE_BASE)}",
            to_string=True,
        )
        _MAILBOX_WATCHPOINT_SET = True
        gdb.write(
            f"[find-kernel] Watching mailbox at {hex(GDB_MAILBOX_IMAGE_BASE)} "
            "for kernel entry.\n"
        )
    except Exception as exc:
        gdb.write(
            "[find-kernel] WARNING: could not install kernel-entry watchpoint: "
            f"{exc}\n"
        )


def on_stop(event):
    global _AUTO_LOAD_IN_PROGRESS

    if _AUTO_LOAD_IN_PROGRESS or _LOADED_TEXT_ADDR is not None:
        return

    _AUTO_LOAD_IN_PROGRESS = True
    try:
        image_base = read_phys_u64(GDB_MAILBOX_IMAGE_BASE)
        if image_base:
            if load_kernel_symbols(
                quiet_scan=True, announce_mode="auto", image_base=image_base
            ):
                write_phys_u64(GDB_MAILBOX_RELEASE, GDB_RELEASE_MAGIC)
                gdb.write("[find-kernel] Released kernel debug wait.\n")
        else:
            load_kernel_symbols(quiet_scan=True, announce_mode="auto")
    except Exception:
        # Ignore transient failures while the firmware is still running.
        pass
    finally:
        _AUTO_LOAD_IN_PROGRESS = False


class FindKernelCommand(gdb.Command):
    """Locate vkernel.efi in memory and load symbols from vkernel.elf."""

    def __init__(self):
        super().__init__("find-kernel", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if not load_kernel_symbols(quiet_scan=False, announce_mode="manual"):
            gdb.write("[find-kernel] Kernel not in memory yet.\n")
            gdb.write("  -> 'continue' to let UEFI load the kernel, then\n")
            gdb.write("     pause and run 'find-kernel' again.\n")
            gdb.write("  -> In VS Code debug console: -exec find-kernel\n")
            return


FindKernelCommand()
if not _STOP_HANDLER_CONNECTED:
    gdb.events.stop.connect(on_stop)
    _STOP_HANDLER_CONNECTED = True
ensure_mailbox_watchpoint()
gdb.write("[find-kernel] Script loaded. Symbols will auto-load on kernel entry.\n")
