/*
 * vgui/imgui_impl_vk.cpp
 * vkernel platform + renderer backend for Dear ImGui.
 *
 * Renderer design:
 *   Double-buffered: all triangles are rasterised into an offscreen
 *   back-buffer first; when the frame is complete the back-buffer is
 *   copied to the UEFI linear framebuffer in one memcpy, eliminating
 *   the visible partial-frame flicker of direct rendering.
 *
 *   For every pixel inside a triangle we:
 *     1. Compute barycentric weights (edge-function rasterizer).
 *     2. Interpolate UV and RGBA color.
 *     3. Sample the alpha-8 font texture (nearest-neighbor).
 *     4. Multiply sampled alpha by vertex alpha -> final alpha.
 *     5. Alpha-blend onto the back-buffer pixel.
 */

#include "imgui_impl_vk.h"

#include <string.h>   /* memset, memcpy */
#include <stdlib.h>   /* malloc, free */
#include <stdint.h>   /* intptr_t */

/* ================================================================
 * Internal backend state
 * ================================================================ */

struct ImGui_ImplVK_Data {
    vk_framebuffer_info_t   fb;
    vk_u64                  last_tick;
    unsigned char*          font_pixels;   /* alpha-8; owned by ImGui */
    int                     font_tex_w;
    int                     font_tex_h;
    /* Double-buffer */
    unsigned int*           backbuf;       /* offscreen render target */
    vk_usize                backbuf_size;  /* bytes allocated         */
    /* Mouse state */
    float                   mouse_x;
    float                   mouse_y;
    vk_u32                  mouse_buttons; /* current button bitmask  */
};

static ImGui_ImplVK_Data* get_bd()
{
    return ImGui::GetCurrentContext()
        ? (ImGui_ImplVK_Data*)ImGui::GetIO().BackendPlatformUserData
        : nullptr;
}

/* ================================================================
 * Custom allocators -- route through newlib malloc/free
 * (newlib's heap is bootstrapped from the kernel allocator via _sbrk)
 * ================================================================ */

static void* vk_imgui_alloc(size_t sz, void*) { return malloc(sz); }
static void  vk_imgui_free (void*  p,  void*) { free(p); }

/* ================================================================
 * Runtime renderer options
 * ================================================================ */

/* When true, the rasterizer performs per-pixel source-over alpha
 * blending against the back-buffer (translucent windows / fades).
 * When false, all pixels are written opaque (fastest path). */
static bool g_blend_enabled = false;

void ImGui_ImplVK_SetTransparencyEnabled(bool enabled) { g_blend_enabled = enabled; }
bool ImGui_ImplVK_GetTransparencyEnabled()             { return g_blend_enabled; }

/* ================================================================
 * Lifecycle
 * ================================================================ */

bool ImGui_ImplVK_Init(const vk_framebuffer_info_t* fb)
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPlatformUserData == nullptr
              && "ImGui_ImplVK_Init called twice");

    ImGui_ImplVK_Data* bd =
        (ImGui_ImplVK_Data*)malloc(sizeof(ImGui_ImplVK_Data));
    if (!bd) return false;
    memset(bd, 0, sizeof(*bd));

    bd->fb        = *fb;
    bd->last_tick = vk_get_api()->vk_tick_count();

    /* Allocate back-buffer (stride * height pixels). */
    bd->backbuf_size = (vk_usize)fb->stride * fb->height * sizeof(unsigned int);
    bd->backbuf = (unsigned int*)malloc(bd->backbuf_size);
    if (!bd->backbuf) {
        free(bd);
        return false;
    }
    memset(bd->backbuf, 0, bd->backbuf_size);

    /* Mouse starts at screen centre. */
    bd->mouse_x       = (float)fb->width  * 0.5f;
    bd->mouse_y       = (float)fb->height * 0.5f;
    bd->mouse_buttons = 0u;

    /* Hook ImGui allocation through newlib so the kernel heap is used. */
    ImGui::SetAllocatorFunctions(vk_imgui_alloc, vk_imgui_free);

    io.BackendPlatformName     = "imgui_impl_vk";
    io.BackendRendererName     = "imgui_impl_vk_sw";
    io.BackendPlatformUserData = bd;

    io.DisplaySize             = ImVec2((float)fb->width, (float)fb->height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    /* Keyboard navigation; mouse cursor support. */
    io.ConfigFlags  |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags  |= ImGuiConfigFlags_NavEnableSetMousePos;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

    /* Build font atlas now so the texture pointer is stable. */
    io.Fonts->Build();
    io.Fonts->GetTexDataAsAlpha8(&bd->font_pixels,
                                  &bd->font_tex_w,
                                  &bd->font_tex_h);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1); /* dummy non-null ID */

    return true;
}

void ImGui_ImplVK_Shutdown()
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformUserData = nullptr;
    io.BackendPlatformName     = nullptr;
    io.BackendRendererName     = nullptr;

    ImGui_ImplVK_Data* bd = get_bd();
    if (bd) {
        if (bd->backbuf) free(bd->backbuf);
        free(bd);
    }
}

/* ================================================================
 * Per-frame: timing + display size
 * ================================================================ */

