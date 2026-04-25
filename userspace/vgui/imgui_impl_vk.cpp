/*
 * vgui/imgui_impl_vk.cpp
 * vkernel platform + renderer backend for Dear ImGui.
 *
 * Renderer design:
 *   ImGui produces a list of textured triangles (ImDrawList).  Each
 *   triangle has three interpolated (pos, uv, color) vertices.
 *   The only texture is the font atlas (alpha-8 bitmap).
 *
 *   For every pixel inside a triangle we:
 *     1. Compute barycentric weights (edge-function rasterizer).
 *     2. Interpolate UV and RGBA color.
 *     3. Sample the alpha-8 font texture (nearest-neighbor).
 *     4. Multiply sampled alpha by vertex alpha → final alpha.
 *     5. Alpha-blend onto the framebuffer.
 *
 *   This is intentionally simple; for a keyboard-driven UI the frame
 *   rate is adequate even on modest hardware.
 */

#include "imgui_impl_vk.h"

#include <string.h>   /* memset */
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
};

static ImGui_ImplVK_Data* get_bd()
{
    return ImGui::GetCurrentContext()
        ? (ImGui_ImplVK_Data*)ImGui::GetIO().BackendPlatformUserData
        : nullptr;
}

/* ================================================================
 * Custom allocators — route through newlib malloc/free
 * (newlib's heap is bootstrapped from the kernel allocator via _sbrk)
 * ================================================================ */

static void* vk_imgui_alloc(size_t sz, void*) { return malloc(sz); }
static void  vk_imgui_free (void*  p,  void*) { free(p); }

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

    /* Hook ImGui allocation through newlib so the kernel heap is used. */
    ImGui::SetAllocatorFunctions(vk_imgui_alloc, vk_imgui_free);

    io.BackendPlatformName     = "imgui_impl_vk";
    io.BackendRendererName     = "imgui_impl_vk_sw";
    io.BackendPlatformUserData = bd;

    io.DisplaySize             = ImVec2((float)fb->width, (float)fb->height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    /* Keyboard-only navigation: no mouse cursor needed. */
    io.ConfigFlags            |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags           |= ImGuiBackendFlags_HasSetMousePos; /* suppress cursor warnings */

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
    if (bd) free(bd);
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

    /* Signal continuous focus so keyboard nav is always active. */
    io.AddFocusEvent(true);
}

/* ================================================================
 * Keyboard input
 * ================================================================ */

