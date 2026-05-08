#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../include/vk.h"
#include "vendor/minimp3_ex.h"
}

namespace {

constexpr vk_u32 kPlayBlockFrames = 4096;
constexpr vk_u32 kQueueCapacityFrames = 32768;
constexpr vk_u32 kQueuePrimeFrames = kPlayBlockFrames * 2;
constexpr vk_u32 kDecodeChunkFrames = 4608;
constexpr vk_u32 kBrowserMaxEntries = 128;
constexpr vk_u32 kBrowserNameMax = 96;
constexpr vk_u32 kBrowserPathMax = 256;
constexpr vk_u32 kBrowserStatusMax = 128;
constexpr vk_u32 kBrowserResponseMax = 16384;
constexpr vk_u32 kBrowserItemMax = 128;
constexpr vk_u32 kUiGlyphWidth = 5;
constexpr vk_u32 kUiGlyphHeight = 7;
constexpr vk_u32 kUiFontScale = 2;
constexpr vk_u32 kUiFontAdvance = (kUiGlyphWidth + 1u) * kUiFontScale;
constexpr vk_u32 kUiRowHeight = (kUiGlyphHeight * kUiFontScale) + 8u;
constexpr vk_u32 kUiPanelMargin = 16;
constexpr vk_u32 kUiPanelPadding = 18;
constexpr vk_u32 kUiHeaderHeight = 76;
constexpr vk_u32 kUiFooterHeight = 30;
constexpr vk_u32 kUiRefreshTicks = 10;

struct BrowserEntry {
    char name[kBrowserNameMax];
    vk_u64 size_bytes;
    bool is_directory;
};

struct BrowserState {
    char current_path[kBrowserPathMax];
    char status[kBrowserStatusMax];
    BrowserEntry entries[kBrowserMaxEntries];
    char response[kBrowserResponseMax];
    char raw_items[kBrowserMaxEntries][kBrowserItemMax];
    vk_u32 entry_count;
    vk_u32 selected_index;
    vk_u32 scroll_index;
};

struct AudioQueueState {
    int16_t queue[kQueueCapacityFrames * 2];
    vk_u32 queue_read;
    vk_u32 queue_write;
    vk_u32 queue_count;
    int16_t play_block[kPlayBlockFrames * 2];
};

struct FileStreamState {
    FILE* file;
};

struct PlayerState {
    FileStreamState stream;
    mp3dec_ex_t decoder;
    mp3dec_io_t io;
    AudioQueueState audio;
    int16_t decode_scratch[kDecodeChunkFrames * 2];
    char loaded_path[kBrowserPathMax];
    char status[kBrowserStatusMax];
    int source_channels;
    int sample_rate;
    int bitrate_kbps;
    bool decoder_open;
    bool end_of_stream;
    bool playback_failed;
    bool current_block_active;
    vk_u32 current_block_frames;
    vk_u32 vu_left;
    vk_u32 vu_right;
    vk_u64 completed_frames;
    vk_u64 total_frames;
};

struct AppState {
    vk_framebuffer_info_t framebuffer;
    vk_u32* present_buffer;
    vk_usize present_pixels;
    BrowserState browser;
    PlayerState player;
    bool quit_requested;
};

static vk_u32 min_u32(vk_u32 lhs, vk_u32 rhs) {
    return lhs < rhs ? lhs : rhs;
}

static vk_u32 max_u32(vk_u32 lhs, vk_u32 rhs) {
    return lhs > rhs ? lhs : rhs;
}

static void copy_string(char* dst, size_t dst_capacity, const char* src) {
    if (dst == nullptr || dst_capacity == 0)
        return;

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_capacity, "%s", src);
}

static char ascii_to_lower(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

static int compare_casefolded(const char* lhs, const char* rhs) {
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

static bool ends_with_casefolded(const char* text, const char* suffix) {
    if (text == nullptr || suffix == nullptr)
        return false;

    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length)
        return false;

    return compare_casefolded(text + (text_length - suffix_length), suffix) == 0;
}

static const char* path_basename(const char* path) {
    if (path == nullptr || path[0] == '\0')
        return "";

    const char* slash = strrchr(path, '/');
    if (slash != nullptr && slash[1] != '\0')
        return slash + 1;
    return path;
}

static vk_u64 parse_u64_decimal(const char* text) {
    vk_u64 value = 0;

    if (text == nullptr)
        return 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10u + static_cast<vk_u64>(*text - '0');
        ++text;
    }

    return value;
}

static void browser_set_status(BrowserState* browser, const char* format, ...) {
    if (browser == nullptr || format == nullptr)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(browser->status, sizeof(browser->status), format, args);
    va_end(args);
}

static void player_set_status(PlayerState* player, const char* format, ...) {
    if (player == nullptr || format == nullptr)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(player->status, sizeof(player->status), format, args);
    va_end(args);
}

static bool browser_is_supported_track(const char* name, vk_u64 size_bytes) {
    if (name == nullptr || name[0] == '\0' || size_bytes == 0)
        return false;

    return ends_with_casefolded(name, ".mp3");
}

static void browser_join_path(char* out, size_t out_capacity, const char* parent, const char* child) {
    if (out == nullptr || out_capacity == 0)
        return;

    if (parent == nullptr || parent[0] == '\0' || strcmp(parent, "/") == 0) {
        snprintf(out, out_capacity, "/%s", child != nullptr ? child : "");
        return;
    }

    snprintf(out, out_capacity, "%s/%s", parent, child != nullptr ? child : "");
}

static void browser_parent_path(char* path) {
    if (path == nullptr || path[0] == '\0' || strcmp(path, "/") == 0)
        return;

    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        --length;
    while (length > 1 && path[length - 1] != '/')
        --length;

    if (length <= 1) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    path[length - 1] = '\0';
}

static bool browser_parse_item_record(const char* record, BrowserEntry* entry) {
    if (record == nullptr || entry == nullptr || (record[0] != 'D' && record[0] != 'F') || record[1] != '\t')
        return false;

    const char* second_tab = strchr(record + 2, '\t');
    if (second_tab == nullptr || second_tab <= record + 2)
        return false;

    const size_t name_length = static_cast<size_t>(second_tab - (record + 2));
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name,
           record + 2,
           name_length < sizeof(entry->name) - 1 ? name_length : sizeof(entry->name) - 1);
    entry->is_directory = record[0] == 'D';
    entry->size_bytes = parse_u64_decimal(second_tab + 1);
    return entry->name[0] != '\0';
}

