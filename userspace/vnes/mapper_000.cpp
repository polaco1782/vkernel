#include "mapper_000.h"

Mapper000::Mapper000()
    : Mapper(0)
{
}

u8 Mapper000::readPrg(u16 addr)
{
    // PRG RAM at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000) {
        if (prgRam && !prgRam->empty()) {
            return (*prgRam)[addr - 0x6000];
        }
        return 0;
    }

    // PRG ROM at $8000-$FFFF
    if (addr >= 0x8000) {
        if (prgRom && !prgRom->empty()) {
            // Simple mirroring for 16KB/32KB ROMs
            u32 mappedAddr = (addr - 0x8000) % prgRom->size();
            return (*prgRom)[mappedAddr];
        }
    }

    return 0;
}

void Mapper000::writePrg(u16 addr, u8 data)
{
    // PRG RAM at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000) {
        if (prgRam && !prgRam->empty()) {
            (*prgRam)[addr - 0x6000] = data;
        }
    }
    // PRG ROM writes are ignored for mapper 0
}

u8 Mapper000::readChr(u16 addr)
{
    if (chrRom && !chrRom->empty() && addr < 0x2000) {
        return (*chrRom)[addr % chrRom->size()];
    }
    return 0;
}

void Mapper000::writeChr(u16 addr, u8 data)
{
    // CHR RAM is writable (when chr_rom_size == 0 in header)
    if (chrRom && !chrRom->empty() && addr < 0x2000) {
        (*chrRom)[addr % chrRom->size()] = data;
    }
}
