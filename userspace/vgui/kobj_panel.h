#ifndef VGUI_KOBJ_PANEL_H
#define VGUI_KOBJ_PANEL_H

#include "vgui_common.h"

namespace vgui {

class WindowManager;

class KobjNavigator {
public:
    void refresh_selected();
    void draw_window(bool& visible, WindowManager& window_manager);

private:
    static constexpr size_t k_value_len = 256;
    static constexpr size_t k_type_len = 32;
    static constexpr size_t k_desc_len = 512;
    static constexpr size_t k_item_len = 64;
    static constexpr int k_items_max = 64;
    static constexpr int k_enum_label_max = 8;

    [[nodiscard]] auto join_path(vk::string_view parent, vk::string_view child) const -> std::string;
    [[nodiscard]] auto list_items(vk::string_view path, std::array<std::string, k_items_max>& items) const -> int;
    [[nodiscard]] auto desc_get_field(vk::string_view key) const -> std::string;
    void parse_edit_state();
    void select_path(vk::string_view path);
    void draw_tree_node(vk::string_view parent_path, const std::string& label, int depth);

    std::string selected_path_ = "sys";
    std::string value_;
    std::string type_;
    std::string desc_;
    std::string path_input_ = "sys";
    vk_u64 last_refresh_tick_ = 0;
    bool auto_refresh_ = true;
    bool writable_ = false;
    std::array<std::string, k_enum_label_max> enum_labels_ {};
    int enum_count_ = 0;
    int enum_selected_ = 0;
    std::string edit_value_;
    bool has_range_ = false;
    long long range_min_ = 0;
    long long range_max_ = 0;
};

} // namespace vgui

#endif // VGUI_KOBJ_PANEL_H