/*
 * vgui/main.cpp
 * Dear ImGui demonstration for vkernel.
 *
 * Keyboard navigation (no mouse required):
 *   Alt / F10          Activate main menu bar
 *   Arrow keys         Navigate menus / lists / sliders
 *   Enter / Space      Activate buttons, checkboxes, open menus
 *   Tab / Shift+Tab    Move focus between widgets
 *   Escape             Close popup / deactivate widget
 *   Ctrl+N             File > New  (reset counter)
 *   Ctrl+Q             Quit
 */

#include "../include/vk.h"
#include "imgui/imgui.h"
#include "imgui_impl_vk.h"

#include <stdio.h>
#include <string.h>
#include <float.h>   /* FLT_MIN */

/* ================================================================
 * Application state
 * ================================================================ */

static bool g_running       = true;
static bool g_show_info     = true;
static bool g_show_console  = true;
static bool g_show_settings = false;
static bool g_show_demo     = false;
static bool g_show_task_manager = true;
static bool g_show_kobj     = true;
static bool g_open_about    = false;   /* one-shot: triggers OpenPopup */
static vk_u32 g_default_app_w = 320;
static vk_u32 g_default_app_h = 200;
static const char* g_launch_manifest_path = "vgui_apps.txt";

struct launch_menu_entry_t {
    char path[96];
    char label[96];
};

static const int LAUNCH_MENU_CAP = 64;
static launch_menu_entry_t g_launch_apps[LAUNCH_MENU_CAP];
static int g_launch_app_count = 0;

struct wm_app_window_t {
    bool used;
    bool open;
    bool close_requested;
    bool focus_next;
    vk_i64 task_id;
    vk_u32 w;
    vk_u32 h;
    vk_u32* pixels;
    vk_u32* snapshot;
    vk_u32* verify;
    int blit_x;
    int blit_y;
    int blit_w;
    int blit_h;
    char title[64];
    /* Dynamic resize — pending dimensions set each frame when avail area
     * differs from the current buffer; actual realloc happens after the
     * size has been stable for RESIZE_DEBOUNCE_TICKS ticks. */
    vk_u32 pending_w;
    vk_u32 pending_h;
    vk_u64 resize_request_tick;
};

static const int WM_MAX_APPS = 6;
static wm_app_window_t g_apps[WM_MAX_APPS];
static int g_focused_app = -1;

static const int TASK_MANAGER_MAX_TASKS = 64;
static const int TASK_MANAGER_HISTORY = 120;

struct task_manager_row_t {
    vk_task_info_t task;
    float cpu_percent;
};

static task_manager_row_t g_task_rows[TASK_MANAGER_MAX_TASKS];
static int g_task_row_count = 0;
static vk_task_info_t g_task_prev[TASK_MANAGER_MAX_TASKS];
static vk_usize g_task_prev_count = 0;
static vk_u64 g_task_prev_tick = 0;
static vk_u64 g_task_last_refresh_tick = 0;
static float g_task_cpu_history[TASK_MANAGER_HISTORY];
static int g_task_cpu_history_count = 0;
static int g_task_cpu_history_head = 0;
static vk_u32 g_task_cpu_count = 1;
static vk_u64 g_task_last_cpu_query_tick = 0;
static float g_task_total_cpu_percent = 0.0f;

static const int KOBJ_PATH_LEN = 128;
static const int KOBJ_VALUE_LEN = 256;
static const int KOBJ_TYPE_LEN = 32;
static const int KOBJ_DESC_LEN = 512;
static const int KOBJ_ITEM_LEN = 64;
static const int KOBJ_ITEMS_MAX = 64;

static char g_kobj_selected_path[KOBJ_PATH_LEN] = "sys";
static char g_kobj_value[KOBJ_VALUE_LEN] = "";
static char g_kobj_type[KOBJ_TYPE_LEN] = "";
static char g_kobj_desc[KOBJ_DESC_LEN] = "";
static char g_kobj_path_input[KOBJ_PATH_LEN] = "sys";
static vk_u64 g_kobj_last_refresh_tick = 0;
static bool g_kobj_auto_refresh = true;

/* Edit state — populated on selection, used by the detail panel. */
static const int KOBJ_ENUM_LABEL_MAX = 8;
static const int KOBJ_ENUM_LABEL_LEN = 32;
static bool g_kobj_writable = false;
static char g_kobj_enum_labels[KOBJ_ENUM_LABEL_MAX][KOBJ_ENUM_LABEL_LEN];
static int  g_kobj_enum_count = 0;
static int  g_kobj_enum_selected = 0;
static char g_kobj_edit_buf[KOBJ_VALUE_LEN] = "";
static bool g_kobj_has_range = false;
static long long g_kobj_range_min = 0;
static long long g_kobj_range_max = 0;

static void try_resize_app(wm_app_window_t& app);

static vk_i64 launch_windowed_app(const char* path, vk_u32 w, vk_u32 h);  /* forward */
static void refresh_launch_apps();
static int focused_app_index();
static void clear_app_focus_if_window_focused();
static void draw_kobj_window();

/* --- Counter widget --- */
static int  g_counter        = 0;
static bool g_counter_wrap   = false;
static int  g_counter_max    = 100;

/* --- Settings --- */
static int   g_style_idx          = 0;      /* 0=Dark  1=Light  2=Classic */
static float g_font_scale         = 1.0f;
static bool  g_transparency       = false;  /* slow path; off by default */

/* ================================================================
 * Simple circular console log  (no heap, fixed size)
 * ================================================================ */

static const int LOG_CAP  = 64;
static const int LOG_WIDTH = 128;

static char g_log_buf[LOG_CAP][LOG_WIDTH];
static int  g_log_head  = 0;   /* next write slot */
static int  g_log_count = 0;

static void log_add(const char* msg)
{
    strncpy(g_log_buf[g_log_head], msg, LOG_WIDTH - 1);
    g_log_buf[g_log_head][LOG_WIDTH - 1] = '\0';
    g_log_head  = (g_log_head + 1) % LOG_CAP;
    if (g_log_count < LOG_CAP) ++g_log_count;
}

static void log_addf(const char* fmt, ...)
{
    char tmp[LOG_WIDTH];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    log_add(tmp);
}

static void log_clear()
{
    g_log_count = 0;
    g_log_head  = 0;
}

static const char* task_state_label(vk_u32 state)
{
    switch (state) {
    case 0u: return "ready";
    case 1u: return "run";
    case 2u: return "sleep";
    case 3u: return "done";
    default: return "?";
    }
}

static void format_task_cpu_label(vk_u32 cpu, char* out, size_t out_size)
{
    if (!out || out_size == 0)
        return;

    if (cpu == VK_TASK_CPU_NONE)
        snprintf(out, out_size, "-");
    else
        snprintf(out, out_size, "%u", (unsigned)cpu);
}

static vk_u64 find_previous_task_ticks(const vk_task_info_t* tasks,
                                       vk_usize count,
                                       vk_u64 id)
{
    for (vk_usize i = 0; i < count; ++i) {
        if (tasks[i].id == id)
            return tasks[i].cpu_ticks;
    }
    return 0;
}

static vk_u64 task_cpu_delta(const vk_task_info_t* task,
                             const vk_task_info_t* prev,
                             vk_usize prev_count)
{
    vk_u64 old_ticks = find_previous_task_ticks(prev, prev_count, task->id);
    return task->cpu_ticks >= old_ticks ? task->cpu_ticks - old_ticks : 0;
}

