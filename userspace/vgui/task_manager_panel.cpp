#include "task_manager_panel.h"

#include "implot.h"
#include "window_manager.h"

#include <stdio.h>

namespace vgui {

auto TaskManagerPanel::task_state_label(vk_u32 state) -> const char*
{
    switch (state) {
    case 0u:
        return "ready";
    case 1u:
        return "run";
    case 2u:
        return "sleep";
    case 3u:
        return "done";
    default:
        return "?";
    }
}

void TaskManagerPanel::format_task_cpu_label(vk_u32 cpu, char* out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return;
    }

    if (cpu == VK_TASK_CPU_NONE) {
        snprintf(out, out_size, "-");
    } else {
        snprintf(out, out_size, "%u", static_cast<unsigned>(cpu));
    }
}

auto TaskManagerPanel::find_previous_task_ticks(const std::array<vk_task_info_t, k_max_tasks>& tasks,
                                                vk_usize count,
                                                vk_u64 id) -> vk_u64
{
    for (vk_usize index = 0; index < count; ++index) {
        if (tasks[index].id == id) {
            return tasks[index].cpu_ticks;
        }
    }

    return 0;
}

auto TaskManagerPanel::task_cpu_delta(const vk_task_info_t& task,
                                      const std::array<vk_task_info_t, k_max_tasks>& previous,
                                      vk_usize previous_count) -> vk_u64
{
    const vk_u64 old_ticks = find_previous_task_ticks(previous, previous_count, task.id);
    return task.cpu_ticks >= old_ticks ? task.cpu_ticks - old_ticks : 0;
}

auto TaskManagerPanel::query_cpu_count() const -> vk_u32
{
    std::array<char, 128> response {};
    std::array<char, 32> value {};

    vk_kobj_rpc_json("{\"op\":\"get\",\"path\":\"sys/cpu/count\"}", response.data(), response.size());
    if (!json_extract_string(response.data(), "value", value)) {
        return cpu_count_ != 0 ? cpu_count_ : 1u;
    }

    const vk_u64 parsed = parse_u64(buffer_view(value));
    if (parsed == 0 || parsed > 256) {
        return 1u;
    }

    return static_cast<vk_u32>(parsed);
}

void TaskManagerPanel::push_history(float percent)
{
    history_[history_head_] = percent;
    history_head_ = (history_head_ + 1) % k_history_size;
    if (history_count_ < k_history_size) {
        ++history_count_;
    }
}

auto TaskManagerPanel::row_precedes(const Row& lhs, const Row& rhs) -> bool
{
    if (lhs.cpu_percent != rhs.cpu_percent) {
        return lhs.cpu_percent > rhs.cpu_percent;
    }

    const bool lhs_running = lhs.task.state == 1u;
    const bool rhs_running = rhs.task.state == 1u;
    if (lhs_running != rhs_running) {
        return lhs_running;
    }

    return lhs.task.id < rhs.task.id;
}

void TaskManagerPanel::sort_rows()
{
    for (int index = 1; index < row_count_; ++index) {
        Row row = rows_[index];
        int insert_index = index;
        while (insert_index > 0 && row_precedes(row, rows_[insert_index - 1])) {
            rows_[insert_index] = rows_[insert_index - 1];
            --insert_index;
        }
        rows_[insert_index] = row;
    }
}

void TaskManagerPanel::copy_previous(const std::array<vk_task_info_t, k_max_tasks>& tasks, vk_usize count)
{
    if (count > static_cast<vk_usize>(k_max_tasks)) {
        count = k_max_tasks;
    }

    for (vk_usize index = 0; index < count; ++index) {
        previous_[index] = tasks[index];
    }
    previous_count_ = count;
}

void TaskManagerPanel::refresh()
{
    const vk_u64 now_tick = VK_CALL(tick_count);
    const vk_u64 ticks_per_second = VK_CALL(ticks_per_sec);
    vk_u64 refresh_period = ticks_per_second / 5ULL;
    if (refresh_period == 0) {
        refresh_period = 1;
    }

    if (last_cpu_query_tick_ == 0
        || now_tick - last_cpu_query_tick_ >= (ticks_per_second != 0 ? ticks_per_second : 1ULL)) {
        cpu_count_ = query_cpu_count();
        last_cpu_query_tick_ = now_tick;
    }

    if (last_refresh_tick_ != 0 && now_tick - last_refresh_tick_ < refresh_period) {
        return;
    }

    std::array<vk_task_info_t, k_max_tasks> tasks {};
    const vk_usize total = VK_CALL(task_snapshot, tasks.data(), k_max_tasks);
    const vk_usize count = total < static_cast<vk_usize>(k_max_tasks) ? total : k_max_tasks;
    last_refresh_tick_ = now_tick;

    if (previous_tick_ == 0) {
        for (vk_usize index = 0; index < count; ++index) {
            rows_[index].task = tasks[index];
            rows_[index].cpu_percent = 0.0f;
        }
        row_count_ = static_cast<int>(count);
        sort_rows();
        copy_previous(tasks, count);
        previous_tick_ = now_tick;
        total_cpu_percent_ = 0.0f;
        push_history(0.0f);
        return;
    }

    vk_u64 elapsed_ticks = now_tick >= previous_tick_ ? now_tick - previous_tick_ : 1ULL;
    if (elapsed_ticks == 0) {
        elapsed_ticks = 1ULL;
    }

    vk_u64 total_delta = 0;
    for (vk_usize index = 0; index < count; ++index) {
        const vk_u64 delta = task_cpu_delta(tasks[index], previous_, previous_count_);
        rows_[index].task = tasks[index];
        rows_[index].cpu_percent = static_cast<float>(delta) * 100.0f / static_cast<float>(elapsed_ticks);
        total_delta += delta;
    }

    row_count_ = static_cast<int>(count);
    sort_rows();
    copy_previous(tasks, count);
    previous_tick_ = now_tick;

    total_cpu_percent_ = static_cast<float>(total_delta) * 100.0f / static_cast<float>(elapsed_ticks);
    const float max_cpu_percent = static_cast<float>((cpu_count_ != 0 ? cpu_count_ : 1u) * 100u);
    if (total_cpu_percent_ > max_cpu_percent) {
        total_cpu_percent_ = max_cpu_percent;
    }
    push_history(total_cpu_percent_);
}

