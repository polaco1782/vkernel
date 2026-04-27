#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"

#include "../doom-src/opl/opl.h"
#include "../doom-src/opl/opl3.h"
#include "../doom-src/opl/opl_internal.h"
#include "../doom-src/opl/opl_queue.h"

typedef struct
{
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t expire_time;
} opl_timer_t;

static opl_callback_queue_t *callback_queue;
static uint64_t current_time;
static uint64_t pause_offset;
static int opl_vk_paused;

static opl3_chip opl_chip;
static int opl_opl3mode;
static int register_num;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;

    if (!timer->enabled)
    {
        return;
    }

    tics = 0x100 - timer->value;
    timer->expire_time = current_time + ((uint64_t) tics * OPL_SECOND) / timer->rate;
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }

                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }
            break;

        case OPL_REG_NEW:
            opl_opl3mode = value & 0x01;
            OPL3_WriteRegBuffered(&opl_chip, reg_num, value);
            break;

        default:
            OPL3_WriteRegBuffered(&opl_chip, reg_num, value);
            break;
    }
}

static void AdvanceTime(unsigned int nsamples)
{
    opl_callback_t callback;
    void *callback_data;
    uint64_t us;

    us = ((uint64_t) nsamples * OPL_SECOND) / opl_sample_rate;
    current_time += us;

    if (opl_vk_paused)
    {
        pause_offset += us;
    }

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
        {
            break;
        }

        callback(callback_data);
    }
}

static int OPL_VK_Init(unsigned int port_base)
{
    (void) port_base;

    callback_queue = OPL_Queue_Create();
    if (callback_queue == NULL)
    {
        return 0;
    }

    current_time = 0;
    pause_offset = 0;
    opl_vk_paused = 0;
    opl_opl3mode = 0;
    register_num = 0;
    timer1.enabled = 0;
    timer2.enabled = 0;
    timer1.value = 0;
    timer2.value = 0;
    timer1.expire_time = 0;
    timer2.expire_time = 0;

    OPL3_Reset(&opl_chip, opl_sample_rate);
    return 1;
}

static void OPL_VK_Shutdown(void)
{
    if (callback_queue != NULL)
    {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }
}

static unsigned int OPL_VK_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
    {
        return 0x00;
    }

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;
        result |= 0x40;
    }

    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;
        result |= 0x20;
    }

    return result;
}

static void OPL_VK_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_REGISTER_PORT_OPL3)
    {
        register_num = value | 0x100;
    }
    else if (port == OPL_DATA_PORT)
    {
        WriteRegister(register_num, value);
    }
}

static void OPL_VK_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    OPL_Queue_Push(callback_queue, callback, data, current_time - pause_offset + us);
}

static void OPL_VK_ClearCallbacks(void)
{
    OPL_Queue_Clear(callback_queue);
}

static void OPL_VK_Lock(void)
{
}

static void OPL_VK_Unlock(void)
{
}

static void OPL_VK_SetPaused(int paused)
{
    opl_vk_paused = paused;
}

static void OPL_VK_AdjustCallbacks(float factor)
{
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
}

opl_driver_t opl_vk_driver = {
    "VK",
    OPL_VK_Init,
    OPL_VK_Shutdown,
    OPL_VK_PortRead,
    OPL_VK_PortWrite,
    OPL_VK_SetCallback,
    OPL_VK_ClearCallbacks,
    OPL_VK_Lock,
    OPL_VK_Unlock,
    OPL_VK_SetPaused,
    OPL_VK_AdjustCallbacks,
};

void OPL_VK_Render(int16_t *buffer, unsigned int nsamples)
{
    unsigned int filled = 0;

    if (buffer == NULL || nsamples == 0)
    {
        return;
    }

    while (filled < nsamples)
    {
        uint64_t next_callback_time;
        uint64_t chunk;

        if (opl_vk_paused || callback_queue == NULL || OPL_Queue_IsEmpty(callback_queue))
        {
            chunk = nsamples - filled;
        }
        else
        {
            next_callback_time = OPL_Queue_Peek(callback_queue) + pause_offset;

            if (next_callback_time <= current_time)
            {
                chunk = 1;
            }
            else
            {
                chunk = (next_callback_time - current_time) * opl_sample_rate;
                chunk = (chunk + OPL_SECOND - 1) / OPL_SECOND;

                if (chunk == 0)
                {
                    chunk = 1;
                }
                if (chunk > nsamples - filled)
                {
                    chunk = nsamples - filled;
                }
            }
        }

        OPL3_GenerateStream(&opl_chip, (Bit16s *) (buffer + filled * 2u), chunk);
        filled += (unsigned int) chunk;
        AdvanceTime((unsigned int) chunk);
    }
}
