// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <utility>
#include <vector>

namespace av::ui {

struct RectI {
    int left = 0, top = 0, right = 0, bottom = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool empty() const { return right <= left || bottom <= top; }
    bool contains(const RectI& other) const {
        return other.left >= left && other.top >= top && other.right <= right && other.bottom <= bottom;
    }
};
RectI intersect_rect(const RectI& a, const RectI& b);
bool overlaps(const RectI& a, const RectI& b);

struct ButtonMetric {
    int id = 0;
    int text_width = 0;
};

struct MainLayoutSpec {
    int client_width = 0;
    int client_height = 0;
    unsigned dpi = 96;
    int action_label_width = 0;
    int profile_label_width = 0;
    int batch_label_width = 0;
    std::vector<ButtonMetric> buttons;
};

struct MainLayout {
    int margin = 0, gap = 0, button_height = 0, side_width = 0;
    RectI tab, filter, clear_filter, list, summary, status;
    RectI group_labels[3];
    RectI separators[2];
    RectI action_bounds, profile_bounds;
    RectI buttons[64];
    int ids[64] = {};
    int button_count = 0;
    bool compact = false;
    bool side_scroll_needed = false;
    int side_content_height = 0, side_page_height = 0, max_side_scroll = 0;
    const RectI* find(int id) const;
};

MainLayout compute_main_layout(const MainLayoutSpec& spec);

struct DialogSpec {
    int content_width_dip = 0;
    int content_height_dip = 0;
    int min_width_dip = 240;
    int min_height_dip = 160;
};

struct DialogPlacement {
    int x = 0, y = 0, width = 0, height = 0;
    int page_height = 0, virtual_height = 0, max_scroll = 0;
    bool scroll_y = false;
};

DialogPlacement place_dialog(const RECT& work_area, const DialogSpec& spec, unsigned dpi);
int clamp_scroll(int value, int content_height, int page_height);
int scroll_to_control(const RECT& control, int current_scroll, int page_height);
bool control_reachable(const RECT& control, int content_height, int page_height, int max_scroll);

int scale_dip(int dip, unsigned dpi);
bool layout_fits(const MainLayout& layout, int client_width, int client_height);
bool text_size_readable(unsigned dpi, int font_dip = 12);

} // namespace av::ui
