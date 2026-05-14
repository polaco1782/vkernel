#include "frontend.h"
#include "hq2x.h"

namespace clownmdemu_frontend {

void log_message(void* user_data, const char* format, va_list arg);
void callback_fm_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                       void (*generate_fm_audio)(ClownMDEmu*, cc_s16l*, size_t));
void callback_psg_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                        void (*generate_psg_audio)(ClownMDEmu*, cc_s16l*, size_t));
void callback_pcm_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                        void (*generate_pcm_audio)(ClownMDEmu*, cc_s16l*, size_t));
void callback_cdda_audio(void* user_data, ClownMDEmu* clownmdemu, size_t total_frames,
                         void (*generate_cdda_audio)(ClownMDEmu*, cc_s16l*, size_t));
void callback_cd_seeked(void* user_data, cc_u32f sector_index);
void callback_cd_sector_read(void* user_data, cc_u16l* buffer);
cc_bool callback_cd_track_seeked(void* user_data, cc_u16f track_index, ClownMDEmu_CDDAMode mode);
size_t callback_cd_audio_read(void* user_data, cc_s16l* sample_buffer, size_t total_frames);

namespace {

void blit_scaled_image(AppState* app,
                       const vk_u32* source_pixels,
                       vk_u32 source_width,
                       vk_u32 source_height,
                       vk_u32 source_stride) {
    if (app == nullptr || source_pixels == nullptr || source_width == 0 || source_height == 0)
        return;

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
        const vk_u32* source_row = &source_pixels[static_cast<vk_usize>(source_y) * source_stride];
        vk_u32* destination_row = &app->present_buffer[(static_cast<vk_usize>(y_offset + y) * app->framebuffer.stride) + x_offset];

        for (vk_u32 x = 0; x < destination_width; ++x) {
            const vk_u32 source_x = min_u32(source_width - 1u,
                static_cast<vk_u32>((static_cast<vk_u64>(x) * source_width) / destination_width));
            destination_row[x] = source_row[source_x];
        }
    }
}

bool try_apply_hq2x(AppState* app,
                    const vk_u32** source_pixels,
                    vk_u32* source_width,
                    vk_u32* source_height,
                    vk_u32* source_stride) {
    if (app == nullptr || source_pixels == nullptr || *source_pixels == nullptr
        || source_width == nullptr || source_height == nullptr || source_stride == nullptr
        || *source_width == 0 || *source_height == 0) {
        return false;
    }

    const vk_usize packed_pixels = static_cast<vk_usize>(*source_width) * static_cast<vk_usize>(*source_height);
    const vk_usize output_pixels = packed_pixels * 4u;
    app->hq2x_input_buffer.resize(packed_pixels);
    app->hq2x_output_buffer.resize(output_pixels);

    for (vk_u32 row = 0; row < *source_height; ++row) {
        memcpy(&app->hq2x_input_buffer[static_cast<vk_usize>(row) * *source_width],
               &(*source_pixels)[static_cast<vk_usize>(row) * *source_stride],
               static_cast<size_t>(*source_width) * sizeof(vk_u32));
    }

    HQ2x filter;
    filter.resize(app->hq2x_input_buffer.data(),
                  *source_width,
                  *source_height,
                  app->hq2x_output_buffer.data());

    *source_pixels = app->hq2x_output_buffer.data();
    *source_width *= 2u;
    *source_height *= 2u;
    *source_stride = *source_width;
    return true;
}

void callback_colour_updated(void* user_data, cc_u16f index, cc_u16f colour) {
    auto* app = static_cast<AppState*>(user_data);
    if (index < VDP_TOTAL_COLOURS)
        app->palette[index] = unpack_palette_colour(colour);
}

