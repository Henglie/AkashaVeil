// SPDX-License-Identifier: Apache-2.0
#include "app/rtl_layout.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace av::ui {
namespace {

std::wstring normalize_language(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::replace(value.begin(), value.end(), L'_', L'-');
    const auto first = value.find_first_not_of(L" \t");
    const auto last = value.find_last_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    return value.substr(first, last - first + 1);
}

bool has_prefix(const std::wstring& value, const wchar_t* prefix) {
    const std::size_t length = std::wcslen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0
        && (value.size() == length || value[length] == L'-');
}

bool technical_column(ColumnKind column) {
    return column == ColumnKind::process || column == ColumnKind::architecture
        || column == ColumnKind::path;
}

} // namespace

TextDirection direction_for_language(const std::wstring& language) {
    const auto value = normalize_language(language);
    if (value.empty() || value == L"auto") return TextDirection::ltr;
    if (has_prefix(value, L"ar") || has_prefix(value, L"fa") || has_prefix(value, L"he")
        || has_prefix(value, L"ur") || has_prefix(value, L"yi") || has_prefix(value, L"dv")
        || has_prefix(value, L"ps") || has_prefix(value, L"sd"))
        return TextDirection::rtl;
    return TextDirection::ltr;
}

bool is_rtl_language(const std::wstring& language) {
    return direction_for_language(language) == TextDirection::rtl;
}

bool contains_rtl_character(const std::wstring& text) {
    for (const wchar_t c : text) {
        const unsigned value = static_cast<unsigned>(c);
        if ((value >= 0x0590 && value <= 0x08FF)
            || (value >= 0xFB1D && value <= 0xFDFF)
            || (value >= 0xFE70 && value <= 0xFEFF))
            return true;
    }
    return false;
}

RectI mirror_rect(const RectI& rect, int client_width) {
    return {client_width - rect.right, rect.top, client_width - rect.left, rect.bottom};
}

MainLayout mirror_main_layout(const MainLayout& layout, int client_width) {
    MainLayout mirrored = layout;
    mirrored.tab = mirror_rect(layout.tab, client_width);
    mirrored.filter = mirror_rect(layout.filter, client_width);
    mirrored.clear_filter = mirror_rect(layout.clear_filter, client_width);
    mirrored.list = mirror_rect(layout.list, client_width);
    mirrored.summary = mirror_rect(layout.summary, client_width);
    mirrored.status = mirror_rect(layout.status, client_width);
    mirrored.action_bounds = mirror_rect(layout.action_bounds, client_width);
    mirrored.profile_bounds = mirror_rect(layout.profile_bounds, client_width);
    for (int i = 0; i < 3; ++i) mirrored.group_labels[i] = mirror_rect(layout.group_labels[i], client_width);
    for (int i = 0; i < 2; ++i) mirrored.separators[i] = mirror_rect(layout.separators[i], client_width);
    for (int i = 0; i < layout.button_count; ++i) mirrored.buttons[i] = mirror_rect(layout.buttons[i], client_width);
    return mirrored;
}

MainLayout resolve_main_layout(const MainLayoutSpec& spec, TextDirection direction) {
    MainLayout layout = compute_main_layout(spec);
    if (direction == TextDirection::rtl) layout = mirror_main_layout(layout, spec.client_width);
    return layout;
}

bool layout_boxes_overlap(const MainLayout& layout) {
    if (overlaps(layout.list, layout.action_bounds) || overlaps(layout.list, layout.profile_bounds))
        return true;
    for (int i = 0; i < layout.button_count; ++i)
        if (overlaps(layout.list, layout.buttons[i])) return true;
    return false;
}

int minimum_client_width(const MainLayoutSpec& spec, TextDirection direction) {
    const MainLayout probe = compute_main_layout(spec);
    const int width = probe.side_width + 2 * probe.margin + scale_dip(220, spec.dpi);
    (void)direction;
    return width;
}

UINT draw_text_flags(TextDirection direction, UINT base_flags) {
    UINT flags = base_flags;
    if (direction == TextDirection::rtl) {
        const UINT alignment = flags & (DT_LEFT | DT_RIGHT | DT_CENTER);
        flags &= ~(DT_LEFT | DT_RIGHT | DT_CENTER);
        if (alignment == DT_RIGHT) flags |= DT_LEFT;
        else if (alignment == DT_LEFT) flags |= DT_RIGHT;
        else flags |= alignment;
        flags |= DT_RTLREADING;
    }
    return flags;
}