static void browser_sort_entries(BrowserState* browser) {
    if (browser == nullptr)
        return;

    for (vk_u32 index = 1; index < browser->entry_count; ++index) {
        const BrowserEntry entry = browser->entries[index];
        vk_u32 insert_index = index;

        while (insert_index > 0) {
            const BrowserEntry& previous = browser->entries[insert_index - 1];
            const bool directories_first = entry.is_directory && !previous.is_directory;
            const bool names_precede = entry.is_directory == previous.is_directory
                                    && compare_casefolded(entry.name, previous.name) < 0;
            if (!directories_first && !names_precede)
                break;

            browser->entries[insert_index] = previous;
            --insert_index;
        }

        browser->entries[insert_index] = entry;
    }
}

static void browser_query_default_path(BrowserState* browser) {
    if (browser == nullptr)
        return;

    memset(browser->response, 0, sizeof(browser->response));
    vk_kobj_rpc_path_json("get", "fs/root_path", browser->response, sizeof(browser->response));
    if (!vk_kobj_response_ok(browser->response)
        || !vk_json_extract_string_field(browser->response, "value", browser->current_path, sizeof(browser->current_path))
        || browser->current_path[0] == '\0') {
        copy_string(browser->current_path, sizeof(browser->current_path), "/");
    }
}

static bool browser_refresh_listing(AppState* app) {
    auto* browser = &app->browser;
    memset(browser->response, 0, sizeof(browser->response));
    memset(browser->raw_items, 0, sizeof(browser->raw_items));

    vk_kobj_rpc_path_json("fs_list", browser->current_path, browser->response, sizeof(browser->response));
    if (!vk_kobj_response_ok(browser->response)) {
        char error[kBrowserStatusMax];
        browser->entry_count = 0;
        browser->selected_index = 0;
        browser->scroll_index = 0;
        if (vk_json_extract_string_field(browser->response, "error", error, sizeof(error))) {
            browser_set_status(browser, "%s", error);
        } else {
            browser_set_status(browser, "FAILED TO LIST DIRECTORY");
        }
        return false;
    }

    const int item_count = vk_json_extract_string_array_field(browser->response,
                                                              "items",
                                                              &browser->raw_items[0][0],
                                                              kBrowserItemMax,
                                                              static_cast<int>(kBrowserMaxEntries));

    browser->entry_count = 0;
    browser->selected_index = 0;
    browser->scroll_index = 0;

    for (int index = 0; index < item_count && browser->entry_count < kBrowserMaxEntries; ++index) {
        BrowserEntry entry;
        if (!browser_parse_item_record(browser->raw_items[index], &entry))
            continue;
        if (!entry.is_directory && !browser_is_supported_track(entry.name, entry.size_bytes))
            continue;
        browser->entries[browser->entry_count++] = entry;
    }

    browser_sort_entries(browser);
    if (browser->entry_count == 0) {
        browser_set_status(browser, "NO MP3 FILES IN THIS DIRECTORY");
    } else {
        browser_set_status(browser,
                           "%u ITEM%s",
                           browser->entry_count,
                           browser->entry_count == 1 ? "" : "S");
    }
    return true;
}

static const unsigned char kUiGlyphs[][kUiGlyphHeight] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x00},
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    {0x0E,0x11,0x19,0x15,0x13,0x11,0x0E},
    {0x04,0x06,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x10,0x0C,0x02,0x01,0x1F},
    {0x1F,0x08,0x04,0x0C,0x10,0x11,0x0E},
    {0x08,0x0C,0x0A,0x09,0x1F,0x08,0x08},
    {0x1F,0x01,0x0F,0x10,0x10,0x11,0x0E},
    {0x0C,0x02,0x01,0x0F,0x11,0x11,0x0E},
    {0x1F,0x10,0x08,0x04,0x02,0x02,0x02},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x1E,0x10,0x08,0x06},
    {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11},
    {0x0F,0x11,0x11,0x0F,0x11,0x11,0x0F},
    {0x0E,0x11,0x01,0x01,0x01,0x11,0x0E},
    {0x0F,0x11,0x11,0x11,0x11,0x11,0x0F},
    {0x1F,0x01,0x01,0x0F,0x01,0x01,0x1F},
    {0x1F,0x01,0x01,0x0F,0x01,0x01,0x01},
    {0x0E,0x11,0x01,0x1D,0x11,0x11,0x1E},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x1C,0x08,0x08,0x08,0x08,0x09,0x06},
    {0x11,0x09,0x05,0x03,0x05,0x09,0x11},
    {0x01,0x01,0x01,0x01,0x01,0x01,0x1F},
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    {0x11,0x13,0x15,0x19,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x0F,0x11,0x11,0x0F,0x01,0x01,0x01},
    {0x0E,0x11,0x11,0x11,0x15,0x09,0x16},
    {0x0F,0x11,0x11,0x0F,0x05,0x09,0x11},
    {0x0E,0x11,0x01,0x0E,0x10,0x11,0x0E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x10,0x08,0x04,0x02,0x01,0x1F},
};

static const unsigned char* ui_glyph_for(char ch) {
    int index = 0;

    if (ch == '/' || ch == '\\') index = 1;
    else if (ch == ':') index = 2;
    else if (ch == '.') index = 3;
    else if (ch == '-') index = 4;
    else if (ch == '_') index = 5;
    else if (ch >= '0' && ch <= '9') index = 6 + (ch - '0');
    else if (ch >= 'A' && ch <= 'Z') index = 16 + (ch - 'A');
    else if (ch >= 'a' && ch <= 'z') index = 16 + (ch - 'a');

    return kUiGlyphs[index];
}

static vk_u32 pack_pixel(unsigned char r, unsigned char g, unsigned char b) {
    return (static_cast<vk_u32>(r) << 16) | (static_cast<vk_u32>(g) << 8) | static_cast<vk_u32>(b);
}

static void ui_present(const AppState* app) {
    if (app == nullptr || app->present_buffer == nullptr)
        return;

    memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(app->framebuffer.base)),
           app->present_buffer,
           app->present_pixels * sizeof(vk_u32));
}