static ImGuiKey scancode_to_key(vk_u32 sc)
{
    /* PS/2 Scan Code Set 1 make codes → ImGuiKey */
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
 * Software renderer — triangle rasterizer
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
    if (tx < 0)       tx = 0;
    else if (tx >= tw) tx = tw - 1;
    if (ty < 0)       ty = 0;
    else if (ty >= th) ty = th - 1;
    return tex[ty * tw + tx];
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

/*
 * Rasterise one ImGui triangle (three ImDrawVert vertices).
 * Clip rect [cx0,cy0)–[cx1,cy1) is applied in addition to the framebuffer bounds.
 */
static void rasterize_triangle(
    const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2,
    int cx0, int cy0, int cx1, int cy1,
    const unsigned char* ftex, int ftw, int fth,
    unsigned int* fb, int fb_w, int fb_h, int fb_stride,
    vk_pixel_format_t fmt)
{
    /* --- Compute integer bounding box, clipped to clip rect + framebuffer --- */
    float fminx = v0.pos.x, fminy = v0.pos.y;
    float fmaxx = v0.pos.x, fmaxy = v0.pos.y;

#define VGUI_MINMAX(a, b) if ((a) < fmin##a) fmin##a = (a); if ((a) > fmax##a) fmax##a = (a)
    if (v1.pos.x < fminx) fminx = v1.pos.x;
    if (v2.pos.x < fminx) fminx = v2.pos.x;
    if (v1.pos.y < fminy) fminy = v1.pos.y;
    if (v2.pos.y < fminy) fminy = v2.pos.y;
    if (v1.pos.x > fmaxx) fmaxx = v1.pos.x;
    if (v2.pos.x > fmaxx) fmaxx = v2.pos.x;
    if (v1.pos.y > fmaxy) fmaxy = v1.pos.y;
    if (v2.pos.y > fmaxy) fmaxy = v2.pos.y;
#undef VGUI_MINMAX

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

    /* --- Unpack vertex colors: ImU32 layout = A[31:24] B[23:16] G[15:8] R[7:0] --- */
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

    /* --- Per-pixel loop --- */
    for (int py = y0; py < y1; ++py) {
        float cy = (float)py + 0.5f;
        unsigned int* row = fb + py * fb_stride;

        for (int px = x0; px < x1; ++px) {
            float cx = (float)px + 0.5f;

            float w0 = edge_fn(v1.pos.x, v1.pos.y,
                                v2.pos.x, v2.pos.y, cx, cy) * inv;
            float w1 = edge_fn(v2.pos.x, v2.pos.y,
                                v0.pos.x, v0.pos.y, cx, cy) * inv;
            float w2 = edge_fn(v0.pos.x, v0.pos.y,
                                v1.pos.x, v1.pos.y, cx, cy) * inv;

            /* Skip pixels outside the triangle (with small epsilon). */
            if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;

            /* Interpolate UV coordinates. */
            float u  = w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x;
            float vc = w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y;

            /* Interpolate RGBA. */
            float sr = w0 * r0 + w1 * r1 + w2 * r2;
            float sg = w0 * g0 + w1 * g1 + w2 * g2;
            float sb = w0 * b0 + w1 * b1 + w2 * b2;
            float sa = w0 * a0 + w1 * a1 + w2 * a2;

            /* Sample font atlas (alpha-8). */
            unsigned char ta = sample_alpha8(ftex, ftw, fth, u, vc);

            /* Final alpha = vertex_alpha * texture_alpha / 255^2 */
            float alpha = sa * (float)ta * (1.0f / (255.0f * 255.0f));
            if (alpha < 0.002f) continue;   /* nearly transparent, skip */

            /* Alpha blend: dst = src * alpha + dst * (1 - alpha) */
            unsigned int dr, dg, db;
            unpack_px(row[px], fmt, &dr, &dg, &db);

            float inv_a = 1.0f - alpha;
            unsigned int fr = (unsigned int)(sr * alpha + (float)dr * inv_a + 0.5f);
            unsigned int fg = (unsigned int)(sg * alpha + (float)dg * inv_a + 0.5f);
            unsigned int fbl = (unsigned int)(sb * alpha + (float)db * inv_a + 0.5f);

            if (fr  > 255u) fr  = 255u;
            if (fg  > 255u) fg  = 255u;
            if (fbl > 255u) fbl = 255u;

            row[px] = pack_px(fr, fg, fbl, fmt);
        }
    }
}

/* ================================================================
 * Public render entry point
 * ================================================================ */

void ImGui_ImplVK_RenderDrawData(ImDrawData* draw_data,
                                  const vk_framebuffer_info_t* fb)
{
    if (!draw_data || draw_data->CmdListsCount == 0) return;
    if (!fb || !fb->valid || fb->base == 0) return;

    ImGui_ImplVK_Data* bd = get_bd();
    if (!bd || !bd->font_pixels) return;

    unsigned int*     pixels = (unsigned int*)(vk_usize)fb->base;
    int               W      = (int)fb->width;
    int               H      = (int)fb->height;
    int               S      = (int)fb->stride;
    vk_pixel_format_t fmt    = fb->format;

    /* --- Clear framebuffer to a dark background --- */
    {
        unsigned int bg = pack_px(22, 22, 30, fmt);
        for (int y = 0; y < H; ++y) {
            unsigned int* row = pixels + y * S;
            for (int x = 0; x < W; ++x)
                row[x] = bg;
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

            /* Rasterise triangles (3 indices each). */
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
                rasterize_triangle(
                    vtx[idx[i]], vtx[idx[i + 1]], vtx[idx[i + 2]],
                    clip_x0, clip_y0, clip_x1, clip_y1,
                    bd->font_pixels, bd->font_tex_w, bd->font_tex_h,
                    pixels, W, H, S, fmt);
            }
        }
    }
}
