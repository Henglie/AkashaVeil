// SPDX-License-Identifier: Apache-2.0
// MSVC 18 rejects the C++/WinRT experimental coroutine header by default.
#ifndef _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#endif
// The product keeps _WIN32_WINNT=0x0601 for the Win7-graded path.  The WGC
// translation unit needs the Windows 10 SDK declarations, but every WinRT
// call is gated on a runtime OS check and the api-set imports are delay-loaded.
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#ifdef WINVER
#undef WINVER
#endif
#define WINVER 0x0A00

#include "platform/windows/frame_backends.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace wg = winrt::Windows::Graphics;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace av {
namespace detail {

namespace {

PipelineError make_error(PipelineError::Domain domain, std::uint32_t code,
    const std::wstring& detail, std::int32_t hresult = 0,
    DWORD win32_error = 0) {
    PipelineError value;
    value.domain = domain;
    value.code = code;
    value.hresult = hresult;
    value.win32_error = win32_error;
    value.detail = detail;
    return value;
}

std::wstring format_hr(std::int32_t value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned>(value));
    return buffer;
}

PipelineError::Domain domain_for_hresult(std::int32_t hresult) {
    if (hresult == E_ACCESSDENIED) return PipelineError::Domain::permission;
    if (hresult == E_INVALIDARG) return PipelineError::Domain::unsupported;
    if (hresult == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
        hresult == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)) {
        return PipelineError::Domain::target_lost;
    }
    return PipelineError::Domain::capture;
}

bool version_at_least_1903() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!fn || fn(&info) != 0) return false;
    return info.dwMajorVersion > 10 ||
        (info.dwMajorVersion == 10 && info.dwBuildNumber >= 18362);
}

std::wstring win32_text(DWORD code) {
    if (code == 0) return L"";
    wchar_t buffer[512]{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buffer, 512, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : L"";
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

bool capture_rect_for_display(const CaptureSource& source, RECT& rect,
    std::wstring& detail) {
    if (source.display_rect.right > source.display_rect.left &&
        source.display_rect.bottom > source.display_rect.top) {
        rect = source.display_rect;
        return true;
    }
    HMONITOR monitor = source.monitor;
    if (!monitor) {
        monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        const DWORD code = GetLastError();
        detail = L"GetMonitorInfo failed: " + win32_text(code);
        return false;
    }
    rect = info.rcMonitor;
    return true;
}

ImageBuffer capture_bitblt_dc(HDC source, int x, int y, int width, int height,
    DWORD rop, PipelineError& error_value) {
    if (width <= 0 || height <= 0) {
        error_value = make_error(PipelineError::Domain::validation, 50,
            L"BitBlt source rectangle is empty");
        return {};
    }
    HDC memory = CreateCompatibleDC(source);
    if (!memory) {
        const DWORD code = GetLastError();
        error_value = make_error(PipelineError::Domain::capture, 51,
            L"CreateCompatibleDC failed: " + win32_text(code),
            HRESULT_FROM_WIN32(code), code);
        return {};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits,
        nullptr, 0);
    if (!bitmap) {
        const DWORD code = GetLastError();
        DeleteDC(memory);
        error_value = make_error(PipelineError::Domain::budget, 52,
            L"CreateDIBSection failed: " + win32_text(code),
            HRESULT_FROM_WIN32(code), code);
        return {};
    }
    HGDIOBJ old = SelectObject(memory, bitmap);
    const BOOL ok = BitBlt(memory, 0, 0, width, height, source, x, y, rop);
    const DWORD code = ok ? 0 : GetLastError();
    ImageBuffer image;
    if (ok) {
        image = ImageBuffer(static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height), 0, AlphaMode::opaque);
        if (!image.valid()) {
            error_value = make_error(PipelineError::Domain::budget, 53,
                L"cannot allocate the BitBlt frame");
        } else {
            std::memcpy(image.bytes().data(), bits, image.bytes().size());
            error_value = {};
        }
    } else {
        error_value = make_error(PipelineError::Domain::capture, 54,
            L"BitBlt failed: " + win32_text(code),
            HRESULT_FROM_WIN32(code), code);
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return image;
}

}  // namespace

std::wstring windows_version_text() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!fn || fn(&info) != 0) return L"unknown";
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%lu.%lu.%lu", info.dwMajorVersion,
        info.dwMinorVersion, info.dwBuildNumber);
    return buffer;
}

