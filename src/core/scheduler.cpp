/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * scheduler.cpp - Round-robin preemptive scheduler
 *
 * Uses the PIT (8254) on the BSP and the local APIC timer on APs
 * for preemption (both on vector 32).  Context switches save/restore
 * RSP; the full register file is pushed by the ISR stub in interrupts.S.
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "scheduler.h"
#include "panic.h"
#include "process_internal.h"
#include "virtual_memory.h"
#include "arch/x86_64/arch.h"
#include "smp.h"

namespace vk {

/* ============================================================
 * Internal state
 * ============================================================ */

static task   g_tasks[MAX_TASKS];
static usize  g_task_count       = 0;
static u64    g_next_task_id     = 1;
static volatile bool g_scheduler_active = false;
static volatile u64  g_tick_count       = 0;
static constexpr usize CPU_IDLE_STACK_SIZE = 4096;

/*
 * Per-CPU current-task index.
 * Indexed by APIC ID (0-based, matches smp::MAX_CPUS).
 * The BSP is always APIC ID 0 on a standard PC; on unusual hardware
 * the BSP's APIC ID might differ — we map via smp::current_cpu_apic_id().
 */
static constexpr u32 MAX_APIC_IDS = 256;
static usize g_per_cpu_task[MAX_APIC_IDS];   /* current task index per CPU */
static u64 g_cpu_idle_rsp[MAX_APIC_IDS];

#if defined(_MSC_VER)
static __declspec(align(16)) u8 g_cpu_idle_stacks[MAX_APIC_IDS][CPU_IDLE_STACK_SIZE];
#else
static u8 g_cpu_idle_stacks[MAX_APIC_IDS][CPU_IDLE_STACK_SIZE] __attribute__((aligned(16)));
#endif

/* Global scheduler spinlock — held only during the brief task-pick window */
static spinlock g_sched_lock;

/* Per-CPU yield-in-progress flag.  Must not be a single global because
 * one CPU's yield flag would be consumed by another CPU's timer ISR. */
static volatile bool g_yield_in_progress[MAX_APIC_IDS];

/* APIC ID of the Bootstrap Processor — only the BSP drives g_tick_count
 * via the PIT (IRQ0).  APs use their LAPIC timer for preemption only;
 * they must not increment the global tick or sleep timers will run N× fast. */
static u8 g_bsp_apic_id = 0;

static constexpr usize FXSAVE_MXCSR_OFFSET      = 24;
static constexpr usize FXSAVE_MXCSR_MASK_OFFSET = 28;
static constexpr u32   FXSAVE_MXCSR_DEFAULT_MASK = 0x0000FFBFu;

static void sanitize_fxsave_area(u8* area) {
    auto* mxcsr = reinterpret_cast<u32*>(area + FXSAVE_MXCSR_OFFSET);
    auto* mxcsr_mask = reinterpret_cast<u32*>(area + FXSAVE_MXCSR_MASK_OFFSET);

    u32 valid_mask = *mxcsr_mask;
    if (valid_mask == 0 || (valid_mask & ~FXSAVE_MXCSR_DEFAULT_MASK) != 0) {
        valid_mask = FXSAVE_MXCSR_DEFAULT_MASK;
        *mxcsr_mask = valid_mask;
    }

    *mxcsr &= valid_mask;
}

static auto find_available_task_slot() -> usize {
    for (usize i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].state == task_state::terminated &&
            g_tasks[i].current_cpu == SCHED_CPU_NONE) {
            return i;
        }
    }

    if (g_task_count < MAX_TASKS) {
        return g_task_count;
    }

    return SCHED_NO_TASK;
}

/* ============================================================
 * PIT (Programmable Interval Timer) helpers — 8254 chip
 * Channel 0 → IRQ 0 → IDT vector 32
 * ============================================================ */

static constexpr u16 PIT_CH0_DATA = 0x40;
static constexpr u16 PIT_CMD      = 0x43;
static constexpr u32 PIT_FREQ     = 1193182;  /* Hz */
static constexpr u32 SCHED_HZ     = SCHED_TICK_HZ;  /* shared scheduler tick rate */

/* PIC (8259A) ports */
static constexpr u16 PIC1_CMD  = 0x20;
static constexpr u16 PIC1_DATA = 0x21;
static constexpr u16 PIC2_CMD  = 0xA0;
static constexpr u16 PIC2_DATA = 0xA1;

