#include "apu.h"
#include "bus.h"

// Length counter lookup table
static const u8 length_table[32] = {
    10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
    12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

// Noise period lookup table (NTSC)
static const u16 noise_period_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

// DMC rate lookup table (NTSC)
static const u16 dmc_rate_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106, 85, 72, 54
};

// Triangle sequence
static const u8 triangle_sequence[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

// Duty cycle sequences for pulse channels
const u8 APU::duty_table[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},  // 12.5%
    {0, 1, 1, 0, 0, 0, 0, 0},  // 25%
    {0, 1, 1, 1, 1, 0, 0, 0},  // 50%
    {1, 0, 0, 1, 1, 1, 1, 1}   // 25% negated
};

APU::APU(Bus& b)
    : frame_counter_mode(0)
    , irq_inhibit(false)
    , irq_flag(false)
    , frame_counter(0)
    , cycles(0)
    , sound()
    , bus(b)
    , sample_accumulator(0.0f)
    , samples_this_frame(0)
{
    // Initialize pulse channels
    for (int i = 0; i < 2; i++) {
        pulse[i].duty = 0;
        pulse[i].volume = 0;
        pulse[i].envelope_volume = 0;
        pulse[i].envelope_counter = 0;
        pulse[i].envelope_start = false;
        pulse[i].constant_volume = false;
        pulse[i].length_halt = false;
        pulse[i].sweep_enabled = false;
        pulse[i].sweep_negate = false;
        pulse[i].sweep_reload = false;
        pulse[i].sweep_shift = 0;
        pulse[i].sweep_period = 0;
        pulse[i].sweep_counter = 0;
        pulse[i].timer = 0;
        pulse[i].timer_period = 0;
        pulse[i].length_counter = 0;
        pulse[i].sequence_pos = 0;
        pulse[i].enabled = false;
    }

    // Initialize triangle
    triangle.control = false;
    triangle.linear_reload_flag = false;
    triangle.linear_counter = 0;
    triangle.linear_reload = 0;
    triangle.timer = 0;
    triangle.timer_period = 0;
    triangle.length_counter = 0;
    triangle.sequence_pos = 0;
    triangle.enabled = false;

    // Initialize noise
    noise.volume = 0;
    noise.envelope_volume = 0;
    noise.envelope_counter = 0;
    noise.envelope_start = false;
    noise.constant_volume = false;
    noise.length_halt = false;
    noise.mode = false;
    noise.timer = 0;
    noise.timer_period = 0;
    noise.length_counter = 0;
    noise.shift_register = 1;
    noise.enabled = false;

    // Initialize DMC
    dmc.irq_enable = false;
    dmc.irq_flag = false;
    dmc.loop = false;
    dmc.rate = 0;
    dmc.output = 0;
    dmc.sample_addr = 0;
    dmc.sample_length = 0;
    dmc.current_addr = 0;
    dmc.bytes_remaining = 0;
    dmc.timer = 0;
    dmc.shift_register = 0;
    dmc.bits_remaining = 0;
    dmc.sample_buffer = 0;
    dmc.sample_buffer_empty = true;
    dmc.enabled = false;
}

void APU::reset()
{
    writeRegister(0x4015, 0);
    writeRegister(0x4017, 0);
    cycles = 0;
    frame_counter = 0;
    sample_accumulator = 0.0f;
    samples_this_frame = 0;

    sound.start();
}

void APU::setMuted(bool value)
{
    if (muted == value) {
        return;
    }

    muted = value;
    if (muted) {
        sound.stop();
    } else {
        sound.start();
    }
}