CaptureCapability probe_wgc(const CaptureSource& source) {
    CaptureCapability capability;
    capability.backend = CaptureBackendKind::wgc;
    capability.source = source.kind;
    capability.os_version = windows_version_text();
    capability.os_supported = version_at_least_1903();
    capability.api_present = false;
    capability.supported = false;
    capability.state = CaptureBackendState::unknown;
    capability.detail = L"Windows.Graphics.Capture requires Windows 10 1903 (build 18362) or later";
    if (!capability.os_supported) {
        capability.state = CaptureBackendState::unsupported_os;
        return capability;
    }
    if (source.kind == CaptureSourceKind::window && !IsWindow(source.window)) {
        capability.state = CaptureBackendState::unavailable;
        capability.detail = L"capture window is not a valid HWND";
        return capability;
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& exception) {
        if (exception.code() != RPC_E_CHANGED_MODE) {
            capability.state = CaptureBackendState::missing_api;
            capability.hresult = static_cast<std::int32_t>(exception.code());
            capability.detail = L"COM/WinRT initialization failed: " +
                format_hr(capability.hresult);
            return capability;
        }
    } catch (...) {
        capability.state = CaptureBackendState::missing_api;
        capability.detail = L"COM/WinRT initialization failed";
        return capability;
    }
    try {
        capability.api_present = true;
        const bool supported = wgc::GraphicsCaptureSession::IsSupported();
        capability.supported = supported;
        capability.state = supported ? CaptureBackendState::available
                                     : CaptureBackendState::unavailable;
        capability.detail = supported
            ? L"Windows.Graphics.Capture is supported"
            : L"Windows.Graphics.Capture reports unsupported";
    } catch (const winrt::hresult_error& exception) {
        capability.state = CaptureBackendState::missing_api;
        capability.hresult = static_cast<std::int32_t>(exception.code());
        capability.detail = L"Windows.Graphics.Capture activation failed: " +
            format_hr(capability.hresult);
    } catch (...) {
        capability.state = CaptureBackendState::missing_api;
        capability.detail = L"Windows.Graphics.Capture activation failed";
    }
    capability.permission_known = false;
    capability.cross_integrity_tested = false;
    return capability;
}

CaptureCapability probe_bitblt(const CaptureSource& source) {
    CaptureCapability capability;
    capability.backend = CaptureBackendKind::bitblt;
    capability.source = source.kind;
    capability.os_version = windows_version_text();
    capability.os_supported = true;
    capability.api_present = true;
    capability.supported = false;
    capability.state = CaptureBackendState::unknown;
    capability.permission_known = false;
    capability.cross_integrity_tested = false;
    if (source.kind == CaptureSourceKind::window) {
        if (!IsWindow(source.window)) {
            capability.state = CaptureBackendState::unavailable;
            capability.detail = L"capture window is not a valid HWND";
            return capability;
        }
        HDC dc = GetDC(source.window);
        if (!dc) {
            const DWORD code = GetLastError();
            capability.state = code == ERROR_ACCESS_DENIED
                ? CaptureBackendState::permission_denied
                : CaptureBackendState::unavailable;
            capability.win32_error = code;
            capability.detail = L"GetDC(window) failed: " + win32_text(code);
            return capability;
        }
        ReleaseDC(source.window, dc);
        capability.supported = true;
        capability.state = CaptureBackendState::available;
        capability.detail = L"BitBlt window client DC is available";
        return capability;
    }

    RECT rect{};
    std::wstring detail;
    if (!capture_rect_for_display(source, rect, detail)) {
        capability.state = CaptureBackendState::unavailable;
        capability.detail = detail;
        return capability;
    }
    HDC dc = GetDC(nullptr);
    if (!dc) {
        const DWORD code = GetLastError();
        capability.state = code == ERROR_ACCESS_DENIED
            ? CaptureBackendState::permission_denied
            : CaptureBackendState::unavailable;
        capability.win32_error = code;
        capability.detail = L"GetDC(display) failed: " + win32_text(code);
        return capability;
    }
    const int depth = GetDeviceCaps(dc, BITSPIXEL);
    ReleaseDC(nullptr, dc);
    capability.supported = true;
    capability.state = CaptureBackendState::available;
    capability.detail = L"BitBlt display capture is available; bit depth=" +
        std::to_wstring(depth) +
        L"; pixel evidence is only produced when a test explicitly captures "
        L"an owned off-screen surface";
    return capability;
}