/* Local APIC MMIO (used for AP timer preemption and universal EOI) */
static constexpr u32 MSR_IA32_APIC_BASE  = 0x1B;
static constexpr u64 APIC_BASE_ENABLE    = (1ULL << 11);
static constexpr u64 APIC_BASE_PHYS_MASK = 0x0000'0000'FFFF'F000ULL;
static constexpr u32 LAPIC_ID            = 0x020;
static constexpr u32 LAPIC_EOI           = 0x0B0;
static constexpr u32 LAPIC_LVT_TIMER     = 0x320;
static constexpr u32 LAPIC_INIT_COUNT    = 0x380;
static constexpr u32 LAPIC_CUR_COUNT     = 0x390;
static constexpr u32 LAPIC_DIVIDE_CONFIG = 0x3E0;

static constexpr u32 LAPIC_TIMER_VECTOR         = 32;
static constexpr u32 LAPIC_TIMER_MASKED         = (1u << 16);
static constexpr u32 LAPIC_TIMER_PERIODIC       = (1u << 17);
static constexpr u32 LAPIC_TIMER_DIVIDE_BY_16   = 0x3;
static constexpr u32 LAPIC_TIMER_SAMPLE_TICKS   = 5;
static constexpr u32 LAPIC_TIMER_MIN_INIT_COUNT = 0x1000;

static void pic_remap() {
    /* Save masks */
    u8 mask1 = arch::inb(PIC1_DATA);
    u8 mask2 = arch::inb(PIC2_DATA);

    log::debug() << "PIC: remapping — IRQ0→vec32, IRQ8→vec40";

    /* ICW1: init + cascade + ICW4 needed */
    arch::outb(PIC1_CMD, 0x11);
    arch::outb(PIC2_CMD, 0x11);

    /* ICW2: vector offsets — IRQ0 → 32, IRQ8 → 40 */
    arch::outb(PIC1_DATA, 32);
    arch::outb(PIC2_DATA, 40);

    /* ICW3: master has slave on IRQ2, slave ID 2 */
    arch::outb(PIC1_DATA, 4);
    arch::outb(PIC2_DATA, 2);

    /* ICW4: 8086 mode */
    arch::outb(PIC1_DATA, 0x01);
    arch::outb(PIC2_DATA, 0x01);

    /* Restore masks — mask everything initially */
    arch::outb(PIC1_DATA, 0xFF);
    arch::outb(PIC2_DATA, 0xFF);
}

static void pit_init() {
    u16 divisor = static_cast<u16>(PIT_FREQ / SCHED_HZ);

    log::debug() << "PIT: divisor=" << log::hex(static_cast<u64>(divisor), 1, true, false) << " (target " << SCHED_HZ << " Hz)";

    /* Channel 0, lobyte/hibyte, rate generator */
    arch::outb(PIT_CMD, 0x36);
    arch::outb(PIT_CH0_DATA, static_cast<u8>(divisor & 0xFF));
    arch::outb(PIT_CH0_DATA, static_cast<u8>((divisor >> 8) & 0xFF));
}

static void pic_unmask_irq0() {
    u8 mask = arch::inb(PIC1_DATA);
    mask &= ~(1u << 0);  /* Clear bit 0 → unmask IRQ0 */
    arch::outb(PIC1_DATA, mask);
}

static void pic_eoi() {
    arch::outb(PIC1_CMD, 0x20);
}

static inline auto lapic_base() -> volatile u32* {
    const u64 apic_base = arch::rdmsr(MSR_IA32_APIC_BASE);
    if ((apic_base & APIC_BASE_ENABLE) == 0) {
        return null;
    }

    return reinterpret_cast<volatile u32*>(
        static_cast<usize>(apic_base & APIC_BASE_PHYS_MASK));
}

static inline auto lapic_read(u32 offset) -> u32 {
    auto* lapic = lapic_base();
    if (lapic == null) {
        return 0;
    }

    return lapic[offset / 4];
}

static inline void lapic_write(u32 offset, u32 value) {
    auto* lapic = lapic_base();
    if (lapic == null) {
        return;
    }

    lapic[offset / 4] = value;
    (void)lapic[LAPIC_ID / 4];
}

static void lapic_timer_prepare_masked() {
    lapic_write(LAPIC_DIVIDE_CONFIG, LAPIC_TIMER_DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_MASKED | LAPIC_TIMER_VECTOR);
}

