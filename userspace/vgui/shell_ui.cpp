#include "shell_ui.h"

#include "console_log.h"
#include "kobj_panel.h"
#include "launch_registry.h"
#include "task_manager_panel.h"
#include "vkfm_panel.h"
#include "window_manager.h"

#include <stdio.h>

namespace vgui {

namespace {

void apply_scheme_ocean()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.08f, 0.12f, colors[ImGuiCol_WindowBg].w);
    colors[ImGuiCol_ChildBg] = ImVec4(0.04f, 0.07f, 0.10f, colors[ImGuiCol_ChildBg].w);
    colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.09f, 0.14f, colors[ImGuiCol_PopupBg].w);
    colors[ImGuiCol_TitleBg] = ImVec4(0.03f, 0.20f, 0.26f, colors[ImGuiCol_TitleBg].w);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.04f, 0.28f, 0.35f, colors[ImGuiCol_TitleBgActive].w);
    colors[ImGuiCol_Header] = ImVec4(0.08f, 0.33f, 0.46f, colors[ImGuiCol_Header].w);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.12f, 0.41f, 0.56f, colors[ImGuiCol_HeaderHovered].w);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.14f, 0.46f, 0.62f, colors[ImGuiCol_HeaderActive].w);
    colors[ImGuiCol_Button] = ImVec4(0.07f, 0.36f, 0.50f, colors[ImGuiCol_Button].w);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.11f, 0.45f, 0.61f, colors[ImGuiCol_ButtonHovered].w);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.50f, 0.68f, colors[ImGuiCol_ButtonActive].w);
    colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.15f, 0.22f, colors[ImGuiCol_FrameBg].w);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.23f, 0.32f, colors[ImGuiCol_FrameBgHovered].w);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.13f, 0.27f, 0.38f, colors[ImGuiCol_FrameBgActive].w);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.64f, 0.82f, colors[ImGuiCol_SliderGrab].w);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.74f, 0.92f, colors[ImGuiCol_SliderGrabActive].w);
    colors[ImGuiCol_CheckMark] = ImVec4(0.36f, 0.80f, 0.94f, colors[ImGuiCol_CheckMark].w);
    colors[ImGuiCol_Separator] = ImVec4(0.18f, 0.34f, 0.44f, colors[ImGuiCol_Separator].w);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.22f, 0.56f, 0.72f, colors[ImGuiCol_ResizeGrip].w);
    colors[ImGuiCol_Tab] = ImVec4(0.07f, 0.24f, 0.34f, colors[ImGuiCol_Tab].w);
    colors[ImGuiCol_TabActive] = ImVec4(0.10f, 0.35f, 0.49f, colors[ImGuiCol_TabActive].w);
}

void apply_scheme_forest()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.10f, 0.08f, colors[ImGuiCol_WindowBg].w);
    colors[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.08f, 0.06f, colors[ImGuiCol_ChildBg].w);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.12f, 0.09f, colors[ImGuiCol_PopupBg].w);
    colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.22f, 0.15f, colors[ImGuiCol_TitleBg].w);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.30f, 0.19f, colors[ImGuiCol_TitleBgActive].w);
    colors[ImGuiCol_Header] = ImVec4(0.17f, 0.32f, 0.20f, colors[ImGuiCol_Header].w);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.40f, 0.25f, colors[ImGuiCol_HeaderHovered].w);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.45f, 0.27f, colors[ImGuiCol_HeaderActive].w);
    colors[ImGuiCol_Button] = ImVec4(0.16f, 0.36f, 0.22f, colors[ImGuiCol_Button].w);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.45f, 0.27f, colors[ImGuiCol_ButtonHovered].w);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.52f, 0.31f, colors[ImGuiCol_ButtonActive].w);
    colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.17f, 0.12f, colors[ImGuiCol_FrameBg].w);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.25f, 0.17f, colors[ImGuiCol_FrameBgHovered].w);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.29f, 0.20f, colors[ImGuiCol_FrameBgActive].w);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.71f, 0.35f, colors[ImGuiCol_SliderGrab].w);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.80f, 0.44f, colors[ImGuiCol_SliderGrabActive].w);
    colors[ImGuiCol_CheckMark] = ImVec4(0.63f, 0.86f, 0.45f, colors[ImGuiCol_CheckMark].w);
    colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.35f, 0.25f, colors[ImGuiCol_Separator].w);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.33f, 0.55f, 0.36f, colors[ImGuiCol_ResizeGrip].w);
    colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.21f, 0.14f, colors[ImGuiCol_Tab].w);
    colors[ImGuiCol_TabActive] = ImVec4(0.21f, 0.36f, 0.24f, colors[ImGuiCol_TabActive].w);
}

