#ifndef VGUI_CONSOLE_LOG_H
#define VGUI_CONSOLE_LOG_H

#include "vgui_common.h"

namespace vgui {

class WindowManager;

class ConsoleLog {
public:
    void add(vk::string_view message);
    void addf(const char* fmt, ...);
    void clear();
    void draw_window(bool& visible, WindowManager& window_manager);

private:
    static constexpr int k_capacity = 64;
    static constexpr vk_usize k_line_limit = 127;

    std::array<std::string, k_capacity> lines_ {};
    int head_ = 0;
    int count_ = 0;
};

} // namespace vgui

#endif // VGUI_CONSOLE_LOG_H