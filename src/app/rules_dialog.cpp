// SPDX-License-Identifier: Apache-2.0
#include "app/rtl_layout.h"
#include "app/themed_controls.h"
#include "app/rules_dialog.h"
#include "core/i18n.h"
#include "platform/windows/profile_edit.h"
#include "platform/windows/windows.h"
#include <cerrno>
#include <cstdint>
#include <cwchar>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>
#include <dwmapi.h>

namespace av {

namespace {

constexpr int dialog_id = 700;
constexpr int rule_list = 701;
constexpr int add_button = 702;
constexpr int update_button = 703;
constexpr int delete_button = 704;
constexpr int toggle_button = 705;

constexpr int path_edit = 710;
constexpr int class_edit = 711;
constexpr int title_edit = 712;
constexpr int x_edit = 713;
constexpr int y_edit = 714;
constexpr int width_edit = 715;
constexpr int height_edit = 716;
constexpr int show_edit = 717;
constexpr int mode_combo = 720;
constexpr int replacement_combo = 721;
constexpr int mark_combo = 722;
constexpr int audience_combo = 723;
constexpr int taskbar_combo = 724;
constexpr int enabled_check = 730;
constexpr int replacement_label_edit = 740;
constexpr int text_edit = 741;
constexpr int label_edit = 742;
constexpr int tag_edit = 743;
constexpr int opacity_edit = 744;
constexpr int font_edit = 745;
constexpr int spacing_edit = 746;
constexpr int foreground_edit = 747;
constexpr int background_edit = 748;
constexpr int status_text = 749;

constexpr int label_path = 750;
constexpr int label_class = 751;
constexpr int label_title = 752;
constexpr int label_mode = 753;
constexpr int label_replacement = 754;
constexpr int label_visible = 755;
constexpr int label_audience = 756;
constexpr int label_taskbar = 757;
constexpr int label_geometry = 760;
constexpr int label_x = 761;
constexpr int label_y = 762;
constexpr int label_width = 763;
constexpr int label_height = 764;
constexpr int label_show = 765;
constexpr int label_replacement_label = 766;
constexpr int label_text = 767;
constexpr int label_label = 768;
constexpr int label_tag = 769;
constexpr int label_opacity = 770;
constexpr int label_font = 771;
constexpr int label_spacing = 772;
constexpr int label_foreground = 773;
constexpr int label_background = 774;

struct DialogContext {
    const RulesDialogStrings* strings = nullptr;
    const ProfileV4* input = nullptr;
    ProfileV4* output = nullptr;
};

struct Entry {
    HWND hwnd = nullptr;
    int id = 0;
    RECT base{};
    bool fixed = false;
};

struct State {
    DialogContext* context = nullptr;
    HWND dialog = nullptr;
    ProfileV4 draft;
    std::vector<Entry> entries;
    ui::DialogPlacement placement{};
    ui::ThemePalette palette{};
    ui::ControlTheme controls_theme{};
    unsigned dpi = 96;
    int scroll_offset = 0;
    bool rtl = false;
    bool dark = false;
    HFONT dialog_font = nullptr;
    HBRUSH background = nullptr;
    HBRUSH field = nullptr;
    ~State() {
        if (dialog_font) DeleteObject(dialog_font);
        if (background) DeleteObject(background);
        if (field) DeleteObject(field);
    }
    static unsigned window_dpi(HWND window) {
        using GetWindowDpi = UINT(WINAPI*)(HWND);
        if (const auto get = reinterpret_cast<GetWindowDpi>(GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))) {
            const UINT value = get(window);
            if (value) return value;
        }
        HDC dc = GetDC(nullptr);
        const UINT value = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX) ? GetDeviceCaps(dc, LOGPIXELSX) : 96);
        ReleaseDC(nullptr, dc);
        return value;
    }
    void refresh_palette() {
        palette = ui::resolve_palette(static_cast<int>(draft.theme), system_dark(), draft.accent);
        dark = palette.dark;
        if (background) DeleteObject(background);
        if (field) DeleteObject(field);
        background = CreateSolidBrush(palette.window_bg);
        field = CreateSolidBrush(palette.field_bg);
        controls_theme.palette = palette;
        controls_theme.rtl = rtl;
        if (dialog) controls_theme.apply_dialog_frame(dialog);
        if (dialog) controls_theme.apply_children(dialog);
    }
    void create_font() {
        if (dialog_font) DeleteObject(dialog_font);
        dialog_font = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(dialog, WM_SETFONT, reinterpret_cast<WPARAM>(dialog_font), TRUE);
        for (const auto& entry : entries)
            SendMessageW(entry.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(dialog_font), TRUE);
    }
    void collect_children(HWND window) {
        entries.clear();
        dialog = window;
        EnumChildWindows(window, [](HWND control, LPARAM data) -> BOOL {
            auto* state = reinterpret_cast<State*>(data);
            const int id = GetDlgCtrlID(control);
            RECT base{};
            if (!GetWindowRect(control, &base)) return TRUE;
            MapWindowPoints(nullptr, state->dialog, reinterpret_cast<POINT*>(&base), 2);
            state->entries.push_back({control, id, base, id == IDOK || id == IDCANCEL});
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
    }
    void set_reading_styles() {
        const int technical[] = {rule_list, path_edit, class_edit, x_edit, y_edit, width_edit,
            height_edit, show_edit, foreground_edit, background_edit};
        for (const auto& entry : entries) {
            const bool is_technical = std::find(std::begin(technical), std::end(technical), entry.id)
                != std::end(technical);
            auto ex = GetWindowLongPtrW(entry.hwnd, GWL_EXSTYLE);
            ex = (rtl && !is_technical) ? (ex | WS_EX_RTLREADING)
                : (ex & ~static_cast<LONG_PTR>(WS_EX_RTLREADING));
            SetWindowLongPtrW(entry.hwnd, GWL_EXSTYLE, ex);
            wchar_t cls[32]{}; GetClassNameW(entry.hwnd, cls, 32);
            auto style = GetWindowLongPtrW(entry.hwnd, GWL_STYLE);
            if (lstrcmpW(cls, L"Static") == 0 && (style & SS_TYPEMASK) != SS_OWNERDRAW) {
                style = (style & ~static_cast<LONG_PTR>(SS_TYPEMASK)) | (rtl ? SS_RIGHT : SS_LEFT);
                SetWindowLongPtrW(entry.hwnd, GWL_STYLE, style);
            }
        }
    }
    void apply_layout(HWND window) {
        RECT client{}; GetClientRect(window, &client);
        const int client_width = std::max(1, static_cast<int>(client.right));
        const int client_height = std::max(1, static_cast<int>(client.bottom));
        scroll_offset = std::clamp(scroll_offset, 0, std::max(0, placement.max_scroll));
        for (const auto& entry : entries) {
            const int w = entry.base.right - entry.base.left;
            const int h = entry.base.bottom - entry.base.top;
            const int x = rtl ? client_width - (entry.base.left + w) : entry.base.left;
            const int y = entry.fixed ? client_height - h - static_cast<int>(ui::scale_dip(6, dpi))
                : entry.base.top - scroll_offset;
            const int bottom = entry.fixed ? client_height : placement.page_height;
            if (!entry.fixed && (y + h <= 0 || y >= bottom)) { ShowWindow(entry.hwnd, SW_HIDE); continue; }
            ShowWindow(entry.hwnd, SW_SHOW);
            SetWindowPos(entry.hwnd, entry.fixed ? HWND_TOP : nullptr, x, y, w,
                std::max(1, std::min(h, bottom - y)), SWP_NOACTIVATE | (entry.fixed ? 0 : SWP_NOZORDER));
        }
        SCROLLINFO info{sizeof(info)};
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = std::max(0, placement.virtual_height - 1);
        info.nPage = static_cast<UINT>(std::max(1, placement.page_height));
        info.nPos = scroll_offset;
        SetScrollInfo(window, SB_VERT, &info, TRUE);
        ShowScrollBar(window, SB_VERT, placement.scroll_y);
    }
    void prepare_layout(HWND window) {
        const HWND owner = GetParent(window);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(owner ? owner : window, MONITOR_DEFAULTTONEAREST), &monitor);
        const int margin = static_cast<int>(ui::scale_dip(8, dpi));
        int max_right = 0, max_bottom = 0, fixed_height = 0;
        for (const auto& entry : entries) {
            if (entry.fixed) {
                fixed_height = std::max(fixed_height, static_cast<int>(entry.base.bottom - entry.base.top));
                continue;
            }
            max_right = std::max(max_right, static_cast<int>(entry.base.right));
            max_bottom = std::max(max_bottom, static_cast<int>(entry.base.bottom));
        }
        const int client_w = std::max(1, max_right + margin);
        int client_h = std::max(1, max_bottom + margin + fixed_height);
        const DWORD style_base = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const DWORD exstyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        RECT frame{0, 0, client_w, client_h};
        AdjustWindowRectEx(&frame, style_base, FALSE, exstyle);
        const int work_left = static_cast<int>(monitor.rcWork.left), work_top = static_cast<int>(monitor.rcWork.top);
        const int work_w = std::max(1, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        const int work_h = std::max(1, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        const int border_h = (frame.bottom - frame.top) - client_h;
        const int max_client_h = std::max(fixed_height + margin, work_h - 2 * margin - border_h);
        if (client_h > max_client_h) {
            client_h = max_client_h;
            frame.left = 0; frame.top = 0; frame.right = client_w; frame.bottom = client_h;
            AdjustWindowRectEx(&frame, style_base, FALSE, exstyle);
        }
        int outer_w = frame.right - frame.left;
        const int outer_h = frame.bottom - frame.top;
        placement.x = work_left + std::max(0, (work_w - outer_w) / 2);
        placement.y = work_top + std::max(0, (work_h - outer_h) / 2);
        placement.width = outer_w; placement.height = outer_h;
        placement.virtual_height = max_bottom;
        placement.page_height = client_h - fixed_height - margin;
        placement.max_scroll = std::max(0, placement.virtual_height - placement.page_height);
        placement.scroll_y = placement.max_scroll > 0;
        if (placement.scroll_y) outer_w += GetSystemMetrics(SM_CXVSCROLL);
        placement.width = outer_w;
        scroll_offset = 0;
        DWORD style = style_base;
        style = placement.scroll_y ? (style | WS_VSCROLL) : (style & ~static_cast<DWORD>(WS_VSCROLL));
        SetWindowLongPtrW(window, GWL_STYLE, static_cast<LONG_PTR>(style));
        SetWindowPos(window, nullptr, placement.x, placement.y, outer_w, outer_h,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        set_reading_styles();
        apply_layout(window);
    }
    void ensure_visible(HWND window, HWND control) {
        if (!control || !placement.scroll_y) return;
        RECT rect{}; GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
        if (rect.top < static_cast<int>(ui::scale_dip(6, dpi)) || rect.bottom > placement.page_height) {
            scroll_offset = std::clamp(static_cast<int>(rect.bottom) - static_cast<int>(placement.page_height),
                0, std::max(0, placement.max_scroll));
            apply_layout(window);
        }
    }
    void scroll_by(HWND window, int delta) {
        const int next = std::clamp(scroll_offset + delta, 0, std::max(0, placement.max_scroll));
        if (next == scroll_offset) return;
        scroll_offset = next;
        apply_layout(window);
    }
};

std::wstring text_of(HWND dialog, int id) {
    const HWND control = GetDlgItem(dialog, id);
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(control, value.data(),
        length + 1)));
    return value;
}

void set_text(HWND dialog, int id, const wchar_t* text) {
    if (const HWND control = GetDlgItem(dialog, id))
        SetWindowTextW(control, text ? text : L"");
}

bool parse_int(HWND dialog, int id, int low, int high, int& value) {
    const auto text = text_of(dialog, id);
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const auto parsed = std::wcstoll(text.c_str(), &end, 10);
    if (errno || *end || parsed < low || parsed > high) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool parse_color(HWND dialog, int id, std::uint32_t& value) {
    auto text = text_of(dialog, id);
    if (text.size() > 2 && text[0] == L'0'
        && (text[1] == L'x' || text[1] == L'X'))
        text = text.substr(2);
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const auto parsed = std::wcstoull(text.c_str(), &end, 16);
    if (errno || *end || parsed > 0xFFFFFF) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

void add_combo_item(HWND dialog, int id, const wchar_t* label,
    std::intptr_t value) {
    const HWND combo = GetDlgItem(dialog, id);
    const auto index = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(label)));
    SendMessageW(combo, CB_SETITEMDATA, index, static_cast<LPARAM>(value));
}

void select_combo(HWND dialog, int id, std::intptr_t value) {
    const HWND combo = GetDlgItem(dialog, id);
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        if (SendMessageW(combo, CB_GETITEMDATA, i, 0) == value) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
}

std::intptr_t combo_value(HWND dialog, int id) {
    const HWND combo = GetDlgItem(dialog, id);
    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0) return 0;
    return SendMessageW(combo, CB_GETITEMDATA, index, 0);
}

