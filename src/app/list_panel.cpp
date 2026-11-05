// SPDX-License-Identifier: Apache-2.0
#include "app/list_panel.h"
#include "app/rtl_layout.h"
#include <imm.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <map>
#include <tuple>

namespace av::ui {
namespace {

int scale(int dip, unsigned dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

std::wstring lower_copy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

unsigned long long number_or_zero(const std::wstring& text) {
    wchar_t* end = nullptr;
    const auto value = std::wcstoull(text.c_str(), &end, 10);
    return end && *end == L'\0' ? value : 0;
}

bool state_equal(const ListRow& a, const ListRow& b) {
    return a.state_text == b.state_text && a.protected_state == b.protected_state
        && a.error == b.error && a.mixed == b.mixed;
}

} // namespace

std::uint64_t GenerationGate::begin() { return ++current_; }
bool GenerationGate::accept(std::uint64_t token) const { return token == current_; }
std::uint64_t GenerationGate::current() const { return current_; }

std::wstring fold_case(std::wstring value) { return lower_copy(std::move(value)); }

std::wstring display_title(const ListRow& row, bool process_view) {
    if (process_view) return row.process_name.empty() ? row.title : row.process_name;
    return row.title.empty() ? row.process_name : row.title;
}

std::wstring sort_text(const ListRow& row, SortColumn column, bool process_view) {
    switch (column) {
    case SortColumn::title: return fold_case(display_title(row, process_view));
    case SortColumn::process: return row.pid_text;
    case SortColumn::architecture: return fold_case(row.architecture);
    case SortColumn::state: return fold_case(row.state_text);
    case SortColumn::path: return fold_case(row.path);
    }
    return {};
}

bool sort_less(const ListRow& a, const ListRow& b, SortSpec spec, bool process_view) {
    int order = 0;
    if (spec.column == SortColumn::process) {
        const auto left = number_or_zero(a.pid_text);
        const auto right = number_or_zero(b.pid_text);
        order = left < right ? -1 : left > right ? 1 : 0;
        if (!order) order = sort_text(a, spec.column, process_view).compare(sort_text(b, spec.column, process_view));
    } else {
        const auto left = sort_text(a, spec.column, process_view);
        const auto right = sort_text(b, spec.column, process_view);
        order = left.compare(right);
    }
    if (!order) order = a.id < b.id ? -1 : b.id < a.id ? 1 : 0;
    if (!order) return false;
    return spec.descending ? order > 0 : order < 0;
}

std::wstring accessible_item_text(const ListRow& row, bool process_view) {
    auto name = display_title(row, process_view);
    if (!row.state_text.empty()) name += L" \x2014 " + row.state_text;
    if (row.mixed && row.state_text.find(L"Mixed") == std::wstring::npos) name += L" \x2014 Mixed";
    if (row.error) name += L" \x2014 Error";
    return name;
}

std::wstring selection_summary(std::size_t rows, std::size_t selected_rows, std::size_t target_windows) {
    return L"Rows: " + std::to_wstring(rows) + L" / Selected: " + std::to_wstring(selected_rows)
        + L" / Target windows: " + std::to_wstring(target_windows);
}

void ListModel::set_source(std::vector<ListRow> rows) {
    source_ = std::move(rows);
    rebuild();
}
void ListModel::set_process_view(bool value) {
    if (process_view_ == value) return;
    process_view_ = value;
    rebuild();
}
void ListModel::set_filter(std::wstring query) {
    const auto folded = lower_copy(std::move(query));
    if (filter_ == folded) return;
    filter_ = folded;
    rebuild();
}
void ListModel::set_sort(SortSpec spec) {
    sort_ = spec;
    rebuild();
}
void ListModel::set_mixed_text(std::wstring text) {
    if (text.empty()) return;
    mixed_text_ = std::move(text);
    rebuild();
}

void ListModel::rebuild() {
    std::vector<ListRow> filtered;
    filtered.reserve(source_.size());
    for (const auto& row : source_) {
        if (!filter_.empty()) {
            const auto haystack = lower_copy(row.title + L" " + row.process_name + L" "
                + row.path + L" " + row.pid_text + L" " + row.state_text);
            if (haystack.find(filter_) == std::wstring::npos) continue;
        }
        if (process_view_) {
            const auto key = std::make_pair(row.id.pid,
                static_cast<unsigned long long>(row.id.process_created));
            auto found = std::find_if(filtered.begin(), filtered.end(), [&](const ListRow& item) {
                return item.id.pid == row.id.pid && item.id.process_created == row.id.process_created;
            });
            if (found != filtered.end()) {
                if (!state_equal(*found, row)) {
                    found->mixed = true;
                    found->state_text = mixed_text_;
                    found->protected_state = found->protected_state || row.protected_state;
                    found->error = found->error || row.error;
                }
                continue;
            }
            (void)key;
        }
        filtered.push_back(row);
    }
    std::stable_sort(filtered.begin(), filtered.end(), [&](const ListRow& a, const ListRow& b) {
        return sort_less(a, b, sort_, process_view_);
    });
    visible_ = std::move(filtered);
}

std::size_t ListModel::find_visible(const RowIdentity& id) const {
    for (std::size_t i = 0; i < visible_.size(); ++i) if (visible_[i].id == id) return i;
    return visible_.size();
}

SelectionSnapshot ListModel::capture(const std::vector<std::size_t>& selected, long focus, long top) const {
    SelectionSnapshot snapshot;
    for (const auto index : selected) {
        if (index < visible_.size()) snapshot.selected.push_back(visible_[index].id);
    }
    if (focus >= 0 && static_cast<std::size_t>(focus) < visible_.size()) {
        snapshot.focus = visible_[focus].id;
        snapshot.has_focus = true;
    }
    if (top >= 0 && static_cast<std::size_t>(top) < visible_.size()) {
        snapshot.top = visible_[top].id;
        snapshot.has_top = true;
    }
    return snapshot;
}

std::vector<std::size_t> ListModel::resolve(const SelectionSnapshot& snapshot, int& focus_out, int& top_out) const {
    std::vector<std::size_t> result;
    for (const auto& id : snapshot.selected) {
        const auto index = find_visible(id);
        if (index < visible_.size()) result.push_back(index);
    }
    const auto focus = snapshot.has_focus ? find_visible(snapshot.focus) : visible_.size();
    const auto top = snapshot.has_top ? find_visible(snapshot.top) : visible_.size();
    focus_out = focus < visible_.size() ? static_cast<int>(focus) : -1;
    top_out = top < visible_.size() ? static_cast<int>(top) : -1;
    return result;
}

void paint_focus_rect(HDC dc, RECT rect, COLORREF color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const int saved = SaveDC(dc);
    for (int x = rect.left; x < rect.right; x += 2) {
        SetPixelV(dc, x, rect.top, color);
        SetPixelV(dc, x, rect.bottom - 1, color);
    }
    for (int y = rect.top; y < rect.bottom; y += 2) {
        SetPixelV(dc, rect.left, y, color);
        SetPixelV(dc, rect.right - 1, y, color);
    }
    RestoreDC(dc, saved);
}

void paint_row(HDC dc, const RowPaintInfo& info, const ThemeColors& theme,
    HFONT font, HIMAGELIST images, unsigned dpi) {
    if (!dc) return;
    const auto& bounds = info.bounds;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;

    COLORREF fill = theme.panel;
    if (info.row_index % 2) fill = blend_color(theme.panel, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), theme.dark ? 96 : 96);
    if (info.row.protected_state || info.row.mixed) fill = blend_color(theme.accent, fill, 22);
    if (info.selected) fill = theme.selected_fill;
    const COLORREF ink = info.selected ? theme.selected_text
        : info.disabled || info.busy ? blend_color(theme.text, fill, 48) : theme.text;

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    const HBRUSH brush = CreateSolidBrush(fill);
    FillRect(dc, &bounds, brush);
    DeleteObject(brush);
    if (info.row.error) {
        RECT edge = bounds;
        edge.right = edge.left + scale(3, dpi);
        const HBRUSH error = CreateSolidBrush(theme.error);
        FillRect(dc, &edge, error);
        DeleteObject(error);
    }
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    for (int column = 0; column < 5; ++column) {
        RECT cell = info.cells[column];
        if (cell.right <= cell.left) continue;
        cell.left += scale(6, dpi);
        cell.right -= scale(6, dpi);
        if (column == 0) {
            if (images && info.row.icon >= 0 && info.row.icon < ImageList_GetImageCount(images)) {
                const int icon_size = scale(16, dpi);
                const int x = info.rtl ? cell.right - icon_size - scale(3, dpi) : cell.left + scale(3, dpi);
                const int y = cell.top + (cell.bottom - cell.top - icon_size) / 2;
                ImageList_DrawEx(images, info.row.icon, dc, x, y, icon_size, icon_size, CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
            }
            if (info.rtl) cell.right -= scale(26, dpi);
            else cell.left += scale(26, dpi);
        }
        if (cell.right <= cell.left) continue;
        const auto text = column == 0 ? display_title(info.row, false) : column == 1 ? info.row.pid_text
            : column == 2 ? info.row.architecture : column == 3 ? info.row.state_text : info.row.path;
        const auto column_kind = static_cast<ColumnKind>(column);
        const TextDirection direction = info.rtl ? TextDirection::rtl : TextDirection::ltr;
        const UINT flags = field_text_flags(column_kind, direction,
            DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawTextW(dc, field_text(column_kind, direction, text).c_str(), -1, &cell, flags);
    }
    if (info.focused && info.list_focused && !info.disabled) {
        RECT focus = bounds;
        InflateRect(&focus, -1, -1);
        paint_focus_rect(dc, focus, info.selected ? theme.focus : blend_color(theme.focus, fill, 80));
    }
    RestoreDC(dc, saved);
}

void paint_empty_state(HDC dc, const RECT& bounds, const wchar_t* text,
    const ThemeColors& theme, HFONT font, unsigned dpi, bool rtl) {
    if (!dc) return;
    const HBRUSH brush = CreateSolidBrush(theme.panel);
    FillRect(dc, &bounds, brush);
    DeleteObject(brush);
    if (!text || !*text) return;
    const int saved = SaveDC(dc);
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme.text);
    RECT area = bounds;
    area.left += scale(16, dpi);
    area.right -= scale(16, dpi);
    area.top += scale(12, dpi);
    area.bottom -= scale(12, dpi);
    DrawTextW(dc, text, -1, &area, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX
        | (rtl ? DT_RTLREADING : 0));
    RestoreDC(dc, saved);
}

int navigate(int current, int count, NavKey key, bool shift, int page) {
    if (count <= 0) return -1;
    if (current < 0 || current >= count) current = 0;
    const int step = shift ? std::max(1, page / 2) : 1;
    const int page_step = std::max(1, page);
    switch (key) {
    case NavKey::up: return std::clamp(current - step, 0, count - 1);
    case NavKey::down: return std::clamp(current + step, 0, count - 1);
    case NavKey::home: return 0;
    case NavKey::end: return count - 1;
    case NavKey::page_up: return std::clamp(current - page_step, 0, count - 1);
    case NavKey::page_down: return std::clamp(current + page_step, 0, count - 1);
    }
    return current;
}

bool ime_composing(HWND focus) {
    if (GetKeyState(VK_PROCESSKEY) & 0x8000) return true;
    if (!focus) return false;
    const HIMC context = ImmGetContext(focus);
    if (!context) return false;
    const bool open = ImmGetOpenStatus(context) != FALSE;
    const auto composing = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(focus, context);
    return open && composing > 0;
}

bool handle_shortcut(bool ctrl, bool shift, UINT vk, HWND focus, HWND edit, HWND list,
    bool composition, int& action) {
    action = shortcut_none;
    if (composition) return false;
    if (vk == VK_F5) { action = shortcut_refresh; return true; }
    if (vk == VK_ESCAPE && focus == edit) { action = shortcut_clear_filter; return true; }
    if (!ctrl) return false;
    if (vk == 'F') { action = shortcut_focus_filter; return true; }
    if (vk == 'H' && focus != edit) { action = shortcut_exclude; return true; }
    if (vk == 'U' && focus != edit) { action = shortcut_restore; return true; }
    if (vk == 'A' && focus == list) { action = shortcut_select_all; return true; }
    (void)shift;
    return false;
}

bool set_native_item_text(HWND list, int index, int column, const std::wstring& text) {
    if (!list || index < 0) return false;
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    return SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item)) != FALSE;
}

std::wstring get_native_item_text(HWND list, int index, int column) {
    if (!list || index < 0) return {};
    wchar_t buffer[4096]{};
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = buffer;
    item.cchTextMax = static_cast<int>(std::size(buffer));
    const auto length = static_cast<int>(SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item)));
    if (length <= 0) return {};
    return std::wstring(buffer, static_cast<std::size_t>(std::min<int>(length, static_cast<int>(std::size(buffer)) - 1)));
}

void apply_selection_snapshot(HWND list, const ListModel& model, const SelectionSnapshot& snapshot) {
    if (!list) return;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED | LVIS_DROPHILITED);
    int focus = -1, top = -1;
    const auto selected = model.resolve(snapshot, focus, top);
    for (const auto index : selected) {
        ListView_SetItemState(list, static_cast<int>(index), LVIS_SELECTED, LVIS_SELECTED);
    }
    if (focus >= 0) ListView_SetItemState(list, focus, LVIS_FOCUSED, LVIS_FOCUSED);
    if (top >= 0) ListView_EnsureVisible(list, top, FALSE);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

} // namespace av::ui
