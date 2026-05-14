#include "bus.h"

#include <string.h>

Bus::Bus()
    : cpu(*this)
    , ppu(*this, cartridge)
    , apu(*this)
    , system_cycles(0)
{
    memset(ram, 0, sizeof(ram));
}

bool Bus::loadCartridge(const std::string& filepath)
{
    return cartridge.load(filepath);
}

void Bus::reset()
{
    cpu.reset();
    ppu.reset();
    apu.reset();
    system_cycles = 0;
    input.clearButtons();
}

void Bus::clock()
{
    ppu.step();

    if ((system_cycles % 3U) == 0U) {
        cpu.step();
        apu.step();
    }

    if (ppu.isNMI()) {
        ppu.clearNMI();
        cpu.nmi();
    }

    if (apu.isIRQ()) {
        apu.clearIRQ();
        cpu.irq();
    }

    if (cartridge.hasIRQ()) {
        cartridge.clearIRQ();
        cpu.irq();
    }

    ++system_cycles;
}

u8 Bus::read(u16 addr)
{
    if (addr < 0x2000U) {
        return ram[addr & 0x07FFU];
    }

    if (addr < 0x4000U) {
        return ppu.readRegister(addr);
    }

    if (addr < 0x4018U) {
        if (addr == 0x4016U) {
            return input.read();
        }
        if (addr == 0x4017U) {
            return 0x40U;
        }
        return apu.readRegister(addr);
    }

    if (addr >= 0x4020U) {
        return cartridge.readPrg(addr);
    }

    return 0;
}

void Bus::write(u16 addr, u8 data)
{
    if (addr < 0x2000U) {
        ram[addr & 0x07FFU] = data;
        return;
    }

    if (addr < 0x4000U) {
        ppu.writeRegister(addr, data);
        return;
    }

    if (addr < 0x4018U) {
        if (addr == 0x4014U) {
            ppu.writeDMA(data);
            return;
        }

        if (addr == 0x4016U) {
            if ((data & 0x01U) != 0U) {
                input.strobe();
            }
            return;
        }

        apu.writeRegister(addr, data);
        return;
    }

    if (addr >= 0x4020U) {
        cartridge.writePrg(addr, data);
    }
}