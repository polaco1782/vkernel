#include "mapper_004.h"

Mapper004::Mapper004()
    : Mapper(4)
    , bankSelect(0)
    , prgRamEnable(true)
    , prgRamWriteProtect(false)
    , irqLatch(0)
    , irqCounter(0)
    , irqReload(false)
    , irqEnabled(false)
    , irqPendingFlag(false)
{
    for (int i = 0; i < 8; i++) {
        bankRegisters[i] = 0;
    }
    for (int i = 0; i < 4; i++) {
        prgBankOffset[i] = 0;
    }
    for (int i = 0; i < 8; i++) {
        chrBankOffset[i] = 0;
    }
}

void Mapper004::init(std::vector<u8>& prg, std::vector<u8>& chr,
                     std::vector<u8>& ram, Mirroring initialMirroring)
{
    Mapper::init(prg, chr, ram, initialMirroring);

    // Reset registers
    bankSelect = 0;
    for (int i = 0; i < 8; i++) {
        bankRegisters[i] = 0;
    }
    prgRamEnable = true;
    prgRamWriteProtect = false;
    irqLatch = 0;
    irqCounter = 0;
    irqReload = false;
    irqEnabled = false;
    irqPendingFlag = false;

    // Initialize banks
    updatePrgBanks();
    updateChrBanks();
}

void Mapper004::updatePrgBanks()
{
    u32 prgBankCount = static_cast<u32>(prgRom->size() / PRG_BANK_8K);
    u32 lastBank = prgBankCount - 1;
    u32 secondToLast = prgBankCount - 2;

    // PRG bank mode (bit 6 of bank select)
    if (bankSelect & 0x40) {
        // Mode 1: $C000 swappable, $8000 fixed to second-to-last
        prgBankOffset[0] = (secondToLast % prgBankCount) * PRG_BANK_8K;
        prgBankOffset[1] = (bankRegisters[7] % prgBankCount) * PRG_BANK_8K;
        prgBankOffset[2] = (bankRegisters[6] % prgBankCount) * PRG_BANK_8K;
        prgBankOffset[3] = (lastBank % prgBankCount) * PRG_BANK_8K;
    } else {
        // Mode 0: $8000 swappable, $C000 fixed to second-to-last
        prgBankOffset[0] = (bankRegisters[6] % prgBankCount) * PRG_BANK_8K;
        prgBankOffset[1] = (bankRegisters[7] % prgBankCount) * PRG_BANK_8K;
        prgBankOffset[2] = (secondToLast % prgBankCount) * PRG_BANK_8K;
        prgBankOffset[3] = (lastBank % prgBankCount) * PRG_BANK_8K;
    }
}

