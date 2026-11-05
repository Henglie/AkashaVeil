// SPDX-License-Identifier: Apache-2.0
#include "app/watermark_files_dialog.h"

#include "app/rtl_layout.h"
#include "app/themed_controls.h"
#include "app/watermark_files.h"
#include "core/i18n.h"
#include "platform/windows/image_codec.h"
#include "platform/windows/windows.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

namespace av {

namespace {

constexpr UINT job_done = WM_APP + 41;
constexpr wchar_t window_class[] = L"AkashaVeil.WatermarkFiles";

enum ControlId {
    id_input_label = 1000,
    id_output_label,
    id_embed_algorithm_label,
    id_extract_algorithm_label,
    id_delta_label,
    id_quality_label,
    id_payload_label,
    id_key_label,
    id_geometry_label,
    id_input,
    id_browse_input,
    id_output,
    id_browse_output,
    id_embed_algorithm,
    id_extract_algorithm,
    id_payload,
    id_payload_count,
    id_key,
    id_delta,
    id_quality,
    id_geometry,
    id_search_tile,
    id_search_phase,
    id_preview,
    id_embed,
    id_extract,
    id_copy,
    id_export,
    id_cancel,
    id_close,
    id_status,
    id_help
};

const wchar_t* translate(const std::wstring& language, const wchar_t* key)
{
    return text_key(key, language);
}

std::wstring localized_detail(const std::wstring& detail, const std::wstring& language)
{
    struct DetailKey { const wchar_t* text; const wchar_t* key; };
    static constexpr DetailKey details[] = {
        {L"input and output paths are required", L"watermark.detail.embed_paths_required"},
        {L"embedding needs an explicit DCT or DWT algorithm", L"watermark.detail.embed_algorithm_required"},
        {L"payload exceeds 32 UTF-8 bytes", L"watermark.detail.payload_too_large"},
        {L"key must be 1-128 bytes", L"watermark.detail.key_length"},
        {L"DWT delta is frozen at 24", L"watermark.detail.dwt_delta_frozen"},
        {L"DCT delta must be 1-255", L"watermark.detail.dct_delta_range"},
        {L"output extension must be .png, .jpg, .jpeg or .bmp", L"watermark.detail.output_extension"},
        {L"JPEG quality must be 1-100", L"watermark.detail.jpeg_quality"},
        {L"the source file cannot be overwritten", L"watermark.detail.source_overwrite"},
        {L"output path is a directory", L"watermark.detail.output_is_directory"},
        {L"the output file exists; explicit confirmation is required", L"watermark.detail.output_exists"},
        {L"image exceeds the 32 Mi pixel budget", L"watermark.detail.pixel_budget"},
        {L"Image exceeds the 32 Mi pixel budget.", L"image.detail.pixel_budget"},
        {L"input path is required", L"watermark.detail.extract_path_required"},
        {L"unknown extraction algorithm", L"watermark.detail.unknown_algorithm"},
        {L"invalid delta", L"watermark.detail.invalid_delta"},
        {L"invalid extraction geometry", L"watermark.detail.invalid_geometry"},
        {L"export path is required", L"watermark.detail.export_path_required"},
        {L"cannot create a temporary export name", L"watermark.detail.export_temp_name"},
        {L"cannot create the export file", L"watermark.detail.export_create"},
        {L"the export could not be written", L"watermark.detail.export_write"},
        {L"the export could not replace the target", L"watermark.detail.export_replace"},
        {L"export cancelled", L"watermark.detail.export_cancelled"},
        {L"out of memory", L"watermark.detail.out_of_memory"},
        {L"unexpected processing failure", L"watermark.detail.unexpected_failure"},
        {L"cannot start the watermark worker", L"watermark.detail.worker_start"},
        {L"empty input path", L"image.detail.empty_input"},
        {L"GDI+ could not be started", L"image.detail.gdiplus_start"},
        {L"cannot read input file: ", L"image.detail.read_failed"},
        {L"the image container is damaged or cannot be decoded", L"image.detail.container_damaged"},
        {L"image has zero width or height", L"image.detail.zero_size"},
        {L"image stride overflows", L"image.detail.stride_overflow"},
        {L"cannot allocate the decoded image", L"image.detail.alloc_decode"},
        {L"cannot convert the image to BGRA8", L"image.detail.convert_bgra"},
        {L"cannot lock the decoded pixels", L"image.detail.lock_pixels"},
        {L"empty output path or invalid image", L"image.detail.empty_output"},
        {L"cannot allocate the output image", L"image.detail.alloc_output"},
        {L"the system image encoder is missing", L"image.detail.encoder_missing"},
        {L"cannot create a temporary output name", L"image.detail.temp_name"},
        {L"the image could not be written (check folder permissions)", L"image.detail.write_failed"},
        {L"the output could not replace the target: ", L"image.detail.replace_failed"},
    };
    for (const auto& entry : details) {
        const std::size_t length = std::wcslen(entry.text);
        if (detail.compare(0, length, entry.text) == 0) {
            return std::wstring(text_key(entry.key, language)) + detail.substr(length);
        }
    }
    return detail;
}

std::wstring get_text(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, value.data(), length + 1);
    value.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return value;
}

std::vector<std::uint8_t> to_utf8(const std::wstring& value)
{
    std::vector<std::uint8_t> out;
    if (value.empty()) {
        return out;
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return out;
    }
    out.resize(static_cast<std::size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        reinterpret_cast<char*>(out.data()), size, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(const std::vector<std::uint8_t>& value)
{
    if (value.empty()) {
        return std::wstring();
    }
    const std::string bytes(reinterpret_cast<const char*>(value.data()), value.size());
    if (!watermark_files::WatermarkFilesService::valid_utf8(bytes)) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), size);
    return out;
}

std::wstring hex_payload(const std::vector<std::uint8_t>& value, std::size_t limit)
{
    static const wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring out;
    const std::size_t count = std::min(value.size(), limit);
    for (std::size_t index = 0; index < count; ++index) {
        out.push_back(digits[value[index] >> 4]);
        out.push_back(digits[value[index] & 0x0F]);
    }
    if (value.size() > count) {
        out += L"...";
    }
    return out;
}

class Win32FileDialogs final : public watermark_files::FileDialogs {
public:
    Win32FileDialogs(HWND owner, std::wstring language)
        : owner_(owner)
        , language_(std::move(language))
    {
    }