void callback_scanline_rendered(void* user_data,
                               cc_u16f scanline,
                               const cc_u8l* pixels,
                               cc_u16f left_boundary,
                               cc_u16f right_boundary,
                               cc_u16f screen_width,
                               cc_u16f screen_height) {
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

cc_bool callback_input_requested(void* user_data, cc_u8f player_id, ClownMDEmu_Button button_id) {
    auto* app = static_cast<AppState*>(user_data);
    if (player_id != 0 || button_id >= CLOWNMDEMU_BUTTON_MAX)
        return cc_false;
    return app->buttons[button_id] ? cc_true : cc_false;
}

cc_bool callback_save_file_opened_for_reading(void* user_data, const char* filename) {
    auto* app = static_cast<AppState*>(user_data);
    close_save_file(app);
    app->save_file = fopen(filename, "rb");
    return app->save_file != nullptr ? cc_true : cc_false;
}

cc_s16f callback_save_file_read(void* user_data) {
    auto* app = static_cast<AppState*>(user_data);
    if (app->save_file == nullptr)
        return -1;

    const int ch = fgetc(app->save_file);
    return ch == EOF ? -1 : static_cast<cc_s16f>(ch);
}

cc_bool callback_save_file_opened_for_writing(void* user_data, const char* filename) {
    (void)user_data;
    (void)filename;
    return cc_false;
}

void callback_save_file_written(void* user_data, cc_u8f byte) {
    (void)user_data;
    (void)byte;
}

void callback_save_file_closed(void* user_data) {
    auto* app = static_cast<AppState*>(user_data);
    close_save_file(app);
}

cc_bool callback_save_file_removed(void* user_data, const char* filename) {
    (void)user_data;
    (void)filename;
    return cc_false;
}

cc_bool callback_save_file_size_obtained(void* user_data, const char* filename, size_t* size) {
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

void configure_callbacks(AppState* app) {
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

void extract_rom_title(AppState* app) {
    app->rom_title.clear();
    if (app->rom_words.empty() || app->rom_words.size() * 2u < 0x180u)
        return;

    const cc_u32f header_offset = app->region == CLOWNMDEMU_REGION_DOMESTIC ? 0x120u : 0x150u;
    if (header_offset + 0x30u > app->rom_words.size() * 2u)
        return;

    app->rom_title.resize(0x30u);
    const cc_u16l* words = &app->rom_words[header_offset / 2u];
    for (size_t i = 0; i < 0x30u / 2u; ++i) {
        const cc_u16l word = words[i];
        app->rom_title[i * 2] = static_cast<char>((word >> 8) & 0xFFu);
        app->rom_title[i * 2 + 1] = static_cast<char>(word & 0xFFu);
    }

    while (!app->rom_title.empty() && app->rom_title.back() == ' ')
        app->rom_title.pop_back();
}

void set_timing(AppState* app) {
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

void present_frame(AppState* app) {
    if (app->present_buffer.empty() || app->screen_width == 0 || app->screen_height == 0)
        return;

    app->present_buffer.assign(app->present_buffer.size(), 0u);

    const vk_u32* source_pixels = app->frame_rgba;
    vk_u32 source_width = app->screen_width;
    vk_u32 source_height = app->screen_height;
    vk_u32 source_stride = VDP_MAX_SCANLINE_WIDTH;

    (void)try_apply_hq2x(app, &source_pixels, &source_width, &source_height, &source_stride);
    blit_scaled_image(app, source_pixels, source_width, source_height, source_stride);

    memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(app->framebuffer.base)),
           app->present_buffer.data(),
           app->present_buffer.size() * sizeof(vk_u32));
}

void prime_audio(AppState* app) {
    while (!app->quit_requested && app->audio.queue_count < kQueuePrimeFrames)
        emulate_frame(app);
    audio_try_submit(app);
}

} // namespace

bool load_rom(AppState* app, const char* path) {
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

    app->rom_words.clear();

    const size_t word_count = (file_size + 1u) / 2u;
    app->rom_words.resize(word_count);

    for (size_t index = 0; index < word_count; ++index) {
        const int high = fgetc(file);
        const int low = fgetc(file);
        app->rom_words[index] = static_cast<cc_u16l>(((high == EOF ? 0 : high) & 0xFF) << 8)
                              | static_cast<cc_u16l>((low == EOF ? 0 : low) & 0xFF);
    }

    fclose(file);

    app->loaded_rom_path = path;
    extract_rom_title(app);

    ClownMDEmu_SetCartridge(&app->emulator, app->rom_words.data(),
                            static_cast<cc_u32f>(app->rom_words.size()));
    ClownMDEmu_HardReset(&app->emulator, cc_true, cc_false);
    return true;
}

bool init_framebuffer(AppState* app) {
    VK_CALL(framebuffer_info, &app->framebuffer);
    if (!app->framebuffer.valid || app->framebuffer.base == 0 ||
        app->framebuffer.width == 0 || app->framebuffer.height == 0) {
        printf("No framebuffer available\n");
        return false;
    }

    const vk_usize present_pixels =
        static_cast<vk_usize>(app->framebuffer.stride) * app->framebuffer.height;
    app->present_buffer.assign(present_pixels, 0u);
    return true;
}

bool init_emulator(AppState* app) {
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

void schedule_next_frame(AppState* app) {
    app->timing.frame_tick_remainder += app->timing.frame_tick_numerator;
    const vk_u64 step = app->timing.frame_tick_remainder / app->timing.frame_tick_denominator;
    app->timing.frame_tick_remainder %= app->timing.frame_tick_denominator;
    app->timing.next_frame_tick += step == 0 ? 1 : step;
}

void idle_until_next_work(AppState* app) {
    const vk_u64 now = VK_CALL(tick_count);
    if (now >= app->timing.next_frame_tick) {
        VK_CALL(yield);
        return;
    }

    const vk_u64 ticks_until_frame = app->timing.next_frame_tick - now;

    /* vk_sleep() is quantised to the 100 Hz scheduler tick, so sleeping for a
     * full tick near the next frame or next audio handoff adds jitter. Only
     * sleep when there is at least one spare tick and a full queued block of
     * audio already waiting behind the active DMA submission. */
    if (ticks_until_frame > 1
        && VK_CALL(snd_is_playing)
        && app->audio.queue_count >= kPlayBlockFrames) {
        VK_CALL(sleep, ticks_until_frame - 1);
        return;
    }

    VK_CALL(yield);
}

void emulate_frame(AppState* app) {
    audio_begin_frame(app);
    memset(app->frame_rgba, 0, sizeof(app->frame_rgba));
    app->screen_width = kDefaultScreenWidth;
    app->screen_height = kDefaultScreenHeight;

    ClownMDEmu_Iterate(&app->emulator);
    audio_commit_frame(app);
    present_frame(app);
}

void reset_emulator(AppState* app) {
    audio_reset(app);
    memset(app->buttons, 0, sizeof(app->buttons));
    app->screen_width = kDefaultScreenWidth;
    app->screen_height = kDefaultScreenHeight;
    ClownMDEmu_HardReset(&app->emulator, cc_true, cc_false);
    set_timing(app);
    prime_audio(app);
    schedule_next_frame(app);
}

void pump_input(AppState* app) {
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
            case 0x0E:
                if (pressed)
                    app->loadrom_requested = true;
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
            default:
                break;
        }
    }
}

void destroy_app(AppState* app) {
    VK_CALL(snd_stop);
    close_save_file(app);
}

} // namespace clownmdemu_frontend