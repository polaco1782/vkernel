#ifndef VGUI_TASK_MANAGER_PANEL_H
#define VGUI_TASK_MANAGER_PANEL_H

#include "vgui_common.h"

namespace vgui {

class WindowManager;

class TaskManagerPanel {
public:
    void draw_window(bool& visible, WindowManager& window_manager);

private:
    static constexpr int k_max_tasks = 64;
    static constexpr int k_history_size = 120;

    struct Row {
        vk_task_info_t task {};
        float cpu_percent = 0.0f;
    };

    [[nodiscard]] static auto task_state_label(vk_u32 state) -> const char*;
    static void format_task_cpu_label(vk_u32 cpu, char* out, size_t out_size);
    [[nodiscard]] static auto find_previous_task_ticks(const std::array<vk_task_info_t, k_max_tasks>& tasks,
                                                       vk_usize count,
                                                       vk_u64 id) -> vk_u64;
    [[nodiscard]] static auto task_cpu_delta(const vk_task_info_t& task,
                                             const std::array<vk_task_info_t, k_max_tasks>& previous,
                                             vk_usize previous_count) -> vk_u64;
    [[nodiscard]] auto query_cpu_count() const -> vk_u32;
    void push_history(float percent);
    [[nodiscard]] static auto row_precedes(const Row& lhs, const Row& rhs) -> bool;
    void sort_rows();
    void copy_previous(const std::array<vk_task_info_t, k_max_tasks>& tasks, vk_usize count);
    void refresh();

    std::array<Row, k_max_tasks> rows_ {};
    int row_count_ = 0;
    std::array<vk_task_info_t, k_max_tasks> previous_ {};
    vk_usize previous_count_ = 0;
    vk_u64 previous_tick_ = 0;
    vk_u64 last_refresh_tick_ = 0;
    std::array<float, k_history_size> history_ {};
    int history_count_ = 0;
    int history_head_ = 0;
    vk_u32 cpu_count_ = 1;
    vk_u64 last_cpu_query_tick_ = 0;
    float total_cpu_percent_ = 0.0f;
};

} // namespace vgui

#endif // VGUI_TASK_MANAGER_PANEL_H