UINT field_text_flags(ColumnKind column, TextDirection direction, UINT base_flags) {
    if (technical_column(column)) {
        UINT flags = base_flags & ~(DT_LEFT | DT_RIGHT | DT_CENTER | DT_RTLREADING);
        return flags | DT_LEFT;
    }
    return draw_text_flags(direction, base_flags);
}

std::wstring isolate_ltr(const std::wstring& text) {
    if (text.empty()) return text;
    if (text.front() == L'\u202A' && text.back() == L'\u202C') return text;
    return L"\u202A" + text + L"\u202C";
}

std::wstring isolate_rtl(const std::wstring& text) {
    if (text.empty()) return text;
    if (text.front() == L'\u202B' && text.back() == L'\u202C') return text;
    return L"\u202B" + text + L"\u202C";
}

std::wstring field_text(ColumnKind column, TextDirection direction, const std::wstring& text) {
    if (technical_column(column)) return isolate_ltr(text);
    if (direction == TextDirection::rtl && contains_rtl_character(text)) return text;
    return text;
}

std::vector<int> visual_column_order(TextDirection direction, int count) {
    std::vector<int> order;
    if (count <= 0) return order;
    order.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) order.push_back(i);
    if (direction == TextDirection::rtl) std::reverse(order.begin(), order.end());
    return order;
}

int logical_column_from_visual(const std::vector<int>& order, int visual_index) {
    if (visual_index < 0 || static_cast<std::size_t>(visual_index) >= order.size()) return -1;
    return order[static_cast<std::size_t>(visual_index)];
}

PopupAnchor popup_anchor(const RECT& button, TextDirection direction, UINT base_flags) {
    PopupAnchor anchor;
    anchor.x = direction == TextDirection::rtl ? button.left : button.right;
    anchor.y = button.top;
    anchor.flags = base_flags & ~(TPM_LEFTALIGN | TPM_RIGHTALIGN | TPM_CENTERALIGN);
    anchor.flags |= direction == TextDirection::rtl ? TPM_LEFTALIGN : TPM_RIGHTALIGN;
    if (direction == TextDirection::rtl) anchor.flags |= TPM_LAYOUTRTL;
    return anchor;
}

SideScrollState side_scroll_state(const MainLayout& layout) {
    SideScrollState state;
    state.max_offset = std::max(0, layout.max_side_scroll);
    state.needed = state.max_offset > 0 || layout.side_scroll_needed;
    state.offset = 0;
    return state;
}

int scroll_side(int current, int delta, const MainLayout& layout) {
    return std::clamp(current + delta, 0, std::max(0, layout.max_side_scroll));
}

bool side_button_reachable(const MainLayout& layout, int index, int* offset_out) {
    if (index < 0 || index >= layout.button_count) return false;
    const RectI& rect = layout.buttons[index];
    const int page_top = layout.margin;
    const int page_bottom = layout.margin + layout.side_page_height;
    if (rect.height() > layout.side_page_height) return false;
    const int low = std::max(0, rect.bottom - page_bottom);
    const int high = std::min(std::max(0, layout.max_side_scroll), rect.top - page_top);
    if (low > high) return false;
    if (offset_out) *offset_out = low;
    return true;
}

bool side_buttons_reachable(const MainLayout& layout, int offset) {
    if (offset < 0 || offset > std::max(0, layout.max_side_scroll)) return false;
    const int page_top = layout.margin;
    const int page_bottom = layout.margin + layout.side_page_height;
    for (int i = 0; i < layout.button_count; ++i) {
        const RectI& rect = layout.buttons[i];
        if (rect.top - offset < page_top || rect.bottom - offset > page_bottom) return false;
    }
    return true;
}

COLORREF blend_color(COLORREF a, COLORREF b, unsigned percent_of_a) {
    const auto mix = [&](int x, int y) {
        return std::clamp<int>((x * static_cast<int>(percent_of_a)
            + y * static_cast<int>(100 - percent_of_a)) / 100, 0, 255);
    };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
        mix(GetBValue(a), GetBValue(b)));
}