static void ui_fill_rect(AppState* app, int x, int y, int width, int height, vk_u32 color) {
    if (app == nullptr || app->present_buffer == nullptr || width <= 0 || height <= 0)
        return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width;
    int y1 = y + height;
    const int max_width = static_cast<int>(app->framebuffer.width);
    const int max_height = static_cast<int>(app->framebuffer.height);

    if (x1 > max_width)
        x1 = max_width;
    if (y1 > max_height)
        y1 = max_height;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (int row = y0; row < y1; ++row) {
        vk_u32* dst = &app->present_buffer[(static_cast<vk_usize>(row) * app->framebuffer.stride) + static_cast<vk_usize>(x0)];
        for (int column = x0; column < x1; ++column)
            dst[column - x0] = color;
    }
}

static void ui_draw_background(AppState* app) {
    if (app == nullptr || app->present_buffer == nullptr)
        return;

    for (vk_u32 y = 0; y < app->framebuffer.height; ++y) {
        const unsigned char red = static_cast<unsigned char>(10u + (y * 14u) / max_u32(app->framebuffer.height, 1));
        const unsigned char green = static_cast<unsigned char>(14u + (y * 20u) / max_u32(app->framebuffer.height, 1));
        const unsigned char blue = static_cast<unsigned char>(24u + (y * 40u) / max_u32(app->framebuffer.height, 1));
        const vk_u32 color = pack_pixel(red, green, blue);
        vk_u32* row = &app->present_buffer[static_cast<vk_usize>(y) * app->framebuffer.stride];
        for (vk_u32 x = 0; x < app->framebuffer.width; ++x)
            row[x] = color;
    }
}

static void ui_draw_char(AppState* app, int x, int y, char ch, vk_u32 color, int scale) {
    if (app == nullptr || scale <= 0)
        return;

    const unsigned char* glyph = ui_glyph_for(ch);
    for (int row = 0; row < static_cast<int>(kUiGlyphHeight); ++row) {
        for (int column = 0; column < static_cast<int>(kUiGlyphWidth); ++column) {
            if ((glyph[row] & (0x01u << column)) == 0)
                continue;
            ui_fill_rect(app,
                         x + column * scale,
                         y + row * scale,
                         scale,
                         scale,
                         color);
        }
    }
}

static int ui_draw_text(AppState* app, int x, int y, const char* text, vk_u32 color, int scale) {
    int cursor_x = x;

    if (text == nullptr)
        return cursor_x;

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        ui_draw_char(app, cursor_x, y, *cursor, color, scale);
        cursor_x += static_cast<int>((kUiGlyphWidth + 1u) * static_cast<vk_u32>(scale));
    }
    return cursor_x;
}

static void ui_draw_text_clipped(AppState* app,
                                 int x,
                                 int y,
                                 const char* text,
                                 vk_u32 color,
                                 int scale,
                                 int max_chars) {
    if (text == nullptr || max_chars <= 0)
        return;

    int cursor_x = x;
    int count = 0;
    for (const char* cursor = text; *cursor != '\0' && count < max_chars; ++cursor, ++count) {
        ui_draw_char(app, cursor_x, y, *cursor, color, scale);
        cursor_x += static_cast<int>((kUiGlyphWidth + 1u) * static_cast<vk_u32>(scale));
    }
}

static void ui_draw_progress_bar(AppState* app,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 vk_u32 border_color,
                                 vk_u32 fill_color,
                                 vk_u32 background_color,
                                 vk_u64 numerator,
                                 vk_u64 denominator) {
    if (app == nullptr || width <= 2 || height <= 2)
        return;

    ui_fill_rect(app, x, y, width, height, border_color);
    ui_fill_rect(app, x + 1, y + 1, width - 2, height - 2, background_color);

    if (denominator == 0)
        return;

    const vk_u64 clamped = numerator > denominator ? denominator : numerator;
    const int inner_width = width - 2;
    const int filled_width = static_cast<int>((clamped * static_cast<vk_u64>(inner_width)) / denominator);
    if (filled_width > 0)
        ui_fill_rect(app, x + 1, y + 1, filled_width, height - 2, fill_color);
}

static void ui_draw_vu_meter(AppState* app,
                             int x,
                             int y,
                             int width,
                             int height,
                             const char* label,
                             vk_u32 level,
                             vk_u32 label_color,
                             vk_u32 border_color,
                             vk_u32 background_color) {
    if (app == nullptr || width <= 18 || height <= 2)
        return;

    const int label_width = 10;
    const int bar_x = x + label_width;
    const int bar_width = width - label_width;
    const int inner_width = bar_width - 2;
    const int green_end = (inner_width * 70) / 100;
    const int amber_end = (inner_width * 90) / 100;
    const int filled_width = static_cast<int>((min_u32(level, 255u) * static_cast<vk_u32>(inner_width)) / 255u);
    const vk_u32 low_color = pack_pixel(110, 228, 158);
    const vk_u32 mid_color = pack_pixel(255, 208, 96);
    const vk_u32 high_color = pack_pixel(255, 118, 88);

    ui_draw_text(app, x, y + 2, label, label_color, 1);
    ui_fill_rect(app, bar_x, y, bar_width, height, border_color);
    ui_fill_rect(app, bar_x + 1, y + 1, bar_width - 2, height - 2, background_color);

    if (filled_width <= 0)
        return;

    const int low_width = filled_width < green_end ? filled_width : green_end;
    const int mid_width = filled_width > green_end
        ? (filled_width < amber_end ? filled_width : amber_end) - green_end
        : 0;
    const int high_width = filled_width > amber_end ? filled_width - amber_end : 0;

    if (low_width > 0)
        ui_fill_rect(app, bar_x + 1, y + 1, low_width, height - 2, low_color);
    if (mid_width > 0)
        ui_fill_rect(app, bar_x + 1 + green_end, y + 1, mid_width, height - 2, mid_color);
    if (high_width > 0)
        ui_fill_rect(app, bar_x + 1 + amber_end, y + 1, high_width, height - 2, high_color);
}

static void browser_format_size(char* out, size_t out_capacity, vk_u64 size_bytes) {
    const vk_u64 kib = 1024u;
    const vk_u64 mib = 1024u * 1024u;

    if (out == nullptr || out_capacity == 0)
        return;

    if (size_bytes >= mib) {
        snprintf(out, out_capacity, "%lluM", static_cast<unsigned long long>((size_bytes + (mib / 2u)) / mib));
    } else if (size_bytes >= kib) {
        snprintf(out, out_capacity, "%lluK", static_cast<unsigned long long>((size_bytes + (kib / 2u)) / kib));
    } else {
        snprintf(out, out_capacity, "%lluB", static_cast<unsigned long long>(size_bytes));
    }
}

