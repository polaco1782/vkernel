#include "frontend.h"

#include "disasm.h"
#include "util.h"

#include "../vgui/imgui/imgui.h"
#include "../vgui/imgui_impl_vk.h"

namespace vnes_frontend {

namespace {

using vnes::disasm::nesPalette;
using vnes::util::formatFlags;
using vnes::util::hexByte;
using vnes::util::hexWord;

auto mirroring_name(Mirroring mirroring) -> const char*
{
    switch (mirroring) {
        case Mirroring::HORIZONTAL: return "Horizontal";
        case Mirroring::VERTICAL: return "Vertical";
        case Mirroring::FOUR_SCREEN: return "Four-screen";
        case Mirroring::SINGLE_LOWER: return "Single lower";
        case Mirroring::SINGLE_UPPER: return "Single upper";
        default: return "Unknown";
    }
}

auto color_from_rgb(u32 rgb) -> ImVec4
{
    const float r = static_cast<float>((rgb >> 16) & 0xFFU) / 255.0f;
    const float g = static_cast<float>((rgb >> 8) & 0xFFU) / 255.0f;
    const float b = static_cast<float>(rgb & 0xFFU) / 255.0f;
    return ImVec4(r, g, b, 1.0f);
}

auto draw_menu_bar(AppState* app) -> bool
{
    bool state_changed = false;

    if (!ImGui::BeginMenuBar()) {
        return false;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open ROM", "Ctrl+O")) {
            browser_open(app);
        }
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
            app->quit_requested = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Emulation")) {
        if (ImGui::MenuItem(app->paused ? "Resume" : "Pause", "Ctrl+P", false, app->rom_loaded)) {
            app->paused = !app->paused;
            set_status(app, app->paused ? "Paused." : "Resumed.");
            state_changed = true;
        }
        if (ImGui::MenuItem("Step Frame", nullptr, false, app->rom_loaded)) {
            app->step_requested = true;
            app->paused = true;
            state_changed = true;
        }
        if (ImGui::MenuItem("Reset", "Ctrl+R", false, app->rom_loaded)) {
            reset_emulator(app);
            state_changed = true;
        }
        if (ImGui::MenuItem(app->muted ? "Unmute Audio" : "Mute Audio", "Ctrl+M")) {
            app->muted = !app->muted;
            set_status(app, app->muted ? "Audio muted." : "Audio unmuted.");
            state_changed = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(app->use_hq2x ? "Disable HQ2x" : "Enable HQ2x")) {
            app->use_hq2x = !app->use_hq2x;
            set_status(app, app->use_hq2x ? "HQ2x enabled." : "HQ2x disabled.");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Windows")) {
        ImGui::MenuItem("CPU", nullptr, &app->show_cpu_window);
        ImGui::MenuItem("PPU", nullptr, &app->show_ppu_window);
        ImGui::MenuItem("APU", nullptr, &app->show_apu_window);
        ImGui::MenuItem("Cartridge", nullptr, &app->show_cartridge_window);
        ImGui::MenuItem("Palette", nullptr, &app->show_palette_window);
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
    return state_changed;
}

void draw_sidebar(AppState* app, bool* state_changed)
{
    ImGui::TextUnformatted(app->rom_loaded ? app->rom_title.c_str() : "No ROM Loaded");
    ImGui::Separator();

    if (ImGui::Button("Open ROM", ImVec2(-1.0f, 0.0f))) {
        browser_open(app);
    }

    if (ImGui::Button(app->paused ? "Resume" : "Pause", ImVec2(-1.0f, 0.0f)) && app->rom_loaded) {
        app->paused = !app->paused;
        set_status(app, app->paused ? "Paused." : "Resumed.");
        *state_changed = true;
    }

    if (ImGui::Button("Step Frame", ImVec2(-1.0f, 0.0f)) && app->rom_loaded) {
        app->paused = true;
        app->step_requested = true;
        *state_changed = true;
    }

    if (ImGui::Button("Reset", ImVec2(-1.0f, 0.0f)) && app->rom_loaded) {
        reset_emulator(app);
        *state_changed = true;
    }

    if (ImGui::Checkbox("Mute Audio", &app->muted)) {
        set_status(app, app->muted ? "Audio muted." : "Audio unmuted.");
        *state_changed = true;
    }
    if (ImGui::Checkbox("HQ2x", &app->use_hq2x)) {
        set_status(app, app->use_hq2x ? "HQ2x enabled." : "HQ2x disabled.");
    }

    ImGui::Separator();
    ImGui::Text("FPS: %.1f", app->stats.fps);
    ImGui::Text("Scanline: %d", app->bus.ppu.getScanline());
    ImGui::Text("Cycle: %d", app->bus.ppu.getCycle());
    ImGui::Text("Mapper: %s", app->bus.cartridge.getMapperName());
    ImGui::Text("Mirroring: %s", mirroring_name(app->bus.cartridge.getMirroring()));

    if (app->rom_loaded) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", app->loaded_rom_path.c_str());
    }

    if (!app->status_message.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", app->status_message.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Controls");
    ImGui::TextWrapped("Arrows move. Z/A = B, X/S = A, Enter = Start, Space = Select.");
    ImGui::TextWrapped("Ctrl+O open ROM, Ctrl+P pause, Ctrl+R reset, Ctrl+M mute, Ctrl+Q quit.");
}

void draw_display(AppState* app)
{
    if (!app->rom_loaded) {
        ImGui::Spacing();
        ImGui::TextWrapped("Load a .nes ROM to start the emulator.");
        return;
    }

    const u32* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 stride = 0;
    get_display_frame(app,
                      app->use_hq2x,
                      &app->hq2x,
                      &app->hq2x_output_buffer,
                      &pixels,
                      &width,
                      &height,
                      &stride);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    float scale = available.x / static_cast<float>(width);
    const float scale_y = available.y / static_cast<float>(height);
    if (scale_y < scale) {
        scale = scale_y;
    }
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    const float draw_width = static_cast<float>(width) * scale;
    const float draw_height = static_cast<float>(height) * scale;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available.x - draw_width) * 0.5f);
    ImGui::InvisibleButton("##nes_viewport", ImVec2(draw_width, draw_height));