    bool open_image(std::wstring& path) override
    {
        wchar_t buffer[32768] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner_;
        dialog.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files (*.*)\0*.*\0\0";
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) {
            return false;
        }
        path = buffer;
        return true;
    }

    bool save_output(std::wstring& path, const std::wstring& suggested_name) override
    {
        wchar_t buffer[32768] = {};
        wcsncpy_s(buffer, std::size(buffer), suggested_name.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner_;
        dialog.lpstrFilter = L"PNG (*.png)\0*.png\0JPEG (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0Bitmap (*.bmp)\0*.bmp\0\0";
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
        dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) {
            return false;
        }
        path = buffer;
        return true;
    }

    bool save_payload(std::wstring& path, const std::wstring& suggested_name) override
    {
        wchar_t buffer[32768] = {};
        wcsncpy_s(buffer, std::size(buffer), suggested_name.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = owner_;
        dialog.lpstrFilter = L"Payload (*.bin;*.txt)\0*.bin;*.txt\0All files (*.*)\0*.*\0\0";
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
        dialog.lpstrDefExt = L"bin";
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) {
            return false;
        }
        path = buffer;
        return true;
    }

    bool confirm_overwrite(const std::wstring& path) override
    {
        const std::wstring message = std::wstring(translate(language_, L"watermark.files.overwrite"))
            + path;
        return MessageBoxW(owner_, message.c_str(),
                   translate(language_, L"watermark.files.title"),
                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
            == IDYES;
    }

private:
    HWND owner_ = nullptr;
    std::wstring language_;
};

UINT window_dpi(HWND window)
{
    using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
    const auto procedure = reinterpret_cast<GetDpiForWindowProc>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (procedure != nullptr) {
        const UINT dpi = procedure(window);
        if (dpi != 0) {
            return dpi;
        }
    }
    HDC dc = GetDC(window);
    const UINT dpi = dc != nullptr ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc != nullptr) {
        ReleaseDC(window, dc);
    }
    return dpi == 0 ? 96 : dpi;
}

class Dialog {
public:
    Dialog(HWND owner, std::wstring language, int theme_mode, unsigned long accent)
        : owner_(owner)
        , language_(std::move(language))
        , codec_()
        , service_(codec_)
        , dialogs_(owner, language_)
        , controller_(service_, dialogs_)
        , runner_(service_)
    {
        palette_ = ui::resolve_palette(theme_mode, system_dark(), static_cast<COLORREF>(accent));
        dark_ = palette_.dark;
    }

    ~Dialog()
    {
        runner_.cancel();
        runner_.wait();
        if (owner_ != nullptr && IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
        }
        if (background_ != nullptr) DeleteObject(background_);
        if (field_ != nullptr) DeleteObject(field_);
        if (font_ != nullptr) DeleteObject(font_);
    }

