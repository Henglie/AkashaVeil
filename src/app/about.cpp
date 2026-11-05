// SPDX-License-Identifier: Apache-2.0
#include "app/about.h"
#include "app/about_resources.h"
#include "app/themed_controls.h"
#include "platform/windows/windows.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace av {
namespace {
constexpr wchar_t kAboutClass[] = L"Henglie.AkashaVeil.About";
constexpr wchar_t kContributionClass[] = L"Henglie.AkashaVeil.Contribute";
constexpr wchar_t kRepositoryUrl[] = L"https://github.com/Henglie/AkashaVeil";

int px(UINT dpi, int value) { return MulDiv(value, static_cast<int>(dpi), 96); }

HFONT make_font(UINT dpi, int dip, int weight) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return CreateFontW(-px(dpi, dip), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    metrics.lfMessageFont.lfHeight = -px(dpi, dip);
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

int language_offset(const wchar_t* language) {
    static const wchar_t* const codes[] = {L"zh", L"en", L"es", L"fr", L"ru", L"ar", L"tr", L"ja", L"ko"};
    if (language) {
        for (int index = 0; index < 9; ++index) {
            if (std::wstring(language) == codes[index]) return index;
        }
    }
    return 1;
}

std::string resource_bytes(HINSTANCE instance, int id) {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return {};
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !size) return {};
    return std::string(static_cast<const char*>(data), size);
}

std::wstring decode_text(const std::string& bytes) {
    if (bytes.size() >= 2) {
        const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
        if (raw[0] == 0xFF && raw[1] == 0xFE) {
            return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
        }
    }
    std::size_t offset = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        offset = 3;
    }
    if (offset >= bytes.size()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset,
        static_cast<int>(bytes.size() - offset), nullptr, 0);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + offset, static_cast<int>(bytes.size() - offset), text.data(), length);
    return text;
}

std::wstring resource_text(HINSTANCE instance, int id) { return decode_text(resource_bytes(instance, id)); }

std::wstring or_fallback(const std::wstring& value, const wchar_t* fallback) {
    return value.empty() ? std::wstring(fallback ? fallback : L"") : value;
}

bool copy_clipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

struct AboutState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND header_icon = nullptr;
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND author = nullptr;
    HWND repository = nullptr;
    HWND tab = nullptr;
    HWND edit = nullptr;
    HWND status = nullptr;
    HWND contribute = nullptr;
    HWND copy = nullptr;
    HWND github = nullptr;
    HWND close = nullptr;
    HINSTANCE instance = nullptr;
    AboutOptions options;
    AboutLabels labels;
    std::wstring about_text;
    std::wstring license_text;
    std::wstring third_party_text;
    std::wstring help_text;
    int tab_index = 0;
    bool done = false;
    UINT dpi = 96;
    HFONT font = nullptr;
    HFONT bold = nullptr;
    HBRUSH background = nullptr;
    HBRUSH panel = nullptr;
    HICON header_icon_handle = nullptr;
    ui::ControlTheme controls_theme{};
};

struct ContributionState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    AboutState* parent = nullptr;
    bool done = false;
    UINT dpi = 96;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
    HBRUSH panel = nullptr;
    IStream* stream = nullptr;
    Gdiplus::Bitmap* image = nullptr;
    std::wstring missing;
    std::wstring hint;
    std::wstring alt;
};

void apply_fonts(AboutState& state) {
    const HWND controls[] = {state.title, state.subtitle, state.author, state.repository, state.tab, state.edit,
        state.status, state.contribute, state.copy, state.github, state.close};
    for (const HWND control : controls) {
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    }
    if (state.title) SendMessageW(state.title, WM_SETFONT, reinterpret_cast<WPARAM>(state.bold), TRUE);
}