    const ImVec2 p_min = ImGui::GetItemRectMin();
    const ImVec2 p_max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(p_min, p_max, IM_COL32(16, 18, 24, 255));

    ImGui_ImplVK_FramebufferImage image = {};
    image.pixels = pixels;
    image.width = width;
    image.height = height;
    image.stride = stride;
    image.format = VK_PIXEL_FORMAT_BGRX_8BPP;
    image.p_min = p_min;
    image.p_max = p_max;
    ImGui_ImplVK_AddFramebufferImage(draw_list, &image);
    draw_list->AddRect(p_min, p_max, IM_COL32(44, 67, 64, 255));

    if (app->paused) {
        draw_list->AddText(ImVec2(p_min.x + 12.0f, p_min.y + 12.0f), IM_COL32(255, 242, 214, 255), "PAUSED");
    }
}

void draw_cpu_window(AppState* app)
{
    if (!app->show_cpu_window) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(18.0f, 96.0f), ImGuiCond_Once);
    if (!ImGui::Begin("CPU", &app->show_cpu_window)) {
        ImGui::End();
        return;
    }

    const CPU& cpu = app->bus.cpu;
    ImGui::Text("PC: $%s", hexWord(cpu.getPC()).c_str());
    ImGui::Text("SP: $%s", hexByte(cpu.getSP()).c_str());
    ImGui::Text("A:  $%s", hexByte(cpu.getA()).c_str());
    ImGui::Text("X:  $%s", hexByte(cpu.getX()).c_str());
    ImGui::Text("Y:  $%s", hexByte(cpu.getY()).c_str());
    ImGui::Text("P:  %s", formatFlags(cpu.getStatus()).c_str());
    ImGui::Text("Cycles: %llu", static_cast<unsigned long long>(cpu.getCycles()));
    ImGui::End();
}

