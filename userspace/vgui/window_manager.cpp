#include "window_manager.h"

#include "console_log.h"

#include <stdio.h>

namespace vgui {

void FramebufferDeleter::operator()(vk_u32* ptr) const noexcept
{
    if (ptr != nullptr) {
        free(ptr);
    }
}

auto FramebufferSurface::allocate(vk_u32 width, vk_u32 height) -> bool
{
    const vk_usize pixel_count = static_cast<vk_usize>(width) * static_cast<vk_usize>(height);
    if (pixel_count == 0) {
        return false;
    }

    const vk_usize bytes = pixel_count * sizeof(vk_u32);
    auto* pixels = static_cast<vk_u32*>(malloc(bytes));
    if (pixels == nullptr) {
        return false;
    }

    reset();
    pixels_.reset(pixels);
    pixel_count_ = pixel_count;
    zero();
    return true;
}

void FramebufferSurface::reset() noexcept
{
    pixels_.reset();
    pixel_count_ = 0;
}

void FramebufferSurface::zero() noexcept
{
    if (pixels_ && pixel_count_ != 0) {
        memset(pixels_.get(), 0, bytes());
    }
}

auto WindowManager::app_task_running(vk_i64 task_id) -> bool
{
    if (task_id < 0) {
        return false;
    }

    std::array<vk_task_info_t, 64> tasks {};
    const vk_usize total = VK_CALL(task_snapshot, tasks.data(), tasks.size());
    const vk_usize count = total < tasks.size() ? total : tasks.size();
    for (vk_usize index = 0; index < count; ++index) {
        if (static_cast<vk_i64>(tasks[index].id) == task_id) {
            return tasks[index].state != 3u;
        }
    }

    return false;
}

auto WindowManager::focused_app_index() -> int
{
    if (focused_app_ < 0 || focused_app_ >= k_max_apps) {
        return -1;
    }

    AppWindow& app = apps_[focused_app_];
    if (!app.used || !app.open || !app_task_running(app.task_id)) {
        focused_app_ = -1;
        return -1;
    }

    return focused_app_;
}

void WindowManager::clear_focus_if_host_window_focused()
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        focused_app_ = -1;
    }
}

auto WindowManager::route_key_event(const vk_key_event_t& event) -> bool
{
    const int focused = focused_app_index();
    if (focused < 0 || !vk_get_api()->vk_send_key) {
        return false;
    }

    (void)vk_get_api()->vk_send_key(static_cast<vk_u64>(apps_[focused].task_id), &event);
    return true;
}

auto WindowManager::route_mouse_event(const vk_mouse_event_t& event) -> bool
{
    const int focused = focused_app_index();
    if (focused < 0 || !vk_get_api()->vk_send_mouse) {
        return false;
    }

    (void)vk_get_api()->vk_send_mouse(static_cast<vk_u64>(apps_[focused].task_id), &event);
    return true;
}

auto WindowManager::find_free_app_slot() const -> int
{
    for (int index = 0; index < k_max_apps; ++index) {
        if (!apps_[index].used) {
            return index;
        }
    }

    return -1;
}

void WindowManager::request_app_termination(const AppWindow& app)
{
    if (app.task_id < 0 || !app_task_running(app.task_id)) {
        return;
    }

    if (vk_get_api()->vk_terminate_task(static_cast<vk_u64>(app.task_id))) {
        log_.addf("App task %lld termination requested.", static_cast<long long>(app.task_id));
    } else {
        log_.addf("Failed to terminate app task %lld.", static_cast<long long>(app.task_id));
    }
}

void WindowManager::release_app_slot(AppWindow& app)
{
    app.pixels.reset();
    app.snapshot.reset();
    app.verify.reset();
    app.used = false;
    app.open = false;
    app.close_requested = false;
    app.focus_next = false;
    app.task_id = -1;
    app.width = 0;
    app.height = 0;
    app.path.clear();
    app.title.clear();
}

void WindowManager::capture_app_snapshot(AppWindow& app)
{
    if (!app.pixels || !app.snapshot || !app.verify || app.pixels.bytes() == 0) {
        return;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        memcpy(app.verify.data(), app.pixels.data(), app.pixels.bytes());
        if (memcmp(app.verify.data(), app.pixels.data(), app.pixels.bytes()) == 0) {
            memcpy(app.snapshot.data(), app.verify.data(), app.verify.bytes());
            return;
        }
    }
}