    bool create()
    {
        WNDCLASSEXW window{};
        window.cbSize = sizeof(window);
        window.style = CS_HREDRAW | CS_VREDRAW;
        window.lpfnWndProc = &Dialog::procedure;
        window.hInstance = GetModuleHandleW(nullptr);
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        window.lpszClassName = window_class;
        RegisterClassExW(&window);

        dpi_ = owner_ != nullptr ? window_dpi(owner_) : 96;
        rtl_ = ui::is_rtl_language(language_);
        controls_theme_.palette = palette_;
        controls_theme_.rtl = rtl_;
        background_ = CreateSolidBrush(palette_.window_bg);
        field_ = CreateSolidBrush(palette_.field_bg);
        RECT bounds{0, 0, px(760), px(640)};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE, 0);
        RECT owner_rect{};
        if (owner_ != nullptr) {
            GetWindowRect(owner_, &owner_rect);
        }
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
        hwnd_ = CreateWindowExW(0, window_class,
            translate(language_, L"watermark.files.title"),
            (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN, x, y, width, height,
            owner_, nullptr, window.hInstance, this);
        if (hwnd_ == nullptr) {
            return false;
        }
        if (owner_ != nullptr) {
            EnableWindow(owner_, FALSE);
        }
        const BOOL dark_title = dark_ ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20, &dark_title, sizeof(dark_title));
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        return true;
    }

    int run()
    {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return static_cast<int>(message.wParam);
    }