void draw_ppu_window(AppState* app)
{
    if (!app->show_ppu_window) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(18.0f, 340.0f), ImGuiCond_Once);
    if (!ImGui::Begin("PPU", &app->show_ppu_window)) {
        ImGui::End();
        return;
    }

    const PPU& ppu = app->bus.ppu;
    ImGui::Text("Scanline: %d", ppu.getScanline());
    ImGui::Text("Cycle: %d", ppu.getCycle());
    ImGui::Separator();
    ImGui::Text("CTRL:   $%s", hexByte(ppu.getCtrl()).c_str());
    ImGui::Text("MASK:   $%s", hexByte(ppu.getMask()).c_str());
    ImGui::Text("STATUS: $%s", hexByte(ppu.getStatus()).c_str());
    ImGui::Text("OAM:    $%s", hexByte(ppu.getOamAddr()).c_str());
    ImGui::Text("VRAM:   $%s", hexWord(ppu.getVramAddr()).c_str());
    ImGui::Text("TEMP:   $%s", hexWord(ppu.getTempAddr()).c_str());
    ImGui::Text("Fine X: %u", static_cast<unsigned>(ppu.getFineX()));
    ImGui::Text("Toggle: %s", ppu.getWriteToggle() ? "second" : "first");
    ImGui::End();
}

void draw_channel_status(const char* label, const APU::ChannelStatus& status)
{
    ImGui::Text("%s", label);
    ImGui::Text("  Enabled: %s", status.enabled ? "yes" : "no");
    ImGui::Text("  Volume: %u", static_cast<unsigned>(status.volume));
    ImGui::Text("  Period: %u", static_cast<unsigned>(status.period));
    ImGui::Text("  Length: %u", static_cast<unsigned>(status.length));
}

void draw_apu_window(AppState* app)
{
    if (!app->show_apu_window) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(930.0f, 96.0f), ImGuiCond_Once);
    if (!ImGui::Begin("APU", &app->show_apu_window)) {
        ImGui::End();
        return;
    }

    const APU& apu = app->bus.apu;
    ImGui::Text("Muted: %s", apu.isMuted() ? "yes" : "no");
    ImGui::Text("Frame Counter Mode: %u", static_cast<unsigned>(apu.getFrameCounterMode()));
    ImGui::Text("IRQ Inhibit: %s", apu.getIrqInhibit() ? "yes" : "no");
    ImGui::Separator();
    draw_channel_status("Pulse 1", apu.getPulse1Status());
    ImGui::Separator();
    draw_channel_status("Pulse 2", apu.getPulse2Status());
    ImGui::Separator();
    draw_channel_status("Triangle", apu.getTriangleStatus());
    ImGui::Separator();
    draw_channel_status("Noise", apu.getNoiseStatus());
    ImGui::Separator();
    draw_channel_status("DMC", apu.getDMCStatus());
    ImGui::End();
}

void draw_cartridge_window(AppState* app)
{
    if (!app->show_cartridge_window) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(930.0f, 380.0f), ImGuiCond_Once);
    if (!ImGui::Begin("Cartridge", &app->show_cartridge_window)) {
        ImGui::End();
        return;
    }

    const Cartridge& cartridge = app->bus.cartridge;
    ImGui::Text("Mapper: %u (%s)", static_cast<unsigned>(cartridge.getMapperNumber()), cartridge.getMapperName());
    ImGui::Text("PRG ROM: %zu KB", cartridge.getPrgRom().size() / 1024U);
    ImGui::Text("CHR ROM: %zu KB", cartridge.getChrRom().size() / 1024U);
    ImGui::Text("Battery: %s", cartridge.hasBattery() ? "yes" : "no");
    ImGui::Text("Mirroring: %s", mirroring_name(cartridge.getMirroring()));
    ImGui::Text("IRQ Pending: %s", app->bus.cartridge.hasIRQ() ? "yes" : "no");
    if (!app->loaded_rom_path.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", app->loaded_rom_path.c_str());
    }
    ImGui::End();
}