static vk_u32 task_manager_query_cpu_count()
{
    char out[128];
    vk_kobj_rpc_json("{\"op\":\"get\",\"path\":\"sys/cpu/count\"}", out, sizeof(out));

    const char* marker = strstr(out, "\"value\":\"");
    if (!marker)
        return g_task_cpu_count ? g_task_cpu_count : 1u;

    marker += 9;
    vk_u64 value = 0;
    while (*marker >= '0' && *marker <= '9') {
        value = value * 10ULL + (vk_u64)(*marker - '0');
        ++marker;
    }

    if (value == 0 || value > 256)
        return 1u;

    return (vk_u32)value;
}

static void task_manager_push_history(float percent)
{
    g_task_cpu_history[g_task_cpu_history_head] = percent;
    g_task_cpu_history_head = (g_task_cpu_history_head + 1) % TASK_MANAGER_HISTORY;
    if (g_task_cpu_history_count < TASK_MANAGER_HISTORY)
        ++g_task_cpu_history_count;
}

static bool task_manager_row_precedes(const task_manager_row_t& lhs,
                                      const task_manager_row_t& rhs)
{
    if (lhs.cpu_percent != rhs.cpu_percent)
        return lhs.cpu_percent > rhs.cpu_percent;

    const bool lhs_running = lhs.task.state == 1u;
    const bool rhs_running = rhs.task.state == 1u;
    if (lhs_running != rhs_running)
        return lhs_running;

    return lhs.task.id < rhs.task.id;
}

static void task_manager_sort_rows()
{
    for (int i = 1; i < g_task_row_count; ++i) {
        task_manager_row_t row = g_task_rows[i];
        int j = i;
        while (j > 0 && task_manager_row_precedes(row, g_task_rows[j - 1])) {
            g_task_rows[j] = g_task_rows[j - 1];
            --j;
        }
        g_task_rows[j] = row;
    }
}

static void task_manager_copy_previous(const vk_task_info_t* tasks, vk_usize count)
{
    if (count > TASK_MANAGER_MAX_TASKS)
        count = TASK_MANAGER_MAX_TASKS;

    if (count > 0)
        memcpy(g_task_prev, tasks, sizeof(g_task_prev[0]) * (size_t)count);
    g_task_prev_count = count;
}

static void task_manager_refresh()
{
    vk_u64 now_tick = VK_CALL(tick_count);
    vk_u64 tps = VK_CALL(ticks_per_sec);
    vk_u64 refresh_period = tps / 5ULL;
    if (refresh_period == 0)
        refresh_period = 1;

    if (g_task_last_cpu_query_tick == 0 || now_tick - g_task_last_cpu_query_tick >= (tps ? tps : 1ULL)) {
        g_task_cpu_count = task_manager_query_cpu_count();
        g_task_last_cpu_query_tick = now_tick;
    }

    if (g_task_last_refresh_tick != 0 && now_tick - g_task_last_refresh_tick < refresh_period)
        return;

    vk_task_info_t tasks[TASK_MANAGER_MAX_TASKS];
    vk_usize total = VK_CALL(task_snapshot, tasks, TASK_MANAGER_MAX_TASKS);
    vk_usize count = total < TASK_MANAGER_MAX_TASKS ? total : TASK_MANAGER_MAX_TASKS;
    g_task_last_refresh_tick = now_tick;

    if (g_task_prev_tick == 0) {
        for (vk_usize i = 0; i < count; ++i) {
            g_task_rows[i].task = tasks[i];
            g_task_rows[i].cpu_percent = 0.0f;
        }
        g_task_row_count = (int)count;
        task_manager_sort_rows();
        task_manager_copy_previous(tasks, count);
        g_task_prev_tick = now_tick;
        g_task_total_cpu_percent = 0.0f;
        task_manager_push_history(0.0f);
        return;
    }

    vk_u64 elapsed_ticks = now_tick >= g_task_prev_tick ? now_tick - g_task_prev_tick : 1ULL;
    if (elapsed_ticks == 0)
        elapsed_ticks = 1;

    vk_u64 total_delta = 0;
    for (vk_usize i = 0; i < count; ++i) {
        vk_u64 delta = task_cpu_delta(&tasks[i], g_task_prev, g_task_prev_count);
        g_task_rows[i].task = tasks[i];
        g_task_rows[i].cpu_percent = (float)delta * 100.0f / (float)elapsed_ticks;
        total_delta += delta;
    }

    g_task_row_count = (int)count;
    task_manager_sort_rows();
    task_manager_copy_previous(tasks, count);
    g_task_prev_tick = now_tick;

    g_task_total_cpu_percent = (float)total_delta * 100.0f / (float)elapsed_ticks;
    const float max_percent = (float)((g_task_cpu_count ? g_task_cpu_count : 1u) * 100u);
    if (g_task_total_cpu_percent > max_percent)
        g_task_total_cpu_percent = max_percent;
    task_manager_push_history(g_task_total_cpu_percent);
}

static void copy_text(char* out, size_t out_cap, const char* text)
{
    if (!out || out_cap == 0)
        return;

    if (!text)
        text = "";

    strncpy(out, text, out_cap - 1);
    out[out_cap - 1] = '\0';
}

static void kobj_build_path_request(char* out,
                                    size_t out_cap,
                                    const char* op,
                                    const char* path)
{
    if (!out || out_cap == 0)
        return;

    vk_usize pos = 0;
    out[pos++] = '{';
    out[pos++] = '"'; out[pos++] = 'o'; out[pos++] = 'p'; out[pos++] = '"';
    out[pos++] = ':'; out[pos++] = '"';
    pos = vk_json_copy_escaped(out, (vk_usize)out_cap, pos, op ? op : "");
    out[pos++] = '"';
    out[pos++] = ','; out[pos++] = '"'; out[pos++] = 'p'; out[pos++] = 'a'; out[pos++] = 't'; out[pos++] = 'h'; out[pos++] = '"';
    out[pos++] = ':'; out[pos++] = '"';
    pos = vk_json_copy_escaped(out, (vk_usize)out_cap, pos, path ? path : "");
    out[pos++] = '"';
    out[pos++] = '}';
    if (pos >= (vk_usize)out_cap)
        pos = (vk_usize)out_cap - 1;
    out[pos] = '\0';
}

static bool json_extract_string_field(const char* json,
                                      const char* key,
                                      char* out,
                                      size_t out_cap)
{
    if (!json || !key || !out || out_cap == 0)
        return false;

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* cur = strstr(json, pattern);
    if (!cur)
        return false;

    cur += strlen(pattern);
    size_t pos = 0;

    while (*cur != '\0') {
        char ch = *cur++;
        if (ch == '"')
            break;

        if (ch == '\\' && *cur != '\0') {
            char esc = *cur++;
            if (esc == 'n') ch = '\n';
            else if (esc == 'r') ch = '\r';
            else if (esc == 't') ch = '\t';
            else ch = esc;
        }

        if (pos + 1 < out_cap)
            out[pos++] = ch;
    }

    out[pos] = '\0';
    return true;
}

static bool json_response_ok(const char* json)
{
    return json && strstr(json, "\"ok\":true") != nullptr;
}

