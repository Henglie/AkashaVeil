// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app/rtl_layout.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>

namespace av::ui {

inline void use_manual_dialog_dpi(HWND dialog) {
    using SetBehavior = BOOL(WINAPI*)(HWND, int, int);
    const auto set = reinterpret_cast<SetBehavior>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetDialogDpiChangeBehavior"));
    // DDC_DISABLE_ALL: these dialogs own their fonts, geometry and scroll area.
    if (set) set(dialog, 1, 1);
}

// Keep native input, check state, default-button and accessibility behavior.
// Only the dark client painting is replaced; no undocumented theme APIs.
struct ControlTheme {
    ThemePalette palette{};
    bool rtl = false;

    void apply(HWND control) const {
        wchar_t name[32]{};
        GetClassNameW(control, name, 32);
        const bool button = lstrcmpiW(name, L"Button") == 0;
        const bool combo = lstrcmpiW(name, L"ComboBox") == 0;
        const bool tab = lstrcmpiW(name, WC_TABCONTROLW) == 0;
        const bool field = lstrcmpiW(name, L"Edit") == 0 || lstrcmpiW(name, L"ListBox") == 0;
        SetWindowTheme(control, palette.dark ? L"" : L"Explorer", nullptr);
        if (button || combo || tab || field)
            SetWindowSubclass(control, procedure, 1, reinterpret_cast<DWORD_PTR>(this));
        InvalidateRect(control, nullptr, TRUE);
    }

