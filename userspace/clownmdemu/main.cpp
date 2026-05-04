#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../include/vk.h"
#define CC_CPLUSPLUS 0
#include "vendor/clownmdemu-core/source/clownmdemu.h"
#include "vendor/clownmdemu-core/source/vdp.h"
#undef CC_CPLUSPLUS
}

namespace {

constexpr const char* kRomPath = "sonic1.bin";
constexpr vk_u32 kOutputSampleRate = 44100;
constexpr vk_u32 kPlayBlockFrames = 2048;
constexpr vk_u32 kQueueCapacityFrames = 32768;
constexpr vk_u32 kQueuePrimeFrames = kPlayBlockFrames * 2;
constexpr vk_u32 kMaxFrameAudioFrames = 2048;
constexpr vk_u32 kMaxCatchupFrames = 3;
constexpr vk_u32 kMaxLogMessage = 512;
constexpr vk_u32 kDefaultScreenWidth = VDP_H40_SCREEN_WIDTH_IN_TILES * VDP_TILE_WIDTH;
constexpr vk_u32 kDefaultScreenHeight = VDP_V28_SCANLINES_IN_TILES * VDP_STANDARD_TILE_HEIGHT;
constexpr size_t kMaxRomBytes = 4u * 1024u * 1024u;

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
    int16_t* fm_scratch;
    size_t fm_scratch_frames;
    int16_t* psg_scratch;
    size_t psg_scratch_frames;
};

struct TimingState {
    vk_u64 next_frame_tick;
    vk_u64 frame_tick_numerator;
    vk_u64 frame_tick_denominator;
    vk_u64 frame_tick_remainder;
};

struct AppState {
    ClownMDEmu emulator;
    ClownMDEmu_Callbacks callbacks;
    cc_u16l* rom_words;
    cc_u32f rom_word_count;
    char rom_title[49];
    vk_framebuffer_info_t framebuffer;
    vk_u32* present_buffer;
    vk_usize present_pixels;
    vk_u32 palette[VDP_TOTAL_COLOURS];
    vk_u32 frame_rgba[VDP_MAX_SCANLINE_WIDTH * VDP_MAX_SCANLINES];
    vk_u32 screen_width;
    vk_u32 screen_height;
    bool buttons[CLOWNMDEMU_BUTTON_MAX];
    bool quit_requested;
    bool reset_requested;
    FILE* save_file;
    ClownMDEmu_TVStandard tv_standard;
    ClownMDEmu_Region region;
    AudioState audio;
    TimingState timing;
};

static vk_u32 min_u32(vk_u32 lhs, vk_u32 rhs) {
    return lhs < rhs ? lhs : rhs;
}

static vk_u32 max_u32(vk_u32 lhs, vk_u32 rhs) {
    return lhs > rhs ? lhs : rhs;
}

static int32_t clamp_i16_range(int32_t value) {
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return value;
}

static int16_t apply_volume_divisor(int16_t sample, vk_u32 divisor) {
    if (divisor <= 1)
        return sample;
    return static_cast<int16_t>(static_cast<int32_t>(sample) / static_cast<int32_t>(divisor));
}

static vk_u32 pack_pixel(const AppState* app, unsigned char r, unsigned char g, unsigned char b) {
    return (static_cast<vk_u32>(r) << 16) | (static_cast<vk_u32>(g) << 8) | static_cast<vk_u32>(b);
}

static vk_u32 unpack_palette_colour(const AppState* app, cc_u16f colour) {
    const unsigned char red = static_cast<unsigned char>((colour & 0x000Fu) * 0x11u);
    const unsigned char green = static_cast<unsigned char>(((colour >> 4) & 0x000Fu) * 0x11u);
    const unsigned char blue = static_cast<unsigned char>(((colour >> 8) & 0x000Fu) * 0x11u);
    return pack_pixel(app, red, green, blue);
}

static vk_u32 current_fm_rate(ClownMDEmu_TVStandard tv_standard) {
    return tv_standard == CLOWNMDEMU_TV_STANDARD_PAL ? CLOWNMDEMU_FM_SAMPLE_RATE_PAL : CLOWNMDEMU_FM_SAMPLE_RATE_NTSC;
}

static vk_u32 current_psg_rate(ClownMDEmu_TVStandard tv_standard) {
    return tv_standard == CLOWNMDEMU_TV_STANDARD_PAL ? CLOWNMDEMU_PSG_SAMPLE_RATE_PAL : CLOWNMDEMU_PSG_SAMPLE_RATE_NTSC;
}