static auto lapic_timer_calibrate() -> u32 {
    while (!g_scheduler_active) {
        arch::cpu_pause();
    }

    lapic_timer_prepare_masked();

    u64 tick = g_tick_count;
    while (g_tick_count == tick) {
        arch::cpu_pause();
    }

    const u64 start_tick = g_tick_count;
    lapic_write(LAPIC_INIT_COUNT, 0xFFFF'FFFFu);

    while (g_tick_count < start_tick + LAPIC_TIMER_SAMPLE_TICKS) {
        arch::cpu_pause();
    }

    const u64 elapsed_ticks = g_tick_count - start_tick;
    const u32 current_count = lapic_read(LAPIC_CUR_COUNT);
    u64 counts_per_tick = (0xFFFF'FFFFULL - current_count) /
        (elapsed_ticks == 0 ? 1 : elapsed_ticks);

    if (counts_per_tick < LAPIC_TIMER_MIN_INIT_COUNT) {
        counts_per_tick = LAPIC_TIMER_MIN_INIT_COUNT;
    }
    if (counts_per_tick > 0xFFFF'FFFFULL) {
        counts_per_tick = 0xFFFF'FFFFULL;
    }

    return static_cast<u32>(counts_per_tick);
}

static void lapic_timer_start_periodic(u32 init_count) {
    lapic_write(LAPIC_DIVIDE_CONFIG, LAPIC_TIMER_DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_INIT_COUNT, init_count);
}

/* ============================================================
 * Scheduler internals
 * ============================================================ */

/* Current-task index for this CPU */
static inline usize cpu_current_task() {
    u8 apic_id = smp::current_cpu_apic_id();
    return g_per_cpu_task[apic_id];
}

static inline void cpu_set_current_task(usize idx) {
    u8 apic_id = smp::current_cpu_apic_id();
    g_per_cpu_task[apic_id] = idx;
}

static inline auto cpu_allows_shared_idle() -> bool {
    return smp::current_cpu_apic_id() == g_bsp_apic_id;
}

/* Keep a task on the CPU that most recently owned its stack.  During an SMP
 * switch the outgoing CPU marks the task ready before the interrupt epilogue
 * has finished unwinding from that stack, so another CPU must not pick it yet. */
static inline auto task_matches_cpu_affinity(usize task_index, u8 this_apic) -> bool {
    const u32 affinity = g_tasks[task_index].affinity_cpu;
    return affinity == SCHED_CPU_NONE || affinity == this_apic;
}

[[maybe_unused]] static void task_trampoline(void* user_data) {
    auto& t = g_tasks[cpu_current_task()];
    arch::enable_interrupts();
    if (t.entry) t.entry(t.user_data);
    sched::exit_task();
}

static void init_task_stack_frame(task& t, task_entry_fn entry, void* user_data) {
    t.entry = entry;
    t.user_data = user_data;

    /*
     * Set up the initial stack frame so the first context switch
     * "returns" into task_trampoline.
     *
     * Keep a full interrupt-frame-shaped tail here.  Same-CPL iretq only
     * consumes RIP, CS and RFLAGS, leaving the saved RSP/SS as harmless stack
     * scratch space; the full shape also matches the rest of the saved-state
     * model used by diagnostics and context switching.
     *
     * Because iretq is not a call, we must also arrange the post-iret stack
     * exactly like a normal SysV function entry: %rsp % 16 == 8.  The padding
     * slot above SS makes the same-CPL iretq leave RSP at stack_top - 24.
     *
     * Stack layout:
     *          alignment padding
     *          SS, RSP, RFLAGS, CS, RIP
     *          error_code, int_no
     *          FS, GS
     *          R15..R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX
     *
     * Total saved frame: 1 + 5 + 2 + 2 + 15 = 25 u64s.
     */
    auto* stack_top = reinterpret_cast<u64*>(
        reinterpret_cast<usize>(&t.stack[TASK_STACK_SIZE]) & ~0xFull);
    auto* iret_frame = stack_top - 6;

    /* Full interrupt-frame-shaped tail */
    iret_frame[0] = reinterpret_cast<u64>(&task_trampoline); /* RIP */
    iret_frame[1] = arch::SEG_KERNEL_CODE; /* CS */
    iret_frame[2] = 0x2;  /* RFLAGS: reserved bit set, IF enabled in trampoline */
    iret_frame[3] = reinterpret_cast<u64>(stack_top); /* RSP if privilege changes */
    iret_frame[4] = arch::SEG_KERNEL_DATA; /* SS if privilege changes */
    iret_frame[5] = 0; /* padding: makes post-iret %rsp 16n+8 */

    /* error_code + int_no */
    iret_frame[-1] = 0; /* error_code */
    iret_frame[-2] = 0; /* int_no */

    /* FS, GS — FS at lower address (pushed last by ISR), GS at higher */
    iret_frame[-3] = 0; /* GS (higher addr) */
    iret_frame[-4] = 0; /* FS (lower addr) */

    /* 15 GPRs: R15..R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX */
    for (int i = 5; i <= 19; ++i)
        iret_frame[-i] = 0;

    t.rsp = reinterpret_cast<u64>(iret_frame - 19);
}

[[noreturn]] static void cpu_idle_trampoline() {
    arch::enable_interrupts();
    while (true) {
        arch::cpu_halt();
    }
}

static void init_cpu_idle_frame(u8 apic_id) {
    auto* stack_top = reinterpret_cast<u64*>(
        reinterpret_cast<usize>(&g_cpu_idle_stacks[apic_id][CPU_IDLE_STACK_SIZE]) & ~0xFull);
    auto* iret_frame = stack_top - 6;

    iret_frame[0] = reinterpret_cast<u64>(&cpu_idle_trampoline); /* RIP */
    iret_frame[1] = arch::SEG_KERNEL_CODE; /* CS */
    iret_frame[2] = 0x2; /* RFLAGS: IF enabled in trampoline */
    iret_frame[3] = reinterpret_cast<u64>(stack_top); /* RSP if privilege changes */
    iret_frame[4] = arch::SEG_KERNEL_DATA; /* SS if privilege changes */
    iret_frame[5] = 0; /* padding: same rationale as task creation */

    for (int i = 1; i <= 19; ++i)
        iret_frame[-i] = 0;

    g_cpu_idle_rsp[apic_id] = reinterpret_cast<u64>(iret_frame - 19);
}

static void wake_sleeping_tasks() {
    for (usize i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].state == task_state::blocked && g_tasks[i].wake_tick <= g_tick_count) {
            g_tasks[i].state = task_state::ready;
        }
    }
}

