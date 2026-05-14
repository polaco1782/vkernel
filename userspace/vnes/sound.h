#ifndef SOUND_H
#define SOUND_H

#include "types.h"

class Sound {
public:
    Sound();

    void start();
    void stop();

    void pushSample(float sample);
    bool initialized;

private:
    void resetQueue();
    void submitPending();

    static constexpr u32 SAMPLE_RATE = 44100;
    static constexpr u32 PLAY_FRAMES = 1024;
    static constexpr u32 MAX_BUFFER_SIZE = SAMPLE_RATE * 2;
    static constexpr float DC_BLOCK_R = 0.995f;

    s16 sample_buffer[MAX_BUFFER_SIZE];
    s16 play_block[PLAY_FRAMES * 2];
    u32 buffer_read;
    u32 buffer_write;
    u32 buffer_count;
    float dc_prev_input;
    float dc_prev_output;
};

#endif // SOUND_H
