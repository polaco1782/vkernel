#include "frontend.h"

namespace clownmdemu_frontend {

vk_u32 min_u32(vk_u32 lhs, vk_u32 rhs) {
    return lhs < rhs ? lhs : rhs;
}

vk_u32 max_u32(vk_u32 lhs, vk_u32 rhs) {
    return lhs > rhs ? lhs : rhs;
}

void copy_string(char* dst, size_t dst_capacity, const char* src) {
    if (dst == nullptr || dst_capacity == 0)
        return;

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_capacity, "%s", src);
}

namespace {

char ascii_to_lower(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

} // namespace

int compare_casefolded(const char* lhs, const char* rhs) {
    if (lhs == nullptr && rhs == nullptr)
        return 0;
    if (lhs == nullptr)
        return -1;
    if (rhs == nullptr)
        return 1;

    while (*lhs != '\0' || *rhs != '\0') {
        const char lhs_ch = ascii_to_lower(*lhs);
        const char rhs_ch = ascii_to_lower(*rhs);
        if (lhs_ch != rhs_ch)
            return static_cast<unsigned char>(lhs_ch) < static_cast<unsigned char>(rhs_ch) ? -1 : 1;
        if (*lhs != '\0')
            ++lhs;
        if (*rhs != '\0')
            ++rhs;
    }

    return 0;
}

bool ends_with_casefolded(const char* text, const char* suffix) {
    if (text == nullptr || suffix == nullptr)
        return false;

    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length)
        return false;

    return compare_casefolded(text + (text_length - suffix_length), suffix) == 0;
}

const char* path_basename(const char* path) {
    if (path == nullptr || path[0] == '\0')
        return "";

    const char* slash = strrchr(path, '/');
    if (slash != nullptr && slash[1] != '\0')
        return slash + 1;
    return path;
}

vk_u32 pack_pixel(unsigned char r, unsigned char g, unsigned char b) {
    return (static_cast<vk_u32>(r) << 16) | (static_cast<vk_u32>(g) << 8) | static_cast<vk_u32>(b);
}

vk_u32 unpack_palette_colour(cc_u16f colour) {
    const unsigned char red = static_cast<unsigned char>((colour & 0x000Fu) * 0x11u);
    const unsigned char green = static_cast<unsigned char>(((colour >> 4) & 0x000Fu) * 0x11u);
    const unsigned char blue = static_cast<unsigned char>(((colour >> 8) & 0x000Fu) * 0x11u);
    return pack_pixel(red, green, blue);
}

vk_u32 current_fm_rate(ClownMDEmu_TVStandard tv_standard) {
    return tv_standard == CLOWNMDEMU_TV_STANDARD_PAL ? CLOWNMDEMU_FM_SAMPLE_RATE_PAL : CLOWNMDEMU_FM_SAMPLE_RATE_NTSC;
}

vk_u32 current_psg_rate(ClownMDEmu_TVStandard tv_standard) {
    return tv_standard == CLOWNMDEMU_TV_STANDARD_PAL ? CLOWNMDEMU_PSG_SAMPLE_RATE_PAL : CLOWNMDEMU_PSG_SAMPLE_RATE_NTSC;
}

void close_save_file(AppState* app) {
    if (app->save_file != nullptr) {
        fclose(app->save_file);
        app->save_file = nullptr;
    }
}

} // namespace clownmdemu_frontend