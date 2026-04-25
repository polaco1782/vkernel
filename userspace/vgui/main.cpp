/*
 * vgui/main.cpp
 * Dear ImGui demonstration for vkernel.
 *
 * Keyboard navigation (no mouse required):
 *   Alt / F10          Activate main menu bar
 *   Arrow keys         Navigate menus / lists / sliders
 *   Enter / Space      Activate buttons, checkboxes, open menus
 *   Tab / Shift+Tab    Move focus between widgets
 *   Escape             Close popup / deactivate widget
 *   Ctrl+N             File > New  (reset counter)
 *   Ctrl+Q             Quit
 */

#include "../include/vk.h"
#include "imgui/imgui.h"
#include "imgui_impl_vk.h"

#include <stdio.h>
#include <string.h>
#include <float.h>   /* FLT_MIN */

/* ================================================================
 * Application state
 * ================================================================ */

static bool g_running       = true;
static bool g_show_info     = true;
static bool g_show_console  = true;
static bool g_show_settings = false;
static bool g_show_demo     = false;
static bool g_open_about    = false;   /* one-shot: triggers OpenPopup */

/* --- Counter widget --- */
static int  g_counter        = 0;
static bool g_counter_wrap   = false;
static int  g_counter_max    = 100;

/* --- Settings --- */
static int  g_style_idx      = 0;      /* 0=Dark  1=Light  2=Classic */
static float g_font_scale    = 1.0f;

/* ================================================================
 * Simple circular console log  (no heap, fixed size)
 * ================================================================ */

static const int LOG_CAP  = 64;
static const int LOG_WIDTH = 128;

static char g_log_buf[LOG_CAP][LOG_WIDTH];
static int  g_log_head  = 0;   /* next write slot */
static int  g_log_count = 0;

static void log_add(const char* msg)
{
    strncpy(g_log_buf[g_log_head], msg, LOG_WIDTH - 1);
    g_log_buf[g_log_head][LOG_WIDTH - 1] = '\0';
    g_log_head  = (g_log_head + 1) % LOG_CAP;
    if (g_log_count < LOG_CAP) ++g_log_count;
}

static void log_addf(const char* fmt, ...)
{
    char tmp[LOG_WIDTH];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    log_add(tmp);
}

static void log_clear()
{
    g_log_count = 0;
    g_log_head  = 0;
}

/* ================================================================
 * apply_style — set ImGui color scheme from g_style_idx
 * ================================================================ */

static void apply_style(int idx)
{
    switch (idx) {
    case 1:  ImGui::StyleColorsLight();   break;
    case 2:  ImGui::StyleColorsClassic(); break;
    default: ImGui::StyleColorsDark();    break;
    }
}

/* ================================================================
 * draw_menu_bar
 * ================================================================ */

static void draw_menu_bar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    /* ---- File ---- */
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) {
            g_counter = 0;
            log_add("File > New: counter reset.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
            log_add("Quit requested via menu.");
            g_running = false;
        }
        ImGui::EndMenu();
    }

    /* ---- Edit ---- */
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Increment Counter", "+")) {
            ++g_counter;
            log_addf("Counter incremented to %d.", g_counter);
        }
        if (ImGui::MenuItem("Decrement Counter", "-")) {
            --g_counter;
            log_addf("Counter decremented to %d.", g_counter);
        }
        if (ImGui::MenuItem("Reset Counter")) {
            g_counter = 0;
            log_add("Counter reset.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...")) {
            g_show_settings = true;
            log_add("Opened Settings.");
        }
        ImGui::EndMenu();
    }

    /* ---- View ---- */
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Info Panel",   nullptr, &g_show_info);
        ImGui::MenuItem("Console",      nullptr, &g_show_console);
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo",   nullptr, &g_show_demo);
        ImGui::EndMenu();
    }

    /* ---- Help ---- */
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About vGUI...")) {
            g_open_about = true;
        }
        ImGui::EndMenu();
    }

    /* Right-aligned FPS */
    {
        ImGuiIO& io = ImGui::GetIO();
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", io.Framerate);
        float w = ImGui::CalcTextSize(fps_buf).x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - w);
        ImGui::TextDisabled("%s", fps_buf);
    }

    ImGui::EndMainMenuBar();
}

/* ================================================================
 * draw_info_window
 * ================================================================ */

