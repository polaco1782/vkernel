#ifndef MAPPER_002_H
#define MAPPER_002_H

#include "mapper.h"

/**
 * Mapper 002 - UxROM
 * 
 * Used by games like Mega Man, Castlevania, Duck Tales, and many others.
 * 
 * Features:
 *   - PRG ROM: Up to 256KB, switchable in 16KB banks
 *   - CHR ROM: 8KB fixed (can be CHR RAM if size=0)
 *   - PRG RAM: 8KB at $6000-$7FFF (optional)
 *   - Fixed mirroring (horizontal or vertical from ROM header)
 * 
 * PRG Memory Map:
 *   $6000-$7FFF: 8KB PRG RAM (optional)
 *   $8000-$BFFF: 16KB switchable PRG ROM bank
 *   $C000-$FFFF: 16KB fixed to last bank of PRG ROM
 * 
 * CHR Memory Map:
 *   $0000-$1FFF: 8KB fixed CHR ROM/RAM
 * 
 * Registers:
 *   $8000-$FFFF: Bank select (writes to any address in this range)
 *                Bits 0-3: Select 16KB PRG bank at $8000-$BFFF
 */
class Mapper002 : public Mapper {
public:
    Mapper002();
    ~Mapper002() override = default;

    void init(std::vector<u8>& prg, std::vector<u8>& chr,
              std::vector<u8>& prgRam, Mirroring initialMirroring) override;

    u8 readPrg(u16 addr) override;
    void writePrg(u16 addr, u8 data) override;

    u8 readChr(u16 addr) override;
    void writeChr(u16 addr, u8 data) override;

    const char* getName() const override { return "UxROM"; }

private:
    // PRG bank register
    u8 prgBankSelect;

    // Computed bank offsets
    u32 prgBankOffset;  // Switchable 16KB bank at $8000
};

#endif // MAPPER_002_H
