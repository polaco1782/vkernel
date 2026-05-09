#include "kobj_panel.h"

#include "window_manager.h"

#include <stdio.h>

namespace vgui {

auto KobjNavigator::join_path(vk::string_view parent, vk::string_view child) const -> std::string
{
    if (parent.empty()) {
        return string_from_view(child);
    }

    std::string result = string_from_view(parent);
    result.push_back('/');
    result.append(child.data(), child.size());
    return result;
}

auto KobjNavigator::list_items(vk::string_view path, std::array<std::string, k_items_max>& items) const -> int
{
    std::array<char, 1536> response {};
    std::array<std::array<char, k_item_len>, k_items_max> raw_items {};

    const std::string path_string = string_from_view(path);
    vk_kobj_rpc_path_json("ls", path_string.c_str(), response.data(), response.size());

    if (!vk_kobj_response_ok(response.data())) {
        return 0;
    }

    const int count = vk_json_extract_string_array_field(response.data(),
                                                         "items",
                                                         raw_items[0].data(),
                                                         k_item_len,
                                                         k_items_max);
    for (int index = 0; index < count; ++index) {
        items[index] = raw_items[index].data();
    }
    return count;
}

auto KobjNavigator::desc_get_field(vk::string_view key) const -> std::string
{
    const vk::string_view text = string_view_of(desc_);
    vk_usize line_start = 0;

    while (line_start < text.size()) {
        vk_usize line_end = line_start;
        while (line_end < text.size() && text[line_end] != '\n') {
            ++line_end;
        }

        const vk::string_view line = subview(text, line_start, line_end - line_start);
        if (line.size() >= key.size() && subview(line, 0, key.size()).equals(key)) {
            vk_usize value_start = key.size();
            while (value_start < line.size()) {
                const char ch = line[value_start];
                if (ch != ' ' && ch != '\t' && ch != ':') {
                    break;
                }
                ++value_start;
            }

            return trim_ascii(subview(line, value_start, line.size() - value_start));
        }

        line_start = line_end < text.size() ? line_end + 1 : line_end;
    }

    return std::string();
}

void KobjNavigator::parse_edit_state()
{
    writable_ = false;
    enum_count_ = 0;
    enum_selected_ = 0;
    has_range_ = false;
    range_min_ = 0;
    range_max_ = 0;
    edit_value_.clear();

    if (string_equals(desc_get_field("writable"), "yes")) {
        writable_ = true;
    }
    if (!writable_) {
        return;
    }

    if (string_equals(type_, "enum")) {
        const std::string labels = desc_get_field("labels");
        const vk::string_view labels_view = string_view_of(labels);
        vk_usize start = 0;

        while (start < labels_view.size() && enum_count_ < k_enum_label_max) {
            while (start < labels_view.size() && labels_view[start] == ' ') {
                ++start;
            }

            vk_usize end = start;
            while (end < labels_view.size() && labels_view[end] != ',') {
                ++end;
            }

            enum_labels_[enum_count_++] = trim_ascii(subview(labels_view, start, end - start));
            start = end < labels_view.size() ? end + 1 : end;
        }

        for (int index = 0; index < enum_count_; ++index) {
            if (enum_labels_[index].compare(value_) == 0) {
                enum_selected_ = index;
                break;
            }
        }
        return;
    }

    const std::string range = desc_get_field("range");
    if (!range.empty() && !string_equals(range, "(unbounded)")) {
        const vk::string_view range_view = string_view_of(range);
        const vk_usize dots = find_substring(range_view, "..");
        if (dots != k_not_found) {
            range_min_ = parse_i64(subview(range_view, 0, dots));
            range_max_ = parse_i64(subview(range_view, dots + 2, range_view.size() - dots - 2));
            has_range_ = range_max_ > range_min_;
        }
    }

    edit_value_ = value_;
}

void KobjNavigator::refresh_selected()
{
    std::array<char, 1536> response {};
    std::array<char, k_value_len> value_buffer {};
    std::array<char, k_type_len> type_buffer {};
    std::array<char, k_desc_len> desc_buffer {};

    vk_kobj_rpc_path_json("get", selected_path_.c_str(), response.data(), response.size());
    if (json_extract_string(response.data(), "value", value_buffer)) {
        value_ = string_from_buffer(value_buffer);
    } else {
        value_ = "(unavailable)";
    }

    if (json_extract_string(response.data(), "type", type_buffer)) {
        type_ = string_from_buffer(type_buffer);
    } else {
        type_ = "(unknown)";
    }

    vk_kobj_rpc_path_json("describe", selected_path_.c_str(), response.data(), response.size());
    if (json_extract_string(response.data(), "text", desc_buffer)) {
        desc_ = string_from_buffer(desc_buffer);
    } else {
        desc_ = "(no description)";
    }

    parse_edit_state();
    last_refresh_tick_ = VK_CALL(tick_count);
}

void KobjNavigator::select_path(vk::string_view path)
{
    const std::string path_string = string_from_view(path);
    if (path_string.empty()) {
        return;
    }

    selected_path_ = path_string;
    path_input_ = selected_path_;
    refresh_selected();
}

void KobjNavigator::draw_tree_node(vk::string_view parent_path, const std::string& label, int depth)
{
    if (label.empty() || depth > 8) {
        return;
    }

    const std::string path = join_path(parent_path, string_view_of(label));

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected_path_.compare(path) == 0) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool open = ImGui::TreeNodeEx(path.c_str(), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked()) {
        select_path(string_view_of(path));
    }

