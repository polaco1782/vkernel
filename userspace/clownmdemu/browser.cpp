#include "frontend.h"

namespace clownmdemu_frontend {

namespace {

void browser_set_status(RomBrowserState* browser, const char* format, ...) {
    if (browser == nullptr || format == nullptr)
        return;

    char tmp[kRomBrowserStatusMax];
    va_list args;
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    browser->status = tmp;
}

bool browser_is_supported_rom(const char* name, vk_u64 size_bytes) {
    if (name == nullptr || name[0] == '\0' || size_bytes == 0 || size_bytes > kMaxRomBytes)
        return false;

    return ends_with_casefolded(name, ".bin")
        || ends_with_casefolded(name, ".gen")
        || ends_with_casefolded(name, ".md")
        || ends_with_casefolded(name, ".smd")
        || ends_with_casefolded(name, ".32x");
}

std::string browser_join_path(const std::string& parent, const std::string& child) {
    std::string result;
    if (parent.empty() || parent == "/") {
        result += "/";
    } else {
        result += parent;
        result += "/";
    }
    result += child;
    return result;
}

std::string browser_parent_path(std::string path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

vk_u64 parse_u64_decimal(const char* text) {
    vk_u64 value = 0;

    if (text == nullptr)
        return 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10u + static_cast<vk_u64>(*text - '0');
        ++text;
    }

    return value;
}

bool browser_parse_item_record(const char* record, RomBrowserEntry* entry) {
    if (record == nullptr || entry == nullptr || (record[0] != 'D' && record[0] != 'F') || record[1] != '\t')
        return false;

    const char* second_tab = strchr(record + 2, '\t');
    if (second_tab == nullptr || second_tab <= record + 2)
        return false;

    const size_t name_length = static_cast<size_t>(second_tab - (record + 2));
    *entry = RomBrowserEntry{};
    const size_t copy_length = name_length < kRomBrowserNameMax - 1 ? name_length : kRomBrowserNameMax - 1;
    entry->name.assign(record + 2, copy_length);
    entry->is_directory = record[0] == 'D';
    entry->size_bytes = parse_u64_decimal(second_tab + 1);
    return !entry->name.empty();
}

void browser_sort_entries(RomBrowserState* browser) {
    if (browser == nullptr)
        return;

    for (vk_u32 index = 1; index < browser->entry_count; ++index) {
        const RomBrowserEntry entry = browser->entries[index];
        vk_u32 insert_index = index;

        while (insert_index > 0) {
            const RomBrowserEntry& previous = browser->entries[insert_index - 1];
            const bool directories_first = entry.is_directory && !previous.is_directory;
            const bool names_precede = entry.is_directory == previous.is_directory
                                    && compare_casefolded(entry.name.c_str(), previous.name.c_str()) < 0;
            if (!directories_first && !names_precede)
                break;

            browser->entries[insert_index] = previous;
            --insert_index;
        }

        browser->entries[insert_index] = entry;
    }
}

void browser_query_default_path(RomBrowserState* browser) {
    if (browser == nullptr)
        return;

    memset(browser->response, 0, sizeof(browser->response));
    vk_kobj_rpc_path_json("get", "fs/root_path", browser->response, sizeof(browser->response));
    char path_buf[kRomBrowserPathMax] = {};
    if (!vk_kobj_response_ok(browser->response)
        || !vk_json_extract_string_field(browser->response, "value", path_buf, sizeof(path_buf))
        || path_buf[0] == '\0') {
        browser->current_path = "/";
    } else {
        browser->current_path = path_buf;
    }
}

bool browser_refresh_listing(AppState* app) {
    auto* browser = &app->browser;
    memset(browser->response, 0, sizeof(browser->response));
    memset(browser->raw_items, 0, sizeof(browser->raw_items));

    vk_kobj_rpc_path_json("fs_list", browser->current_path.c_str(), browser->response, sizeof(browser->response));
    if (!vk_kobj_response_ok(browser->response)) {
        char error[kRomBrowserStatusMax];
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
                                                              kRomBrowserItemMax,
                                                              static_cast<int>(kRomBrowserMaxEntries));

    browser->entry_count = 0;
    browser->selected_index = 0;
    browser->scroll_index = 0;

    for (int index = 0; index < item_count && browser->entry_count < kRomBrowserMaxEntries; ++index) {
        RomBrowserEntry entry;
        if (!browser_parse_item_record(browser->raw_items[index], &entry))
            continue;
        if (!entry.is_directory && !browser_is_supported_rom(entry.name.c_str(), entry.size_bytes))
            continue;
        browser->entries[browser->entry_count++] = entry;
    }

    browser_sort_entries(browser);
    if (browser->entry_count == 0) {
        browser_set_status(browser, "NO ROM FILES IN THIS DIRECTORY");
    } else {
        browser_set_status(browser,
                           "%u ITEM%s",
                           browser->entry_count,
                           browser->entry_count == 1 ? "" : "S");
    }
    return true;
}

const unsigned char kBrowserGlyphs[][kUiGlyphHeight] = {
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

const unsigned char* browser_glyph_for(char ch) {
    int index = 0;

    if (ch == '/' || ch == '\\') index = 1;
    else if (ch == ':') index = 2;
    else if (ch == '.') index = 3;
    else if (ch == '-') index = 4;
    else if (ch == '_') index = 5;
    else if (ch >= '0' && ch <= '9') index = 6 + (ch - '0');
    else if (ch >= 'A' && ch <= 'Z') index = 16 + (ch - 'A');
    else if (ch >= 'a' && ch <= 'z') index = 16 + (ch - 'a');

    return kBrowserGlyphs[index];
}

void browser_present_buffer(const AppState* app) {
    if (app == nullptr || app->present_buffer.empty())
        return;

    memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(app->framebuffer.base)),
           app->present_buffer.data(),
           app->present_buffer.size() * sizeof(vk_u32));
}

void browser_fill_rect(AppState* app, int x, int y, int width, int height, vk_u32 color) {
    if (app == nullptr || app->present_buffer.empty() || width <= 0 || height <= 0)
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

void browser_draw_background(AppState* app) {
    if (app == nullptr || app->present_buffer.empty())
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

void browser_draw_char(AppState* app, int x, int y, char ch, vk_u32 color, int scale) {
    if (app == nullptr || scale <= 0)
        return;

    const unsigned char* glyph = browser_glyph_for(ch);
    for (int row = 0; row < static_cast<int>(kUiGlyphHeight); ++row) {
        for (int column = 0; column < static_cast<int>(kUiGlyphWidth); ++column) {
            if ((glyph[row] & (0x01u << column)) == 0)
                continue;
            browser_fill_rect(app,
                              x + column * scale,
                              y + row * scale,
                              scale,
                              scale,
                              color);
        }
    }
}

int browser_draw_text(AppState* app, int x, int y, const char* text, vk_u32 color, int scale) {
    int cursor_x = x;

    if (text == nullptr)
        return cursor_x;

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        browser_draw_char(app, cursor_x, y, *cursor, color, scale);
        cursor_x += static_cast<int>((kUiGlyphWidth + 1u) * static_cast<vk_u32>(scale));
    }
    return cursor_x;
}

void browser_draw_text_clipped(AppState* app,
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
        browser_draw_char(app, cursor_x, y, *cursor, color, scale);
        cursor_x += static_cast<int>((kUiGlyphWidth + 1u) * static_cast<vk_u32>(scale));
    }
}

void browser_format_size(char* out, size_t out_capacity, vk_u64 size_bytes) {
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

void browser_move_selection(RomBrowserState* browser, int delta) {
    if (browser == nullptr || browser->entry_count == 0 || delta == 0)
        return;

    int next_index = static_cast<int>(browser->selected_index) + delta;
    if (next_index < 0)
        next_index = 0;
    if (next_index >= static_cast<int>(browser->entry_count))
        next_index = static_cast<int>(browser->entry_count) - 1;
    browser->selected_index = static_cast<vk_u32>(next_index);
}

void render_rom_browser(AppState* app) {
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

    browser_draw_background(app);
    browser_fill_rect(app, panel_x, panel_y, panel_width, panel_height, panel_bg);
    browser_fill_rect(app, panel_x, panel_y, panel_width, 2, panel_border);
    browser_fill_rect(app, panel_x, panel_y + panel_height - 2, panel_width, 2, panel_border);
    browser_fill_rect(app, panel_x, panel_y, 2, panel_height, panel_border);
    browser_fill_rect(app, panel_x + panel_width - 2, panel_y, 2, panel_height, panel_border);
    browser_fill_rect(app, panel_x + 2, panel_y + 2, panel_width - 4, static_cast<int>(kUiHeaderHeight) - 2, header_bg);
    browser_fill_rect(app,
                      panel_x + 2,
                      panel_y + panel_height - static_cast<int>(kUiFooterHeight),
                      panel_width - 4,
                      static_cast<int>(kUiFooterHeight) - 2,
                      footer_bg);

    browser_draw_text(app, content_x, header_y, "SELECT ROM", title_fg, static_cast<int>(kUiFontScale));
    browser_draw_text_clipped(app,
                              content_x,
                              header_y + 28,
                              browser->current_path.c_str(),
                              path_fg,
                              1,
                              content_width / static_cast<int>(kUiGlyphWidth + 1u));

    char count_text[32];
    snprintf(count_text,
             sizeof(count_text),
             "%u/%u",
             browser->entry_count == 0 ? 0u : browser->selected_index + 1u,
             browser->entry_count);
    const int count_width = static_cast<int>(strlen(count_text)) * static_cast<int>(kUiGlyphWidth + 1u);
    browser_draw_text(app,
                      panel_x + panel_width - kUiPanelPadding - count_width,
                      header_y + 30,
                      count_text,
                      dim_fg,
                      1);

    if (browser->entry_count == 0) {
        browser_draw_text_clipped(app,
                                  content_x,
                                  content_y + 8,
                                  "NO ROM FILES FOUND",
                                  empty_fg,
                                  static_cast<int>(kUiFontScale),
                                  content_width / static_cast<int>(kUiFontAdvance));
        browser_draw_text_clipped(app,
                                  content_x,
                                  content_y + 40,
                                  "SUPPORTED: .BIN .MD .GEN .SMD .32X",
                                  dim_fg,
                                  1,
                                  content_width / static_cast<int>(kUiGlyphWidth + 1u));
    } else {
        const int size_column_chars = 6;
        const int size_column_width = size_column_chars * static_cast<int>(kUiGlyphWidth + 1u);
        const int name_char_limit = static_cast<int>(max_u32(
            1u,
            static_cast<vk_u32>((content_width - size_column_width - 16) / static_cast<int>(kUiGlyphWidth + 1u))));

        for (int row = 0; row < rows_visible; ++row) {
            const vk_u32 entry_index = browser->scroll_index + static_cast<vk_u32>(row);
            if (entry_index >= browser->entry_count)
                break;

            const RomBrowserEntry& entry = browser->entries[entry_index];
            const int row_y = content_y + row * static_cast<int>(kUiRowHeight);
            const bool selected = entry_index == browser->selected_index;
            const vk_u32 row_fg = selected ? selected_fg : text_fg;
            const vk_u32 name_fg = entry.is_directory ? (selected ? selected_dir_fg : dir_fg) : row_fg;
            char label[kRomBrowserNameMax + 2];
            char size_label[16];

            snprintf(label, sizeof(label), "%s%s", entry.name.c_str(), entry.is_directory ? "/" : "");

            if (selected)
                browser_fill_rect(app, content_x - 8, row_y - 4, content_width + 16, static_cast<int>(kUiRowHeight) - 2, selected_bg);
            browser_fill_rect(app, content_x - 8, row_y - 4, 4, static_cast<int>(kUiRowHeight) - 2, selected ? title_fg : panel_border);

            browser_draw_text_clipped(app,
                                      content_x,
                                      row_y,
                                      label,
                                      name_fg,
                                      1,
                                      name_char_limit);

            if (entry.is_directory) {
                snprintf(size_label, sizeof(size_label), "DIR");
            } else {
                browser_format_size(size_label, sizeof(size_label), entry.size_bytes);
            }

            const int size_width = static_cast<int>(strlen(size_label)) * static_cast<int>(kUiGlyphWidth + 1u);
            browser_draw_text(app,
                              panel_x + panel_width - kUiPanelPadding - size_width,
                              row_y,
                              size_label,
                              selected ? selected_fg : dim_fg,
                              1);
        }
    }

    browser_draw_text_clipped(app,
                              content_x,
                              panel_y + panel_height - static_cast<int>(kUiFooterHeight) + 6,
                              browser->status.c_str(),
                              text_fg,
                              1,
                              content_width / static_cast<int>(kUiGlyphWidth + 1u));
    browser_draw_text_clipped(app,
                              content_x,
                              panel_y + panel_height - static_cast<int>(kUiFooterHeight) + 16,
                              "ARROWS MOVE  ENTER OPEN  BACKSPACE UP  ESC QUIT",
                              dim_fg,
                              1,
                              content_width / static_cast<int>(kUiGlyphWidth + 1u));

    browser_present_buffer(app);
}

void browser_go_parent(AppState* app) {
    auto* browser = &app->browser;
    const std::string parent_path = browser_parent_path(browser->current_path);
    if (parent_path == browser->current_path) {
        browser_set_status(browser, "ALREADY AT ROOT");
        return;
    }

    browser->current_path = parent_path;
    browser_refresh_listing(app);
}

bool browser_open_selection(AppState* app) {
    auto* browser = &app->browser;

    if (browser->entry_count == 0 || browser->selected_index >= browser->entry_count)
        return false;

    const RomBrowserEntry& entry = browser->entries[browser->selected_index];
    const std::string path = browser_join_path(browser->current_path, entry.name);

    if (entry.is_directory) {
        browser->current_path = path;
        browser_refresh_listing(app);
        return false;
    }

    if (load_rom(app, path.c_str())) {
        browser_set_status(browser, "LOADED %s", path_basename(path.c_str()));
        return true;
    }

    browser_set_status(browser, "FAILED TO LOAD %s", path_basename(path.c_str()));
    return false;
}

} // namespace

bool browse_and_load_rom(AppState* app) {
    auto* browser = &app->browser;
    bool dirty = true;

    *browser = RomBrowserState{};
    browser_query_default_path(browser);
    browser_refresh_listing(app);

    while (!app->quit_requested) {
        if (dirty) {
            render_rom_browser(app);
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
                    browser_move_selection(browser, -1);
                    dirty = true;
                    break;
                case 0xD0:
                case 0x50:
                    browser_move_selection(browser, 1);
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
                    if (browser_open_selection(app))
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

} // namespace clownmdemu_frontend