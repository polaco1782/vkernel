#include "mapper_001.h"

Mapper001::Mapper001()
    : Mapper(1)
    , shiftReg(0x10)
    , shiftCount(0)
    , ctrlReg(0x0C)      // Default: 16KB PRG mode, fix last bank
    , chrBank0(0)
    , chrBank1(0)
    , prgBank(0)
{
    prgBankOffset[0] = 0;
    prgBankOffset[1] = 0;
    chrBankOffset[0] = 0;
    chrBankOffset[1] = 0;
}

void Mapper001::init(std::vector<u8>& prg, std::vector<u8>& chr,
                     std::vector<u8>& ram, Mirroring initialMirroring)
{
    Mapper::init(prg, chr, ram, initialMirroring);

    // Reset state
    shiftReg = 0x10;
    shiftCount = 0;
    ctrlReg = 0x0C;
    chrBank0 = 0;
    chrBank1 = 0;
    prgBank = 0;

    // MMC1 defaults: first 16KB at $8000, last 16KB at $C000
    prgBankOffset[0] = 0;
    prgBankOffset[1] = static_cast<u32>(prgRom->size() - PRG_ROM_UNIT);
    chrBankOffset[0] = 0;
    chrBankOffset[1] = CHR_BANK_4K;
}

u8 Mapper001::readPrg(u16 addr)
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
            return (*prgRom)[prgBankOffset[0] + (addr - 0x8000)];
        } else {
            return (*prgRom)[prgBankOffset[1] + (addr - 0xC000)];
        }
    }

    return 0;
}

void Mapper001::writePrg(u16 addr, u8 data)
{
    // PRG RAM at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000) {
        if (prgRam && !prgRam->empty()) {
            (*prgRam)[addr - 0x6000] = data;
        }
        return;
    }

    // Register writes at $8000-$FFFF
    if (addr >= 0x8000) {
        writeRegister(addr, data);
    }
}

void Mapper001::writeRegister(u16 addr, u8 data)
{
    // Bit 7 set = reset shift register
    if (data & 0x80) {
        shiftReg = 0x10;
        shiftCount = 0;
        ctrlReg |= 0x0C;  // Set PRG mode to 3 (fix last bank)
        updateBanks();
        return;
    }

    // Shift in bit 0
    shiftReg = (shiftReg >> 1) | ((data & 0x01) << 4);
    shiftCount++;

    // After 5 writes, transfer to internal register
    if (shiftCount == 5) {
        u8 value = shiftReg;

        // Select register based on address
        if (addr < 0xA000) {
            // $8000-$9FFF: Control register
            ctrlReg = value;

            // Update mirroring based on bits 0-1
            switch (value & 0x03) {
                case 0: mirroring = Mirroring::SINGLE_LOWER; break;
                case 1: mirroring = Mirroring::SINGLE_UPPER; break;
                case 2: mirroring = Mirroring::VERTICAL; break;
                case 3: mirroring = Mirroring::HORIZONTAL; break;
            }
        }
        else if (addr < 0xC000) {
            // $A000-$BFFF: CHR bank 0
            chrBank0 = value;
        }
        else if (addr < 0xE000) {
            // $C000-$DFFF: CHR bank 1
            chrBank1 = value;
        }
        else {
            // $E000-$FFFF: PRG bank
            prgBank = value & 0x0F;
        }

        updateBanks();

        // Reset shift register
        shiftReg = 0x10;
        shiftCount = 0;
    }
}

void Mapper001::updateBanks()
{
    u32 prgBankCount = static_cast<u32>(prgRom->size() / PRG_ROM_UNIT);
    u32 chrBankCount = static_cast<u32>(chrRom->size() / CHR_BANK_4K);

    // PRG ROM bank mode (bits 2-3 of control register)
    u8 prgMode = (ctrlReg >> 2) & 0x03;

    switch (prgMode) {
        case 0:
        case 1:
            // 32KB mode: switch both banks together, ignore low bit
            {
                u32 bank = (prgBank & 0x0E) % prgBankCount;
                prgBankOffset[0] = bank * PRG_ROM_UNIT;
                prgBankOffset[1] = (bank + 1) * PRG_ROM_UNIT;
            }
            break;
        case 2:
            // Fix first bank at $8000, switch $C000
            prgBankOffset[0] = 0;
            prgBankOffset[1] = (prgBank % prgBankCount) * PRG_ROM_UNIT;
            break;
        case 3:
            // Fix last bank at $C000, switch $8000
            prgBankOffset[0] = (prgBank % prgBankCount) * PRG_ROM_UNIT;
            prgBankOffset[1] = (prgBankCount - 1) * PRG_ROM_UNIT;
            break;
    }

    // CHR ROM bank mode (bit 4 of control register)
    if (chrBankCount == 0) return;  // No CHR to bank

    if (ctrlReg & 0x10) {
        // 4KB mode: switch two separate 4KB banks
        chrBankOffset[0] = (chrBank0 % chrBankCount) * CHR_BANK_4K;
        chrBankOffset[1] = (chrBank1 % chrBankCount) * CHR_BANK_4K;
    } else {
        // 8KB mode: switch 8KB at a time, ignore low bit of bank 0
        u32 bank = (chrBank0 & 0x1E) % chrBankCount;
        chrBankOffset[0] = bank * CHR_BANK_4K;
        chrBankOffset[1] = (bank + 1) * CHR_BANK_4K;
    }
}

u8 Mapper001::readChr(u16 addr)
{
    if (!chrRom || chrRom->empty()) return 0;

    // Two 4KB banks
    if (addr < 0x1000) {
        return (*chrRom)[(chrBankOffset[0] + addr) % chrRom->size()];
    } else {
        return (*chrRom)[(chrBankOffset[1] + (addr - 0x1000)) % chrRom->size()];
    }
}

void Mapper001::writeChr(u16 addr, u8 data)
{
    // CHR RAM is writable
    if (!chrRom || chrRom->empty()) return;

    if (addr < 0x1000) {
        u32 mappedAddr = (chrBankOffset[0] + addr) % chrRom->size();
        (*chrRom)[mappedAddr] = data;
    } else {
        u32 mappedAddr = (chrBankOffset[1] + (addr - 0x1000)) % chrRom->size();
        (*chrRom)[mappedAddr] = data;
    }
}
