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
#include "spinlock.h"
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

inline constexpr usize TASK_STACK_SIZE = 1048576; /* 1 MiB per task (Quake needs more headroom) */
inline constexpr usize MAX_TASKS = 64;
inline constexpr u32 SCHED_TICK_HZ = 100;        /* 10 ms scheduler tick */
inline constexpr u32 SCHED_CPU_NONE = 0xFFFF'FFFFu;

/* Sentinel: CPU is idle and not currently executing any task.
 * Used as the initial g_per_cpu_task value for APs so that their first
 * LAPIC-timer preemption does not corrupt an existing task's saved state. */
inline constexpr usize SCHED_NO_TASK = MAX_TASKS;

using task_entry_fn = void(*)(void*);

struct task {
    u64         rsp;                        /* Saved stack pointer */
    u64         id;
    task_state  state;
    u64         wake_tick;
    static_string<32> name;
    void*       user_data;
    #if defined(_MSC_VER)
        __declspec(align(16)) u8 stack[TASK_STACK_SIZE];
        /* XSAVE area: 512 bytes, must be 64-byte aligned. Saves
         * AVX + x87 + MMX + SSE state (XMM0..XMM15 + MXCSR + FPU regs). */
        __declspec(align(64)) u8 xsave_area[2696];
    #else
        u8 stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
        u8 xsave_area[2696] __attribute__((aligned(64)));
    #endif
    task_entry_fn entry;
    bool          xsave_valid;             /* true after first XSAVE for this task */
    bool          allow_secondary_cpu;     /* tasks created before scheduler start stay on BSP */
    u32           current_cpu;             /* APIC ID while running, SCHED_CPU_NONE otherwise */
    u64           cpu_ticks;               /* Timer ticks consumed while running */

    [[nodiscard]] constexpr auto is_runnable() const -> bool {
        return state == task_state::ready || state == task_state::running;
    }
};

struct task_snapshot {
    u64        id;
    task_state state;
    u32        cpu;
    u64        cpu_ticks;
    string_view name;
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
[[nodiscard]] auto create_task(string_view name, task_entry_fn entry, void* user_data = null) -> i64;
[[nodiscard]] auto create_task(const char* name, task_entry_fn entry, void* user_data = null) -> i64;

/* Start the scheduler on the BSP — does not return */
[[noreturn]] void start();

/*
 * Start the scheduler on an Application Processor.
 * Called from ap_init_secondary() after arch::ap_activate().
 * Enables LAPIC timer, then enters the idle/dispatch loop.
 * Does not return.
 */
[[noreturn]] void start_ap();

/* Yield CPU to next runnable task */
void yield();

/* Block current task for N scheduler ticks */
void sleep(u64 ticks);

/* Wait until specified task terminates */
void wait_for_task(u64 task_id);

/* Terminate current task — does not return to caller */
[[noreturn]] void exit_task();

/*
 * Detach the current task from any process context: atomically
 * read its user_data, null it out in the task slot, and mark the
 * task terminated.  Returns the previous user_data pointer (or
 * null if there was no current task or it was already detached).
 *
 * Used by exception handlers BEFORE freeing the process resources,
 * so that any nested fault that re-enters the dispatcher will not
 * see a dangling user_data pointer and try to clean up the same
 * process twice.
 */
auto detach_current_task() -> void*;

/* Query current task info */
[[nodiscard]] auto current_task_id() -> u64;
[[nodiscard]] auto current_task_name_view() -> string_view;
[[nodiscard]] auto current_task_name() -> const char*;
[[nodiscard]] auto current_task_user_data() -> void*;
[[nodiscard]] auto task_user_data(u64 task_id) -> void*;
[[nodiscard]] auto tick_count() -> u64;
[[nodiscard]] auto snapshot_tasks(task_snapshot* out, usize max_tasks) -> usize;

/* Debug: print task list */
void dump_tasks();

/*
 * Preemption handler — called from timer ISR (both PIT on BSP and
 * LAPIC timer on APs).  Saves current context, picks next runnable
 * task for this CPU, returns new context pointer.
 */
[[nodiscard]] auto preempt(arch::register_state* regs) -> arch::register_state*;

} // namespace sched

} // namespace vk

#endif /* VKERNEL_SCHEDULER_H */