namespace {

class BitbltBackend final : public CaptureBackend {
public:
    CaptureBackendKind kind() const noexcept override {
        return CaptureBackendKind::bitblt;
    }

    bool start(const CaptureSource& source, PipelineError& error_value) override {
        source_ = source;
        error_value = {};
        if (source.kind == CaptureSourceKind::window && !IsWindow(source.window)) {
            error_value = make_error(PipelineError::Domain::validation, 60,
                L"BitBlt window capture source is not a valid HWND");
            return false;
        }
        if (source.kind == CaptureSourceKind::display) {
            std::wstring detail;
            if (!capture_rect_for_display(source, display_rect_, detail)) {
                error_value = make_error(PipelineError::Domain::capture, 61,
                    detail);
                return false;
            }
        }
        return true;
    }

    bool next(Frame& out, std::uint32_t, PipelineError& error_value) override {
        const auto begin = std::chrono::steady_clock::now();
        Frame frame;
        if (source_.kind == CaptureSourceKind::window) {
            RECT client{};
            if (!GetClientRect(source_.window, &client)) {
                const DWORD code = GetLastError();
                error_value = make_error(PipelineError::Domain::target_lost, 62,
                    L"GetClientRect failed: " + win32_text(code),
                    HRESULT_FROM_WIN32(code), code);
                return false;
            }
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            HDC dc = GetDC(source_.window);
            if (!dc) {
                const DWORD code = GetLastError();
                error_value = make_error(PipelineError::Domain::capture, 63,
                    L"GetDC(window) failed: " + win32_text(code),
                    HRESULT_FROM_WIN32(code), code);
                return false;
            }
            frame.image = capture_bitblt_dc(dc, 0, 0, width, height, SRCCOPY,
                error_value);
            ReleaseDC(source_.window, dc);
        } else {
            const int width = display_rect_.right - display_rect_.left;
            const int height = display_rect_.bottom - display_rect_.top;
            HDC dc = GetDC(nullptr);
            if (!dc) {
                const DWORD code = GetLastError();
                error_value = make_error(PipelineError::Domain::capture, 64,
                    L"GetDC(display) failed: " + win32_text(code),
                    HRESULT_FROM_WIN32(code), code);
                return false;
            }
            frame.image = capture_bitblt_dc(dc, display_rect_.left,
                display_rect_.top, width, height, SRCCOPY | CAPTUREBLT,
                error_value);
            ReleaseDC(nullptr, dc);
        }
        if (!frame.image.valid()) {
            if (error_value.ok()) {
                error_value = make_error(PipelineError::Domain::capture, 65,
                    L"BitBlt returned an empty frame");
            }
            return false;
        }
        last_processing_ms_ = std::chrono::duration<double,
            std::milli>(std::chrono::steady_clock::now() - begin).count();
        frame.capture_tick_ms = GetTickCount64();
        out = std::move(frame);
        error_value = {};
        return true;
    }

    double last_processing_ms() const noexcept override {
        return last_processing_ms_;
    }