auto WindowManager::launch_windowed_app(vk::string_view path, vk_u32 width, vk_u32 height) -> vk_i64
{
    const int slot = find_free_app_slot();
    if (slot < 0) {
        log_.add("No free app window slots.");
        return -1;
    }

    if (!vk_get_api()->vk_run_with_fb) {
        log_.add("Kernel API too old: missing run_with_fb.");
        return -1;
    }

    AppWindow& app = apps_[slot];
    release_app_slot(app);
    app.width = width;
    app.height = height;
    if (!app.pixels.allocate(width, height)
        || !app.snapshot.allocate(width, height)
        || !app.verify.allocate(width, height)) {
        release_app_slot(app);
        log_.add("Failed to allocate app surface.");
        return -1;
    }

    vk_framebuffer_info_t framebuffer = {};
    framebuffer.base = static_cast<vk_u64>(reinterpret_cast<vk_usize>(app.pixels.data()));
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.stride = width;
    framebuffer.format = VK_PIXEL_FORMAT_BGRX_8BPP;
    framebuffer.valid = 1u;

    const std::string path_string = string_from_view(path);
    const vk_i64 task = vk_get_api()->vk_run_with_fb(path_string.c_str(), &framebuffer);
    if (task < 0) {
        release_app_slot(app);
        log_.addf("Failed to start app: %s", path_string.c_str());
        return -1;
    }

    app.used = true;
    app.open = true;
    app.focus_next = true;
    app.task_id = task;
    app.path = path_string;

    std::array<char, 160> title {};
    snprintf(title.data(), title.size(), "App: %s##%lld", path_string.c_str(), static_cast<long long>(task));
    app.title = title.data();
    focused_app_ = slot;

    if (vk_get_api()->vk_set_compositor_default_fb) {
        (void)vk_get_api()->vk_set_compositor_default_fb(&framebuffer);
    }

    log_.addf("App started: %s (task %lld).", path_string.c_str(), static_cast<long long>(task));
    return task;
}

void WindowManager::shutdown()
{
    bool waiting_for_apps = false;
    for (int index = 0; index < k_max_apps; ++index) {
        AppWindow& app = apps_[index];
        if (!app.used) {
            continue;
        }

        if (app_task_running(app.task_id)) {
            if (!app.close_requested) {
                request_app_termination(app);
                app.close_requested = true;
            }
            waiting_for_apps = true;
        }
    }

    while (waiting_for_apps) {
        waiting_for_apps = false;
        for (int index = 0; index < k_max_apps; ++index) {
            AppWindow& app = apps_[index];
            if (app.used && app_task_running(app.task_id)) {
                waiting_for_apps = true;
                break;
            }
        }

        if (waiting_for_apps) {
            VK_CALL(sleep, 1);
        }
    }

    for (int index = 0; index < k_max_apps; ++index) {
        AppWindow& app = apps_[index];
        if (app.used) {
            release_app_slot(app);
        }
    }

    focused_app_ = -1;
    if (vk_get_api()->vk_set_compositor_default_fb) {
        (void)vk_get_api()->vk_set_compositor_default_fb(nullptr);
    }
}

void WindowManager::draw_windows()
{
    for (int index = 0; index < k_max_apps; ++index) {
        AppWindow& app = apps_[index];
        if (!app.used) {
            continue;
        }

        const bool running = app_task_running(app.task_id);
        if (!running) {
            app.open = false;
            app.close_requested = true;
        }

        if (!app.open) {
            if (running) {
                if (!app.close_requested) {
                    request_app_termination(app);
                    app.close_requested = true;
                }
                continue;
            }

            if (focused_app_ == index) {
                focused_app_ = -1;
            }
            release_app_slot(app);
            continue;
        }

        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(app.width) + 40.0f,
                                        static_cast<float>(app.height) + 80.0f),
                                 ImGuiCond_FirstUseEver);
        if (app.focus_next) {
            ImGui::SetNextWindowFocus();
            app.focus_next = false;
        }

        const bool draw_contents = imgui_begin_window_readable_caption(app.title.c_str(), &app.open);
        if (draw_contents) {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                focused_app_ = index;
            }

            const ImVec2 available = ImGui::GetContentRegionAvail();
            float scale_x = available.x / static_cast<float>(app.width);
            float scale_y = available.y / static_cast<float>(app.height);
            float scale = scale_x < scale_y ? scale_x : scale_y;
            if (scale <= 0.0f) {
                scale = 1.0f;
            }

            int draw_width = static_cast<int>(static_cast<float>(app.width) * scale);
            int draw_height = static_cast<int>(static_cast<float>(app.height) * scale);
            if (draw_width > static_cast<int>(available.x)) {
                draw_width = static_cast<int>(available.x);
            }
            if (draw_height > static_cast<int>(available.y)) {
                draw_height = static_cast<int>(available.y);
            }
            if (draw_width < 1) {
                draw_width = 1;
            }
            if (draw_height < 1) {
                draw_height = 1;
            }

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available.x - static_cast<float>(draw_width)) * 0.5f);
            ImGui::InvisibleButton("##app_canvas", ImVec2(static_cast<float>(draw_width), static_cast<float>(draw_height)));

            const ImVec2 p_min = ImGui::GetItemRectMin();
            const ImVec2 p_max = ImGui::GetItemRectMax();

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(p_min, p_max, IM_COL32(8, 8, 8, 255));
            capture_app_snapshot(app);

            ImGui_ImplVK_FramebufferImage image = {};
            image.pixels = app.snapshot.data();
            image.width = app.width;
            image.height = app.height;
            image.stride = app.width;
            image.format = VK_PIXEL_FORMAT_BGRX_8BPP;
            image.p_min = p_min;
            image.p_max = p_max;
            ImGui_ImplVK_AddFramebufferImage(draw_list, &image);
            draw_list->AddRect(p_min, p_max, IM_COL32(180, 180, 180, 255));
        }

        ImGui::End();

        if (!app.open) {
            if (!app.close_requested) {
                request_app_termination(app);
                app.close_requested = true;
            }

            if (!app_task_running(app.task_id)) {
                if (focused_app_ == index) {
                    focused_app_ = -1;
                }
                release_app_slot(app);
            }
        }
    }
}

} // namespace vgui