std::wstring rule_line(const RuleV4& rule) {
    std::wstring line = rule.enabled ? L"[x] " : L"[ ] ";
    line += rule.path;
    if (!rule.class_name.empty()) line += L" | " + rule.class_name;
    if (!rule.title.empty()) line += L" | " + rule.title;
    return line;
}

void populate_labels(HWND dialog, const RulesDialogStrings& strings) {
    SetWindowTextW(dialog, strings.title);
    set_text(dialog, label_path, strings.path);
    set_text(dialog, label_class, strings.class_name);
    set_text(dialog, label_title, strings.window_title);
    set_text(dialog, label_mode, strings.mode);
    set_text(dialog, label_replacement, strings.replacement);
    set_text(dialog, label_visible, strings.visible);
    set_text(dialog, label_audience, strings.audience);
    set_text(dialog, label_taskbar, strings.taskbar);
    set_text(dialog, label_geometry, strings.geometry);
    set_text(dialog, label_x, strings.x);
    set_text(dialog, label_y, strings.y);
    set_text(dialog, label_width, strings.width);
    set_text(dialog, label_height, strings.height);
    set_text(dialog, label_show, strings.show);
    set_text(dialog, label_replacement_label, strings.replacement_label);
    set_text(dialog, label_text, strings.text);
    set_text(dialog, label_label, strings.label);
    set_text(dialog, label_tag, strings.tag);
    set_text(dialog, label_opacity, strings.opacity);
    set_text(dialog, label_font, strings.font_size);
    set_text(dialog, label_spacing, strings.spacing);
    set_text(dialog, label_foreground, strings.foreground);
    set_text(dialog, label_background, strings.background);
    set_text(dialog, enabled_check, strings.enabled);
    set_text(dialog, add_button, strings.add);
    set_text(dialog, update_button, strings.update);
    set_text(dialog, delete_button, strings.remove);
    set_text(dialog, toggle_button, strings.toggle);
    set_text(dialog, IDOK, strings.ok);
    set_text(dialog, IDCANCEL, strings.cancel);

    add_combo_item(dialog, mode_combo, strings.mode_none,
        static_cast<std::intptr_t>(AffinityMode::none));
    add_combo_item(dialog, mode_combo, strings.mode_blank,
        static_cast<std::intptr_t>(AffinityMode::monitor));
    add_combo_item(dialog, mode_combo, strings.mode_exclude,
        static_cast<std::intptr_t>(AffinityMode::exclude_from_capture));
    add_combo_item(dialog, replacement_combo, strings.replacement_none,
        static_cast<std::intptr_t>(ReplacementKind::none));
    add_combo_item(dialog, replacement_combo, strings.replacement_solid,
        static_cast<std::intptr_t>(ReplacementKind::solid));
    add_combo_item(dialog, replacement_combo, strings.replacement_label_kind,
        static_cast<std::intptr_t>(ReplacementKind::label));
    add_combo_item(dialog, mark_combo, strings.mark_none,
        static_cast<std::intptr_t>(MarkLayout::none));
    add_combo_item(dialog, mark_combo, strings.mark_tiled,
        static_cast<std::intptr_t>(MarkLayout::tiled));
    add_combo_item(dialog, mark_combo, strings.mark_center,
        static_cast<std::intptr_t>(MarkLayout::center));
    add_combo_item(dialog, mark_combo, strings.mark_corner,
        static_cast<std::intptr_t>(MarkLayout::corner));
    add_combo_item(dialog, audience_combo, strings.audience_both,
        static_cast<std::intptr_t>(WatermarkAudience::screen_and_capture));
    add_combo_item(dialog, audience_combo, strings.audience_capture,
        static_cast<std::intptr_t>(WatermarkAudience::capture_only));
    add_combo_item(dialog, taskbar_combo, strings.taskbar_leave,
        static_cast<std::intptr_t>(TaskbarIntent::leave_unchanged));
    add_combo_item(dialog, taskbar_combo, strings.taskbar_hide,
        static_cast<std::intptr_t>(TaskbarIntent::hide));

    SendMessageW(GetDlgItem(dialog, path_edit), EM_SETLIMITTEXT, 32768, 0);
    SendMessageW(GetDlgItem(dialog, title_edit), EM_SETLIMITTEXT, 32768, 0);
    SendMessageW(GetDlgItem(dialog, class_edit), EM_SETLIMITTEXT, 1024, 0);
    for (const int id : {replacement_label_edit, text_edit, label_edit, tag_edit})
        SendMessageW(GetDlgItem(dialog, id), EM_SETLIMITTEXT, 1024, 0);
}