void ImGui_ImplVK_NewFrame()
{
    ImGui_ImplVK_Data* bd = get_bd();
    IM_ASSERT(bd != nullptr && "Call ImGui_ImplVK_Init first");

    ImGuiIO& io = ImGui::GetIO();

    /* Delta time */
    const vk_api_t* api = vk_get_api();
    vk_u64  now = api->vk_tick_count();
    vk_u32  tps = api->vk_ticks_per_sec();
    if (tps == 0) tps = 1000;

    float dt = (bd->last_tick != 0)
                   ? (float)(now - bd->last_tick) / (float)tps
                   : 1.0f / 60.0f;
    if (dt <= 0.0f) dt = 1.0f / 1000.0f;
    if (dt  > 1.0f) dt = 1.0f;

    io.DeltaTime  = dt;
    bd->last_tick = now;

    /* Display size (static for our single-screen setup). */
    io.DisplaySize = ImVec2((float)bd->fb.width, (float)bd->fb.height);

    /* Push current mouse position so ImGui knows where the cursor is. */
    io.AddMousePosEvent(bd->mouse_x, bd->mouse_y);

    /* Signal continuous focus so keyboard nav is always active. */
    io.AddFocusEvent(true);
}

/* ================================================================
 * Keyboard input
 * ================================================================ */

static ImGuiKey scancode_to_key(vk_u32 sc)
{
    /* PS/2 Scan Code Set 1 make codes -> ImGuiKey */
    switch (sc) {
    /* Top row numbers */
    case 0x02: return ImGuiKey_1;
    case 0x03: return ImGuiKey_2;
    case 0x04: return ImGuiKey_3;
    case 0x05: return ImGuiKey_4;
    case 0x06: return ImGuiKey_5;
    case 0x07: return ImGuiKey_6;
    case 0x08: return ImGuiKey_7;
    case 0x09: return ImGuiKey_8;
    case 0x0A: return ImGuiKey_9;
    case 0x0B: return ImGuiKey_0;
    case 0x0C: return ImGuiKey_Minus;
    case 0x0D: return ImGuiKey_Equal;
    case 0x0E: return ImGuiKey_Backspace;
    case 0x0F: return ImGuiKey_Tab;
    /* QWERTY row */
    case 0x10: return ImGuiKey_Q;
    case 0x11: return ImGuiKey_W;
    case 0x12: return ImGuiKey_E;
    case 0x13: return ImGuiKey_R;
    case 0x14: return ImGuiKey_T;
    case 0x15: return ImGuiKey_Y;
    case 0x16: return ImGuiKey_U;
    case 0x17: return ImGuiKey_I;
    case 0x18: return ImGuiKey_O;
    case 0x19: return ImGuiKey_P;
    case 0x1A: return ImGuiKey_LeftBracket;
    case 0x1B: return ImGuiKey_RightBracket;
    case 0x1C: return ImGuiKey_Enter;
    case 0x1D: return ImGuiKey_LeftCtrl;
    /* ASDF row */
    case 0x1E: return ImGuiKey_A;
    case 0x1F: return ImGuiKey_S;
    case 0x20: return ImGuiKey_D;
    case 0x21: return ImGuiKey_F;
    case 0x22: return ImGuiKey_G;
    case 0x23: return ImGuiKey_H;
    case 0x24: return ImGuiKey_J;
    case 0x25: return ImGuiKey_K;
    case 0x26: return ImGuiKey_L;
    case 0x27: return ImGuiKey_Semicolon;
    case 0x28: return ImGuiKey_Apostrophe;
    /* ZXCV row */
    case 0x2A: return ImGuiKey_LeftShift;
    case 0x2C: return ImGuiKey_Z;
    case 0x2D: return ImGuiKey_X;
    case 0x2E: return ImGuiKey_C;
    case 0x2F: return ImGuiKey_V;
    case 0x30: return ImGuiKey_B;
    case 0x31: return ImGuiKey_N;
    case 0x32: return ImGuiKey_M;
    case 0x33: return ImGuiKey_Comma;
    case 0x34: return ImGuiKey_Period;
    case 0x35: return ImGuiKey_Slash;
    case 0x36: return ImGuiKey_RightShift;
    case 0x38: return ImGuiKey_LeftAlt;
    case 0x39: return ImGuiKey_Space;
    /* Escape */
    case 0x01: return ImGuiKey_Escape;
    /* Function keys */
    case 0x3B: return ImGuiKey_F1;
    case 0x3C: return ImGuiKey_F2;
    case 0x3D: return ImGuiKey_F3;
    case 0x3E: return ImGuiKey_F4;
    case 0x3F: return ImGuiKey_F5;
    case 0x40: return ImGuiKey_F6;
    case 0x41: return ImGuiKey_F7;
    case 0x42: return ImGuiKey_F8;
    case 0x43: return ImGuiKey_F9;
    case 0x44: return ImGuiKey_F10;
    case 0x57: return ImGuiKey_F11;
    case 0x58: return ImGuiKey_F12;
    /* Navigation cluster */
    case 0x47: return ImGuiKey_Home;
    case 0x48: return ImGuiKey_UpArrow;
    case 0x49: return ImGuiKey_PageUp;
    case 0x4B: return ImGuiKey_LeftArrow;
    case 0x4D: return ImGuiKey_RightArrow;
    case 0x4F: return ImGuiKey_End;
    case 0x50: return ImGuiKey_DownArrow;
    case 0x51: return ImGuiKey_PageDown;
    case 0x52: return ImGuiKey_Insert;
    case 0x53: return ImGuiKey_Delete;
    default:   return ImGuiKey_None;
    }
}

