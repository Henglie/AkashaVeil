// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <string>
#include <vector>

namespace av::ui {

struct RowIdentity {
    std::uintptr_t handle = 0;
    unsigned long pid = 0;
    std::uint64_t process_created = 0;
};
inline bool operator==(const RowIdentity& a, const RowIdentity& b) {
    return a.handle == b.handle && a.pid == b.pid && a.process_created == b.process_created;
}
inline bool operator!=(const RowIdentity& a, const RowIdentity& b) { return !(a == b); }
inline bool operator<(const RowIdentity& a, const RowIdentity& b) {
    if (a.pid != b.pid) return a.pid < b.pid;
    if (a.process_created != b.process_created) return a.process_created < b.process_created;
    return a.handle < b.handle;
}

struct ListRow {
    RowIdentity id;
    std::wstring title;
    std::wstring process_name;
    std::wstring pid_text;
    std::wstring architecture;
    std::wstring state_text;
    std::wstring path;
    bool protected_state = false;
    bool error = false;
    bool mixed = false;
    int icon = -1;
    int group = 0;
};

enum class SortColumn { title = 0, process = 1, architecture = 2, state = 3, path = 4 };

struct SortSpec {
    SortColumn column = SortColumn::title;
    bool descending = false;
};

struct SelectionSnapshot {
    std::vector<RowIdentity> selected;
    RowIdentity focus{};
    RowIdentity top{};
    bool has_focus = false;
    bool has_top = false;
};

class GenerationGate {
public:
    std::uint64_t begin();
    bool accept(std::uint64_t token) const;
    std::uint64_t current() const;
private:
    std::uint64_t current_ = 0;
};

class ListModel {
public:
    void set_source(std::vector<ListRow> rows);
    void set_process_view(bool value);
    void set_filter(std::wstring query);
    void set_sort(SortSpec spec);
    void set_mixed_text(std::wstring text);

    const std::vector<ListRow>& source() const { return source_; }
    const std::vector<ListRow>& visible() const { return visible_; }
    const SortSpec& sort_spec() const { return sort_; }

    std::size_t find_visible(const RowIdentity& id) const;
    SelectionSnapshot capture(const std::vector<std::size_t>& selected, long focus, long top) const;
    std::vector<std::size_t> resolve(const SelectionSnapshot& snapshot, int& focus_out, int& top_out) const;

private:
    void rebuild();
    std::vector<ListRow> source_;
    std::vector<ListRow> visible_;
    bool process_view_ = false;
    std::wstring filter_;
    SortSpec sort_{};
    std::wstring mixed_text_ = L"Mixed";
};

std::wstring fold_case(std::wstring value);
std::wstring display_title(const ListRow& row, bool process_view);
std::wstring sort_text(const ListRow& row, SortColumn column, bool process_view);
bool sort_less(const ListRow& a, const ListRow& b, SortSpec spec, bool process_view);
std::wstring accessible_item_text(const ListRow& row, bool process_view);
std::wstring selection_summary(std::size_t rows, std::size_t selected_rows, std::size_t target_windows);

struct ThemeColors {
    COLORREF panel = RGB(255, 255, 255);
    COLORREF text = RGB(0, 0, 0);
    COLORREF accent = RGB(166, 107, 55);
    COLORREF selected_fill = RGB(0, 120, 215);
    COLORREF selected_text = RGB(255, 255, 255);
    COLORREF focus = RGB(0, 0, 0);
    COLORREF error = RGB(190, 45, 45);
    bool dark = false;
};

struct RowPaintInfo {
    RECT bounds{};
    RECT cells[5]{};
    ListRow row;
    int row_index = 0;
    bool selected = false;
    bool focused = false;
    bool list_focused = true;
    bool disabled = false;
    bool busy = false;
    bool rtl = false;
};

void paint_row(HDC dc, const RowPaintInfo& info, const ThemeColors& theme,
    HFONT font, HIMAGELIST images, unsigned dpi);
void paint_empty_state(HDC dc, const RECT& bounds, const wchar_t* text,
    const ThemeColors& theme, HFONT font, unsigned dpi, bool rtl = false);
void paint_focus_rect(HDC dc, RECT rect, COLORREF color);
double contrast_ratio(COLORREF a, COLORREF b);
COLORREF blend_color(COLORREF a, COLORREF b, unsigned percent_of_a);

enum class NavKey { up, down, home, end, page_up, page_down };
int navigate(int current, int count, NavKey key, bool shift, int page);

enum ShortcutAction {
    shortcut_none = 0,
    shortcut_focus_filter = 1,
    shortcut_exclude = 2,
    shortcut_restore = 3,
    shortcut_select_all = 4,
    shortcut_refresh = 5,
    shortcut_clear_filter = 6
};

bool handle_shortcut(bool ctrl, bool shift, UINT vk, HWND focus, HWND edit, HWND list,
    bool ime_composing, int& action);
bool ime_composing(HWND focus);

// Native list helpers. All item text stays in the native list view; custom draw
// only changes presentation. The accessible name includes protection state.
bool set_native_item_text(HWND list, int index, int column, const std::wstring& text);
std::wstring get_native_item_text(HWND list, int index, int column);
void apply_selection_snapshot(HWND list, const ListModel& model, const SelectionSnapshot& snapshot);

} // namespace av::ui
