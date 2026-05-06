#ifndef VGUI_LAUNCH_REGISTRY_H
#define VGUI_LAUNCH_REGISTRY_H

#include "vgui_common.h"

namespace vgui {

class ConsoleLog;

struct LaunchMenuEntry {
    std::string path;
    std::string label;
};

class LaunchRegistry {
public:
    void refresh(ConsoleLog& log);

    [[nodiscard]] auto empty() const -> bool { return count_ == 0; }
    [[nodiscard]] auto size() const -> int { return count_; }
    [[nodiscard]] auto entry(int index) const -> const LaunchMenuEntry& { return entries_[index]; }

private:
    static constexpr int k_capacity = 64;

    auto load_from_file(vk::string_view path) -> bool;
    void parse_launch_line(vk::string_view line);
    void add_app(vk::string_view path);
    [[nodiscard]] auto exists(vk::string_view path) const -> bool;
    void sort();

    std::array<LaunchMenuEntry, k_capacity> entries_ {};
    int count_ = 0;
};

} // namespace vgui

#endif // VGUI_LAUNCH_REGISTRY_H