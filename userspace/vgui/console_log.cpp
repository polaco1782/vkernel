#include "console_log.h"

#include "window_manager.h"

#include <stdarg.h>
#include <stdio.h>

namespace vgui {

void ConsoleLog::add(vk::string_view message)
{
    if (message.size() > k_line_limit) {
        message = subview(message, 0, k_line_limit);
    }

    lines_[head_] = string_from_view(message);
    head_ = (head_ + 1) % k_capacity;
    if (count_ < k_capacity) {
        ++count_;
    }
}

void ConsoleLog::addf(const char* fmt, ...)
{
    std::array<char, 128> buffer {};

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);

    add(buffer_view(buffer));
}

void ConsoleLog::clear()
{
    count_ = 0;
    head_ = 0;
}

void ConsoleLog::draw_window(bool& visible, WindowManager& window_manager)
{
    if (!visible) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(10.0f, 330.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500.0f, 200.0f), ImGuiCond_FirstUseEver);

    if (!imgui_begin_window_readable_caption("Console", &visible)) {
        ImGui::End();
        return;
    }
    window_manager.clear_focus_if_host_window_focused();

    const float footer_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##log_scroll",
                      ImVec2(0.0f, -footer_height),
                      false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const int start = (count_ >= k_capacity) ? head_ : 0;
    for (int index = 0; index < count_; ++index) {
        const int line_index = (start + index) % k_capacity;
        ImGui::TextUnformatted(lines_[line_index].c_str());
    }

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Clear")) {
        clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d lines", count_, k_capacity);

    ImGui::End();
}

} // namespace vgui