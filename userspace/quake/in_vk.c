/*
 * in_vk.c - Input handling for Chocolate Quake on vkernel
 * Replaces in_main.c, in_keyboard.c, in_mouse.c, in_gamepad.c
 *
 * Keyboard events are dispatched in Sys_SendKeyEvents (sys_vk.c).
 * Mouse deltas are accumulated in sys_vk.c; we consume them here.
 */

#include "input.h"
#include "client.h"

/* Mouse delta accumulated by Sys_SendKeyEvents in sys_vk.c */
extern int vk_mouse_acc_dx;
extern int vk_mouse_acc_dy;

/* extern cvars defined in cl_main.c (declared in client.h) */
extern cvar_t sensitivity;
extern cvar_t m_yaw;
extern cvar_t m_pitch;
extern cvar_t m_side;
extern cvar_t m_forward;

/* kbutton_t in_strafe/in_mlook defined in cl_input.c */
extern kbutton_t in_strafe;
extern kbutton_t in_mlook;

/* ---------------------------------------------------------------
 * Init / Shutdown
 * --------------------------------------------------------------- */

void IN_Init(void)
{
    /* Nothing to do — VK doesn't need an input device init */
}

void IN_Shutdown(void)
{
}

/* ---------------------------------------------------------------
 * SDL event stubs (events handled via vk_poll_key/vk_poll_mouse)
 * --------------------------------------------------------------- */

void IN_KeyboardEvent(const SDL_Event *ev)  { (void)ev; }
void IN_MouseEvent(const SDL_Event *ev)     { (void)ev; }
void IN_GamepadEvent(const SDL_Event *ev)   { (void)ev; }

/* ---------------------------------------------------------------
 * Mouse activation stubs
 * --------------------------------------------------------------- */

void IN_ActivateMouse(void)   {}
void IN_DeactivateMouse(void) {}
void IN_ShowMouse(void)       {}
void IN_HideMouse(void)       {}
void IN_ClearStates(void)
{
    vk_mouse_acc_dx = 0;
    vk_mouse_acc_dy = 0;
}

/* ---------------------------------------------------------------
 * Mouse movement applied to view/strafe/forward
 * --------------------------------------------------------------- */

static void IN_AddHorizontalMove(usercmd_t *cmd, float move_x)
{
    if ((in_strafe.state & 1) || (lookstrafe.value != 0.0f && (in_mlook.state & 1)))
        cmd->sidemove += m_side.value * move_x;
    else
        cl.viewangles[YAW] -= m_yaw.value * move_x;
}

static void IN_AddVerticalMove(usercmd_t *cmd, float move_y)
{
    if (in_mlook.state & 1) {
        float p = cl.viewangles[PITCH] + m_pitch.value * move_y;
        if (p < -70.0f) p = -70.0f;
        if (p >  80.0f) p =  80.0f;
        cl.viewangles[PITCH] = p;
    } else if (lookspring.value == 0.0f) {
        cmd->upmove -= m_forward.value * move_y;
    } else {
        cmd->forwardmove -= m_forward.value * move_y;
    }
}

void IN_Move(usercmd_t *cmd)
{
    if (!vk_mouse_acc_dx && !vk_mouse_acc_dy)
        return;

    float mx = (float)vk_mouse_acc_dx * sensitivity.value;
    float my = (float)vk_mouse_acc_dy * sensitivity.value;
    vk_mouse_acc_dx = 0;
    vk_mouse_acc_dy = 0;

    IN_AddHorizontalMove(cmd, mx);
    IN_AddVerticalMove(cmd, my);
}