static void close_save_file(AppState* app) {
    if (app->save_file != nullptr) {
        fclose(app->save_file);
        app->save_file = nullptr;
    }
}

static bool ensure_scratch_buffer(int16_t** buffer, size_t* capacity_frames, size_t required_frames, size_t channels) {
    if (required_frames <= *capacity_frames)
        return true;

    const size_t sample_count = required_frames * channels;
    auto* resized = static_cast<int16_t*>(realloc(*buffer, sample_count * sizeof(int16_t)));
    if (resized == nullptr)
        return false;

    *buffer = resized;
    *capacity_frames = required_frames;
    return true;
}

static void audio_begin_frame(AppState* app) {
    app->audio.fm.frame_cursor = 0;
    app->audio.psg.frame_cursor = 0;
    app->audio.frame_frames = 0;
    memset(app->audio.frame_mix, 0, sizeof(app->audio.frame_mix));
}

static void audio_add_sample(AppState* app, vk_u32 frame_index, int32_t left, int32_t right) {
    if (frame_index >= kMaxFrameAudioFrames)
        return;

    app->audio.frame_mix[frame_index * 2] += left;
    app->audio.frame_mix[frame_index * 2 + 1] += right;
    if (frame_index + 1 > app->audio.frame_frames)
        app->audio.frame_frames = frame_index + 1;
}

static void audio_mix_chunk(AppState* app, AudioSourceState* source, const int16_t* samples, size_t total_frames,
                            bool mono, vk_u32 volume_divisor) {
    if (samples == nullptr || total_frames == 0 || source->source_rate == 0)
        return;

    size_t start_index = 0;
    if (!source->started) {
        const int16_t first_left = apply_volume_divisor(mono ? samples[0] : samples[0], volume_divisor);
        const int16_t first_right = mono ? first_left : samples[1];
        source->started = true;
        source->prev_left = first_left;
        source->prev_right = first_right;
        audio_add_sample(app, source->frame_cursor++, first_left, first_right);
        start_index = 1;
    }

    for (size_t frame = start_index; frame < total_frames; ++frame) {
        const int16_t current_left = apply_volume_divisor(mono ? samples[frame] : samples[frame * 2], volume_divisor);
        const int16_t current_right = mono ? current_left : apply_volume_divisor(samples[frame * 2 + 1], volume_divisor);

        source->accumulator += kOutputSampleRate;
        while (source->accumulator >= source->source_rate) {
            const vk_u32 overshoot = source->accumulator - source->source_rate;
            const vk_u32 current_weight = kOutputSampleRate - overshoot;
            const vk_u32 previous_weight = overshoot;

            const int32_t mixed_left =
                (static_cast<int32_t>(source->prev_left) * static_cast<int32_t>(previous_weight) +
                 static_cast<int32_t>(current_left) * static_cast<int32_t>(current_weight)) /
                static_cast<int32_t>(kOutputSampleRate);
            const int32_t mixed_right =
                (static_cast<int32_t>(source->prev_right) * static_cast<int32_t>(previous_weight) +
                 static_cast<int32_t>(current_right) * static_cast<int32_t>(current_weight)) /
                static_cast<int32_t>(kOutputSampleRate);

            audio_add_sample(app, source->frame_cursor++, mixed_left, mixed_right);
            source->accumulator -= source->source_rate;
        }

        source->prev_left = current_left;
        source->prev_right = current_right;
    }
}

static void audio_queue_push(AppState* app, int16_t left, int16_t right) {
    if (app->audio.queue_count >= kQueueCapacityFrames)
        return;

    app->audio.queue[app->audio.queue_write * 2] = left;
    app->audio.queue[app->audio.queue_write * 2 + 1] = right;
    app->audio.queue_write = (app->audio.queue_write + 1) % kQueueCapacityFrames;
    ++app->audio.queue_count;
}

static void audio_commit_frame(AppState* app) {
    const vk_u32 source_frames = max_u32(app->audio.frame_frames,
        max_u32(app->audio.fm.frame_cursor, app->audio.psg.frame_cursor));

    for (vk_u32 frame = 0; frame < source_frames; ++frame) {
        const int16_t left = static_cast<int16_t>(clamp_i16_range(app->audio.frame_mix[frame * 2]));
        const int16_t right = static_cast<int16_t>(clamp_i16_range(app->audio.frame_mix[frame * 2 + 1]));
        audio_queue_push(app, left, right);
    }
}

