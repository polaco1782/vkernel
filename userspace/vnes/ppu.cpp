#include "ppu.h"
#include "bus.h"
#include "cartridge.h"
#include "disasm.h"

using namespace vnes::disasm;

PPU::PPU(Bus& b, Cartridge& c)
    : bus(b), cart(c)
    , ctrl(0), mask(0), status(0), oam_addr(0)
    , v(0), t(0), fine_x(0), w(false)
    , data_buffer(0)
    , scanline(0), cycle(0), odd_frame(false)
    , frame_complete(false), nmi_occurred(false)
    , nt_byte(0), at_byte(0), bg_lo(0), bg_hi(0)
    , bg_shifter_lo(0), bg_shifter_hi(0)
    , at_shifter_lo(0), at_shifter_hi(0)
    , at_latch_lo(0), at_latch_hi(0)
    , sprite_count(0), sprite_zero_on_line(false)
{
    for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++)
        framebuffer[i] = 0;
    for (int i = 0; i < 2048; i++)
        nametable[i] = 0;
    for (int i = 0; i < 32; i++)
        palette[i] = 0;
    for (int i = 0; i < 256; i++)
        oam[i] = 0;
    for (int i = 0; i < 8; i++)
        secondary_oam[i] = { 0, 0, 0, 0, 0, 0, false };
}

void PPU::reset()
{
    ctrl = 0;
    mask = 0;
    status = 0;
    oam_addr = 0;
    v = 0;
    t = 0;
    fine_x = 0;
    w = false;
    data_buffer = 0;
    scanline = 0;
    cycle = 0;
    odd_frame = false;
    frame_complete = false;
    nmi_occurred = false;
}

u8 PPU::ppuRead(u16 addr)
{
    addr &= 0x3FFF;

    if (addr < 0x2000) {
        // Pattern tables (CHR ROM/RAM)
        return cart.readChr(addr);
    }
    else if (addr < 0x3F00) {
        // Nametables
        addr &= 0x0FFF;
        // Handle mirroring
        Mirroring mirror = cart.getMirroring();
        if (mirror == Mirroring::VERTICAL) {
            addr &= 0x07FF;
        }
        else if (mirror == Mirroring::HORIZONTAL) {
            // Horizontal: Top two nametables (0,1) share memory, bottom two (2,3) share memory
            // Map bit 11 to bit 10, keep bits 0-9
            addr = ((addr & 0x0800) >> 1) | (addr & 0x03FF);
        }
        else if (mirror == Mirroring::SINGLE_LOWER) {
            addr &= 0x03FF;  // All map to first nametable
        }
        else if (mirror == Mirroring::SINGLE_UPPER) {
            addr = 0x0400 + (addr & 0x03FF);  // All map to second nametable
        }
        return nametable[addr & 0x07FF];
    }
    else {
        // Palette
        addr &= 0x1F;
        if (addr == 0x10 || addr == 0x14 || addr == 0x18 || addr == 0x1C)
            addr &= 0x0F;
        return palette[addr];
    }
}

void PPU::ppuWrite(u16 addr, u8 data)
{
    addr &= 0x3FFF;

    if (addr < 0x2000) {
        // Pattern tables (CHR RAM only)
        cart.writeChr(addr, data);
    }
    else if (addr < 0x3F00) {
        // Nametables
        addr &= 0x0FFF;
        Mirroring mirror = cart.getMirroring();
        if (mirror == Mirroring::VERTICAL) {
            addr &= 0x07FF;
        }
        else if (mirror == Mirroring::HORIZONTAL) {
            // Horizontal: Top two nametables (0,1) share memory, bottom two (2,3) share memory
            // Map bit 11 to bit 10, keep bits 0-9
            addr = ((addr & 0x0800) >> 1) | (addr & 0x03FF);
        }
        else if (mirror == Mirroring::SINGLE_LOWER) {
            addr &= 0x03FF;  // All map to first nametable
        }
        else if (mirror == Mirroring::SINGLE_UPPER) {
            addr = 0x0400 + (addr & 0x03FF);  // All map to second nametable
        }
        nametable[addr & 0x07FF] = data;
    }
    else {
        // Palette
        addr &= 0x1F;
        if (addr == 0x10 || addr == 0x14 || addr == 0x18 || addr == 0x1C)
            addr &= 0x0F;
        palette[addr] = data;
    }
}