void APU::clockTimers()
{
    // Pulse channels (clocked every other CPU cycle, so every APU cycle)
    for (int i = 0; i < 2; i++) {
        if (pulse[i].timer == 0) {
            pulse[i].timer = pulse[i].timer_period;
            pulse[i].sequence_pos = (pulse[i].sequence_pos + 1) & 0x07;
        } else {
            pulse[i].timer--;
        }
    }

    // Noise channel
    if (noise.timer == 0) {
        noise.timer = noise.timer_period;
        // Feedback bit
        u8 feedback_bit = noise.mode ? 6 : 1;
        u16 feedback = (noise.shift_register & 1) ^ ((noise.shift_register >> feedback_bit) & 1);
        noise.shift_register = (noise.shift_register >> 1) | (feedback << 14);
    } else {
        noise.timer--;
    }
}

void APU::clockTriangleTimer()
{
    // Triangle channel (clocked every CPU cycle)
    if (triangle.timer == 0) {
        triangle.timer = triangle.timer_period;
        if (triangle.length_counter > 0 && triangle.linear_counter > 0) {
            triangle.sequence_pos = (triangle.sequence_pos + 1) & 0x1F;
        }
    } else {
        triangle.timer--;
    }
}

void APU::clockLengthCounters()
{
    // Pulse channels
    for (int i = 0; i < 2; i++) {
        if (!pulse[i].length_halt && pulse[i].length_counter > 0) {
            pulse[i].length_counter--;
        }
    }
    
    // Triangle
    if (!triangle.control && triangle.length_counter > 0) {
        triangle.length_counter--;
    }
    
    // Noise
    if (!noise.length_halt && noise.length_counter > 0) {
        noise.length_counter--;
    }
}

void APU::clockSweeps()
{
    for (int i = 0; i < 2; i++) {
        Pulse& p = pulse[i];
        if (p.sweep_counter == 0) {
            if (p.sweep_enabled && p.sweep_shift > 0) {
                u16 change = p.timer_period >> p.sweep_shift;
                u16 target = p.sweep_negate
                    ? (i == 0 ? (p.timer_period - change - 1) : (p.timer_period - change))
                    : (p.timer_period + change);

                if (p.timer_period >= 8 && target <= 0x7FF) {
                    p.timer_period = target;
                }
            }
            p.sweep_counter = p.sweep_period;
            p.sweep_reload = false;
        } else if (p.sweep_reload) {
            p.sweep_counter = p.sweep_period;
            p.sweep_reload = false;
        } else {
            p.sweep_counter--;
        }
    }
}

void APU::clockEnvelopes()
{
    // Pulse channels
    for (int i = 0; i < 2; i++) {
        if (pulse[i].envelope_start) {
            pulse[i].envelope_start = false;
            pulse[i].envelope_volume = 15;
            pulse[i].envelope_counter = pulse[i].volume;
        } else {
            if (pulse[i].envelope_counter > 0) {
                pulse[i].envelope_counter--;
            } else {
                pulse[i].envelope_counter = pulse[i].volume;
                if (pulse[i].envelope_volume > 0) {
                    pulse[i].envelope_volume--;
                } else if (pulse[i].length_halt) {
                    pulse[i].envelope_volume = 15;
                }
            }
        }
    }
    
    // Noise
    if (noise.envelope_start) {
        noise.envelope_start = false;
        noise.envelope_volume = 15;
        noise.envelope_counter = noise.volume;
    } else {
        if (noise.envelope_counter > 0) {
            noise.envelope_counter--;
        } else {
            noise.envelope_counter = noise.volume;
            if (noise.envelope_volume > 0) {
                noise.envelope_volume--;
            } else if (noise.length_halt) {
                noise.envelope_volume = 15;
            }
        }
    }
}

void APU::clockTriangleLinear()
{
    if (triangle.linear_reload_flag) {
        triangle.linear_counter = triangle.linear_reload;
    } else if (triangle.linear_counter > 0) {
        triangle.linear_counter--;
    }
    
    if (!triangle.control) {
        triangle.linear_reload_flag = false;
    }
}

