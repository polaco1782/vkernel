/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * reloc_stub.cpp - Minimal PE base-relocation block
 *
 * objcopy sees a non-empty .reloc section and therefore:
 *   1. Copies it as the PE Base Relocation Table (Data Directory entry 5).
 *   2. Does NOT set IMAGE_FILE_RELOCS_STRIPPED in the COFF Characteristics.
 *
 * The firmware (OVMF / EDK II) then knows it may load the image at any
 * available address.  Since all code is RIP-relative (-fpic), zero actual
 * fixups are needed — the single empty block below carries that meaning:
 *
 *   DWORD VirtualAddress = 0
 *   DWORD SizeOfBlock    = 8   (header only, no TypeOffset[] entries)
 */

#if defined(_MSC_VER)
#pragma section(".reloc", read)
__declspec(allocate(".reloc")) constexpr unsigned int pe_reloc_stub[2] = {0u, 8u};
#else
[[gnu::section(".reloc"), gnu::used]]
constexpr unsigned int pe_reloc_stub[2] = {0u, 8u};
#endif