static inline auto task_can_run_on_this_cpu(usize task_index) -> bool {
    if (task_index >= g_task_count) {
        return false;
    }

    const u8 this_apic = smp::current_cpu_apic_id();

    if (!task_matches_cpu_affinity(task_index, this_apic)) {
        return false;
    }

    if (task_index == 0) {
        return cpu_allows_shared_idle();
    }

    if (!cpu_allows_shared_idle() && !g_tasks[task_index].allow_secondary_cpu) {
        return false;
    }

    return true;
}

static auto pick_next_task(usize cur) -> usize {
    if (g_task_count == 0) {
        return SCHED_NO_TASK;
    }

    /* Task 0 is the idle task. Keep it out of normal round-robin so it
     * only consumes CPU when every real task is blocked or terminated. */
    if (g_task_count > 1) {
        usize start = (cur > 0 && cur < g_task_count) ? cur : 0;
        for (usize i = 1; i < g_task_count; ++i) {
            usize next = ((start + i - 1) % (g_task_count - 1)) + 1;
            if (g_tasks[next].state == task_state::ready &&
                task_can_run_on_this_cpu(next)) {
                return next;
            }
        }
    }

    if (cur > 0 && cur < g_task_count &&
        g_tasks[cur].state == task_state::ready &&
        task_can_run_on_this_cpu(cur)) {
        return cur;
    }

    if (g_tasks[0].state == task_state::ready && task_can_run_on_this_cpu(0)) {
        return 0;
    }

    return SCHED_NO_TASK;
}

/* ============================================================
 * Entry point validation helper (freestanding-safe)
 * ============================================================ */

[[maybe_unused]] static bool validate_entry_point(u64 entry_va) {
    const u8* bytes = reinterpret_cast<const u8*>(entry_va);
    
    /* Check for obvious fill patterns (0x00, 0xFF, 0xCC) */
    bool has_real_code = false;
    for (int i = 0; i < 16; ++i) {
        u8 b = bytes[i];
        if (b != 0x00 && b != 0xFF && b != 0xCC && b != 0x90) {
            has_real_code = true;
            break;
        }
    }
    return has_real_code;
}

[[maybe_unused]] static void dump_entry_bytes(u64 entry_va, usize count = 32) {
    const u8* bytes = reinterpret_cast<const u8*>(entry_va);

    constexpr usize max_dump_bytes = 32;
    char bytes_buf[max_dump_bytes * 3 + 1];
    usize dump_count = count <= max_dump_bytes ? count : max_dump_bytes;
    log::hex_bytes(bytes_buf, sizeof(bytes_buf), bytes, dump_count);
    log::debug() << "Dumping " << dump_count << " bytes at entry point " << log::hex(static_cast<u64>(static_cast<unsigned long long>(entry_va)), 1, true, false) << ": " << bytes_buf;
}