void APU::clockDMC()
{
    // Fetch new sample byte when buffer empty
    if (dmc.sample_buffer_empty && dmc.bytes_remaining > 0) {
        dmc.sample_buffer = bus.read(dmc.current_addr);
        dmc.sample_buffer_empty = false;

        dmc.current_addr = (dmc.current_addr == 0xFFFF) ? 0x8000 : (dmc.current_addr + 1);
        dmc.bytes_remaining--;

        if (dmc.bytes_remaining == 0) {
            if (dmc.loop) {
                dmc.current_addr = dmc.sample_addr;
                dmc.bytes_remaining = dmc.sample_length;
            } else if (dmc.irq_enable) {
                dmc.irq_flag = true;
            }
        }
    }

    if (dmc.timer == 0) {
        dmc.timer = dmc_rate_table[dmc.rate];

        if (dmc.bits_remaining == 0) {
            if (!dmc.sample_buffer_empty) {
                dmc.shift_register = dmc.sample_buffer;
                dmc.bits_remaining = 8;
                dmc.sample_buffer_empty = true;
            }
        }

        if (dmc.bits_remaining > 0) {
            if (dmc.shift_register & 0x01) {
                if (dmc.output <= 125) dmc.output += 2;
            } else {
                if (dmc.output >= 2) dmc.output -= 2;
            }
            dmc.shift_register >>= 1;
            dmc.bits_remaining--;
        }
    } else {
        dmc.timer--;
    }
}

void APU::step()
{
    cycles++;

    // Triangle is clocked every CPU cycle
    clockTriangleTimer();

    // DMC runs every CPU cycle
    if (dmc.enabled) {
        clockDMC();
    }

    // Clock pulse/noise timers every other CPU cycle (APU runs at half CPU speed for pulse/noise)
    if (cycles % 2 == 0) {
        clockTimers();
    }
    
    // Frame counter - approximately 240Hz for quarter frames
    // 4-step: steps at ~3728.5, 7456.5, 11185.5, 14914.5 CPU cycles (then reset)
    // 5-step: steps at ~3728.5, 7456.5, 11185.5, 14914.5, 18640.5 CPU cycles
    frame_counter++;
    
    // Simplified frame counter timing
    if (frame_counter_mode == 0) {
        // 4-step mode (approx every 3728 CPU cycles)
        if (frame_counter == 3729 || frame_counter == 7457 || 
            frame_counter == 11186 || frame_counter == 14915) {
            clockEnvelopes();
            clockTriangleLinear();
        }
        if (frame_counter == 7457 || frame_counter == 14915) {
            clockLengthCounters();
            clockSweeps();
        }
        if (frame_counter >= 14915) {
            frame_counter = 0;
            if (!irq_inhibit) {
                irq_flag = true;
            }
        }
    } else {
        // 5-step mode
        if (frame_counter == 3729 || frame_counter == 7457 || 
            frame_counter == 11186 || frame_counter == 18641) {
            clockEnvelopes();
            clockTriangleLinear();
        }
        if (frame_counter == 7457 || frame_counter == 18641) {
            clockLengthCounters();
            clockSweeps();
        }
        if (frame_counter >= 18641) {
            frame_counter = 0;
        }
    }
    
    // Sample generation - output a sample at 44.1kHz rate
    sample_accumulator += 1.0f;
    if (sample_accumulator >= CYCLES_PER_SAMPLE) {
        sample_accumulator -= CYCLES_PER_SAMPLE;
        if (!muted) {
            sound.pushSample(getOutput());
        }
    }
}

u8 APU::readRegister(u16 addr)
{
    u8 data = 0;

    if (addr == 0x4015) {
        // Status register
        if (pulse[0].length_counter > 0) data |= 0x01;
        if (pulse[1].length_counter > 0) data |= 0x02;
        if (triangle.length_counter > 0) data |= 0x04;
        if (noise.length_counter > 0)    data |= 0x08;
        if (dmc.bytes_remaining > 0)     data |= 0x10;
        if (irq_flag)                    data |= 0x40;
        if (dmc.irq_flag)                data |= 0x80;
        
        irq_flag = false;  // Clear frame IRQ flag on read
    }

    return data;
}

