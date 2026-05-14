#ifndef APU_H
#define APU_H

#include "types.h"
#include "sound.h"

class Sound;
class Bus;

class APU {
public:
    explicit APU(Bus& bus);

    void reset();
    void step();

    // CPU interface (registers $4000-$4017)
    u8 readRegister(u16 addr);
    void writeRegister(u16 addr, u8 data);

    // Frame counter IRQ
    bool isIRQ() const { return irq_flag || dmc.irq_flag; }
    void clearIRQ() { irq_flag = false; dmc.irq_flag = false; }

    // For debugger - channel status
    struct ChannelStatus {
        bool enabled;
        u8 volume;
        u16 period;
        u8 length;
    };
    
    ChannelStatus getPulse1Status() const {
        return { pulse[0].enabled, pulse[0].volume, pulse[0].timer_period, pulse[0].length_counter };
    }
    ChannelStatus getPulse2Status() const {
        return { pulse[1].enabled, pulse[1].volume, pulse[1].timer_period, pulse[1].length_counter };
    }
    ChannelStatus getTriangleStatus() const {
        return { triangle.enabled, 0, triangle.timer_period, triangle.length_counter };
    }
    ChannelStatus getNoiseStatus() const {
        return { noise.enabled, noise.volume, noise.timer_period, noise.length_counter };
    }
    ChannelStatus getDMCStatus() const {
        return { dmc.enabled, dmc.output, dmc.rate, 0 };
    }
    
    u8 getFrameCounterMode() const { return frame_counter_mode; }
    bool getIrqInhibit() const { return irq_inhibit; }
    
    // Audio output
    float getOutput() const;
    void setMuted(bool muted);
    bool isMuted() const { return muted; }

private:
    void clockTimers();
    void clockTriangleTimer();
    void clockLengthCounters();
    void clockSweeps();
    void clockEnvelopes();
    void clockTriangleLinear();
    void clockDMC();
    
    // Pulse duty cycle sequences
    static const u8 duty_table[4][8];
    
    // Pulse channels
    struct Pulse {
        u8 duty;
        u8 volume;
        u8 envelope_volume;
        u8 envelope_counter;
        bool envelope_start;
        bool constant_volume;
        bool length_halt;
        bool sweep_enabled;
        bool sweep_negate;
        bool sweep_reload;
        u8 sweep_shift;
        u8 sweep_period;
        u8 sweep_counter;
        u16 timer;
        u16 timer_period;
        u8 length_counter;
        u8 sequence_pos;
        bool enabled;
    } pulse[2];

    // Triangle channel
    struct Triangle {
        bool control;
        bool linear_reload_flag;
        u8 linear_counter;
        u8 linear_reload;
        u16 timer;
        u16 timer_period;
        u8 length_counter;
        u8 sequence_pos;
        bool enabled;
    } triangle;

    // Noise channel
    struct Noise {
        u8 volume;
        u8 envelope_volume;
        u8 envelope_counter;
        bool envelope_start;
        bool constant_volume;
        bool length_halt;
        bool mode;
        u16 timer;
        u16 timer_period;
        u8 length_counter;
        u16 shift_register;
        bool enabled;
    } noise;

    // DMC channel
    struct DMC {
        bool irq_enable;
        bool irq_flag;
        bool loop;
        u8 rate;
        u8 output;
        u16 sample_addr;
        u16 sample_length;
        u16 current_addr;
        u16 bytes_remaining;
        u16 timer;
        u8 shift_register;
        u8 bits_remaining;
        u8 sample_buffer;
        bool sample_buffer_empty;
        bool enabled;
    } dmc;

    // Frame counter
    u8 frame_counter_mode;
    bool irq_inhibit;
    bool irq_flag;
    u32 frame_counter;

    u64 cycles;

    // Sample generation
    Sound sound;
    Bus& bus;
    float sample_accumulator = 0.0f;
    int samples_this_frame = 0;
    static constexpr float CPU_CLOCK_RATE = 1789773.0f;
    static constexpr float SAMPLE_RATE = 44100.0f;
    static constexpr float CYCLES_PER_SAMPLE = CPU_CLOCK_RATE / SAMPLE_RATE;
    bool muted = false;
};

#endif // APU_H