void apply_scheme_sunset()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.14f, 0.09f, 0.10f, colors[ImGuiCol_WindowBg].w);
    colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.07f, 0.08f, colors[ImGuiCol_ChildBg].w);
    colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.10f, 0.11f, colors[ImGuiCol_PopupBg].w);
    colors[ImGuiCol_TitleBg] = ImVec4(0.31f, 0.15f, 0.12f, colors[ImGuiCol_TitleBg].w);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.40f, 0.20f, 0.16f, colors[ImGuiCol_TitleBgActive].w);
    colors[ImGuiCol_Header] = ImVec4(0.45f, 0.24f, 0.17f, colors[ImGuiCol_Header].w);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.53f, 0.30f, 0.20f, colors[ImGuiCol_HeaderHovered].w);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.59f, 0.35f, 0.22f, colors[ImGuiCol_HeaderActive].w);
    colors[ImGuiCol_Button] = ImVec4(0.47f, 0.24f, 0.15f, colors[ImGuiCol_Button].w);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.56f, 0.30f, 0.18f, colors[ImGuiCol_ButtonHovered].w);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.64f, 0.35f, 0.20f, colors[ImGuiCol_ButtonActive].w);
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.12f, 0.11f, colors[ImGuiCol_FrameBg].w);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.29f, 0.17f, 0.14f, colors[ImGuiCol_FrameBgHovered].w);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.35f, 0.20f, 0.16f, colors[ImGuiCol_FrameBgActive].w);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.88f, 0.52f, 0.23f, colors[ImGuiCol_SliderGrab].w);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.97f, 0.61f, 0.28f, colors[ImGuiCol_SliderGrabActive].w);
    colors[ImGuiCol_CheckMark] = ImVec4(0.99f, 0.73f, 0.35f, colors[ImGuiCol_CheckMark].w);
    colors[ImGuiCol_Separator] = ImVec4(0.41f, 0.24f, 0.17f, colors[ImGuiCol_Separator].w);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.67f, 0.37f, 0.21f, colors[ImGuiCol_ResizeGrip].w);
    colors[ImGuiCol_Tab] = ImVec4(0.24f, 0.13f, 0.11f, colors[ImGuiCol_Tab].w);
    colors[ImGuiCol_TabActive] = ImVec4(0.40f, 0.22f, 0.15f, colors[ImGuiCol_TabActive].w);
}

