#include "shell_ui.h"

#include "console_log.h"
#include "kobj_panel.h"
#include "launch_registry.h"
#include "task_manager_panel.h"
#include "vkfm_panel.h"
#include "window_manager.h"

#include <stdio.h>

namespace vgui {

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
    switch (style_index_) {
    case 1:
        ImGui::StyleColorsLight();
        break;
    case 2:
        ImGui::StyleColorsClassic();
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

    if (!ImGui::Begin("Info Panel", &show_info_)) {
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

    if (!ImGui::Begin("Settings", &show_settings_, ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }
    window_manager.clear_focus_if_host_window_focused();

    ImGui::SeparatorText("Appearance");

    const char* style_names[] = { "Dark", "Light", "Classic" };
    if (ImGui::Combo("Color scheme", &style_index_, style_names, 3)) {
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