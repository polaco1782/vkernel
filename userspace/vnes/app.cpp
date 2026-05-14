#include "frontend.h"

#include "../vgui/imgui/imgui.h"

#include <stdarg.h>
#include <stdio.h>

#include <iostream>

namespace vnes_frontend {

namespace {

auto last_separator_index(const std::string& path) -> size_t
{
    for (size_t index = path.size(); index > 0; --index) {
        const char ch = path[index - 1];
        if (ch == '/' || ch == '\\') {
            return index - 1;
        }
    }

    return path.size();
}

auto last_dot_index(const std::string& name) -> size_t
{
    for (size_t index = name.size(); index > 0; --index) {
        if (name[index - 1] == '.') {
            return index - 1;
        }
    }

    return name.size();
}

auto basename_of(const std::string& path) -> std::string
{
    const size_t slash = last_separator_index(path);
    if (slash == path.size()) {
        return path;
    }
    return path.substr(slash + 1);
}

auto strip_extension(const std::string& name) -> std::string
{
    const size_t dot = last_dot_index(name);
    if (dot == name.size()) {
        return name;
    }
    return name.substr(0, dot);
}

void reset_timing(AppState* app)
{
    const vk_u64 ticks_per_second = static_cast<vk_u64>(VK_CALL(ticks_per_sec));
    app->timing.next_frame_tick = VK_CALL(tick_count);
    app->timing.frame_tick_numerator = ticks_per_second;
    app->timing.frame_tick_denominator = 60;
    app->timing.frame_tick_remainder = 0;

    app->stats.last_sample_tick = app->timing.next_frame_tick;
    app->stats.frames_since_sample = 0;
    app->stats.fps = 0.0f;
}

void schedule_next_frame(AppState* app)
{
    app->timing.frame_tick_remainder += app->timing.frame_tick_numerator;
    const vk_u64 step = app->timing.frame_tick_remainder / app->timing.frame_tick_denominator;
    app->timing.frame_tick_remainder %= app->timing.frame_tick_denominator;
    app->timing.next_frame_tick += step == 0 ? 1 : step;
}

void update_stats(AppState* app, vk_u32 frames_emulated)
{
    app->stats.frames_since_sample += frames_emulated;

    const vk_u64 now = VK_CALL(tick_count);
    if (app->stats.last_sample_tick == 0) {
        app->stats.last_sample_tick = now;
        return;
    }

    const vk_u64 ticks_per_second = static_cast<vk_u64>(VK_CALL(ticks_per_sec));
    const vk_u64 elapsed = now - app->stats.last_sample_tick;
    if (elapsed >= ticks_per_second && ticks_per_second != 0) {
        app->stats.fps = static_cast<float>(app->stats.frames_since_sample)
                       * static_cast<float>(ticks_per_second)
                       / static_cast<float>(elapsed);
        app->stats.frames_since_sample = 0;
        app->stats.last_sample_tick = now;
    }
}

void set_controller_button(AppState* app, Input::Button button, bool pressed)
{
    app->bus.input.setButton(button, pressed);
}

void handle_controller_key(AppState* app, const vk_key_event_t& event)
{
    const bool pressed = event.pressed != 0;

    switch (event.scancode) {
        case 0x48:
        case 0xC8:
            set_controller_button(app, Input::BUTTON_UP, pressed);
            break;
        case 0x50:
        case 0xD0:
            set_controller_button(app, Input::BUTTON_DOWN, pressed);
            break;
        case 0x4B:
        case 0xCB:
            set_controller_button(app, Input::BUTTON_LEFT, pressed);
            break;
        case 0x4D:
        case 0xCD:
            set_controller_button(app, Input::BUTTON_RIGHT, pressed);
            break;
        case 0x1E:
        case 0x2C:
            set_controller_button(app, Input::BUTTON_B, pressed);
            break;
        case 0x1F:
        case 0x2D:
            set_controller_button(app, Input::BUTTON_A, pressed);
            break;
        case 0x39:
            set_controller_button(app, Input::BUTTON_SELECT, pressed);
            break;
        case 0x1C:
            set_controller_button(app, Input::BUTTON_START, pressed);
            break;
        default:
            break;
    }
}

void emulate_frame(AppState* app)
{
    app->bus.ppu.clearFrameComplete();

    while (!app->bus.ppu.isFrameComplete()) {
        app->bus.clock();
    }

    app->bus.cartridge.signalFrameComplete();
}

} // namespace

bool init_framebuffer(AppState* app)
{
    VK_CALL(framebuffer_info, &app->framebuffer);
    if (app->framebuffer.valid == 0
        || app->framebuffer.base == 0
        || app->framebuffer.width == 0
        || app->framebuffer.height == 0) {
        return false;
    }

    return true;
}

void set_status(AppState* app, const char* format, ...)
{
    char buffer[256] = {};

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    app->status_message = buffer;
}

bool load_rom(AppState* app, const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        set_status(app, "No ROM path provided.");
        app->rom_loaded = false;
        sync_audio_state(app);
        return false;
    }