static int json_extract_items(const char* json,
                              char items[][KOBJ_ITEM_LEN],
                              int max_items)
{
    if (!json || !items || max_items <= 0)
        return 0;

    const char* cur = strstr(json, "\"items\":[");
    if (!cur)
        return 0;

    cur += 9;
    int count = 0;

    while (*cur != '\0' && *cur != ']' && count < max_items) {
        while (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n' || *cur == ',')
            ++cur;

        if (*cur != '"')
            break;

        ++cur;
        size_t pos = 0;
        while (*cur != '\0' && *cur != '"') {
            char ch = *cur++;
            if (ch == '\\' && *cur != '\0') {
                char esc = *cur++;
                if (esc == 'n') ch = '\n';
                else if (esc == 'r') ch = '\r';
                else if (esc == 't') ch = '\t';
                else ch = esc;
            }

            if (pos + 1 < KOBJ_ITEM_LEN)
                items[count][pos++] = ch;
        }

        items[count][pos] = '\0';
        if (*cur == '"')
            ++cur;
        ++count;
    }

    return count;
}

static void kobj_join_path(const char* parent,
                           const char* child,
                           char* out,
                           size_t out_cap)
{
    if (!out || out_cap == 0)
        return;

    if (!parent || parent[0] == '\0') {
        copy_text(out, out_cap, child);
        return;
    }

    snprintf(out, out_cap, "%s/%s", parent, child ? child : "");
    out[out_cap - 1] = '\0';
}

static int kobj_ls_items(const char* path,
                         char items[][KOBJ_ITEM_LEN],
                         int max_items)
{
    char req[256];
    char out[1536];

    kobj_build_path_request(req, sizeof(req), "ls", path);
    vk_kobj_rpc_json(req, out, sizeof(out));

    if (!json_response_ok(out))
        return 0;

    return json_extract_items(out, items, max_items);
}

/* Parse a "key:   value\n" line from describe text.
 * Returns pointer to value (trimmed) in a static buffer, or nullptr. */
static const char* desc_get_field(const char* text, const char* key)
{
    static char s_buf[256];
    size_t klen = strlen(key);
    const char* p = text;
    while (*p) {
        if (strncmp(p, key, klen) == 0) {
            const char* v = p + klen;
            while (*v == ' ' || *v == '\t' || *v == ':') ++v;
            size_t n = 0;
            while (v[n] && v[n] != '\n' && n + 1 < sizeof(s_buf)) ++n;
            memcpy(s_buf, v, n);
            s_buf[n] = '\0';
            /* rtrim */
            while (n > 0 && (s_buf[n-1] == ' ' || s_buf[n-1] == '\t')) s_buf[--n] = '\0';
            return s_buf;
        }
        while (*p && *p != '\n') ++p;
        if (*p == '\n') ++p;
    }
    return nullptr;
}

/* Simple signed integer parser (no stdlib dependency). */
static long long parse_ll(const char* s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') ++s;
    long long sign = 1, v = 0;
    if (*s == '-') { sign = -1; ++s; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); }
    return sign * v;
}

static void kobj_parse_edit_state()
{
    g_kobj_writable = false;
    g_kobj_enum_count = 0;
    g_kobj_enum_selected = 0;
    g_kobj_has_range = false;
    g_kobj_range_min = 0;
    g_kobj_range_max = 0;
    g_kobj_edit_buf[0] = '\0';

    const char* w = desc_get_field(g_kobj_desc, "writable");
    if (w && strcmp(w, "yes") == 0)
        g_kobj_writable = true;

    if (!g_kobj_writable)
        return;

    /* Enum labels: "labels:   a, b, c" */
    if (strcmp(g_kobj_type, "enum") == 0) {
        const char* labels = desc_get_field(g_kobj_desc, "labels");
        if (labels) {
            const char* p = labels;
            while (*p && g_kobj_enum_count < KOBJ_ENUM_LABEL_MAX) {
                while (*p == ' ') ++p;
                size_t n = 0;
                while (p[n] && p[n] != ',' && n + 1 < (size_t)KOBJ_ENUM_LABEL_LEN) ++n;
                /* rtrim */
                size_t m = n;
                while (m > 0 && (p[m-1] == ' ')) --m;
                memcpy(g_kobj_enum_labels[g_kobj_enum_count], p, m);
                g_kobj_enum_labels[g_kobj_enum_count][m] = '\0';
                ++g_kobj_enum_count;
                p += n;
                if (*p == ',') ++p;
            }
        }
        /* find current index */
        for (int i = 0; i < g_kobj_enum_count; ++i) {
            if (strcmp(g_kobj_enum_labels[i], g_kobj_value) == 0) {
                g_kobj_enum_selected = i;
                break;
            }
        }
        return;
    }

    /* Range for numeric types */
    const char* range = desc_get_field(g_kobj_desc, "range");
    if (range && strcmp(range, "(unbounded)") != 0) {
        const char* dots = strstr(range, "..");
        if (dots) {
            g_kobj_range_min = parse_ll(range);
            g_kobj_range_max = parse_ll(dots + 2);
            g_kobj_has_range = (g_kobj_range_max > g_kobj_range_min);
        }
    }

    /* Seed edit buffer with current value */
    strncpy(g_kobj_edit_buf, g_kobj_value, sizeof(g_kobj_edit_buf) - 1);
    g_kobj_edit_buf[sizeof(g_kobj_edit_buf) - 1] = '\0';
}

static void kobj_refresh_selected()
{
    char req[256];
    char out[1536];

    kobj_build_path_request(req, sizeof(req), "get", g_kobj_selected_path);
    vk_kobj_rpc_json(req, out, sizeof(out));
    if (!json_extract_string_field(out, "value", g_kobj_value, sizeof(g_kobj_value)))
        copy_text(g_kobj_value, sizeof(g_kobj_value), "(unavailable)");
    if (!json_extract_string_field(out, "type", g_kobj_type, sizeof(g_kobj_type)))
        copy_text(g_kobj_type, sizeof(g_kobj_type), "(unknown)");

    kobj_build_path_request(req, sizeof(req), "describe", g_kobj_selected_path);
    vk_kobj_rpc_json(req, out, sizeof(out));
    if (!json_extract_string_field(out, "text", g_kobj_desc, sizeof(g_kobj_desc)))
        copy_text(g_kobj_desc, sizeof(g_kobj_desc), "(no description)");

    kobj_parse_edit_state();

    g_kobj_last_refresh_tick = VK_CALL(tick_count);
}

static void kobj_select_path(const char* path)
{
    if (!path || path[0] == '\0')
        return;

    copy_text(g_kobj_selected_path, sizeof(g_kobj_selected_path), path);
    copy_text(g_kobj_path_input, sizeof(g_kobj_path_input), path);
    kobj_refresh_selected();
}

static void draw_kobj_tree_node(const char* parent_path,
                                const char* label,
                                int depth)
{
    if (!label || label[0] == '\0' || depth > 8)
        return;

    char path[KOBJ_PATH_LEN];
    kobj_join_path(parent_path, label, path, sizeof(path));

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (strcmp(g_kobj_selected_path, path) == 0)
        flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx(path, flags, "%s", label);
    if (ImGui::IsItemClicked())
        kobj_select_path(path);

    if (open) {
        char children[KOBJ_ITEMS_MAX][KOBJ_ITEM_LEN];
        int child_count = kobj_ls_items(path, children, KOBJ_ITEMS_MAX);
        if (child_count == 0) {
            ImGui::TextDisabled("(empty)");
        } else {
            for (int i = 0; i < child_count; ++i)
                draw_kobj_tree_node(path, children[i], depth + 1);
        }
        ImGui::TreePop();
    }
}

