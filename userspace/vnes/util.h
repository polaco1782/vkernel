#ifndef VNES_UTIL_H
#define VNES_UTIL_H

#include "types.h"

#include <string>

namespace vnes::util {

inline auto hex_digit(u8 value) -> char
{
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + (value - 10));
}

inline auto toHexUnsigned(unsigned long long value, int width) -> std::string
{
    if (width < 1) {
        width = 1;
    }

    std::string result;
    result.resize(static_cast<size_t>(width), '0');
    for (int index = width - 1; index >= 0; --index) {
        result[static_cast<size_t>(index)] = hex_digit(static_cast<u8>(value & 0x0FULL));
        value >>= 4;
    }

    return result;
}

template<typename T>
inline auto toHex(T value, int width) -> std::string
{
    return toHexUnsigned(static_cast<unsigned long long>(value), width);
}

inline auto hexByte(u8 value) -> std::string
{
    return toHex(value, 2);
}

inline auto hexWord(u16 value) -> std::string
{
    return toHex(value, 4);
}

inline auto formatFlags(u8 status) -> std::string
{
    std::string flags;
    flags.reserve(8);
    flags.push_back((status & 0x80U) ? 'N' : 'n');
    flags.push_back((status & 0x40U) ? 'V' : 'v');
    flags.push_back((status & 0x20U) ? 'U' : 'u');
    flags.push_back((status & 0x10U) ? 'B' : 'b');
    flags.push_back((status & 0x08U) ? 'D' : 'd');
    flags.push_back((status & 0x04U) ? 'I' : 'i');
    flags.push_back((status & 0x02U) ? 'Z' : 'z');
    flags.push_back((status & 0x01U) ? 'C' : 'c');
    return flags;
}

} // namespace vnes::util

#endif // VNES_UTIL_H
