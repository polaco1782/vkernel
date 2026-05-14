#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stddef.h>

#include <string>
#include <vector>

#include "mapper.h"
#include "types.h"

struct INESHeader {
    u8 magic[4];
    u8 prg_rom_size;
    u8 chr_rom_size;
    u8 flags6;
    u8 flags7;
    u8 flags8;
    u8 flags9;
    u8 flags10;
    u8 padding[5];
};

class Cartridge {
public:
    Cartridge();
    ~Cartridge();

    bool load(const std::string& filepath);

    bool isLoaded() const { return loaded; }
    u8 getMapperNumber() const { return mapperNumber; }
    Mirroring getMirroring() const;
    bool hasBattery() const { return battery; }
    const char* getMapperName() const;
    Mapper* getMapper() const { return mapper.get(); }

    const std::vector<u8>& getPrgRom() const { return prg_rom; }
    const std::vector<u8>& getChrRom() const { return chr_rom; }

    u8 readPrg(u16 addr) const;
    void writePrg(u16 addr, u8 data);

    u8 readChr(u16 addr) const;
    void writeChr(u16 addr, u8 data);

    void scanline();
    void clearIRQ();
    bool hasIRQ();

    bool addGGCode(const std::string& code);
    void removeGGCode(const std::string& code);

    void signalFrameComplete();
    void flushSRAM();

private:
    struct GGActiveEntry {
        std::string code;
        u16 addr = 0;
        u8 value = 0;
        u8 compare = 0;
        bool has_compare = false;
        u32 hits = 0;
    };

    static constexpr size_t MAX_GG_CODES = 16;

    bool parseHeader(const INESHeader& header);

    bool loaded;
    u8 mapperNumber;
    bool battery;

    std::vector<u8> prg_rom;
    std::vector<u8> chr_rom;
    std::vector<u8> prg_ram;

    u8 gg_count;
    GGActiveEntry gg_active_entries[MAX_GG_CODES];

    vk::unique_ptr<Mapper> mapper;
    Mirroring initialMirroring;

    bool prgRamDirty;
    u32 framesSinceLastSave;
    std::string savePath;
};

#endif // CARTRIDGE_H