static void browser_move_selection(BrowserState* browser, int delta) {
    if (browser == nullptr || browser->entry_count == 0 || delta == 0)
        return;

    int next_index = static_cast<int>(browser->selected_index) + delta;
    if (next_index < 0)
        next_index = 0;
    if (next_index >= static_cast<int>(browser->entry_count))
        next_index = static_cast<int>(browser->entry_count) - 1;
    browser->selected_index = static_cast<vk_u32>(next_index);
}

static void render_browser(AppState* app) {
    auto* browser = &app->browser;
    const int screen_width = static_cast<int>(app->framebuffer.width);
    const int screen_height = static_cast<int>(app->framebuffer.height);
    const int panel_x = kUiPanelMargin;
    const int panel_y = kUiPanelMargin;
    const int panel_width = screen_width - (kUiPanelMargin * 2);
    const int panel_height = screen_height - (kUiPanelMargin * 2);
    const int header_y = panel_y + kUiPanelPadding;
    const int content_x = panel_x + kUiPanelPadding;
    const int content_width = panel_width - (kUiPanelPadding * 2);
    const int content_y = panel_y + static_cast<int>(kUiHeaderHeight);
    const int content_height = panel_height - static_cast<int>(kUiHeaderHeight) - static_cast<int>(kUiFooterHeight);
    const vk_u32 panel_bg = pack_pixel(18, 26, 42);
    const vk_u32 panel_border = pack_pixel(82, 140, 210);
    const vk_u32 header_bg = pack_pixel(24, 36, 58);
    const vk_u32 footer_bg = pack_pixel(22, 30, 46);
    const vk_u32 title_fg = pack_pixel(255, 230, 170);
    const vk_u32 path_fg = pack_pixel(150, 210, 255);
    const vk_u32 text_fg = pack_pixel(245, 245, 245);
    const vk_u32 dim_fg = pack_pixel(168, 186, 210);
    const vk_u32 dir_fg = pack_pixel(120, 225, 200);
    const vk_u32 selected_bg = pack_pixel(58, 80, 118);
    const vk_u32 selected_fg = pack_pixel(255, 255, 255);
    const vk_u32 selected_dir_fg = pack_pixel(185, 255, 232);
    const vk_u32 empty_fg = pack_pixel(255, 190, 120);
    const int rows_visible = static_cast<int>(max_u32(1u, static_cast<vk_u32>(content_height / static_cast<int>(kUiRowHeight))));

    if (browser->selected_index < browser->scroll_index)
        browser->scroll_index = browser->selected_index;
    if (browser->selected_index >= browser->scroll_index + static_cast<vk_u32>(rows_visible))
        browser->scroll_index = browser->selected_index - static_cast<vk_u32>(rows_visible) + 1u;

    ui_draw_background(app);
    ui_fill_rect(app, panel_x, panel_y, panel_width, panel_height, panel_bg);
    ui_fill_rect(app, panel_x, panel_y, panel_width, 2, panel_border);
    ui_fill_rect(app, panel_x, panel_y + panel_height - 2, panel_width, 2, panel_border);
    ui_fill_rect(app, panel_x, panel_y, 2, panel_height, panel_border);
    ui_fill_rect(app, panel_x + panel_width - 2, panel_y, 2, panel_height, panel_border);
    ui_fill_rect(app, panel_x + 2, panel_y + 2, panel_width - 4, static_cast<int>(kUiHeaderHeight) - 2, header_bg);
    ui_fill_rect(app,
                 panel_x + 2,
                 panel_y + panel_height - static_cast<int>(kUiFooterHeight),
                 panel_width - 4,
                 static_cast<int>(kUiFooterHeight) - 2,
                 footer_bg);

    ui_draw_text(app, content_x, header_y, "SELECT MP3", title_fg, static_cast<int>(kUiFontScale));
    ui_draw_text_clipped(app,
                         content_x,
                         header_y + 28,
                         browser->current_path,
                         path_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));

    char count_text[32];
    snprintf(count_text,
             sizeof(count_text),
             "%u/%u",
             browser->entry_count == 0 ? 0u : browser->selected_index + 1u,
             browser->entry_count);
    const int count_width = static_cast<int>(strlen(count_text)) * static_cast<int>(kUiGlyphWidth + 1u);
    ui_draw_text(app,
                 panel_x + panel_width - kUiPanelPadding - count_width,
                 header_y + 30,
                 count_text,
                 dim_fg,
                 1);

    if (browser->entry_count == 0) {
        ui_draw_text_clipped(app,
                             content_x,
                             content_y + 8,
                             "NO MP3 FILES FOUND",
                             empty_fg,
                             static_cast<int>(kUiFontScale),
                             static_cast<int>(max_u32(1u,
                                 static_cast<vk_u32>(content_width / static_cast<int>(kUiFontAdvance)))));
        ui_draw_text_clipped(app,
                             content_x,
                             content_y + 40,
                             "SUPPORTED: .MP3",
                             dim_fg,
                             1,
                             static_cast<int>(max_u32(1u,
                                 static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));
    } else {
        const int size_column_chars = 6;
        const int size_column_width = size_column_chars * static_cast<int>(kUiGlyphWidth + 1u);
        const int name_char_limit = static_cast<int>(max_u32(1u,
            static_cast<vk_u32>((content_width - size_column_width - 16) / static_cast<int>(kUiGlyphWidth + 1u))));

        for (int row = 0; row < rows_visible; ++row) {
            const vk_u32 entry_index = browser->scroll_index + static_cast<vk_u32>(row);
            if (entry_index >= browser->entry_count)
                break;

            const BrowserEntry& entry = browser->entries[entry_index];
            const int row_y = content_y + row * static_cast<int>(kUiRowHeight);
            const bool selected = entry_index == browser->selected_index;
            const vk_u32 row_fg = selected ? selected_fg : text_fg;
            const vk_u32 name_fg = entry.is_directory ? (selected ? selected_dir_fg : dir_fg) : row_fg;
            char label[kBrowserNameMax + 2];
            char size_label[16];

            snprintf(label, sizeof(label), "%s%s", entry.name, entry.is_directory ? "/" : "");

            if (selected)
                ui_fill_rect(app, content_x - 8, row_y - 4, content_width + 16, static_cast<int>(kUiRowHeight) - 2, selected_bg);
            ui_fill_rect(app, content_x - 8, row_y - 4, 4, static_cast<int>(kUiRowHeight) - 2, selected ? title_fg : panel_border);

            ui_draw_text_clipped(app,
                                 content_x,
                                 row_y,
                                 label,
                                 name_fg,
                                 1,
                                 name_char_limit);

            if (entry.is_directory) {
                copy_string(size_label, sizeof(size_label), "DIR");
            } else {
                browser_format_size(size_label, sizeof(size_label), entry.size_bytes);
            }

            const int size_width = static_cast<int>(strlen(size_label)) * static_cast<int>(kUiGlyphWidth + 1u);
            ui_draw_text(app,
                         panel_x + panel_width - kUiPanelPadding - size_width,
                         row_y,
                         size_label,
                         selected ? selected_fg : dim_fg,
                         1);
        }
    }

    ui_draw_text_clipped(app,
                         content_x,
                         panel_y + panel_height - static_cast<int>(kUiFooterHeight) + 6,
                         browser->status,
                         text_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));
    ui_draw_text_clipped(app,
                         content_x,
                         panel_y + panel_height - static_cast<int>(kUiFooterHeight) + 16,
                         "ARROWS MOVE  ENTER OPEN  BACKSPACE UP  ESC QUIT",
                         dim_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));

    ui_present(app);
}