static vk_u32 audio_pop_frames(AppState* app, int16_t* output, vk_u32 requested_frames) {
    const vk_u32 total_frames = min_u32(requested_frames, app->audio.queue_count);
    for (vk_u32 frame = 0; frame < total_frames; ++frame) {
        output[frame * 2] = app->audio.queue[app->audio.queue_read * 2];
        output[frame * 2 + 1] = app->audio.queue[app->audio.queue_read * 2 + 1];
        app->audio.queue_read = (app->audio.queue_read + 1) % kQueueCapacityFrames;
    }
    app->audio.queue_count -= total_frames;
    return total_frames;
}

static void audio_try_submit(AppState* app) {
    if (VK_CALL(snd_is_playing) || app->audio.queue_count == 0)
        return;

    const vk_u32 frames = audio_pop_frames(app, app->audio.play_block, kPlayBlockFrames);
    if (frames == 0)
        return;

    VK_CALL(snd_play,
        app->audio.play_block,
        frames * 2u * static_cast<vk_u32>(sizeof(int16_t)),
        VK_SND_FORMAT_SIGNED_16_STEREO);
}

static void audio_reset(AppState* app) {
    VK_CALL(snd_stop);
    app->audio.fm.accumulator = 0;
    app->audio.fm.frame_cursor = 0;
    app->audio.fm.prev_left = 0;
    app->audio.fm.prev_right = 0;
    app->audio.fm.started = false;
    app->audio.psg.accumulator = 0;
    app->audio.psg.frame_cursor = 0;
    app->audio.psg.prev_left = 0;
    app->audio.psg.prev_right = 0;
    app->audio.psg.started = false;
    app->audio.frame_frames = 0;
    app->audio.queue_read = 0;
    app->audio.queue_write = 0;
    app->audio.queue_count = 0;
    memset(app->audio.frame_mix, 0, sizeof(app->audio.frame_mix));
    VK_CALL(snd_set_sample_rate, kOutputSampleRate);
    VK_CALL(snd_set_volume, 255, 255);
}

static void set_timing(AppState* app) {
    const vk_u64 ticks_per_second = static_cast<vk_u64>(VK_CALL(ticks_per_sec));
    app->timing.next_frame_tick = VK_CALL(tick_count);
    app->timing.frame_tick_remainder = 0;

    if (app->tv_standard == CLOWNMDEMU_TV_STANDARD_PAL) {
        app->timing.frame_tick_numerator = ticks_per_second;
        app->timing.frame_tick_denominator = 50;
    } else {
        app->timing.frame_tick_numerator = ticks_per_second * 1001u;
        app->timing.frame_tick_denominator = 60000u;
    }
}

static void schedule_next_frame(AppState* app) {
    app->timing.frame_tick_remainder += app->timing.frame_tick_numerator;
    const vk_u64 step = app->timing.frame_tick_remainder / app->timing.frame_tick_denominator;
    app->timing.frame_tick_remainder %= app->timing.frame_tick_denominator;
    app->timing.next_frame_tick += step == 0 ? 1 : step;
}

static void log_message(void* user_data, const char* format, va_list arg) {
    auto* app = static_cast<AppState*>(user_data);
    (void)app;

    char buffer[kMaxLogMessage];
    vsnprintf(buffer, sizeof(buffer), format, arg);
    VK_CALL(puts, "[clownmdemu] ");
    VK_CALL(puts, buffer);
    const size_t length = strlen(buffer);
    if (length == 0 || buffer[length - 1] != '\n')
        VK_CALL(putc, '\n');
}

static void callback_colour_updated(void* user_data, cc_u16f index, cc_u16f colour) {
    auto* app = static_cast<AppState*>(user_data);
    if (index < VDP_TOTAL_COLOURS)
        app->palette[index] = unpack_palette_colour(app, colour);
}