void ImGui_ImplVK_ProcessKey(const vk_key_event_t* evt)
{
    ImGuiIO& io = ImGui::GetIO();
    bool down   = (evt->pressed != 0);

    /* Keep modifier state in sync on every key event. */
    io.AddKeyEvent(ImGuiMod_Shift, (evt->modifiers & 1u) != 0u);
    io.AddKeyEvent(ImGuiMod_Ctrl,  (evt->modifiers & 2u) != 0u);
    io.AddKeyEvent(ImGuiMod_Alt,   (evt->modifiers & 4u) != 0u);

    /* Translate scan code and fire the event. */
    ImGuiKey key = scancode_to_key(evt->scancode);
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, down);

    /* Feed printable ASCII into ImGui's text-input queue (key down only). */
    if (down && evt->ascii >= 0x20 && evt->ascii < 0x7F)
        io.AddInputCharacter((unsigned int)(unsigned char)evt->ascii);
}

/* ================================================================
 * Mouse input
 * ================================================================ */

void ImGui_ImplVK_ProcessMouse(const vk_mouse_event_t* evt)
{
    ImGui_ImplVK_Data* bd = get_bd();
    if (!bd || !evt) return;

    /* Accumulate relative movement into absolute position. */
    bd->mouse_x += (float)evt->dx;
    bd->mouse_y += (float)evt->dy;

    /* Clamp to framebuffer bounds. */
    if (bd->mouse_x < 0.0f)                         bd->mouse_x = 0.0f;
    if (bd->mouse_y < 0.0f)                         bd->mouse_y = 0.0f;
    if (bd->mouse_x > (float)bd->fb.width  - 1.0f)  bd->mouse_x = (float)bd->fb.width  - 1.0f;
    if (bd->mouse_y > (float)bd->fb.height - 1.0f)  bd->mouse_y = (float)bd->fb.height - 1.0f;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(bd->mouse_x, bd->mouse_y);

    /* Fire button events for changed buttons. */
    vk_u32 changed = evt->buttons ^ bd->mouse_buttons;
    if (changed) {
        if (changed & 0x01u)
            io.AddMouseButtonEvent(ImGuiMouseButton_Left,   (evt->buttons & 0x01u) != 0u);
        if (changed & 0x02u)
            io.AddMouseButtonEvent(ImGuiMouseButton_Right,  (evt->buttons & 0x02u) != 0u);
        if (changed & 0x04u)
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, (evt->buttons & 0x04u) != 0u);
        bd->mouse_buttons = evt->buttons;
    }
}

/* Pack RGB into a 32-bit framebuffer pixel word. */
static inline unsigned int pack_px(unsigned int r, unsigned int g, unsigned int b,
                                    vk_pixel_format_t fmt)
{
    if (fmt == VK_PIXEL_FORMAT_BGRX_8BPP)
        return (b << 16) | (g << 8) | r;
    return (r << 16) | (g << 8) | b; /* RGBX and fallback */
}

/* Extract RGB channels from a packed pixel word. */
static inline void unpack_px(unsigned int px, vk_pixel_format_t fmt,
                               unsigned int* r, unsigned int* g, unsigned int* b)
{
    if (fmt == VK_PIXEL_FORMAT_BGRX_8BPP) {
        *r = (px      ) & 0xFFu;
        *g = (px >>  8) & 0xFFu;
        *b = (px >> 16) & 0xFFu;
    } else {
        *r = (px >> 16) & 0xFFu;
        *g = (px >>  8) & 0xFFu;
        *b = (px      ) & 0xFFu;
    }
}

/* ================================================================
 * Axis-aligned quad fast path
 *
 * ~90% of ImGui draws are axis-aligned rectangles (2 triangles, 4
 * unique vertices).  Detecting these and filling them as simple rects
 * eliminates:
 *   - The per-pixel coverage test (every pixel inside the rect is hit)
 *   - The 2x bounding-box iteration (one rect pass instead of two
 *     triangle passes that each scan the full bbox)
 *   - 30+ float derivative computations per triangle for solid fills
 *
 * For a 500x500 window background this alone is a ~4x speedup:
 *   Before: 2 triangles * 250k pixels * (coverage test + step) = 500k iters
 *   After:  1 rect * 250k pixels * (store) = 250k iters, zero tests
 * ================================================================ */

