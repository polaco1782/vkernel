#ifndef MAPPER_001_H
#define MAPPER_001_H

#include "mapper.h"

/**
 * Mapper 001 - MMC1 (Nintendo SxROM)
 * 
 * One of the most common NES mappers, used by many games including
 * The Legend of Zelda, Metroid, and many others.
 * 
 * Features:
 *   - PRG ROM: Up to 512KB, switchable in 16KB or 32KB banks
 *   - CHR ROM/RAM: Up to 128KB, switchable in 4KB or 8KB banks
 *   - PRG RAM: 8KB at $6000-$7FFF with optional battery backup
 *   - Programmable mirroring
 * 
 * Registers are written serially, 5 bits at a time via a shift register.
 * Writing with bit 7 set resets the shift register.
 * 
 * Register Map:
 *   $8000-$9FFF: Control (mirroring, PRG/CHR mode)
 *   $A000-$BFFF: CHR bank 0
 *   $C000-$DFFF: CHR bank 1
 *   $E000-$FFFF: PRG bank
 */
class Mapper001 : public Mapper {
public:
    Mapper001();

    void init(std::vector<u8>& prg, std::vector<u8>& chr,
              std::vector<u8>& prgRam, Mirroring initialMirroring) override;

    u8 readPrg(u16 addr) override;
    void writePrg(u16 addr, u8 data) override;

    u8 readChr(u16 addr) override;
    void writeChr(u16 addr, u8 data) override;

    const char* getName() const override { return "MMC1"; }

private:
    void writeRegister(u16 addr, u8 data);
    void updateBanks();

    // Shift register
    u8 shiftReg;
    u8 shiftCount;

    // Internal registers
    u8 ctrlReg;      // Control: mirroring, PRG/CHR modes
    u8 chrBank0;     // CHR bank 0 select
    u8 chrBank1;     // CHR bank 1 select
    u8 prgBank;      // PRG bank select

    // Computed bank offsets
    u32 prgBankOffset[2];  // Two 16KB PRG banks
    u32 chrBankOffset[2];  // Two 4KB CHR banks
};

#endif // MAPPER_001_H
