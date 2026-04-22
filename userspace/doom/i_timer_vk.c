/*
 * i_timer_vk.c - Chocolate Doom timer for vkernel
 *
 * Replaces i_timer.c.  Uses vk_tick_count() / vk_ticks_per_sec()
 * and vk_sleep() for timing.
 */

#include <stdint.h>
#include "i_timer.h"
#include "../../include/vkernel/vk.h"

static uint32_t basetime = 0;
static int basetime_set = 0;

/* Return current time in milliseconds since first call */
static uint32_t GetTicks(void)
{
    uint64_t ticks = VK_CALL(tick_count);
    uint32_t tps   = VK_CALL(ticks_per_sec);
    uint32_t ms    = (uint32_t)((ticks * 1000ULL) / tps);

    if (!basetime_set) {
        basetime = ms;
        basetime_set = 1;
    }

    return ms - basetime;
}

/* Implement the SDL_GetTicks declared in our SDL shim */
uint32_t SDL_GetTicks(void)
{
    return GetTicks();
}

int I_GetTime(void)
{
    uint32_t ticks = GetTicks();
    return (int)((ticks * TICRATE) / 1000);
}

int I_GetTimeMS(void)
{
    return (int)GetTicks();
}

void I_Sleep(int ms)
{
    if (ms <= 0) return;
    uint32_t tps = VK_CALL(ticks_per_sec);
    uint64_t sleep_ticks = ((uint64_t)ms * tps) / 1000;
    if (sleep_ticks == 0) sleep_ticks = 1;
    VK_CALL(sleep, sleep_ticks);
}

/* Implement SDL_Delay declared in our SDL shim */
void SDL_Delay(uint32_t ms)
{
    I_Sleep((int)ms);
}

void I_WaitVBL(int count)
{
    I_Sleep((count * 1000) / 70);
}

void I_InitTimer(void)
{
    /* Reset base time */
    basetime_set = 0;
    GetTicks();
}
