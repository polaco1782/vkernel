#include "vkfm_panel.h"

#include "console_log.h"
#include "window_manager.h"

#include <stdio.h>

namespace vgui {

namespace {

constexpr vk_u32 k_launch_width = 640;
constexpr vk_u32 k_launch_height = 400;

auto is_separator(char ch) -> bool
{
    return ch == '/' || ch == '\\';
}

auto is_ascii_printable(char ch) -> bool
{
    return ch >= 32 && ch <= 126;
}

} // namespace

auto VkfmPanel::query_default_path() const -> std::string
{
    std::array<char, 128> response {};
    std::array<char, 96> value {};

    vk_kobj_rpc_path_json("get", "fs/root_path", response.data(), response.size());
    if (json_extract_string(response.data(), "value", value) && value[0] != '\0') {
        return string_from_buffer(value);
    }

    return "/";
}

auto VkfmPanel::canonicalize_absolute_path(vk::string_view path) const -> std::string
{
    std::array<std::string, 32> parts {};
    int part_count = 0;
    vk_usize index = 0;

    while (index < path.size()) {
        while (index < path.size() && is_separator(path[index])) {
            ++index;
        }
        if (index >= path.size()) {
            break;
        }

        const vk_usize start = index;
        while (index < path.size() && !is_separator(path[index])) {
            ++index;
        }

        const vk::string_view part = subview(path, start, index - start);
        if (part.empty() || part.equals(".")) {
            continue;
        }
        if (part.equals("..")) {
            if (part_count > 0) {
                --part_count;
            }
            continue;
        }
        if (part_count < static_cast<int>(parts.size())) {
            parts[part_count++] = string_from_view(part);
        }
    }

    std::string result("/");
    for (int part_index = 0; part_index < part_count; ++part_index) {
        if (part_index != 0) {
            result.push_back('/');
        }
        result += parts[part_index];
    }
    return result;
}

auto VkfmPanel::resolve_input_path(vk::string_view raw_path) const -> std::string
{
    std::string base = current_path_.empty() ? query_default_path() : current_path_;
    std::string text = trim_ascii(raw_path);
    if (text.empty()) {
        return canonicalize_absolute_path(string_view_of(base));
    }

    if (is_separator(text[0])) {
        return canonicalize_absolute_path(string_view_of(text));
    }

    if (!base.empty() && base.compare("/") != 0) {
        std::string combined = base;
        combined.push_back('/');
        combined += text;
        text = combined;
    } else {
        std::string combined("/");
        combined += text;
        text = combined;
    }

    return canonicalize_absolute_path(string_view_of(text));
}

auto VkfmPanel::join_path(vk::string_view parent, vk::string_view child) const -> std::string
{
    if (parent.empty() || parent.equals("/")) {
        std::string result("/");
        result.append(child.data(), child.size());
        return result;
    }

    std::string result = string_from_view(parent);
    result.push_back('/');
    result.append(child.data(), child.size());
    return result;
}

auto VkfmPanel::parent_path(vk::string_view path) const -> std::string
{
    std::string normalized = canonicalize_absolute_path(path);
    if (normalized.compare("/") == 0) {
        return normalized;
    }

    vk_usize end = string_view_of(normalized).size();
    while (end > 1 && normalized[end - 1] == '/') {
        --end;
    }
    while (end > 1 && normalized[end - 1] != '/') {
        --end;
    }
    if (end <= 1) {
        return "/";
    }

    normalized.resize(static_cast<size_t>(end - 1));
    return normalized;
}

auto VkfmPanel::parse_item_record(vk::string_view record, Entry& out) const -> bool
{
    if (record.size() < 4 || record[1] != '\t') {
        return false;
    }

    vk_usize second_tab = k_not_found;
    for (vk_usize index = 2; index < record.size(); ++index) {
        if (record[index] == '\t') {
            second_tab = index;
            break;
        }
    }
    if (second_tab == k_not_found || second_tab <= 2) {
        return false;
    }

    out.is_directory = record[0] == 'D';
    out.name = string_from_view(subview(record, 2, second_tab - 2));
    out.size = parse_u64(subview(record, second_tab + 1, record.size() - second_tab - 1));
    return !out.name.empty();
}

auto VkfmPanel::selected_path() const -> std::string
{
    if (selected_index_ < 0 || selected_index_ >= entry_count_) {
        return current_path_;
    }
    return join_path(string_view_of(current_path_), string_view_of(entries_[selected_index_].name));
}

void VkfmPanel::sort_entries()
{
    for (int index = 1; index < entry_count_; ++index) {
        Entry entry = entries_[index];
        int insert_index = index;
        while (insert_index > 0) {
            const Entry& previous = entries_[insert_index - 1];
            const bool directories_first = entry.is_directory && !previous.is_directory;
            const bool names_precede = entry.is_directory == previous.is_directory
                                    && entry.name.compare(previous.name) < 0;
            if (!directories_first && !names_precede) {
                break;
            }
            entries_[insert_index] = previous;
            --insert_index;
        }
        entries_[insert_index] = entry;
    }
}

void VkfmPanel::refresh_listing(ConsoleLog* log)
{
    std::string previously_selected;
    if (selected_index_ >= 0 && selected_index_ < entry_count_) {
        previously_selected = entries_[selected_index_].name;
    }

    const std::string requested = resolve_input_path(string_view_of(path_input_));
    std::array<char, 12288> response {};
    std::array<std::array<char, k_item_len>, k_items_max> raw_items {};

    vk_kobj_rpc_path_json("fs_list", requested.c_str(), response.data(), response.size());
    if (!vk_kobj_response_ok(response.data())) {
        std::array<char, 128> error {};
        if (json_extract_string(response.data(), "error", error)) {
            status_ = string_from_buffer(error);
        } else {
            status_ = "Failed to open directory.";
        }
        if (log != nullptr) {
            log->addf("vkfm: %s (%s)", status_.c_str(), requested.c_str());
        }
        return;
    }

    const int count = vk_json_extract_string_array_field(response.data(),
                                                         "items",
                                                         raw_items[0].data(),
                                                         static_cast<vk_usize>(k_item_len),
                                                         k_items_max);

    entry_count_ = 0;
    selected_index_ = -1;
    preview_.clear();
    for (int index = 0; index < count && entry_count_ < k_items_max; ++index) {
        Entry parsed {};
        if (!parse_item_record(buffer_view(raw_items[index]), parsed)) {
            continue;
        }
        entries_[entry_count_++] = parsed;
    }

    sort_entries();
    current_path_ = requested;
    path_input_ = current_path_;

    if (entry_count_ == 0) {
        status_ = "Directory is empty.";
        return;
    }

    status_ = string_from_i64(entry_count_);
    status_ += entry_count_ == 1 ? " entry" : " entries";

    for (int index = 0; index < entry_count_; ++index) {
        if (entries_[index].name.compare(previously_selected) == 0) {
            select_entry(index, false);
            return;
        }
    }

    select_entry(0, false);
}

void VkfmPanel::select_entry(int index, bool load_preview)
{
    if (index < 0 || index >= entry_count_) {
        selected_index_ = -1;
        preview_.clear();
        return;
    }

    selected_index_ = index;
    preview_.clear();
    if (load_preview) {
        load_preview_for_selection();
    }
}

void VkfmPanel::load_preview_for_selection()
{
    preview_.clear();
    if (selected_index_ < 0 || selected_index_ >= entry_count_) {
        return;
    }

    const Entry& entry = entries_[selected_index_];
    const std::string path = selected_path();
    if (entry.is_directory) {
        preview_ = "Directory\nOpen it to browse its contents.";
        return;
    }

    const vk_file_handle_t handle = VK_CALL(file_open, path.c_str(), "r");
    if (handle == static_cast<vk_file_handle_t>(0)) {
        preview_ = "Unable to open file.";
        return;
    }

    std::array<char, 256> chunk {};
    vk_usize total = 0;

    while (total < k_preview_limit) {
        const vk_usize remaining = k_preview_limit - total;
        const vk_usize chunk_size = remaining < static_cast<vk_usize>(chunk.size()) ? remaining : static_cast<vk_usize>(chunk.size());
        const vk_usize read_count = VK_CALL(file_read_handle, handle, chunk.data(), chunk_size);
        if (read_count == 0) {
            break;
        }

        for (vk_usize index = 0; index < read_count; ++index) {
            const char ch = chunk[index];
            if (ch == '\r') {
                continue;
            }
            if (is_ascii_printable(ch) || ch == '\n' || ch == '\t') {
                preview_.push_back(ch);
            } else {
                preview_.push_back('.');
            }
        }
        total += read_count;
        if (read_count < chunk_size) {
            break;
        }
    }

    const bool truncated = entry.size > total;
    VK_CALL(file_close, handle);

    if (preview_.empty()) {
        preview_ = "(empty file)";
    }
    if (truncated) {
        preview_ += "\n\n[preview truncated]";
    }
}

void VkfmPanel::open_selected(ConsoleLog& log)
{
    if (selected_index_ < 0 || selected_index_ >= entry_count_) {
        return;
    }

    const Entry& entry = entries_[selected_index_];
    const std::string path = selected_path();
    if (entry.is_directory) {
        path_input_ = path;
        refresh_listing(&log);
        log.addf("vkfm: opened %s", path.c_str());
        return;
    }

    load_preview_for_selection();
    log.addf("vkfm: previewed %s", path.c_str());
}

void VkfmPanel::draw_window(bool& visible, WindowManager& window_manager, ConsoleLog& log)
{
    if (!visible) {
        return;
    }

    if (!initialized_) {
        current_path_ = query_default_path();
        path_input_ = current_path_;
        refresh_listing();
        initialized_ = true;
    }

    ImGui::SetNextWindowPos(ImVec2(350.0f, 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500.0f, 700.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("vkfm", &visible)) {
        ImGui::End();
        return;
    }
    window_manager.clear_focus_if_host_window_focused();

    if (ImGui::Button("Home")) {
        path_input_ = query_default_path();
        refresh_listing(&log);
    }
    ImGui::SameLine();
    if (ImGui::Button("Up")) {
        path_input_ = parent_path(string_view_of(current_path_));
        refresh_listing(&log);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        refresh_listing(&log);
    }

    ImGui::SetNextItemWidth(-60.0f);
    const bool submit_path = imgui_input_text("##vkfm_path",
                                              path_input_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Go") || submit_path) {
        refresh_listing(&log);
    }

    ImGui::TextDisabled("%s", current_path_.c_str());
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", status_.c_str());
    }

    ImGui::SeparatorText("Entries");
    const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_Borders
                                      | ImGuiTableFlags_Resizable
                                      | ImGuiTableFlags_ScrollY
                                      | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##vkfm_entries", 3, table_flags, ImVec2(0.0f, 280.0f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.70f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableHeadersRow();

        for (int index = 0; index < entry_count_; ++index) {
            const Entry& entry = entries_[index];
            const bool is_selected = selected_index_ == index;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool activated = ImGui::Selectable(entry.name.c_str(),
                                                     is_selected,
                                                     ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
            if (activated) {
                select_entry(index, false);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    open_selected(log);
                }
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry.is_directory ? "dir" : "file");
            ImGui::TableSetColumnIndex(2);
            if (entry.is_directory) {
                ImGui::TextUnformatted("-");
            } else {
                ImGui::Text("%llu", static_cast<unsigned long long>(entry.size));
            }
        }

        ImGui::EndTable();
    }

    ImGui::SeparatorText("Selection");
    if (selected_index_ >= 0 && selected_index_ < entry_count_) {
        const Entry& entry = entries_[selected_index_];
        const std::string path = selected_path();
        ImGui::TextWrapped("%s", path.c_str());
        ImGui::Text("Type: %s", entry.is_directory ? "Directory" : "File");
        if (!entry.is_directory) {
            ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(entry.size));
        }

        if (ImGui::Button(entry.is_directory ? "Open" : "Preview")) {
            open_selected(log);
        }
        if (!entry.is_directory && ends_with(string_view_of(path), ".vbin")) {
            ImGui::SameLine();
            if (ImGui::Button("Launch")) {
                (void)window_manager.launch_windowed_app(string_view_of(path), k_launch_width, k_launch_height);
                log.addf("vkfm: launched %s", path.c_str());
            }
        }

        ImGui::BeginChild("##vkfm_preview", ImVec2(0.0f, 0.0f), true);
        if (preview_.empty()) {
            ImGui::TextDisabled("Select Preview/Open to inspect this entry.");
        } else {
            ImGui::TextWrapped("%s", preview_.c_str());
        }
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("No entry selected.");
    }

    ImGui::End();
}

} // namespace vgui