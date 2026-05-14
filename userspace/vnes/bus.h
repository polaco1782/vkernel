#ifndef BUS_H
#define BUS_H

#include "types.h"
#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "cartridge.h"
#include "input.h"

#include <string>

class Bus {
public:
    Bus();

    bool loadCartridge(const std::string& filepath);
    void reset();
    void clock();

    u8 read(u16 addr);
    void write(u16 addr, u8 data);

    CPU cpu;
    PPU ppu;
    APU apu;
    Cartridge cartridge;
    Input input;

private:
    u8 ram[2048];
    u64 system_cycles;
};

#endif // BUS_H