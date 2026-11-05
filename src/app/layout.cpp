// SPDX-License-Identifier: Apache-2.0
#include "app/layout.h"
#include <algorithm>
#include <array>

namespace av::ui {
namespace {
constexpr int id_filter = 100;
constexpr int id_refresh = 101;
constexpr int id_exclude = 102;
constexpr int id_blank = 103;
constexpr int id_restore = 104;
constexpr int id_remember = 105;
constexpr int id_save = 106;
constexpr int id_load = 107;
constexpr int id_geometry = 108;
constexpr int id_export_list = 109;
constexpr int id_apply_rules = 110;
constexpr int id_about = 201;
constexpr int id_select_all = 211;
constexpr int id_clear_selection = 212;
constexpr int id_settings = 300;
constexpr int id_action_group = 302;
constexpr int id_profile_group = 303;
constexpr int id_exclude_all = 304;
constexpr int id_restore_all = 305;
constexpr int id_effects = 308;
constexpr int id_selection_summary = 309;
constexpr int id_batch_group = 310;
constexpr int id_clear_filter = 311;

bool is_pair(int id) {
    return id == id_exclude_all || id == id_restore_all || id == id_select_all
        || id == id_clear_selection || id == id_save || id == id_load
        || id == id_settings || id == id_about;
}

int width_for(const std::vector<ButtonMetric>& buttons, int id, int fallback = 80) {
    for (const auto& button : buttons) if (button.id == id) return button.text_width;
    return fallback;
}

struct Cursor {
    MainLayout& layout;
    int x, width, button_height, gap;
    int index = 0;
    void add(int id, int left, int top, int w, int h) {
        if (index >= 64) return;
        layout.ids[index] = id;
        layout.buttons[index++] = {left, top, left + w, top + h};
    }
    void single(int id, int& y) {
        add(id, x, y, width, button_height);
        y += button_height + gap;
    }
    void pair(int left, int right, int& y) {
        const int half = (width - gap) / 2;
        add(left, x, y, half, button_height);
        add(right, x + half + gap, y, width - half - gap, button_height);
        y += button_height + gap;
    }
};
} // namespace

RectI intersect_rect(const RectI& a, const RectI& b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
bool overlaps(const RectI& a, const RectI& b) {
    return !intersect_rect(a, b).empty();
}

int scale_dip(int dip, unsigned dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

const RectI* MainLayout::find(int id) const {
    for (int i = 0; i < button_count; ++i) if (ids[i] == id) return &buttons[i];
    return nullptr;
}

MainLayout compute_main_layout(const MainLayoutSpec& spec) {
    MainLayout layout;
    layout.margin = scale_dip(8, spec.dpi);
    layout.compact = spec.client_height < scale_dip(560, spec.dpi);
    layout.gap = scale_dip(layout.compact ? 3 : 6, spec.dpi);
    layout.button_height = scale_dip(layout.compact ? 23 : 26, spec.dpi);
    layout.side_width = scale_dip(154, spec.dpi);
    for (const auto& button : spec.buttons) {
        if (button.id == id_clear_filter) continue;
        const int padded = button.text_width + scale_dip(is_pair(button.id) ? 14 : 30, spec.dpi);
        layout.side_width = std::max(layout.side_width, is_pair(button.id)
            ? 2 * padded + scale_dip(22, spec.dpi) : padded);
    }
    const int margin = layout.margin;
    const int side_x = spec.client_width - margin - layout.side_width;
    const int left_width = std::max(scale_dip(220, spec.dpi), side_x - 2 * margin);

    layout.tab = {margin, margin, margin + left_width, margin + scale_dip(24, spec.dpi)};
    layout.filter = {margin, scale_dip(38, spec.dpi), margin + left_width - scale_dip(27, spec.dpi),
        scale_dip(38, spec.dpi) + scale_dip(24, spec.dpi)};
    layout.clear_filter = {margin + left_width - scale_dip(25, spec.dpi), scale_dip(38, spec.dpi),
        margin + left_width, scale_dip(38, spec.dpi) + scale_dip(24, spec.dpi)};
    layout.list = {margin, scale_dip(68, spec.dpi), margin + left_width,
        scale_dip(68, spec.dpi) + std::max<int>(1, spec.client_height - scale_dip(116, spec.dpi))};
    layout.summary = {margin, spec.client_height - scale_dip(45, spec.dpi), margin + left_width,
        spec.client_height - scale_dip(27, spec.dpi)};
    layout.status = {margin, spec.client_height - scale_dip(21, spec.dpi),
        std::max(margin + 1, spec.client_width - margin), spec.client_height - scale_dip(3, spec.dpi)};

    Cursor cursor{layout, side_x + scale_dip(8, spec.dpi), layout.side_width - scale_dip(16, spec.dpi),
        layout.button_height, layout.gap};
    int y = margin + scale_dip(20, spec.dpi);
    cursor.single(id_exclude, y);
    cursor.single(id_blank, y);
    cursor.single(id_effects, y);
    y += scale_dip(4, spec.dpi);
    cursor.single(id_restore, y);
    y += scale_dip(15, spec.dpi);
    const int batch_label_width = std::max(scale_dip(90, spec.dpi), spec.batch_label_width);
    layout.group_labels[2] = {side_x + scale_dip(7, spec.dpi), y - scale_dip(3, spec.dpi),
        side_x + scale_dip(7, spec.dpi) + batch_label_width, y + scale_dip(15, spec.dpi)};
    layout.separators[0] = {side_x + scale_dip(4, spec.dpi), y - scale_dip(7, spec.dpi),
        side_x + layout.side_width - scale_dip(4, spec.dpi), y - scale_dip(6, spec.dpi)};
    y += scale_dip(18, spec.dpi);
    cursor.pair(id_exclude_all, id_restore_all, y);
    y += scale_dip(8, spec.dpi);
    cursor.pair(id_select_all, id_clear_selection, y);
    cursor.single(id_refresh, y);

    layout.action_bounds = {side_x, margin, side_x + layout.side_width, y + scale_dip(3, spec.dpi)};
    layout.group_labels[0] = {side_x + scale_dip(7, spec.dpi), margin,
        side_x + scale_dip(7, spec.dpi) + std::max(scale_dip(70, spec.dpi), spec.action_label_width),
        margin + scale_dip(18, spec.dpi)};

    const int profile_y = y + scale_dip(11, spec.dpi);
    y = profile_y + scale_dip(20, spec.dpi);
    cursor.single(id_remember, y);
    cursor.single(id_apply_rules, y);
    cursor.single(id_geometry, y);
    y += scale_dip(3, spec.dpi);
    cursor.pair(id_save, id_load, y);
    cursor.single(id_export_list, y);

    layout.profile_bounds = {side_x, profile_y, side_x + layout.side_width, y + scale_dip(3, spec.dpi)};
    layout.separators[1] = {side_x + scale_dip(4, spec.dpi), profile_y - scale_dip(7, spec.dpi),
        side_x + layout.side_width - scale_dip(4, spec.dpi), profile_y - scale_dip(6, spec.dpi)};
    layout.group_labels[1] = {side_x + scale_dip(7, spec.dpi), profile_y,
        side_x + scale_dip(7, spec.dpi) + std::max(scale_dip(80, spec.dpi), spec.profile_label_width),
        profile_y + scale_dip(18, spec.dpi)};

    const int bottom_y = std::max(y + scale_dip(9, spec.dpi),
        spec.client_height - scale_dip(29, spec.dpi) - layout.button_height);
    int bottom_cursor = bottom_y;
    cursor.pair(id_settings, id_about, bottom_cursor);

    layout.side_content_height = std::max(y, bottom_cursor);
    layout.side_page_height = std::max(1, spec.client_height - 2 * margin);
    layout.max_side_scroll = std::max(0, layout.side_content_height - layout.side_page_height);
    layout.side_scroll_needed = layout.max_side_scroll > 0;
    layout.button_count = cursor.index;
    layout.list.right = std::max(layout.list.left + 1, layout.list.right);
    layout.list.bottom = std::max(layout.list.top + 1, layout.list.bottom);
    return layout;
}

DialogPlacement place_dialog(const RECT& work_area, const DialogSpec& spec, unsigned dpi) {
    DialogPlacement placement;
    const int border_width = scale_dip(16, dpi);
    const int border_height = scale_dip(39, dpi);
    const int margin = scale_dip(8, dpi);
    const int work_width = std::max(1, static_cast<int>(work_area.right - work_area.left) - 2 * margin);
    const int work_height = std::max(1, static_cast<int>(work_area.bottom - work_area.top) - 2 * margin);
    const int content_width = scale_dip(spec.content_width_dip, dpi);
    const int content_height = scale_dip(spec.content_height_dip, dpi);
    placement.width = std::min(std::max(content_width + border_width, scale_dip(spec.min_width_dip, dpi) + border_width), work_width);
    placement.height = std::min(std::max(content_height + border_height, scale_dip(spec.min_height_dip, dpi) + border_height), work_height);
    placement.virtual_height = content_height;
    placement.page_height = std::max(scale_dip(80, dpi), placement.height - border_height);
    placement.max_scroll = std::max(0, placement.virtual_height - placement.page_height);
    placement.scroll_y = placement.max_scroll > 0;
    placement.x = work_area.left + (work_area.right - work_area.left - placement.width) / 2;
    placement.y = work_area.top + (work_area.bottom - work_area.top - placement.height) / 2;
    placement.x = std::clamp(placement.x, static_cast<int>(work_area.left) + margin,
        std::max(static_cast<int>(work_area.left) + margin, static_cast<int>(work_area.right) - placement.width - margin));
    placement.y = std::clamp(placement.y, static_cast<int>(work_area.top) + margin,
        std::max(static_cast<int>(work_area.top) + margin, static_cast<int>(work_area.bottom) - placement.height - margin));
    return placement;
}

int clamp_scroll(int value, int content_height, int page_height) {
    return std::clamp(value, 0, std::max(0, content_height - page_height));
}

int scroll_to_control(const RECT& control, int current_scroll, int page_height) {
    if (control.top < current_scroll) return std::max(0, static_cast<int>(control.top));
    if (control.bottom > current_scroll + page_height) return std::max(0, static_cast<int>(control.bottom) - page_height);
    return current_scroll;
}

bool control_reachable(const RECT& control, int content_height, int page_height, int max_scroll) {
    if (control.bottom <= control.top) return true;
    const int height = control.bottom - control.top;
    if (height > page_height) {
        const int min_scroll = std::max(0, static_cast<int>(control.top) - (page_height - 1));
        return min_scroll <= max_scroll && control.top < content_height;
    }
    const int min_scroll = std::max(0, static_cast<int>(control.bottom) - page_height);
    const int max_visible = std::min(max_scroll, static_cast<int>(control.top));
    return min_scroll <= max_visible && control.bottom <= content_height;
}

bool layout_fits(const MainLayout& layout, int client_width, int client_height) {
    if (layout.list.width() < scale_dip(220, 96) || layout.list.height() <= 0) return false;
    for (int i = 0; i < layout.button_count; ++i) {
        const auto& rect = layout.buttons[i];
        if (rect.left < 0 || rect.top < 0 || rect.right > client_width || rect.bottom > client_height) return false;
        if (rect.empty()) return false;
    }
    if (layout.tab.right > client_width || layout.status.right > client_width) return false;
    if (layout.status.bottom > client_height || layout.summary.bottom > client_height) return false;
    return true;
}

bool text_size_readable(unsigned dpi, int font_dip) {
    return dpi >= 96 && font_dip >= 9;
}

} // namespace av::ui