void apply_scheme_win9x()
{
    ImGui::StyleColorsClassic();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    const auto win9x_rgb = [](int r, int g, int b, float a) {
        /* Our software path currently interprets ImGui vertex channels as BGR. */
        return ImVec4((float)b / 255.0f, (float)g / 255.0f, (float)r / 255.0f, a);
    };

    colors[ImGuiCol_Text] = win9x_rgb(0, 0, 0, colors[ImGuiCol_Text].w);
    colors[ImGuiCol_TextDisabled] = win9x_rgb(90, 90, 90, colors[ImGuiCol_TextDisabled].w);

    /* Windows and controls stay classic gray (#C0C0C0). */
    colors[ImGuiCol_WindowBg] = win9x_rgb(192, 192, 192, colors[ImGuiCol_WindowBg].w);
    colors[ImGuiCol_ChildBg] = win9x_rgb(192, 192, 192, colors[ImGuiCol_ChildBg].w);
    colors[ImGuiCol_PopupBg] = win9x_rgb(192, 192, 192, colors[ImGuiCol_PopupBg].w);

    /* 3D cue: white highlight and dark gray shadow. */
    colors[ImGuiCol_Border] = win9x_rgb(79, 79, 79, colors[ImGuiCol_Border].w);
    colors[ImGuiCol_BorderShadow] = win9x_rgb(255, 255, 255, colors[ImGuiCol_BorderShadow].w);

    colors[ImGuiCol_FrameBg] = win9x_rgb(192, 192, 192, colors[ImGuiCol_FrameBg].w);
    colors[ImGuiCol_FrameBgHovered] = win9x_rgb(220, 220, 220, colors[ImGuiCol_FrameBgHovered].w);
    colors[ImGuiCol_FrameBgActive] = win9x_rgb(168, 168, 168, colors[ImGuiCol_FrameBgActive].w);

    colors[ImGuiCol_TitleBg] = win9x_rgb(64, 64, 160, colors[ImGuiCol_TitleBg].w);
    colors[ImGuiCol_TitleBgActive] = win9x_rgb(0, 0, 128, colors[ImGuiCol_TitleBgActive].w);
    colors[ImGuiCol_MenuBarBg] = win9x_rgb(192, 192, 192, colors[ImGuiCol_MenuBarBg].w);

    colors[ImGuiCol_ScrollbarBg] = win9x_rgb(192, 192, 192, colors[ImGuiCol_ScrollbarBg].w);
    colors[ImGuiCol_ScrollbarGrab] = win9x_rgb(174, 174, 174, colors[ImGuiCol_ScrollbarGrab].w);
    colors[ImGuiCol_ScrollbarGrabHovered] = win9x_rgb(148, 148, 148, colors[ImGuiCol_ScrollbarGrabHovered].w);
    colors[ImGuiCol_ScrollbarGrabActive] = win9x_rgb(115, 115, 115, colors[ImGuiCol_ScrollbarGrabActive].w);

    colors[ImGuiCol_CheckMark] = win9x_rgb(0, 0, 0, colors[ImGuiCol_CheckMark].w);
    colors[ImGuiCol_SliderGrab] = win9x_rgb(160, 160, 160, colors[ImGuiCol_SliderGrab].w);
    colors[ImGuiCol_SliderGrabActive] = win9x_rgb(115, 115, 115, colors[ImGuiCol_SliderGrabActive].w);

    colors[ImGuiCol_Button] = win9x_rgb(192, 192, 192, colors[ImGuiCol_Button].w);
    colors[ImGuiCol_ButtonHovered] = win9x_rgb(220, 220, 220, colors[ImGuiCol_ButtonHovered].w);
    colors[ImGuiCol_ButtonActive] = win9x_rgb(168, 168, 168, colors[ImGuiCol_ButtonActive].w);

    colors[ImGuiCol_Header] = win9x_rgb(198, 210, 234, colors[ImGuiCol_Header].w);
    colors[ImGuiCol_HeaderHovered] = win9x_rgb(172, 194, 230, colors[ImGuiCol_HeaderHovered].w);
    colors[ImGuiCol_HeaderActive] = win9x_rgb(142, 173, 223, colors[ImGuiCol_HeaderActive].w);
    colors[ImGuiCol_Separator] = win9x_rgb(96, 96, 96, colors[ImGuiCol_Separator].w);
    colors[ImGuiCol_ResizeGrip] = win9x_rgb(160, 160, 160, colors[ImGuiCol_ResizeGrip].w);
    colors[ImGuiCol_ResizeGripHovered] = win9x_rgb(128, 128, 128, colors[ImGuiCol_ResizeGripHovered].w);
    colors[ImGuiCol_ResizeGripActive] = win9x_rgb(90, 90, 90, colors[ImGuiCol_ResizeGripActive].w);
    colors[ImGuiCol_Tab] = win9x_rgb(192, 192, 192, colors[ImGuiCol_Tab].w);
    colors[ImGuiCol_TabHovered] = win9x_rgb(220, 220, 220, colors[ImGuiCol_TabHovered].w);
    colors[ImGuiCol_TabActive] = win9x_rgb(168, 168, 168, colors[ImGuiCol_TabActive].w);

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.FramePadding = ImVec2(5.0f, 3.0f);
}

}

void ShellUi::initialize(const vk_framebuffer_info_t& framebuffer)
{
    default_app_width_ = framebuffer.width / 2;
    default_app_height_ = framebuffer.height / 2;
    if (default_app_width_ < 320) {
        default_app_width_ = 320;
    }
    if (default_app_height_ < 200) {
        default_app_height_ = 200;
    }

    ImGui_ImplVK_SetTransparencyEnabled(transparency_);
    apply_style();
    ImGui::GetIO().FontGlobalScale = font_scale_;
}

void ShellUi::request_quit(ConsoleLog* log, vk::string_view message)
{
    if (log != nullptr && !message.empty()) {
        log->add(message);
    }
    running_ = false;
}

void ShellUi::reset_counter(ConsoleLog* log, vk::string_view message)
{
    counter_ = 0;
    if (log != nullptr && !message.empty()) {
        log->add(message);
    }
}