static void callback_scanline_rendered(void* user_data, cc_u16f scanline, const cc_u8l* pixels,
                                       cc_u16f left_boundary, cc_u16f right_boundary,
                                       cc_u16f screen_width, cc_u16f screen_height) {
    auto* app = static_cast<AppState*>(user_data);
    if (scanline >= VDP_MAX_SCANLINES || pixels == nullptr || left_boundary >= VDP_MAX_SCANLINE_WIDTH)
        return;

    const vk_u32 safe_right = right_boundary > VDP_MAX_SCANLINE_WIDTH ? VDP_MAX_SCANLINE_WIDTH : right_boundary;
    if (safe_right <= left_boundary)
        return;

    vk_u32* row = &app->frame_rgba[static_cast<size_t>(scanline) * VDP_MAX_SCANLINE_WIDTH];
    for (cc_u16f i = 0; i < safe_right - left_boundary; ++i) {
        const cc_u8l palette_index = pixels[i];
        row[left_boundary + i] = palette_index < VDP_TOTAL_COLOURS ? app->palette[palette_index] : 0;
    }

    app->screen_width = screen_width;
    app->screen_height = screen_height;
}

static cc_bool callback_input_requested(void* user_data, cc_u8f player_id, ClownMDEmu_Button button_id) {
    auto* app = static_cast<AppState*>(user_data);
    if (player_id != 0 || button_id >= CLOWNMDEMU_BUTTON_MAX)
        return cc_false;
    return app->buttons[button_id] ? cc_true : cc_false;
}

static void callback_fm_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                              void (*generate_fm_audio)(ClownMDEmu*, cc_s16l*, size_t)) {
    auto* app = static_cast<AppState*>(user_data);
    if (!ensure_scratch_buffer(&app->audio.fm_scratch, &app->audio.fm_scratch_frames, total_frames, 2)) {
        app->quit_requested = true;
        return;
    }

    memset(app->audio.fm_scratch, 0, total_frames * 2u * sizeof(int16_t));
    generate_fm_audio(clownmdemu, app->audio.fm_scratch, total_frames);
    audio_mix_chunk(app, &app->audio.fm, app->audio.fm_scratch, total_frames, false, CLOWNMDEMU_FM_VOLUME_DIVISOR);
}

static void callback_psg_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                               void (*generate_psg_audio)(ClownMDEmu*, cc_s16l*, size_t)) {
    auto* app = static_cast<AppState*>(user_data);
    if (!ensure_scratch_buffer(&app->audio.psg_scratch, &app->audio.psg_scratch_frames, total_frames, 1)) {
        app->quit_requested = true;
        return;
    }

    memset(app->audio.psg_scratch, 0, total_frames * sizeof(int16_t));
    generate_psg_audio(clownmdemu, app->audio.psg_scratch, total_frames);
    audio_mix_chunk(app, &app->audio.psg, app->audio.psg_scratch, total_frames, true, CLOWNMDEMU_PSG_VOLUME_DIVISOR);
}

static void callback_pcm_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                               void (*generate_pcm_audio)(ClownMDEmu*, cc_s16l*, size_t)) {
    (void)user_data;
    (void)clownmdemu;
    (void)total_frames;
    (void)generate_pcm_audio;
}

static void callback_cdda_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                                void (*generate_cdda_audio)(ClownMDEmu*, cc_s16l*, size_t)) {
    (void)user_data;
    (void)clownmdemu;
    (void)total_frames;
    (void)generate_cdda_audio;
}

static void callback_cd_seeked(void* user_data, cc_u32f sector_index) {
    (void)user_data;
    (void)sector_index;
}

static void callback_cd_sector_read(void* user_data, cc_u16l* buffer) {
    (void)user_data;
    (void)buffer;
}

static cc_bool callback_cd_track_seeked(void* user_data, cc_u16f track_index, ClownMDEmu_CDDAMode mode) {
    (void)user_data;
    (void)track_index;
    (void)mode;
    return cc_false;
}

static size_t callback_cd_audio_read(void* user_data, cc_s16l* sample_buffer, size_t total_frames) {
    (void)user_data;
    (void)sample_buffer;
    (void)total_frames;
    return 0;
}

static cc_bool callback_save_file_opened_for_reading(void* user_data, const char* filename) {
    auto* app = static_cast<AppState*>(user_data);
    close_save_file(app);
    app->save_file = fopen(filename, "rb");
    return app->save_file != nullptr ? cc_true : cc_false;
}

static cc_s16f callback_save_file_read(void* user_data) {
    auto* app = static_cast<AppState*>(user_data);
    if (app->save_file == nullptr)
        return -1;

    const int ch = fgetc(app->save_file);
    return ch == EOF ? -1 : static_cast<cc_s16f>(ch);
}

static cc_bool callback_save_file_opened_for_writing(void* user_data, const char* filename) {
    (void)user_data;
    (void)filename;
    return cc_false;
}

