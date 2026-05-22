/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * reloc_stub.cpp - Minimal PE base-relocation block
 */

#if defined(_MSC_VER)
#pragma section(".reloc", read)
__declspec(allocate(".reloc")) constexpr unsigned int pe_reloc_stub[2] = {0u, 8u};
#else
[[gnu::section(".reloc"), gnu::used]]
constexpr unsigned int pe_reloc_stub[2] = {0u, 8u};
#endif