static void browser_go_parent(AppState* app) {
    auto* browser = &app->browser;
    char parent_path[kBrowserPathMax];

    copy_string(parent_path, sizeof(parent_path), browser->current_path);
    browser_parent_path(parent_path);
    if (strcmp(parent_path, browser->current_path) == 0) {
        browser_set_status(browser, "ALREADY AT ROOT");
        return;
    }

    copy_string(browser->current_path, sizeof(browser->current_path), parent_path);
    browser_refresh_listing(app);
}

static bool browser_open_selection(AppState* app, char* out_path, size_t out_path_capacity) {
    auto* browser = &app->browser;

    if (out_path == nullptr || out_path_capacity == 0)
        return false;
    if (browser->entry_count == 0 || browser->selected_index >= browser->entry_count)
        return false;

    const BrowserEntry& entry = browser->entries[browser->selected_index];
    char path[kBrowserPathMax];
    browser_join_path(path, sizeof(path), browser->current_path, entry.name);

    if (entry.is_directory) {
        copy_string(browser->current_path, sizeof(browser->current_path), path);
        browser_refresh_listing(app);
        return false;
    }

    copy_string(out_path, out_path_capacity, path);
    browser_set_status(browser, "SELECTED %s", path_basename(path));
    return true;
}

static void browser_init(AppState* app) {
    auto* browser = &app->browser;
    memset(browser, 0, sizeof(*browser));
    browser_query_default_path(browser);
    browser_refresh_listing(app);
}

static bool browse_for_track(AppState* app, char* out_path, size_t out_path_capacity) {
    bool dirty = true;

    while (!app->quit_requested) {
        if (dirty) {
            render_browser(app);
            dirty = false;
        }

        vk_key_event_t event;
        bool saw_input = false;
        while (VK_CALL(poll_key, &event)) {
            saw_input = true;
            if (!event.pressed)
                continue;

            switch (event.scancode) {
                case 0x01:
                    app->quit_requested = true;
                    return false;
                case 0xC8:
                case 0x48:
                    browser_move_selection(&app->browser, -1);
                    dirty = true;
                    break;
                case 0xD0:
                case 0x50:
                    browser_move_selection(&app->browser, 1);
                    dirty = true;
                    break;
                case 0xCB:
                case 0x4B:
                case 0x0E:
                    browser_go_parent(app);
                    dirty = true;
                    break;
                case 0xCD:
                case 0x4D:
                case 0x1C:
                    if (browser_open_selection(app, out_path, out_path_capacity))
                        return true;
                    dirty = true;
                    break;
                case 0x0F:
                    browser_refresh_listing(app);
                    dirty = true;
                    break;
                default:
                    break;
            }
        }

        if (!saw_input)
            VK_CALL(yield);
    }

    return false;
}

static const char* player_error_text(int code) {
    switch (code) {
        case MP3D_E_PARAM:
            return "BAD MP3 PARAMETERS";
        case MP3D_E_MEMORY:
            return "OUT OF MEMORY";
        case MP3D_E_IOERROR:
            return "MP3 I/O ERROR";
        case MP3D_E_USER:
            return "USER STOP";
        case MP3D_E_DECODE:
            return "MP3 DECODE ERROR";
        default:
            return "MP3 ERROR";
    }
}

static size_t player_read_callback(void* buf, size_t size, void* user_data) {
    auto* stream = static_cast<FileStreamState*>(user_data);
    if (buf == nullptr || size == 0 || stream == nullptr || stream->file == nullptr)
        return 0;
    return fread(buf, 1, size, stream->file);
}

static int player_seek_callback(uint64_t position, void* user_data) {
    auto* stream = static_cast<FileStreamState*>(user_data);
    if (stream == nullptr || stream->file == nullptr)
        return -1;
    if (position > static_cast<uint64_t>(LONG_MAX))
        return -1;
    return fseek(stream->file, static_cast<long>(position), SEEK_SET) == 0 ? 0 : -1;
}

static void player_reset_queue(PlayerState* player) {
    if (player == nullptr)
        return;

    player->audio.queue_read = 0;
    player->audio.queue_write = 0;
    player->audio.queue_count = 0;
    memset(player->audio.queue, 0, sizeof(player->audio.queue));
    memset(player->audio.play_block, 0, sizeof(player->audio.play_block));
    player->current_block_active = false;
    player->current_block_frames = 0;
}

static void player_close_decoder(PlayerState* player) {
    if (player == nullptr)
        return;

    VK_CALL(snd_stop);
    player_reset_queue(player);

    if (player->decoder_open) {
        mp3dec_ex_close(&player->decoder);
        player->decoder_open = false;
    }

    if (player->stream.file != nullptr) {
        fclose(player->stream.file);
        player->stream.file = nullptr;
    }

    player->source_channels = 0;
    player->sample_rate = 0;
    player->bitrate_kbps = 0;
    player->end_of_stream = false;
    player->playback_failed = false;
    player->vu_left = 0;
    player->vu_right = 0;
    player->completed_frames = 0;
    player->total_frames = 0;
}