static bool string_ends_with(const char* text, const char* suffix)
{
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (text_len < suffix_len)
        return false;
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

static const char* path_basename(const char* path)
{
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

static void trim_ascii_whitespace(char* text)
{
    char* start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        ++start;

    char* end = start + strlen(start);
    while (end > start) {
        char ch = end[-1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            break;
        --end;
    }
    *end = '\0';

    if (start != text)
        memmove(text, start, (size_t)(end - start) + 1);
}

static bool launch_app_exists(const char* path)
{
    for (int i = 0; i < g_launch_app_count; ++i) {
        if (strcmp(g_launch_apps[i].path, path) == 0)
            return true;
    }
    return false;
}

static void launch_app_add(const char* path)
{
    if (!path || *path == '\0')
        return;
    if (!string_ends_with(path, ".vbin"))
        return;
    if (g_launch_app_count >= LAUNCH_MENU_CAP)
        return;
    if (launch_app_exists(path))
        return;

    launch_menu_entry_t& entry = g_launch_apps[g_launch_app_count++];
    strncpy(entry.path, path, sizeof(entry.path) - 1);
    entry.path[sizeof(entry.path) - 1] = '\0';

    const char* base = path_basename(path);
    strncpy(entry.label, base, sizeof(entry.label) - 1);
    entry.label[sizeof(entry.label) - 1] = '\0';
}

static void parse_launch_line(char* line)
{
    trim_ascii_whitespace(line);
    if (line[0] == '\0' || line[0] == '#')
        return;

    const char* path = line;
    if (strncmp(line, "run", 3) == 0 && (line[3] == ' ' || line[3] == '\t')) {
        path = line + 3;
        while (*path == ' ' || *path == '\t')
            ++path;
    }

    launch_app_add(path);
}

static void sort_launch_apps()
{
    for (int i = 1; i < g_launch_app_count; ++i) {
        launch_menu_entry_t entry = g_launch_apps[i];
        int j = i;
        while (j > 0 && strcmp(g_launch_apps[j - 1].label, entry.label) > 0) {
            g_launch_apps[j] = g_launch_apps[j - 1];
            --j;
        }
        g_launch_apps[j] = entry;
    }
}

static bool load_launch_apps_from_file(const char* path)
{
    vk_file_handle_t fh = VK_CALL(file_open, path, "r");
    if (fh == (vk_file_handle_t)0)
        return false;

    char chunk[256];
    char line[128];
    int line_pos = 0;
    vk_usize read_count;

    while ((read_count = VK_CALL(file_read_handle, fh, chunk, sizeof(chunk))) > 0) {
        for (vk_usize i = 0; i < read_count; ++i) {
            const char ch = chunk[i];
            if (ch == '\r')
                continue;

            if (ch == '\n' || line_pos >= (int)sizeof(line) - 1) {
                line[line_pos] = '\0';
                parse_launch_line(line);
                line_pos = 0;
                if (ch == '\n')
                    continue;
            }

            line[line_pos++] = ch;
        }
    }

    if (line_pos > 0) {
        line[line_pos] = '\0';
        parse_launch_line(line);
    }

    VK_CALL(file_close, fh);
    return true;
}

static void refresh_launch_apps()
{
    g_launch_app_count = 0;

    const char* source = nullptr;
    if (load_launch_apps_from_file(g_launch_manifest_path)) {
        source = g_launch_manifest_path;
    } else if (load_launch_apps_from_file("shell.txt")) {
        source = "shell.txt";
    }

    sort_launch_apps();

    if (source && g_launch_app_count > 0) {
        log_addf("Launch menu loaded %d app%s from %s.",
                 g_launch_app_count,
                 g_launch_app_count == 1 ? "" : "s",
                 source);
    } else if (source) {
        log_addf("Launch menu file %s did not contain any runnable apps.", source);
    } else {
        log_add("Launch menu file not found.");
    }
}

/* ================================================================
 * apply_style — set ImGui color scheme from g_style_idx
 * ================================================================ */

static void apply_style(int idx)
{
    switch (idx) {
    case 1:  ImGui::StyleColorsLight();   break;
    case 2:  ImGui::StyleColorsClassic(); break;
    default: ImGui::StyleColorsDark();    break;
    }

    /* When transparency is disabled, force every theme color fully
     * opaque so the renderer's fast path is taken on every fill. */
    if (!g_transparency) {
        ImGuiStyle& s = ImGui::GetStyle();
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
            s.Colors[i].w = 1.0f;
    }
}

/* ================================================================
 * draw_menu_bar
 * ================================================================ */

static void draw_menu_bar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    /* ---- File ---- */
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) {
            g_counter = 0;
            log_add("File > New: counter reset.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
            log_add("Quit requested via menu.");
            g_running = false;
        }
        ImGui::EndMenu();
    }

    /* ---- Edit ---- */
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Increment Counter", "+")) {
            ++g_counter;
            log_addf("Counter incremented to %d.", g_counter);
        }
        if (ImGui::MenuItem("Decrement Counter", "-")) {
            --g_counter;
            log_addf("Counter decremented to %d.", g_counter);
        }
        if (ImGui::MenuItem("Reset Counter")) {
            g_counter = 0;
            log_add("Counter reset.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...")) {
            g_show_settings = true;
            log_add("Opened Settings.");
        }
        ImGui::EndMenu();
    }

    /* ---- Launch ---- */
    if (ImGui::BeginMenu("Launch")) {
        if (ImGui::MenuItem("Refresh App List"))
            refresh_launch_apps();

        ImGui::Separator();

        if (g_launch_app_count == 0) {
            ImGui::BeginDisabled();
            ImGui::MenuItem("No staged apps found", nullptr, false, false);
            ImGui::EndDisabled();
        } else {
            for (int i = 0; i < g_launch_app_count; ++i) {
                if (ImGui::MenuItem(g_launch_apps[i].label))
                    (void)launch_windowed_app(g_launch_apps[i].path, g_default_app_w, g_default_app_h);
            }
        }

        ImGui::EndMenu();
    }

    /* ---- View ---- */
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Info Panel",   nullptr, &g_show_info);
        ImGui::MenuItem("Console",      nullptr, &g_show_console);
        ImGui::MenuItem("Task Manager", nullptr, &g_show_task_manager);
        ImGui::MenuItem("KObj Navigator", nullptr, &g_show_kobj);
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo",   nullptr, &g_show_demo);
        ImGui::EndMenu();
    }

    /* ---- Help ---- */
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About vGUI...")) {
            g_open_about = true;
        }
        ImGui::EndMenu();
    }

    /* Right-aligned FPS */
    {
        ImGuiIO& io = ImGui::GetIO();
        char fps_buf[32];
        unsigned fps10 = (unsigned)(io.Framerate * 10.0f + 0.5f);
        snprintf(fps_buf, sizeof(fps_buf), "%u.%u FPS", fps10 / 10, fps10 % 10);
        float w = ImGui::CalcTextSize(fps_buf).x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - w);
        ImGui::TextDisabled("%s", fps_buf);
    }

    ImGui::EndMainMenuBar();
}

/* ================================================================
 * draw_info_window
 * ================================================================ */