static void activate_task_address_space(const task& t) {
    auto* ctx = static_cast<process::process_task_context*>(t.user_data);
    const phys_addr before = vm::active_cr3();
    const phys_addr target = (ctx != null && ctx->address_space != null)
        ? ctx->address_space->pml4_phys
        : vm::kernel_cr3();
    static usize s_vm_switch_trace_budget = 64;
    if (s_vm_switch_trace_budget > 0 && before != target) {
        --s_vm_switch_trace_budget;
        log::debug() << "vmtrace: switch task_id=" << static_cast<unsigned long long>(t.id)
                     << " name=" << t.name.c_str()
                     << " from_cr3=" << reinterpret_cast<const void*>(static_cast<usize>(before))
                     << " to_cr3=" << reinterpret_cast<const void*>(static_cast<usize>(target))
                     << " as=" << (ctx != null ? reinterpret_cast<const void*>(ctx->address_space) : null);
    }
    if (ctx != null && ctx->address_space != null) {
        vm::activate(ctx->address_space);
    } else {
        vm::activate_kernel();
    }
}

/* ============================================================
 * Scheduler API
 * ============================================================ */

auto sched::init() -> status_code {
    memory::set(g_tasks, 0, sizeof(g_tasks));
    /* Initialise every CPU's "current task" slot to SCHED_NO_TASK.
     * Zero-filling would leave them pointing at task 0, which is wrong:
     * an AP that fires its LAPIC timer before calling start_ap() would
     * corrupt g_tasks[0].rsp if the slot were 0. */
    for (usize i = 0; i < MAX_APIC_IDS; ++i)
        g_per_cpu_task[i] = SCHED_NO_TASK;
    for (usize i = 0; i < MAX_APIC_IDS; ++i)
        g_yield_in_progress[i] = false;
    for (usize i = 0; i < MAX_APIC_IDS; ++i)
        init_cpu_idle_frame(static_cast<u8>(i));
    g_task_count       = 0;
    g_next_task_id     = 1;
    g_scheduler_active = false;
    g_tick_count       = 0;
    g_sched_lock.locked = 0;
    g_bsp_apic_id      = smp::current_cpu_apic_id();

    /* Remap PIC so IRQ0 -> vector 32 */
    pic_remap();

    /* Init PIT for periodic ticks */
    pit_init();

    log::info() << "Scheduler initialized (PIT @ 100 Hz)";
    return status_code::success;
}

auto sched::create_task(string_view name, task_entry_fn entry, void* user_data) -> i64 {
    g_sched_lock.acquire();
    const usize slot = find_available_task_slot();
    if (slot >= MAX_TASKS) {
        g_sched_lock.release();
        return -1;
    }

    auto& t = g_tasks[slot];
    t.name.clear();
    if (!t.name.assign(name)) {
        g_sched_lock.release();
        return -1;
    }

    t.id    = g_next_task_id++;
    t.state = task_state::ready;
    t.wake_tick = 0;
    t.xsave_valid = false;
    t.allow_secondary_cpu = g_scheduler_active;
    t.affinity_cpu = SCHED_CPU_NONE;
    t.current_cpu = SCHED_CPU_NONE;
    t.cpu_ticks = 0;
    if (user_data != null) {
        static_cast<process::process_task_context*>(user_data)->task_id = t.id;
    }
    if (slot == g_task_count) {
        ++g_task_count;
    }

    init_task_stack_frame(t, entry, user_data);

    if constexpr (log::debug_enabled()) {
        log::debug() << "task '" << t.name.c_str() << "': entry=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(reinterpret_cast<u64>(entry))), 1, true, false) << " user_data=" << reinterpret_cast<const void*>(user_data) << " rsp=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(t.rsp)), 1, true, false);

        if (!validate_entry_point(reinterpret_cast<u64>(entry))) {
            log::warn() << "Task '" << t.name.c_str() << "' entry point may contain fill pattern";
            dump_entry_bytes(reinterpret_cast<u64>(entry), 32);
        } else {
            log::debug() << "Task '" << t.name.c_str() << "' entry point looks like valid code";
            dump_entry_bytes(reinterpret_cast<u64>(entry), 16);
        }

        auto* frame = reinterpret_cast<u64*>(t.rsp);
        log::debug() << "Initial frame @" << log::hex(static_cast<u64>(static_cast<unsigned long long>(t.rsp)), 1, true, false) << ": RIP=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(frame[19])), 1, true, false) << " CS=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(frame[20])), 1, true, false) << " RFLAGS=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(frame[21])), 1, true, false) << " entry_rsp=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(frame[22])), 1, true, false) << " RCX=" << log::hex(static_cast<u64>(static_cast<unsigned long long>(frame[16])), 1, true, false);
    }

    i64 id = static_cast<i64>(t.id);
    g_sched_lock.release();

    log::info() << "Task created: " << t.name.c_str() << " (id=" << static_cast<unsigned long long>(id) << ")";

    return id;
}

auto sched::create_task(const char* name, task_entry_fn entry, void* user_data) -> i64 {
    return create_task(string_view(name), entry, user_data);
}