private:
    int px(int value) const
    {
        return MulDiv(value, static_cast<int>(dpi_), 96);
    }

    std::wstring text(const wchar_t* key) const
    {
        return translate(language_, key);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        Dialog* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            dialog = static_cast<Dialog*>(create->lpCreateParams);
            dialog->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        }
        if (dialog != nullptr) {
            return dialog->handle(message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND control(int id) const
    {
        return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr;
    }

    HWND make(const wchar_t* type, const wchar_t* caption, DWORD style, int id,
        DWORD extended = 0)
    {
        return CreateWindowExW(extended, type, caption, style | WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
    }

    void set_font(HWND window)
    {
        if (window != nullptr) {
            SendMessageW(window, WM_SETFONT,
                reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }

    void create_font()
    {
        const HFONT old = font_;
        font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        set_font(hwnd_);
        EnumChildWindows(hwnd_, [](HWND child, LPARAM data) -> BOOL {
            reinterpret_cast<Dialog*>(data)->set_font(child); return TRUE;
        }, reinterpret_cast<LPARAM>(this));
        if (old) DeleteObject(old);
    }

    void create_controls()
    {
        create_font();
        input_label_ = make(L"STATIC", text(L"watermark.files.input").c_str(), SS_LEFT, id_input_label);
        output_label_ = make(L"STATIC", text(L"watermark.files.output").c_str(), SS_LEFT, id_output_label);
        embed_algorithm_label_ = make(L"STATIC", text(L"watermark.files.embed_algorithm").c_str(), SS_LEFT, id_embed_algorithm_label);
        extract_algorithm_label_ = make(L"STATIC", text(L"watermark.files.extract_algorithm").c_str(), SS_LEFT, id_extract_algorithm_label);
        delta_label_ = make(L"STATIC", text(L"watermark.files.delta").c_str(), SS_LEFT, id_delta_label);
        quality_label_ = make(L"STATIC", text(L"watermark.files.quality").c_str(), SS_LEFT, id_quality_label);
        payload_label_ = make(L"STATIC", text(L"watermark.files.payload").c_str(), SS_LEFT, id_payload_label);
        key_label_ = make(L"STATIC", text(L"watermark.files.key").c_str(), SS_LEFT, id_key_label);
        geometry_label_ = make(L"STATIC", text(L"watermark.files.geometry").c_str(), SS_LEFT, id_geometry_label);
        input_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, id_input, WS_EX_CLIENTEDGE);
        output_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, id_output, WS_EX_CLIENTEDGE);
        browse_input_ = make(L"BUTTON", text(L"watermark.files.browse").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_browse_input);
        browse_output_ = make(L"BUTTON", text(L"watermark.files.browse").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_browse_output);
        embed_algorithm_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id_embed_algorithm);
        extract_algorithm_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id_extract_algorithm);
        payload_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id_payload, WS_EX_CLIENTEDGE);
        payload_count_ = make(L"STATIC", L"", SS_LEFT, id_payload_count);
        key_ = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, id_key, WS_EX_CLIENTEDGE);
        delta_ = make(L"EDIT", L"24", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, id_delta, WS_EX_CLIENTEDGE);
        quality_ = make(L"EDIT", L"90", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, id_quality, WS_EX_CLIENTEDGE);
        geometry_ = make(L"EDIT", L"1,0,0,0,0,0,0", WS_BORDER | ES_AUTOHSCROLL, id_geometry, WS_EX_CLIENTEDGE);
        search_tile_ = make(L"BUTTON", text(L"watermark.files.search_tile").c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, id_search_tile);
        search_phase_ = make(L"BUTTON", text(L"watermark.files.search_phase").c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, id_search_phase);
        preview_ = make(L"STATIC", L"", SS_OWNERDRAW | WS_BORDER, id_preview);
        embed_ = make(L"BUTTON", text(L"watermark.files.embed").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_embed);
        extract_ = make(L"BUTTON", text(L"watermark.files.extract").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_extract);
        copy_ = make(L"BUTTON", text(L"watermark.files.copy").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_copy);
        export_ = make(L"BUTTON", text(L"watermark.files.export").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_export);
        cancel_ = make(L"BUTTON", text(L"watermark.files.cancel").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_cancel);
        close_ = make(L"BUTTON", text(L"watermark.files.close").c_str(), BS_PUSHBUTTON | WS_TABSTOP, id_close);
        status_ = make(L"STATIC", text(L"watermark.files.ready").c_str(), SS_LEFT | SS_NOPREFIX | WS_BORDER, id_status);
        help_ = make(L"STATIC", text(L"watermark.files.help").c_str(), SS_LEFT | SS_NOPREFIX, id_help);

        SendMessageW(embed_algorithm_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DCT"));
        SendMessageW(embed_algorithm_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DWT"));
        SendMessageW(embed_algorithm_, CB_SETITEMDATA, 0, static_cast<LPARAM>(watermark_files::Algorithm::dct));
        SendMessageW(embed_algorithm_, CB_SETITEMDATA, 1, static_cast<LPARAM>(watermark_files::Algorithm::dwt));
        SendMessageW(embed_algorithm_, CB_SETCURSEL, 0, 0);

        SendMessageW(extract_algorithm_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text(L"watermark.algorithm.auto").c_str()));
        SendMessageW(extract_algorithm_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DCT"));
        SendMessageW(extract_algorithm_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DWT"));
        SendMessageW(extract_algorithm_, CB_SETITEMDATA, 0, static_cast<LPARAM>(watermark_files::Algorithm::auto_detect));
        SendMessageW(extract_algorithm_, CB_SETITEMDATA, 1, static_cast<LPARAM>(watermark_files::Algorithm::dct));
        SendMessageW(extract_algorithm_, CB_SETITEMDATA, 2, static_cast<LPARAM>(watermark_files::Algorithm::dwt));
        SendMessageW(extract_algorithm_, CB_SETCURSEL, 0, 0);
        SendMessageW(search_tile_, BM_SETCHECK, BST_CHECKED, 0);

        const HWND all[] = {input_label_, output_label_, embed_algorithm_label_,
            extract_algorithm_label_, delta_label_, quality_label_, payload_label_, key_label_,
            geometry_label_, input_, output_, browse_input_, browse_output_, embed_algorithm_,
            extract_algorithm_, payload_, payload_count_, key_, delta_, quality_, geometry_,
            search_tile_, search_phase_, preview_, embed_, extract_, copy_, export_, cancel_,
            close_, status_, help_};
        for (HWND window : all) {
            set_font(window);
            controls_theme_.apply(window);
        }
        const HWND technical[] = {input_, output_, payload_, key_, delta_, quality_, geometry_};
        for (HWND window : all) {
            const bool is_technical = std::find(std::begin(technical), std::end(technical), window)
                != std::end(technical);
            auto ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
            ex = (rtl_ && !is_technical) ? (ex | WS_EX_RTLREADING)
                : (ex & ~static_cast<LONG_PTR>(WS_EX_RTLREADING));
            SetWindowLongPtrW(window, GWL_EXSTYLE, ex);
            wchar_t cls[32]{}; GetClassNameW(window, cls, 32);
            auto style = GetWindowLongPtrW(window, GWL_STYLE);
            if (lstrcmpW(cls, L"Static") == 0 && (style & SS_TYPEMASK) != SS_OWNERDRAW) {
                style = (style & ~static_cast<LONG_PTR>(SS_TYPEMASK)) | (rtl_ ? SS_RIGHT : SS_LEFT);
                SetWindowLongPtrW(window, GWL_STYLE, style);
            }
        }
        layout();
        update_payload_count();
        set_busy(false);
    }

    void layout()
    {
        if (hwnd_ == nullptr) {
            return;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int margin = px(12);
        const int gap = px(6);
        const int button = px(86);
        const int width = client.right - client.left;
        int label = px(205);
        HDC dc = GetDC(hwnd_);
        const auto old_font = font_ ? SelectObject(dc, font_) : nullptr;
        for (HWND caption : {input_label_, output_label_, payload_label_, key_label_}) {
            const auto value = get_text(caption);
            SIZE size{};
            if (GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &size))
                label = std::max(label, static_cast<int>(size.cx) + px(12));
        }
        if (old_font) SelectObject(dc, old_font);
        ReleaseDC(hwnd_, dc);
        label = std::min(label, std::max(px(205), width - margin * 2 - button - gap * 2 - px(160)));
        const int field = std::max(px(80), width - margin * 2 - label - button - gap * 2);
        int y = margin;
        const int row = px(26);
        auto place = [&](HWND window, int x, int top, int w, int h) {
            if (window != nullptr) {
                MoveWindow(window, rtl_ ? width - x - w : x, top, w, h, TRUE);
            }
        };

        place(control(id_input_label), margin, y + px(5), label, row);
        place(control(id_input), margin + label, y, field, row);
        place(control(id_browse_input), margin + label + field + gap, y, button, row);
        y += row + gap;
        place(control(id_output_label), margin, y + px(5), label, row);
        place(control(id_output), margin + label, y, field, row);
        place(control(id_browse_output), margin + label + field + gap, y, button, row);
        y += row + gap;

        const int small_label = px(205);
        const int combo = px(96);
        int x = margin;
        place(control(id_embed_algorithm_label), x, y + px(5), small_label, row);
        x += small_label;
        place(control(id_embed_algorithm), x, y, combo, px(160));
        x += combo + px(10);
        place(control(id_extract_algorithm_label), x, y + px(5), small_label, row);
        x += small_label;
        place(control(id_extract_algorithm), x, y, combo, px(160));
        y += row + gap;
        x = margin;
        place(control(id_delta_label), x, y + px(5), px(90), row);
        x += px(90);
        place(control(id_delta), x, y, px(60), row);
        x += px(60) + px(10);
        place(control(id_quality_label), x, y + px(5), px(110), row);
        x += px(110);
        place(control(id_quality), x, y, px(55), row);
        y += row + gap;

        place(control(id_payload_label), margin, y + px(5), label, row);
        place(control(id_payload), margin + label, y, field, row);
        place(control(id_payload_count), margin + label + field + gap, y, button, row);
        y += row + gap;
        place(control(id_key_label), margin, y + px(5), label, row);
        place(control(id_key), margin + label, y, field, row);
        y += row + gap;
        place(control(id_geometry_label), margin, y + px(5), width - margin * 2, row);
        y += row + gap;
        const int geometry_width = std::max(px(120), width - margin * 2 - px(340));
        place(control(id_geometry), margin, y, geometry_width, row);
        place(control(id_search_tile), margin + geometry_width + gap, y, px(160), row);
        place(control(id_search_phase), margin + geometry_width + gap + px(166), y, px(160), row);
        y += row + gap;

        const int preview_height = std::max(px(120), static_cast<int>(client.bottom) - y - px(230));
        place(control(id_preview), margin, y, width - margin * 2, preview_height);
        y += preview_height + gap;

        const int action_width = (width - margin * 2 - gap * 2) / 3;
        int index = 0;
        for (const int id : {id_embed, id_extract, id_cancel, id_copy, id_export, id_close}) {
            place(control(id), margin + (index % 3) * (action_width + gap),
                y + (index / 3) * (row + gap), action_width, row);
            ++index;
        }
        y += 2 * (row + gap);
        place(control(id_status), margin, y, width - margin * 2, px(52));
        y += px(52) + gap;
        place(control(id_help), margin, y, width - margin * 2, std::max(px(36), static_cast<int>(client.bottom) - y - margin));
    }

    void set_busy(bool busy)
    {
        for (const int id : {id_embed, id_extract, id_browse_input, id_browse_output}) {
            EnableWindow(control(id), busy ? FALSE : TRUE);
        }
        EnableWindow(control(id_cancel), busy ? TRUE : FALSE);
        EnableWindow(control(id_copy), (!busy && last_extract_success_) ? TRUE : FALSE);
        EnableWindow(control(id_export), (!busy && last_extract_success_) ? TRUE : FALSE);
        busy_ = busy;
    }

    void set_status(const std::wstring& value)
    {
        SetWindowTextW(control(id_status), value.c_str());
    }

    void update_payload_count()
    {
        const auto bytes = to_utf8(get_text(control(id_payload)));
        const std::wstring label = std::to_wstring(bytes.size()) + L" / 32";
        SetWindowTextW(control(id_payload_count), label.c_str());
    }

    void open_input()
    {
        if (busy_) {
            return;
        }
        if (!controller_.open_input()) {
            return;
        }
        SetWindowTextW(control(id_input), controller_.input().c_str());
        if (!output_custom_) {
            const auto suggested = watermark_files::WatermarkFilesService::suggested_output_path(
                controller_.input(), watermark_files::Algorithm::dct);
            controller_.set_output(suggested);
            SetWindowTextW(control(id_output), suggested.c_str());
        }
        load_preview(controller_.input());
        set_status(text(L"watermark.files.ready"));
    }

    void choose_output()
    {
        if (busy_ || !controller_.choose_output()) {
            return;
        }
        output_custom_ = true;
        SetWindowTextW(control(id_output), controller_.output().c_str());
    }

    void load_preview(const std::wstring& path)
    {
        preview_image_ = watermark_files::LoadedImage{};
        const auto loaded = codec_.load(path, preview_image_);
        if (!loaded.ok()) {
            preview_image_ = watermark_files::LoadedImage{};
            set_status(text(L"watermark.status.corrupted") + L": " + localized_detail(loaded.detail, language_));
        } else if (static_cast<std::uint64_t>(preview_image_.width) * preview_image_.height
            > 8ull * 1024ull * 1024ull) {
            preview_image_ = watermark_files::LoadedImage{};
            set_status(text(L"watermark.files.preview_too_large"));
        }
        InvalidateRect(control(id_preview), nullptr, TRUE);
    }

    void draw_preview(DRAWITEMSTRUCT* item)
    {
        HDC dc = item->hDC;
        RECT rect = item->rcItem;
        FillRect(dc, &rect, field_);
        if (preview_image_.valid()) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(preview_image_.width);
            info.bmiHeader.biHeight = -static_cast<LONG>(preview_image_.height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            const double scale = std::min(static_cast<double>(rect.right - rect.left) / preview_image_.width,
                static_cast<double>(rect.bottom - rect.top) / preview_image_.height);
            const int width = std::max(1, static_cast<int>(preview_image_.width * scale));
            const int height = std::max(1, static_cast<int>(preview_image_.height * scale));
            StretchDIBits(dc, rect.left + (rect.right - rect.left - width) / 2,
                rect.top + (rect.bottom - rect.top - height) / 2, width,
                height, 0, 0, static_cast<int>(preview_image_.width),
                static_cast<int>(preview_image_.height), preview_image_.pixels.data(), &info,
                DIB_RGB_COLORS, SRCCOPY);
        } else {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, palette_.field_text);
            const auto old = SelectObject(dc, font_);
            DrawTextW(dc, text(L"watermark.files.preview").c_str(), -1, &rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
    }

    watermark_files::Algorithm selected_algorithm(HWND combo, bool extraction) const
    {
        const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        const LRESULT value = index >= 0
            ? SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0) : 0;
        if (extraction && value == 0) {
            return watermark_files::Algorithm::auto_detect;
        }
        return value == static_cast<LRESULT>(watermark_files::Algorithm::dwt)
            ? watermark_files::Algorithm::dwt : watermark_files::Algorithm::dct;
    }

    bool parse_number(HWND window, double minimum, double maximum, double& value) const
    {
        const std::wstring text = get_text(window);
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
            return false;
        }
        value = parsed;
        return true;
    }

    bool parse_geometry(watermark_files::ExtractOptions& options) const
    {
        const std::wstring text = get_text(control(id_geometry));
        wchar_t* cursor = const_cast<wchar_t*>(text.c_str());
        double values[7] = {};
        for (int index = 0; index < 7; ++index) {
            wchar_t* end = nullptr;
            values[index] = std::wcstod(cursor, &end);
            if (end == cursor || !std::isfinite(values[index])) {
                return false;
            }
            cursor = end;
            if (index < 6) {
                while (*cursor == L' ' || *cursor == L'\t') {
                    ++cursor;
                }
                if (*cursor != L',') {
                    return false;
                }
                ++cursor;
            }
        }
        if (values[0] <= 0.0) {
            return false;
        }
        options.scale = values[0];
        options.original_width = static_cast<std::uint32_t>(std::max(0.0, values[1]));
        options.original_height = static_cast<std::uint32_t>(std::max(0.0, values[2]));
        options.phase_x = static_cast<int>(values[3]);
        options.phase_y = static_cast<int>(values[4]);
        options.tile_x = static_cast<int>(values[5]);
        options.tile_y = static_cast<int>(values[6]);
        if (options.phase_x < 0 || options.phase_x > 7 || options.phase_y < 0
            || options.phase_y > 7 || options.tile_x < 0 || options.tile_x > 31
            || options.tile_y < 0 || options.tile_y > 31) {
            return false;
        }
        options.search_tile = SendMessageW(control(id_search_tile), BM_GETCHECK, 0, 0) == BST_CHECKED;
        options.search_phase = SendMessageW(control(id_search_phase), BM_GETCHECK, 0, 0) == BST_CHECKED;
        return true;
    }

    void start_embed()
    {
        if (busy_) {
            return;
        }
        if (controller_.input().empty()) {
            set_status(text(L"watermark.files.no_input"));
            return;
        }
        const auto payload = to_utf8(get_text(control(id_payload)));
        if (payload.size() > watermark_files::max_payload) {
            set_status(text(L"watermark.files.invalid_payload"));
            return;
        }
        const auto key = to_utf8(get_text(control(id_key)));
        if (key.empty() || key.size() > watermark_files::max_key) {
            set_status(text(L"watermark.files.invalid_key"));
            return;
        }
        double delta = 0.0;
        double quality = 0.0;
        if (!parse_number(control(id_delta), 1.0, 255.0, delta)
            || !parse_number(control(id_quality), 1.0, 100.0, quality)) {
            set_status(text(L"watermark.files.invalid_number"));
            return;
        }
        watermark_files::EmbedOptions options;
        options.input_path = controller_.input();
        options.output_path = controller_.output();
        options.algorithm = selected_algorithm(control(id_embed_algorithm), false);
        options.payload = payload;
        options.key = key;
        options.delta = delta;
        options.jpeg_quality = static_cast<int>(quality);
        last_kind_ = watermark_files::JobKind::embed;
        last_extract_success_ = false;
        last_payload_.clear();
        set_busy(true);
        set_status(text(L"watermark.files.working"));
        const std::uint64_t generation = session_.begin();
        runner_.start_embed(options, generation, &Dialog::notify, this);
    }

    void start_extract()
    {
        if (busy_) {
            return;
        }
        if (controller_.input().empty()) {
            set_status(text(L"watermark.files.no_input"));
            return;
        }
        const auto key = to_utf8(get_text(control(id_key)));
        if (key.empty() || key.size() > watermark_files::max_key) {
            set_status(text(L"watermark.files.invalid_key"));
            return;
        }
        watermark_files::ExtractOptions options;
        options.input_path = controller_.input();
        options.algorithm = selected_algorithm(control(id_extract_algorithm), true);
        options.key = key;
        double delta = 0.0;
        if (!parse_number(control(id_delta), 1.0, 255.0, delta)
            || !parse_geometry(options)) {
            set_status(text(L"watermark.files.invalid_number"));
            return;
        }
        options.delta = delta;
        last_kind_ = watermark_files::JobKind::extract;
        last_extract_success_ = false;
        last_payload_.clear();
        set_busy(true);
        set_status(text(L"watermark.files.working"));
        const std::uint64_t generation = session_.begin();
        runner_.start_extract(options, generation, &Dialog::notify, this);
    }

    static void notify(void* context)
    {
        auto* dialog = static_cast<Dialog*>(context);
        if (dialog->hwnd_ != nullptr) {
            PostMessageW(dialog->hwnd_, job_done, 0, 0);
        }
    }

    std::wstring describe(const watermark_files::FileOutcome& outcome) const
    {
        std::wstring description = text(watermark_files::status_key(outcome.status));
        if (!outcome.detail.empty()) {
            description += L" — " + localized_detail(outcome.detail, language_);
        }
        if (outcome.status == watermark_files::StatusCode::success) {
            description += L"\n" + std::to_wstring(outcome.width) + L"x"
                + std::to_wstring(outcome.height) + L"  "
                + translate(language_, watermark_files::algorithm_key(outcome.algorithm_used));
            if (last_kind_ == watermark_files::JobKind::embed) {
                description += L"  " + std::to_wstring(outcome.payload_bytes) + L" B  -> "
                    + outcome.output_path;
            } else {
                description += L"  sync " + std::to_wstring(outcome.sync_score)
                    + L"  " + std::to_wstring(outcome.payload_bytes) + L" B";
                if (!outcome.payload.empty()) {
                    const std::wstring utf8 = from_utf8(outcome.payload);
                    description += L"\n"
                        + (utf8.empty() ? hex_payload(outcome.payload, 48) : utf8);
                }
            }
        }
        return description;
    }

    void job_completed()
    {
        auto result = runner_.take_result();
        if (!result) {
            return;
        }
        last_kind_ = result->kind;
        watermark_files::FileOutcome outcome = std::move(result->outcome);
        if (!session_.apply(std::move(outcome))) {
            set_busy(false);
            set_status(text(L"watermark.files.stale"));
            return;
        }
        const auto last = session_.last();
        if (!last) {
            set_busy(false);
            return;
        }
        last_extract_success_ = last_kind_ == watermark_files::JobKind::extract
            && last->status == watermark_files::StatusCode::success;
        last_payload_ = last_extract_success_ ? last->payload : std::vector<std::uint8_t>{};
        set_busy(false);
        set_status(describe(*last));
        if (last_kind_ == watermark_files::JobKind::embed
            && last->status == watermark_files::StatusCode::success) {
            SetWindowTextW(control(id_output), last->output_path.c_str());
            controller_.set_output(last->output_path);
            load_preview(last->output_path);
        }
    }

    void copy_result()
    {
        if (!last_extract_success_) {
            set_status(text(L"watermark.files.no_result"));
            return;
        }
        std::wstring clipboard = from_utf8(last_payload_);
        if (clipboard.empty()) {
            clipboard = hex_payload(last_payload_, last_payload_.size());
        }
        if (!OpenClipboard(hwnd_)) {
            return;
        }
        EmptyClipboard();
        const std::size_t bytes = (clipboard.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            void* target = GlobalLock(memory);
            if (target != nullptr) {
                std::memcpy(target, clipboard.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
        set_status(text(L"watermark.files.copied"));
    }

    void export_result()
    {
        if (!last_extract_success_) {
            set_status(text(L"watermark.files.no_result"));
            return;
        }
        std::wstring detail;
        if (!controller_.export_payload(last_payload_, detail)) {
            set_status(detail.empty() ? text(L"watermark.status.cancelled") : localized_detail(detail, language_));
            return;
        }
        set_status(text(L"watermark.files.exported"));
    }

    void cancel_job()
    {
        if (runner_.cancel()) {
            set_status(text(L"watermark.files.cancelling"));
        }
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message) {
        case WM_CREATE:
            create_controls();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client,
                background_ ? background_ : reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX
                || ui::is_field_control(reinterpret_cast<HWND>(lparam));
            SetTextColor(dc, field ? palette_.field_text : palette_.text);
            SetBkColor(dc, field ? palette_.field_bg : palette_.window_bg);
            HBRUSH brush = field && field_ ? field_ : background_;
            if (brush == nullptr) brush = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            RECT minimum{0, 0, px(760), px(600)};
            AdjustWindowRectEx(&minimum, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE, 0);
            info->ptMinTrackSize.x = minimum.right - minimum.left;
            info->ptMinTrackSize.y = minimum.bottom - minimum.top;
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            create_font();
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            if (id == id_payload && notification == EN_CHANGE) {
                update_payload_count();
                return 0;
            }
            if (notification != BN_CLICKED) {
                return 0;
            }
            switch (id) {
            case IDCANCEL:
                cancel_job(); DestroyWindow(hwnd_); break;
            case id_browse_input: open_input(); break;
            case id_browse_output: choose_output(); break;
            case id_embed: start_embed(); break;
            case id_extract: start_extract(); break;
            case id_cancel: cancel_job(); break;
            case id_copy: copy_result(); break;
            case id_export: export_result(); break;
            case id_close: DestroyWindow(hwnd_); break;
            default: break;
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item->CtlID == id_preview) {
                draw_preview(const_cast<DRAWITEMSTRUCT*>(item));
                return TRUE;
            }
            break;
        }
        case job_done:
            job_completed();
            return 0;
        case WM_CLOSE:
            cancel_job();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            runner_.cancel();
            if (owner_ != nullptr && IsWindow(owner_)) {
                EnableWindow(owner_, TRUE);
            }
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: {
            HWND window = hwnd_;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
            hwnd_ = nullptr;
            return result;
        }
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    std::wstring language_;
    ui::ThemePalette palette_{};
    ui::ControlTheme controls_theme_{};
    HFONT font_ = nullptr;
    bool dark_ = false;
    HBRUSH background_ = nullptr;
    HBRUSH field_ = nullptr;
    UINT dpi_ = 96;
    bool rtl_ = false;
    bool busy_ = false;
    bool output_custom_ = false;
    bool last_extract_success_ = false;
    watermark_files::JobKind last_kind_ = watermark_files::JobKind::embed;
    std::vector<std::uint8_t> last_payload_;
    watermark_files::LoadedImage preview_image_;

    HWND input_label_ = nullptr;
    HWND output_label_ = nullptr;
    HWND embed_algorithm_label_ = nullptr;
    HWND extract_algorithm_label_ = nullptr;
    HWND delta_label_ = nullptr;
    HWND quality_label_ = nullptr;
    HWND payload_label_ = nullptr;
    HWND key_label_ = nullptr;
    HWND geometry_label_ = nullptr;
    HWND input_ = nullptr;
    HWND output_ = nullptr;
    HWND browse_input_ = nullptr;
    HWND browse_output_ = nullptr;
    HWND embed_algorithm_ = nullptr;
    HWND extract_algorithm_ = nullptr;
    HWND payload_ = nullptr;
    HWND payload_count_ = nullptr;
    HWND key_ = nullptr;
    HWND delta_ = nullptr;
    HWND quality_ = nullptr;
    HWND geometry_ = nullptr;
    HWND search_tile_ = nullptr;
    HWND search_phase_ = nullptr;
    HWND preview_ = nullptr;
    HWND embed_ = nullptr;
    HWND extract_ = nullptr;
    HWND copy_ = nullptr;
    HWND export_ = nullptr;
    HWND cancel_ = nullptr;
    HWND close_ = nullptr;
    HWND status_ = nullptr;
    HWND help_ = nullptr;

    watermark_files::GdiplusImageCodec codec_;
    watermark_files::WatermarkFilesService service_;
    Win32FileDialogs dialogs_;
    watermark_files::WatermarkFilesController controller_;
    watermark_files::WatermarkJobRunner runner_;
    watermark_files::WatermarkFilesSession session_;
};

} // namespace

void show_watermark_files(HWND owner, const std::wstring& language, int theme_mode, unsigned long accent)
{
    Dialog dialog(owner, language, theme_mode, accent);
    if (!dialog.create()) {
        return;
    }
    dialog.run();
}

} // namespace av