void layout_about(AboutState& state) {
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int margin = px(state.dpi, 18);
    const int icon_size = px(state.dpi, 56);
    const int gap = px(state.dpi, 12);
    const int button_height = px(state.dpi, 27);
    int y = margin;
    if (state.header_icon) MoveWindow(state.header_icon, margin, y, icon_size, icon_size, TRUE);
    const int text_x = margin + icon_size + px(state.dpi, 14);
    const int text_width = std::max<int>(px(state.dpi, 120), client.right - text_x - margin);
    if (state.title) MoveWindow(state.title, text_x, y, text_width, px(state.dpi, 26), TRUE);
    if (state.subtitle) MoveWindow(state.subtitle, text_x, y + px(state.dpi, 27), text_width, px(state.dpi, 20), TRUE);
    if (state.author) MoveWindow(state.author, text_x, y + px(state.dpi, 49), text_width, px(state.dpi, 20), TRUE);
    if (state.repository) MoveWindow(state.repository, text_x, y + px(state.dpi, 71), text_width, px(state.dpi, 20), TRUE);
    y += std::max(icon_size, px(state.dpi, 91)) + px(state.dpi, 14);
    const int bottom_y = client.bottom - margin - button_height;
    const int tab_height = std::max<int>(px(state.dpi, 80), bottom_y - gap - y);
    if (state.tab) {
        MoveWindow(state.tab, margin, y, client.right - 2 * margin, tab_height, TRUE);
        RECT display{0, 0, client.right - 2 * margin, tab_height};
        TabCtrl_AdjustRect(state.tab, FALSE, &display);
        MapWindowPoints(state.tab, state.hwnd, reinterpret_cast<POINT*>(&display), 2);
        if (state.edit) {
            MoveWindow(state.edit, display.left, display.top,
                std::max<int>(1, display.right - display.left),
                std::max<int>(1, display.bottom - display.top), TRUE);
        }
    }
    if (state.contribute) MoveWindow(state.contribute, margin, bottom_y, px(state.dpi, 112), button_height, TRUE);
    if (state.github) MoveWindow(state.github, margin + px(state.dpi, 124), bottom_y, px(state.dpi, 90), button_height, TRUE);
    const int close_width = px(state.dpi, 82), copy_width = px(state.dpi, 96);
    if (state.close) MoveWindow(state.close, client.right - margin - close_width, bottom_y, close_width, button_height, TRUE);
    if (state.copy) MoveWindow(state.copy, client.right - margin - close_width - gap - copy_width, bottom_y,
        copy_width, button_height, TRUE);
    if (state.status) MoveWindow(state.status, margin + px(state.dpi, 224), bottom_y + px(state.dpi, 5),
        std::max<int>(1, client.right - margin - px(state.dpi, 224) - close_width - gap - copy_width - margin),
        px(state.dpi, 20), TRUE);
}

std::wstring current_text(const AboutState& state) {
    switch (state.tab_index) {
    case 1: return or_fallback(state.license_text, state.labels.license_missing);
    case 2: return or_fallback(state.third_party_text, state.labels.third_party_missing);
    case 3: return or_fallback(state.help_text, state.labels.help_missing);
    default: return or_fallback(state.about_text, state.labels.about_missing);
    }
}

void set_tab(AboutState& state, int index) {
    state.tab_index = std::max(0, std::min(3, index));
    if (state.tab) TabCtrl_SetCurSel(state.tab, state.tab_index);
    if (state.edit) {
        const std::wstring content = current_text(state);
        SetWindowTextW(state.edit, content.c_str());
        SendMessageW(state.edit, EM_SETSEL, 0, 0);
        SendMessageW(state.edit, EM_SCROLLCARET, 0, 0);
    }
}

std::wstring copy_text(const AboutState& state) {
    auto value = [](const wchar_t* text) { return std::wstring(text ? text : L""); };
    std::wstring out = value(state.labels.window_title) + L" " + value(state.options.version) +
        L" · " + value(state.options.architecture) + L"\r\n" +
        value(state.labels.author_line) + L"\r\n" +
        value(state.labels.copyright_line) + L"\r\n" +
        value(state.labels.privacy_line) + L"\r\n" + kRepositoryUrl + L"\r\n\r\n" + current_text(state);
    return out;
}

void create_contribution(AboutState& parent);

LRESULT CALLBACK contribution_procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);