void sched::yield() {
    if (!g_scheduler_active || g_task_count < 2) return;
    arch::disable_interrupts();
    g_yield_in_progress[smp::current_cpu_apic_id()] = true;
    arch::enable_interrupts();
    asm_int_timer();
}

auto sched::preempt(arch::register_state* regs) -> arch::register_state* {
    /* Only count real PIT hardware ticks, not software yields.
     * Note: on APs the LAPIC timer fires on vector 32 as well;  the
     * PIT only fires on the BSP (IRQ0 is wired to the BSP by the PIC). */
    u8 this_apic = smp::current_cpu_apic_id();
    const bool timer_tick = !g_yield_in_progress[this_apic];
    if (g_yield_in_progress[this_apic]) {
        g_yield_in_progress[this_apic] = false;
    } else if (this_apic == g_bsp_apic_id) {
        /* Only the BSP's PIT (IRQ0) advances the global clock.
         * APs fire vector 32 from their LAPIC timers for scheduling only;
         * if they also incremented g_tick_count, ticks would run N× fast. */
        (void)arch::atomic_add(&g_tick_count, 1);
    }

    /* Send EOI.
     * - The BSP receives IRQ0 via the 8259 PIC (pic_eoi() writes port 0x20).
     * - APs receive vector 32 from their local LAPIC timer.
     *   The LAPIC requires a write to its EOI register (offset 0xB0) to
     *   acknowledge the interrupt; the 8259 EOI is a no-op for LAPIC sources.
     * Writing both is safe: the PIC ignores spurious EOIs and the LAPIC
     * ignores the PIC port write.
     */
    pic_eoi();
    lapic_write(LAPIC_EOI, 0);

    if (!g_scheduler_active || g_task_count < 2) {
        return regs;
    }

    g_sched_lock.acquire();

    wake_sleeping_tasks();

    usize cur = cpu_current_task();
    if (timer_tick && cur < g_task_count && g_tasks[cur].state == task_state::running) {
        ++g_tasks[cur].cpu_ticks;
    }

    /* Save current task only if this CPU is actually running one.
     * cur == SCHED_NO_TASK on an AP that has never been assigned a task yet
     * (set in start_ap() before the first LAPIC-timer preemption).  Saving
     * into g_tasks[SCHED_NO_TASK] would be an out-of-bounds write and would
     * also corrupt another task's saved RSP if two CPUs happen to both have
     * SCHED_NO_TASK simultaneously. */
    process::process_task_context* terminated_ctx = null;

    if (cur < g_task_count) {
        g_tasks[cur].rsp = reinterpret_cast<u64>(regs);
        /* Save x87/SSE state.  Without this, XMM register contents
         * leak across context switches and corrupt user processes
         * that use SSE (e.g. doom, anything compiled with -msse2).
         *
         * FXSAVE/FXRSTOR is enough for the current userspace ABI and avoids
         * XRSTOR #GPs if the extended-state header is stale or corrupted. */
        arch::fxsave(g_tasks[cur].xsave_area);
        sanitize_fxsave_area(g_tasks[cur].xsave_area);
        g_tasks[cur].xsave_valid = true;
        if (g_tasks[cur].state == task_state::terminated &&
            g_tasks[cur].user_data != null) {
            terminated_ctx = static_cast<process::process_task_context*>(g_tasks[cur].user_data);
            g_tasks[cur].user_data = null;
        } else if (g_tasks[cur].state == task_state::running) {
            g_tasks[cur].state = task_state::ready;
        }
        g_tasks[cur].current_cpu = SCHED_CPU_NONE;
    }

    usize next = pick_next_task(cur);
    if (next >= g_task_count) {
        const bool cur_non_runnable = cur < g_task_count && !g_tasks[cur].is_runnable();
        if (cur_non_runnable) {
            cpu_set_current_task(SCHED_NO_TASK);
            init_cpu_idle_frame(this_apic);
            g_sched_lock.release();
            vm::activate_kernel();
            if (terminated_ctx != null) {
                process::cleanup_process_context(terminated_ctx, -1);
            }
            return reinterpret_cast<arch::register_state*>(g_cpu_idle_rsp[this_apic]);
        }

        g_sched_lock.release();
        return regs;
    }

    cpu_set_current_task(next);
    g_tasks[next].state = task_state::running;
    if (g_tasks[next].affinity_cpu == SCHED_CPU_NONE) {
        g_tasks[next].affinity_cpu = this_apic;
    }
    g_tasks[next].current_cpu = this_apic;
    const u64 next_rsp = g_tasks[next].rsp;

    /* Restore the next task's x87/SSE state.  On the very first
     * dispatch the task has never run yet, so xsave_valid is false
     * and we leave the FPU in its boot-time state (which the task
     * trampoline / entry point will initialise as needed). */
    if (g_tasks[next].xsave_valid) {
        sanitize_fxsave_area(g_tasks[next].xsave_area);
        arch::fxrstor(g_tasks[next].xsave_area);
    }
    g_sched_lock.release();

    if (terminated_ctx != null) {
        process::cleanup_process_context(terminated_ctx, -1);
    }

    activate_task_address_space(g_tasks[next]);
    return reinterpret_cast<arch::register_state*>(next_rsp);
}

