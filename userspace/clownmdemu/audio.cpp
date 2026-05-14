#include "frontend.h"

namespace clownmdemu_frontend {

namespace {

int32_t clamp_i16_range(int32_t value) {
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return value;
}

int16_t apply_volume_divisor(int16_t sample, vk_u32 divisor) {
    if (divisor <= 1)
        return sample;
    return static_cast<int16_t>(static_cast<int32_t>(sample) / static_cast<int32_t>(divisor));
}

bool ensure_scratch_buffer(int16_t** buffer, size_t* capacity_frames, size_t required_frames, size_t channels) {
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

void audio_add_sample(AppState* app, vk_u32 frame_index, int32_t left, int32_t right) {
    if (frame_index >= kMaxFrameAudioFrames)
        return;

    app->audio.frame_mix[frame_index * 2] += left;
    app->audio.frame_mix[frame_index * 2 + 1] += right;
    if (frame_index + 1 > app->audio.frame_frames)
        app->audio.frame_frames = frame_index + 1;
}

void audio_mix_chunk(AppState* app,
                     AudioSourceState* source,
                     const int16_t* samples,
                     size_t total_frames,
                     bool mono,
                     vk_u32 volume_divisor) {
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

void audio_queue_push(AppState* app, int16_t left, int16_t right) {
    if (app->audio.queue_count >= kQueueCapacityFrames)
        return;

    app->audio.queue[app->audio.queue_write * 2] = left;
    app->audio.queue[app->audio.queue_write * 2 + 1] = right;
    app->audio.queue_write = (app->audio.queue_write + 1) % kQueueCapacityFrames;
    ++app->audio.queue_count;
}

vk_u32 audio_pop_frames(AppState* app, int16_t* output, vk_u32 requested_frames) {
    const vk_u32 total_frames = min_u32(requested_frames, app->audio.queue_count);
    for (vk_u32 frame = 0; frame < total_frames; ++frame) {
        output[frame * 2] = app->audio.queue[app->audio.queue_read * 2];
        output[frame * 2 + 1] = app->audio.queue[app->audio.queue_read * 2 + 1];
        app->audio.queue_read = (app->audio.queue_read + 1) % kQueueCapacityFrames;
    }
    app->audio.queue_count -= total_frames;
    return total_frames;
}

bool audio_stage_play_block(AppState* app) {
    if (app->audio.play_block_pending || app->audio.queue_count == 0)
        return false;

    app->audio.play_block_frames = audio_pop_frames(app, app->audio.play_block, kPlayBlockFrames);
    app->audio.play_block_pending = app->audio.play_block_frames != 0;
    return app->audio.play_block_pending;
}

} // namespace

void audio_begin_frame(AppState* app) {
    app->audio.fm.frame_cursor = 0;
    app->audio.psg.frame_cursor = 0;
    app->audio.frame_frames = 0;
    memset(app->audio.frame_mix, 0, sizeof(app->audio.frame_mix));
}

void audio_commit_frame(AppState* app) {
    const vk_u32 source_frames = max_u32(app->audio.frame_frames,
        max_u32(app->audio.fm.frame_cursor, app->audio.psg.frame_cursor));

    for (vk_u32 frame = 0; frame < source_frames; ++frame) {
        const int32_t mixed_left = static_cast<int32_t>((static_cast<int64_t>(app->audio.frame_mix[frame * 2])
            * static_cast<int64_t>(kOutputGainNumerator)) / static_cast<int64_t>(kOutputGainDenominator));
        const int32_t mixed_right = static_cast<int32_t>((static_cast<int64_t>(app->audio.frame_mix[frame * 2 + 1])
            * static_cast<int64_t>(kOutputGainNumerator)) / static_cast<int64_t>(kOutputGainDenominator));

        const int16_t left = static_cast<int16_t>(clamp_i16_range(mixed_left));
        const int16_t right = static_cast<int16_t>(clamp_i16_range(mixed_right));
        audio_queue_push(app, left, right);
    }
}

void audio_try_submit(AppState* app) {
    while (true) {
        if (!app->audio.play_block_pending && !audio_stage_play_block(app))
            return;

        if (!VK_CALL(snd_play,
                app->audio.play_block,
                app->audio.play_block_frames * 2u * static_cast<vk_u32>(sizeof(int16_t)),
                VK_SND_FORMAT_SIGNED_16_STEREO)) {
            return;
        }

        app->audio.play_block_frames = 0;
        app->audio.play_block_pending = false;
    }
}

void audio_reset(AppState* app) {
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
    app->audio.play_block_frames = 0;
    app->audio.play_block_pending = false;
    memset(app->audio.frame_mix, 0, sizeof(app->audio.frame_mix));
    VK_CALL(snd_set_sample_rate, kOutputSampleRate);
    VK_CALL(snd_set_volume, 255, 255);
}

void log_message(void* user_data, const char* format, va_list arg) {
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

void callback_fm_audio(void* user_data,
                       ClownMDEmu* clownmdemu,
                       size_t total_frames,
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

void callback_psg_audio(void* user_data,
                        ClownMDEmu* clownmdemu,
                        size_t total_frames,
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

void callback_pcm_audio(void* user_data,
                        ClownMDEmu* clownmdemu,
                        size_t total_frames,
                        void (*generate_pcm_audio)(ClownMDEmu*, cc_s16l*, size_t)) {
    (void)user_data;
    (void)clownmdemu;
    (void)total_frames;
    (void)generate_pcm_audio;
}

void callback_cdda_audio(void* user_data,
                         ClownMDEmu* clownmdemu,
                         size_t total_frames,
                         void (*generate_cdda_audio)(ClownMDEmu*, cc_s16l*, size_t)) {
    (void)user_data;
    (void)clownmdemu;
    (void)total_frames;
    (void)generate_cdda_audio;
}

void callback_cd_seeked(void* user_data, cc_u32f sector_index) {
    (void)user_data;
    (void)sector_index;
}

void callback_cd_sector_read(void* user_data, cc_u16l* buffer) {
    (void)user_data;
    (void)buffer;
}

cc_bool callback_cd_track_seeked(void* user_data, cc_u16f track_index, ClownMDEmu_CDDAMode mode) {
    (void)user_data;
    (void)track_index;
    (void)mode;
    return cc_false;
}

size_t callback_cd_audio_read(void* user_data, cc_s16l* sample_buffer, size_t total_frames) {
    (void)user_data;
    (void)sample_buffer;
    (void)total_frames;
    return 0;
}

} // namespace clownmdemu_frontend