void layout_contribution(ContributionState& state) {
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int margin = px(state.dpi, 16);
    const int button_height = px(state.dpi, 27);
    const int bottom = client.bottom - margin - button_height;
    const int title_height = px(state.dpi, 28);
    const int hint_height = px(state.dpi, 40);
    const int alt_height = px(state.dpi, 22);
    const int top = margin + title_height;
    MoveWindow(GetDlgItem(state.hwnd, 1200), margin, margin, client.right - 2 * margin, title_height, TRUE);
    const int image_bottom = bottom - margin - hint_height - alt_height;
    if (const HWND image = GetDlgItem(state.hwnd, IDC_AV_CONTRIB_IMAGE)) {
        MoveWindow(image, margin, top, client.right - 2 * margin, std::max<int>(1, image_bottom - top), TRUE);
    }
    if (const HWND hint = GetDlgItem(state.hwnd, IDC_AV_CONTRIB_HINT)) {
        MoveWindow(hint, margin, image_bottom + px(state.dpi, 4), client.right - 2 * margin, hint_height, TRUE);
    }
    if (const HWND alt = GetDlgItem(state.hwnd, IDC_AV_CONTRIB_ALT)) {
        MoveWindow(alt, margin, image_bottom + px(state.dpi, 4) + hint_height, client.right - 2 * margin, alt_height, TRUE);
    }
    if (const HWND close = GetDlgItem(state.hwnd, IDC_AV_CONTRIB_CLOSE)) {
        MoveWindow(close, client.right - margin - px(state.dpi, 82), bottom, px(state.dpi, 82), button_height, TRUE);
    }
}

void draw_contribution_image(ContributionState& state, HDC dc, const RECT& area) {
    FillRect(dc, &area, state.panel);
    if (state.image && state.image->GetLastStatus() == Gdiplus::Ok) {
        const int width = area.right - area.left;
        const int height = area.bottom - area.top;
        const double fit = std::min(static_cast<double>(width) / state.image->GetWidth(),
            static_cast<double>(height) / state.image->GetHeight());
        const double scale = std::max(0.05, std::min(fit, 2.0));
        const int draw_width = std::max(1, static_cast<int>(state.image->GetWidth() * scale));
        const int draw_height = std::max(1, static_cast<int>(state.image->GetHeight() * scale));
        const int x = area.left + (width - draw_width) / 2;
        const int y = area.top + (height - draw_height) / 2;
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(state.image, x, y, draw_width, draw_height);
    } else {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        RECT text = area;
        text.left += px(state.dpi, 20);
        text.right -= px(state.dpi, 20);
        DrawTextW(dc, state.missing.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
}

void paint_contribution(ContributionState& state) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(state.hwnd, &paint);
    RECT client{};
    GetClientRect(state.hwnd, &client);
    FillRect(dc, &client, state.background);
    EndPaint(state.hwnd, &paint);
}

void run_modal(HWND hwnd, HWND owner, bool& done, bool activate) {
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    if (activate) SetForegroundWindow(hwnd);
    MSG message{};
    while (!done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        if (activate) {
            SetActiveWindow(owner);
            SetForegroundWindow(owner);
        }
    }
}

void create_contribution(AboutState& parent) {
    ContributionState state;
    state.owner = parent.hwnd;
    state.parent = &parent;
    state.dpi = parent.dpi;
    state.missing = parent.labels.contribution_missing;
    state.hint = parent.labels.contribution_hint;
    state.alt = parent.labels.contribution_alt;

    const std::string image_bytes = resource_bytes(parent.instance, IDR_AV_CONTRIBUTION_IMAGE);
    if (!image_bytes.empty()) {
        state.stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(image_bytes.data()),
            static_cast<UINT>(image_bytes.size()));
        if (state.stream) {
            state.image = Gdiplus::Bitmap::FromStream(state.stream, FALSE);
            if (!state.image || state.image->GetLastStatus() != Gdiplus::Ok) {
                delete state.image;
                state.image = nullptr;
                state.stream->Release();
                state.stream = nullptr;
            }
        }
    }

    state.background = parent.background;
    state.panel = parent.panel;
    state.font = make_font(state.dpi, 12, FW_NORMAL);
    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = contribution_procedure;
    cls.hInstance = parent.instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kContributionClass;
    cls.hIcon = load_app_icon(parent.instance, GetSystemMetrics(SM_CXICON));
    cls.hIconSm = load_app_icon(parent.instance, GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&cls);
    const int window_width = px(state.dpi, 520), window_height = px(state.dpi, 580);
    RECT owner_rect{};
    if (parent.hwnd) GetWindowRect(parent.hwnd, &owner_rect);
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - window_width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - window_height) / 2;
    const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT | (parent.options.rtl ? WS_EX_LAYOUTRTL : 0);
    state.hwnd = CreateWindowExW(ex_style, kContributionClass, parent.labels.contribution_title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, window_width, window_height,
        parent.hwnd, nullptr, parent.instance, &state);
    if (!state.hwnd) {
        if (state.image) delete state.image;
        if (state.stream) state.stream->Release();
        if (state.font) DeleteObject(state.font);
        return;
    }
    BOOL dark = parent.options.dark;
    DwmSetWindowAttribute(state.hwnd, 20, &dark, sizeof(dark));
    run_modal(state.hwnd, parent.hwnd, state.done, parent.options.activate);
    if (state.image) delete state.image;
    if (state.stream) state.stream->Release();
    if (state.font) DeleteObject(state.font);
    UnregisterClassW(kContributionClass, parent.instance);
}