    void stop() override { source_ = {}; }
    unsigned width() const noexcept override {
        if (source_.kind == CaptureSourceKind::display) {
            return static_cast<unsigned>(
                std::max(0L, display_rect_.right - display_rect_.left));
        }
        RECT client{};
        if (source_.window && GetClientRect(source_.window, &client)) {
            return static_cast<unsigned>(std::max(0L, client.right));
        }
        return 0;
    }
    unsigned height() const noexcept override {
        if (source_.kind == CaptureSourceKind::display) {
            return static_cast<unsigned>(
                std::max(0L, display_rect_.bottom - display_rect_.top));
        }
        RECT client{};
        if (source_.window && GetClientRect(source_.window, &client)) {
            return static_cast<unsigned>(std::max(0L, client.bottom));
        }
        return 0;
    }

private:
    CaptureSource source_;
    RECT display_rect_{};
    double last_processing_ms_ = 0.0;
};

class WgcBackend final : public CaptureBackend {
public:
    CaptureBackendKind kind() const noexcept override {
        return CaptureBackendKind::wgc;
    }

    bool start(const CaptureSource& source, PipelineError& error_value) override {
        stop();
        source_ = source;
        if (!version_at_least_1903()) {
            error_value = make_error(PipelineError::Domain::unsupported, 69,
                L"Windows.Graphics.Capture requires Windows 10 1903 or later");
            return false;
        }
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error& exception) {
            // RPC_E_CHANGED_MODE means the thread already has an apartment.
            if (exception.code() != RPC_E_CHANGED_MODE) {
                error_value = make_error(PipelineError::Domain::capture, 70,
                    L"COM apartment initialization failed: " +
                        format_hr(static_cast<std::int32_t>(exception.code())),
                    static_cast<std::int32_t>(exception.code()));
                return false;
            }
        }
        if (!create_device(error_value)) return false;
        if (!make_item(error_value)) return false;
        if (!create_pool(error_value)) return false;
        error_value = {};
        return true;
    }