__attribute__((optimize("O2")))
static bool try_render_quad(
    const ImDrawVert* vtx, const ImDrawIdx* idx,
    int cx0, int cy0, int cx1, int cy1,
    const unsigned char* ftex, int ftw, int fth,
    unsigned int* fb, int fb_w, int fb_h, int fb_stride,
    vk_pixel_format_t fmt)
{
    /* Collect up to 4 unique vertex indices from the 6 indices. */
    ImDrawIdx ui[4];
    int nu = 0;
    for (int i = 0; i < 6; ++i) {
        ImDrawIdx vi = idx[i];
        bool dup = false;
        for (int j = 0; j < nu; ++j)
            if (ui[j] == vi) { dup = true; break; }
        if (!dup) {
            if (nu >= 4) return false;
            ui[nu++] = vi;
        }
    }
    if (nu != 4) return false;

    const ImDrawVert* v[4] = { &vtx[ui[0]], &vtx[ui[1]], &vtx[ui[2]], &vtx[ui[3]] };

    /* Find bounding box. */
    float minx = v[0]->pos.x, maxx = v[0]->pos.x;
    float miny = v[0]->pos.y, maxy = v[0]->pos.y;
    for (int i = 1; i < 4; ++i) {
        if (v[i]->pos.x < minx) minx = v[i]->pos.x;
        if (v[i]->pos.x > maxx) maxx = v[i]->pos.x;
        if (v[i]->pos.y < miny) miny = v[i]->pos.y;
        if (v[i]->pos.y > maxy) maxy = v[i]->pos.y;
    }

    /* Axis-aligned check: every vertex must sit on a bbox edge. */
    for (int i = 0; i < 4; ++i) {
        if ((v[i]->pos.x != minx && v[i]->pos.x != maxx) ||
            (v[i]->pos.y != miny && v[i]->pos.y != maxy))
            return false;
    }

    /* Clip to clip-rect and framebuffer. */
    int x0 = (int)minx,            y0 = (int)miny;
    int x1 = (int)(maxx + 0.5f),   y1 = (int)(maxy + 0.5f);
    if (x0 < cx0) x0 = cx0;  if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;  if (y1 > cy1) y1 = cy1;
    if (x0 < 0)   x0 = 0;    if (y0 < 0)   y0 = 0;
    if (x1 > fb_w) x1 = fb_w; if (y1 > fb_h) y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return true; /* fully clipped — handled */

    /* --- Same colour on all 4 vertices: solid fill path --- */
    if (v[0]->col == v[1]->col && v[1]->col == v[2]->col && v[2]->col == v[3]->col) {
        ImU32 col = v[0]->col;

        /* True solid fill iff all 4 vertices share the same UV
         * (single texel sample for the whole quad).  We must NOT
         * key off the *sampled* alpha values because text glyph
         * quads frequently have all 4 corners at zero-alpha border
         * texels in the font atlas — that would falsely classify
         * a glyph as a transparent solid fill and skip it. */
        bool same_uv =
            v[0]->uv.x == v[1]->uv.x && v[1]->uv.x == v[2]->uv.x && v[2]->uv.x == v[3]->uv.x &&
            v[0]->uv.y == v[1]->uv.y && v[1]->uv.y == v[2]->uv.y && v[2]->uv.y == v[3]->uv.y;

        if (same_uv) {
            /* Sample the (single) texel and compute combined alpha. */
            int tx = (int)(v[0]->uv.x * (float)ftw); if (tx < 0) tx = 0; else if (tx >= ftw) tx = ftw - 1;
            int ty = (int)(v[0]->uv.y * (float)fth); if (ty < 0) ty = 0; else if (ty >= fth) ty = fth - 1;
            unsigned char ts = ftex[ty * ftw + tx];

            float alpha = (float)((col >> 24) & 0xFFu) * (float)ts
                          * (1.0f / (255.0f * 255.0f));

            if (alpha <= 0.002f) return true; /* invisible */

            unsigned int cr = (col      ) & 0xFFu;
            unsigned int cg = (col >>  8) & 0xFFu;
            unsigned int cb = (col >> 16) & 0xFFu;

            if (!g_blend_enabled || alpha >= 0.999f) {
                /* Opaque rect: tight store loop, no read-modify-write. */
                unsigned int packed = pack_px(cr, cg, cb, fmt);
                for (int py = y0; py < y1; ++py) {
                    unsigned int* row = fb + py * fb_stride;
                    for (int px = x0; px < x1; ++px)
                        row[px] = packed;
                }
                return true;
            }

            /* Semi-transparent solid rect (blend mode). */
            float inv_a = 1.0f - alpha;
            float sr = (float)cr * alpha;
            float sg = (float)cg * alpha;
            float sb = (float)cb * alpha;
            for (int py = y0; py < y1; ++py) {
                unsigned int* row = fb + py * fb_stride;
                for (int px = x0; px < x1; ++px) {
                    unsigned int dr, dg, db;
                    unpack_px(row[px], fmt, &dr, &dg, &db);
                    unsigned int or_ = (unsigned int)(sr + (float)dr * inv_a + 0.5f); if (or_ > 255u) or_ = 255u;
                    unsigned int og  = (unsigned int)(sg + (float)dg * inv_a + 0.5f); if (og  > 255u) og  = 255u;
                    unsigned int ob  = (unsigned int)(sb + (float)db * inv_a + 0.5f); if (ob  > 255u) ob  = 255u;
                    row[px] = pack_px(or_, og, ob, fmt);
                }
            }
            return true;
        }

        /* Same colour but UVs differ (text glyphs).
         * Linearly interpolate UV across the rect; find corners. */
        float u_left = 0, u_right = 0, v_top = 0, v_bottom = 0;
        for (int i = 0; i < 4; ++i) {
            if (v[i]->pos.x == minx && v[i]->pos.y == miny) { u_left  = v[i]->uv.x; v_top    = v[i]->uv.y; }
            if (v[i]->pos.x == maxx && v[i]->pos.y == maxy) { u_right = v[i]->uv.x; v_bottom = v[i]->uv.y; }
        }
        float rect_w = maxx - minx, rect_h = maxy - miny;
        if (rect_w < 0.5f || rect_h < 0.5f) return false;

        float du_dx = (u_right - u_left) / rect_w;
        float dv_dy = (v_bottom - v_top) / rect_h;

        unsigned int cr = col & 0xFFu, cg = (col >> 8) & 0xFFu, cb = (col >> 16) & 0xFFu;
        float fva  = (float)((col >> 24) & 0xFFu) * (1.0f / (255.0f * 255.0f));
        float fcr  = (float)cr, fcg = (float)cg, fcb = (float)cb;
        float ftw_f = (float)ftw, fth_f = (float)fth;

        float v_row = v_top + ((float)y0 + 0.5f - miny) * dv_dy;
        float u_base = u_left + ((float)x0 + 0.5f - minx) * du_dx;

        for (int py = y0; py < y1; ++py) {
            float su = u_base;
            unsigned int* row = fb + py * fb_stride;
            for (int px = x0; px < x1; ++px) {
                int tx = (int)(su * ftw_f);
                int ty = (int)(v_row * fth_f);
                if (tx < 0) tx = 0; else if (tx >= ftw) tx = ftw - 1;
                if (ty < 0) ty = 0; else if (ty >= fth) ty = fth - 1;
                unsigned char ta_s = ftex[ty * ftw + tx];
                if (ta_s > 0) {
                    if (g_blend_enabled) {
                        float alpha = (float)ta_s * fva;
                        if (alpha >= 0.999f) {
                            row[px] = pack_px(cr, cg, cb, fmt);
                        } else {
                            float inv_a = 1.0f - alpha;
                            unsigned int dr, dg, db;
                            unpack_px(row[px], fmt, &dr, &dg, &db);
                            unsigned int or_ = (unsigned int)(fcr * alpha + (float)dr * inv_a + 0.5f); if (or_ > 255u) or_ = 255u;
                            unsigned int og  = (unsigned int)(fcg * alpha + (float)dg * inv_a + 0.5f); if (og  > 255u) og  = 255u;
                            unsigned int ob  = (unsigned int)(fcb * alpha + (float)db * inv_a + 0.5f); if (ob  > 255u) ob  = 255u;
                            row[px] = pack_px(or_, og, ob, fmt);
                        }
                    } else {
                        /* Opaque write — no alpha blending. */
                        row[px] = pack_px(cr, cg, cb, fmt);
                    }
                }
                su += du_dx;
            }
            v_row += dv_dy;
        }
        return true;
    }

    return false; /* different vertex colours — fall back to triangles */
}

