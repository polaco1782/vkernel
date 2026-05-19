#ifndef VKERNEL_ARCH_X86_64_EXCEPTION_DEBUG_H
#define VKERNEL_ARCH_X86_64_EXCEPTION_DEBUG_H

#include "arch.h"

namespace vk {
namespace process {
struct process_task_context;
}
namespace arch {

void log_exception_backtrace(const register_state* regs,
                             const process::process_task_context* ctx);

} // namespace arch
} // namespace vk

#endif /* VKERNEL_ARCH_X86_64_EXCEPTION_DEBUG_H */