u8 PPU::readRegister(u16 addr)
{
    u8 data = 0;

    switch (addr & 0x07) {
    case 2: // PPUSTATUS
        data = (status & 0xE0) | (data_buffer & 0x1F);
        status &= ~0x80;  // Clear vblank flag
        w = false;        // Reset write toggle
        break;

    case 4: // OAMDATA
        data = oam[oam_addr];
        break;

    case 7: // PPUDATA
        data = data_buffer;
        data_buffer = ppuRead(v);
        // Palette data is not buffered
        if (v >= 0x3F00)
            data = data_buffer;
        v += (ctrl & 0x04) ? 32 : 1;
        break;
    }

    return data;
}

void PPU::writeRegister(u16 addr, u8 data)
{
    switch (addr & 0x07) {
    case 0: // PPUCTRL
        ctrl = data;
        t = (t & 0xF3FF) | ((data & 0x03) << 10);
        break;

    case 1: // PPUMASK
        mask = data;
        break;

    case 3: // OAMADDR
        oam_addr = data;
        break;

    case 4: // OAMDATA
        oam[oam_addr++] = data;
        break;

    case 5: // PPUSCROLL
        if (!w) {
            fine_x = data & 0x07;
            t = (t & 0xFFE0) | (data >> 3);
        }
        else {
            t = (t & 0x8C1F) | ((data & 0x07) << 12) | ((data & 0xF8) << 2);
        }
        w = !w;
        break;

    case 6: // PPUADDR
        if (!w) {
            t = (t & 0x00FF) | ((data & 0x3F) << 8);
        }
        else {
            t = (t & 0xFF00) | data;
            v = t;
        }
        w = !w;
        break;

    case 7: // PPUDATA
        ppuWrite(v, data);
        v += (ctrl & 0x04) ? 32 : 1;
        break;
    }
}

void PPU::writeDMA(u8 data)
{
    // OAM DMA - transfer 256 bytes from CPU memory
    u16 addr = data << 8;
    for (int i = 0; i < 256; i++) {
        oam[oam_addr++] = bus.read(addr + i);
    }
}

u32 PPU::getColorFromPalette(u8 pal, u8 pixel)
{
    u8 index = ppuRead(0x3F00 + (pal << 2) + pixel) & 0x3F;
    return nesPalette[index] | 0xFF000000;  // Add alpha
}

