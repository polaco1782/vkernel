#ifndef VGUI_COMMON_H
#define VGUI_COMMON_H

#include "../include/vk.h"
#include "imgui/imgui.h"
#include "imgui_impl_vk.h"

#include "vkernel/types.h"
#include "vkernel/unique_ptr.h"

#include <array>
#include <string>

namespace vgui {

inline constexpr vk_usize k_not_found = static_cast<vk_usize>(-1);

inline auto is_ascii_space(char ch) -> bool
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

inline auto string_from_view(vk::string_view view) -> std::string
{
    std::string result;
    if (!view.empty()) {
        result.append(view.data(), view.size());
    }
    return result;
}

inline void append_decimal(std::string& out, unsigned long long value)
{
    if (value >= 10ULL) {
        append_decimal(out, value / 10ULL);
    }
    out.push_back(static_cast<char>('0' + (value % 10ULL)));
}

inline auto string_from_i64(long long value) -> std::string
{
    std::string result;
    if (value < 0) {
        result.push_back('-');
        append_decimal(result, static_cast<unsigned long long>(-(value + 1)) + 1ULL);
    } else {
        append_decimal(result, static_cast<unsigned long long>(value));
    }
    return result;
}

inline auto string_view_of(const std::string& value) -> vk::string_view
{
    return vk::string_view(value.c_str(), value.size());
}

inline auto subview(vk::string_view view, vk_usize start, vk_usize count) -> vk::string_view
{
    if (start > view.size()) {
        return vk::string_view();
    }

    vk_usize available = view.size() - start;
    if (count > available) {
        count = available;
    }

    return vk::string_view(view.data() + start, count);
}

template <size_t N>
inline auto buffer_view(const std::array<char, N>& buffer) -> vk::string_view
{
    if constexpr (N == 0) {
        return vk::string_view();
    }

    return vk::string_view(buffer.data());
}

template <size_t N>
inline auto string_from_buffer(const std::array<char, N>& buffer) -> std::string
{
    if constexpr (N == 0) {
        return std::string();
    }

    return std::string(buffer.data());
}

inline auto string_equals(vk::string_view lhs, vk::string_view rhs) -> bool
{
    return lhs.equals(rhs);
}

inline auto string_equals(const std::string& lhs, vk::string_view rhs) -> bool
{
    return string_equals(string_view_of(lhs), rhs);
}

inline auto starts_with(vk::string_view text, vk::string_view prefix) -> bool
{
    return text.starts_with(prefix);
}

inline auto ends_with(vk::string_view text, vk::string_view suffix) -> bool
{
    if (suffix.size() > text.size()) {
        return false;
    }

    return subview(text, text.size() - suffix.size(), suffix.size()).equals(suffix);
}

inline auto path_basename(vk::string_view path) -> vk::string_view
{
    vk_usize start = 0;
    for (vk_usize index = 0; index < path.size(); ++index) {
        if (path[index] == '/' || path[index] == '\\') {
            start = index + 1;
        }
    }

    return subview(path, start, path.size() - start);
}

inline auto trim_ascii(vk::string_view text) -> std::string
{
    vk_usize start = 0;
    vk_usize end = text.size();

    while (start < end && is_ascii_space(text[start])) {
        ++start;
    }
    while (end > start && is_ascii_space(text[end - 1])) {
        --end;
    }

    return string_from_view(subview(text, start, end - start));
}

inline auto find_substring(vk::string_view text, vk::string_view needle) -> vk_usize
{
    if (needle.empty()) {
        return 0;
    }
    if (needle.size() > text.size()) {
        return k_not_found;
    }

    for (vk_usize index = 0; index + needle.size() <= text.size(); ++index) {
        bool matches = true;
        for (vk_usize inner = 0; inner < needle.size(); ++inner) {
            if (text[index + inner] != needle[inner]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return index;
        }
    }

    return k_not_found;
}

inline auto parse_i64(vk::string_view text) -> long long
{
    vk_usize index = 0;
    while (index < text.size() && is_ascii_space(text[index])) {
        ++index;
    }

    long long sign = 1;
    if (index < text.size() && text[index] == '-') {
        sign = -1;
        ++index;
    }

    long long value = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        value = (value * 10) + static_cast<long long>(text[index] - '0');
        ++index;
    }

    return sign * value;
}

inline auto parse_u64(vk::string_view text) -> vk_u64
{
    vk_usize index = 0;
    while (index < text.size() && is_ascii_space(text[index])) {
        ++index;
    }

    vk_u64 value = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        value = (value * 10ULL) + static_cast<vk_u64>(text[index] - '0');
        ++index;
    }

