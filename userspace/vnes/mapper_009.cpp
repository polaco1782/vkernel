#include "mapper_009.h"

Mapper009::Mapper009()
    : Mapper(9)
    , prgBankSelect(0)
    , chrBank0FD(0)
    , chrBank0FE(0)
    , chrBank1FD(0)
    , chrBank1FE(0)
    , latch0(true)   // Start with $FE selected
    , latch1(true)   // Start with $FE selected
    , prgBankOffset(0)
{
    chrBankOffset[0] = 0;
    chrBankOffset[1] = 0;
}

void Mapper009::init(std::vector<u8>& prg, std::vector<u8>& chr,
                     std::vector<u8>& ram, Mirroring initialMirroring)
{
    Mapper::init(prg, chr, ram, initialMirroring);

    // Reset registers
    prgBankSelect = 0;
    chrBank0FD = 0;
    chrBank0FE = 0;
    chrBank1FD = 0;
    chrBank1FE = 0;
    latch0 = true;
    latch1 = true;

    // Initialize PRG bank offset
    prgBankOffset = 0;

    // Initialize CHR banks
    updateChrBanks();
}

u8 Mapper009::readPrg(u16 addr)
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

        u32 prgSize = static_cast<u32>(prgRom->size());
        u32 bankCount = prgSize / PRG_BANK_8K;

        if (addr < 0xA000) {
            // $8000-$9FFF: Switchable bank
            return (*prgRom)[prgBankOffset + (addr - 0x8000)];
        }
        else if (addr < 0xC000) {
            // $A000-$BFFF: Fixed to second-to-last bank
            u32 offset = (bankCount - 3) * PRG_BANK_8K;
            return (*prgRom)[offset + (addr - 0xA000)];
        }
        else if (addr < 0xE000) {
            // $C000-$DFFF: Fixed to third-to-last bank
            u32 offset = (bankCount - 2) * PRG_BANK_8K;
            return (*prgRom)[offset + (addr - 0xC000)];
        }
        else {
            // $E000-$FFFF: Fixed to last bank
            u32 offset = (bankCount - 1) * PRG_BANK_8K;
            return (*prgRom)[offset + (addr - 0xE000)];
        }
    }

    return 0;
}

void Mapper009::writePrg(u16 addr, u8 data)
{
    // PRG RAM at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000) {
        if (prgRam && !prgRam->empty()) {
            (*prgRam)[addr - 0x6000] = data;
        }
        return;
    }

    // Register writes at $A000-$FFFF
    if (addr >= 0xA000 && addr < 0xB000) {
        // $A000-$AFFF: PRG bank select
        prgBankSelect = data & 0x0F;
        u32 bankCount = static_cast<u32>(prgRom->size() / PRG_BANK_8K);
        prgBankOffset = (prgBankSelect % bankCount) * PRG_BANK_8K;
    }
    else if (addr >= 0xB000 && addr < 0xC000) {
        // $B000-$BFFF: CHR bank 0 select ($FD latch)
        chrBank0FD = data & 0x1F;
        updateChrBanks();
    }
    else if (addr >= 0xC000 && addr < 0xD000) {
        // $C000-$CFFF: CHR bank 0 select ($FE latch)
        chrBank0FE = data & 0x1F;
        updateChrBanks();
    }
    else if (addr >= 0xD000 && addr < 0xE000) {
        // $D000-$DFFF: CHR bank 1 select ($FD latch)
        chrBank1FD = data & 0x1F;
        updateChrBanks();
    }
    else if (addr >= 0xE000 && addr < 0xF000) {
        // $E000-$EFFF: CHR bank 1 select ($FE latch)
        chrBank1FE = data & 0x1F;
        updateChrBanks();
    }
    else if (addr >= 0xF000) {
        // $F000-$FFFF: Mirroring control
        mirroring = (data & 0x01) ? Mirroring::HORIZONTAL : Mirroring::VERTICAL;
    }
}

void Mapper009::updateChrBanks()
{
    if (!chrRom || chrRom->empty()) return;

    u32 chrBankCount = static_cast<u32>(chrRom->size() / CHR_BANK_4K);
    if (chrBankCount == 0) return;

    // Select CHR bank 0 based on latch 0
    u8 bank0 = latch0 ? chrBank0FE : chrBank0FD;
    chrBankOffset[0] = (bank0 % chrBankCount) * CHR_BANK_4K;

    // Select CHR bank 1 based on latch 1
    u8 bank1 = latch1 ? chrBank1FE : chrBank1FD;
    chrBankOffset[1] = (bank1 % chrBankCount) * CHR_BANK_4K;
}

u8 Mapper009::readChr(u16 addr)
{
    if (!chrRom || chrRom->empty()) return 0;

    u8 data;

    if (addr < 0x1000) {
        // CHR bank 0: $0000-$0FFF
        data = (*chrRom)[(chrBankOffset[0] + addr) % chrRom->size()];

        // Check for latch trigger tiles
        // The latch changes AFTER the data is read
        u16 tileAddr = addr & 0x0FF8;  // Tile address (ignore fine Y)
        if (tileAddr == 0x0FD8) {
            latch0 = false;  // $FD tile
            updateChrBanks();
        }
        else if (tileAddr == 0x0FE8) {
            latch0 = true;   // $FE tile
            updateChrBanks();
        }
    }
    else {
        // CHR bank 1: $1000-$1FFF
        data = (*chrRom)[(chrBankOffset[1] + (addr - 0x1000)) % chrRom->size()];

        // Check for latch trigger tiles
        u16 tileAddr = addr & 0x0FF8;
        if (tileAddr == 0x1FD8) {
            latch1 = false;  // $FD tile
            updateChrBanks();
        }
        else if (tileAddr == 0x1FE8) {
            latch1 = true;   // $FE tile
            updateChrBanks();
        }
    }

    return data;
}

void Mapper009::writeChr(u16 addr, u8 data)
{
    // MMC2 uses CHR ROM, which is not writable
    // But we include this for completeness in case of CHR RAM variants
    (void)addr;
    (void)data;
}