static void callback_save_file_written(void* user_data, cc_u8f byte) {
    (void)user_data;
    (void)byte;
}

static void callback_save_file_closed(void* user_data) {
    auto* app = static_cast<AppState*>(user_data);
    close_save_file(app);
}

static cc_bool callback_save_file_removed(void* user_data, const char* filename) {
    (void)user_data;
    (void)filename;
    return cc_false;
}

static cc_bool callback_save_file_size_obtained(void* user_data, const char* filename, size_t* size) {
    (void)user_data;
    if (size == nullptr)
        return cc_false;

    FILE* file = fopen(filename, "rb");
    if (file == nullptr)
        return cc_false;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return cc_false;
    }

    const long length = ftell(file);
    fclose(file);
    if (length < 0)
        return cc_false;

    *size = static_cast<size_t>(length);
    return cc_true;
}

static void configure_callbacks(AppState* app) {
    app->callbacks = {};
    app->callbacks.user_data = app;
    app->callbacks.colour_updated = callback_colour_updated;
    app->callbacks.scanline_rendered = callback_scanline_rendered;
    app->callbacks.input_requested = callback_input_requested;
    app->callbacks.fm_audio_to_be_generated = callback_fm_audio;
    app->callbacks.psg_audio_to_be_generated = callback_psg_audio;
    app->callbacks.pcm_audio_to_be_generated = callback_pcm_audio;
    app->callbacks.cdda_audio_to_be_generated = callback_cdda_audio;
    app->callbacks.cd_seeked = callback_cd_seeked;
    app->callbacks.cd_sector_read = callback_cd_sector_read;
    app->callbacks.cd_track_seeked = callback_cd_track_seeked;
    app->callbacks.cd_audio_read = callback_cd_audio_read;
    app->callbacks.save_file_opened_for_reading = callback_save_file_opened_for_reading;
    app->callbacks.save_file_read = callback_save_file_read;
    app->callbacks.save_file_opened_for_writing = callback_save_file_opened_for_writing;
    app->callbacks.save_file_written = callback_save_file_written;
    app->callbacks.save_file_closed = callback_save_file_closed;
    app->callbacks.save_file_removed = callback_save_file_removed;
    app->callbacks.save_file_size_obtained = callback_save_file_size_obtained;
}

static void extract_rom_title(AppState* app) {
    memset(app->rom_title, 0, sizeof(app->rom_title));
    if (app->rom_words == nullptr || app->rom_word_count * 2u < 0x180u)
        return;

    const cc_u32f header_offset = app->region == CLOWNMDEMU_REGION_DOMESTIC ? 0x120u : 0x150u;
    if (header_offset + 0x30u > app->rom_word_count * 2u)
        return;

    const cc_u16l* words = &app->rom_words[header_offset / 2u];
    for (size_t i = 0; i < 0x30u / 2u; ++i) {
        const cc_u16l word = words[i];
        app->rom_title[i * 2] = static_cast<char>((word >> 8) & 0xFFu);
        app->rom_title[i * 2 + 1] = static_cast<char>(word & 0xFFu);
    }

    size_t end = strlen(app->rom_title);
    while (end > 0 && app->rom_title[end - 1] == ' ') {
        app->rom_title[end - 1] = '\0';
        --end;
    }
}