static vk_u32 player_scale_sample_level(int16_t sample) {
    const int value = static_cast<int>(sample);
    const vk_u32 magnitude = static_cast<vk_u32>(value < 0 ? -value : value);
    const vk_u32 scaled = magnitude >> 7;
    return scaled > 255u ? 255u : scaled;
}

static void player_update_vu_from_block(PlayerState* player, const int16_t* samples, vk_u32 frames) {
    if (player == nullptr || samples == nullptr || frames == 0) {
        return;
    }

    vk_u64 left_total = 0;
    vk_u64 right_total = 0;
    vk_u32 left_peak = 0;
    vk_u32 right_peak = 0;

    for (vk_u32 frame = 0; frame < frames; ++frame) {
        const vk_u32 left = player_scale_sample_level(samples[frame * 2]);
        const vk_u32 right = player_scale_sample_level(samples[frame * 2 + 1]);
        left_total += left;
        right_total += right;
        if (left > left_peak)
            left_peak = left;
        if (right > right_peak)
            right_peak = right;
    }

    const vk_u32 left_average = static_cast<vk_u32>(left_total / frames);
    const vk_u32 right_average = static_cast<vk_u32>(right_total / frames);
    player->vu_left = min_u32(255u, (left_average + (left_peak * 3u)) / 4u);
    player->vu_right = min_u32(255u, (right_average + (right_peak * 3u)) / 4u);
}

static bool player_push_stereo_frame(PlayerState* player, int16_t left, int16_t right) {
    if (player == nullptr || player->audio.queue_count >= kQueueCapacityFrames)
        return false;

    player->audio.queue[player->audio.queue_write * 2] = left;
    player->audio.queue[player->audio.queue_write * 2 + 1] = right;
    player->audio.queue_write = (player->audio.queue_write + 1) % kQueueCapacityFrames;
    ++player->audio.queue_count;
    return true;
}

static bool player_decode_more(PlayerState* player, vk_u32 requested_frames) {
    if (player == nullptr || !player->decoder_open || player->end_of_stream || player->playback_failed)
        return false;

    const vk_u32 free_frames = kQueueCapacityFrames - player->audio.queue_count;
    const vk_u32 decode_frames = min_u32(requested_frames, free_frames);
    if (decode_frames == 0 || player->source_channels < 1 || player->source_channels > 2)
        return false;

    const size_t requested_samples = static_cast<size_t>(decode_frames) * static_cast<size_t>(player->source_channels);
    const size_t decoded_samples = mp3dec_ex_read(&player->decoder,
                                                  reinterpret_cast<mp3d_sample_t*>(player->decode_scratch),
                                                  requested_samples);

    if (decoded_samples == 0) {
        if (player->decoder.last_error != 0) {
            player->playback_failed = true;
            player_set_status(player, "%s", player_error_text(player->decoder.last_error));
        } else {
            player->end_of_stream = true;
        }
        return false;
    }

    const vk_u32 frames = static_cast<vk_u32>(decoded_samples / static_cast<size_t>(player->source_channels));
    if (frames == 0)
        return false;

    if (player->source_channels == 1) {
        for (vk_u32 frame = 0; frame < frames; ++frame) {
            const int16_t sample = player->decode_scratch[frame];
            if (!player_push_stereo_frame(player, sample, sample))
                break;
        }
    } else {
        for (vk_u32 frame = 0; frame < frames; ++frame) {
            const int16_t left = player->decode_scratch[frame * 2];
            const int16_t right = player->decode_scratch[frame * 2 + 1];
            if (!player_push_stereo_frame(player, left, right))
                break;
        }
    }

    return true;
}

static void player_fill_queue(PlayerState* player, vk_u32 target_frames) {
    if (player == nullptr)
        return;

    if (target_frames > kQueueCapacityFrames)
        target_frames = kQueueCapacityFrames;

    while (!player->end_of_stream && !player->playback_failed && player->audio.queue_count < target_frames) {
        const vk_u32 remaining = target_frames - player->audio.queue_count;
        if (!player_decode_more(player, min_u32(kDecodeChunkFrames, remaining)))
            break;
    }
}

static vk_u32 player_pop_frames(PlayerState* player, int16_t* output, vk_u32 requested_frames) {
    if (player == nullptr || output == nullptr)
        return 0;

    const vk_u32 total_frames = min_u32(requested_frames, player->audio.queue_count);
    for (vk_u32 frame = 0; frame < total_frames; ++frame) {
        output[frame * 2] = player->audio.queue[player->audio.queue_read * 2];
        output[frame * 2 + 1] = player->audio.queue[player->audio.queue_read * 2 + 1];
        player->audio.queue_read = (player->audio.queue_read + 1) % kQueueCapacityFrames;
    }
    player->audio.queue_count -= total_frames;
    return total_frames;
}

static void player_reconcile_current_block(PlayerState* player) {
    if (player == nullptr || !player->current_block_active)
        return;
    if (VK_CALL(snd_is_playing))
        return;

    player->completed_frames += player->current_block_frames;
    player->current_block_active = false;
    player->current_block_frames = 0;
}

static void player_try_submit(PlayerState* player) {
    if (player == nullptr || player->playback_failed)
        return;

    player_reconcile_current_block(player);
    if (player->current_block_active || player->audio.queue_count == 0)
        return;

    const vk_u32 frames = player_pop_frames(player, player->audio.play_block, kPlayBlockFrames);
    if (frames == 0)
        return;

    player_update_vu_from_block(player, player->audio.play_block, frames);

    if (!VK_CALL(snd_play,
                 player->audio.play_block,
                 frames * 2u * static_cast<vk_u32>(sizeof(int16_t)),
                 VK_SND_FORMAT_SIGNED_16_STEREO)) {
        player->playback_failed = true;
        player_set_status(player, "FAILED TO START AUDIO");
        player->current_block_active = false;
        player->current_block_frames = 0;
        return;
    }

    player->current_block_active = true;
    player->current_block_frames = frames;
}

