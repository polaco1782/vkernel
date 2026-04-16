/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * shell.h - Built-in kernel shell
 */

#ifndef VKERNEL_SHELL_H
#define VKERNEL_SHELL_H

#include "types.h"

namespace vk {
namespace shell {

/* Shell task entry point — runs as a scheduler task, never returns */
void shell_main();

} // namespace shell
} // namespace vk

#endif /* VKERNEL_SHELL_H */