/*
 * sched_switch_to — load a full register_state frame and iretq.
 * On MSVC this is implemented in msvc_asm.asm as asm_sched_switch_to.
 * On GCC/Clang we use a naked function with inline asm.
 */
[[noreturn]] static inline void sched_switch_to(u64 rsp) {
    asm_sched_switch_to(rsp);
}

[[noreturn]] void sched::start() {
    if (g_task_count == 0) {
        vk_panic(__FILE__, __LINE__, "No tasks to schedule");
    }

    /* Pick the first task to run and enable the scheduler atomically
     * under the spinlock.  Prefer a real task over idle so boot does
     * not depend on an immediate timer interrupt to escape task 0. */
    g_sched_lock.acquire();
    usize first = pick_next_task(SCHED_NO_TASK);
    if (first >= g_task_count) {
        first = 0;
    }
    cpu_set_current_task(first);
    g_tasks[first].state = task_state::running;
    if (g_tasks[first].affinity_cpu == SCHED_CPU_NONE) {
        g_tasks[first].affinity_cpu = g_bsp_apic_id;
    }
    g_tasks[first].current_cpu = g_bsp_apic_id;
    g_scheduler_active = true;
    activate_task_address_space(g_tasks[first]);
    g_sched_lock.release();

    /* Unmask IRQ0 and enable interrupts (BSP only) */
    pic_unmask_irq0();

    log::info() << "Scheduler starting with task " << static_cast<unsigned long long>(g_tasks[first].id) << " (" << g_tasks[first].name.c_str() << ")...";

    /* Jump into the first task — naked asm, no compiler interference */
    sched_switch_to(g_tasks[first].rsp);
}

/* ============================================================
 * AP entry point — called from ap_init_secondary() after
 * arch::ap_activate() and smp::lapic_init_local().
 *
 * Each AP:
 *   1. Waits for the BSP scheduler to go live.
 *   2. Calibrates its local LAPIC timer against the BSP PIT tick.
 *   3. Enables interrupts and idles with no current task.  Vector 32
 *      timer interrupts will dispatch runnable non-idle work onto this CPU.
 * ============================================================ */
[[noreturn]] void sched::start_ap() {
    g_sched_lock.acquire();
    cpu_set_current_task(SCHED_NO_TASK);
    g_sched_lock.release();

    const u32 timer_init = lapic_timer_calibrate();
    lapic_timer_start_periodic(timer_init);

    log::debug() << "AP APIC " << smp::current_cpu_apic_id()
                 << ": LAPIC timer armed (init=" << timer_init
                 << ", target " << SCHED_HZ << " Hz), waiting for runnable work";

    arch::enable_interrupts();

    while (true) {
        arch::cpu_halt();
    }
}

auto sched::current_task_id() -> u64 {
    usize t = cpu_current_task();
    if (t >= g_task_count) return 0;
    return g_tasks[t].id;
}

auto sched::current_task_name_view() -> string_view {
    usize t = cpu_current_task();
    if (t >= g_task_count) return "<idle>";
    return g_tasks[t].name.view();
}

auto sched::current_task_name() -> const char* {
    usize t = cpu_current_task();
    if (t >= g_task_count) return "<idle>";
    return g_tasks[t].name.c_str();
}

auto sched::current_task_user_data() -> void* {
    usize t = cpu_current_task();
    if (t >= g_task_count) return null;
    return g_tasks[t].user_data;
}

auto sched::task_user_data(u64 task_id) -> void* {
    g_sched_lock.acquire();
    void* user_data = null;
    for (usize i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].id == task_id) {
            user_data = g_tasks[i].user_data;
            break;
        }
    }
    g_sched_lock.release();
    return user_data;
}

auto sched::tick_count() -> u64 {
    return g_tick_count;
}

auto sched::snapshot_tasks(task_snapshot* out, usize max_tasks) -> usize {
    g_sched_lock.acquire();
    usize total = 0;
    for (usize i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].state != task_state::terminated) {
            ++total;
        }
    }

    if (out == null || max_tasks == 0) {
        g_sched_lock.release();
        return total;
    }

    usize count = 0;
    for (usize i = 0; i < g_task_count && count < max_tasks; ++i) {
        if (g_tasks[i].state == task_state::terminated) {
            continue;
        }

        out[count].id = g_tasks[i].id;
        out[count].state = g_tasks[i].state;
        out[count].cpu = g_tasks[i].current_cpu;
        out[count].cpu_ticks = g_tasks[i].cpu_ticks;
        out[count].name = g_tasks[i].name.view();
        ++count;
    }
    g_sched_lock.release();
    return total;
}