void draw_palette_window(AppState* app)
{
    if (!app->show_palette_window) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(930.0f, 600.0f), ImGuiCond_Once);
    if (!ImGui::Begin("Palette", &app->show_palette_window)) {
        ImGui::End();
        return;
    }

    for (int index = 0; index < 32; ++index) {
        const u8 palette_index = app->bus.ppu.getPaletteByte(static_cast<u8>(index)) & 0x3FU;
        const ImVec4 color = color_from_rgb(nesPalette[palette_index]);
        char id[16] = {};
        snprintf(id, sizeof(id), "##pal_%d", index);
        ImGui::ColorButton(id, color, ImGuiColorEditFlags_NoTooltip, ImVec2(20.0f, 20.0f));
        if ((index & 3) != 3) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped("Palette RAM values are shown using the current PPU palette entries.");
    ImGui::End();
}

void draw_rom_browser(AppState* app)
{
    if (app->browser.open && !ImGui::IsPopupOpen("Open ROM")) {
        ImGui::OpenPopup("Open ROM");
    }

    bool keep_open = app->browser.open;
    ImGui::SetNextWindowSize(ImVec2(720.0f, 460.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Open ROM", &keep_open, ImGuiWindowFlags_NoResize)) {
        app->browser.open = keep_open;
        return;
    }

    if (app->browser.focus_path_input) {
        ImGui::SetKeyboardFocusHere();
        app->browser.focus_path_input = false;
    }

    if (ImGui::InputText("Path", app->browser.path_input.data(), app->browser.path_input.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        (void)browser_refresh_listing(app);
    }
    ImGui::SameLine();
    if (ImGui::Button("Up")) {
        browser_navigate_to_parent(app);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        (void)browser_refresh_listing(app);
    }

    ImGui::Separator();
    ImGui::BeginChild("##rom_list", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.5f), true);
    for (int index = 0; index < app->browser.entry_count; ++index) {
        const RomBrowserEntry& entry = app->browser.entries[static_cast<size_t>(index)];
        const bool selected = app->browser.selected_index == index;
        std::string label;
        if (entry.is_directory) {
            label = "[DIR] ";
            label += entry.name;
        } else {
            label = entry.name;
        }
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            app->browser.selected_index = index;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                (void)browser_activate_selection(app);
                keep_open = app->browser.open;
            }
        }
    }
    ImGui::EndChild();

    ImGui::TextWrapped("%s", app->browser.status.c_str());
    if (ImGui::Button("Open", ImVec2(120.0f, 0.0f))) {
        (void)browser_activate_selection(app);
        keep_open = app->browser.open;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
        keep_open = false;
    }

    app->browser.open = keep_open;
    if (!app->browser.open) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace

void draw_ui(AppState* app)
{
    bool state_changed = false;

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(880.0f, 720.0f), ImGuiCond_Once);
    if (ImGui::Begin("vNES", nullptr, ImGuiWindowFlags_MenuBar)) {
        state_changed |= draw_menu_bar(app);

        ImGui::BeginChild("##sidebar", ImVec2(260.0f, 0.0f), true);
        draw_sidebar(app, &state_changed);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##display", ImVec2(0.0f, 0.0f), true);
        draw_display(app);
        ImGui::EndChild();
    }
    ImGui::End();

    if (state_changed) {
        sync_audio_state(app);
    }

    draw_cpu_window(app);
    draw_ppu_window(app);
    draw_apu_window(app);
    draw_cartridge_window(app);
    draw_palette_window(app);
    draw_rom_browser(app);
}

} // namespace vnes_frontend