#pragma once

#include <array>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" {
#include "../include/vk.h"
#define CC_CPLUSPLUS 0
#include "vendor/clownmdemu-core/source/clownmdemu.h"
#include "vendor/clownmdemu-core/source/vdp.h"
#undef CC_CPLUSPLUS
}

namespace clownmdemu_frontend {

inline constexpr vk_u32 kOutputSampleRate = 44100;
/* AC'97 playback still runs in coarse DMA windows, so bigger submissions buy
 * more underrun tolerance when emulation or rendering stalls for a frame or two. */
inline constexpr vk_u32 kPlayBlockFrames = 4096;
inline constexpr vk_u32 kQueueCapacityFrames = kPlayBlockFrames * 4;
inline constexpr vk_u32 kQueuePrimeFrames = kPlayBlockFrames * 2;
inline constexpr vk_u32 kMaxFrameAudioFrames = 2048;
inline constexpr vk_u32 kMaxCatchupFrames = 8;
inline constexpr vk_u32 kOutputGainNumerator = 3;
inline constexpr vk_u32 kOutputGainDenominator = 2;
inline constexpr vk_u32 kMaxLogMessage = 512;
inline constexpr vk_u32 kDefaultScreenWidth = VDP_H40_SCREEN_WIDTH_IN_TILES * VDP_TILE_WIDTH;
inline constexpr vk_u32 kDefaultScreenHeight = VDP_V28_SCANLINES_IN_TILES * VDP_STANDARD_TILE_HEIGHT;
inline constexpr size_t kMaxRomBytes = 4u * 1024u * 1024u;
inline constexpr vk_u32 kRomBrowserMaxEntries = 128;
inline constexpr vk_u32 kRomBrowserNameMax = 96;
inline constexpr vk_u32 kRomBrowserPathMax = 256;
inline constexpr vk_u32 kRomBrowserStatusMax = 128;
inline constexpr vk_u32 kRomBrowserResponseMax = 16384;
inline constexpr vk_u32 kRomBrowserItemMax = 128;
inline constexpr vk_u32 kUiGlyphWidth = 5;
inline constexpr vk_u32 kUiGlyphHeight = 7;
inline constexpr vk_u32 kUiFontScale = 2;
inline constexpr vk_u32 kUiFontAdvance = (kUiGlyphWidth + 1u) * kUiFontScale;
inline constexpr vk_u32 kUiRowHeight = (kUiGlyphHeight * kUiFontScale) + 8u;
inline constexpr vk_u32 kUiPanelMargin = 16;
inline constexpr vk_u32 kUiPanelPadding = 18;
inline constexpr vk_u32 kUiHeaderHeight = 76;
inline constexpr vk_u32 kUiFooterHeight = 30;

struct AudioSourceState {
    vk_u32 source_rate;
    vk_u32 accumulator;
    vk_u32 frame_cursor;
    int16_t prev_left;
    int16_t prev_right;
    bool started;
};

struct AudioState {
    AudioSourceState fm;
    AudioSourceState psg;
    int32_t frame_mix[kMaxFrameAudioFrames * 2];
    vk_u32 frame_frames;
    int16_t queue[kQueueCapacityFrames * 2];
    vk_u32 queue_read;
    vk_u32 queue_write;
    vk_u32 queue_count;
    int16_t play_block[kPlayBlockFrames * 2];
    vk_u32 play_block_frames;
    bool play_block_pending;
    std::vector<int16_t> fm_scratch;
    std::vector<int16_t> psg_scratch;
};

struct TimingState {
    vk_u64 next_frame_tick;
    vk_u64 frame_tick_numerator;
    vk_u64 frame_tick_denominator;
    vk_u64 frame_tick_remainder;
};

struct RomBrowserEntry {
    std::string name;
    vk_u64 size_bytes = 0;
    bool is_directory = false;
};

struct RomBrowserState {
    std::string current_path;
    std::string status;
    std::array<RomBrowserEntry, kRomBrowserMaxEntries> entries;
    char response[kRomBrowserResponseMax];
    char raw_items[kRomBrowserMaxEntries][kRomBrowserItemMax];
    vk_u32 entry_count = 0;
    vk_u32 selected_index = 0;
    vk_u32 scroll_index = 0;
};

struct AppState {
    ClownMDEmu emulator;
    ClownMDEmu_Callbacks callbacks;
    std::vector<cc_u16l> rom_words;
    std::string rom_title;
    std::string loaded_rom_path;
    vk_framebuffer_info_t framebuffer;
    std::vector<vk_u32> present_buffer;
    std::vector<vk_u32> hq2x_input_buffer;
    std::vector<vk_u32> hq2x_output_buffer;
    vk_u32 palette[VDP_TOTAL_COLOURS];
    vk_u32 frame_rgba[VDP_MAX_SCANLINE_WIDTH * VDP_MAX_SCANLINES];
    vk_u32 screen_width;
    vk_u32 screen_height;
    bool buttons[CLOWNMDEMU_BUTTON_MAX];
    bool quit_requested;
    bool reset_requested;
    bool loadrom_requested;
    FILE* save_file;
    ClownMDEmu_TVStandard tv_standard;
    ClownMDEmu_Region region;
    AudioState audio;
    TimingState timing;
    RomBrowserState browser;
};

vk_u32 min_u32(vk_u32 lhs, vk_u32 rhs);
vk_u32 max_u32(vk_u32 lhs, vk_u32 rhs);
void copy_string(char* dst, size_t dst_capacity, const char* src);
int compare_casefolded(const char* lhs, const char* rhs);
bool ends_with_casefolded(const char* text, const char* suffix);
const char* path_basename(const char* path);
vk_u32 pack_pixel(unsigned char r, unsigned char g, unsigned char b);
vk_u32 unpack_palette_colour(cc_u16f colour);
vk_u32 current_fm_rate(ClownMDEmu_TVStandard tv_standard);
vk_u32 current_psg_rate(ClownMDEmu_TVStandard tv_standard);
void close_save_file(AppState* app);

void audio_begin_frame(AppState* app);
void audio_commit_frame(AppState* app);
void audio_reset(AppState* app);
void audio_try_submit(AppState* app);

bool load_rom(AppState* app, const char* path);
bool init_framebuffer(AppState* app);
bool init_emulator(AppState* app);
void schedule_next_frame(AppState* app);
void idle_until_next_work(AppState* app);
void emulate_frame(AppState* app);
void reset_emulator(AppState* app);
void pump_input(AppState* app);
bool browse_and_load_rom(AppState* app);
void destroy_app(AppState* app);

} // namespace clownmdemu_frontend