void sched::sleep(u64 ticks) {
    if (ticks == 0) {
        yield();
        return;
    }

    /* Set wake_tick and state under the spinlock so that another CPU's
     * preempt() → wake_sleeping_tasks() cannot observe state==blocked
     * with a stale wake_tick (which would wake us immediately). */
    g_sched_lock.acquire();
    usize cur = cpu_current_task();
    g_tasks[cur].wake_tick = g_tick_count + ticks;
    g_tasks[cur].state = task_state::blocked;
    g_sched_lock.release();

    /* Yield repeatedly until woken.  In practice the first yield()
     * will not return until wake_sleeping_tasks() marks us ready
     * and preempt() dispatches us back. */
    while (g_tasks[cpu_current_task()].state == task_state::blocked) {
        yield();
    }
}

void sched::wait_for_task(u64 task_id) {
    while (true) {
        bool found = false;
        for (usize i = 0; i < g_task_count; ++i) {
            if (g_tasks[i].id == task_id) {
                found = true;
                if (g_tasks[i].state == task_state::terminated)
                    return;
                break;
            }
        }
        if (!found) return; /* task never existed */
        sleep(1);
    }
}

auto sched::terminate_task(u64 task_id) -> bool {
    process::process_task_context* terminated_ctx = null;

    g_sched_lock.acquire();

    for (usize i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].id != task_id) {
            continue;
        }

        if (i == 0) {
            g_sched_lock.release();
            return false;
        }

        g_tasks[i].state = task_state::terminated;
        g_tasks[i].wake_tick = 0;
        g_tasks[i].affinity_cpu = SCHED_CPU_NONE;
        if (g_tasks[i].current_cpu == SCHED_CPU_NONE && g_tasks[i].user_data != null) {
            terminated_ctx = static_cast<process::process_task_context*>(g_tasks[i].user_data);
            g_tasks[i].user_data = null;
        }
        log::debug() << "Task termination requested: " << g_tasks[i].name.c_str();
        g_sched_lock.release();

        if (terminated_ctx != null) {
            process::cleanup_process_context(terminated_ctx, -1);
        }

        return true;
    }

    g_sched_lock.release();
    return false;
}

[[noreturn]] void sched::exit_task() {
    g_sched_lock.acquire();
    usize cur = cpu_current_task();
    g_tasks[cur].state = task_state::terminated;
    g_tasks[cur].affinity_cpu = SCHED_CPU_NONE;
    g_tasks[cur].current_cpu = SCHED_CPU_NONE;
    g_tasks[cur].user_data = null;
    log::debug() << "Task terminated: " << g_tasks[cur].name.c_str();
    g_sched_lock.release();
    /* Yield to let scheduler pick another task */
    while (true) { sched::yield(); arch::cpu_halt(); }
    VK_UNREACHABLE();
}

auto sched::detach_current_task() -> void* {
    g_sched_lock.acquire();
    void* prev = null;
    usize cur = cpu_current_task();
    if (cur < g_task_count) {
        prev = g_tasks[cur].user_data;
        g_tasks[cur].user_data = null;
        g_tasks[cur].state     = task_state::terminated;
        g_tasks[cur].affinity_cpu = SCHED_CPU_NONE;
        g_tasks[cur].current_cpu = SCHED_CPU_NONE;
    }
    g_sched_lock.release();
    return prev;
}

void sched::dump_tasks() {
    log::info() << "Task list:";
    for (usize i = 0; i < g_task_count; ++i) {
        const char* state =
            (g_tasks[i].state == task_state::ready) ? "ready" :
            (g_tasks[i].state == task_state::running) ? "running" :
            (g_tasks[i].state == task_state::blocked) ? "blocked" :
            (g_tasks[i].state == task_state::terminated) ? "terminated" : "unknown";
        if (g_tasks[i].current_cpu == SCHED_CPU_NONE) {
            log::info() << "  [" << static_cast<unsigned long long>(g_tasks[i].id)
                        << "] " << g_tasks[i].name.c_str() << " - " << state
                        << " cpu=-";
        } else {
            log::info() << "  [" << static_cast<unsigned long long>(g_tasks[i].id)
                        << "] " << g_tasks[i].name.c_str() << " - " << state
                        << " cpu=" << static_cast<unsigned long long>(g_tasks[i].current_cpu);
        }
    }
}

} // namespace vk
