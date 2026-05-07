#ifndef VGUI_VKFM_PANEL_H
#define VGUI_VKFM_PANEL_H

#include "vgui_common.h"

namespace vgui {

class ConsoleLog;
class WindowManager;

class VkfmPanel {
public:
    void draw_window(bool& visible, WindowManager& window_manager, ConsoleLog& log);

private:
    static constexpr int k_items_max = 128;
    static constexpr int k_item_len = 192;
    static constexpr vk_usize k_preview_limit = 8192;

    struct Entry {
        std::string name;
        bool is_directory = false;
        vk_u64 size = 0;
    };

    [[nodiscard]] auto query_default_path() const -> std::string;
    [[nodiscard]] auto canonicalize_absolute_path(vk::string_view path) const -> std::string;
    [[nodiscard]] auto resolve_input_path(vk::string_view raw_path) const -> std::string;
    [[nodiscard]] auto join_path(vk::string_view parent, vk::string_view child) const -> std::string;
    [[nodiscard]] auto parent_path(vk::string_view path) const -> std::string;
    [[nodiscard]] auto parse_item_record(vk::string_view record, Entry& out) const -> bool;
    [[nodiscard]] auto selected_path() const -> std::string;
    void sort_entries();
    void refresh_listing(ConsoleLog* log = nullptr);
    void select_entry(int index, bool load_preview);
    void load_preview_for_selection();
    void open_selected(ConsoleLog& log);

    std::array<Entry, k_items_max> entries_ {};
    int entry_count_ = 0;
    int selected_index_ = -1;
    bool initialized_ = false;
    std::string current_path_;
    std::string path_input_;
    std::string status_;
    std::string preview_;
};

} // namespace vgui

#endif // VGUI_VKFM_PANEL_H