static bool load_rom(AppState* app, const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        printf("Unable to open %s\n", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    const long file_size_long = ftell(file);
    if (file_size_long < 0) {
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    const size_t file_size = static_cast<size_t>(file_size_long);
    if (file_size == 0 || file_size > kMaxRomBytes) {
        fclose(file);
        printf("Invalid ROM size: %zu bytes\n", file_size);
        return false;
    }

    free(app->rom_words);
    app->rom_words = nullptr;
    app->rom_word_count = 0;

    const size_t word_count = (file_size + 1u) / 2u;
    auto* rom_words = static_cast<cc_u16l*>(malloc(word_count * sizeof(cc_u16l)));
    if (rom_words == nullptr) {
        fclose(file);
        return false;
    }

    for (size_t index = 0; index < word_count; ++index) {
        const int high = fgetc(file);
        const int low = fgetc(file);
        rom_words[index] = static_cast<cc_u16l>(((high == EOF ? 0 : high) & 0xFF) << 8)
                         | static_cast<cc_u16l>((low == EOF ? 0 : low) & 0xFF);
    }

    fclose(file);

    app->rom_words = rom_words;
    app->rom_word_count = static_cast<cc_u32f>(word_count);
    extract_rom_title(app);

    ClownMDEmu_SetCartridge(&app->emulator, app->rom_words, app->rom_word_count);
    ClownMDEmu_HardReset(&app->emulator, cc_true, cc_false);
    return true;
}

static bool init_framebuffer(AppState* app) {
    VK_CALL(framebuffer_info, &app->framebuffer);
    if (!app->framebuffer.valid || app->framebuffer.base == 0 ||
        app->framebuffer.width == 0 || app->framebuffer.height == 0) {
        printf("No framebuffer available\n");
        return false;
    }

    app->present_pixels = static_cast<vk_usize>(app->framebuffer.stride) * app->framebuffer.height;
    app->present_buffer = static_cast<vk_u32*>(malloc(app->present_pixels * sizeof(vk_u32)));
    if (app->present_buffer == nullptr) {
        printf("Failed to allocate presentation buffer\n");
        return false;
    }

    memset(app->present_buffer, 0, app->present_pixels * sizeof(vk_u32));
    return true;
}

static void present_frame(const AppState* app) {
    if (app->present_buffer == nullptr || app->screen_width == 0 || app->screen_height == 0)
        return;

    memset(app->present_buffer, 0, app->present_pixels * sizeof(vk_u32));

    const vk_u32 source_width = app->screen_width;
    const vk_u32 source_height = app->screen_height;
    vk_u32 destination_width = 0;
    vk_u32 destination_height = 0;

    if (static_cast<vk_u64>(app->framebuffer.width) * source_height <=
        static_cast<vk_u64>(app->framebuffer.height) * source_width) {
        destination_width = app->framebuffer.width;
        destination_height = static_cast<vk_u32>(
            (static_cast<vk_u64>(app->framebuffer.width) * source_height) / source_width);
    } else {
        destination_height = app->framebuffer.height;
        destination_width = static_cast<vk_u32>(
            (static_cast<vk_u64>(app->framebuffer.height) * source_width) / source_height);
    }

    if (destination_width == 0)
        destination_width = 1;
    if (destination_height == 0)
        destination_height = 1;

    const vk_u32 x_offset = (app->framebuffer.width - destination_width) / 2u;
    const vk_u32 y_offset = (app->framebuffer.height - destination_height) / 2u;

    for (vk_u32 y = 0; y < destination_height; ++y) {
        const vk_u32 source_y = min_u32(source_height - 1u,
            static_cast<vk_u32>((static_cast<vk_u64>(y) * source_height) / destination_height));
        const vk_u32* source_row = &app->frame_rgba[static_cast<size_t>(source_y) * VDP_MAX_SCANLINE_WIDTH];
        vk_u32* destination_row = &app->present_buffer[(static_cast<vk_usize>(y_offset + y) * app->framebuffer.stride) + x_offset];

        for (vk_u32 x = 0; x < destination_width; ++x) {
            const vk_u32 source_x = min_u32(source_width - 1u,
                static_cast<vk_u32>((static_cast<vk_u64>(x) * source_width) / destination_width));
            destination_row[x] = source_row[source_x];
        }
    }

    memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(app->framebuffer.base)),
           app->present_buffer,
           app->present_pixels * sizeof(vk_u32));
}

static void emulate_frame(AppState* app) {
    audio_begin_frame(app);
    memset(app->frame_rgba, 0, sizeof(app->frame_rgba));
    app->screen_width = kDefaultScreenWidth;
    app->screen_height = kDefaultScreenHeight;

    ClownMDEmu_Iterate(&app->emulator);
    audio_commit_frame(app);
    present_frame(app);
}

static void prime_audio(AppState* app) {
    while (!app->quit_requested && app->audio.queue_count < kQueuePrimeFrames)
        emulate_frame(app);
    audio_try_submit(app);
}

static void reset_emulator(AppState* app) {
    audio_reset(app);
    memset(app->buttons, 0, sizeof(app->buttons));
    app->screen_width = kDefaultScreenWidth;
    app->screen_height = kDefaultScreenHeight;
    ClownMDEmu_HardReset(&app->emulator, cc_true, cc_false);
    set_timing(app);
    prime_audio(app);
}