void refresh_list(HWND dialog, State& state, std::size_t select) {
    const HWND list = GetDlgItem(dialog, rule_list);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& rule : state.draft.rules)
        SendMessageW(list, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(rule_line(rule).c_str()));
    if (state.draft.rules.empty()) {
        SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        return;
    }
    if (select >= state.draft.rules.size()) select = state.draft.rules.size() - 1;
    SendMessageW(list, LB_SETCURSEL, select, 0);
}

void load_rule(HWND dialog, State& state) {
    const HWND list = GetDlgItem(dialog, rule_list);
    const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<std::size_t>(index) >= state.draft.rules.size())
        return;
    const auto& rule = state.draft.rules[static_cast<std::size_t>(index)];
    SetDlgItemTextW(dialog, path_edit, rule.path.c_str());
    SetDlgItemTextW(dialog, class_edit, rule.class_name.c_str());
    SetDlgItemTextW(dialog, title_edit, rule.title.c_str());
    SetDlgItemInt(dialog, x_edit, static_cast<UINT>(rule.x), TRUE);
    SetDlgItemInt(dialog, y_edit, static_cast<UINT>(rule.y), TRUE);
    SetDlgItemInt(dialog, width_edit, static_cast<UINT>(rule.width), FALSE);
    SetDlgItemInt(dialog, height_edit, static_cast<UINT>(rule.height), FALSE);
    SetDlgItemInt(dialog, show_edit, static_cast<UINT>(rule.show), FALSE);
    SetDlgItemInt(dialog, opacity_edit, rule.request.visible.opacity, FALSE);
    SetDlgItemInt(dialog, font_edit, rule.request.visible.font_size, FALSE);
    SetDlgItemInt(dialog, spacing_edit, rule.request.visible.spacing, FALSE);
    wchar_t color[16]{};
    wsprintfW(color, L"%06X", rule.request.visible.foreground);
    SetDlgItemTextW(dialog, foreground_edit, color);
    wsprintfW(color, L"%06X", rule.request.visible.background);
    SetDlgItemTextW(dialog, background_edit, color);
    SetDlgItemTextW(dialog, replacement_label_edit,
        rule.request.replacement.label.c_str());
    SetDlgItemTextW(dialog, text_edit, rule.request.visible.text.c_str());
    SetDlgItemTextW(dialog, label_edit, rule.request.visible.label.c_str());
    SetDlgItemTextW(dialog, tag_edit, rule.request.visible.tag.c_str());
    select_combo(dialog, mode_combo,
        static_cast<std::intptr_t>(rule.request.affinity));
    select_combo(dialog, replacement_combo,
        static_cast<std::intptr_t>(rule.request.replacement.kind));
    select_combo(dialog, mark_combo,
        static_cast<std::intptr_t>(rule.request.visible.layout));
    select_combo(dialog, audience_combo,
        static_cast<std::intptr_t>(rule.request.visible.audience));
    select_combo(dialog, taskbar_combo,
        static_cast<std::intptr_t>(rule.request.taskbar));
    CheckDlgButton(dialog, enabled_check,
        rule.enabled ? BST_CHECKED : BST_UNCHECKED);
}