void ShellUi::apply_style()
{
    /* Default desktop clear used by non-Win9x themes. */
    ImGui_ImplVK_SetClearColor(22, 22, 30);

    switch (style_index_) {
    case 1:
        ImGui::StyleColorsLight();
        break;
    case 2:
        ImGui::StyleColorsClassic();
        break;
    case 3:
        apply_scheme_ocean();
        break;
    case 4:
        apply_scheme_forest();
        break;
    case 5:
        apply_scheme_sunset();
        break;
    case 6:
        apply_scheme_win9x();
        /* #018281 desktop, pre-swizzled for current BGR vertex path. */
        ImGui_ImplVK_SetClearColor(129, 130, 1);
        break;
    default:
        ImGui::StyleColorsDark();
        break;
    }

    if (!transparency_) {
        ImGuiStyle& style = ImGui::GetStyle();
        for (int index = 0; index < ImGuiCol_COUNT; ++index) {
            style.Colors[index].w = 1.0f;
        }
    }
}

void ShellUi::draw_menu_bar(LaunchRegistry& launch_registry, WindowManager& window_manager, ConsoleLog& log)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) {
            reset_counter(&log, "File > New: counter reset.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
            request_quit(&log, "Quit requested via menu.");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Increment Counter", "+")) {
            ++counter_;
            log.addf("Counter incremented to %d.", counter_);
        }
        if (ImGui::MenuItem("Decrement Counter", "-")) {
            --counter_;
            log.addf("Counter decremented to %d.", counter_);
        }
        if (ImGui::MenuItem("Reset Counter")) {
            reset_counter(&log, "Counter reset.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...")) {
            show_settings_ = true;
            log.add("Opened Settings.");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Launch")) {
        if (ImGui::MenuItem("Refresh App List")) {
            launch_registry.refresh(log);
        }

        ImGui::Separator();
        if (launch_registry.empty()) {
            ImGui::BeginDisabled();
            ImGui::MenuItem("No staged apps found", nullptr, false, false);
            ImGui::EndDisabled();
        } else {
            for (int index = 0; index < launch_registry.size(); ++index) {
                const LaunchMenuEntry& entry = launch_registry.entry(index);
                if (ImGui::MenuItem(entry.label.c_str())) {
                    (void)window_manager.launch_windowed_app(string_view_of(entry.path),
                                                             default_app_width_,
                                                             default_app_height_);
                }
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Info Panel", nullptr, &show_info_);
        ImGui::MenuItem("Console", nullptr, &show_console_);
        ImGui::MenuItem("Task Manager", nullptr, &show_task_manager_);
        ImGui::MenuItem("KObj Navigator", nullptr, &show_kobj_);
        ImGui::MenuItem("vkfm", nullptr, &show_vkfm_);
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &show_demo_);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About vGUI...")) {
            open_about_ = true;
        }
        ImGui::EndMenu();
    }

    ImGuiIO& io = ImGui::GetIO();
    std::array<char, 32> fps_buffer {};
    const unsigned fps_times_ten = static_cast<unsigned>(io.Framerate * 10.0f + 0.5f);
    snprintf(fps_buffer.data(), fps_buffer.size(), "%u.%u FPS", fps_times_ten / 10, fps_times_ten % 10);
    const float width = ImGui::CalcTextSize(fps_buffer.data()).x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - width);
    ImGui::TextDisabled("%s", fps_buffer.data());

    ImGui::EndMainMenuBar();
}

void ShellUi::draw_info_window(const vk_framebuffer_info_t& framebuffer,
                               WindowManager& window_manager,
                               ConsoleLog& log)
{
    if (!show_info_) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(10.0f, 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 290.0f), ImGuiCond_FirstUseEver);

    if (!imgui_begin_window_readable_caption("Info Panel", &show_info_)) {
        ImGui::End();
        return;
    }
    window_manager.clear_focus_if_host_window_focused();

    ImGui::SeparatorText("Framebuffer");
    ImGui::Text("Resolution  : %u x %u", framebuffer.width, framebuffer.height);
    ImGui::Text("Stride      : %u px", framebuffer.stride);
    ImGui::Text("Format      : %s",
                framebuffer.format == VK_PIXEL_FORMAT_BGRX_8BPP ? "BGRX-8bpp"
              : framebuffer.format == VK_PIXEL_FORMAT_RGBX_8BPP ? "RGBX-8bpp"
              : framebuffer.format == VK_PIXEL_FORMAT_BITMASK ? "Bitmask"
                                                               : "BLT-only");
    ImGui::Text("Base        : 0x%llx", static_cast<unsigned long long>(framebuffer.base));

    ImGui::SeparatorText("Timing");
    ImGuiIO& io = ImGui::GetIO();
    const unsigned frame_ms_times_100 = static_cast<unsigned>(io.DeltaTime * 100000.0f + 0.5f);
    const unsigned fps_times_ten = static_cast<unsigned>(io.Framerate * 10.0f + 0.5f);
    ImGui::Text("Frame time  : %u.%02u ms", frame_ms_times_100 / 100, frame_ms_times_100 % 100);
    ImGui::Text("FPS         : %u.%u", fps_times_ten / 10, fps_times_ten % 10);

    const vk_api_t* api = vk_get_api();
    const vk_u64 tick = api->vk_tick_count();
    const vk_u32 ticks_per_second = api->vk_ticks_per_sec();
    const vk_u64 seconds = ticks_per_second != 0 ? tick / ticks_per_second : 0;
    ImGui::Text("Uptime      : %llus", static_cast<unsigned long long>(seconds));

    ImGui::SeparatorText("Counter");
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##counter_val", &counter_, 0, 0);

    ImGui::SameLine();
    if (ImGui::Button(" + ")) {
        if (counter_wrap_ && counter_ >= counter_max_) {
            counter_ = 0;
        } else {
            ++counter_;
        }
        log.addf("Counter -> %d", counter_);
    }
    ImGui::SameLine();
    if (ImGui::Button(" - ")) {
        --counter_;
        log.addf("Counter -> %d", counter_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        reset_counter(&log, "Counter reset.");
    }

    const int display_max = counter_max_ > 0 ? counter_max_ : 1;
    float progress = static_cast<float>(counter_) / static_cast<float>(display_max);
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }
    ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));

    ImGui::Checkbox("Wrap at max", &counter_wrap_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("Max", &counter_max_, 1, 10);

    ImGui::End();
}

