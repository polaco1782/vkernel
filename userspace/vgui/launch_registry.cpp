#include "launch_registry.h"

#include "console_log.h"

#include <stdio.h>

namespace vgui {

namespace {

constexpr auto k_manifest_path = "vgui_apps.txt";
constexpr auto k_fallback_manifest_path = "shell.txt";

} // namespace

auto LaunchRegistry::load_from_file(vk::string_view path) -> bool
{
    const std::string path_string = string_from_view(path);
    const vk_file_handle_t handle = VK_CALL(file_open, path_string.c_str(), "r");
    if (handle == static_cast<vk_file_handle_t>(0)) {
        return false;
    }

    std::array<char, 256> chunk {};
    std::string line;
    vk_usize read_count = 0;

    while ((read_count = VK_CALL(file_read_handle, handle, chunk.data(), chunk.size())) > 0) {
        for (vk_usize index = 0; index < read_count; ++index) {
            const char ch = chunk[index];
            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                parse_launch_line(string_view_of(line));
                line.clear();
                continue;
            }

            line.push_back(ch);
        }
    }

    if (!line.empty()) {
        parse_launch_line(string_view_of(line));
    }

    VK_CALL(file_close, handle);
    return true;
}

void LaunchRegistry::parse_launch_line(vk::string_view line)
{
    std::string trimmed = trim_ascii(line);
    if (trimmed.empty() || trimmed[0] == '#') {
        return;
    }

    vk::string_view path = string_view_of(trimmed);
    if (starts_with(path, "run") && path.size() > 3 && is_ascii_space(path[3])) {
        path.remove_prefix(3);
        while (!path.empty() && is_ascii_space(path[0])) {
            path.remove_prefix(1);
        }
    }

    add_app(path);
}

void LaunchRegistry::add_app(vk::string_view path)
{
    if (path.empty() || !ends_with(path, ".vbin") || count_ >= k_capacity || exists(path)) {
        return;
    }

    LaunchMenuEntry& entry = entries_[count_++];
    entry.path = string_from_view(path);
    entry.label = string_from_view(path_basename(path));
}

auto LaunchRegistry::exists(vk::string_view path) const -> bool
{
    for (int index = 0; index < count_; ++index) {
        if (string_equals(entries_[index].path, path)) {
            return true;
        }
    }

    return false;
}

void LaunchRegistry::sort()
{
    for (int index = 1; index < count_; ++index) {
        LaunchMenuEntry entry = entries_[index];
        int insert_index = index;
        while (insert_index > 0 && entries_[insert_index - 1].label.compare(entry.label) > 0) {
            entries_[insert_index] = entries_[insert_index - 1];
            --insert_index;
        }
        entries_[insert_index] = entry;
    }
}

void LaunchRegistry::refresh(ConsoleLog& log)
{
    count_ = 0;

    const char* source = nullptr;
    if (load_from_file(k_manifest_path)) {
        source = k_manifest_path;
    } else if (load_from_file(k_fallback_manifest_path)) {
        source = k_fallback_manifest_path;
    }

    sort();

    if (source != nullptr && count_ > 0) {
        log.addf("Launch menu loaded %d app%s from %s.",
                 count_,
                 count_ == 1 ? "" : "s",
                 source);
    } else if (source != nullptr) {
        log.addf("Launch menu file %s did not contain any runnable apps.", source);
    } else {
        log.add("Launch menu file not found.");
    }
}

} // namespace vgui