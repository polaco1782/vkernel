"""
find_kernel.py - GDB Python script to dynamically locate vkernel.efi in memory
and load symbols from vkernel.elf at the correct relocated address.

Usage (from GDB):
  source .vscode/find_kernel.py
  find-kernel
  find-process userspace/shell/shell.vbin
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
USER_IMAGE_BASE = 0x0000400000000000

_AUTO_LOAD_IN_PROGRESS = False
_LOADED_TEXT_ADDR = None
_LOADED_PROCESS_SYMBOLS = {}
_AUTO_PROCESS_PATH = None
_AUTO_PROCESS_BREAKPOINT = None
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


def align_up(value, alignment):
    if alignment <= 0:
        return value
    return (value + alignment - 1) & ~(alignment - 1)


def read_elf_u16(blob, offset):
    return struct.unpack_from("<H", blob, offset)[0]


def read_elf_u32(blob, offset):
    return struct.unpack_from("<I", blob, offset)[0]


def read_elf_u64(blob, offset):
    return struct.unpack_from("<Q", blob, offset)[0]


def resolve_process_elf_path(arg):
    token = arg.strip()
    if not token:
        raise gdb.GdbError("usage: find-process <path-to-.vbin-or-program-name>")

    workspace_root = os.path.normpath(os.path.join(_SCRIPT_DIR, ".."))
    variants = [token]
    if not token.endswith(".vbin"):
        variants.append(f"{token}.vbin")

    candidates = []
    for variant in variants:
        if os.path.isabs(variant):
            candidates.append(variant)
            continue
        candidates.append(os.path.join(workspace_root, variant))
        candidates.append(os.path.join(workspace_root, "userspace", variant))

    seen = set()
    for candidate in candidates:
        normalized = os.path.normpath(candidate)
        if normalized in seen:
            continue
        seen.add(normalized)
        if os.path.isfile(normalized):
            return normalized

    matches = []
    wanted_names = {os.path.basename(variant) for variant in variants}
    userspace_root = os.path.join(workspace_root, "userspace")
    for root, _, files in os.walk(userspace_root):
        for name in files:
            if name in wanted_names:
                matches.append(os.path.normpath(os.path.join(root, name)))

    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise gdb.GdbError(
            "multiple matching binaries found:\n  " + "\n  ".join(sorted(matches))
        )

    raise gdb.GdbError(
        f"could not find '{token}'. Try a path like userspace/shell/shell.vbin"
    )


def get_process_text_runtime_address(elf_path):
    with open(elf_path, "rb") as f:
        data = f.read()

    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise gdb.GdbError(f"{elf_path} is not an ELF64 binary")
    if data[4] != 2 or data[5] != 1:
        raise gdb.GdbError(f"{elf_path} is not a little-endian ELF64 binary")

    e_phoff = read_elf_u64(data, 0x20)
    e_shoff = read_elf_u64(data, 0x28)
    e_phentsize = read_elf_u16(data, 0x36)
    e_phnum = read_elf_u16(data, 0x38)
    e_shentsize = read_elf_u16(data, 0x3A)
    e_shnum = read_elf_u16(data, 0x3C)
    e_shstrndx = read_elf_u16(data, 0x3E)

    if e_phentsize != 56 or e_phnum == 0:
        raise gdb.GdbError(f"{elf_path} has no usable ELF program headers")
    if e_shentsize != 64 or e_shnum == 0 or e_shstrndx >= e_shnum:
        raise gdb.GdbError(f"{elf_path} has no usable ELF section headers")

    vaddr_min = None
    max_align = 0x1000
    for index in range(e_phnum):
        phoff = e_phoff + index * e_phentsize
        if phoff + e_phentsize > len(data):
            raise gdb.GdbError(f"{elf_path} has truncated program headers")

        p_type = read_elf_u32(data, phoff + 0x00)
        p_vaddr = read_elf_u64(data, phoff + 0x10)
        p_align = read_elf_u64(data, phoff + 0x30)
        if p_type != 1:
            continue
        if vaddr_min is None or p_vaddr < vaddr_min:
            vaddr_min = p_vaddr
        if p_align > max_align:
            max_align = p_align

    if vaddr_min is None:
        raise gdb.GdbError(f"{elf_path} has no PT_LOAD segments")

    shstr_hdr = e_shoff + e_shstrndx * e_shentsize
    shstr_off = read_elf_u64(data, shstr_hdr + 0x18)
    shstr_size = read_elf_u64(data, shstr_hdr + 0x20)
    if shstr_off + shstr_size > len(data):
        raise gdb.GdbError(f"{elf_path} has a truncated section-string table")

    shstr = data[shstr_off : shstr_off + shstr_size]
    text_vaddr = None
    for index in range(e_shnum):
        shoff = e_shoff + index * e_shentsize
        if shoff + e_shentsize > len(data):
            raise gdb.GdbError(f"{elf_path} has truncated section headers")

        sh_name = read_elf_u32(data, shoff + 0x00)
        sh_addr = read_elf_u64(data, shoff + 0x10)
        if sh_name >= len(shstr):
            continue
        name_end = shstr.find(b"\0", sh_name)
        if name_end == -1:
            continue
        if shstr[sh_name:name_end] == b".text":
            text_vaddr = sh_addr
            break

    if text_vaddr is None:
        raise gdb.GdbError(f"{elf_path} does not contain a .text section")

    image_base = align_up(USER_IMAGE_BASE, max_align)
    return image_base + (text_vaddr - vaddr_min)


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


def load_process_symbols(arg):
    elf_path = resolve_process_elf_path(arg)
    text_addr = get_process_text_runtime_address(elf_path)
    previous_addr = _LOADED_PROCESS_SYMBOLS.get(elf_path)
    if previous_addr == text_addr:
        gdb.write(
            f"[find-process] Symbols already loaded for {elf_path} "
            f"at .text={hex(text_addr)}\n"
        )
        return True

    gdb.write(f"[find-process] Loading symbols from {elf_path}\n")
    gdb.write(f"[find-process] Runtime .text address: {hex(text_addr)}\n")
    cmd = f'add-symbol-file "{elf_path}" {hex(text_addr)}'
    gdb.write(f"[find-process] Executing: {cmd}\n")
    gdb.execute(cmd, to_string=False)
    _LOADED_PROCESS_SYMBOLS[elf_path] = text_addr
    gdb.write(
        "[find-process] Done. Prefer 'hbreak <symbol>' for the first stop in a "
        "userspace task.\n"
    )
    return True


def current_frame_name():
    try:
        frame = gdb.selected_frame()
        if frame is None:
            return None
        return frame.name()
    except Exception:
        return None


def normalize_program_token(token):
    return os.path.basename(token.replace("\\", "/"))


def parse_program_path(command_line):
    if command_line is None:
        return None

    size = len(command_line)
    index = 0
    while index < size and command_line[index] in " \t\r\n":
        index += 1
    if index >= size:
        return None

    quote = None
    if command_line[index] in ('"', "'"):
        quote = command_line[index]
        index += 1

    start = index
    while index < size:
        ch = command_line[index]
        if quote is not None:
            if ch == quote:
                break
        elif ch in " \t\r\n":
            break
        index += 1

    if index <= start:
        return None
    return command_line[start:index]


def read_current_process_command_line():
    try:
        frame = gdb.selected_frame()
        if frame is None:
            return None

        try:
            cmd_len = int(
                gdb.parse_and_eval(
                    "((vk::process::process_task_context*)user_data)->command_line_len"
                )
            )
            if cmd_len <= 0:
                return None

            raw = gdb.parse_and_eval(
                "((vk::process::process_task_context*)user_data)->command_line"
            )
            try:
                return raw.string(length=cmd_len)
            except Exception:
                chars = []
                for index in range(cmd_len):
                    value = int(raw[index])
                    if value == 0:
                        break
                    chars.append(chr(value))
                return "".join(chars)
        except Exception:
            pass

        ctx = None
        try:
            ctx = frame.read_var("ctx")
        except Exception:
            ctx = None

        if ctx is None:
            try:
                user_data = frame.read_var("user_data")
                if user_data is None:
                    return None
                ctx_ptr_type = gdb.lookup_type(
                    "vk::process::process_task_context"
                ).pointer()
                ctx = user_data.cast(ctx_ptr_type)
            except Exception:
                return None

        if ctx is None:
            return None

        ctx_value = ctx.dereference()
        cmd_len = int(ctx_value["command_line_len"])
        if cmd_len <= 0:
            return None

        raw = ctx_value["command_line"]
        try:
            return raw.string(length=cmd_len)
        except Exception:
            chars = []
            for index in range(cmd_len):
                value = int(raw[index])
                if value == 0:
                    break
                chars.append(chr(value))
            return "".join(chars)
    except Exception:
        return None


def current_process_matches_target():
    if _AUTO_PROCESS_PATH is None:
        return False

    command_line = read_current_process_command_line()
    program = parse_program_path(command_line)
    if not program:
        return False

    target_name = normalize_program_token(_AUTO_PROCESS_PATH)
    program_name = normalize_program_token(program)
    return program_name == target_name


class ProcessTaskMainBreakpoint(gdb.Breakpoint):
    """Stops only when process_task_main is launching the armed target app."""

    def __init__(self, target_path):
        super().__init__("vk::process::process_task_main", internal=False)
        self.target_path = target_path

    def stop(self):
        command_line = read_current_process_command_line()
        program = parse_program_path(command_line)
        label = program if program else "<unknown>"
        target_name = normalize_program_token(self.target_path)

        if not current_process_matches_target():
            gdb.write(
                f"[find-process] Skipping process_task_main for {label}; waiting for {target_name}.\n"
            )
            return False

        if load_process_symbols(self.target_path):
            gdb.write(
                "[find-process] Auto-loaded userspace symbols at process_task_main. "
                "Set or re-toggle VS Code source breakpoints now, or use 'hbreak main'.\n"
            )
        return True


def clear_auto_process_breakpoint():
    global _AUTO_PROCESS_BREAKPOINT

    if _AUTO_PROCESS_BREAKPOINT is None:
        return

    try:
        _AUTO_PROCESS_BREAKPOINT.delete()
    except Exception:
        pass
    _AUTO_PROCESS_BREAKPOINT = None


def arm_auto_process_breakpoint(target_path):
    global _AUTO_PROCESS_BREAKPOINT

    clear_auto_process_breakpoint()
    _AUTO_PROCESS_BREAKPOINT = ProcessTaskMainBreakpoint(target_path)


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

    if _AUTO_LOAD_IN_PROGRESS:
        return

    _AUTO_LOAD_IN_PROGRESS = True
    try:
        if _LOADED_TEXT_ADDR is None:
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


class FindProcessCommand(gdb.Command):
    """Load symbols for a userspace .vbin at the fixed process image base."""

    def __init__(self):
        super().__init__("find-process", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        try:
            load_process_symbols(arg)
        except gdb.GdbError as exc:
            gdb.write(f"[find-process] ERROR: {exc}\n")
            gdb.write(
                "  -> Example: find-process userspace/shell/shell.vbin\n"
            )
            gdb.write(
                "  -> In VS Code debug console: -exec find-process userspace/shell/shell.vbin\n"
            )


class AutoProcessSymbolsCommand(gdb.Command):
    """Arm automatic userspace symbol loading on process_task_main stops."""

    def __init__(self):
        super().__init__("auto-process-symbols", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        global _AUTO_PROCESS_PATH

        token = arg.strip()
        if not token:
            _AUTO_PROCESS_PATH = None
            clear_auto_process_breakpoint()
            gdb.write("[find-process] Cleared auto process symbol loading.\n")
            return

        try:
            resolved = resolve_process_elf_path(token)
        except gdb.GdbError as exc:
            gdb.write(f"[find-process] ERROR: {exc}\n")
            gdb.write(
                "  -> In VS Code debug console: -exec auto-process-symbols userspace/shell/shell.vbin\n"
            )
            return

        _AUTO_PROCESS_PATH = resolved
        arm_auto_process_breakpoint(_AUTO_PROCESS_PATH)
        gdb.write(
            f"[find-process] Auto process symbol loading armed for {_AUTO_PROCESS_PATH}\n"
        )
        gdb.write(
            "[find-process] A process_task_main breakpoint will stop only for that target app.\n"
        )


FindKernelCommand()
FindProcessCommand()
AutoProcessSymbolsCommand()
if not _STOP_HANDLER_CONNECTED:
    gdb.events.stop.connect(on_stop)
    _STOP_HANDLER_CONNECTED = True
ensure_mailbox_watchpoint()
gdb.write(
    "[find-kernel] Script loaded. Kernel symbols auto-load on kernel entry; "
    "use 'find-process <app.vbin>' or 'auto-process-symbols <app.vbin>' for userspace symbols.\n"
)
gdb.write(
    "[find-kernel] VS Code debug console users should prefix custom commands with "
    "'-exec '.\n"
)
