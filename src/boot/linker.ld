/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * Linker script for x86_64 UEFI application (PIC / static)
 *
 * The kernel is compiled with -fpic so all internal references use
 * RIP-relative addressing.  reloc_stub.cpp injects an 8-byte
 * .reloc section with an empty base-relocation block, which:
 *   1. Prevents objcopy from setting IMAGE_FILE_RELOCS_STRIPPED.
 *   2. Tells the UEFI PE loader "zero fixups to apply — load anywhere".
 */

OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(efi_main)

SECTIONS
{
    /*
     * Base at 0 — image is fully position-independent.
     * Start sections at 0x1000 so RVA 0 is reserved for the PE header
     * (SizeOfHeaders ≤ 0x1000).
     */
    . = 0;
    ImageBase = .;
    . = 0x1000;

    /* Code */
    .text :
    {
        _text = .;
        *(.text .text.*)
        _etext = .;
    }

    . = ALIGN(4096);

    /* PE base-relocation table (8-byte stub from reloc_stub.cpp) */
    .reloc : { *(.reloc) }

    . = ALIGN(4096);

    /* Read-only data */
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);

    /* Initialised data + GOT */
    .data :
    {
        _data = .;
        *(.got.plt)
        _got_start = .;
        *(.got)
        _got_end = .;
        *(.data .data.*)
        *(.sdata)
        _edata = .;
    }

    . = ALIGN(4096);

    /* BSS (zero-initialised) */
    .bss :
    {
        _bss = .;
        *(.bss .bss.*)
        *(COMMON)
        _ebss = .;
    }

    _end = .;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note.*)
        *(.comment)
        *(.hash)
        *(.gnu.hash)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.rel.reloc)
        *(.note.GNU-stack)
    }
}
