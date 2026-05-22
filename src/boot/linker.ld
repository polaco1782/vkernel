/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * Linker script for x86_64 UEFI application (PIC / static)
 */

OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(efi_main)

SECTIONS
{
    /* Keep RVA 0 reserved for PE headers; sections start at 0x1000. */
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

    /* BSS + fixed boot heap (zero-initialised, not stored in the image) */
    .bss (NOLOAD) :
    {
        _bss = .;
        *(.bss .bss.*)
        *(COMMON)
        _ebss = .;

        . = ALIGN(4096);
        _heap_start = .;
        KEEP(*(.heap .heap.*))
        _heap_end = .;
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
