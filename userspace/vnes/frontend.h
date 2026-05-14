#ifndef VNES_FRONTEND_H
#define VNES_FRONTEND_H

#include "bus.h"
#include "hq2x.h"

#include "../include/vk.h"

#include <array>
#include <string>
#include <vector>

namespace vnes_frontend {

inline constexpr size_t kRomBrowserMaxEntries = 192;
inline constexpr size_t kRomBrowserPathMax = 256;
inline constexpr size_t kRomBrowserResponseMax = 16384;
inline constexpr size_t kRomBrowserItemMax = 128;
inline constexpr vk_u32 kMaxCatchupFrames = 4;

struct RomBrowserEntry {
    std::string name;
    vk_u64 size_bytes = 0;
    bool is_directory = false;
};

struct RomBrowserState {
    bool open = false;
    bool focus_path_input = false;
    std::string current_path;
    std::string status;
    std::array<char, kRomBrowserPathMax> path_input {};
    std::array<char, kRomBrowserResponseMax> response {};
    std::array<std::array<char, kRomBrowserItemMax>, kRomBrowserMaxEntries> raw_items {};
    std::array<RomBrowserEntry, kRomBrowserMaxEntries> entries {};
    int entry_count = 0;
    int selected_index = -1;
};

struct TimingState {
    vk_u64 next_frame_tick = 0;
    vk_u64 frame_tick_numerator = 0;
    vk_u64 frame_tick_denominator = 60;
    vk_u64 frame_tick_remainder = 0;
};

struct StatsState {
    vk_u64 last_sample_tick = 0;
    vk_u32 frames_since_sample = 0;
    float fps = 0.0f;
};

struct AppState {
    vk_framebuffer_info_t framebuffer = {};
    Bus bus;
    HQ2x hq2x;
    std::vector<u32> hq2x_output_buffer;
    bool quit_requested = false;
    bool rom_loaded = false;
    bool paused = false;
    bool step_requested = false;
    bool muted = false;
    bool use_hq2x = false;
    bool show_cpu_window = true;
    bool show_ppu_window = true;
    bool show_apu_window = false;
    bool show_cartridge_window = true;
    bool show_palette_window = false;
    std::string loaded_rom_path;
    std::string rom_title;
    std::string status_message;
    TimingState timing;
    StatsState stats;
    RomBrowserState browser;
};

bool init_framebuffer(AppState* app);
void set_status(AppState* app, const char* format, ...);
bool load_rom(AppState* app, const char* path);
void reset_emulator(AppState* app);
void handle_key_event(AppState* app, const vk_key_event_t& event, bool want_text_input);
void advance_emulation(AppState* app);
void idle_until_next_work(AppState* app);
void sync_audio_state(AppState* app);
void shutdown_app(AppState* app);

void get_display_frame(const AppState* app,
                       bool use_hq2x,
                       HQ2x* hq2x,
                       std::vector<u32>* hq2x_output_buffer,
                       const u32** pixels,
                       u32* width,
                       u32* height,
                       u32* stride);

void browser_open(AppState* app);
bool browser_refresh_listing(AppState* app);
void browser_navigate_to_parent(AppState* app);
bool browser_activate_selection(AppState* app);

void draw_ui(AppState* app);

} // namespace vnes_frontend

#endif // VNES_FRONTEND_H