void APU::writeRegister(u16 addr, u8 data)
{
    switch (addr) {
        // Pulse 1
        case 0x4000:
            pulse[0].duty = (data >> 6) & 0x03;
            pulse[0].length_halt = data & 0x20;
            pulse[0].constant_volume = data & 0x10;
            pulse[0].volume = data & 0x0F;
            break;
        case 0x4001:
            pulse[0].sweep_enabled = data & 0x80;
            pulse[0].sweep_period = (data >> 4) & 0x07;
            pulse[0].sweep_negate = data & 0x08;
            pulse[0].sweep_shift = data & 0x07;
            pulse[0].sweep_reload = true;
            break;
        case 0x4002:
            pulse[0].timer_period = (pulse[0].timer_period & 0x700) | data;
            break;
        case 0x4003:
            pulse[0].timer_period = (pulse[0].timer_period & 0xFF) | ((data & 0x07) << 8);
            if (pulse[0].enabled)
                pulse[0].length_counter = length_table[data >> 3];
            pulse[0].envelope_start = true;
            pulse[0].sequence_pos = 0;
            pulse[0].sweep_reload = true;
            break;

        // Pulse 2
        case 0x4004:
            pulse[1].duty = (data >> 6) & 0x03;
            pulse[1].length_halt = data & 0x20;
            pulse[1].constant_volume = data & 0x10;
            pulse[1].volume = data & 0x0F;
            break;
        case 0x4005:
            pulse[1].sweep_enabled = data & 0x80;
            pulse[1].sweep_period = (data >> 4) & 0x07;
            pulse[1].sweep_negate = data & 0x08;
            pulse[1].sweep_shift = data & 0x07;
            pulse[1].sweep_reload = true;
            break;
        case 0x4006:
            pulse[1].timer_period = (pulse[1].timer_period & 0x700) | data;
            break;
        case 0x4007:
            pulse[1].timer_period = (pulse[1].timer_period & 0xFF) | ((data & 0x07) << 8);
            if (pulse[1].enabled)
                pulse[1].length_counter = length_table[data >> 3];
            pulse[1].envelope_start = true;
            pulse[1].sequence_pos = 0;
            pulse[1].sweep_reload = true;
            break;

        // Triangle
        case 0x4008:
            triangle.control = data & 0x80;
            triangle.linear_reload = data & 0x7F;
            break;
        case 0x400A:
            triangle.timer_period = (triangle.timer_period & 0x700) | data;
            break;
        case 0x400B:
            triangle.timer_period = (triangle.timer_period & 0xFF) | ((data & 0x07) << 8);
            if (triangle.enabled)
                triangle.length_counter = length_table[data >> 3];
            triangle.linear_reload_flag = true;
            break;

        // Noise
        case 0x400C:
            noise.length_halt = data & 0x20;
            noise.constant_volume = data & 0x10;
            noise.volume = data & 0x0F;
            break;
        case 0x400E:
            noise.mode = data & 0x80;
            noise.timer_period = noise_period_table[data & 0x0F];
            break;
        case 0x400F:
            if (noise.enabled)
                noise.length_counter = length_table[data >> 3];
            noise.envelope_start = true;
            break;

        // DMC
        case 0x4010:
            dmc.irq_enable = data & 0x80;
            dmc.loop = data & 0x40;
            dmc.rate = data & 0x0F;
            if (!dmc.irq_enable) dmc.irq_flag = false;
            break;
        case 0x4011:
            dmc.output = data & 0x7F;
            break;
        case 0x4012:
            dmc.sample_addr = 0xC000 + (data << 6);
            break;
        case 0x4013:
            dmc.sample_length = (data << 4) + 1;
            break;

        // Status
        case 0x4015:
            pulse[0].enabled = data & 0x01;
            pulse[1].enabled = data & 0x02;
            triangle.enabled = data & 0x04;
            noise.enabled = data & 0x08;
            dmc.enabled = data & 0x10;

            if (!pulse[0].enabled) pulse[0].length_counter = 0;
            if (!pulse[1].enabled) pulse[1].length_counter = 0;
            if (!triangle.enabled) triangle.length_counter = 0;
            if (!noise.enabled) noise.length_counter = 0;
            if (dmc.enabled) {
                if (dmc.bytes_remaining == 0) {
                    dmc.current_addr = dmc.sample_addr;
                    dmc.bytes_remaining = dmc.sample_length;
                }
            } else {
                dmc.bytes_remaining = 0;
                dmc.sample_buffer_empty = true;
                dmc.bits_remaining = 0;
                dmc.irq_flag = false;
            }
            break;

        // Frame counter
        case 0x4017:
            frame_counter_mode = (data >> 7) & 0x01;
            irq_inhibit = data & 0x40;
            if (irq_inhibit) irq_flag = false;
            frame_counter = 0;
            if (frame_counter_mode == 1) {
                clockEnvelopes();
                clockTriangleLinear();
                clockLengthCounters();
                clockSweeps();
            }
            break;
    }
}