static bool player_open_track(PlayerState* player, const char* path) {
    if (player == nullptr || path == nullptr || path[0] == '\0')
        return false;

    player_close_decoder(player);
    memset(player, 0, sizeof(*player));
    copy_string(player->loaded_path, sizeof(player->loaded_path), path);

    player->stream.file = fopen(path, "rb");
    if (player->stream.file == nullptr) {
        player_set_status(player, "FAILED TO OPEN %s", path_basename(path));
        return false;
    }

    player->io.read = player_read_callback;
    player->io.read_data = &player->stream;
    player->io.seek = player_seek_callback;
    player->io.seek_data = &player->stream;

    const int open_result = mp3dec_ex_open_cb(&player->decoder, &player->io, MP3D_DO_NOT_SCAN);
    if (open_result < 0) {
        player_set_status(player, "%s", player_error_text(open_result));
        fclose(player->stream.file);
        player->stream.file = nullptr;
        return false;
    }

    player->decoder_open = true;
    player->source_channels = player->decoder.info.channels;
    player->sample_rate = player->decoder.info.hz;
    player->bitrate_kbps = player->decoder.info.bitrate_kbps;
    if (player->source_channels > 0)
        player->total_frames = player->decoder.samples / static_cast<vk_u64>(player->source_channels);

    if (player->source_channels < 1 || player->source_channels > 2 || player->sample_rate <= 0) {
        player_set_status(player, "UNSUPPORTED MP3 STREAM");
        player_close_decoder(player);
        return false;
    }

    if (!VK_CALL(snd_set_sample_rate, static_cast<vk_u32>(player->sample_rate))) {
        player_set_status(player, "UNSUPPORTED SAMPLE RATE");
        player_close_decoder(player);
        return false;
    }

    VK_CALL(snd_set_volume, 255, 255);
    player_set_status(player, "PLAYING %s", path_basename(path));
    player_fill_queue(player, kQueuePrimeFrames);
    player_try_submit(player);

    if (player->playback_failed) {
        player_close_decoder(player);
        return false;
    }
    if (player->audio.queue_count == 0 && player->end_of_stream && !player->current_block_active) {
        player_set_status(player, "NO DECODED AUDIO");
        player_close_decoder(player);
        return false;
    }

    return true;
}

static void format_clock_text(char* out, size_t out_capacity, vk_u64 frames, int sample_rate) {
    if (out == nullptr || out_capacity == 0) {
        return;
    }
    if (sample_rate <= 0) {
        copy_string(out, out_capacity, "--:--");
        return;
    }

    const vk_u64 total_seconds = frames / static_cast<vk_u64>(sample_rate);
    const vk_u64 minutes = total_seconds / 60u;
    const vk_u64 seconds = total_seconds % 60u;
    snprintf(out,
             out_capacity,
             "%llu:%02llu",
             static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
}

static const char* player_channel_label(int channels) {
    return channels == 1 ? "MONO" : "STEREO";
}

static void render_player_screen(AppState* app) {
    auto* player = &app->player;
    const int screen_width = static_cast<int>(app->framebuffer.width);
    const int screen_height = static_cast<int>(app->framebuffer.height);
    const int panel_x = kUiPanelMargin;
    const int panel_y = kUiPanelMargin;
    const int panel_width = screen_width - (kUiPanelMargin * 2);
    const int panel_height = screen_height - (kUiPanelMargin * 2);
    const int header_y = panel_y + kUiPanelPadding;
    const int content_x = panel_x + kUiPanelPadding;
    const int content_width = panel_width - (kUiPanelPadding * 2);
    const int content_y = panel_y + static_cast<int>(kUiHeaderHeight);
    const vk_u32 panel_bg = pack_pixel(18, 26, 42);
    const vk_u32 panel_border = pack_pixel(82, 140, 210);
    const vk_u32 header_bg = pack_pixel(24, 36, 58);
    const vk_u32 footer_bg = pack_pixel(22, 30, 46);
    const vk_u32 title_fg = pack_pixel(255, 230, 170);
    const vk_u32 path_fg = pack_pixel(150, 210, 255);
    const vk_u32 text_fg = pack_pixel(245, 245, 245);
    const vk_u32 dim_fg = pack_pixel(168, 186, 210);
    const vk_u32 accent_fg = pack_pixel(120, 225, 200);
    const vk_u32 empty_fg = pack_pixel(255, 190, 120);
    const vk_u32 progress_bg = pack_pixel(44, 54, 76);
    const vk_u32 progress_fill = pack_pixel(255, 230, 170);
    const vk_u32 progress_border = pack_pixel(82, 140, 210);
    const vk_u32 meter_bg = pack_pixel(32, 42, 60);
    const vk_u64 visible_frames = player->completed_frames;
    const vk_u32 buffered_frames = player->audio.queue_count + (player->current_block_active ? player->current_block_frames : 0u);
    char info_line[64];
    char progress_line[64];
    char queue_line[64];
    char elapsed_text[24];
    char total_text[24];

    format_clock_text(elapsed_text, sizeof(elapsed_text), visible_frames, player->sample_rate);
    if (player->total_frames != 0) {
        format_clock_text(total_text, sizeof(total_text), player->total_frames, player->sample_rate);
        snprintf(progress_line, sizeof(progress_line), "%s / %s", elapsed_text, total_text);
    } else {
        copy_string(progress_line, sizeof(progress_line), elapsed_text);
    }

    if (player->bitrate_kbps > 0) {
        snprintf(info_line,
                 sizeof(info_line),
                 "%d HZ  %s  %d KBPS",
                 player->sample_rate,
                 player_channel_label(player->source_channels),
                 player->bitrate_kbps);
    } else {
        snprintf(info_line,
                 sizeof(info_line),
                 "%d HZ  %s",
                 player->sample_rate,
                 player_channel_label(player->source_channels));
    }
    snprintf(queue_line, sizeof(queue_line), "%u FRAMES BUFFERED", buffered_frames);

    ui_draw_background(app);
    ui_fill_rect(app, panel_x, panel_y, panel_width, panel_height, panel_bg);
    ui_fill_rect(app, panel_x, panel_y, panel_width, 2, panel_border);
    ui_fill_rect(app, panel_x, panel_y + panel_height - 2, panel_width, 2, panel_border);
    ui_fill_rect(app, panel_x, panel_y, 2, panel_height, panel_border);
    ui_fill_rect(app, panel_x + panel_width - 2, panel_y, 2, panel_height, panel_border);
    ui_fill_rect(app, panel_x + 2, panel_y + 2, panel_width - 4, static_cast<int>(kUiHeaderHeight) - 2, header_bg);
    ui_fill_rect(app,
                 panel_x + 2,
                 panel_y + panel_height - static_cast<int>(kUiFooterHeight),
                 panel_width - 4,
                 static_cast<int>(kUiFooterHeight) - 2,
                 footer_bg);

    ui_draw_text(app, content_x, header_y, "PLAY MP3", title_fg, static_cast<int>(kUiFontScale));
    ui_draw_text_clipped(app,
                         content_x,
                         header_y + 28,
                         player->loaded_path,
                         path_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));

    ui_draw_text_clipped(app,
                         content_x,
                         content_y + 8,
                         path_basename(player->loaded_path),
                         text_fg,
                         static_cast<int>(kUiFontScale),
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiFontAdvance)))));
    ui_draw_text_clipped(app,
                         content_x,
                         content_y + 42,
                         info_line,
                         accent_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));
    ui_draw_text_clipped(app,
                         content_x,
                         content_y + 62,
                         progress_line,
                         text_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));

    if (player->total_frames != 0) {
        ui_draw_progress_bar(app,
                             content_x,
                             content_y + 84,
                             content_width,
                             16,
                             progress_border,
                             progress_fill,
                             progress_bg,
                             visible_frames,
                             player->total_frames);
    } else {
        ui_draw_text_clipped(app,
                             content_x,
                             content_y + 86,
                             "TRACK LENGTH UNKNOWN",
                             dim_fg,
                             1,
                             static_cast<int>(max_u32(1u,
                                 static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));
    }

    ui_draw_text_clipped(app,
                         content_x,
                         content_y + 114,
                         queue_line,
                         dim_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));
    ui_draw_text_clipped(app,
                         content_x,
                         content_y + 132,
                         "OUTPUT LEVEL",
                         accent_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));
    ui_draw_vu_meter(app,
                     content_x,
                     content_y + 148,
                     content_width,
                     12,
                     "L",
                     player->vu_left,
                     text_fg,
                     progress_border,
                     meter_bg);
    ui_draw_vu_meter(app,
                     content_x,
                     content_y + 166,
                     content_width,
                     12,
                     "R",
                     player->vu_right,
                     text_fg,
                     progress_border,
                     meter_bg);

    ui_draw_text_clipped(app,
                         content_x,
                         panel_y + panel_height - static_cast<int>(kUiFooterHeight) + 6,
                         player->status[0] != '\0' ? player->status : "PLAYBACK ACTIVE",
                         player->playback_failed ? empty_fg : text_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));
    ui_draw_text_clipped(app,
                         content_x,
                         panel_y + panel_height - static_cast<int>(kUiFooterHeight) + 16,
                         "BACKSPACE BROWSE  TAB RESTART  ESC QUIT",
                         dim_fg,
                         1,
                         static_cast<int>(max_u32(1u,
                             static_cast<vk_u32>(content_width / static_cast<int>(kUiGlyphWidth + 1u)))));

    ui_present(app);
}

