/*
 * i_video_vk.c - Chocolate Doom video for vkernel framebuffer
 *
 * Replaces i_video.c.  Renders Doom's 320x200 8-bit paletted screen
 * directly to the vkernel linear framebuffer, scaling with nearest-
 * neighbour to fill the display.
 */

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_diskicon.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "d_loop.h"
#include "deh_str.h"

#include "../../include/vkernel/vk.h"

/* ---- framebuffer state ---- */
static vk_framebuffer_info_t vk_fb;
static uint32_t *fb_pixels;   /* pointer to raw framebuffer memory */

/* Doom's palette, converted to 32-bit XRGB */
static uint32_t palette_xrgb[256];

/* Our 320x200 screen buffer that Doom draws into (I_VideoBuffer) */
pixel_t *I_VideoBuffer = NULL;

/* Variables referenced by config system */
int vanilla_keyboard_mapping = 1;
int usemouse = 1;
int fullscreen = 0;
int aspect_ratio_correct = 1;
int integer_scaling = 0;
int vga_porch_flash = 0;
int window_width = 320;
int window_height = 200;

/* Input variables (from i_input.c) - we skip vanilla_keyboard_mapping and novert bindings */
float mouse_acceleration = 2.0f;
int mouse_threshold = 10;
int fullscreen_width = 0;
int fullscreen_height = 0;
int force_software_renderer = 0;
int max_scaling_buffer_pixels = 16000000;
int startup_delay = 0;
extern int show_endoom;
extern int show_diskicon;
int png_screenshots = 0;
char *video_driver = "";
char *window_position = "";
int usegamma = 0;

int graphical_startup = 0;
int grabmouse = 1;

static boolean initialized = false;
static grabmouse_callback_t grabmouse_callback = NULL;