    void apply_children(HWND parent) const {
        EnumChildWindows(parent, [](HWND child, LPARAM data) -> BOOL {
            reinterpret_cast<const ControlTheme*>(data)->apply(child);
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
    }

    void apply_dialog_frame(HWND dialog) const {
        SetWindowSubclass(dialog, procedure, 1, reinterpret_cast<DWORD_PTR>(this));
    }

private:
    void paint_tab(HWND hwnd, HDC dc) const {
        const int saved = SaveDC(dc);
        RECT area{}; GetClientRect(hwnd, &area);
        HBRUSH bg = CreateSolidBrush(palette.window_bg); FillRect(dc, &area, bg); DeleteObject(bg);
        SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0)));
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, palette.text);
        for (int index = 0; index < TabCtrl_GetItemCount(hwnd); ++index) {
            RECT r{}; TabCtrl_GetItemRect(hwnd, index, &r);
            const bool selected = index == TabCtrl_GetCurSel(hwnd);
            bg = CreateSolidBrush(selected ? palette.panel_bg : palette.window_bg);
            FillRect(dc, &r, bg); DeleteObject(bg);
            bg = CreateSolidBrush(selected ? palette.focus : palette.border);
            FrameRect(dc, &r, bg); DeleteObject(bg);
            wchar_t caption[256]{}; TCITEMW item{};
            item.mask = TCIF_TEXT; item.pszText = caption; item.cchTextMax = 256;
            TabCtrl_GetItem(hwnd, index, &item);
            DrawTextW(dc, caption, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | (rtl ? DT_RTLREADING : 0));
            if (selected && GetFocus() == hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                InflateRect(&r, -3, -3); DrawFocusRect(dc, &r);
            }
        }
        RestoreDC(dc, saved);
    }

    void paint_frame(HWND hwnd, HDC dc) const {
        const int saved = SaveDC(dc);
        RECT window{}, client{}; GetWindowRect(hwnd, &window); GetClientRect(hwnd, &client);
        POINT origin{}; ClientToScreen(hwnd, &origin);
        OffsetRect(&client, origin.x - window.left, origin.y - window.top);
        ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
        RECT frame{0, 0, window.right-window.left, window.bottom-window.top};
        if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION)
            IntersectClipRect(dc, 0, client.top, frame.right, frame.bottom);
        HBRUSH brush = CreateSolidBrush(palette.border); FillRect(dc, &frame, brush); DeleteObject(brush);
        for (const LONG object : {OBJID_VSCROLL, OBJID_HSCROLL}) {
            SCROLLBARINFO info{sizeof(info)};
            if (!GetScrollBarInfo(hwnd, object, &info) || (info.rgstate[0] & STATE_SYSTEM_INVISIBLE)) continue;
            RECT track = info.rcScrollBar; OffsetRect(&track, -window.left, -window.top);
            const bool vertical = object == OBJID_VSCROLL;
            brush = CreateSolidBrush(palette.window_bg); FillRect(dc, &track, brush); DeleteObject(brush);
            RECT thumb = track;
            if (vertical) { thumb.top += info.xyThumbTop; thumb.bottom = track.top + info.xyThumbBottom; InflateRect(&thumb, -2, 0); }
            else { thumb.left += info.xyThumbTop; thumb.right = track.left + info.xyThumbBottom; InflateRect(&thumb, 0, -2); }
            if (!(info.rgstate[0] & STATE_SYSTEM_UNAVAILABLE) && info.xyThumbBottom > info.xyThumbTop) {
                brush = CreateSolidBrush(palette.muted_text); FillRect(dc, &thumb, brush); DeleteObject(brush);
            }
            const HPEN pen = CreatePen(PS_SOLID, 1, palette.muted_text); const auto old = SelectObject(dc, pen);
            for (const bool last : {false, true}) {
                const int center = vertical ? (track.left+track.right)/2 : (track.top+track.bottom)/2;
                const int along = vertical ? (last ? track.bottom-info.dxyLineButton/2 : track.top+info.dxyLineButton/2)
                    : (last ? track.right-info.dxyLineButton/2 : track.left+info.dxyLineButton/2);
                const int size = std::max<int>(2, info.dxyLineButton/5), sign = last ? 1 : -1;
                if (vertical) { MoveToEx(dc, center-size, along-sign*size/2, nullptr); LineTo(dc, center, along+sign*size/2); LineTo(dc, center+size, along-sign*size/2); }
                else { MoveToEx(dc, along-sign*size/2, center-size, nullptr); LineTo(dc, along+sign*size/2, center); LineTo(dc, along-sign*size/2, center+size); }
            }
            SelectObject(dc, old); DeleteObject(pen);
        }
        RestoreDC(dc, saved);
    }

    void paint(HWND hwnd, HDC dc, bool combo, bool check) const {
        const int saved = SaveDC(dc);
        RECT area{}; GetClientRect(hwnd, &area);
        const bool enabled = IsWindowEnabled(hwnd) != FALSE;
        const auto state = combo ? 0 : SendMessageW(hwnd, BM_GETSTATE, 0, 0);
        const bool pressed = !combo && (state & BST_PUSHED);
        const COLORREF bg = check ? palette.window_bg : pressed ? palette.selected_bg : palette.field_bg;
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(dc, &area, brush); DeleteObject(brush);
        SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? palette.text : palette.muted_text);
        TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
        const int gap = std::max<int>(3, metrics.tmHeight / 3);
        RECT text = area;
        if (check) {
            const int size = std::max<int>(10, metrics.tmHeight - 2);
            RECT box{rtl ? area.right - size : area.left, (area.bottom - size) / 2,
                rtl ? area.right : area.left + size, (area.bottom + size) / 2};
            brush = CreateSolidBrush(palette.field_bg); FillRect(dc, &box, brush); DeleteObject(brush);
            brush = CreateSolidBrush(enabled ? palette.border : palette.muted_text);
            FrameRect(dc, &box, brush); DeleteObject(brush);
            if (SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                const HPEN pen = CreatePen(PS_SOLID, std::max(1, size / 8), enabled ? palette.text : palette.muted_text);
                const auto old = SelectObject(dc, pen);
                MoveToEx(dc, box.left + size / 5, box.top + size / 2, nullptr);
                LineTo(dc, box.left + size * 2 / 5, box.top + size * 3 / 4);
                LineTo(dc, box.left + size * 4 / 5, box.top + size / 4);
                SelectObject(dc, old); DeleteObject(pen);
            }
            if (rtl) text.right -= size + gap; else text.left += size + gap;
        } else {
            brush = CreateSolidBrush(GetFocus() == hwnd ? palette.focus : palette.border);
            FrameRect(dc, &area, brush); DeleteObject(brush);
            InflateRect(&text, -gap, 0);
        }
        if (combo) {
            COMBOBOXINFO info{sizeof(info)};
            if (GetComboBoxInfo(hwnd, &info)) {
                RECT arrow = info.rcButton;
                brush = CreateSolidBrush(palette.panel_bg); FillRect(dc, &arrow, brush); DeleteObject(brush);
                const int x = (arrow.left + arrow.right) / 2, y = (arrow.top + arrow.bottom) / 2;
                const int size = std::max<int>(3, metrics.tmHeight / 5);
                const HPEN pen = CreatePen(PS_SOLID, std::max(1, size / 3), enabled ? palette.text : palette.muted_text);
                const auto old = SelectObject(dc, pen);
                MoveToEx(dc, x - size, y - size / 2, nullptr); LineTo(dc, x, y + size / 2); LineTo(dc, x + size, y - size / 2);
                SelectObject(dc, old); DeleteObject(pen);
                if (arrow.left < area.right / 2) text.left = arrow.right + gap;
                else text.right = arrow.left - gap;
            }
        }
        const int length = GetWindowTextLengthW(hwnd);
        std::wstring caption(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(hwnd, caption.data(), length + 1);
        UINT flags = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | (rtl ? DT_RTLREADING : 0);
        flags |= (check || combo) ? (rtl ? DT_RIGHT : DT_LEFT) : DT_CENTER;
        if (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
        if (pressed) OffsetRect(&text, 1, 1);
        DrawTextW(dc, caption.c_str(), -1, &text, flags);
        if (GetFocus() == hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RECT focus = text; InflateRect(&focus, -2, -2); DrawFocusRect(dc, &focus);
        }
        RestoreDC(dc, saved);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR data) {
        const auto* theme = reinterpret_cast<const ControlTheme*>(data);
        wchar_t name[32]{}; GetClassNameW(hwnd, name, 32);
        const bool combo = lstrcmpiW(name, L"ComboBox") == 0;
        const bool tab = lstrcmpiW(name, WC_TABCONTROLW) == 0;
        const bool button = lstrcmpiW(name, L"Button") == 0;
        const bool field = lstrcmpiW(name, L"Edit") == 0 || lstrcmpiW(name, L"ListBox") == 0
            || lstrcmpiW(name, L"#32770") == 0;
        const auto type = GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK;
        const bool check = !combo && (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX);
        const bool supported = tab || combo || (button && (check || type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON));
        if (theme->palette.dark && supported) {
            if (message == WM_PAINT || message == WM_PRINTCLIENT) {
                PAINTSTRUCT ps{};
                const HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wp);
                if (tab) theme->paint_tab(hwnd, dc); else theme->paint(hwnd, dc, combo, check);
                if (message == WM_PAINT) EndPaint(hwnd, &ps);
                return 0;
            }
            if (message == WM_ERASEBKGND) return 1;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, procedure, id);
        const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
        if (theme->palette.dark && field && IsWindow(hwnd)) {
            if (message == WM_PRINT && (lp & PRF_NONCLIENT)) theme->paint_frame(hwnd, reinterpret_cast<HDC>(wp));
            else if (message == WM_NCPAINT || message == WM_PAINT || message == WM_NCMOUSEMOVE
                || message == WM_NCLBUTTONDOWN || message == WM_VSCROLL || message == WM_HSCROLL) {
                HDC dc = GetWindowDC(hwnd); theme->paint_frame(hwnd, dc); ReleaseDC(hwnd, dc);
            }
        }
        switch (message) {
        case BM_SETCHECK: case BM_SETSTATE: case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_UPDATEUISTATE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_KEYDOWN: case WM_KEYUP:
        case CB_SETCURSEL: case CB_SHOWDROPDOWN:
        case TCM_SETCURSEL: case WM_MOUSEMOVE:
            InvalidateRect(hwnd, nullptr, FALSE); break;
        }
        return result;
    }
};

inline bool is_field_control(HWND hwnd) {
    wchar_t name[32]{}; GetClassNameW(hwnd, name, 32);
    return lstrcmpiW(name, L"Edit") == 0 || lstrcmpiW(name, L"ComboBox") == 0
        || lstrcmpiW(name, L"ListBox") == 0;
}

} // namespace av::ui