LRESULT CALLBACK contribution_procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<ContributionState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<ContributionState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wp, lp);
    switch (message) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", state->parent->labels.contribution_title,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1200)), state->parent->instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_CONTRIB_IMAGE)),
            state->parent->instance, nullptr);
        CreateWindowExW(0, L"STATIC", state->hint.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_CONTRIB_HINT)),
            state->parent->instance, nullptr);
        CreateWindowExW(0, L"STATIC", state->alt.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_CONTRIB_ALT)),
            state->parent->instance, nullptr);
        CreateWindowExW(0, L"BUTTON", state->parent->labels.close,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_CONTRIB_CLOSE)), state->parent->instance, nullptr);
        for (const int id : {1200, IDC_AV_CONTRIB_HINT, IDC_AV_CONTRIB_ALT}) {
            SendMessageW(GetDlgItem(hwnd, id), WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        }
        SendMessageW(GetDlgItem(hwnd, IDC_AV_CONTRIB_CLOSE), WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        state->parent->controls_theme.apply_children(hwnd);
        layout_contribution(*state);
        SetFocus(GetDlgItem(hwnd, IDC_AV_CONTRIB_CLOSE));
        return 0;
    }
    case WM_SIZE: layout_contribution(*state); return 0;
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wp);
        const HFONT old = state->font;
        state->font = make_font(state->dpi, 12, FW_NORMAL);
        for (const int id : {1200, IDC_AV_CONTRIB_HINT, IDC_AV_CONTRIB_ALT, IDC_AV_CONTRIB_CLOSE})
            SendMessageW(GetDlgItem(hwnd, id), WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        if (old) DeleteObject(old);
        const auto* rect = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_contribution(*state);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {px(state->dpi, 430), px(state->dpi, 470)};
        return 0;
    }
    case WM_PAINT: paint_contribution(*state); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw->CtlID != IDC_AV_CONTRIB_IMAGE) break;
        draw_contribution_image(*state, draw->hDC, draw->rcItem);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        const HWND control = reinterpret_cast<HWND>(lp);
        const bool image_area = control == GetDlgItem(hwnd, IDC_AV_CONTRIB_IMAGE);
        SetTextColor(reinterpret_cast<HDC>(wp), state->parent->options.foreground);
        SetBkColor(reinterpret_cast<HDC>(wp), image_area ? state->parent->options.panel : state->parent->options.background);
        return reinterpret_cast<LRESULT>(image_area ? state->panel : state->background);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_AV_CONTRIB_CLOSE || LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        state->done = true;
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT CALLBACK about_procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AboutState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<AboutState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wp, lp);
    switch (message) {
    case WM_CREATE: {
        state->font = make_font(state->dpi, 12, FW_NORMAL);
        state->bold = make_font(state->dpi, 16, FW_SEMIBOLD);
        state->background = CreateSolidBrush(state->options.background);
        state->panel = CreateSolidBrush(state->options.panel);
        state->header_icon = CreateWindowExW(0, L"STATIC", state->labels.window_title,
            WS_CHILD | WS_VISIBLE | SS_ICON | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_HEADER_ICON)), state->instance, nullptr);
        state->header_icon_handle = load_app_icon(state->instance, px(state->dpi, 56));
        if (state->header_icon_handle && state->header_icon) {
            SendMessageW(state->header_icon, STM_SETICON, reinterpret_cast<WPARAM>(state->header_icon_handle), 0);
        }
        state->title = CreateWindowExW(0, L"STATIC", state->labels.window_title,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_TITLE)), state->instance, nullptr);
        const std::wstring subtitle = std::wstring(state->labels.version_label) + L" " + state->options.version +
            L" · " + state->options.architecture;
        state->subtitle = CreateWindowExW(0, L"STATIC", subtitle.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_SUBTITLE)), state->instance, nullptr);
        state->author = CreateWindowExW(0, L"STATIC", state->labels.author_line,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_AUTHOR)), state->instance, nullptr);
        state->repository = CreateWindowExW(0, L"STATIC", kRepositoryUrl,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_REPOSITORY)), state->instance, nullptr);
        state->tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_TAB)), state->instance, nullptr);
        const wchar_t* tab_labels[] = {state->labels.tab_about, state->labels.tab_license,
            state->labels.tab_third_party, state->labels.tab_help};
        for (int i = 0; i < 4; ++i) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tab_labels[i]);
            TabCtrl_InsertItem(state->tab, i, &item);
        }
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
            WS_VSCROLL | ES_NOHIDESEL, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_EDIT)), state->instance, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        state->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_STATUS)),
            state->instance, nullptr);
        state->contribute = CreateWindowExW(0, L"BUTTON", state->labels.contribute,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_CONTRIBUTE)), state->instance, nullptr);
        state->copy = CreateWindowExW(0, L"BUTTON", state->labels.copy_info,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_COPY)), state->instance, nullptr);
        state->github = CreateWindowExW(0, L"BUTTON", L"GitHub",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_GITHUB)), state->instance, nullptr);
        state->close = CreateWindowExW(0, L"BUTTON", state->labels.close,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AV_ABOUT_CLOSE)), state->instance, nullptr);
        apply_fonts(*state);
        state->controls_theme.palette = ui::palette_for(state->options.dark);
        state->controls_theme.palette.window_bg = state->options.background;
        state->controls_theme.palette.field_bg = state->options.panel;
        state->controls_theme.palette.text = state->options.foreground;
        state->controls_theme.rtl = state->options.rtl;
        state->controls_theme.apply_children(hwnd);
        if (state->options.dark) {
            SetWindowTheme(state->tab, L"DarkMode_Explorer", nullptr);
            SetWindowTheme(state->edit, L"DarkMode_Explorer", nullptr);
        }
        SetWindowTextW(hwnd, state->labels.window_title);
        set_tab(*state, state->tab_index);
        layout_about(*state);
        SetFocus(state->tab);
        return 0;
    }
    case WM_SIZE: layout_about(*state); return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {px(state->dpi, 560), px(state->dpi, 430)};
        return 0;
    }
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wp);
        const HFONT old_font = state->font, old_bold = state->bold;
        state->font = make_font(state->dpi, 12, FW_NORMAL);
        state->bold = make_font(state->dpi, 16, FW_SEMIBOLD);
        apply_fonts(*state);
        if (old_font) DeleteObject(old_font);
        if (old_bold) DeleteObject(old_bold);
        if (state->header_icon_handle) DestroyIcon(state->header_icon_handle);
        state->header_icon_handle = load_app_icon(state->instance, px(state->dpi, 56));
        if (state->header_icon_handle && state->header_icon) {
            SendMessageW(state->header_icon, STM_SETICON, reinterpret_cast<WPARAM>(state->header_icon_handle), 0);
        }
        const auto* rect = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        layout_about(*state);
        return 0;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lp);
        if (header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
            set_tab(*state, TabCtrl_GetCurSel(state->tab));
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDCANCEL: case IDC_AV_ABOUT_CLOSE: DestroyWindow(hwnd); return 0;
        case IDC_AV_ABOUT_CONTRIBUTE: create_contribution(*state); return 0;
        case IDC_AV_ABOUT_GITHUB: {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", kRepositoryUrl,
                nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) {
                DWORD error = result == SE_ERR_NOASSOC ? ERROR_NO_ASSOCIATION : GetLastError();
                if (!error) error = ERROR_OPEN_FAILED;
                const auto detail = error_text(error);
                MessageBoxW(hwnd, detail.c_str(), state->labels.window_title, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case IDC_AV_ABOUT_COPY:
            if (copy_clipboard(hwnd, copy_text(*state))) {
                SetWindowTextW(state->status, state->labels.copied);
                SetTimer(hwnd, 1, 2000, nullptr);
            }
            return 0;
        default: break;
        }
        break;
    case WM_TIMER:
        if (wp == 1) { KillTimer(hwnd, 1); SetWindowTextW(state->status, L""); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (wp == VK_F1) { set_tab(*state, 3); return 0; }
        break;
    case AV_ABOUT_SET_TAB: set_tab(*state, static_cast<int>(wp)); return 0;
    case AV_ABOUT_GET_TAB: return state->tab_index;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: {
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, state->options.foreground);
        const HWND control = reinterpret_cast<HWND>(lp);
        const bool editing = control == state->edit;
        SetBkColor(dc, editing ? state->options.panel : state->options.background);
        return reinterpret_cast<LRESULT>(editing ? state->panel : state->background);
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, state->background);
        const HPEN pen = CreatePen(PS_SOLID, 1, state->options.dark ? RGB(70, 70, 70) : GetSysColor(COLOR_3DSHADOW));
        const auto old = SelectObject(dc, pen);
        MoveToEx(dc, px(state->dpi, 18), client.bottom - px(state->dpi, 40), nullptr);
        LineTo(dc, client.right - px(state->dpi, 18), client.bottom - px(state->dpi, 40));
        SelectObject(dc, old);
        DeleteObject(pen);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        state->done = true;
        KillTimer(hwnd, 1);
        if (state->font) DeleteObject(state->font);
        if (state->bold) DeleteObject(state->bold);
        if (state->background) DeleteObject(state->background);
        if (state->panel) DeleteObject(state->panel);
        if (state->header_icon_handle) DestroyIcon(state->header_icon_handle);
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}

HICON load_app_icon(HINSTANCE instance, int size) {
    if (!instance) return nullptr;
    HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_AV_APP), IMAGE_ICON,
        size, size, LR_DEFAULTCOLOR));
    if (!icon) {
        icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_AV_APP), IMAGE_ICON,
            0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE));
    }
    return icon;
}