void ShellUi::draw_settings_window(WindowManager& window_manager, ConsoleLog& log)
{
    if (!show_settings_) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(200.0f, 150.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(270.0f, 170.0f), ImGuiCond_FirstUseEver);

    if (!imgui_begin_window_readable_caption("Settings", &show_settings_, ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }
    window_manager.clear_focus_if_host_window_focused();

    ImGui::SeparatorText("Appearance");

    const char* style_names[] = { "Dark", "Light", "Classic", "Ocean", "Forest", "Sunset", "Win9x" };
    const int style_name_count = static_cast<int>(sizeof(style_names) / sizeof(style_names[0]));
    if (ImGui::Combo("Color scheme", &style_index_, style_names, style_name_count)) {
        apply_style();
        log.addf("Style changed to %s.", style_names[style_index_]);
    }

    if (ImGui::SliderFloat("Font scale", &font_scale_, 0.5f, 2.0f, "%.1f")) {
        ImGui::GetIO().FontGlobalScale = font_scale_;
    }

    ImGui::SeparatorText("Renderer");
    if (ImGui::Checkbox("Transparency (slow)", &transparency_)) {
        ImGui_ImplVK_SetTransparencyEnabled(transparency_);
        apply_style();
        log.addf("Transparency %s.", transparency_ ? "ON" : "OFF");
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
        show_settings_ = false;
    }

    ImGui::End();
}

void ShellUi::draw_about_modal()
{
    if (open_about_) {
        ImGui::OpenPopup("About vGUI");
        open_about_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(320.0f, 160.0f), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("About vGUI", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("vGUI - Dear ImGui GUI for vkernel");
        ImGui::Spacing();
        ImGui::TextWrapped("Software renderer: barycentric triangle fill,");
        ImGui::TextWrapped("alpha-8 font atlas, framebuffer blending.");
        ImGui::Spacing();
        ImGui::TextWrapped("Keyboard-only navigation, newlib C runtime.");
        ImGui::Spacing();
        ImGui::Separator();

        const float button_width = 100.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - button_width) * 0.5f);
        if (ImGui::Button("OK", ImVec2(button_width, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void ShellUi::draw(const vk_framebuffer_info_t& framebuffer,
                   LaunchRegistry& launch_registry,
                   WindowManager& window_manager,
                   ConsoleLog& log,
                   TaskManagerPanel& task_manager,
                   KobjNavigator& kobj_navigator,
                   VkfmPanel& vkfm_panel)
{
    draw_menu_bar(launch_registry, window_manager, log);
    draw_info_window(framebuffer, window_manager, log);
    log.draw_window(show_console_, window_manager);
    task_manager.draw_window(show_task_manager_, window_manager);
    kobj_navigator.draw_window(show_kobj_, window_manager);
    vkfm_panel.draw_window(show_vkfm_, window_manager, log);
    window_manager.draw_windows();
    draw_settings_window(window_manager, log);
    draw_about_modal();

    if (show_demo_) {
        ImGui::ShowDemoWindow(&show_demo_);
    }
}

} // namespace vgui