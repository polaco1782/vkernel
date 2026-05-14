#include "cartridge.h"

#include "util.h"

#include <ctype.h>
#include <stdio.h>

#include <iostream>

using vnes::util::toHex;

namespace {

auto gg_nibble_for_char(char ch) -> int
{
    static const char k_alphabet[] = "APZLGITYEOXUKSVN";

    for (int index = 0; k_alphabet[index] != '\0'; ++index) {
        if (k_alphabet[index] == ch) {
            return index;
        }
    }

    return -1;
}

auto build_save_path(const std::string& filepath) -> std::string
{
    std::string path = filepath;
    size_t dot = path.size();
    for (size_t index = path.size(); index > 0; --index) {
        if (path[index - 1] == '.') {
            dot = index - 1;
            break;
        }
    }

    if (dot != path.size()) {
        path = path.substr(0, dot);
    }
    path += ".sav";
    return path;
}

} // namespace

Cartridge::Cartridge()
    : loaded(false)
    , mapperNumber(0)
    , battery(false)
    , gg_count(0)
    , initialMirroring(Mirroring::HORIZONTAL)
    , prgRamDirty(false)
    , framesSinceLastSave(0)
{
    for (size_t index = 0; index < MAX_GG_CODES; ++index) {
        gg_active_entries[index] = GGActiveEntry();
    }
}

Cartridge::~Cartridge()
{
    flushSRAM();
}

bool Cartridge::addGGCode(const std::string& code)
{
    std::string normalized;
    for (const char* cursor = code.c_str(); *cursor != '\0'; ++cursor) {
        if (isalpha(static_cast<unsigned char>(*cursor)) != 0) {
            normalized.push_back(static_cast<char>(toupper(static_cast<unsigned char>(*cursor))));
        }
    }

    if (normalized.empty() || (normalized.length() != 6 && normalized.length() != 8)) {
        return false;
    }

    u8 nibbles[8] = {};
    for (size_t index = 0; index < normalized.length(); ++index) {
        const int nibble = gg_nibble_for_char(normalized[index]);
        if (nibble < 0) {
            return false;
        }
        nibbles[index] = static_cast<u8>(nibble);
    }

    const u16 addr = static_cast<u16>(0x8000U
        + ((nibbles[3] & 7U) << 12)
        + ((nibbles[5] & 7U) << 8)
        + ((nibbles[4] & 8U) << 8)
        + ((nibbles[2] & 7U) << 4)
        + ((nibbles[1] & 8U) << 4)
        + (nibbles[4] & 7U)
        + (nibbles[3] & 8U));

    u8 value = 0;
    u8 compare = 0;
    if (normalized.length() == 6) {
        value = static_cast<u8>(((nibbles[1] & 7U) << 4)
            + ((nibbles[0] & 8U) << 4)
            + (nibbles[0] & 7U)
            + (nibbles[5] & 8U));
    } else {
        value = static_cast<u8>(((nibbles[1] & 7U) << 4)
            + ((nibbles[0] & 8U) << 4)
            + (nibbles[0] & 7U)
            + (nibbles[7] & 8U));
        compare = static_cast<u8>(((nibbles[7] & 7U) << 4)
            + ((nibbles[6] & 8U) << 4)
            + (nibbles[6] & 7U)
            + (nibbles[5] & 8U));
    }

    std::cout << "Adding Game Genie code: " << code
              << " -> Write $" << toHex(value, 2)
              << " to $" << toHex(addr, 4) << std::endl;

    GGActiveEntry entry;
    entry.code = code;
    entry.addr = addr;
    entry.value = value;
    entry.compare = compare;
    entry.has_compare = normalized.length() == 8;

    for (u8 index = 0; index < gg_count; ++index) {
        if (gg_active_entries[index].addr == addr) {
            gg_active_entries[index] = entry;
            return true;
        }
    }

    if (gg_count >= MAX_GG_CODES) {
        std::cerr << "Error: Maximum number of Game Genie codes reached (" << MAX_GG_CODES << ")" << std::endl;
        return false;
    }

    gg_active_entries[gg_count] = entry;
    ++gg_count;
    return true;
}

