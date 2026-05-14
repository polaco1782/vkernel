/*
 * vgui/main.cpp
 * Dear ImGui demonstration for vkernel.
 */

#include "console_log.h"
#include "implot.h"
#include "kobj_panel.h"
#include "launch_registry.h"
#include "shell_ui.h"
#include "task_manager_panel.h"
#include "vkfm_panel.h"
#include "window_manager.h"

#include <unistd.h>

namespace {

auto framebuffer_available(const vk_framebuffer_info_t& framebuffer) -> bool
{
    return framebuffer.valid != 0 && framebuffer.base != 0 && framebuffer.width != 0 && framebuffer.height != 0;
}

} // namespace

int main(int /*argc*/, char** /*argv*/)
{
    bool drop_to_shell = false;

    vk_framebuffer_info_t framebuffer = {};
    VK_CALL(framebuffer_info, &framebuffer);
    if (!framebuffer_available(framebuffer)) {
        VK_CALL(puts, "vgui: no framebuffer available\n");
        return 1;
    }

    ImGui::CreateContext();
    ImPlot::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
    }

    if (!ImGui_ImplVK_Init(&framebuffer)) {
        VK_CALL(puts, "vgui: backend init failed\n");
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        return 1;
    }

    {
        vgui::ConsoleLog log;
        vgui::LaunchRegistry launch_registry;
        vgui::TaskManagerPanel task_manager;
        vgui::KobjNavigator kobj_navigator;
        vgui::VkfmPanel vkfm_panel;
        vgui::WindowManager window_manager(log);
        vgui::ShellUi ui;

        ui.initialize(framebuffer);

        /* Performance tuning for the software rasterizer. */
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 0.0f;
        style.PopupRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding = 0.0f;
        style.TabRounding = 0.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.AntiAliasedLines = false;
        style.AntiAliasedLinesUseTex = false;
        style.AntiAliasedFill = false;

        log.add("vGUI started.");
        log.addf("Framebuffer: %ux%u @ %s",
                 framebuffer.width,
                 framebuffer.height,
                 framebuffer.format == VK_PIXEL_FORMAT_BGRX_8BPP ? "BGRX" : "RGBX");
        log.add("Move the mouse to control the cursor.");
        log.add("Alt to open the menu bar. Tab/Arrows to navigate.");
        log.add("Enter/Space to activate. Ctrl+Q to quit.");
        log.add("Use Launch from the menu bar to start staged apps.");
        launch_registry.refresh(log);
        kobj_navigator.refresh_selected();

        if (vk_get_api()->vk_set_compositor_active) {
            (void)vk_get_api()->vk_set_compositor_active(1u);
        }

        while (ui.running()) {
            vk_key_event_t key_event;
            while (vk_get_api()->vk_poll_key(&key_event)) {
                if (!window_manager.route_key_event(key_event)) {
                    ImGui_ImplVK_ProcessKey(&key_event);
                }

                if (key_event.pressed != 0
                    && (key_event.modifiers & 2u) != 0
                    && (key_event.ascii == 'q' || key_event.ascii == 'Q')) {
                    ui.request_quit();
                }
            }

            vk_mouse_event_t mouse_event;
            while (vk_get_api()->vk_poll_mouse(&mouse_event)) {
                ImGui_ImplVK_ProcessMouse(&mouse_event);
                (void)window_manager.route_mouse_event(mouse_event);
            }

            ImGui_ImplVK_NewFrame();
            ImGui::NewFrame();

            if (ImGui::GetIO().KeyCtrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
                    ui.request_quit(&log, "Ctrl+Q - quitting.");
                }
                if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
                    ui.reset_counter(&log, "Ctrl+N - counter reset.");
                }
            }

            ui.draw(framebuffer,
                    launch_registry,
                    window_manager,
                    log,
                    task_manager,
                    kobj_navigator,
                    vkfm_panel);

            ImGui::Render();
            ImGui_ImplVK_RenderDrawData(ImGui::GetDrawData(), &framebuffer);
            vk_get_api()->vk_sleep(1);
        }

        drop_to_shell = ui.drop_to_shell_requested();
        window_manager.shutdown();
    }

    ImGui_ImplVK_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    if (vk_get_api()->vk_set_compositor_active) {
        (void)vk_get_api()->vk_set_compositor_active(0u);
    }
    if (vk_get_api()->vk_set_compositor_default_fb) {
        (void)vk_get_api()->vk_set_compositor_default_fb(nullptr);
    }

    if (drop_to_shell) {
        char* const argv[] = {
            const_cast<char*>("shell.vbin"),
            nullptr,
        };
        (void)execve("shell.vbin", argv, nullptr);
        VK_CALL(puts, "vgui: failed to exec shell.vbin\n");
        return 1;
    }

    VK_CALL(puts, "vgui: clean exit.\n");

    return 0;
}