static void draw_info_window(const vk_framebuffer_info_t* fb)
{
    if (!g_show_info) return;

    ImGui::SetNextWindowPos (ImVec2(10.0f, 30.0f),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 290.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Info Panel", &g_show_info)) {
        ImGui::End();
        return;
    }

    /* ---- Framebuffer info ---- */
    ImGui::SeparatorText("Framebuffer");
    ImGui::Text("Resolution  : %u x %u", fb->width, fb->height);
    ImGui::Text("Stride      : %u px",   fb->stride);
    ImGui::Text("Format      : %s",
        fb->format == VK_PIXEL_FORMAT_BGRX_8BPP ? "BGRX-8bpp" :
        fb->format == VK_PIXEL_FORMAT_RGBX_8BPP ? "RGBX-8bpp" :
        fb->format == VK_PIXEL_FORMAT_BITMASK   ? "Bitmask"   : "BLT-only");
    ImGui::Text("Base        : 0x%llx", (unsigned long long)fb->base);

    /* ---- Timing ---- */
    ImGui::SeparatorText("Timing");
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Frame time  : %.2f ms",  io.DeltaTime * 1000.0f);
    ImGui::Text("FPS         : %.1f",     io.Framerate);

    const vk_api_t* api  = vk_get_api();
    vk_u64          tick = api->vk_tick_count();
    vk_u32          tps  = api->vk_ticks_per_sec();
    vk_u64          sec  = tps ? (tick / tps) : 0;
    ImGui::Text("Uptime      : %llus",    (unsigned long long)sec);

    /* ---- Counter demo ---- */
    ImGui::SeparatorText("Counter");

    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##counter_val", &g_counter, 0, 0);

    ImGui::SameLine();
    if (ImGui::Button(" + ")) {
        if (g_counter_wrap && g_counter >= g_counter_max)
            g_counter = 0;
        else
            ++g_counter;
        log_addf("Counter → %d", g_counter);
    }
    ImGui::SameLine();
    if (ImGui::Button(" - ")) {
        --g_counter;
        log_addf("Counter → %d", g_counter);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        g_counter = 0;
        log_add("Counter reset.");
    }

    /* Clamp display value for progress bar */
    int   display_max = (g_counter_max > 0) ? g_counter_max : 1;
    float progress    = (float)g_counter / (float)display_max;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));

    ImGui::Checkbox("Wrap at max", &g_counter_wrap);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("Max", &g_counter_max, 1, 10);

    ImGui::End();
}

/* ================================================================
 * draw_console_window
 * ================================================================ */

static void draw_console_window()
{
    if (!g_show_console) return;

    ImGui::SetNextWindowPos (ImVec2(10.0f,  330.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500.0f, 200.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Console", &g_show_console)) {
        ImGui::End();
        return;
    }

    /* Scrolling text region. */
    float footer_h = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##log_scroll", ImVec2(0.0f, -footer_h),
                       false, ImGuiWindowFlags_HorizontalScrollbar);

    /* Print from oldest to newest (circular buffer iteration). */
    int start = (g_log_count >= LOG_CAP) ? g_log_head : 0;
    for (int i = 0; i < g_log_count; ++i) {
        int idx = (start + i) % LOG_CAP;
        ImGui::TextUnformatted(g_log_buf[idx]);
    }

    /* Auto-scroll to bottom. */
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Clear"))
        log_clear();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d lines", g_log_count, LOG_CAP);

    ImGui::End();
}

/* ================================================================
 * draw_settings_window
 * ================================================================ */