/* ================================================================
 * Software renderer -- triangle rasterizer
 * ================================================================ */

/* Signed edge function: positive if (px,py) is on the left of AB. */
static inline float edge_fn(float ax, float ay,
                              float bx, float by,
                              float cx, float cy)
{
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

/* Sample alpha-8 font texture, nearest-neighbor, clamp to border. */
static inline unsigned char sample_alpha8(const unsigned char* tex,
                                           int tw, int th,
                                           float u, float v)
{
    int tx = (int)(u * (float)tw);
    int ty = (int)(v * (float)th);
    if (tx < 0)        tx = 0;
    else if (tx >= tw) tx = tw - 1;
    if (ty < 0)        ty = 0;
    else if (ty >= th) ty = th - 1;
    return tex[ty * tw + tx];
}

/*
 * Rasterise one ImGui triangle (three ImDrawVert vertices).
 * Clip rect [cx0,cy0)--[cx1,cy1) is applied in addition to the framebuffer bounds.
 *
 * Performance notes:
 *   - Incremental edge-function evaluation: per-row and per-column float steps
 *     are precomputed so the inner loop only does additions (no multiplies for
 *     coverage/interpolation).
 *   - Opaque fast path: when combined alpha >= 1.0 (most UI fills and outlines)
 *     the destination pixel is not read and no blend multiply is needed.
 *   - Force O2 even in debug builds so QEMU stays interactive.
 */
__attribute__((optimize("O2")))
static void rasterize_triangle(
    const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2,
    int cx0, int cy0, int cx1, int cy1,
    const unsigned char* ftex, int ftw, int fth,
    unsigned int* fb, int fb_w, int fb_h, int fb_stride,
    vk_pixel_format_t fmt)
{
    /* --- Bounding box clipped to clip rect + framebuffer --- */
    float fminx = v0.pos.x, fminy = v0.pos.y;
    float fmaxx = v0.pos.x, fmaxy = v0.pos.y;

    if (v1.pos.x < fminx) fminx = v1.pos.x;
    if (v2.pos.x < fminx) fminx = v2.pos.x;
    if (v1.pos.y < fminy) fminy = v1.pos.y;
    if (v2.pos.y < fminy) fminy = v2.pos.y;
    if (v1.pos.x > fmaxx) fmaxx = v1.pos.x;
    if (v2.pos.x > fmaxx) fmaxx = v2.pos.x;
    if (v1.pos.y > fmaxy) fmaxy = v1.pos.y;
    if (v2.pos.y > fmaxy) fmaxy = v2.pos.y;

    int x0 = (int)fminx;            int y0 = (int)fminy;
    int x1 = (int)(fmaxx + 1.0f);   int y1 = (int)(fmaxy + 1.0f);

    if (x0 < cx0)   x0 = cx0;
    if (y0 < cy0)   y0 = cy0;
    if (x1 > cx1)   x1 = cx1;
    if (y1 > cy1)   y1 = cy1;
    if (x0 < 0)     x0 = 0;
    if (y0 < 0)     y0 = 0;
    if (x1 > fb_w)  x1 = fb_w;
    if (y1 > fb_h)  y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return;

    /* --- Triangle area; skip degenerate triangles --- */
    float area = edge_fn(v0.pos.x, v0.pos.y,
                          v1.pos.x, v1.pos.y,
                          v2.pos.x, v2.pos.y);
    if (area == 0.0f) return;
    float inv = 1.0f / area;

    /* --- Edge function step values ---
     * For edge e_i, moving +1 in x adds A_i; moving +1 in y adds B_i.
     * e0 = edge(v1->v2), weight for v0.
     * e1 = edge(v2->v0), weight for v1.
     * A2 = -(A0+A1), B2 = -(B0+B1)  because A0+A1+A2 = 0.
     */
    float A0 = v2.pos.y - v1.pos.y,  B0 = v1.pos.x - v2.pos.x;
    float A1 = v0.pos.y - v2.pos.y,  B1 = v2.pos.x - v0.pos.x;
    float A2 = -(A0 + A1),            B2 = -(B0 + B1);

    /* Coverage weight steps */
    float dw0_dx = A0 * inv,  dw0_dy = B0 * inv;
    float dw1_dx = A1 * inv,  dw1_dy = B1 * inv;

    /* --- Evaluate coverage at the first sample point (x0+0.5, y0+0.5) --- */
    float px0f = (float)x0 + 0.5f;
    float py0f = (float)y0 + 0.5f;
    float w0_row = edge_fn(v1.pos.x, v1.pos.y, v2.pos.x, v2.pos.y, px0f, py0f) * inv;
    float w1_row = edge_fn(v2.pos.x, v2.pos.y, v0.pos.x, v0.pos.y, px0f, py0f) * inv;

    float ftw_f = (float)ftw,  fth_f = (float)fth;

    /* --- Constant-colour fast path (checked BEFORE computing attribute derivatives) ---
     * Most ImGui draws (window backgrounds, title bars, panels, buttons)
     * are solid-filled rectangles: all 3 vertices carry the same ImU32
     * colour and their UVs all land on the same texel.  When detected,
     * skip all per-pixel attribute interpolation; only the two barycentric
     * coverage weights are stepped per pixel.
     */
    if (v0.col == v1.col && v1.col == v2.col) {
        auto clamp_tx = [&](float u) -> int {
            int t = (int)(u * ftw_f); return t < 0 ? 0 : t >= ftw ? ftw - 1 : t;
        };
        auto clamp_ty = [&](float v_) -> int {
            int t = (int)(v_ * fth_f); return t < 0 ? 0 : t >= fth ? fth - 1 : t;
        };
        unsigned char ta0 = ftex[clamp_ty(v0.uv.y) * ftw + clamp_tx(v0.uv.x)];
        unsigned char ta1 = ftex[clamp_ty(v1.uv.y) * ftw + clamp_tx(v1.uv.x)];
        unsigned char ta2 = ftex[clamp_ty(v2.uv.y) * ftw + clamp_tx(v2.uv.x)];
        if (ta0 == ta1 && ta1 == ta2) {
            unsigned int va  = (v0.col >> 24) & 0xFFu;
            float alpha = (float)va * (float)ta0 * (1.0f / (255.0f * 255.0f));
            if (alpha >= 0.002f) {
                unsigned int cr = (v0.col      ) & 0xFFu;
                unsigned int cg = (v0.col >>  8) & 0xFFu;
                unsigned int cb = (v0.col >> 16) & 0xFFu;

                if (!g_blend_enabled || alpha >= 0.999f) {
                    /* Opaque: just coverage-test + store */
                    unsigned int packed = pack_px(cr, cg, cb, fmt);
                    for (int py = y0; py < y1; ++py) {
                        float w0 = w0_row, w1 = w1_row;
                        unsigned int* row = fb + py * fb_stride;
                        for (int px = x0; px < x1; ++px) {
                            if (w0 >= -0.001f && w1 >= -0.001f && (1.0f - w0 - w1) >= -0.001f)
                                row[px] = packed;
                            w0 += dw0_dx; w1 += dw1_dx;
                        }
                        w0_row += dw0_dy; w1_row += dw1_dy;
                    }
                    return;
                }

                /* Constant semi-transparent: blend with precomputed src terms */
                float inv_a = 1.0f - alpha;
                float sr = (float)cr * alpha;
                float sg = (float)cg * alpha;
                float sb = (float)cb * alpha;
                for (int py = y0; py < y1; ++py) {
                    float w0 = w0_row, w1 = w1_row;
                    unsigned int* row = fb + py * fb_stride;
                    for (int px = x0; px < x1; ++px) {
                        if (w0 >= -0.001f && w1 >= -0.001f && (1.0f - w0 - w1) >= -0.001f) {
                            unsigned int dr, dg, db;
                            unpack_px(row[px], fmt, &dr, &dg, &db);
                            unsigned int or_ = (unsigned int)(sr + (float)dr * inv_a + 0.5f); if (or_ > 255u) or_ = 255u;
                            unsigned int og  = (unsigned int)(sg + (float)dg * inv_a + 0.5f); if (og  > 255u) og  = 255u;
                            unsigned int ob  = (unsigned int)(sb + (float)db * inv_a + 0.5f); if (ob  > 255u) ob  = 255u;
                            row[px] = pack_px(or_, og, ob, fmt);
                        }
                        w0 += dw0_dx; w1 += dw1_dx;
                    }
                    w0_row += dw0_dy; w1_row += dw1_dy;
                }
                return;
            }
        }
    }

    /* --- General path: compute full attribute derivatives --- */
    float r0 = (float)((v0.col      ) & 0xFFu);
    float g0 = (float)((v0.col >>  8) & 0xFFu);
    float b0 = (float)((v0.col >> 16) & 0xFFu);
    float a0 = (float)((v0.col >> 24) & 0xFFu);
    float r1 = (float)((v1.col      ) & 0xFFu);
    float g1 = (float)((v1.col >>  8) & 0xFFu);
    float b1 = (float)((v1.col >> 16) & 0xFFu);
    float a1 = (float)((v1.col >> 24) & 0xFFu);
    float r2 = (float)((v2.col      ) & 0xFFu);
    float g2 = (float)((v2.col >>  8) & 0xFFu);
    float b2 = (float)((v2.col >> 16) & 0xFFu);
    float a2 = (float)((v2.col >> 24) & 0xFFu);
    float u0 = v0.uv.x, u1 = v1.uv.x, u2 = v2.uv.x;
    float vc0= v0.uv.y, vc1= v1.uv.y, vc2= v2.uv.y;

    float dr_dx  = (A0*r0  + A1*r1  + A2*r2)  * inv;
    float dg_dx  = (A0*g0  + A1*g1  + A2*g2)  * inv;
    float db_dx  = (A0*b0  + A1*b1  + A2*b2)  * inv;
    float da_dx  = (A0*a0  + A1*a1  + A2*a2)  * inv;
    float du_dx  = (A0*u0  + A1*u1  + A2*u2)  * inv;
    float dvc_dx = (A0*vc0 + A1*vc1 + A2*vc2) * inv;

    float dr_dy  = (B0*r0  + B1*r1  + B2*r2)  * inv;
    float dg_dy  = (B0*g0  + B1*g1  + B2*g2)  * inv;
    float db_dy  = (B0*b0  + B1*b1  + B2*b2)  * inv;
    float da_dy  = (B0*a0  + B1*a1  + B2*a2)  * inv;
    float du_dy  = (B0*u0  + B1*u1  + B2*u2)  * inv;
    float dvc_dy = (B0*vc0 + B1*vc1 + B2*vc2) * inv;

    float w2_row = 1.0f - w0_row - w1_row;
    float r_row  = w0_row*r0  + w1_row*r1  + w2_row*r2;
    float g_row  = w0_row*g0  + w1_row*g1  + w2_row*g2;
    float b_row  = w0_row*b0  + w1_row*b1  + w2_row*b2;
    float a_row  = w0_row*a0  + w1_row*a1  + w2_row*a2;
    float u_row  = w0_row*u0  + w1_row*u1  + w2_row*u2;
    float vc_row = w0_row*vc0 + w1_row*vc1 + w2_row*vc2;

    /* --- Main rasterization loop --- */
    for (int py = y0; py < y1; ++py) {
        float w0 = w0_row,  w1 = w1_row;
        float sr = r_row,   sg = g_row,  sb = b_row,  sa = a_row;
        float su = u_row,   sv = vc_row;
        unsigned int* row = fb + py * fb_stride;

        for (int px = x0; px < x1; ++px) {
            /* Coverage test: w2 = 1 - w0 - w1 */
            if (w0 >= -0.001f && w1 >= -0.001f && (1.0f - w0 - w1) >= -0.001f) {

                /* Texture sample (integer UV, no division) */
                int tx = (int)(su * ftw_f);
                int ty = (int)(sv * fth_f);
                if (tx < 0) tx = 0; else if (tx >= ftw) tx = ftw - 1;
                if (ty < 0) ty = 0; else if (ty >= fth) ty = fth - 1;
                unsigned char ta = ftex[ty * ftw + tx];

                float alpha = sa * (float)ta * (1.0f / (255.0f * 255.0f));

                if (alpha >= 0.002f) {
                    if (!g_blend_enabled || alpha >= 0.999f) {
                        /* ---- Opaque write: skip read-modify-write ---- */
                        unsigned int fr  = (unsigned int)(sr + 0.5f); if (fr  > 255u) fr  = 255u;
                        unsigned int fg  = (unsigned int)(sg + 0.5f); if (fg  > 255u) fg  = 255u;
                        unsigned int fbl = (unsigned int)(sb + 0.5f); if (fbl > 255u) fbl = 255u;
                        row[px] = pack_px(fr, fg, fbl, fmt);
                    } else {
                        /* ---- Alpha blend ---- */
                        unsigned int dr, dg, db;
                        unpack_px(row[px], fmt, &dr, &dg, &db);
                        float inv_a = 1.0f - alpha;
                        unsigned int fr  = (unsigned int)(sr * alpha + (float)dr * inv_a + 0.5f); if (fr  > 255u) fr  = 255u;
                        unsigned int fg  = (unsigned int)(sg * alpha + (float)dg * inv_a + 0.5f); if (fg  > 255u) fg  = 255u;
                        unsigned int fbl = (unsigned int)(sb * alpha + (float)db * inv_a + 0.5f); if (fbl > 255u) fbl = 255u;
                        row[px] = pack_px(fr, fg, fbl, fmt);
                    }
                }
            }

            /* Increment all attributes by one x-step (additions only) */
            w0 += dw0_dx;  w1 += dw1_dx;
            sr += dr_dx;   sg += dg_dx;   sb += db_dx;   sa += da_dx;
            su += du_dx;   sv += dvc_dx;
        }

        /* Advance row base values by one y-step */
        w0_row  += dw0_dy;  w1_row  += dw1_dy;
        r_row   += dr_dy;   g_row   += dg_dy;   b_row   += db_dy;   a_row   += da_dy;
        u_row   += du_dy;   vc_row  += dvc_dy;
    }
}

/* ================================================================
 * Public render entry point
 * ================================================================ */

/*
 * Draw a simple software cross cursor into the back-buffer.
 */
static void draw_cursor(unsigned int* buf, int W, int H, int S,
                         int cx, int cy, vk_pixel_format_t fmt)
{
    unsigned int white  = pack_px(255, 255, 255, fmt);
    unsigned int shadow = pack_px(0,   0,   0,   fmt);
    const int ARM = 8;

    for (int i = -ARM; i <= ARM; ++i) {
        /* Horizontal shadow then white */
        int sx = cx + i, sy = cy + 1;
        if (sx >= 0 && sx < W && sy >= 0 && sy < H) buf[sy * S + sx] = shadow;
        sy = cy;
        if (sx >= 0 && sx < W && sy >= 0 && sy < H) buf[sy * S + sx] = white;
        /* Vertical shadow then white */
        sx = cx + 1; sy = cy + i;
        if (sx >= 0 && sx < W && sy >= 0 && sy < H) buf[sy * S + sx] = shadow;
        sx = cx;
        if (sx >= 0 && sx < W && sy >= 0 && sy < H) buf[sy * S + sx] = white;
    }
}

__attribute__((optimize("O2")))
void ImGui_ImplVK_RenderDrawData(ImDrawData* draw_data,
                                  const vk_framebuffer_info_t* fb)
{
    if (!draw_data || draw_data->CmdListsCount == 0) return;
    if (!fb || !fb->valid || fb->base == 0) return;

    ImGui_ImplVK_Data* bd = get_bd();
    if (!bd || !bd->font_pixels || !bd->backbuf) return;

    unsigned int*     screen = (unsigned int*)(vk_usize)fb->base;
    unsigned int*     pixels = bd->backbuf;   /* render to back-buffer */
    int               W      = (int)fb->width;
    int               H      = (int)fb->height;
    int               S      = (int)fb->stride;
    vk_pixel_format_t fmt    = fb->format;

    /* --- Clear back-buffer to dark background --- */
    {
        unsigned int bg = pack_px(22, 22, 30, fmt);
        if (S == W) {
            /* Flat single pass; compiler will auto-vectorize (SSE/AVX). */
            unsigned int* p   = pixels;
            unsigned int* end = pixels + (vk_usize)W * H;
            while (p < end) *p++ = bg;
        } else {
            for (int y = 0; y < H; ++y) {
                unsigned int* row = pixels + y * S;
                for (int x = 0; x < W; ++x)
                    row[x] = bg;
            }
        }
    }

    /* --- Walk draw lists --- */
    float scale_x = draw_data->FramebufferScale.x;
    float scale_y = draw_data->FramebufferScale.y;

    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* dl = draw_data->CmdLists[n];

        for (int ci = 0; ci < dl->CmdBuffer.Size; ++ci) {
            const ImDrawCmd& cmd = dl->CmdBuffer[ci];

            /* User callbacks (e.g. custom rendering). */
            if (cmd.UserCallback) {
                cmd.UserCallback(dl, &cmd);
                continue;
            }

            /* Clip rect in framebuffer pixels. */
            int clip_x0 = (int)(cmd.ClipRect.x * scale_x);
            int clip_y0 = (int)(cmd.ClipRect.y * scale_y);
            int clip_x1 = (int)(cmd.ClipRect.z * scale_x);
            int clip_y1 = (int)(cmd.ClipRect.w * scale_y);
            if (clip_x0 < 0)  clip_x0 = 0;
            if (clip_y0 < 0)  clip_y0 = 0;
            if (clip_x1 > W)  clip_x1 = W;
            if (clip_y1 > H)  clip_y1 = H;
            if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) continue;

            /* Vertex and index data for this command. */
            const ImDrawVert* vtx = dl->VtxBuffer.Data + cmd.VtxOffset;
            const ImDrawIdx*  idx = dl->IdxBuffer.Data  + cmd.IdxOffset;

            /* Rasterise triangles (3 indices each).
             * Try to batch every pair of triangles as an axis-aligned
             * quad first — this is the common case and far cheaper. */
            unsigned int i = 0;
            for (; i + 5 < cmd.ElemCount; i += 6) {
                if (!try_render_quad(
                        vtx, idx + i,
                        clip_x0, clip_y0, clip_x1, clip_y1,
                        bd->font_pixels, bd->font_tex_w, bd->font_tex_h,
                        pixels, W, H, S, fmt))
                {
                    /* Not an AA quad — render as two separate triangles. */
                    rasterize_triangle(
                        vtx[idx[i]], vtx[idx[i+1]], vtx[idx[i+2]],
                        clip_x0, clip_y0, clip_x1, clip_y1,
                        bd->font_pixels, bd->font_tex_w, bd->font_tex_h,
                        pixels, W, H, S, fmt);
                    rasterize_triangle(
                        vtx[idx[i+3]], vtx[idx[i+4]], vtx[idx[i+5]],
                        clip_x0, clip_y0, clip_x1, clip_y1,
                        bd->font_pixels, bd->font_tex_w, bd->font_tex_h,
                        pixels, W, H, S, fmt);
                }
            }
            /* Handle any leftover odd triangle. */
            for (; i + 2 < cmd.ElemCount; i += 3) {
                rasterize_triangle(
                    vtx[idx[i]], vtx[idx[i + 1]], vtx[idx[i + 2]],
                    clip_x0, clip_y0, clip_x1, clip_y1,
                    bd->font_pixels, bd->font_tex_w, bd->font_tex_h,
                    pixels, W, H, S, fmt);
            }
        }
    }

    /* --- Draw software mouse cursor --- */
    draw_cursor(pixels, W, H, S, (int)bd->mouse_x, (int)bd->mouse_y, fmt);

    /* --- Blit back-buffer to UEFI framebuffer (single shot, eliminates flicker) --- */
    if (S == W) {
        memcpy(screen, pixels, (vk_usize)W * H * sizeof(unsigned int));
    } else {
        for (int y = 0; y < H; ++y) {
            const unsigned int* src = pixels + y * S;
            unsigned int*       dst = screen + y * S;
            memcpy(dst, src, (vk_usize)W * sizeof(unsigned int));
        }
    }
}