float APU::getOutput() const
{
    // Generate actual waveform samples
    
    // Pulse 1
    u8 pulse1_sample = 0;
    if (pulse[0].enabled && pulse[0].length_counter > 0 && pulse[0].timer_period >= 8) {
        u16 change = pulse[0].sweep_shift ? (pulse[0].timer_period >> pulse[0].sweep_shift) : 0;
        u16 target = pulse[0].sweep_negate
            ? (pulse[0].timer_period - change - 1)
            : (pulse[0].timer_period + change);
        if (!pulse[0].sweep_enabled || pulse[0].sweep_shift == 0 || target <= 0x7FF) {
            u8 duty_out = duty_table[pulse[0].duty][pulse[0].sequence_pos];
            u8 vol = pulse[0].constant_volume ? pulse[0].volume : pulse[0].envelope_volume;
            pulse1_sample = duty_out ? vol : 0;
        }
    }
    
    // Pulse 2
    u8 pulse2_sample = 0;
    if (pulse[1].enabled && pulse[1].length_counter > 0 && pulse[1].timer_period >= 8) {
        u16 change = pulse[1].sweep_shift ? (pulse[1].timer_period >> pulse[1].sweep_shift) : 0;
        u16 target = pulse[1].sweep_negate
            ? (pulse[1].timer_period - change)
            : (pulse[1].timer_period + change);
        if (!pulse[1].sweep_enabled || pulse[1].sweep_shift == 0 || target <= 0x7FF) {
            u8 duty_out = duty_table[pulse[1].duty][pulse[1].sequence_pos];
            u8 vol = pulse[1].constant_volume ? pulse[1].volume : pulse[1].envelope_volume;
            pulse2_sample = duty_out ? vol : 0;
        }
    }
    
    // Triangle
    u8 triangle_sample = 0;
    if (triangle.enabled && triangle.length_counter > 0 && triangle.linear_counter > 0 && triangle.timer_period >= 2) {
        triangle_sample = triangle_sequence[triangle.sequence_pos];
    }
    
    // Noise
    u8 noise_sample = 0;
    if (noise.enabled && noise.length_counter > 0 && !(noise.shift_register & 1)) {
        noise_sample = noise.constant_volume ? noise.volume : noise.envelope_volume;
    }
    
    // DMC
    u8 dmc_sample = dmc.output;
    
    // Mix using NES mixer formulas
    float pulse_out = 0.0f;
    float tnd_out = 0.0f;
    
    if (pulse1_sample || pulse2_sample) {
        pulse_out = 95.88f / ((8128.0f / (pulse1_sample + pulse2_sample)) + 100.0f);
    }
    
    if (triangle_sample || noise_sample || dmc_sample) {
        tnd_out = 159.79f / ((1.0f / ((triangle_sample / 8227.0f) + (noise_sample / 12241.0f) + (dmc_sample / 22638.0f))) + 100.0f);
    }
    
    // Return combined output (normalized to approximately -1.0 to 1.0)
    return (pulse_out + tnd_out) * 2.0f - 1.0f;
}