void PPU::fillScanlineBuffer()
{
    int x = cycle - 1;

    if (x < 0 || x >= 256)
        return;

    u8 bg_pixel = 0;
    u8 bg_palette = 0;

    // Background rendering
    if (mask & 0x08) {  // Show background
        if ((mask & 0x02) || x >= 8) {  // Show left 8 pixels
            u16 bit_mux = 0x8000 >> fine_x;
            u8 p0 = (bg_shifter_lo & bit_mux) ? 1 : 0;
            u8 p1 = (bg_shifter_hi & bit_mux) ? 2 : 0;
            bg_pixel = p0 | p1;

            u8 a0 = (at_shifter_lo & bit_mux) ? 1 : 0;
            u8 a1 = (at_shifter_hi & bit_mux) ? 2 : 0;
            bg_palette = a0 | a1;
        }
    }

    // Store background data
    scanline_buffer.bg_pixels[x] = bg_pixel;
    scanline_buffer.bg_palettes[x] = bg_palette;

    // Sprite rendering
    u8 sprite_pixel = 0;
    u8 sprite_palette = 0;
    bool sprite_priority = false;

    if (mask & 0x10) {  // Show sprites
        for (int i = 0; i < sprite_count; i++) {
            if (!secondary_oam[i].active)
                continue;

            int sprite_x = secondary_oam[i].x;

            // Check if sprite is at current x position
            if (x >= sprite_x && x < sprite_x + 8) {
                // Check left clip
                if ((mask & 0x04) || x >= 8) {  // Show left 8 pixels for sprites
                    int sprite_x_offset = x - sprite_x;

                    // Handle horizontal flip
                    if (secondary_oam[i].attr & 0x40) {
                        sprite_x_offset = 7 - sprite_x_offset;
                    }

                    // Get pixel value from pattern data
                    u8 pixel = ((secondary_oam[i].pattern_lo >> (7 - sprite_x_offset)) & 1) |
                        (((secondary_oam[i].pattern_hi >> (7 - sprite_x_offset)) & 1) << 1);

                    // Only use sprite if pixel is non-transparent
                    if (pixel != 0) {
                        // First non-transparent sprite pixel wins
                        if (sprite_pixel == 0) {
                            sprite_pixel = pixel;
                            sprite_palette = (secondary_oam[i].attr & 0x03) + 4;  // Sprite palettes are 4-7
                            sprite_priority = (secondary_oam[i].attr & 0x20) != 0;  // Priority bit

                            // Check if this is sprite 0
                            if (i == 0 && sprite_zero_on_line && bg_pixel != 0 && sprite_pixel != 0 && x != 255 && !(status & 0x40))
                                    status |= 0x40;  // Set sprite 0 hit flag
                        }
                    }
                }
            }
        }
    }

    // Store sprite data
    scanline_buffer.sprite_pixels[x] = sprite_pixel;
    scanline_buffer.sprite_palettes[x] = sprite_palette;
    scanline_buffer.sprite_priority[x] = sprite_priority;
}

void PPU::renderScanlineBurst()
{
    if (scanline < 0 || scanline >= NES_HEIGHT)
        return;

    int y = scanline;
    u32* line = &framebuffer[y * NES_WIDTH];

    // Render all 256 pixels in one burst
    for (int x = 0; x < 256; x++) {
        u8 bg_pixel = scanline_buffer.bg_pixels[x];
        u8 bg_palette = scanline_buffer.bg_palettes[x];
        u8 sprite_pixel = scanline_buffer.sprite_pixels[x];
        u8 sprite_palette = scanline_buffer.sprite_palettes[x];
        bool sprite_priority = scanline_buffer.sprite_priority[x];

        // Priority multiplexer
        u8 final_pixel;
        u8 final_palette;

        if (bg_pixel == 0 && sprite_pixel == 0) {
            // Both transparent - use backdrop color
            final_pixel = 0;
            final_palette = 0;
        }
        else if (bg_pixel == 0 && sprite_pixel > 0) {
            // Background transparent, sprite visible
            final_pixel = sprite_pixel;
            final_palette = sprite_palette;
        }
        else if (bg_pixel > 0 && sprite_pixel == 0) {
            // Sprite transparent, background visible
            final_pixel = bg_pixel;
            final_palette = bg_palette;
        }
        else {
            // Both visible - check priority
            if (sprite_priority) {
                // Sprite behind background
                final_pixel = bg_pixel;
                final_palette = bg_palette;
            }
            else {
                // Sprite in front of background
                final_pixel = sprite_pixel;
                final_palette = sprite_palette;
            }
        }

        // Write final color directly to framebuffer
        line[x] = getColorFromPalette(final_palette, final_pixel);
    }
}