static void draw_info_window(const vk_framebuffer_info_t* fb)
{
    if (!g_show_info) return;

    ImGui::SetNextWindowPos (ImVec2(10.0f, 30.0f),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 290.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Info Panel", &g_show_info)) {
        ImGui::End();
        return;
    }
    clear_app_focus_if_window_focused();

    /* ---- Framebuffer info ---- */
    ImGui::SeparatorText("Framebuffer");
    ImGui::Text("Resolution  : %u x %u", fb->width, fb->height);
    ImGui::Text("Stride      : %u px",   fb->stride);
    ImGui::Text("Format      : %s",
        fb->format == VK_PIXEL_FORMAT_BGRX_8BPP ? "BGRX-8bpp" :
        fb->format == VK_PIXEL_FORMAT_RGBX_8BPP ? "RGBX-8bpp" :
        fb->format == VK_PIXEL_FORMAT_BITMASK   ? "Bitmask"   : "BLT-only");
    ImGui::Text("Base        : 0x%llx", (unsigned long long)fb->base);

    /* ---- Timing ---- */
    ImGui::SeparatorText("Timing");
    ImGuiIO& io = ImGui::GetIO();
    unsigned frame_ms100 = (unsigned)(io.DeltaTime * 100000.0f + 0.5f); /* ms * 100 */
    unsigned fps10 = (unsigned)(io.Framerate * 10.0f + 0.5f);
    ImGui::Text("Frame time  : %u.%02u ms", frame_ms100 / 100, frame_ms100 % 100);
    ImGui::Text("FPS         : %u.%u",      fps10 / 10, fps10 % 10);

    const vk_api_t* api  = vk_get_api();
    vk_u64          tick = api->vk_tick_count();
    vk_u32          tps  = api->vk_ticks_per_sec();
    vk_u64          sec  = tps ? (tick / tps) : 0;
    ImGui::Text("Uptime      : %llus",    (unsigned long long)sec);

    /* ---- Counter demo ---- */
    ImGui::SeparatorText("Counter");

    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##counter_val", &g_counter, 0, 0);

    ImGui::SameLine();
    if (ImGui::Button(" + ")) {
        if (g_counter_wrap && g_counter >= g_counter_max)
            g_counter = 0;
        else
            ++g_counter;
        log_addf("Counter → %d", g_counter);
    }
    ImGui::SameLine();
    if (ImGui::Button(" - ")) {
        --g_counter;
        log_addf("Counter → %d", g_counter);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        g_counter = 0;
        log_add("Counter reset.");
    }

    /* Clamp display value for progress bar */
    int   display_max = (g_counter_max > 0) ? g_counter_max : 1;
    float progress    = (float)g_counter / (float)display_max;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));

    ImGui::Checkbox("Wrap at max", &g_counter_wrap);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("Max", &g_counter_max, 1, 10);

    ImGui::End();
}

/* ================================================================
 * draw_console_window
 * ================================================================ */

static void draw_console_window()
{
    if (!g_show_console) return;

    ImGui::SetNextWindowPos (ImVec2(10.0f,  330.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500.0f, 200.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Console", &g_show_console)) {
        ImGui::End();
        return;
    }
    clear_app_focus_if_window_focused();

    /* Scrolling text region. */
    float footer_h = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##log_scroll", ImVec2(0.0f, -footer_h),
                       false, ImGuiWindowFlags_HorizontalScrollbar);

    /* Print from oldest to newest (circular buffer iteration). */
    int start = (g_log_count >= LOG_CAP) ? g_log_head : 0;
    for (int i = 0; i < g_log_count; ++i) {
        int idx = (start + i) % LOG_CAP;
        ImGui::TextUnformatted(g_log_buf[idx]);
    }

    /* Auto-scroll to bottom. */
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Clear"))
        log_clear();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d lines", g_log_count, LOG_CAP);

    ImGui::End();
}

static void draw_task_manager_window()
{
    if (!g_show_task_manager) return;

    task_manager_refresh();

    ImGui::SetNextWindowPos(ImVec2(860.0f, 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470.0f, 420.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Task Manager", &g_show_task_manager)) {
        ImGui::End();
        return;
    }
    clear_app_focus_if_window_focused();

    const float max_cpu_percent = (float)((g_task_cpu_count ? g_task_cpu_count : 1u) * 100u);
    float history[TASK_MANAGER_HISTORY];
    int history_count = g_task_cpu_history_count;
    int history_start = (g_task_cpu_history_count >= TASK_MANAGER_HISTORY) ? g_task_cpu_history_head : 0;
    for (int i = 0; i < history_count; ++i)
        history[i] = g_task_cpu_history[(history_start + i) % TASK_MANAGER_HISTORY];

    ImGui::SeparatorText("CPU Load");
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "%.1f%% / %.0f%%", g_task_total_cpu_percent, max_cpu_percent);
    ImGui::PlotLines("##cpu_history",
                     history_count > 0 ? history : nullptr,
                     history_count,
                     0,
                     history_count > 0 ? overlay : "Collecting samples...",
                     0.0f,
                     max_cpu_percent,
                     ImVec2(-1.0f, 90.0f));
    ImGui::Text("Online CPUs: %u", (unsigned)g_task_cpu_count);
    ImGui::SameLine();
    ImGui::Text("Tasks shown: %d", g_task_row_count);

    ImGui::SeparatorText("Current CPU Owners");
    for (vk_u32 cpu = 0; cpu < g_task_cpu_count; ++cpu) {
        const vk_task_info_t* owner = nullptr;
        for (int i = 0; i < g_task_row_count; ++i) {
            if (g_task_rows[i].task.state == 1u && g_task_rows[i].task.cpu == cpu) {
                owner = &g_task_rows[i].task;
                break;
            }
        }

        if (owner) {
            ImGui::BulletText("CPU %u: %s (#%llu)",
                              (unsigned)cpu,
                              owner->name,
                              (unsigned long long)owner->id);
        } else {
            ImGui::BulletText("CPU %u: idle / no task", (unsigned)cpu);
        }
    }

    ImGui::SeparatorText("Processes");
    ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_Borders
                                | ImGuiTableFlags_Resizable
                                | ImGuiTableFlags_ScrollY
                                | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##task_table", 6, table_flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("PID",   ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("CPU",   ImGuiTableColumnFlags_WidthFixed, 42.0f);
        ImGui::TableSetupColumn("CPU%",  ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Ticks", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < g_task_row_count; ++i) {
            const task_manager_row_t& row = g_task_rows[i];
            char cpu_buf[8];
            format_task_cpu_label(row.task.cpu, cpu_buf, sizeof(cpu_buf));

            ImGui::TableNextRow();
            if (row.task.state == 1u)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.95f, 0.72f, 1.0f));

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", (unsigned long long)row.task.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cpu_buf);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", row.cpu_percent);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", (unsigned long long)row.task.cpu_ticks);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(task_state_label(row.task.state));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(row.task.name);

            if (row.task.state == 1u)
                ImGui::PopStyleColor();
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

