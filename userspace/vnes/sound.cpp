#include "sound.h"

#include "../include/vk.h"

namespace {

inline auto clamp_sample(float value) -> float
{
    if (value > 1.0f) {
        return 1.0f;
    }
    if (value < -1.0f) {
        return -1.0f;
    }
    return value;
}

} // namespace

Sound::Sound()
    : initialized(false)
    , buffer_read(0)
    , buffer_write(0)
    , buffer_count(0)
    , dc_prev_input(0.0f)
    , dc_prev_output(0.0f)
{
    resetQueue();
}

void Sound::resetQueue()
{
    buffer_read = 0;
    buffer_write = 0;
    buffer_count = 0;
    dc_prev_input = 0.0f;
    dc_prev_output = 0.0f;
}

void Sound::start()
{
    VK_CALL(snd_stop);
    VK_CALL(snd_set_sample_rate, SAMPLE_RATE);
    VK_CALL(snd_set_volume, 255, 255);
    resetQueue();
    initialized = true;
}

void Sound::stop()
{
    if (!initialized) {
        return;
    }

    VK_CALL(snd_stop);
    resetQueue();
}

void Sound::submitPending()
{
    while (buffer_count >= PLAY_FRAMES) {
        for (u32 index = 0; index < PLAY_FRAMES; ++index) {
            const s16 sample = sample_buffer[(buffer_read + index) % MAX_BUFFER_SIZE];
            play_block[index * 2] = sample;
            play_block[index * 2 + 1] = sample;
        }

        if (!VK_CALL(snd_play,
                     play_block,
                     PLAY_FRAMES * 2u * static_cast<u32>(sizeof(s16)),
                     VK_SND_FORMAT_SIGNED_16_STEREO)) {
            return;
        }

        buffer_read = (buffer_read + PLAY_FRAMES) % MAX_BUFFER_SIZE;
        buffer_count -= PLAY_FRAMES;
    }
}

void Sound::pushSample(float sample)
{
    float filtered = sample - dc_prev_input + (DC_BLOCK_R * dc_prev_output);
    dc_prev_input = sample;
    dc_prev_output = filtered;

    const s16 output = static_cast<s16>(clamp_sample(filtered) * 32767.0f);

    sample_buffer[buffer_write] = output;
    buffer_write = (buffer_write + 1) % MAX_BUFFER_SIZE;
    if (buffer_count < MAX_BUFFER_SIZE) {
        ++buffer_count;
    } else {
        buffer_read = (buffer_read + 1) % MAX_BUFFER_SIZE;
    }

    submitPending();
}