static void draw_settings_window()
{
    if (!g_show_settings) return;

    ImGui::SetNextWindowPos (ImVec2(200.0f, 150.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(270.0f, 170.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Settings", &g_show_settings, ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Appearance");

    const char* style_names[] = { "Dark", "Light", "Classic" };
    if (ImGui::Combo("Color scheme", &g_style_idx, style_names, 3)) {
        apply_style(g_style_idx);
        log_addf("Style changed to %s.", style_names[g_style_idx]);
    }

    if (ImGui::SliderFloat("Font scale", &g_font_scale, 0.5f, 2.0f, "%.1f")) {
        ImGui::GetIO().FontGlobalScale = g_font_scale;
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
        g_show_settings = false;

    ImGui::End();
}

/* ================================================================
 * draw_about_modal
 * ================================================================ */

static void draw_about_modal()
{
    /* One-shot: open on the frame the flag is set. */
    if (g_open_about) {
        ImGui::OpenPopup("About vGUI");
        g_open_about = false;
    }

    /* Center on screen each time the popup appears. */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(320.0f, 160.0f),     ImGuiCond_Always);

    if (ImGui::BeginPopupModal("About vGUI", nullptr,
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove))
    {
        ImGui::TextWrapped("vGUI — Dear ImGui GUI for vkernel");
        ImGui::Spacing();
        ImGui::TextWrapped("Software renderer: barycentric triangle fill,");
        ImGui::TextWrapped("alpha-8 font atlas, framebuffer blending.");
        ImGui::Spacing();
        ImGui::TextWrapped("Keyboard-only navigation, newlib C runtime.");
        ImGui::Spacing();
        ImGui::Separator();

        float btn_w = 100.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btn_w) * 0.5f);
        if (ImGui::Button("OK", ImVec2(btn_w, 0.0f)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

/* ================================================================
 * main
 * ================================================================ */

int main(int /*argc*/, char** /*argv*/)
{
    /* ---- Framebuffer ---- */
    vk_framebuffer_info_t fb = {};
    VK_CALL(framebuffer_info, &fb);

    if (!fb.valid || fb.base == 0 || fb.width == 0 || fb.height == 0) {
        VK_CALL(puts, "vgui: no framebuffer available\n");
        return 1;
    }

    /* ---- ImGui context ---- */
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   /* no .ini file (kernel has no writable FS) */
        io.LogFilename = nullptr;
    }

    /* ---- Backend ---- */
    if (!ImGui_ImplVK_Init(&fb)) {
        VK_CALL(puts, "vgui: backend init failed\n");
        ImGui::DestroyContext();
        return 1;
    }

    /* ---- Style ---- */
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& s     = ImGui::GetStyle();
        s.WindowRounding  = 5.0f;
        s.ChildRounding   = 3.0f;
        s.FrameRounding   = 3.0f;
        s.PopupRounding   = 4.0f;
        s.ScrollbarRounding = 3.0f;
        s.GrabRounding    = 3.0f;
        s.WindowBorderSize = 1.0f;
        s.FrameBorderSize  = 0.0f;
    }

    /* ---- Startup log ---- */
    log_add("vGUI started.");
    log_addf("Framebuffer: %ux%u @ %s",
        fb.width, fb.height,
        fb.format == VK_PIXEL_FORMAT_BGRX_8BPP ? "BGRX" : "RGBX");
    log_add("Press Alt to open the menu bar.");
    log_add("Tab / Arrows to navigate.  Enter / Space to activate.");
    log_add("Ctrl+Q to quit.");

    /* ================================================================
     * Main loop
     * ================================================================ */
    while (g_running) {

        /* --- 1. Drain keyboard events --- */
        {
            vk_key_event_t evt;
            while (vk_get_api()->vk_poll_key(&evt)) {
                ImGui_ImplVK_ProcessKey(&evt);

                /* Hard quit: Ctrl+Q (checked in raw events as well) */
                if (evt.pressed &&
                    (evt.modifiers & 2u) &&
                    (evt.ascii == 'q' || evt.ascii == 'Q'))
                {
                    g_running = false;
                }
            }
        }

        /* --- 2. Begin frame --- */
        ImGui_ImplVK_NewFrame();
        ImGui::NewFrame();

        /* --- 3. Global keyboard shortcuts (ImGui-side) --- */
        if (ImGui::GetIO().KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
                log_add("Ctrl+Q — quitting.");
                g_running = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
                g_counter = 0;
                log_add("Ctrl+N — counter reset.");
            }
        }

        /* --- 4. Build UI --- */
        draw_menu_bar();
        draw_info_window(&fb);
        draw_console_window();
        draw_settings_window();
        draw_about_modal();

        if (g_show_demo)
            ImGui::ShowDemoWindow(&g_show_demo);

        /* --- 5. Render --- */
        ImGui::Render();
        ImGui_ImplVK_RenderDrawData(ImGui::GetDrawData(), &fb);

        /* --- 6. Yield CPU slice back to the scheduler --- */
        vk_get_api()->vk_yield();
    }

    /* ---- Cleanup ---- */
    ImGui_ImplVK_Shutdown();
    ImGui::DestroyContext();

    VK_CALL(puts, "vgui: clean exit.\n");
    return 0;
}