static bool play_track(AppState* app, const char* path) {
    if (!player_open_track(&app->player, path)) {
        browser_set_status(&app->browser,
                           "%s",
                           app->player.status[0] != '\0' ? app->player.status : "FAILED TO OPEN MP3");
        return true;
    }

    bool dirty = true;
    vk_u64 next_refresh_tick = 0;

    while (!app->quit_requested) {
        player_reconcile_current_block(&app->player);
        player_fill_queue(&app->player, kQueuePrimeFrames);
        player_try_submit(&app->player);
        player_reconcile_current_block(&app->player);

        if (app->player.playback_failed) {
            browser_set_status(&app->browser,
                               "%s",
                               app->player.status[0] != '\0' ? app->player.status : "PLAYBACK FAILED");
            player_close_decoder(&app->player);
            return true;
        }

        if (app->player.end_of_stream
            && app->player.audio.queue_count == 0
            && !app->player.current_block_active) {
            browser_set_status(&app->browser, "PLAYED %s", path_basename(path));
            player_close_decoder(&app->player);
            return true;
        }

        const vk_u64 now = VK_CALL(tick_count);
        if (dirty || now >= next_refresh_tick) {
            render_player_screen(app);
            dirty = false;
            next_refresh_tick = now + kUiRefreshTicks;
        }

        vk_key_event_t event;
        bool saw_input = false;
        while (VK_CALL(poll_key, &event)) {
            saw_input = true;
            if (!event.pressed)
                continue;

            switch (event.scancode) {
                case 0x01:
                    app->quit_requested = true;
                    player_close_decoder(&app->player);
                    return false;
                case 0xCB:
                case 0x4B:
                case 0x0E:
                    browser_set_status(&app->browser, "STOPPED %s", path_basename(path));
                    player_close_decoder(&app->player);
                    return true;
                case 0x0F:
                    if (!player_open_track(&app->player, path)) {
                        browser_set_status(&app->browser,
                                           "%s",
                                           app->player.status[0] != '\0' ? app->player.status : "FAILED TO RESTART MP3");
                        return true;
                    }
                    dirty = true;
                    next_refresh_tick = 0;
                    break;
                default:
                    break;
            }
        }

        if (!saw_input)
            VK_CALL(sleep, 1);
    }

    player_close_decoder(&app->player);
    return false;
}

static bool init_framebuffer(AppState* app) {
    VK_CALL(framebuffer_info, &app->framebuffer);
    if (!app->framebuffer.valid || app->framebuffer.base == 0
        || app->framebuffer.width == 0 || app->framebuffer.height == 0) {
        printf("No framebuffer available\n");
        return false;
    }

    app->present_pixels = static_cast<vk_usize>(app->framebuffer.stride) * app->framebuffer.height;
    app->present_buffer = static_cast<vk_u32*>(malloc(app->present_pixels * sizeof(vk_u32)));
    if (app->present_buffer == nullptr) {
        printf("Failed to allocate presentation buffer\n");
        return false;
    }

    memset(app->present_buffer, 0, app->present_pixels * sizeof(vk_u32));
    return true;
}

static void destroy_app(AppState* app) {
    if (app == nullptr)
        return;

    player_close_decoder(&app->player);
    free(app->present_buffer);
    app->present_buffer = nullptr;
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    auto* app = static_cast<AppState*>(calloc(1, sizeof(AppState)));
    if (app == nullptr) {
        printf("Failed to allocate app state\n");
        return 1;
    }

    if (!init_framebuffer(app)) {
        free(app);
        return 1;
    }

    browser_init(app);

    char selected_path[kBrowserPathMax] = {};
    while (!app->quit_requested) {
        if (!browse_for_track(app, selected_path, sizeof(selected_path)))
            break;
        if (!play_track(app, selected_path))
            break;
    }

    destroy_app(app);
    free(app);
    return 0;
}