#include "mapper_002.h"

Mapper002::Mapper002()
    : Mapper(2)
    , prgBankSelect(0)
    , prgBankOffset(0)
{
}

void Mapper002::init(std::vector<u8>& prg, std::vector<u8>& chr,
                     std::vector<u8>& ram, Mirroring initialMirroring)
{
    Mapper::init(prg, chr, ram, initialMirroring);

    // Reset register
    prgBankSelect = 0;

    // Initialize bank offset
    prgBankOffset = 0;
}

u8 Mapper002::readPrg(u16 addr)
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
        if (!prgRom || prgRom->empty()) return 0;

        if (addr < 0xC000) {
            // $8000-$BFFF: Switchable bank
            return (*prgRom)[prgBankOffset + (addr - 0x8000)];
        } else {
            // $C000-$FFFF: Fixed to last 16KB bank
            u32 lastBankOffset = static_cast<u32>(prgRom->size() - PRG_ROM_UNIT);
            return (*prgRom)[lastBankOffset + (addr - 0xC000)];
        }
    }

    return 0;
}

void Mapper002::writePrg(u16 addr, u8 data)
{
    // PRG RAM at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000) {
        if (prgRam && !prgRam->empty()) {
            (*prgRam)[addr - 0x6000] = data;
        }
        return;
    }

    // Bank select register at $8000-$FFFF
    if (addr >= 0x8000) {
        prgBankSelect = data & 0x0F;  // 4 bits for bank selection
        u32 bankCount = static_cast<u32>(prgRom->size()) / PRG_ROM_UNIT;
        prgBankOffset = (prgBankSelect % bankCount) * PRG_ROM_UNIT;
    }
}

u8 Mapper002::readChr(u16 addr)
{
    if (chrRom && !chrRom->empty() && addr < 0x2000) {
        return (*chrRom)[addr % chrRom->size()];
    }
    return 0;
}

void Mapper002::writeChr(u16 addr, u8 data)
{
    // CHR RAM is writable (when chr_rom_size == 0 in header)
    if (chrRom && !chrRom->empty() && addr < 0x2000) {
        (*chrRom)[addr % chrRom->size()] = data;
    }
}