static void draw_kobj_window()
{
    if (!g_show_kobj) return;

    if (g_kobj_auto_refresh) {
        vk_u64 now = VK_CALL(tick_count);
        vk_u32 tps = VK_CALL(ticks_per_sec);
        vk_u64 period = tps ? (vk_u64)(tps / 2u) : 0;
        if (period == 0 || now - g_kobj_last_refresh_tick >= period)
            kobj_refresh_selected();
    }

    ImGui::SetNextWindowPos(ImVec2(860.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470.0f, 260.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("KObj Navigator", &g_show_kobj)) {
        ImGui::End();
        return;
    }
    clear_app_focus_if_window_focused();

    if (ImGui::Button("Refresh"))
        kobj_refresh_selected();
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &g_kobj_auto_refresh);

    ImGui::SetNextItemWidth(-64.0f);
    ImGui::InputText("##kobj_path", g_kobj_path_input, sizeof(g_kobj_path_input));
    ImGui::SameLine();
    if (ImGui::Button("Go"))
        kobj_select_path(g_kobj_path_input);

    ImGui::BeginChild("##kobj_tree", ImVec2(180.0f, 0.0f), true);
    char roots[KOBJ_ITEMS_MAX][KOBJ_ITEM_LEN];
    int root_count = kobj_ls_items("", roots, KOBJ_ITEMS_MAX);
    if (root_count == 0) {
        ImGui::TextDisabled("No kobj nodes.");
    } else {
        for (int i = 0; i < root_count; ++i)
            draw_kobj_tree_node("", roots[i], 0);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##kobj_detail", ImVec2(0.0f, 0.0f), true);
    ImGui::Text("Path: %s", g_kobj_selected_path);
    ImGui::Text("Type: %s", g_kobj_type[0] ? g_kobj_type : "(unknown)");
    ImGui::SeparatorText("Value");
    ImGui::TextWrapped("%s", g_kobj_value[0] ? g_kobj_value : "(empty)");

    if (g_kobj_writable) {
        ImGui::SeparatorText("Edit");

        bool did_set = false;
        char set_req[512];
        char set_out[256];

        if (strcmp(g_kobj_type, "enum") == 0 && g_kobj_enum_count > 0) {
            /* Build a pointer array for the Combo */
            const char* items[KOBJ_ENUM_LABEL_MAX];
            for (int i = 0; i < g_kobj_enum_count; ++i)
                items[i] = g_kobj_enum_labels[i];

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##kobj_enum", &g_kobj_enum_selected, items, g_kobj_enum_count))
                did_set = true;

            if (did_set) {
                snprintf(set_req, sizeof(set_req),
                    "{\"op\":\"set\",\"path\":\"%s\",\"value\":\"%s\"}",
                    g_kobj_selected_path,
                    g_kobj_enum_labels[g_kobj_enum_selected]);
                vk_kobj_rpc_json(set_req, set_out, sizeof(set_out));
                kobj_refresh_selected();
            }
        } else if (strcmp(g_kobj_type, "bool") == 0) {
            bool cur_bool = (strcmp(g_kobj_value, "yes") == 0 || strcmp(g_kobj_value, "true") == 0);
            if (ImGui::Checkbox("Enabled##kobj_bool", &cur_bool)) {
                snprintf(set_req, sizeof(set_req),
                    "{\"op\":\"set\",\"path\":\"%s\",\"value\":\"%s\"}",
                    g_kobj_selected_path, cur_bool ? "true" : "false");
                vk_kobj_rpc_json(set_req, set_out, sizeof(set_out));
                kobj_refresh_selected();
            }
        } else if ((strcmp(g_kobj_type, "u64") == 0 || strcmp(g_kobj_type, "i64") == 0)
                   && g_kobj_has_range) {
            int cur_int = (int)parse_ll(g_kobj_value);
            int rmin = (int)g_kobj_range_min;
            int rmax = (int)g_kobj_range_max;
            ImGui::SetNextItemWidth(-80.0f);
            if (ImGui::SliderInt("##kobj_slider", &cur_int, rmin, rmax)) {
                snprintf(g_kobj_edit_buf, sizeof(g_kobj_edit_buf), "%d", cur_int);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set##kobj_range")) {
                snprintf(set_req, sizeof(set_req),
                    "{\"op\":\"set\",\"path\":\"%s\",\"value\":\"%s\"}",
                    g_kobj_selected_path, g_kobj_edit_buf);
                vk_kobj_rpc_json(set_req, set_out, sizeof(set_out));
                kobj_refresh_selected();
            }
        } else {
            ImGui::SetNextItemWidth(-80.0f);
            ImGui::InputText("##kobj_edit", g_kobj_edit_buf, sizeof(g_kobj_edit_buf));
            ImGui::SameLine();
            if (ImGui::Button("Set##kobj_text")) {
                snprintf(set_req, sizeof(set_req),
                    "{\"op\":\"set\",\"path\":\"%s\",\"value\":\"%s\"}",
                    g_kobj_selected_path, g_kobj_edit_buf);
                vk_kobj_rpc_json(set_req, set_out, sizeof(set_out));
                kobj_refresh_selected();
            }
        }
    }

    ImGui::SeparatorText("Describe");
    ImGui::TextWrapped("%s", g_kobj_desc[0] ? g_kobj_desc : "(no description)");
    ImGui::EndChild();

    ImGui::End();
}

/* ================================================================
 * draw_settings_window
 * ================================================================ */

static void draw_settings_window()
{
    if (!g_show_settings) return;

    ImGui::SetNextWindowPos (ImVec2(200.0f, 150.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(270.0f, 170.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Settings", &g_show_settings, ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }
    clear_app_focus_if_window_focused();

    ImGui::SeparatorText("Appearance");

    const char* style_names[] = { "Dark", "Light", "Classic" };
    if (ImGui::Combo("Color scheme", &g_style_idx, style_names, 3)) {
        apply_style(g_style_idx);
        log_addf("Style changed to %s.", style_names[g_style_idx]);
    }

    if (ImGui::SliderFloat("Font scale", &g_font_scale, 0.5f, 2.0f, "%.1f")) {
        ImGui::GetIO().FontGlobalScale = g_font_scale;
    }

    ImGui::SeparatorText("Renderer");

    if (ImGui::Checkbox("Transparency (slow)", &g_transparency)) {
        ImGui_ImplVK_SetTransparencyEnabled(g_transparency);
        /* Re-apply style so theme alphas reflect the new mode. */
        apply_style(g_style_idx);
        log_addf("Transparency %s.", g_transparency ? "ON" : "OFF");
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
        g_show_settings = false;

    ImGui::End();
}

/* ================================================================
 * draw_about_modal
 * ================================================================ */

static void draw_about_modal()
{
    /* One-shot: open on the frame the flag is set. */
    if (g_open_about) {
        ImGui::OpenPopup("About vGUI");
        g_open_about = false;
    }

    /* Center on screen each time the popup appears. */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(320.0f, 160.0f),     ImGuiCond_Always);

    if (ImGui::BeginPopupModal("About vGUI", nullptr,
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove))
    {
        ImGui::TextWrapped("vGUI — Dear ImGui GUI for vkernel");
        ImGui::Spacing();
        ImGui::TextWrapped("Software renderer: barycentric triangle fill,");
        ImGui::TextWrapped("alpha-8 font atlas, framebuffer blending.");
        ImGui::Spacing();
        ImGui::TextWrapped("Keyboard-only navigation, newlib C runtime.");
        ImGui::Spacing();
        ImGui::Separator();

        float btn_w = 100.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btn_w) * 0.5f);
        if (ImGui::Button("OK", ImVec2(btn_w, 0.0f)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

static bool app_task_running(vk_i64 task_id)
{
    if (task_id < 0) return false;
    vk_task_info_t tasks[64];
    vk_usize total = VK_CALL(task_snapshot, tasks, 64);
    vk_usize count = total < 64 ? total : 64;
    for (vk_usize i = 0; i < count; ++i) {
        if ((vk_i64)tasks[i].id == task_id)
            return tasks[i].state != 3u; /* terminated */
    }
    return false;
}

static int focused_app_index()
{
    if (g_focused_app < 0 || g_focused_app >= WM_MAX_APPS)
        return -1;

    wm_app_window_t& app = g_apps[g_focused_app];
    if (!app.used || !app.open || !app_task_running(app.task_id)) {
        g_focused_app = -1;
        return -1;
    }

    return g_focused_app;
}

static void clear_app_focus_if_window_focused()
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        g_focused_app = -1;
}

static void capture_app_snapshot(wm_app_window_t& app)
{
    if (!app.pixels || !app.snapshot || !app.verify) return;

    vk_usize app_bytes = (vk_usize)app.w * app.h * sizeof(vk_u32);
    for (int attempt = 0; attempt < 3; ++attempt) {
        VK_CALL(memcpy, app.verify, app.pixels, app_bytes);
        if (VK_CALL(memcmp, app.verify, app.pixels, app_bytes) == 0) {
            VK_CALL(memcpy, app.snapshot, app.verify, app_bytes);
            return;
        }
    }
}

static void request_app_termination(const wm_app_window_t& app)
{
    if (app.task_id < 0)
        return;

    if (!app_task_running(app.task_id))
        return;

    if (vk_get_api()->vk_terminate_task((vk_u64)app.task_id))
        log_addf("App task %lld termination requested.", (long long)app.task_id);
    else
        log_addf("Failed to terminate app task %lld.", (long long)app.task_id);
}

static void release_app_slot(wm_app_window_t& app)
{
    if (app.pixels) {
        VK_CALL(free, app.pixels);
        app.pixels = nullptr;
    }
    if (app.snapshot) {
        VK_CALL(free, app.snapshot);
        app.snapshot = nullptr;
    }
    if (app.verify) {
        VK_CALL(free, app.verify);
        app.verify = nullptr;
    }
    app = {};
}

static int find_free_app_slot()
{
    for (int i = 0; i < WM_MAX_APPS; ++i)
        if (!g_apps[i].used) return i;
    return -1;
}

static void try_resize_app(wm_app_window_t& app)
{
    static const vk_u32 RESIZE_DEBOUNCE_TICKS = 20; /* ~200 ms at 100 Hz */
    static const vk_u32 MIN_SIZE = 64;
    static const vk_u32 MAX_SIZE = 2048;

    if (app.pending_w == 0 || app.pending_h == 0)
        return;

    /* Clamp to sane bounds */
    vk_u32 nw = app.pending_w < MIN_SIZE ? MIN_SIZE : (app.pending_w > MAX_SIZE ? MAX_SIZE : app.pending_w);
    vk_u32 nh = app.pending_h < MIN_SIZE ? MIN_SIZE : (app.pending_h > MAX_SIZE ? MAX_SIZE : app.pending_h);

    /* Already at this size? */
    if (nw == app.w && nh == app.h) {
        app.pending_w = 0;
        app.pending_h = 0;
        return;
    }

    /* Wait for the size to be stable (debounce) */
    vk_u64 now = VK_CALL(tick_count);
    if (now - app.resize_request_tick < RESIZE_DEBOUNCE_TICKS)
        return;

    /* Kernel API must support set_task_framebuffer */
    if (!vk_get_api()->vk_set_task_framebuffer)
        return;

    /* Allocate new buffers */
    vk_usize new_bytes = (vk_usize)nw * nh * sizeof(vk_u32);
    vk_u32* new_pixels   = (vk_u32*)VK_CALL(malloc, new_bytes);
    vk_u32* new_snapshot = (vk_u32*)VK_CALL(malloc, new_bytes);
    vk_u32* new_verify   = (vk_u32*)VK_CALL(malloc, new_bytes);
    if (!new_pixels || !new_snapshot || !new_verify) {
        VK_CALL(free, new_pixels);
        VK_CALL(free, new_snapshot);
        VK_CALL(free, new_verify);
        app.pending_w = 0;
        app.pending_h = 0;
        return;
    }
    VK_CALL(memset, new_pixels,   0, new_bytes);
    VK_CALL(memset, new_snapshot, 0, new_bytes);
    VK_CALL(memset, new_verify,   0, new_bytes);

    /* Tell the kernel to redirect the app's framebuffer to the new buffer.
     * After this returns the app will write into new_pixels on its next frame. */
    vk_framebuffer_info_t new_fb = {};
    new_fb.base   = (vk_u64)(vk_usize)new_pixels;
    new_fb.width  = nw;
    new_fb.height = nh;
    new_fb.stride = nw;
    new_fb.format = VK_PIXEL_FORMAT_BGRX_8BPP;
    new_fb.valid  = 1u;
    vk_get_api()->vk_set_task_framebuffer((vk_u64)app.task_id, &new_fb);

    /* Free old buffers and swap in new ones */
    VK_CALL(free, app.pixels);
    VK_CALL(free, app.snapshot);
    VK_CALL(free, app.verify);

    app.pixels   = new_pixels;
    app.snapshot = new_snapshot;
    app.verify   = new_verify;
    app.w        = nw;
    app.h        = nh;
    app.pending_w = 0;
    app.pending_h = 0;
}

static vk_i64 launch_windowed_app(const char* path, vk_u32 w, vk_u32 h)
{
    int slot = find_free_app_slot();
    if (slot < 0) {
        log_add("No free app window slots.");
        return -1;
    }

    if (!vk_get_api()->vk_run_with_fb) {
        log_add("Kernel API too old: missing run_with_fb.");
        return -1;
    }

    wm_app_window_t& app = g_apps[slot];
    app = {};
    app.w = w;
    app.h = h;
    app.pixels = (vk_u32*)VK_CALL(malloc, (vk_usize)w * h * sizeof(vk_u32));
    app.snapshot = (vk_u32*)VK_CALL(malloc, (vk_usize)w * h * sizeof(vk_u32));
    app.verify = (vk_u32*)VK_CALL(malloc, (vk_usize)w * h * sizeof(vk_u32));
    if (!app.pixels || !app.snapshot || !app.verify) {
        release_app_slot(app);
        log_add("Failed to allocate app surface.");
        return -1;
    }
    VK_CALL(memset, app.pixels, 0, (vk_usize)w * h * sizeof(vk_u32));
    VK_CALL(memset, app.snapshot, 0, (vk_usize)w * h * sizeof(vk_u32));
    VK_CALL(memset, app.verify, 0, (vk_usize)w * h * sizeof(vk_u32));

    vk_framebuffer_info_t app_fb = {};
    app_fb.base = (vk_u64)(vk_usize)app.pixels;
    app_fb.width = w;
    app_fb.height = h;
    app_fb.stride = w;
    app_fb.format = VK_PIXEL_FORMAT_BGRX_8BPP;
    app_fb.valid = 1u;

    vk_i64 task = vk_get_api()->vk_run_with_fb(path, &app_fb);
    if (task < 0) {
        release_app_slot(app);
        log_addf("Failed to start app: %s", path);
        return -1;
    }
    app.used = true;
    app.open = true;
    app.focus_next = true;
    app.task_id = task;
    app.blit_x = app.blit_y = -1;
    app.blit_w = app.blit_h = 0;
    snprintf(app.title, sizeof(app.title), "App: %s##%lld", path, (long long)task);
    g_focused_app = slot;

    if (vk_get_api()->vk_set_compositor_default_fb)
        (void)vk_get_api()->vk_set_compositor_default_fb(&app_fb);

    log_addf("App started: %s (task %lld).", path, (long long)task);
    return task;
}

static void draw_app_windows()
{
    for (int i = 0; i < WM_MAX_APPS; ++i) {
        wm_app_window_t& app = g_apps[i];
        if (!app.used) continue;
        bool running = app_task_running(app.task_id);
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
            if (g_focused_app == i)
                g_focused_app = -1;
            release_app_slot(app);
            continue;
        }

        ImGui::SetNextWindowSize(ImVec2((float)app.w + 40.0f, (float)app.h + 80.0f), ImGuiCond_FirstUseEver);
        if (app.focus_next) {
            ImGui::SetNextWindowFocus();
            app.focus_next = false;
        }
        bool draw_contents = ImGui::Begin(app.title, &app.open);
        if (draw_contents) {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                g_focused_app = i;

            ImVec2 avail = ImGui::GetContentRegionAvail();
            /* Keep aspect ratio; allow both up- and down-scaling so the
             * image fills the window without distortion or clipping. */
            float sx = (float)avail.x / (float)app.w;
            float sy = (float)avail.y / (float)app.h;
            float s = sx < sy ? sx : sy;
            if (s <= 0.0f) s = 1.0f;
            int draw_w = (int)((float)app.w * s);
            int draw_h = (int)((float)app.h * s);
            if (draw_w > (int)avail.x) draw_w = (int)avail.x;
            if (draw_h > (int)avail.y) draw_h = (int)avail.y;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - (float)draw_w) * 0.5f);
            ImGui::InvisibleButton("##app_canvas", ImVec2((float)draw_w, (float)draw_h));
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            app.blit_x = (int)p0.x;
            app.blit_y = (int)p0.y;
            app.blit_w = draw_w;
            app.blit_h = draw_h;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, IM_COL32(8, 8, 8, 255));
            capture_app_snapshot(app);
            ImGui_ImplVK_FramebufferImage image = {};
            image.pixels = app.snapshot;
            image.width = app.w;
            image.height = app.h;
            image.stride = app.w;
            image.format = VK_PIXEL_FORMAT_BGRX_8BPP;
            image.p_min = p0;
            image.p_max = p1;
            ImGui_ImplVK_AddFramebufferImage(dl, &image);
            dl->AddRect(p0, p1, IM_COL32(180, 180, 180, 255));
        }

        ImGui::End();

        if (!app.open) {
            if (!app.close_requested) {
                request_app_termination(app);
                app.close_requested = true;
            }

            if (!app_task_running(app.task_id)) {
                if (g_focused_app == i)
                    g_focused_app = -1;
                release_app_slot(app);
            }
        }
    }
}

/* ================================================================
 * main
 * ================================================================ */

int main(int /*argc*/, char** /*argv*/)
{
    /* ---- Framebuffer ---- */
    vk_framebuffer_info_t fb = {};
    VK_CALL(framebuffer_info, &fb);

    if (!fb.valid || fb.base == 0 || fb.width == 0 || fb.height == 0) {
        VK_CALL(puts, "vgui: no framebuffer available\n");
        return 1;
    }

    /* Default app buffer at half the screen resolution — large enough that
     * upscaling inside the window manager is minimal and quality is good. */
    g_default_app_w = fb.width  / 2;
    g_default_app_h = fb.height / 2;
    if (g_default_app_w < 320) g_default_app_w = 320;
    if (g_default_app_h < 200) g_default_app_h = 200;

    /* ---- ImGui context ---- */
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   /* no .ini file (kernel has no writable FS) */
        io.LogFilename = nullptr;
    }

    /* ---- Backend ---- */
    if (!ImGui_ImplVK_Init(&fb)) {
        VK_CALL(puts, "vgui: backend init failed\n");
        ImGui::DestroyContext();
        return 1;
    }

    /* ---- Style ----
     * Performance critical for the software rasterizer:
     *   - Rounded corners generate a fan of ~12 small non-AA-aligned
     *     triangles per corner that fall through to the slow general
     *     triangle path.  Set to 0 so rects stay as 2 axis-aligned
     *     triangles that hit the try_render_quad fast path.
     *   - AntiAliasedLines / AntiAliasedFill add a 1-pixel feathered
     *     edge with per-vertex alpha to every shape, generating many
     *     additional thin triangles with attribute interpolation.
     *     Disabling them keeps every filled shape as one solid quad.
     */
    ImGui_ImplVK_SetTransparencyEnabled(g_transparency);
    apply_style(g_style_idx);
    {
        ImGuiStyle& s            = ImGui::GetStyle();
        s.WindowRounding         = 0.0f;
        s.ChildRounding          = 0.0f;
        s.FrameRounding          = 0.0f;
        s.PopupRounding          = 0.0f;
        s.ScrollbarRounding      = 0.0f;
        s.GrabRounding           = 0.0f;
        s.TabRounding            = 0.0f;
        s.WindowBorderSize       = 1.0f;
        s.FrameBorderSize        = 0.0f;
        s.AntiAliasedLines       = false;
        s.AntiAliasedLinesUseTex = false;
        s.AntiAliasedFill        = false;
    }

    /* ---- Startup log ---- */
    log_add("vGUI started.");
    log_addf("Framebuffer: %ux%u @ %s",
        fb.width, fb.height,
        fb.format == VK_PIXEL_FORMAT_BGRX_8BPP ? "BGRX" : "RGBX");
    log_add("Move the mouse to control the cursor.");
    log_add("Alt to open the menu bar.  Tab/Arrows to navigate.");
    log_add("Enter/Space to activate.  Ctrl+Q to quit.");
    log_add("Use Launch from the menu bar to start staged apps.");
    refresh_launch_apps();
    kobj_refresh_selected();

    if (vk_get_api()->vk_set_compositor_active)
        (void)vk_get_api()->vk_set_compositor_active(1u);

    /* ================================================================
     * Main loop
     * ================================================================ */
    while (g_running) {
        for (int i = 0; i < WM_MAX_APPS; ++i) {
            g_apps[i].blit_x = -1;
            g_apps[i].blit_y = -1;
            g_apps[i].blit_w = 0;
            g_apps[i].blit_h = 0;
        }

        /* --- 1. Drain keyboard events --- */
        {
            vk_key_event_t evt;
            while (vk_get_api()->vk_poll_key(&evt)) {
                int focused = focused_app_index();
                if (focused >= 0 && vk_get_api()->vk_send_key) {
                    (void)vk_get_api()->vk_send_key((vk_u64)g_apps[focused].task_id, &evt);
                } else {
                    ImGui_ImplVK_ProcessKey(&evt);
                }

                /* Hard quit: Ctrl+Q (checked in raw events as well) */
                if (evt.pressed &&
                    (evt.modifiers & 2u) &&
                    (evt.ascii == 'q' || evt.ascii == 'Q'))
                {
                    g_running = false;
                }
            }
        }

        /* --- 1b. Drain mouse events --- */
        {
            vk_mouse_event_t mev;
            while (vk_get_api()->vk_poll_mouse(&mev)) {
                ImGui_ImplVK_ProcessMouse(&mev);
                int focused = focused_app_index();
                if (focused >= 0 && vk_get_api()->vk_send_mouse) {
                    (void)vk_get_api()->vk_send_mouse((vk_u64)g_apps[focused].task_id, &mev);
                }
            }
        }

        /* --- 2. Begin frame --- */
        ImGui_ImplVK_NewFrame();
        ImGui::NewFrame();

        /* --- 3. Global keyboard shortcuts (ImGui-side) --- */
        if (ImGui::GetIO().KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
                log_add("Ctrl+Q — quitting.");
                g_running = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
                g_counter = 0;
                log_add("Ctrl+N — counter reset.");
            }
        }

        /* --- 4. Build UI --- */
        draw_menu_bar();
        draw_info_window(&fb);
        draw_console_window();
        draw_task_manager_window();
        draw_kobj_window();
        draw_app_windows();
        draw_settings_window();
        draw_about_modal();

        if (g_show_demo)
            ImGui::ShowDemoWindow(&g_show_demo);

        /* --- 5. Render --- */
        ImGui::Render();
        ImGui_ImplVK_RenderDrawData(ImGui::GetDrawData(), &fb);

        /* --- 6. Yield CPU slice back to the scheduler --- */
        vk_get_api()->vk_sleep(1);
    }

    /* ---- Cleanup ---- */
    ImGui_ImplVK_Shutdown();
    ImGui::DestroyContext();

    if (vk_get_api()->vk_set_compositor_active)
        (void)vk_get_api()->vk_set_compositor_active(0u);
    if (vk_get_api()->vk_set_compositor_default_fb)
        (void)vk_get_api()->vk_set_compositor_default_fb(nullptr);

    VK_CALL(puts, "vgui: clean exit.\n");
    return 0;
}
