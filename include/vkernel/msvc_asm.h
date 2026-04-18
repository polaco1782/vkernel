/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * msvc_asm.h - Compatibility redirect to arch/x86_64/msvc_asm.h
 *
 * The canonical header now lives in arch/x86_64/.  Include this file
 * only from non-arch-specific code that still needs the declarations;
 * arch code should include "msvc_asm.h" relative to its own directory.
 */

#ifndef VKERNEL_MSVC_ASM_H
#define VKERNEL_MSVC_ASM_H

#include "arch/x86_64/msvc_asm.h"

#endif /* VKERNEL_MSVC_ASM_H */