void Mapper004::updateChrBanks()
{
    if (!chrRom || chrRom->empty()) return;

    u32 chrBankCount = static_cast<u32>(chrRom->size() / CHR_BANK_1K);
    if (chrBankCount == 0) return;

    // CHR A12 inversion (bit 7 of bank select)
    if (bankSelect & 0x80) {
        // Mode 1: 1KB banks at $0000-$0FFF, 2KB banks at $1000-$1FFF
        chrBankOffset[0] = (bankRegisters[2] % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[1] = (bankRegisters[3] % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[2] = (bankRegisters[4] % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[3] = (bankRegisters[5] % chrBankCount) * CHR_BANK_1K;
        // 2KB banks (ignore low bit)
        u8 r0 = bankRegisters[0] & 0xFE;
        u8 r1 = bankRegisters[1] & 0xFE;
        chrBankOffset[4] = (r0 % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[5] = ((r0 + 1) % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[6] = (r1 % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[7] = ((r1 + 1) % chrBankCount) * CHR_BANK_1K;
    } else {
        // Mode 0: 2KB banks at $0000-$0FFF, 1KB banks at $1000-$1FFF
        // 2KB banks (ignore low bit)
        u8 r0 = bankRegisters[0] & 0xFE;
        u8 r1 = bankRegisters[1] & 0xFE;
        chrBankOffset[0] = (r0 % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[1] = ((r0 + 1) % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[2] = (r1 % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[3] = ((r1 + 1) % chrBankCount) * CHR_BANK_1K;
        // 1KB banks
        chrBankOffset[4] = (bankRegisters[2] % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[5] = (bankRegisters[3] % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[6] = (bankRegisters[4] % chrBankCount) * CHR_BANK_1K;
        chrBankOffset[7] = (bankRegisters[5] % chrBankCount) * CHR_BANK_1K;
    }
}

u8 Mapper004::readPrg(u16 addr)
{
    // PRG RAM at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000) {
        if (prgRam && !prgRam->empty() && prgRamEnable) {
            return (*prgRam)[addr - 0x6000];
        }
        return 0;
    }

    // PRG ROM at $8000-$FFFF
    if (addr >= 0x8000) {
        if (!prgRom || prgRom->empty()) return 0;

        int bank = (addr - 0x8000) / PRG_BANK_8K;
        u32 offset = (addr - 0x8000) % PRG_BANK_8K;
        return (*prgRom)[(prgBankOffset[bank] + offset) % prgRom->size()];
    }

    return 0;
}

void Mapper004::writePrg(u16 addr, u8 data)
{
    // PRG RAM at $6000-$7FFF
    if (addr >= 0x6000 && addr < 0x8000) {
        if (prgRam && !prgRam->empty() && prgRamEnable && !prgRamWriteProtect) {
            (*prgRam)[addr - 0x6000] = data;
        }
        return;
    }

    // Register writes at $8000-$FFFF
    if (addr >= 0x8000) {
        bool isEven = (addr & 0x0001) == 0;

        if (addr < 0xA000) {
            // $8000-$9FFF
            if (isEven) {
                // Bank select ($8000, $8002, etc.)
                bankSelect = data;
                updatePrgBanks();
                updateChrBanks();
            } else {
                // Bank data ($8001, $8003, etc.)
                u8 reg = bankSelect & 0x07;
                bankRegisters[reg] = data;
                if (reg < 6) {
                    updateChrBanks();
                } else {
                    updatePrgBanks();
                }
            }
        }
        else if (addr < 0xC000) {
            // $A000-$BFFF
            if (isEven) {
                // Mirroring ($A000, $A002, etc.)
                mirroring = (data & 0x01) ? Mirroring::HORIZONTAL : Mirroring::VERTICAL;
            } else {
                // PRG RAM protect ($A001, $A003, etc.)
                prgRamWriteProtect = (data & 0x40) != 0;
                prgRamEnable = (data & 0x80) != 0;
            }
        }
        else if (addr < 0xE000) {
            // $C000-$DFFF
            if (isEven) {
                // IRQ latch ($C000, $C002, etc.)
                irqLatch = data;
            } else {
                // IRQ reload ($C001, $C003, etc.)
                irqCounter = 0;
                irqReload = true;
            }
        }
        else {
            // $E000-$FFFF
            if (isEven) {
                // IRQ disable ($E000, $E002, etc.)
                irqEnabled = false;
                irqPendingFlag = false;
            } else {
                // IRQ enable ($E001, $E003, etc.)
                irqEnabled = true;
            }
        }
    }
}

void Mapper004::scanline()
{
    // Called at the end of each visible scanline (when A12 rises)
    if (irqCounter == 0 || irqReload) {
        irqCounter = irqLatch;
        irqReload = false;
    } else {
        irqCounter--;
    }

    if (irqCounter == 0 && irqEnabled) {
        irqPendingFlag = true;
    }
}

u8 Mapper004::readChr(u16 addr)
{
    if (!chrRom || chrRom->empty()) return 0;

    // Each 1KB bank
    int bank = addr / CHR_BANK_1K;
    u32 offset = addr % CHR_BANK_1K;
    return (*chrRom)[(chrBankOffset[bank] + offset) % chrRom->size()];
}

void Mapper004::writeChr(u16 addr, u8 data)
{
    // CHR RAM is writable
    if (!chrRom || chrRom->empty()) return;

    int bank = addr / CHR_BANK_1K;
    u32 offset = addr % CHR_BANK_1K;
    u32 mappedAddr = (chrBankOffset[bank] + offset) % chrRom->size();
    (*chrRom)[mappedAddr] = data;
}