int show_about(HWND owner, const AboutOptions& options, const AboutLabels& labels, int initial_tab) {
    if (!options.instance) return 0;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);

    AboutState state;
    state.owner = owner;
    state.instance = options.instance;
    state.options = options;
    state.labels = labels;
    state.dpi = options.dpi ? options.dpi : 96;
    state.tab_index = std::max(0, std::min(3, initial_tab));
    const int language = language_offset(options.language);
    state.about_text = or_fallback(resource_text(options.instance, IDR_AV_ABOUT_TEXT_ZH + language), labels.about_missing);
    if (state.about_text.empty()) state.about_text = or_fallback(resource_text(options.instance, IDR_AV_ABOUT_TEXT_EN), labels.about_missing);
    state.license_text = or_fallback(resource_text(options.instance, IDR_AV_LICENSE), labels.license_missing);
    state.third_party_text = or_fallback(resource_text(options.instance, IDR_AV_THIRD_PARTY_ZH + language), labels.third_party_missing);
    if (state.third_party_text.empty()) state.third_party_text = or_fallback(resource_text(options.instance, IDR_AV_THIRD_PARTY_EN), labels.third_party_missing);
    state.help_text = or_fallback(resource_text(options.instance, IDR_AV_HELP_ZH + language), labels.help_missing);
    if (state.help_text.empty()) state.help_text = or_fallback(resource_text(options.instance, IDR_AV_HELP_EN), labels.help_missing);

    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = about_procedure;
    cls.hInstance = options.instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kAboutClass;
    cls.hIcon = load_app_icon(options.instance, GetSystemMetrics(SM_CXICON));
    cls.hIconSm = load_app_icon(options.instance, GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&cls);

    const int width = px(state.dpi, 720), height = px(state.dpi, 600);
    RECT owner_rect{};
    if (owner) GetWindowRect(owner, &owner_rect);
    const int x = owner ? owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2 : CW_USEDEFAULT;
    const int y = owner ? owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2 : CW_USEDEFAULT;
    const DWORD ex_style = WS_EX_CONTROLPARENT | (options.rtl ? WS_EX_LAYOUTRTL : 0);
    state.hwnd = CreateWindowExW(ex_style, kAboutClass, labels.window_title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        x, y, width, height, owner, nullptr, options.instance, &state);
    if (!state.hwnd) return 0;
    BOOL dark = options.dark;
    DwmSetWindowAttribute(state.hwnd, 20, &dark, sizeof(dark));
    run_modal(state.hwnd, owner, state.done, options.activate);
    UnregisterClassW(kAboutClass, options.instance);
    return 0;
}
}