void Cartridge::removeGGCode(const std::string& code)
{
    for (u8 index = 0; index < gg_count; ++index) {
        if (gg_active_entries[index].code == code) {
            const u8 last = static_cast<u8>(gg_count - 1);
            if (index != last) {
                gg_active_entries[index] = gg_active_entries[last];
            }
            gg_active_entries[last] = GGActiveEntry();
            --gg_count;
            return;
        }
    }
}

bool Cartridge::load(const std::string& filepath)
{
    flushSRAM();

    loaded = false;
    prg_rom.clear();
    chr_rom.clear();
    prg_ram.clear();
    mapper.reset();
    savePath.clear();
    prgRamDirty = false;
    framesSinceLastSave = 0;

    for (size_t index = 0; index < MAX_GG_CODES; ++index) {
        gg_active_entries[index] = GGActiveEntry();
    }
    gg_count = 0;

    FILE* file = fopen(filepath.c_str(), "rb");
    if (file == nullptr) {
        std::cerr << "Error: Cannot open file: " << filepath << std::endl;
        return false;
    }

    INESHeader header = {};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        std::cerr << "Error: Cannot read header" << std::endl;
        fclose(file);
        return false;
    }

    if (!parseHeader(header)) {
        fclose(file);
        return false;
    }

    if ((header.flags6 & 0x04U) != 0U && fseek(file, TRAINER_SIZE, SEEK_CUR) != 0) {
        std::cerr << "Error: Cannot skip trainer" << std::endl;
        fclose(file);
        return false;
    }

    const u32 prg_size = header.prg_rom_size * PRG_ROM_UNIT;
    prg_rom.resize(prg_size);
    if (prg_size > 0 && fread(prg_rom.data(), 1, prg_size, file) != prg_size) {
        std::cerr << "Error: Cannot read PRG ROM" << std::endl;
        fclose(file);
        return false;
    }

    const u32 chr_size = header.chr_rom_size * CHR_ROM_UNIT;
    if (chr_size > 0) {
        chr_rom.resize(chr_size);
        if (fread(chr_rom.data(), 1, chr_size, file) != chr_size) {
            std::cerr << "Error: Cannot read CHR ROM" << std::endl;
            fclose(file);
            return false;
        }
    } else {
        chr_rom.resize(CHR_ROM_UNIT, 0);
    }

    fclose(file);

    prg_ram.resize(8192, 0);
    mapper = MapperFactory::create(mapperNumber);
    mapper->init(prg_rom, chr_rom, prg_ram, initialMirroring);

    if (battery) {
        savePath = build_save_path(filepath);
        FILE* save_file = fopen(savePath.c_str(), "rb");
        if (save_file != nullptr) {
            const size_t read_count = fread(prg_ram.data(), 1, prg_ram.size(), save_file);
            fclose(save_file);
            if (read_count > 0) {
                std::cout << "  Loaded SRAM from " << savePath << std::endl;
            }
        }
    }

    loaded = true;

    std::cout << "ROM loaded: " << filepath << std::endl;
    std::cout << "  PRG ROM: " << (prg_size / 1024U) << " KB" << std::endl;
    std::cout << "  CHR ROM: " << (chr_size / 1024U) << " KB";
    if (chr_size == 0) {
        std::cout << " (using CHR RAM)";
    }
    std::cout << std::endl;
    std::cout << "  Mapper: " << (int)mapperNumber << " (" << getMapperName() << ")" << std::endl;
    std::cout << "  Mirroring: ";
    switch (initialMirroring) {
        case Mirroring::HORIZONTAL:   std::cout << "Horizontal"; break;
        case Mirroring::VERTICAL:     std::cout << "Vertical"; break;
        case Mirroring::FOUR_SCREEN:  std::cout << "Four-screen"; break;
        case Mirroring::SINGLE_LOWER: std::cout << "Single (lower)"; break;
        case Mirroring::SINGLE_UPPER: std::cout << "Single (upper)"; break;
    }
    std::cout << std::endl;
    std::cout << "  Battery: " << (battery ? "Yes" : "No") << std::endl;

    return true;
}