    if (open) {
        std::array<std::string, k_items_max> children {};
        const int child_count = list_items(string_view_of(path), children);
        if (child_count == 0) {
            ImGui::TextDisabled("(empty)");
        } else {
            for (int index = 0; index < child_count; ++index) {
                draw_tree_node(string_view_of(path), children[index], depth + 1);
            }
        }
        ImGui::TreePop();
    }
}

void KobjNavigator::draw_window(bool& visible, WindowManager& window_manager)
{
    if (!visible) {
        return;
    }

    if (auto_refresh_) {
        const vk_u64 now = VK_CALL(tick_count);
        const vk_u32 ticks_per_second = VK_CALL(ticks_per_sec);
        const vk_u64 period = ticks_per_second != 0 ? static_cast<vk_u64>(ticks_per_second / 2u) : 0;
        if (period == 0 || now - last_refresh_tick_ >= period) {
            refresh_selected();
        }
    }

    ImGui::SetNextWindowPos(ImVec2(860.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470.0f, 260.0f), ImGuiCond_FirstUseEver);

    if (!imgui_begin_window_readable_caption("KObj Navigator", &visible)) {
        ImGui::End();
        return;
    }
    window_manager.clear_focus_if_host_window_focused();

    if (ImGui::Button("Refresh")) {
        refresh_selected();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &auto_refresh_);

    ImGui::SetNextItemWidth(-64.0f);
    imgui_input_text("##kobj_path", path_input_);
    ImGui::SameLine();
    if (ImGui::Button("Go")) {
        select_path(string_view_of(path_input_));
    }

    ImGui::BeginChild("##kobj_tree", ImVec2(180.0f, 0.0f), true);
    std::array<std::string, k_items_max> roots {};
    const int root_count = list_items("", roots);
    if (root_count == 0) {
        ImGui::TextDisabled("No kobj nodes.");
    } else {
        for (int index = 0; index < root_count; ++index) {
            draw_tree_node("", roots[index], 0);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##kobj_detail", ImVec2(0.0f, 0.0f), true);
    ImGui::Text("Path: %s", selected_path_.c_str());
    ImGui::Text("Type: %s", type_.empty() ? "(unknown)" : type_.c_str());
    ImGui::SeparatorText("Value");
    ImGui::TextWrapped("%s", value_.empty() ? "(empty)" : value_.c_str());

    if (writable_) {
        ImGui::SeparatorText("Edit");
        std::array<char, 256> response {};

        if (string_equals(type_, "enum") && enum_count_ > 0) {
            std::array<const char*, k_enum_label_max> items {};
            for (int index = 0; index < enum_count_; ++index) {
                items[index] = enum_labels_[index].c_str();
            }

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##kobj_enum", &enum_selected_, items.data(), enum_count_)) {
                vk_kobj_rpc_path_value_json("set",
                                            selected_path_.c_str(),
                                            enum_labels_[enum_selected_].c_str(),
                                            response.data(),
                                            response.size());
                refresh_selected();
            }
        } else if (string_equals(type_, "bool")) {
            bool current = string_equals(value_, "yes") || string_equals(value_, "true");
            if (ImGui::Checkbox("Enabled##kobj_bool", &current)) {
                vk_kobj_rpc_path_value_json("set",
                                            selected_path_.c_str(),
                                            current ? "true" : "false",
                                            response.data(),
                                            response.size());
                refresh_selected();
            }
        } else if ((string_equals(type_, "u64") || string_equals(type_, "i64")) && has_range_) {
            int current = static_cast<int>(parse_i64(string_view_of(value_)));
            const int min_value = static_cast<int>(range_min_);
            const int max_value = static_cast<int>(range_max_);

            ImGui::SetNextItemWidth(-80.0f);
            if (ImGui::SliderInt("##kobj_slider", &current, min_value, max_value)) {
                edit_value_ = string_from_i64(current);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set##kobj_range")) {
                vk_kobj_rpc_path_value_json("set",
                                            selected_path_.c_str(),
                                            edit_value_.c_str(),
                                            response.data(),
                                            response.size());
                refresh_selected();
            }
        } else {
            ImGui::SetNextItemWidth(-80.0f);
            imgui_input_text("##kobj_edit", edit_value_);
            ImGui::SameLine();
            if (ImGui::Button("Set##kobj_text")) {
                vk_kobj_rpc_path_value_json("set",
                                            selected_path_.c_str(),
                                            edit_value_.c_str(),
                                            response.data(),
                                            response.size());
                refresh_selected();
            }
        }
    }

    ImGui::SeparatorText("Describe");
    ImGui::TextWrapped("%s", desc_.empty() ? "(no description)" : desc_.c_str());
    ImGui::EndChild();

    ImGui::End();
}

} // namespace vgui