bool read_rule(HWND dialog, State& state, RuleV4& rule) {
    const HWND list = GetDlgItem(dialog, rule_list);
    const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (index >= 0 && static_cast<std::size_t>(index) < state.draft.rules.size())
        rule = state.draft.rules[static_cast<std::size_t>(index)];
    else {
        rule = RuleV4{};
        rule.request = state.draft.defaults;
    }
    rule.path = text_of(dialog, path_edit);
    rule.class_name = text_of(dialog, class_edit);
    rule.title = text_of(dialog, title_edit);
    if (!parse_int(dialog, x_edit, -1000000, 1000000, rule.x)
        || !parse_int(dialog, y_edit, -1000000, 1000000, rule.y)
        || !parse_int(dialog, width_edit, 1, 65535, rule.width)
        || !parse_int(dialog, height_edit, 1, 65535, rule.height)
        || !parse_int(dialog, show_edit, 1, 3, rule.show))
        return false;
    int value = 0;
    if (!parse_int(dialog, opacity_edit, 5, 100, value)) return false;
    rule.request.visible.opacity = static_cast<unsigned>(value);
    rule.request.replacement.opacity = static_cast<unsigned>(value);
    if (!parse_int(dialog, font_edit, 10, 96, value)) return false;
    rule.request.visible.font_size = static_cast<unsigned>(value);
    if (!parse_int(dialog, spacing_edit, 100, 600, value)) return false;
    rule.request.visible.spacing = static_cast<unsigned>(value);
    if (!parse_color(dialog, foreground_edit, rule.request.visible.foreground)
        || !parse_color(dialog, background_edit, rule.request.visible.background))
        return false;
    rule.request.replacement.foreground = rule.request.visible.foreground;
    rule.request.replacement.background = rule.request.visible.background;
    rule.request.affinity = static_cast<AffinityMode>(combo_value(dialog, mode_combo));
    rule.request.replacement.kind = static_cast<ReplacementKind>(
        combo_value(dialog, replacement_combo));
    rule.request.replacement.label = text_of(dialog, replacement_label_edit);
    rule.request.visible.layout = static_cast<MarkLayout>(
        combo_value(dialog, mark_combo));
    rule.request.visible.enabled =
        rule.request.visible.layout != MarkLayout::none;
    rule.request.visible.audience = static_cast<WatermarkAudience>(
        combo_value(dialog, audience_combo));
    rule.request.visible.text = text_of(dialog, text_edit);
    rule.request.visible.label = text_of(dialog, label_edit);
    rule.request.visible.tag = text_of(dialog, tag_edit);
    rule.request.taskbar = static_cast<TaskbarIntent>(
        combo_value(dialog, taskbar_combo));
    rule.enabled = IsDlgButtonChecked(dialog, enabled_check) == BST_CHECKED;
    DWORD rule_error = ERROR_SUCCESS;
    return valid_rule_v4(rule, rule_error);
}

