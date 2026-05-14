#include "frontend.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace vnes_frontend {

namespace {

auto is_separator(char ch) -> bool
{
    return ch == '/' || ch == '\\';
}

auto ends_with_casefolded(const char* text, const char* suffix) -> bool
{
    if (text == nullptr || suffix == nullptr) {
        return false;
    }

    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length) {
        return false;
    }

    const char* start = text + (text_length - suffix_length);
    for (size_t index = 0; index < suffix_length; ++index) {
        if (tolower(static_cast<unsigned char>(start[index]))
            != tolower(static_cast<unsigned char>(suffix[index]))) {
            return false;
        }
    }

    return true;
}

auto browser_is_supported_rom(const char* name, vk_u64 size_bytes) -> bool
{
    return name != nullptr
        && name[0] != '\0'
        && size_bytes != 0
        && ends_with_casefolded(name, ".nes");
}

void set_path_input(RomBrowserState* browser, const std::string& path)
{
    snprintf(browser->path_input.data(), browser->path_input.size(), "%s", path.c_str());
}

auto query_default_path() -> std::string
{
    char response[256] = {};
    char value[128] = {};

    vk_kobj_rpc_path_json("get", "fs/root_path", response, sizeof(response));
    if (vk_kobj_response_ok(response)
        && vk_json_extract_string_field(response, "value", value, sizeof(value))
        && value[0] != '\0') {
        return value;
    }

    return "/";
}

auto canonicalize_absolute_path(const std::string& path) -> std::string
{
    std::vector<std::string> parts;
    size_t index = 0;

    while (index < path.size()) {
        while (index < path.size() && is_separator(path[index])) {
            ++index;
        }
        if (index >= path.size()) {
            break;
        }

        const size_t start = index;
        while (index < path.size() && !is_separator(path[index])) {
            ++index;
        }

        const std::string part = path.substr(start, index - start);
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }

        parts.push_back(part);
    }

    std::string result("/");
    for (size_t part_index = 0; part_index < parts.size(); ++part_index) {
        if (part_index != 0) {
            result.push_back('/');
        }
        result += parts[part_index];
    }
    return result;
}

auto join_path(const std::string& parent, const std::string& child) -> std::string
{
    if (parent.empty() || parent == "/") {
        std::string result("/");
        result += child;
        return result;
    }

    std::string result = parent;
    result += "/";
    result += child;
    return result;
}

auto parent_path(const std::string& path) -> std::string
{
    const std::string normalized = canonicalize_absolute_path(path);
    size_t slash = normalized.size();
    for (size_t index = normalized.size(); index > 0; --index) {
        if (normalized[index - 1] == '/') {
            slash = index - 1;
            break;
        }
    }

    if (slash == normalized.size() || slash == 0) {
        return "/";
    }
    return normalized.substr(0, slash);
}

auto parse_u64_decimal(const char* text) -> vk_u64
{
    vk_u64 value = 0;

    if (text == nullptr) {
        return 0;
    }

    while (*text >= '0' && *text <= '9') {
        value = value * 10U + static_cast<vk_u64>(*text - '0');
        ++text;
    }

    return value;
}

auto parse_item_record(const char* record, RomBrowserEntry* entry) -> bool
{
    if (record == nullptr || entry == nullptr || (record[0] != 'D' && record[0] != 'F') || record[1] != '\t') {
        return false;
    }

    const char* second_tab = strchr(record + 2, '\t');
    if (second_tab == nullptr || second_tab <= record + 2) {
        return false;
    }

    entry->name.assign(record + 2, static_cast<size_t>(second_tab - (record + 2)));
    entry->is_directory = record[0] == 'D';
    entry->size_bytes = parse_u64_decimal(second_tab + 1);
    return !entry->name.empty();
}

void sort_entries(RomBrowserState* browser)
{
    for (int index = 1; index < browser->entry_count; ++index) {
        const RomBrowserEntry entry = browser->entries[index];
        int insert_index = index;

        while (insert_index > 0) {
            const RomBrowserEntry& previous = browser->entries[insert_index - 1];
            const bool directories_first = entry.is_directory && !previous.is_directory;
            const bool names_precede = entry.is_directory == previous.is_directory
                                    && entry.name.compare(previous.name) < 0;
            if (!directories_first && !names_precede) {
                break;
            }

            browser->entries[insert_index] = previous;
            --insert_index;
        }

        browser->entries[insert_index] = entry;
    }
}

} // namespace

void browser_open(AppState* app)
{
    if (app->browser.current_path.empty()) {
        app->browser.current_path = query_default_path();
    }

    app->browser.open = true;
    app->browser.focus_path_input = true;
    set_path_input(&app->browser, app->browser.current_path);
    (void)browser_refresh_listing(app);
}

bool browser_refresh_listing(AppState* app)
{
    RomBrowserState* browser = &app->browser;

    const std::string requested = canonicalize_absolute_path(
        browser->path_input[0] == '\0' ? browser->current_path : browser->path_input.data());

    browser->response.fill('\0');
    for (size_t index = 0; index < browser->raw_items.size(); ++index) {
        browser->raw_items[index].fill('\0');
    }

    vk_kobj_rpc_path_json("fs_list", requested.c_str(), browser->response.data(), browser->response.size());
    if (!vk_kobj_response_ok(browser->response.data())) {
        char error[128] = {};
        browser->entry_count = 0;
        browser->selected_index = -1;
        if (vk_json_extract_string_field(browser->response.data(), "error", error, sizeof(error))) {
            browser->status = error;
        } else {
            browser->status = "Failed to list directory.";
        }
        return false;
    }

    const int item_count = vk_json_extract_string_array_field(browser->response.data(),
                                                              "items",
                                                              browser->raw_items[0].data(),
                                                              kRomBrowserItemMax,
                                                              static_cast<int>(browser->raw_items.size()));

    browser->entry_count = 0;
    browser->selected_index = -1;
    for (int index = 0; index < item_count && browser->entry_count < static_cast<int>(browser->entries.size()); ++index) {
        RomBrowserEntry entry;
        if (!parse_item_record(browser->raw_items[static_cast<size_t>(index)].data(), &entry)) {
            continue;
        }
        if (!entry.is_directory && !browser_is_supported_rom(entry.name.c_str(), entry.size_bytes)) {
            continue;
        }
        browser->entries[static_cast<size_t>(browser->entry_count)] = entry;
        ++browser->entry_count;
    }

    sort_entries(browser);
    browser->current_path = requested;
    set_path_input(browser, browser->current_path);
    browser->status = browser->entry_count == 0 ? "No ROMs in this directory." : "Ready.";
    if (browser->entry_count > 0) {
        browser->selected_index = 0;
    }
    return true;
}

void browser_navigate_to_parent(AppState* app)
{
    app->browser.current_path = parent_path(app->browser.current_path);
    set_path_input(&app->browser, app->browser.current_path);
    (void)browser_refresh_listing(app);
}

bool browser_activate_selection(AppState* app)
{
    RomBrowserState* browser = &app->browser;
    if (browser->selected_index < 0 || browser->selected_index >= browser->entry_count) {
        return false;
    }

    const RomBrowserEntry& entry = browser->entries[static_cast<size_t>(browser->selected_index)];
    const std::string target = join_path(browser->current_path, entry.name);
    if (entry.is_directory) {
        browser->current_path = target;
        set_path_input(browser, browser->current_path);
        return browser_refresh_listing(app);
    }

    if (load_rom(app, target.c_str())) {
        browser->open = false;
        return true;
    }

    browser->status = "Failed to load ROM.";
    return false;
}

} // namespace vnes_frontend