    return value;
}

template <size_t N>
inline auto json_extract_string(const char* json, const char* key, std::array<char, N>& out) -> bool
{
    if constexpr (N == 0) {
        return false;
    }

    return vk_json_extract_string_field(json, key, out.data(), static_cast<vk_usize>(out.size())) != 0;
}

struct ImGuiStringInputUserData {
    std::string* string = nullptr;
    ImGuiInputTextCallback chained_callback = nullptr;
    void* chained_user_data = nullptr;
};

inline auto imgui_string_input_callback(ImGuiInputTextCallbackData* data) -> int
{
    auto* user_data = static_cast<ImGuiStringInputUserData*>(data->UserData);
    if (user_data == nullptr || user_data->string == nullptr) {
        return 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string& value = *user_data->string;
        const size_t requested_capacity = data->BufSize > 0 ? static_cast<size_t>(data->BufSize - 1) : 0;
        if (requested_capacity > value.capacity()) {
            value.reserve(requested_capacity);
        }
        value.resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = value.data();
        return 0;
    }

    if (user_data->chained_callback != nullptr) {
        data->UserData = user_data->chained_user_data;
        return user_data->chained_callback(data);
    }

    return 0;
}

inline auto imgui_input_text(const char* label,
                             std::string& value,
                             ImGuiInputTextFlags flags = 0,
                             ImGuiInputTextCallback callback = nullptr,
                             void* user_data = nullptr) -> bool
{
    ImGuiStringInputUserData callback_data = { &value, callback, user_data };
    return ImGui::InputText(label,
                            value.data(),
                            value.capacity() + 1,
                            flags | ImGuiInputTextFlags_CallbackResize,
                            imgui_string_input_callback,
                            &callback_data);
}

inline auto imgui_input_text_multiline(const char* label,
                                       std::string& value,
                                       const ImVec2& size = ImVec2(0.0f, 0.0f),
                                       ImGuiInputTextFlags flags = 0,
                                       ImGuiInputTextCallback callback = nullptr,
                                       void* user_data = nullptr) -> bool
{
    ImGuiStringInputUserData callback_data = { &value, callback, user_data };
    return ImGui::InputTextMultiline(label,
                                     value.data(),
                                     value.capacity() + 1,
                                     size,
                                     flags | ImGuiInputTextFlags_CallbackResize,
                                     imgui_string_input_callback,
                                     &callback_data);
}

inline auto imgui_title_should_use_light_text() -> bool
{
    const ImVec4& title_bg = ImGui::GetStyle().Colors[ImGuiCol_TitleBgActive];
    const float luminance = (title_bg.x * 0.2126f) + (title_bg.y * 0.7152f) + (title_bg.z * 0.0722f);
    return luminance < 0.45f;
}

inline auto imgui_begin_window_readable_caption(const char* name,
                                                bool* p_open = nullptr,
                                                ImGuiWindowFlags flags = 0) -> bool
{
    const bool use_light_title = imgui_title_should_use_light_text();
    if (use_light_title) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    const bool open = ImGui::Begin(name, p_open, flags);

    if (use_light_title) {
        ImGui::PopStyleColor();
    }

    return open;
}

} // namespace vgui

#endif // VGUI_COMMON_H