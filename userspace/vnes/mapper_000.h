#ifndef MAPPER_000_H
#define MAPPER_000_H

#include "mapper.h"

/**
 * Mapper 000 - NROM
 * 
 * The simplest NES mapper with no bank switching.
 * 
 * PRG ROM: 16KB or 32KB (mirrored if 16KB)
 * CHR ROM: 8KB (can be CHR RAM)
 * 
 * CPU Memory Map:
 *   $6000-$7FFF: PRG RAM (if present)
 *   $8000-$BFFF: First 16KB of PRG ROM
 *   $C000-$FFFF: Last 16KB of PRG ROM (or mirror of $8000 for 16KB ROMs)
 * 
 * PPU Memory Map:
 *   $0000-$1FFF: 8KB CHR ROM/RAM
 */
class Mapper000 : public Mapper {
public:
    Mapper000();
    ~Mapper000() override = default;

    u8 readPrg(u16 addr) override;
    void writePrg(u16 addr, u8 data) override;

    u8 readChr(u16 addr) override;
    void writeChr(u16 addr, u8 data) override;

    const char* getName() const override { return "NROM"; }
};

#endif // MAPPER_000_H