    bool next(Frame& out, std::uint32_t timeout_ms,
        PipelineError& error_value) override {
        if (!started_) {
            error_value = make_error(PipelineError::Domain::validation, 71,
                L"WGC backend is not started");
            return false;
        }
        if (!recreate_if_needed(error_value)) return false;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        for (;;) {
            try {
                auto frame = pool_.TryGetNextFrame();
                if (frame) {
                    const auto copy_begin = std::chrono::steady_clock::now();
                    const bool ok = copy_frame(frame, out, error_value);
                    last_processing_ms_ = std::chrono::duration<double,
                        std::milli>(std::chrono::steady_clock::now() -
                        copy_begin).count();
                    frame.Close();
                    if (!ok) return false;
                    out.capture_tick_ms = GetTickCount64();
                    error_value = {};
                    return true;
                }
            } catch (const winrt::hresult_error& exception) {
                const auto code = static_cast<std::int32_t>(exception.code());
                error_value = make_error(domain_for_hresult(code), 72,
                    L"WGC TryGetNextFrame failed: " + format_hr(code),
                    code);
                return false;
            } catch (...) {
                error_value = make_error(PipelineError::Domain::capture, 73,
                    L"WGC TryGetNextFrame failed");
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                // A timeout is not a backend failure; the caller decides
                // whether the target is unavailable.
                error_value = {};
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void stop() override {
        try { if (session_) session_.Close(); } catch (...) {}
        try { if (pool_) pool_.Close(); } catch (...) {}
        session_ = nullptr;
        pool_ = nullptr;
        item_ = nullptr;
        staging_.Reset();
        started_ = false;
        last_width_ = 0;
        last_height_ = 0;
    }

    unsigned width() const noexcept override {
        return static_cast<unsigned>(std::max(0, last_width_));
    }
    unsigned height() const noexcept override {
        return static_cast<unsigned>(std::max(0, last_height_));
    }
    double last_processing_ms() const noexcept override {
        return last_processing_ms_;
    }

private:
    bool create_device(PipelineError& error_value) {
        if (d3d_) return true;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
            d3d_.ReleaseAndGetAddressOf(), &level,
            context_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                flags, nullptr, 0, D3D11_SDK_VERSION,
                d3d_.ReleaseAndGetAddressOf(), &level,
                context_.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) {
            error_value = make_error(PipelineError::Domain::capture, 74,
                L"D3D11CreateDevice failed: " + format_hr(static_cast<std::int32_t>(hr)),
                static_cast<std::int32_t>(hr));
            return false;
        }
        ComPtr<IDXGIDevice> dxgi;
        hr = d3d_.As(&dxgi);
        if (FAILED(hr)) {
            error_value = make_error(PipelineError::Domain::capture, 75,
                L"IDXGIDevice query failed: " + format_hr(static_cast<std::int32_t>(hr)),
                static_cast<std::int32_t>(hr));
            return false;
        }
        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
        if (FAILED(hr)) {
            error_value = make_error(PipelineError::Domain::capture, 76,
                L"CreateDirect3D11DeviceFromDXGIDevice failed: " +
                    format_hr(static_cast<std::int32_t>(hr)),
                static_cast<std::int32_t>(hr));
            return false;
        }
        device_ = inspectable.as<wgd3d::IDirect3DDevice>();
        return true;
    }

    bool make_item(PipelineError& error_value) {
        try {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            HRESULT hr = E_FAIL;
            if (source_.kind == CaptureSourceKind::window) {
                hr = interop->CreateForWindow(source_.window,
                    winrt::guid_of<wgc::GraphicsCaptureItem>(),
                    winrt::put_abi(item_));
            } else {
                HMONITOR monitor = source_.monitor;
                if (!monitor) {
                    monitor = MonitorFromPoint(POINT{0, 0},
                        MONITOR_DEFAULTTOPRIMARY);
                }
                hr = interop->CreateForMonitor(monitor,
                    winrt::guid_of<wgc::GraphicsCaptureItem>(),
                    winrt::put_abi(item_));
            }
            if (FAILED(hr)) {
                const auto code = static_cast<std::int32_t>(hr);
                auto domain = domain_for_hresult(code);
                std::wstring detail = source_.kind == CaptureSourceKind::window
                    ? L"WGC CreateForWindow failed: "
                    : L"WGC CreateForMonitor failed: ";
                detail += format_hr(code);
                if (code == E_INVALIDARG && source_.kind == CaptureSourceKind::window) {
                    detail += L" (WS_EX_TOOLWINDOW, WS_EX_NOACTIVATE, "
                              L"hidden windows and WDA_EXCLUDEFROMCAPTURE make "
                              L"WGC window capture fail)";
                }
                error_value = make_error(domain, 77, detail, code);
                return false;
            }
            return true;
        } catch (const winrt::hresult_error& exception) {
            const auto code = static_cast<std::int32_t>(exception.code());
            error_value = make_error(domain_for_hresult(code), 78,
                L"WGC item activation failed: " + format_hr(code), code);
            return false;
        } catch (...) {
            error_value = make_error(PipelineError::Domain::capture, 79,
                L"WGC item activation failed");
            return false;
        }
    }

    bool create_pool(PipelineError& error_value) {
        try {
            const auto size = item_.Size();
            pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                size);
            session_ = pool_.CreateCaptureSession(item_);
            try { session_.IsCursorCaptureEnabled(false); } catch (...) {}
            try { session_.IsBorderRequired(false); } catch (...) {}
            session_.StartCapture();
            last_width_ = size.Width;
            last_height_ = size.Height;
            started_ = true;
            return true;
        } catch (const winrt::hresult_error& exception) {
            const auto code = static_cast<std::int32_t>(exception.code());
            error_value = make_error(domain_for_hresult(code), 80,
                L"WGC frame pool/session failed: " + format_hr(code), code);
            return false;
        } catch (...) {
            error_value = make_error(PipelineError::Domain::capture, 81,
                L"WGC frame pool/session failed");
            return false;
        }
    }

    bool recreate_if_needed(PipelineError& error_value) {
        try {
            const auto size = item_.Size();
            if (static_cast<int>(size.Width) == last_width_ &&
                static_cast<int>(size.Height) == last_height_) {
                return true;
            }
            pool_.Recreate(device_,
                wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            last_width_ = size.Width;
            last_height_ = size.Height;
            staging_.Reset();
            return true;
        } catch (const winrt::hresult_error& exception) {
            const auto code = static_cast<std::int32_t>(exception.code());
            error_value = make_error(domain_for_hresult(code), 82,
                L"WGC frame pool resize failed: " + format_hr(code), code);
            return false;
        }
    }

    bool copy_frame(const wgc::Direct3D11CaptureFrame& frame, Frame& out,
        PipelineError& error_value) {
        try {
            auto surface = frame.Surface();
            winrt::com_ptr<Windows::Graphics::DirectX::Direct3D11::
                    IDirect3DDxgiInterfaceAccess> access;
            winrt::check_hresult(winrt::get_unknown(surface)->QueryInterface(
                IID_PPV_ARGS(access.put())));
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(
                winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (!staging_) {
                D3D11_TEXTURE2D_DESC staging_desc = desc;
                staging_desc.Usage = D3D11_USAGE_STAGING;
                staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                staging_desc.BindFlags = 0;
                staging_desc.MiscFlags = 0;
                staging_desc.ArraySize = 1;
                staging_desc.MipLevels = 1;
                HRESULT hr = d3d_->CreateTexture2D(&staging_desc, nullptr,
                    staging_.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    error_value = make_error(PipelineError::Domain::budget, 83,
                        L"CreateTexture2D staging failed: " +
                            format_hr(static_cast<std::int32_t>(hr)),
                        static_cast<std::int32_t>(hr));
                    return false;
                }
            }
            context_->CopyResource(staging_.Get(), texture.get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0,
                &mapped);
            if (FAILED(hr)) {
                error_value = make_error(PipelineError::Domain::capture, 84,
                    L"Map staging failed: " +
                        format_hr(static_cast<std::int32_t>(hr)),
                    static_cast<std::int32_t>(hr));
                return false;
            }
            ImageBuffer image(desc.Width, desc.Height, 0, AlphaMode::opaque);
            if (!image.valid()) {
                context_->Unmap(staging_.Get(), 0);
                error_value = make_error(PipelineError::Domain::budget, 85,
                    L"cannot allocate the WGC frame");
                return false;
            }
            const std::size_t row_bytes =
                static_cast<std::size_t>(desc.Width) * bgra8_bytes_per_pixel;
            if (mapped.RowPitch < row_bytes) {
                context_->Unmap(staging_.Get(), 0);
                error_value = make_error(PipelineError::Domain::capture, 86,
                    L"WGC row pitch is smaller than the frame width");
                return false;
            }
            for (std::uint32_t row = 0; row < desc.Height; ++row) {
                std::memcpy(image.bytes().data() +
                        static_cast<std::size_t>(row) * image.stride(),
                    static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch,
                    row_bytes);
            }
            context_->Unmap(staging_.Get(), 0);
            out.image = std::move(image);
            last_width_ = static_cast<int>(desc.Width);
            last_height_ = static_cast<int>(desc.Height);
            return true;
        } catch (const winrt::hresult_error& exception) {
            const auto code = static_cast<std::int32_t>(exception.code());
            error_value = make_error(domain_for_hresult(code), 87,
                L"WGC frame copy failed: " + format_hr(code), code);
            return false;
        } catch (...) {
            error_value = make_error(PipelineError::Domain::capture, 88,
                L"WGC frame copy failed");
            return false;
        }
    }

    CaptureSource source_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;
    wgd3d::IDirect3DDevice device_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    int last_width_ = 0;
    int last_height_ = 0;
    bool started_ = false;
    double last_processing_ms_ = 0.0;
};

}  // namespace

std::unique_ptr<CaptureBackend> make_wgc_backend() {
    return std::make_unique<WgcBackend>();
}

std::unique_ptr<CaptureBackend> make_bitblt_backend() {
    return std::make_unique<BitbltBackend>();
}

}  // namespace detail
}  // namespace av
