#ifndef PPU_H
#define PPU_H

#include "types.h"

// Forward declarations
class Bus;
class Cartridge;

// Screen dimensions
static const int NES_WIDTH = 256;
static const int NES_HEIGHT = 240;

class PPU {
public:
    explicit PPU(Bus& bus, Cartridge& cart);
    void reset();
    void step();

    // CPU interface (memory-mapped registers $2000-$2007)
    u8 readRegister(u16 addr);
    void writeRegister(u16 addr, u8 data);

    // OAM DMA
    void writeDMA(u8 data);

    // State
    bool isFrameComplete() const { return frame_complete; }
    void clearFrameComplete() { frame_complete = false; }
    bool isNMI() const { return nmi_occurred; }
    void clearNMI() { nmi_occurred = false; }

    // Framebuffer access (RGB format)
    const u32* getFramebuffer() const { return framebuffer; }

    // For debugging
    int getScanline() const { return scanline; }
    int getCycle() const { return cycle; }

    // For debugger manipulation
    u8 getCtrl() const { return ctrl; }
    u8 getMask() const { return mask; }
    u8 getStatus() const { return status; }
    u8 getOamAddr() const { return oam_addr; }
    u16 getVramAddr() const { return v; }
    u16 getTempAddr() const { return t; }
    u8 getFineX() const { return fine_x; }
    bool getWriteToggle() const { return w; }

    void setCtrl(u8 val) { ctrl = val; }
    void setMask(u8 val) { mask = val; }
    void setStatus(u8 val) { status = val; }
    void setOamAddr(u8 val) { oam_addr = val; }
    void setVramAddr(u16 val) { v = val; }
    void setTempAddr(u16 val) { t = val; }
    void setFineX(u8 val) { fine_x = val & 0x07; }

    // OAM access for debugging
    u8 getOamByte(u8 index) const { return oam[index]; }
    const u8* getOam() const { return oam; }

    // Palette access for debugging
    u8 getPaletteByte(u8 index) const { return palette[index & 0x1F]; }
    const u8* getPalette() const { return palette; }

    // Nametable access for debugging
    u8 getNametableByte(u16 addr) const { return nametable[addr & 0x7FF]; }
    const u8* getNametable() const { return nametable; }

private:
    // Internal VRAM access
    u8 ppuRead(u16 addr);
    void ppuWrite(u16 addr, u8 data);

    // Bus and Cartridge references
    Bus& bus;
    Cartridge& cart;

    // Rendering helpers
    void fillScanlineBuffer();
    void renderScanlineBurst();
    u32 getColorFromPalette(u8 palette, u8 pixel);

    // Registers
    u8 ctrl;        // $2000 PPUCTRL
    u8 mask;        // $2001 PPUMASK
    u8 status;      // $2002 PPUSTATUS
    u8 oam_addr;    // $2003 OAMADDR

    // Loopy registers for scrolling
    u16 v;          // Current VRAM address (15 bits)
    u16 t;          // Temporary VRAM address (15 bits)
    u8 fine_x;      // Fine X scroll (3 bits)
    bool w;         // Write toggle

    // Data buffer for PPUDATA reads
    u8 data_buffer;

    // Timing
    int scanline;
    int cycle;
    bool odd_frame;
    bool frame_complete;
    bool nmi_occurred;

    // Memory
    u8 nametable[2048];     // 2KB VRAM for nametables
    u8 palette[32];         // Palette RAM
    u8 oam[256];            // Object Attribute Memory (sprites)

    // Background rendering latches
    u8 nt_byte;
    u8 at_byte;
    u8 bg_lo;
    u8 bg_hi;
    u16 bg_shifter_lo;
    u16 bg_shifter_hi;
    u16 at_shifter_lo;
    u16 at_shifter_hi;
    u8 at_latch_lo;
    u8 at_latch_hi;

    // Sprite rendering
    struct Sprite {
        u8 y;
        u8 tile;
        u8 attr;
        u8 x;
        u8 pattern_lo;
        u8 pattern_hi;
        bool active;
    };
    Sprite secondary_oam[8];  // Up to 8 sprites per scanline
    int sprite_count;
    bool sprite_zero_on_line;

    // Scanline rendering buffer for burst rendering
    struct ScanlineData {
        u8 bg_pixels[256];        // Background pixel values (0-3)
        u8 bg_palettes[256];      // Background palette indices (0-3)
        u8 sprite_pixels[256];    // Sprite pixel values (0-3)
        u8 sprite_palettes[256];  // Sprite palette indices (4-7)
        bool sprite_priority[256]; // Sprite behind background flag
    };
    ScanlineData scanline_buffer;

    // Output
    u32 framebuffer[NES_WIDTH * NES_HEIGHT];
};

#endif // PPU_H