bool Cartridge::parseHeader(const INESHeader& header)
{
    if (header.magic[0] != 'N'
        || header.magic[1] != 'E'
        || header.magic[2] != 'S'
        || header.magic[3] != 0x1AU) {
        std::cerr << "Error: Invalid iNES header" << std::endl;
        return false;
    }

    mapperNumber = static_cast<u8>((header.flags6 >> 4) | (header.flags7 & 0xF0U));

    if ((header.flags6 & 0x08U) != 0U) {
        initialMirroring = Mirroring::FOUR_SCREEN;
    } else if ((header.flags6 & 0x01U) != 0U) {
        initialMirroring = Mirroring::VERTICAL;
    } else {
        initialMirroring = Mirroring::HORIZONTAL;
    }

    battery = (header.flags6 & 0x02U) != 0U;

    if ((header.flags7 & 0x0CU) == 0x08U) {
        std::cerr << "Warning: NES 2.0 format detected, treating as iNES 1.0" << std::endl;
    }

    return true;
}

Mirroring Cartridge::getMirroring() const
{
    if (!mapper) {
        return initialMirroring;
    }
    return mapper->getMirroring();
}

const char* Cartridge::getMapperName() const
{
    if (!mapper) {
        return "UNLOADED";
    }
    if (!mapper->unsupported) {
        return mapper->getName();
    }
    return "UNSUPPORTED";
}

u8 Cartridge::readPrg(u16 addr) const
{
    if (!mapper) {
        return 0;
    }

    const u8 base_value = mapper->readPrg(addr);
    if (addr >= 0x8000U) {
        for (u8 index = 0; index < gg_count; ++index) {
            const GGActiveEntry& entry = gg_active_entries[index];
            if (entry.addr == addr && (!entry.has_compare || base_value == entry.compare)) {
                return entry.value;
            }
        }
    }

    return base_value;
}

void Cartridge::writePrg(u16 addr, u8 data)
{
    if (battery && addr >= 0x6000U && addr < 0x8000U) {
        prgRamDirty = true;
        framesSinceLastSave = 0;
    }

    if (mapper) {
        mapper->writePrg(addr, data);
    }
}

u8 Cartridge::readChr(u16 addr) const
{
    return mapper ? mapper->readChr(addr) : 0;
}

void Cartridge::writeChr(u16 addr, u8 data)
{
    if (mapper) {
        mapper->writeChr(addr, data);
    }
}

void Cartridge::signalFrameComplete()
{
    if (!battery || !prgRamDirty) {
        return;
    }

    ++framesSinceLastSave;
    if (framesSinceLastSave >= 60U) {
        flushSRAM();
        framesSinceLastSave = 0;
    }
}

void Cartridge::scanline()
{
    if (mapper) {
        mapper->scanline();
    }
}

void Cartridge::clearIRQ()
{
    if (mapper) {
        mapper->clearIrq();
    }
}

bool Cartridge::hasIRQ()
{
    return mapper ? mapper->irqPending() : false;
}

void Cartridge::flushSRAM()
{
    if (!battery || !prgRamDirty || prg_ram.empty() || savePath.empty()) {
        return;
    }

    FILE* saveFile = fopen(savePath.c_str(), "wb");
    if (saveFile == nullptr) {
        return;
    }

    fwrite(prg_ram.data(), 1, prg_ram.size(), saveFile);
    fclose(saveFile);
    prgRamDirty = false;
    std::cout << "SRAM saved to " << savePath << std::endl;
}