void show_status(HWND dialog, const wchar_t* text) {
    SetDlgItemTextW(dialog, status_text, text ? text : L"");
}

INT_PTR CALLBACK rules_proc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<State*>(lp);
        state->dialog = dialog;
        ui::use_manual_dialog_dpi(dialog);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        populate_labels(dialog, *state->context->strings);
        refresh_list(dialog, *state, 0);
        load_rule(dialog, *state);
        state->rtl = ui::is_rtl_language(choose_language(state->draft.language, system_language()));
        state->refresh_palette();
        const BOOL dark_title = state->dark;
        DwmSetWindowAttribute(dialog, 20, &dark_title, sizeof(dark_title));
        state->dpi = State::window_dpi(dialog);
        state->collect_children(dialog);
        state->controls_theme.apply_children(dialog);
        state->create_font();
        state->prepare_layout(dialog);
        return TRUE;
    }
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: {
        if (!state) break;
        const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX
            || ui::is_field_control(reinterpret_cast<HWND>(lp));
        SetTextColor(reinterpret_cast<HDC>(wp), state->palette.text);
        SetBkColor(reinterpret_cast<HDC>(wp), field ? state->palette.field_bg : state->palette.window_bg);
        return reinterpret_cast<INT_PTR>(field ? state->field : state->background);
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        if (!state) break;
        state->refresh_palette();
        { const BOOL dark_title = state->dark; DwmSetWindowAttribute(dialog, 20, &dark_title, sizeof(dark_title)); }
        InvalidateRect(dialog, nullptr, TRUE);
        return TRUE;
    case WM_VSCROLL:
        if (!state) break;
        {
            SCROLLINFO info{sizeof(info)}; info.fMask = SIF_ALL;
            GetScrollInfo(dialog, SB_VERT, &info);
            int pos = info.nPos;
            const int page = static_cast<int>(info.nPage);
            const int line = static_cast<int>(ui::scale_dip(24, state->dpi));
            switch (LOWORD(wp)) {
            case SB_LINEUP: pos -= line; break;
            case SB_LINEDOWN: pos += line; break;
            case SB_PAGEUP: pos -= page; break;
            case SB_PAGEDOWN: pos += page; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = info.nTrackPos; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = state->placement.virtual_height; break;
            default: return TRUE;
            }
            state->scroll_by(dialog, pos - state->scroll_offset);
        }
        return TRUE;
    case WM_MOUSEWHEEL:
        if (!state) break;
        state->scroll_by(dialog, -GET_WHEEL_DELTA_WPARAM(wp)
            * static_cast<int>(ui::scale_dip(48, state->dpi)) / WHEEL_DELTA);
        return TRUE;
    case WM_DPICHANGED: {
        if (!state) break;
        const unsigned next = HIWORD(wp);
        if (next && next != state->dpi) {
            const unsigned old = state->dpi;
            for (auto& entry : state->entries) {
                entry.base.left = MulDiv(entry.base.left, next, old);
                entry.base.top = MulDiv(entry.base.top, next, old);
                entry.base.right = MulDiv(entry.base.right, next, old);
                entry.base.bottom = MulDiv(entry.base.bottom, next, old);
            }
            state->dpi = next;
            state->create_font();
            state->prepare_layout(dialog);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        if (!state) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (code == BN_SETFOCUS || code == EN_SETFOCUS || code == CBN_SETFOCUS)
            state->ensure_visible(dialog, reinterpret_cast<HWND>(lp));
        if (id == IDOK) {
            DWORD error = ERROR_SUCCESS;
            if (!valid_profile_v4(state->draft, error)) {
                show_status(dialog, state->context->strings->invalid);
                return TRUE;
            }
            *state->context->output = state->draft;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL || id == IDCLOSE) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (id == rule_list && code == LBN_SELCHANGE) {
            load_rule(dialog, *state);
            return TRUE;
        }
        if (id == add_button || id == update_button || id == toggle_button
            || id == delete_button) {
            const HWND list = GetDlgItem(dialog, rule_list);
            int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
            if (id == add_button) {
                RuleV4 rule;
                if (!read_rule(dialog, *state, rule)) {
                    show_status(dialog, state->context->strings->invalid);
                    return TRUE;
                }
                const auto result = add_rule(state->draft, rule);
                if (result.status == RuleEditStatus::duplicate) {
                    show_status(dialog, state->context->strings->duplicate);
                    return TRUE;
                }
                if (!result.ok()) {
                    show_status(dialog, state->context->strings->invalid);
                    return TRUE;
                }
                refresh_list(dialog, *state, result.index);
                load_rule(dialog, *state);
                show_status(dialog, L"");
                return TRUE;
            }
            if (index < 0) {
                show_status(dialog, state->context->strings->no_selection);
                return TRUE;
            }
            if (id == delete_button) {
                const auto result = remove_rule(state->draft,
                    static_cast<std::size_t>(index));
                if (!result.ok()) {
                    show_status(dialog, state->context->strings->invalid);
                    return TRUE;
                }
                refresh_list(dialog, *state, static_cast<std::size_t>(index));
                load_rule(dialog, *state);
                show_status(dialog, L"");
                return TRUE;
            }
            if (id == toggle_button) {
                const auto& rule = state->draft.rules[static_cast<std::size_t>(index)];
                const auto result = set_rule_enabled(state->draft,
                    static_cast<std::size_t>(index), !rule.enabled);
                if (!result.ok()) {
                    show_status(dialog, state->context->strings->invalid);
                    return TRUE;
                }
                refresh_list(dialog, *state, static_cast<std::size_t>(index));
                load_rule(dialog, *state);
                show_status(dialog, L"");
                return TRUE;
            }
            RuleV4 rule;
            if (!read_rule(dialog, *state, rule)) {
                show_status(dialog, state->context->strings->invalid);
                return TRUE;
            }
            const auto result = update_rule(state->draft,
                static_cast<std::size_t>(index), rule);
            if (result.status == RuleEditStatus::duplicate) {
                show_status(dialog, state->context->strings->duplicate);
                return TRUE;
            }
            if (!result.ok()) {
                show_status(dialog, state->context->strings->invalid);
                return TRUE;
            }
            refresh_list(dialog, *state, static_cast<std::size_t>(index));
            load_rule(dialog, *state);
            show_status(dialog, L"");
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

}

const RulesDialogStrings& default_rules_dialog_strings() {
    static const RulesDialogStrings strings;
    return strings;
}

bool show_rules_dialog(HWND owner, const RulesDialogStrings& strings,
    const ProfileV4& input, ProfileV4& output) {
    DialogContext context{&strings, &input, &output};
    State state;
    state.context = &context;
    state.draft = input;
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(dialog_id), owner, rules_proc,
        reinterpret_cast<LPARAM>(&state));
    return result == IDOK;
}

}