static void pump_input(AppState* app) {
    vk_key_event_t event;
    while (VK_CALL(poll_key, &event)) {
        const bool pressed = event.pressed != 0;
        switch (event.scancode) {
            case 0x01:
                if (pressed)
                    app->quit_requested = true;
                break;
            case 0x0F:
                if (pressed)
                    app->reset_requested = true;
                break;
            case 0xC8:
            case 0x48:
                app->buttons[CLOWNMDEMU_BUTTON_UP] = pressed;
                break;
            case 0xD0:
            case 0x50:
                app->buttons[CLOWNMDEMU_BUTTON_DOWN] = pressed;
                break;
            case 0xCB:
            case 0x4B:
                app->buttons[CLOWNMDEMU_BUTTON_LEFT] = pressed;
                break;
            case 0xCD:
            case 0x4D:
                app->buttons[CLOWNMDEMU_BUTTON_RIGHT] = pressed;
                break;
            case 0x1E:
                app->buttons[CLOWNMDEMU_BUTTON_A] = pressed;
                break;
            case 0x1F:
                app->buttons[CLOWNMDEMU_BUTTON_B] = pressed;
                break;
            case 0x20:
                app->buttons[CLOWNMDEMU_BUTTON_C] = pressed;
                break;
            case 0x10:
                app->buttons[CLOWNMDEMU_BUTTON_X] = pressed;
                break;
            case 0x11:
                app->buttons[CLOWNMDEMU_BUTTON_Y] = pressed;
                break;
            case 0x12:
                app->buttons[CLOWNMDEMU_BUTTON_Z] = pressed;
                break;
            case 0x1C:
                app->buttons[CLOWNMDEMU_BUTTON_START] = pressed;
                break;
            case 0x0E:
                app->buttons[CLOWNMDEMU_BUTTON_MODE] = pressed;
                break;
            default:
                break;
        }
    }
}

static bool init_emulator(AppState* app) {
    app->tv_standard = CLOWNMDEMU_TV_STANDARD_NTSC;
    app->region = CLOWNMDEMU_REGION_OVERSEAS;

    configure_callbacks(app);

    ClownMDEmu_InitialConfiguration config = {};
    config.general.region = app->region;
    config.general.tv_standard = app->tv_standard;
    config.general.low_pass_filter_disabled = cc_false;
    config.general.cd_add_on_enabled = cc_false;

    ClownMDEmu_Initialise(&app->emulator, &config, &app->callbacks);
    ClownMDEmu_SetLogCallback(log_message, app);

    app->audio.fm.source_rate = current_fm_rate(app->tv_standard);
    app->audio.psg.source_rate = current_psg_rate(app->tv_standard);
    return true;
}

static void destroy_app(AppState* app) {
    VK_CALL(snd_stop);
    close_save_file(app);
    free(app->audio.fm_scratch);
    free(app->audio.psg_scratch);
    free(app->present_buffer);
    free(app->rom_words);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    ClownMDEmu_Constant_Initialise();

    auto* app = static_cast<AppState*>(calloc(1, sizeof(AppState)));
    if (app == nullptr) {
        printf("Failed to allocate app state\n");
        return 1;
    }

    if (!init_framebuffer(app)) {
        free(app);
        return 1;
    }
    if (!init_emulator(app)) {
        destroy_app(app);
        free(app);
        return 1;
    }
    if (!load_rom(app, kRomPath)) {
        printf("Place %s next to the staged userspace binaries.\n", kRomPath);
        destroy_app(app);
        free(app);
        return 1;
    }

    audio_reset(app);
    set_timing(app);
    prime_audio(app);
    schedule_next_frame(app);

    printf("ClownMDEmu\n");
    if (app->rom_title[0] != '\0')
        printf("Loaded: %s\n", app->rom_title);
    printf("Controls: arrows, A/S/D, Q/W/E, Enter, Backspace, Tab, Escape\n");

    while (!app->quit_requested) {
        pump_input(app);

        if (app->reset_requested) {
            app->reset_requested = false;
            reset_emulator(app);
            schedule_next_frame(app);
            continue;
        }

        audio_try_submit(app);

        vk_u64 now = VK_CALL(tick_count);
        vk_u32 catchup_frames = 0;
        while (now >= app->timing.next_frame_tick && catchup_frames < kMaxCatchupFrames) {
            emulate_frame(app);
            audio_try_submit(app);
            schedule_next_frame(app);
            now = VK_CALL(tick_count);
            ++catchup_frames;
        }

        VK_CALL(yield);
    }

    destroy_app(app);
    free(app);
    return 0;
}