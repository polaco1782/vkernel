#ifndef MAPPER_004_H
#define MAPPER_004_H

#include "mapper.h"

/**
 * Mapper 004 - MMC3 (Nintendo TxROM)
 * 
 * One of the most popular NES mappers, used by many games including
 * Super Mario Bros. 2/3, Mega Man 3-6, and many others.
 * 
 * Features:
 *   - PRG ROM: Up to 512KB, switchable in 8KB banks
 *   - CHR ROM/RAM: Up to 256KB, switchable in 1KB/2KB banks
 *   - PRG RAM: 8KB at $6000-$7FFF with optional write protection
 *   - Programmable mirroring (horizontal/vertical)
 *   - Scanline counter with IRQ generation
 * 
 * PRG Memory Map:
 *   $6000-$7FFF: 8KB PRG RAM (optional)
 *   $8000-$9FFF: 8KB switchable or fixed to second-to-last bank
 *   $A000-$BFFF: 8KB switchable
 *   $C000-$DFFF: 8KB switchable or fixed to second-to-last bank
 *   $E000-$FFFF: 8KB fixed to last bank
 * 
 * CHR Memory Map (depends on CHR A12 inversion bit):
 *   Mode 0: 2KB banks at $0000/$0800, 1KB banks at $1000-$1C00
 *   Mode 1: 1KB banks at $0000-$0C00, 2KB banks at $1000/$1800
 * 
 * Registers:
 *   $8000-$9FFE (even): Bank select
 *   $8001-$9FFF (odd): Bank data
 *   $A000-$BFFE (even): Mirroring
 *   $A001-$BFFF (odd): PRG RAM protect
 *   $C000-$DFFE (even): IRQ latch
 *   $C001-$DFFF (odd): IRQ reload
 *   $E000-$FFFE (even): IRQ disable (and acknowledge)
 *   $E001-$FFFF (odd): IRQ enable
 */
class Mapper004 : public Mapper {
public:
    Mapper004();
    ~Mapper004() override = default;

    void init(std::vector<u8>& prg, std::vector<u8>& chr,
              std::vector<u8>& prgRam, Mirroring initialMirroring) override;

    u8 readPrg(u16 addr) override;
    void writePrg(u16 addr, u8 data) override;

    u8 readChr(u16 addr) override;
    void writeChr(u16 addr, u8 data) override;

    // Scanline counter for IRQ
    void scanline() override;

    const char* getName() const override { return "MMC3"; }

    // IRQ status
    bool irqPending() const { return irqPendingFlag; }
    void clearIrq() { irqPendingFlag = false; }

private:
    void updatePrgBanks();
    void updateChrBanks();

    // Bank select register ($8000)
    u8 bankSelect;      // Bits 0-2: Bank register to update
                             // Bit 6: PRG ROM bank mode
                             // Bit 7: CHR A12 inversion

    // Bank data registers (selected via bankSelect bits 0-2)
    u8 bankRegisters[8];  // R0-R7 bank registers

    // PRG RAM protect ($A001)
    bool prgRamEnable;
    bool prgRamWriteProtect;

    // IRQ registers
    u8 irqLatch;        // Value to reload counter with
    u8 irqCounter;      // Current counter value
    bool irqReload;          // Flag to reload counter on next scanline
    bool irqEnabled;         // IRQ enable flag
    bool irqPendingFlag;     // IRQ pending flag

    // Computed bank offsets
    u32 prgBankOffset[4];  // Four 8KB PRG banks
    u32 chrBankOffset[8];  // Eight 1KB CHR banks
};

#endif // MAPPER_004_H