ThemePalette palette_for(bool dark) {
    ThemePalette palette;
    palette.dark = dark;
    if (!dark) {
        palette.window_bg = RGB(240, 240, 240);
        palette.panel_bg = RGB(255, 255, 255);
        palette.text = RGB(0, 0, 0);
        palette.muted_text = RGB(80, 80, 80);
        palette.accent = RGB(166, 107, 55);
        palette.selected_bg = RGB(0, 110, 200);
        palette.selected_text = RGB(255, 255, 255);
        palette.focus = RGB(0, 0, 0);
        palette.error = RGB(176, 30, 30);
        palette.border = RGB(140, 140, 140);
        palette.field_bg = RGB(255, 255, 255);
        palette.field_text = RGB(0, 0, 0);
        palette.menu_bg = RGB(240, 240, 240);
        palette.menu_text = RGB(0, 0, 0);
        palette.menu_selected_bg = RGB(0, 110, 200);
        palette.menu_selected_text = RGB(255, 255, 255);
    } else {
        palette.window_bg = RGB(32, 32, 32);
        palette.panel_bg = RGB(26, 26, 26);
        palette.text = RGB(230, 230, 230);
        palette.muted_text = RGB(170, 170, 170);
        palette.accent = RGB(196, 137, 85);
        palette.selected_bg = RGB(57, 83, 112);
        palette.selected_text = RGB(255, 255, 255);
        palette.focus = RGB(255, 255, 255);
        palette.error = RGB(240, 140, 140);
        palette.border = RGB(100, 100, 100);
        palette.field_bg = RGB(24, 24, 24);
        palette.field_text = RGB(230, 230, 230);
        palette.menu_bg = RGB(26, 26, 26);
        palette.menu_text = RGB(230, 230, 230);
        palette.menu_selected_bg = RGB(65, 75, 88);
        palette.menu_selected_text = RGB(255, 255, 255);
    }
    return palette;
}

ThemePalette resolve_palette(int mode, bool system_dark, COLORREF accent) {
    ThemePalette palette = mode == 1 ? palette_for(false)
        : mode == 2 ? palette_for(true) : palette_for(system_dark);
    if (accent != CLR_INVALID) palette.accent = accent;
    return palette;
}

double contrast_ratio(COLORREF a, COLORREF b) {
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double la = 0.2126 * channel(GetRValue(a))
        + 0.7152 * channel(GetGValue(a)) + 0.0722 * channel(GetBValue(a));
    const double lb = 0.2126 * channel(GetRValue(b))
        + 0.7152 * channel(GetGValue(b)) + 0.0722 * channel(GetBValue(b));
    const double high = std::max(la, lb), low = std::min(la, lb);
    return (high + 0.05) / (low + 0.05);
}

ThemeContrast theme_contrast(const ThemePalette& palette) {
    ThemeContrast result;
    result.body = std::min({
        contrast_ratio(palette.text, palette.window_bg),
        contrast_ratio(palette.text, palette.panel_bg),
        contrast_ratio(palette.field_text, palette.field_bg),
        contrast_ratio(palette.menu_text, palette.menu_bg),
        contrast_ratio(palette.selected_text, palette.selected_bg),
        contrast_ratio(palette.menu_selected_text, palette.menu_selected_bg)});
    result.disabled = std::min({
        contrast_ratio(palette.muted_text, palette.window_bg),
        contrast_ratio(palette.muted_text, palette.panel_bg),
        contrast_ratio(palette.muted_text, palette.field_bg)});
    result.focus = std::min({
        contrast_ratio(palette.focus, palette.selected_bg),
        contrast_ratio(palette.focus, palette.panel_bg),
        contrast_ratio(palette.focus, palette.window_bg)});
    result.error = std::min({
        contrast_ratio(palette.error, palette.window_bg),
        contrast_ratio(palette.error, palette.panel_bg)});
    result.accent = contrast_ratio(palette.text, blend_color(palette.accent, palette.panel_bg, 22));
    result.ok = result.body >= 4.5 && result.disabled >= 3.0 && result.focus >= 3.0
        && result.error >= 3.0 && result.accent >= 4.5;
    return result;
}

DialogDluMetrics dialog_dlu_metrics(unsigned dpi) {
    DialogDluMetrics metrics;
    metrics.dpi = static_cast<int>(dpi ? dpi : 96);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return metrics;
    const int height = -MulDiv(9, metrics.dpi, 72);
    HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    const HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW text{};
    if (GetTextMetricsW(dc, &text)) {
        metrics.avg_char_width = std::max(1, static_cast<int>(text.tmAveCharWidth));
        metrics.font_height = std::max(1, static_cast<int>(text.tmHeight + text.tmExternalLeading));
    }
    if (old) SelectObject(dc, old);
    if (font) DeleteObject(font);
    DeleteDC(dc);
    return metrics;
}

int dlu_to_px_x(int dlu, const DialogDluMetrics& metrics) {
    return MulDiv(dlu, metrics.avg_char_width, 4);
}

