// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app/layout.h"
#include <string>
#include <vector>

namespace av::ui {

enum class TextDirection { ltr, rtl };

enum class ColumnKind { title = 0, process = 1, architecture = 2, state = 3, path = 4 };

TextDirection direction_for_language(const std::wstring& language);
bool is_rtl_language(const std::wstring& language);
bool contains_rtl_character(const std::wstring& text);

RectI mirror_rect(const RectI& rect, int client_width);
MainLayout mirror_main_layout(const MainLayout& layout, int client_width);
MainLayout resolve_main_layout(const MainLayoutSpec& spec, TextDirection direction);
bool layout_boxes_overlap(const MainLayout& layout);
int minimum_client_width(const MainLayoutSpec& spec, TextDirection direction = TextDirection::ltr);

UINT draw_text_flags(TextDirection direction, UINT base_flags);
UINT field_text_flags(ColumnKind column, TextDirection direction, UINT base_flags);
std::wstring field_text(ColumnKind column, TextDirection direction, const std::wstring& text);
std::wstring isolate_ltr(const std::wstring& text);
std::wstring isolate_rtl(const std::wstring& text);
std::vector<int> visual_column_order(TextDirection direction, int count);
int logical_column_from_visual(const std::vector<int>& order, int visual_index);

struct PopupAnchor {
    int x = 0;
    int y = 0;
    UINT flags = 0;
};
PopupAnchor popup_anchor(const RECT& button, TextDirection direction, UINT base_flags);

struct SideScrollState {
    int offset = 0;
    int max_offset = 0;
    bool needed = false;
};
SideScrollState side_scroll_state(const MainLayout& layout);
int scroll_side(int current, int delta, const MainLayout& layout);
bool side_button_reachable(const MainLayout& layout, int index, int* offset_out = nullptr);
bool side_buttons_reachable(const MainLayout& layout, int offset);

struct ThemePalette {
    COLORREF window_bg = 0;
    COLORREF panel_bg = 0;
    COLORREF text = 0;
    COLORREF muted_text = 0;
    COLORREF accent = 0;
    COLORREF selected_bg = 0;
    COLORREF selected_text = 0;
    COLORREF focus = 0;
    COLORREF error = 0;
    COLORREF border = 0;
    COLORREF field_bg = 0;
    COLORREF field_text = 0;
    COLORREF menu_bg = 0;
    COLORREF menu_text = 0;
    COLORREF menu_selected_bg = 0;
    COLORREF menu_selected_text = 0;
    bool dark = false;
};
ThemePalette palette_for(bool dark);
ThemePalette resolve_palette(int mode, bool system_dark, COLORREF accent = CLR_INVALID);
double contrast_ratio(COLORREF a, COLORREF b);
COLORREF blend_color(COLORREF a, COLORREF b, unsigned percent_of_a);

struct ThemeContrast {
    double body = 0;
    double disabled = 0;
    double focus = 0;
    double error = 0;
    double accent = 0;
    bool ok = false;
};
ThemeContrast theme_contrast(const ThemePalette& palette);

struct DialogDluMetrics {
    int avg_char_width = 6;
    int font_height = 15;
    int dpi = 96;
};
DialogDluMetrics dialog_dlu_metrics(unsigned dpi);
int dlu_to_px_x(int dlu, const DialogDluMetrics& metrics);
int dlu_to_px_y(int dlu, const DialogDluMetrics& metrics);
RectI dialog_rect_from_dlu(const RECT& dlu, const DialogDluMetrics& metrics);
RectI mirror_dialog_rect_dlu(const RECT& dlu, int dialog_width_dlu);
DialogPlacement place_dialog_dlu(const RECT& work_area, int width_dlu, int height_dlu,
    int content_height_dlu, unsigned dpi);

struct DialogScrollState {
    int offset = 0;
    int max_offset = 0;
    int page_height = 0;
    int virtual_height = 0;
    bool needed = false;
};
DialogScrollState dialog_scroll_state(const DialogPlacement& placement);
int scroll_dialog(int current, int delta, const DialogPlacement& placement);
bool dialog_control_reachable(const RECT& control, const DialogPlacement& placement);
int scroll_dialog_to_control(const RECT& control, int current, const DialogPlacement& placement);

bool font_supports_text(HDC dc, HFONT font, const std::wstring& text);

} // namespace av::ui
