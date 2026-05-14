#include "frontend.h"

#include "../vgui/imgui/imgui.h"
#include "../vgui/imgui_impl_vk.h"

#include <iostream>

namespace vnes_frontend {

namespace {

void apply_style()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.10f, 0.13f, 0.16f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.95f, 0.93f, 0.89f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.92f, 0.90f, 0.85f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.96f, 0.94f, 0.90f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.42f, 0.49f, 0.45f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.88f, 0.86f, 0.81f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.72f, 0.79f, 0.75f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.62f, 0.72f, 0.68f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.18f, 0.28f, 0.28f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.21f, 0.34f, 0.33f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.86f, 0.83f, 0.78f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.24f, 0.42f, 0.40f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.50f, 0.47f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.33f, 0.31f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.60f, 0.72f, 0.66f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.49f, 0.63f, 0.58f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.38f, 0.53f, 0.49f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.18f, 0.36f, 0.34f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.42f, 0.40f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.18f, 0.33f, 0.31f, 1.0f);
}

} // namespace

} // namespace vnes_frontend

int main(int argc, char** argv)
{
    vnes_frontend::AppState app;
    if (!vnes_frontend::init_framebuffer(&app)) {
        std::cout << "vnes: no framebuffer available\n";
        return 1;
    }

    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
    }

    if (!ImGui_ImplVK_Init(&app.framebuffer)) {
        std::cout << "vnes: backend init failed\n";
        ImGui::DestroyContext();
        return 1;
    }

    ImGui_ImplVK_SetTransparencyEnabled(false);
    ImGui_ImplVK_SetClearColor(236U, 230U, 218U);
    vnes_frontend::apply_style();

    if (argc > 1) {
        if (!vnes_frontend::load_rom(&app, argv[1])) {
            vnes_frontend::browser_open(&app);
        }
    } else {
        vnes_frontend::browser_open(&app);
    }

    while (!app.quit_requested) {
        const bool want_text_input = ImGui::GetIO().WantTextInput;

        vk_key_event_t key_event = {};
        while (vk_get_api()->vk_poll_key(&key_event)) {
            ImGui_ImplVK_ProcessKey(&key_event);
            vnes_frontend::handle_key_event(&app, key_event, want_text_input);
        }

        vk_mouse_event_t mouse_event = {};
        while (vk_get_api()->vk_poll_mouse(&mouse_event)) {
            ImGui_ImplVK_ProcessMouse(&mouse_event);
        }

        vnes_frontend::advance_emulation(&app);

        ImGui_ImplVK_NewFrame();
        ImGui::NewFrame();
        vnes_frontend::draw_ui(&app);
        ImGui::Render();
        ImGui_ImplVK_RenderDrawData(ImGui::GetDrawData(), &app.framebuffer);
        vnes_frontend::idle_until_next_work(&app);
    }

    vnes_frontend::shutdown_app(&app);
    ImGui_ImplVK_Shutdown();
    ImGui::DestroyContext();

    std::cout << "vnes: clean exit\n";
    return 0;
}