static uint32_t pack_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    switch (vk_fb.format) {
    /* RGBX: bytes in memory are [R,G,B,X]  → uint32 (LE) = (B<<16)|(G<<8)|R */
    case VK_PIXEL_FORMAT_RGBX_8BPP:
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    /* BGRX: bytes in memory are [B,G,R,X]  → uint32 (LE) = (R<<16)|(G<<8)|B */
    case VK_PIXEL_FORMAT_BGRX_8BPP:
    default:
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

void I_SetPalette(byte *pal)
{
    for (int i = 0; i < 256; ++i) {
        uint8_t r = gammatable[usegamma][pal[i * 3 + 0]];
        uint8_t g = gammatable[usegamma][pal[i * 3 + 1]];
        uint8_t b = gammatable[usegamma][pal[i * 3 + 2]];
        palette_xrgb[i] = pack_pixel(r, g, b);
    }
}

int I_GetPaletteIndex(int r, int g, int b)
{
    /* Simple nearest-colour search (used rarely) */
    int best = 0, best_dist = 0x7fffffff;
    byte *pal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    for (int i = 0; i < 256; ++i) {
        int dr = r - pal[i * 3 + 0];
        int dg = g - pal[i * 3 + 1];
        int db = b - pal[i * 3 + 2];
        int dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

void I_InitGraphics(void)
{
    if (initialized) return;

    /* Query the kernel framebuffer */
    VK_CALL(framebuffer_info, &vk_fb);
    if (!vk_fb.valid || !vk_fb.base) {
        I_Error("I_InitGraphics: no framebuffer available");
    }

    fb_pixels = (uint32_t *)(uintptr_t)vk_fb.base;

    /* Allocate Doom's 320x200 screen buffer */
    I_VideoBuffer = (pixel_t *)Z_Malloc(SCREENWIDTH * SCREENHEIGHT,
                                         PU_STATIC, NULL);
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);

    initialized = true;
}

void I_ShutdownGraphics(void)
{
    if (!initialized) return;
    initialized = false;
}

void I_StartFrame(void)
{
    /* Poll keyboard events and feed them to Doom's event queue */
    vk_key_event_t kev;
    while (VK_CALL(poll_key, &kev)) {
        event_t ev;
        ev.type = kev.pressed ? ev_keydown : ev_keyup;

        /* Map PS/2 scancode set 1 to Doom keys.
         * The doomkeys.h SCANCODE_TO_KEYS_ARRAY is indexed by SDL scancodes
         * which happen to match USB HID usage codes.  PS/2 set 1 make codes
         * differ, so we do our own mapping for the most common keys.
         */
        int dk = 0;
        switch (kev.scancode) {
        /* Letters */
        case 0x1E: dk = 'a'; break; case 0x30: dk = 'b'; break;
        case 0x2E: dk = 'c'; break; case 0x20: dk = 'd'; break;
        case 0x12: dk = 'e'; break; case 0x21: dk = 'f'; break;
        case 0x22: dk = 'g'; break; case 0x23: dk = 'h'; break;
        case 0x17: dk = 'i'; break; case 0x24: dk = 'j'; break;
        case 0x25: dk = 'k'; break; case 0x26: dk = 'l'; break;
        case 0x32: dk = 'm'; break; case 0x31: dk = 'n'; break;
        case 0x18: dk = 'o'; break; case 0x19: dk = 'p'; break;
        case 0x10: dk = 'q'; break; case 0x13: dk = 'r'; break;
        case 0x1F: dk = 's'; break; case 0x14: dk = 't'; break;
        case 0x16: dk = 'u'; break; case 0x2F: dk = 'v'; break;
        case 0x11: dk = 'w'; break; case 0x2D: dk = 'x'; break;
        case 0x15: dk = 'y'; break; case 0x2C: dk = 'z'; break;
        /* Digits */
        case 0x02: dk = '1'; break; case 0x03: dk = '2'; break;
        case 0x04: dk = '3'; break; case 0x05: dk = '4'; break;
        case 0x06: dk = '5'; break; case 0x07: dk = '6'; break;
        case 0x08: dk = '7'; break; case 0x09: dk = '8'; break;
        case 0x0A: dk = '9'; break; case 0x0B: dk = '0'; break;
        /* Special keys */
        case 0x01: dk = 27;  break;  /* Escape */
        case 0x1C: dk = 13;  break;  /* Enter */
        case 0x0E: dk = 0x7F; break; /* Backspace */
        case 0x0F: dk = 9;   break;  /* Tab */
        case 0x39: dk = ' '; break;  /* Space */
        case 0x0C: dk = 0x2D; break; /* Minus */
        case 0x0D: dk = 0x3D; break; /* Equals */
        /* Arrow keys — extended (0xE0 prefix), kernel delivers make|0x80 */
        case 0xC8: dk = KEY_UPARROW;    break; /* 0x48|0x80 */
        case 0xD0: dk = KEY_DOWNARROW;  break; /* 0x50|0x80 */
        case 0xCB: dk = KEY_LEFTARROW;  break; /* 0x4B|0x80 */
        case 0xCD: dk = KEY_RIGHTARROW; break; /* 0x4D|0x80 */
        /* Non-extended modifiers */
        case 0x2A: dk = KEY_RSHIFT; break; /* Left Shift  */
        case 0x36: dk = KEY_RSHIFT; break; /* Right Shift */
        case 0x1D: dk = KEY_RCTRL;  break; /* Left Ctrl   */
        case 0x38: dk = KEY_RALT;   break; /* Left Alt    */
        /* Extended modifiers */
        case 0x9D: dk = KEY_RCTRL;  break; /* Right Ctrl  (0x1D|0x80) */
        case 0xB8: dk = KEY_RALT;   break; /* Right Alt   (0x38|0x80) */
        /* Function keys (non-extended) */
        case 0x3B: dk = KEY_F1;  break;
        case 0x3C: dk = KEY_F2;  break;
        case 0x3D: dk = KEY_F3;  break;
        case 0x3E: dk = KEY_F4;  break;
        case 0x3F: dk = KEY_F5;  break;
        case 0x40: dk = KEY_F6;  break;
        case 0x41: dk = KEY_F7;  break;
        case 0x42: dk = KEY_F8;  break;
        case 0x43: dk = KEY_F9;  break;
        case 0x44: dk = KEY_F10; break;
        case 0x57: dk = KEY_F11; break;
        case 0x58: dk = KEY_F12; break;
        /* Extended navigation keys */
        case 0xD2: dk = KEY_INS;  break; /* Insert   (0x52|0x80) */
        case 0xD3: dk = KEY_DEL;  break; /* Delete   (0x53|0x80) */
        case 0xC7: dk = KEY_HOME; break; /* Home     (0x47|0x80) */
        case 0xCF: dk = KEY_END;  break; /* End      (0x4F|0x80) */
        case 0xC9: dk = KEY_PGUP; break; /* Page Up  (0x49|0x80) */
        case 0xD1: dk = KEY_PGDN; break; /* Page Dn  (0x51|0x80) */
        /* Misc non-extended */
        case 0x45: dk = KEY_NUMLOCK; break;
        case 0x46: dk = KEY_SCRLCK; break;
        /* Punctuation */
        case 0x1A: dk = '['; break;
        case 0x1B: dk = ']'; break;
        case 0x27: dk = ';'; break;
        case 0x28: dk = '\''; break;
        case 0x29: dk = '`'; break;
        case 0x2B: dk = '\\'; break;
        case 0x33: dk = ','; break;
        case 0x34: dk = '.'; break;
        case 0x35: dk = '/'; break;
        default:   dk = 0;   break;
        }

        if (dk == 0) continue;

        ev.data1 = dk;
        ev.data2 = (kev.ascii >= 0x20 && kev.ascii < 0x7F) ? kev.ascii : 0;
        ev.data3 = 0;
        D_PostEvent(&ev);
    }
}

void I_StartTic(void)
{
    /* Nothing extra needed — I_StartFrame handles everything */
}

void I_FinishUpdate(void)
{
    if (!initialized) return;

    /* Scale 320x200 → framebuffer with nearest-neighbour */
    uint32_t fb_w = vk_fb.width;
    uint32_t fb_h = vk_fb.height;
    uint32_t stride = vk_fb.stride;

    /* Compute integer scale factors */
    uint32_t scale_x = fb_w / SCREENWIDTH;
    uint32_t scale_y = fb_h / SCREENHEIGHT;
    if (scale_x == 0) scale_x = 1;
    if (scale_y == 0) scale_y = 1;
    /* Use the smaller to maintain aspect */
    uint32_t scale = scale_x < scale_y ? scale_x : scale_y;

    uint32_t dst_w = SCREENWIDTH * scale;
    uint32_t dst_h = SCREENHEIGHT * scale;
    uint32_t off_x = (fb_w - dst_w) / 2;
    uint32_t off_y = (fb_h - dst_h) / 2;

    /* Convert paletted buffer → scaled 32-bit framebuffer */
    for (uint32_t sy = 0; sy < (uint32_t)SCREENHEIGHT; ++sy) {
        const pixel_t *src_row = I_VideoBuffer + sy * SCREENWIDTH;
        for (uint32_t dy = 0; dy < scale; ++dy) {
            uint32_t fy = off_y + sy * scale + dy;
            if (fy >= fb_h) break;
            uint32_t *dst_row = fb_pixels + fy * stride + off_x;
            for (uint32_t sx = 0; sx < (uint32_t)SCREENWIDTH; ++sx) {
                uint32_t pixel = palette_xrgb[src_row[sx]];
                for (uint32_t dx = 0; dx < scale; ++dx) {
                    dst_row[sx * scale + dx] = pixel;
                }
            }
        }
    }
}

void I_ReadScreen(pixel_t *scr)
{
    memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT * sizeof(*scr));
}

void I_UpdateNoBlit(void)
{
    /* nothing */
}

void I_BeginRead(void)
{
}

void I_SetWindowTitle(const char *title)
{
    (void)title;
}

void I_GraphicsCheckCommandLine(void)
{
}

void I_SetGrabMouseCallback(grabmouse_callback_t func)
{
    grabmouse_callback = func;
}

void I_CheckIsScreensaver(void)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
    (void)dots_on;
}

