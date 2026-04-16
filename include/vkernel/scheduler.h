/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * scheduler.h - Cooperative/preemptive round-robin task scheduler
 */

#ifndef VKERNEL_SCHEDULER_H
#define VKERNEL_SCHEDULER_H

#include "types.h"
#include "arch/x86_64/arch.h"

namespace vk {

/* ============================================================
 * Task state
 * ============================================================ */

enum class task_state : u8 {
    ready = 0,
    running,
    blocked,
    terminated,
};

/* ============================================================
 * Task Control Block (TCB)
 * ============================================================ */

inline constexpr usize TASK_STACK_SIZE = 16384;  /* 16 KB per task */
inline constexpr usize MAX_TASKS = 64;

using task_entry_fn = void(*)(void*);

struct task {
    u64         rsp;                        /* Saved stack pointer */
    u64         id;
    task_state  state;
    u64         wake_tick;
    char        name[32];
    void*       user_data;
    u8          stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
    task_entry_fn entry;

    [[nodiscard]] constexpr auto is_runnable() const -> bool {
        return state == task_state::ready || state == task_state::running;
    }
};

/* ============================================================
 * Scheduler interface
 * ============================================================ */

namespace sched {

auto init() -> status_code;

/* Create a new kernel task. Returns task ID or -1 on failure. */
auto create_task(const char* name, task_entry_fn entry, void* user_data = null) -> i64;

/* Yield CPU to next ready task (cooperative) */
void yield();

/* Called from timer IRQ to preempt the current task.
 * Returns pointer to the register_state to restore (may differ on switch). */
auto preempt(arch::register_state* regs) -> arch::register_state*;

/* Start the scheduler — does not return */
VK_NORETURN void start();

/* Get current task ID */
auto current_task_id() -> u64;

/* Get current task name */
auto current_task_name() -> const char*;

/* Get current task user data */
auto current_task_user_data() -> void*;

/* Get current scheduler tick count */
auto tick_count() -> u64;

/* Sleep current task for at least the given number of ticks */
void sleep(u64 ticks);

/* Terminate the calling task */
VK_NORETURN void exit_task();

/* Dump task list */
void dump_tasks();

} // namespace sched

} // namespace vk

#endif /* VKERNEL_SCHEDULER_H */