void TaskManagerPanel::draw_window(bool& visible, WindowManager& window_manager)
{
    if (!visible) {
        return;
    }

    refresh();

    ImGui::SetNextWindowPos(ImVec2(860.0f, 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470.0f, 420.0f), ImGuiCond_FirstUseEver);

    if (!imgui_begin_window_readable_caption("Task Manager", &visible)) {
        ImGui::End();
        return;
    }
    window_manager.clear_focus_if_host_window_focused();

    const float max_cpu_percent = static_cast<float>((cpu_count_ != 0 ? cpu_count_ : 1u) * 100u);

    std::array<float, k_history_size> ordered_history {};
    const int history_start = (history_count_ >= k_history_size) ? history_head_ : 0;
    for (int index = 0; index < history_count_; ++index) {
        ordered_history[index] = history_[(history_start + index) % k_history_size];
    }

    ImGui::SeparatorText("CPU Load");
    std::array<char, 64> overlay {};
    snprintf(overlay.data(), overlay.size(), "%.1f%% / %.0f%%", total_cpu_percent_, max_cpu_percent);
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(8.0f, 6.0f));
    if (ImPlot::BeginPlot("##cpu_history",
                          ImVec2(-1.0f, 90.0f),
                          ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
        const double x_max = history_count_ > 1 ? static_cast<double>(history_count_ - 1) : 1.0;
        const double y_max = max_cpu_percent > 0.0f ? static_cast<double>(max_cpu_percent) : 1.0;
        const ImVec4 line_color = ImVec4(0.33f, 0.84f, 0.52f, 1.0f);

        ImPlot::SetupAxes(nullptr,
                          nullptr,
                          ImPlotAxisFlags_NoDecorations,
                          ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxesLimits(0.0, x_max, 0.0, y_max, ImPlotCond_Always);

        if (history_count_ > 0) {
            ImPlot::PlotLine("CPU Load",
                             ordered_history.data(),
                             history_count_,
                             1.0,
                             0.0,
                             {
                                 ImPlotProp_LineColor, line_color,
                                 ImPlotProp_FillColor, line_color,
                                 ImPlotProp_FillAlpha, 0.20f,
                                 ImPlotProp_Flags, ImPlotLineFlags_Shaded
                             });
            ImPlot::TagY(static_cast<double>(total_cpu_percent_), line_color, "%.1f%%", total_cpu_percent_);
        }

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar();
    ImGui::TextDisabled("%s", history_count_ > 0 ? overlay.data() : "Collecting samples...");
    ImGui::Text("Online CPUs: %u", static_cast<unsigned>(cpu_count_));
    ImGui::SameLine();
    ImGui::Text("Tasks shown: %d", row_count_);

    ImGui::SeparatorText("Current CPU Owners");
    for (vk_u32 cpu = 0; cpu < cpu_count_; ++cpu) {
        const vk_task_info_t* owner = nullptr;
        for (int index = 0; index < row_count_; ++index) {
            if (rows_[index].task.state == 1u && rows_[index].task.cpu == cpu) {
                owner = &rows_[index].task;
                break;
            }
        }

        if (owner != nullptr) {
            ImGui::BulletText("CPU %u: %s (#%llu)",
                              static_cast<unsigned>(cpu),
                              owner->name,
                              static_cast<unsigned long long>(owner->id));
        } else {
            ImGui::BulletText("CPU %u: idle / no task", static_cast<unsigned>(cpu));
        }
    }

    ImGui::SeparatorText("Processes");
    const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_Borders
                                      | ImGuiTableFlags_Resizable
                                      | ImGuiTableFlags_ScrollY
                                      | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##task_table", 6, table_flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, 42.0f);
        ImGui::TableSetupColumn("CPU%", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Ticks", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int index = 0; index < row_count_; ++index) {
            const Row& row = rows_[index];
            std::array<char, 8> cpu_label {};
            format_task_cpu_label(row.task.cpu, cpu_label.data(), cpu_label.size());

            ImGui::TableNextRow();
            if (row.task.state == 1u) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.95f, 0.72f, 1.0f));
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(row.task.id));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cpu_label.data());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", row.cpu_percent);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(row.task.cpu_ticks));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(task_state_label(row.task.state));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(row.task.name);

            if (row.task.state == 1u) {
                ImGui::PopStyleColor();
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace vgui
