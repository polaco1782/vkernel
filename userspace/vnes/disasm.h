#ifndef VNES_DISASM_H
#define VNES_DISASM_H

#include "types.h"

namespace vnes::disasm {

// Addressing modes for 6502 instructions
enum AddrMode {
    IMP,  // Implied
    ACC,  // Accumulator
    IMM,  // Immediate
    ZP,   // Zero Page
    ZPX,  // Zero Page,X
    ZPY,  // Zero Page,Y
    ABS,  // Absolute
    ABX,  // Absolute,X
    ABY,  // Absolute,Y
    IND,  // Indirect
    IZX,  // Indexed Indirect (,X)
    IZY,  // Indirect Indexed (,Y)
    REL   // Relative
};

// Opcode names for all 256 6502 instructions
[[maybe_unused]] static const char* opcodeNames[256] = {
    "BRK","ORA","???","???","???","ORA","ASL","???","PHP","ORA","ASL","???","???","ORA","ASL","???",
    "BPL","ORA","???","???","???","ORA","ASL","???","CLC","ORA","???","???","???","ORA","ASL","???",
    "JSR","AND","???","???","BIT","AND","ROL","???","PLP","AND","ROL","???","BIT","AND","ROL","???",
    "BMI","AND","???","???","???","AND","ROL","???","SEC","AND","???","???","???","AND","ROL","???",
    "RTI","EOR","???","???","???","EOR","LSR","???","PHA","EOR","LSR","???","JMP","EOR","LSR","???",
    "BVC","EOR","???","???","???","EOR","LSR","???","CLI","EOR","???","???","???","EOR","LSR","???",
    "RTS","ADC","???","???","???","ADC","ROR","???","PLA","ADC","ROR","???","JMP","ADC","ROR","???",
    "BVS","ADC","???","???","???","ADC","ROR","???","SEI","ADC","???","???","???","ADC","ROR","???",
    "???","STA","???","???","STY","STA","STX","???","DEY","???","TXA","???","STY","STA","STX","???",
    "BCC","STA","???","???","STY","STA","STX","???","TYA","STA","TXS","???","???","STA","???","???",
    "LDY","LDA","LDX","???","LDY","LDA","LDX","???","TAY","LDA","TAX","???","LDY","LDA","LDX","???",
    "BCS","LDA","???","???","LDY","LDA","LDX","???","CLV","LDA","TSX","???","LDY","LDA","LDX","???",
    "CPY","CMP","???","???","CPY","CMP","DEC","???","INY","CMP","DEX","???","CPY","CMP","DEC","???",
    "BNE","CMP","???","???","???","CMP","DEC","???","CLD","CMP","???","???","???","CMP","DEC","???",
    "CPX","SBC","???","???","CPX","SBC","INC","???","INX","SBC","NOP","???","CPX","SBC","INC","???",
    "BEQ","SBC","???","???","???","SBC","INC","???","SED","SBC","???","???","???","SBC","INC","???"
};

// Addressing modes indexed by opcode
[[maybe_unused]] static const AddrMode addrModes[256] = {
    IMP,IZX,IMP,IMP,IMP,ZP ,ZP ,IMP,IMP,IMM,ACC,IMP,IMP,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,IMP,ZPX,ZPX,IMP,IMP,ABY,IMP,IMP,IMP,ABX,ABX,IMP,
    ABS,IZX,IMP,IMP,ZP ,ZP ,ZP ,IMP,IMP,IMM,ACC,IMP,ABS,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,IMP,ZPX,ZPX,IMP,IMP,ABY,IMP,IMP,IMP,ABX,ABX,IMP,
    IMP,IZX,IMP,IMP,IMP,ZP ,ZP ,IMP,IMP,IMM,ACC,IMP,ABS,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,IMP,ZPX,ZPX,IMP,IMP,ABY,IMP,IMP,IMP,ABX,ABX,IMP,
    IMP,IZX,IMP,IMP,IMP,ZP ,ZP ,IMP,IMP,IMM,ACC,IMP,IND,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,IMP,ZPX,ZPX,IMP,IMP,ABY,IMP,IMP,IMP,ABX,ABX,IMP,
    IMP,IZX,IMP,IMP,ZP ,ZP ,ZP ,IMP,IMP,IMP,IMP,IMP,ABS,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,ZPX,ZPX,ZPY,IMP,IMP,ABY,IMP,IMP,IMP,ABX,IMP,IMP,
    IMM,IZX,IMM,IMP,ZP ,ZP ,ZP ,IMP,IMP,IMM,IMP,IMP,ABS,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,ZPX,ZPX,ZPY,IMP,IMP,ABY,IMP,IMP,ABX,ABX,ABY,IMP,
    IMM,IZX,IMP,IMP,ZP ,ZP ,ZP ,IMP,IMP,IMM,IMP,IMP,ABS,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,IMP,ZPX,ZPX,IMP,IMP,ABY,IMP,IMP,IMP,ABX,ABX,IMP,
    IMM,IZX,IMP,IMP,ZP ,ZP ,ZP ,IMP,IMP,IMM,IMP,IMP,ABS,ABS,ABS,IMP,
    REL,IZY,IMP,IMP,IMP,ZPX,ZPX,IMP,IMP,ABY,IMP,IMP,IMP,ABX,ABX,IMP
};

// Instruction lengths by addressing mode
[[maybe_unused]] static const int modeLengths[13] = {
    1,  // IMP
    1,  // ACC
    2,  // IMM
    2,  // ZP
    2,  // ZPX
    2,  // ZPY
    3,  // ABS
    3,  // ABX
    3,  // ABY
    3,  // IND
    2,  // IZX
    2,  // IZY
    2   // REL
};

inline auto getMemoryRegion(u16 addr) -> const char*
{
    if (addr < 0x2000) return "RAM";
    if (addr < 0x4000) return "PPU";
    if (addr < 0x4018) return "APU/IO";
    if (addr < 0x6000) return "EXP";
    if (addr < 0x8000) return "SRAM";
    return "ROM";
}

inline auto getHardwareSymbol(u16 addr) -> const char*
{
    switch (addr) {
        // PPU Registers
        case 0x2000: return "PPU_CTRL";
        case 0x2001: return "PPU_MASK";
        case 0x2002: return "PPU_STATUS";
        case 0x2003: return "OAM_ADDR";
        case 0x2004: return "OAM_DATA";
        case 0x2005: return "PPU_SCROLL";
        case 0x2006: return "PPU_ADDR";
        case 0x2007: return "PPU_DATA";
        
        // APU Pulse 1
        case 0x4000: return "APU_PULSE1_CTRL";
        case 0x4001: return "APU_PULSE1_SWEEP";
        case 0x4002: return "APU_PULSE1_TIMER_LO";
        case 0x4003: return "APU_PULSE1_TIMER_HI";
        
        // APU Pulse 2
        case 0x4004: return "APU_PULSE2_CTRL";
        case 0x4005: return "APU_PULSE2_SWEEP";
        case 0x4006: return "APU_PULSE2_TIMER_LO";
        case 0x4007: return "APU_PULSE2_TIMER_HI";
        
        // APU Triangle
        case 0x4008: return "APU_TRIANGLE_CTRL";
        case 0x400A: return "APU_TRIANGLE_TIMER_LO";
        case 0x400B: return "APU_TRIANGLE_TIMER_HI";
        
        // APU Noise
        case 0x400C: return "APU_NOISE_CTRL";
        case 0x400E: return "APU_NOISE_PERIOD";
        case 0x400F: return "APU_NOISE_LENGTH";
        
        // APU DMC
        case 0x4010: return "APU_DMC_CTRL";
        case 0x4011: return "APU_DMC_OUTPUT";
        case 0x4012: return "APU_DMC_ADDR";
        case 0x4013: return "APU_DMC_LENGTH";
        
        // Other I/O
        case 0x4014: return "OAM_DMA";
        case 0x4015: return "APU_STATUS";
        case 0x4016: return "JOY1";
        case 0x4017: return "JOY2_FRAME_COUNTER";
        
        default: return "";
    }
}

// NES color palette (2C02 NTSC) for GUI use
static const u32 nesPalette[64] = {
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00,
    0x333500, 0x0B4800, 0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000,
    0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00,
    0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000,
    0xFFFEFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
    0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000,
    0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5,
    0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000
};

} // namespace vnes::disasm

#endif // VNES_DISASM_H