void PPU::step()
{
    // Visible scanlines (0-239) and pre-render scanline (261)
    if (scanline < 240 || scanline == 261) {

        // Sprite evaluation at cycle 257-320
        if (cycle == 257 && scanline < 240) {
            sprite_count = 0;
            sprite_zero_on_line = false;

            // Sprite height: 8 or 16 based on PPUCTRL bit 5
            int sprite_height = (ctrl & 0x20) ? 16 : 8;

            // Evaluate all 64 sprites
            for (int i = 0; i < 64; i++) {
                int sprite_y = oam[i * 4];
                int diff = scanline - sprite_y;

                // Check if sprite is on this scanline
                if (diff >= 0 && diff < sprite_height) {
                    if (sprite_count < 8) {
                        // Copy sprite to secondary OAM
                        secondary_oam[sprite_count].y = sprite_y;
                        secondary_oam[sprite_count].tile = oam[i * 4 + 1];
                        secondary_oam[sprite_count].attr = oam[i * 4 + 2];
                        secondary_oam[sprite_count].x = oam[i * 4 + 3];
                        secondary_oam[sprite_count].active = false;  // Will be set during fetch

                        if (i == 0) {
                            sprite_zero_on_line = true;
                        }

                        sprite_count++;
                    }
                    else {
                        // Sprite overflow
                        status |= 0x20;
                        break;
                    }
                }
            }
        }

        // Sprite pattern fetches at cycles 257-320 (64 cycles for 8 sprites)
        if (cycle >= 257 && cycle <= 320 && scanline < 240) {
            int fetch_cycle = (cycle - 257) / 8;
            if (fetch_cycle < sprite_count) {
                int phase = (cycle - 257) % 8;

                if (phase == 0) {
                    // Start fetching this sprite
                    secondary_oam[fetch_cycle].active = true;
                }
                else if (phase == 5) {
                    // Fetch pattern low byte
                    int sprite_height = (ctrl & 0x20) ? 16 : 8;
                    int sprite_y_offset = scanline - secondary_oam[fetch_cycle].y;

                    // Handle vertical flip
                    if (secondary_oam[fetch_cycle].attr & 0x80) {
                        sprite_y_offset = sprite_height - 1 - sprite_y_offset;
                    }

                    // Get pattern table address
                    u16 pattern_addr;
                    u8 tile = secondary_oam[fetch_cycle].tile;

                    if (sprite_height == 8) {
                        // 8x8 sprites: use PPUCTRL bit 3 for pattern table
                        pattern_addr = ((ctrl & 0x08) << 9) | (tile << 4) | sprite_y_offset;
                    }
                    else {
                        // 8x16 sprites: tile bit 0 selects pattern table, use top/bottom tile
                        int table = tile & 0x01;
                        tile &= 0xFE;
                        if (sprite_y_offset >= 8) {
                            sprite_y_offset -= 8;
                            tile += 1;
                        }
                        pattern_addr = (table << 12) | (tile << 4) | sprite_y_offset;
                    }
                    secondary_oam[fetch_cycle].pattern_lo = ppuRead(pattern_addr);
                }
                else if (phase == 7) {
                    // Fetch pattern high byte
                    int sprite_height = (ctrl & 0x20) ? 16 : 8;
                    int sprite_y_offset = scanline - secondary_oam[fetch_cycle].y;

                    // Handle vertical flip
                    if (secondary_oam[fetch_cycle].attr & 0x80) {
                        sprite_y_offset = sprite_height - 1 - sprite_y_offset;
                    }

                    // Get pattern table address
                    u16 pattern_addr;
                    u8 tile = secondary_oam[fetch_cycle].tile;

                    if (sprite_height == 8) {
                        // 8x8 sprites: use PPUCTRL bit 3 for pattern table
                        pattern_addr = ((ctrl & 0x08) << 9) | (tile << 4) | sprite_y_offset | 8;
                    }
                    else {
                        // 8x16 sprites: tile bit 0 selects pattern table, use top/bottom tile
                        int table = tile & 0x01;
                        tile &= 0xFE;
                        if (sprite_y_offset >= 8) {
                            sprite_y_offset -= 8;
                            tile += 1;
                        }
                        pattern_addr = (table << 12) | (tile << 4) | sprite_y_offset | 8;
                    }
                    secondary_oam[fetch_cycle].pattern_hi = ppuRead(pattern_addr);
                }
            }
        }

        // Background fetches
        if ((cycle >= 1 && cycle <= 256) || (cycle >= 321 && cycle <= 336)) {
            // Shift registers
            bg_shifter_lo <<= 1;
            bg_shifter_hi <<= 1;
            at_shifter_lo <<= 1;
            at_shifter_hi <<= 1;

            // At cycle 1, load pattern and attribute data into shifters
            if ((cycle & 0x07) == 1) {
                // Load shifters with pattern data (8 bits at once)
                bg_shifter_lo = (bg_shifter_lo & 0xFF00) | bg_lo;
                bg_shifter_hi = (bg_shifter_hi & 0xFF00) | bg_hi;
                at_shifter_lo = (at_shifter_lo & 0xFF00) | ((at_byte & 1) ? 0xFF : 0x00);
                at_shifter_hi = (at_shifter_hi & 0xFF00) | ((at_byte & 2) ? 0xFF : 0x00);
            }

            switch (cycle & 0x07) {
            case 1:  // Nametable byte
                nt_byte = ppuRead(0x2000 | (v & 0x0FFF));
                break;
            case 3:  // Attribute byte
                at_byte = ppuRead(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07));
                if (v & 0x40) at_byte >>= 4;
                if (v & 0x02) at_byte >>= 2;
                at_byte &= 0x03;
                break;
            case 5:  // Pattern low
                bg_lo = ppuRead(((ctrl & 0x10) << 8) + (nt_byte << 4) + ((v >> 12) & 0x07));
                break;
            case 7:  // Pattern high
                bg_hi = ppuRead(((ctrl & 0x10) << 8) + (nt_byte << 4) + ((v >> 12) & 0x07) + 8);
                break;
            case 0:  // Increment horizontal
                if (mask & 0x18) {  // Rendering enabled
                    if ((v & 0x001F) == 31) {
                        v &= ~0x001F;
                        v ^= 0x0400;
                    }
                    else {
                        v++;
                    }
                }
                break;
            }
        }

        // Increment vertical at cycle 256
        if (cycle == 256 && (mask & 0x18)) {
            if ((v & 0x7000) != 0x7000) {
                v += 0x1000;
            }
            else {
                v &= ~0x7000;
                int y = (v & 0x03E0) >> 5;
                if (y == 29) {
                    y = 0;
                    v ^= 0x0800;
                }
                else if (y == 31) {
                    y = 0;
                }
                else {
                    y++;
                }
                v = (v & ~0x03E0) | (y << 5);
            }
        }

        // Copy horizontal bits at cycle 257
        if (cycle == 257 && (mask & 0x18)) {
            v = (v & ~0x041F) | (t & 0x041F);
        }

        // Copy vertical bits during pre-render (261), cycles 280-304
        if (scanline == 261 && cycle >= 280 && cycle <= 304 && (mask & 0x18)) {
            v = (v & ~0x7BE0) | (t & 0x7BE0);
        }

        // MMC3 IRQ counter: Clock at cycle 260 on visible and pre-render scanlines
        // This is when the PPU starts fetching tiles for the next scanline
        if (cycle == 260 && (scanline < 240 || scanline == 261)) {
            if (mask & 0x18) {  // Only clock when rendering is enabled
                cart.scanline();
            }
        }

        // Fill scanline buffer during rendering
        if (scanline < 240 && cycle >= 1 && cycle <= 256) {
            fillScanlineBuffer();
        }

        // Render entire scanline in burst at cycle 257
        if (scanline < 240 && cycle == 257) {
            renderScanlineBurst();
        }
    }

    // Pre-render scanline - clear flags
    if (scanline == 261 && cycle == 1) {
        status &= ~0xE0;  // Clear vblank, sprite 0, overflow
        nmi_occurred = false;
    }

    // VBlank begins at scanline 241
    if (scanline == 241 && cycle == 1) {
        status |= 0x80;  // Set vblank flag
        if (ctrl & 0x80) {
            nmi_occurred = true;
        }
    }

    // Advance cycle/scanline
    cycle++;

    // Odd frame cycle skip: on odd frames with rendering enabled,
    // skip cycle 0 of scanline 0 (effectively skip last cycle of pre-render)
    if (scanline == 261 && cycle == 340 && odd_frame && (mask & 0x18)) {
        cycle = 0;
        scanline = 0;
        frame_complete = true;
        odd_frame = false;
    }
    else if (cycle > 340) {
        cycle = 0;
        scanline++;
        if (scanline > 261) {
            scanline = 0;
            frame_complete = true;
            odd_frame = !odd_frame;
        }
    }
}