    if (!app->bus.loadCartridge(path)) {
        set_status(app, "Failed to load %s", path);
        app->rom_loaded = false;
        app->loaded_rom_path.clear();
        app->rom_title.clear();
        sync_audio_state(app);
        return false;
    }

    app->bus.reset();
    app->loaded_rom_path = path;
    app->rom_title = strip_extension(basename_of(app->loaded_rom_path));
    app->rom_loaded = true;
    app->paused = false;
    app->step_requested = false;
    reset_timing(app);
    sync_audio_state(app);
    set_status(app, "Loaded %s", app->rom_title.c_str());
    return true;
}

void reset_emulator(AppState* app)
{
    if (!app->rom_loaded) {
        return;
    }

    app->bus.reset();
    app->paused = false;
    app->step_requested = false;
    reset_timing(app);
    sync_audio_state(app);
    set_status(app, "Reset %s", app->rom_title.c_str());
}

void handle_key_event(AppState* app, const vk_key_event_t& event, bool want_text_input)
{
    if (event.pressed != 0) {
        if ((event.modifiers & 2U) != 0U) {
            if (event.ascii == 'q' || event.ascii == 'Q') {
                app->quit_requested = true;
                return;
            }
            if (event.ascii == 'o' || event.ascii == 'O') {
                browser_open(app);
                return;
            }
            if (event.ascii == 'p' || event.ascii == 'P') {
                app->paused = !app->paused;
                set_status(app, app->paused ? "Paused." : "Resumed.");
            }
            if (event.ascii == 'r' || event.ascii == 'R') {
                reset_emulator(app);
                return;
            }
            if (event.ascii == 'm' || event.ascii == 'M') {
                app->muted = !app->muted;
                set_status(app, app->muted ? "Audio muted." : "Audio unmuted.");
            }
        }

        if (event.scancode == 0x01U && app->browser.open) {
            app->browser.open = false;
            return;
        }
    }

    if (!app->rom_loaded || app->browser.open || want_text_input) {
        return;
    }

    handle_controller_key(app, event);
}

void sync_audio_state(AppState* app)
{
    app->bus.apu.setMuted(!app->rom_loaded || app->muted || app->paused || app->browser.open);
}

void advance_emulation(AppState* app)
{
    sync_audio_state(app);
    if (!app->rom_loaded) {
        return;
    }

    if (app->step_requested) {
        emulate_frame(app);
        app->step_requested = false;
        update_stats(app, 1);
        return;
    }

    if (app->paused || app->browser.open) {
        return;
    }

    vk_u32 frames_emulated = 0;
    vk_u64 now = VK_CALL(tick_count);
    while (now >= app->timing.next_frame_tick && frames_emulated < kMaxCatchupFrames) {
        emulate_frame(app);
        schedule_next_frame(app);
        ++frames_emulated;
        now = VK_CALL(tick_count);
    }

    if (frames_emulated != 0) {
        update_stats(app, frames_emulated);
    }
}

void idle_until_next_work(AppState* app)
{
    if (!app->rom_loaded || app->paused || app->browser.open) {
        VK_CALL(yield);
        return;
    }

    const vk_u64 now = VK_CALL(tick_count);
    if (now >= app->timing.next_frame_tick) {
        VK_CALL(yield);
        return;
    }

    const vk_u64 ticks_until_frame = app->timing.next_frame_tick - now;
    if (ticks_until_frame > 1) {
        VK_CALL(sleep, ticks_until_frame - 1);
        return;
    }

    VK_CALL(yield);
}

void shutdown_app(AppState* app)
{
    sync_audio_state(app);
    app->bus.cartridge.flushSRAM();
}

void get_display_frame(const AppState* app,
                       bool use_hq2x,
                       HQ2x* hq2x,
                       std::vector<u32>* hq2x_output_buffer,
                       const u32** pixels,
                       u32* width,
                       u32* height,
                       u32* stride)
{
    *pixels = app->bus.ppu.getFramebuffer();
    *width = NES_WIDTH;
    *height = NES_HEIGHT;
    *stride = NES_WIDTH;

    if (!use_hq2x || hq2x == nullptr || hq2x_output_buffer == nullptr) {
        return;
    }

    hq2x_output_buffer->resize(static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4U);
    hq2x->resize(*pixels, *width, *height, hq2x_output_buffer->data());
    *pixels = hq2x_output_buffer->data();
    *width *= 2U;
    *height *= 2U;
    *stride = *width;
}

} // namespace vnes_frontend