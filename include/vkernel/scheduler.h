/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * scheduler.h - Round-robin preemptive scheduler interface
 */

#ifndef VKERNEL_SCHEDULER_H
#define VKERNEL_SCHEDULER_H

#include "config.h"
#include "types.h"
#include "arch/x86_64/arch.h"

namespace vk {

/* ============================================================
 * Task configuration
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

inline constexpr usize TASK_STACK_SIZE = 262144;  /* 256 KB per task (enough for Doom) */
inline constexpr usize MAX_TASKS = 64;

using task_entry_fn = void(*)(void*);

struct task {
    u64         rsp;                        /* Saved stack pointer */
    u64         id;
    task_state  state;
    u64         wake_tick;
    char        name[32];
    void*       user_data;
    #if defined(_MSC_VER)
        __declspec(align(16)) u8 stack[TASK_STACK_SIZE];
    #else
        u8 stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
    #endif
    task_entry_fn entry;

    [[nodiscard]] constexpr auto is_runnable() const -> bool {
        return state == task_state::ready || state == task_state::running;
    }
};

/* ============================================================
 * Scheduler API
 * ============================================================ */

namespace sched {

[[nodiscard]] auto init() -> status_code;

/*
 * Create a new task.
 * @param name     Human-readable task name (copied internally)
 * @param entry    Entry point function (MSVC x64 ABI: arg in RCX)
 * @param user_data First argument passed to entry function
 * @return Task ID on success, -1 on failure
 */
[[nodiscard]] auto create_task(const char* name, task_entry_fn entry, void* user_data = null) -> i64;

/* Start the scheduler — does not return */
[[noreturn]] void start();

/* Yield CPU to next runnable task */
void yield();

/* Block current task for N scheduler ticks */
void sleep(u64 ticks);

/* Wait until specified task terminates */
void wait_for_task(u64 task_id);

/* Terminate current task — does not return to caller */
[[noreturn]] void exit_task();

/* Query current task info */
[[nodiscard]] auto current_task_id() -> u64;
[[nodiscard]] auto current_task_name() -> const char*;
[[nodiscard]] auto current_task_user_data() -> void*;
[[nodiscard]] auto tick_count() -> u64;

/* Debug: print task list */
void dump_tasks();

/*
 * Preemption handler — called from timer ISR.
 * Saves current context, picks next task, returns new context pointer.
 * The assembly ISR stub uses the returned value to restore registers.
 */
[[nodiscard]] auto preempt(arch::register_state* regs) -> arch::register_state*;

} // namespace sched

} // namespace vk

#endif /* VKERNEL_SCHEDULER_H */