int dlu_to_px_y(int dlu, const DialogDluMetrics& metrics) {
    return MulDiv(dlu, metrics.font_height, 8);
}

RectI dialog_rect_from_dlu(const RECT& dlu, const DialogDluMetrics& metrics) {
    const int left = dlu_to_px_x(dlu.left, metrics);
    const int top = dlu_to_px_y(dlu.top, metrics);
    return {left, top, left + dlu_to_px_x(dlu.right - dlu.left, metrics),
        top + dlu_to_px_y(dlu.bottom - dlu.top, metrics)};
}

RectI mirror_dialog_rect_dlu(const RECT& dlu, int dialog_width_dlu) {
    return {dialog_width_dlu - dlu.right, dlu.top,
        dialog_width_dlu - dlu.left, dlu.bottom};
}

DialogPlacement place_dialog_dlu(const RECT& work_area, int width_dlu, int height_dlu,
    int content_height_dlu, unsigned dpi) {
    const DialogDluMetrics metrics = dialog_dlu_metrics(dpi);
    const int content_width = dlu_to_px_x(width_dlu, metrics);
    const int content_height = dlu_to_px_y(content_height_dlu, metrics);
    const int border_width = scale_dip(16, dpi);
    const int border_height = scale_dip(39, dpi);
    const int margin = scale_dip(8, dpi);
    const int work_width = std::max(1, static_cast<int>(work_area.right - work_area.left) - 2 * margin);
    const int work_height = std::max(1, static_cast<int>(work_area.bottom - work_area.top) - 2 * margin);

    DialogPlacement placement;
    placement.width = std::min(content_width + border_width, work_width);
    placement.height = std::min(content_height + border_height, work_height);
    placement.virtual_height = content_height;
    placement.page_height = std::max(scale_dip(80, dpi), placement.height - border_height);
    placement.max_scroll = std::max(0, placement.virtual_height - placement.page_height);
    placement.scroll_y = placement.max_scroll > 0;
    placement.x = work_area.left + (work_area.right - work_area.left - placement.width) / 2;
    placement.y = work_area.top + (work_area.bottom - work_area.top - placement.height) / 2;
    placement.x = std::clamp(placement.x, static_cast<int>(work_area.left) + margin,
        std::max(static_cast<int>(work_area.left) + margin,
            static_cast<int>(work_area.right) - placement.width - margin));
    placement.y = std::clamp(placement.y, static_cast<int>(work_area.top) + margin,
        std::max(static_cast<int>(work_area.top) + margin,
            static_cast<int>(work_area.bottom) - placement.height - margin));
    (void)height_dlu;
    return placement;
}

DialogScrollState dialog_scroll_state(const DialogPlacement& placement) {
    DialogScrollState state;
    state.max_offset = std::max(0, placement.max_scroll);
    state.page_height = placement.page_height;
    state.virtual_height = placement.virtual_height;
    state.needed = state.max_offset > 0;
    return state;
}

int scroll_dialog(int current, int delta, const DialogPlacement& placement) {
    return std::clamp(current + delta, 0, std::max(0, placement.max_scroll));
}

bool dialog_control_reachable(const RECT& control, const DialogPlacement& placement) {
    return control_reachable(control, placement.virtual_height, placement.page_height,
        placement.max_scroll);
}

int scroll_dialog_to_control(const RECT& control, int current, const DialogPlacement& placement) {
    if (placement.page_height <= 0) return current;
    if (control.top < current) return std::max(0, static_cast<int>(control.top));
    if (control.bottom > current + placement.page_height)
        return std::clamp(static_cast<int>(control.bottom) - placement.page_height, 0,
            std::max(0, placement.max_scroll));
    return current;
}

bool font_supports_text(HDC dc, HFONT font, const std::wstring& text) {
    if (!dc || !font || text.empty()) return false;
    const HGDIOBJ old = SelectObject(dc, font);
    std::vector<WORD> glyphs(text.size(), 0);
    const DWORD result = GetGlyphIndicesW(dc, text.c_str(), static_cast<int>(text.size()),
        glyphs.data(), GGI_MARK_NONEXISTING_GLYPHS);
    SelectObject(dc, old);
    if (result == GDI_ERROR) return false;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const unsigned value = static_cast<unsigned>(text[i]);
        if (value < 0x20 || value == 0x7F) continue;
        if (glyphs[i] == 0xFFFF) return false;
    }
    return true;
}

} // namespace av::ui