void I_BindVideoVariables(void)
{
    M_BindIntVariable("usegamma",            &usegamma);
    M_BindIntVariable("fullscreen",          &fullscreen);
    M_BindIntVariable("aspect_ratio_correct", &aspect_ratio_correct);
    M_BindIntVariable("integer_scaling",      &integer_scaling);
    M_BindIntVariable("vga_porch_flash",      &vga_porch_flash);
    M_BindIntVariable("startup_delay",        &startup_delay);
    M_BindIntVariable("fullscreen_width",     &fullscreen_width);
    M_BindIntVariable("fullscreen_height",    &fullscreen_height);
    M_BindIntVariable("window_width",         &window_width);
    M_BindIntVariable("window_height",        &window_height);
    M_BindIntVariable("grabmouse",            &grabmouse);
    M_BindStringVariable("video_driver",      &video_driver);
    M_BindStringVariable("window_position",   &window_position);
    M_BindIntVariable("use_mouse",            &usemouse);
    M_BindIntVariable("png_screenshots",      &png_screenshots);
}

void I_InitWindowTitle(void)
{
}

void I_RegisterWindowIcon(const unsigned int *icon, int width, int height)
{
    (void)icon; (void)width; (void)height;
}

void I_InitWindowIcon(void)
{
}

void I_EnableLoadingDisk(int xoffs, int yoffs)
{
    (void)xoffs; (void)yoffs;
}

/* i_input.c replacements — we handle input in I_StartFrame above */
void I_BindInputVariables(void)
{
    M_BindFloatVariable("mouse_acceleration",      &mouse_acceleration);
    M_BindIntVariable("mouse_threshold",           &mouse_threshold);
}

void I_ReadMouse(void) {}
void I_StartTextInput(int x1, int y1, int x2, int y2) {
    (void)x1;(void)y1;(void)x2;(void)y2;
}
void I_StopTextInput(void) {}

void I_HandleKeyboardEvent(SDL_Event *ev) { (void)ev; }
void I_HandleMouseEvent(SDL_Event *ev) { (void)ev; }
