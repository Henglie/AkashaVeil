// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/frame_render.h"

#include <dwmapi.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifndef PW_RENDERFULLCONTENT
// The product keeps _WIN32_WINNT=0x0601, while the flag itself is available on
// the Windows versions that can run WGC; define the documented value.
#define PW_RENDERFULLCONTENT 0x00000002
#endif

using Microsoft::WRL::ComPtr;

namespace av {

const wchar_t* render_session_state_name(RenderSessionState state) noexcept {
    switch (state) {
    case RenderSessionState::idle: return L"idle";
    case RenderSessionState::starting: return L"starting";
    case RenderSessionState::running: return L"running";
    case RenderSessionState::target_unavailable: return L"target_unavailable";
    case RenderSessionState::target_lost: return L"target_lost";
    case RenderSessionState::failed: return L"failed";
    case RenderSessionState::stopped: return L"stopped";
    }
    return L"unknown";
}

namespace {

constexpr DWORD kCaptureOnlyWda = 0x11;
constexpr wchar_t kOwnerClass[] = L"Henglie.AkashaVeil.CaptureOnlyOwner";
constexpr wchar_t kLayerClass[] = L"Henglie.AkashaVeil.CaptureOnlyLayer";

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

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}


std::string utc_text() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds);
    return buffer;
}

std::uint64_t process_created_for(unsigned long pid) {
    if (!pid) return 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    FILETIME creation{}, exit{}, kernel{}, user{};
    std::uint64_t value = 0;
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        value = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
            creation.dwLowDateTime;
    }
    CloseHandle(process);
    return value;
}

std::wstring process_path_for(unsigned long pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buffer[32768]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::wstring path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        path.assign(buffer, size);
    }
    CloseHandle(process);
    return path;
}

TargetMetadata describe_target(HWND hwnd, const TargetMetadata& fallback) {
    TargetMetadata target = fallback;
    target.handle = reinterpret_cast<std::uintptr_t>(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) target.pid = pid;
    if (!target.process_created) target.process_created = process_created_for(pid);
    if (target.title.empty()) {
        const int length = GetWindowTextLengthW(hwnd);
        if (length > 0 && length <= 32760) {
            std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
            title.resize(GetWindowTextW(hwnd, title.data(), length + 1));
            target.title = std::move(title);
        }
    }
    if (target.class_name.empty()) {
        wchar_t name[256]{};
        if (GetClassNameW(hwnd, name, 256) > 0) target.class_name = name;
    }
    if (target.path.empty()) target.path = process_path_for(pid);
    return target;
}

unsigned dpi_for(HWND hwnd) {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<GetDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn) {
        const UINT dpi = fn(hwnd);
        if (dpi >= 48 && dpi <= 960) return dpi;
    }
    HDC dc = GetDC(hwnd);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi >= 48 && dpi <= 960 ? static_cast<unsigned>(dpi) : 96;
}

bool cloaked(HWND hwnd) {
    DWORD value = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 14, &value, sizeof(value)))) {
        return value != 0;
    }
    return false;
}

bool save_png(const std::wstring& path, const ImageBuffer& image) {
    if (!image.valid()) return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
            &encoder))) {
        return false;
    }
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(&frame, &properties))) return false;
    if (FAILED(frame->Initialize(properties.Get()))) return false;
    if (FAILED(frame->SetSize(image.width(), image.height()))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (format != GUID_WICPixelFormat32bppBGRA) return false;
    if (FAILED(frame->WritePixels(image.height(),
            static_cast<UINT>(image.stride()),
            static_cast<UINT>(image.bytes().size()),
            const_cast<BYTE*>(image.bytes().data())))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

bool write_text_atomic(const std::wstring& path, const std::string& text) {
    if (path.empty()) return true;
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(),
        static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
        static_cast<std::size_t>(written) == text.size();
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) return false;
    return MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

}  // namespace

struct CaptureOnlyRenderSession::Impl {
    struct LayerContext {
        Impl* owner = nullptr;
        bool watermarked = false;
    };

    struct FrameStore {
        std::mutex mutex;
        FramePair latest;
        std::uint64_t generation = 0;
        bool has_frame = false;
    };

    CaptureOnlyRenderConfig config;
    mutable std::mutex status_mutex;
    CaptureOnlyRenderStatus status;
    FrameStore store;
    std::unique_ptr<FramePipeline> pipeline;
    std::thread capture_thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> capture_done{false};
    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool start_done = false;
    bool start_ok = false;
    PipelineError start_error;
    HWND owner = nullptr;
    HWND capture = nullptr;
    HWND clean = nullptr;
    LayerContext* capture_context = nullptr;
    LayerContext* clean_context = nullptr;
    int width = 0;
    int height = 0;
    unsigned dpi = 96;
    std::uint64_t seen_generation = 0;
    bool first_frame_painted = false;

    static LRESULT CALLBACK owner_proc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static void paint_to_dc(Impl* self, bool watermarked, HDC dc) {
        if (!self || !dc) return;
        RECT client{};
        if (!GetClientRect(watermarked ? self->capture : self->clean, &client)) {
            return;
        }
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) return;
        std::lock_guard<std::mutex> lock(self->store.mutex);
        if (!self->store.has_frame) {
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &client, black);
            DeleteObject(black);
            return;
        }
        const Frame& frame = watermarked ? self->store.latest.watermarked
                                         : self->store.latest.clean;
        if (!frame.valid()) {
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &client, black);
            DeleteObject(black);
            return;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(frame.image.width());
        info.bmiHeader.biHeight = -static_cast<LONG>(frame.image.height());
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (static_cast<int>(frame.image.width()) == width &&
            static_cast<int>(frame.image.height()) == height) {
            SetDIBitsToDevice(dc, 0, 0, width, height, 0, 0, 0,
                static_cast<UINT>(height), frame.image.bytes().data(), &info,
                DIB_RGB_COLORS);
        } else {
            StretchDIBits(dc, 0, 0, width, height, 0, 0,
                static_cast<int>(frame.image.width()),
                static_cast<int>(frame.image.height()),
                frame.image.bytes().data(), &info, DIB_RGB_COLORS, SRCCOPY);
        }
    }

    static LRESULT CALLBACK layer_proc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam) {
        auto* context = reinterpret_cast<LayerContext*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            context = static_cast<LayerContext*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(context));
        }
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (context && context->owner) {
                paint_to_dc(context->owner, context->watermarked, dc);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINT:
        case WM_PRINTCLIENT:
            if (context && context->owner) {
                paint_to_dc(context->owner, context->watermarked,
                    reinterpret_cast<HDC>(wparam));
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            return TRUE;
        case WM_NCDESTROY:
            delete context;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    bool register_classes(std::wstring& detail) {
        WNDCLASSEXW owner_class{sizeof(owner_class)};
        owner_class.lpfnWndProc = owner_proc;
        owner_class.hInstance = GetModuleHandleW(nullptr);
        owner_class.lpszClassName = kOwnerClass;
        if (!RegisterClassExW(&owner_class) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            detail = L"owner class registration failed: " +
                win32_text(GetLastError());
            return false;
        }
        WNDCLASSEXW layer_class{sizeof(layer_class)};
        layer_class.lpfnWndProc = layer_proc;
        layer_class.hInstance = GetModuleHandleW(nullptr);
        layer_class.lpszClassName = kLayerClass;
        layer_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&layer_class) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            detail = L"layer class registration failed: " +
                win32_text(GetLastError());
            return false;
        }
        return true;
    }

    bool create_layers(std::wstring& detail) {
        if (!register_classes(detail)) return false;
        owner = CreateWindowExW(0, kOwnerClass, L"", WS_POPUP, -6000, -6000,
            1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!owner) {
            detail = L"owner window creation failed: " +
                win32_text(GetLastError());
            return false;
        }
        auto* capture_data = new LayerContext{this, true};
        capture = CreateWindowExW(WS_EX_TRANSPARENT, kLayerClass, L"", WS_POPUP,
            -5000, -5000, 320, 240, owner, nullptr,
            GetModuleHandleW(nullptr), capture_data);
        if (!capture) {
            delete capture_data;
            detail = L"capture layer creation failed: " +
                win32_text(GetLastError());
            return false;
        }
        capture_context = capture_data;
        auto* clean_data = new LayerContext{this, false};
        clean = CreateWindowExW(WS_EX_TRANSPARENT, kLayerClass, L"", WS_POPUP,
            -5000, -5000, 320, 240, owner, nullptr,
            GetModuleHandleW(nullptr), clean_data);
        if (!clean) {
            delete clean_data;
            detail = L"clean layer creation failed: " +
                win32_text(GetLastError());
            return false;
        }
        clean_context = clean_data;
        return true;
    }

    void destroy_layers() {
        if (clean) { DestroyWindow(clean); clean = nullptr; clean_context = nullptr; }
        if (capture) { DestroyWindow(capture); capture = nullptr; capture_context = nullptr; }
        if (owner) { DestroyWindow(owner); owner = nullptr; }
        first_frame_painted = false;
    }

    bool target_available(int& out_width, int& out_height, unsigned& out_dpi,
        std::wstring& reason, bool& lost) {
        lost = false;
        HWND hwnd = reinterpret_cast<HWND>(status.target.handle);
        if (!hwnd || !IsWindow(hwnd)) {
            lost = true;
            reason = L"target window was destroyed";
            return false;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (status.target.pid && pid != status.target.pid) {
            lost = true;
            reason = L"target PID changed (HWND reuse)";
            return false;
        }
        if (status.target.process_created &&
            process_created_for(pid) != status.target.process_created) {
            lost = true;
            reason = L"target process creation time changed (PID reuse)";
            return false;
        }
        if (!IsWindowVisible(hwnd)) {
            reason = L"target is hidden";
            return false;
        }
        if (IsIconic(hwnd)) {
            reason = L"target is minimized";
            return false;
        }
        if (cloaked(hwnd)) {
            reason = L"target is DWM-cloaked";
            return false;
        }
        RECT client{};
        if (!GetClientRect(hwnd, &client)) {
            lost = true;
            reason = L"GetClientRect failed: " +
                win32_text(GetLastError());
            return false;
        }
        POINT origin{client.left, client.top};
        if (!ClientToScreen(hwnd, &origin)) {
            lost = true;
            reason = L"ClientToScreen failed: " +
                win32_text(GetLastError());
            return false;
        }
        out_width = client.right - client.left;
        out_height = client.bottom - client.top;
        out_dpi = dpi_for(hwnd);
        if (out_width <= 0 || out_height <= 0) {
            reason = L"target client rectangle is empty";
            return false;
        }
        return true;
    }

    void place_layers(int x, int y, int w, int h) {
        SetWindowPos(capture,
            reinterpret_cast<HWND>(status.target.handle), x, y, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(clean, capture, x, y, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void force_present(HWND hwnd) {
        if (!hwnd) return;
        RECT client{};
        if (!GetClientRect(hwnd, &client)) return;
        const int w = client.right - client.left;
        const int h = client.bottom - client.top;
        if (w <= 0 || h <= 0) return;
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
        if (bitmap) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT);
            SelectObject(memory, old);
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
    }

    void paint_layers(bool force) {
        if (!capture || !clean) return;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(store.mutex);
            generation = store.generation;
        }
        if (!force && generation == seen_generation && first_frame_painted) {
            return;
        }
        seen_generation = generation;
        HDC capture_dc = GetDC(capture);
        if (capture_dc) {
            paint_to_dc(this, true, capture_dc);
            ReleaseDC(capture, capture_dc);
        }
        HDC clean_dc = GetDC(clean);
        if (clean_dc) {
            paint_to_dc(this, false, clean_dc);
            ReleaseDC(clean, clean_dc);
        }
        if (config.force_present_for_offscreen_test) {
            force_present(capture);
            force_present(clean);
        }
        first_frame_painted = true;
    }

    void update_stats_from_pipeline() {
        if (!pipeline) return;
        std::lock_guard<std::mutex> lock(status_mutex);
        status.stats = pipeline->stats();
        status.backend = pipeline->backend_selection();
    }

    static void capture_main(Impl* self) {
        // WGC initializes its own COM apartment inside the backend, after the
        // OS capability gate.  A Win7 load therefore does not touch WinRT.
        auto capture_pipeline = std::make_unique<FramePipeline>();
        PipelineError pipeline_error;
        const bool started = capture_pipeline->start(self->config.pipeline,
            pipeline_error);
        {
            std::lock_guard<std::mutex> lock(self->start_mutex);
            self->start_done = true;
            self->start_ok = started;
            self->start_error = pipeline_error;
            self->pipeline = std::move(capture_pipeline);
        }
        self->start_cv.notify_all();
        if (!started) {
            self->capture_done = true;
            return;
        }
        const std::uint32_t fps = self->config.pipeline.target_fps == 0
            ? 30 : self->config.pipeline.target_fps;
        const auto frame_interval =
            std::chrono::milliseconds(1000 / std::max<std::uint32_t>(1, fps));
        while (!self->stop_requested.load()) {
            const auto begin = std::chrono::steady_clock::now();
            FramePair pair;
            PipelineError error;
            const auto result = self->pipeline->next(pair,
                self->config.pipeline.capture_timeout_ms, error);
            if (result == FramePipeline::NextResult::frame) {
                const bool hidden_embedded = pair.hidden_embedded;
                std::uint64_t published = 0;
                {
                    std::lock_guard<std::mutex> lock(self->store.mutex);
                    self->store.latest = std::move(pair);
                    ++self->store.generation;
                    self->store.has_frame = true;
                    published = self->store.generation;
                }
                std::lock_guard<std::mutex> lock(self->status_mutex);
                self->status.published_frames = published;
                self->status.last_publish_tick_ms = GetTickCount64();
                if (self->config.pipeline.hidden.enabled) {
                    self->status.hidden_status =
                        hidden_embedded ? L"embedded" : L"failed";
                }
                self->status.stats = self->pipeline->stats();
                self->status.backend = self->pipeline->backend_selection();
            } else if (result == FramePipeline::NextResult::timeout) {
                self->update_stats_from_pipeline();
            } else if (result == FramePipeline::NextResult::cancelled) {
                break;
            } else {
                std::lock_guard<std::mutex> lock(self->status_mutex);
                self->status.state = RenderSessionState::failed;
                self->status.last_error = error;
                self->status.detail = L"capture pipeline failed: " +
                    error.detail;
                self->status.stats = self->pipeline->stats();
                break;
            }
            const auto spent = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin);
            if (spent < frame_interval) {
                std::this_thread::sleep_for(frame_interval - spent);
            }
        }
        if (self->pipeline) self->pipeline->stop();
        self->capture_done = true;
    }

    void shutdown_capture() {
        stop_requested = true;
        if (capture_thread.joinable()) capture_thread.join();
        if (pipeline) {
            pipeline->stop();
            std::lock_guard<std::mutex> lock(status_mutex);
            status.stats = pipeline->stats();
        }
    }
};

CaptureOnlyRenderSession::CaptureOnlyRenderSession()
    : impl_(std::make_unique<Impl>()) {}

CaptureOnlyRenderSession::~CaptureOnlyRenderSession() { stop(); }

bool CaptureOnlyRenderSession::start(const CaptureOnlyRenderConfig& config,
    PipelineError& error) {
    stop();
    if (config.pipeline.source.kind != CaptureSourceKind::window) {
        error.domain = PipelineError::Domain::unsupported;
        error.code = 100;
        error.detail = L"capture-only A route currently requires a window source";
        return false;
    }
    HWND target = config.pipeline.source.window;
    if (!target || !IsWindow(target)) {
        error.domain = PipelineError::Domain::validation;
        error.code = 101;
        error.detail = L"capture-only target HWND is not valid";
        return false;
    }
    impl_->config = config;
    impl_->config.pipeline.source.window = target;
    impl_->stop_requested = false;
    impl_->capture_done = false;
    impl_->start_done = false;
    impl_->start_ok = false;
    impl_->start_error = {};
    impl_->seen_generation = 0;
    impl_->first_frame_painted = false;
    impl_->width = 0;
    impl_->height = 0;
    impl_->dpi = 96;
    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status = {};
        impl_->status.state = RenderSessionState::starting;
        impl_->status.target = describe_target(target,
            config.pipeline.target);
        impl_->config.pipeline.target = impl_->status.target;
        impl_->status.hidden_requested = config.pipeline.hidden.enabled;
        impl_->status.hidden_status = config.pipeline.hidden.enabled
            ? L"pending_t13_t14" : L"disabled";
        DWORD affinity = 0;
        if (GetWindowDisplayAffinity(target, &affinity)) {
            impl_->status.target_affinity_known = true;
            impl_->status.target_affinity_before = affinity;
        }
    }

    std::wstring detail;
    if (!impl_->create_layers(detail)) {
        impl_->destroy_layers();
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status.state = RenderSessionState::failed;
        impl_->status.detail = detail;
        error.domain = PipelineError::Domain::capture;
        error.code = 102;
        error.detail = detail;
        return false;
    }

    if (!SetWindowDisplayAffinity(impl_->clean, kCaptureOnlyWda)) {
        const DWORD code = GetLastError();
        detail = L"SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed: " +
            win32_text(code);
        impl_->destroy_layers();
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status.state = RenderSessionState::failed;
        impl_->status.detail = detail;
        impl_->status.last_error.domain =
            code == ERROR_ACCESS_DENIED ? PipelineError::Domain::permission
                                        : PipelineError::Domain::unsupported;
        impl_->status.last_error.win32_error = code;
        error = impl_->status.last_error;
        error.detail = detail;
        return false;
    }
    DWORD clean_affinity = 0;
    if (!GetWindowDisplayAffinity(impl_->clean, &clean_affinity) ||
        clean_affinity != kCaptureOnlyWda) {
        detail = L"clean layer WDA read-back is not 0x11";
        impl_->destroy_layers();
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status.state = RenderSessionState::failed;
        impl_->status.detail = detail;
        error.domain = PipelineError::Domain::unsupported;
        error.code = 103;
        error.detail = detail;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status.clean_affinity = clean_affinity;
        impl_->status.clean_layer_excluded = true;
        impl_->status.owner_layer =
            reinterpret_cast<std::uintptr_t>(impl_->owner);
        impl_->status.capture_layer =
            reinterpret_cast<std::uintptr_t>(impl_->capture);
        impl_->status.clean_layer =
            reinterpret_cast<std::uintptr_t>(impl_->clean);
        impl_->status.layers_destroyed = false;
    }

    impl_->capture_thread = std::thread(&Impl::capture_main, impl_.get());
    {
        std::unique_lock<std::mutex> lock(impl_->start_mutex);
        if (!impl_->start_cv.wait_for(lock, std::chrono::seconds(8),
                [&] { return impl_->start_done; })) {
            impl_->stop_requested = true;
            impl_->capture_thread.join();
            impl_->destroy_layers();
            std::lock_guard<std::mutex> status_lock(impl_->status_mutex);
            impl_->status.state = RenderSessionState::failed;
            impl_->status.detail = L"capture backend did not start within 8s";
            impl_->status.layers_destroyed = true;
            error.domain = PipelineError::Domain::capture;
            error.code = 104;
            error.detail = impl_->status.detail;
            return false;
        }
    }
    if (!impl_->start_ok) {
        impl_->capture_thread.join();
        impl_->destroy_layers();
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status.state = RenderSessionState::failed;
        impl_->status.last_error = impl_->start_error;
        impl_->status.detail = impl_->start_error.detail;
        impl_->status.layers_destroyed = true;
        error = impl_->start_error;
        return false;
    }
    sync();
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    if (impl_->status.state == RenderSessionState::starting) {
        impl_->status.state = RenderSessionState::running;
    }
    error = {};
    return true;
}

void CaptureOnlyRenderSession::request_stop() {
    impl_->stop_requested = true;
}

void CaptureOnlyRenderSession::stop() {
    impl_->shutdown_capture();
    const HWND target = reinterpret_cast<HWND>(impl_->status.target.handle);
    DWORD final_affinity = 0;
    const bool affinity_known = target && IsWindow(target) &&
        GetWindowDisplayAffinity(target, &final_affinity) != FALSE;
    impl_->destroy_layers();
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    impl_->status.capture_layer = 0;
    impl_->status.clean_layer = 0;
    impl_->status.owner_layer = 0;
    impl_->status.capture_layer_visible = false;
    impl_->status.clean_layer_visible = false;
    impl_->status.layers_destroyed = true;
    if (affinity_known) {
        impl_->status.target_affinity_known = true;
        impl_->status.target_affinity_after = final_affinity;
    }
    if (impl_->status.state != RenderSessionState::target_lost &&
        impl_->status.state != RenderSessionState::failed) {
        impl_->status.state = RenderSessionState::stopped;
    }
    if (impl_->status.detail.empty()) {
        impl_->status.detail = L"capture-only session stopped; target was never modified";
    }
}

void CaptureOnlyRenderSession::sync() {
    if (!impl_->capture || !impl_->clean) return;
    HWND target = reinterpret_cast<HWND>(impl_->status.target.handle);
    int width = 0;
    int height = 0;
    unsigned dpi = 96;
    std::wstring reason;
    bool lost = false;
    const bool available = impl_->target_available(width, height, dpi, reason,
        lost);

    if (lost) {
        {
            std::lock_guard<std::mutex> lock(impl_->status_mutex);
            impl_->status.state = RenderSessionState::target_lost;
            impl_->status.detail = reason;
            impl_->status.capture_layer_visible = false;
            impl_->status.clean_layer_visible = false;
        }
        ShowWindow(impl_->capture, SW_HIDE);
        ShowWindow(impl_->clean, SW_HIDE);
        impl_->stop_requested = true;
        return;
    }

    if (!available) {
        {
            std::lock_guard<std::mutex> lock(impl_->status_mutex);
            impl_->status.state = RenderSessionState::target_unavailable;
            impl_->status.detail = reason;
            impl_->status.capture_layer_visible = false;
            impl_->status.clean_layer_visible = false;
        }
        ShowWindow(impl_->capture, SW_HIDE);
        ShowWindow(impl_->clean, SW_HIDE);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        if (impl_->status.state == RenderSessionState::failed) {
            ShowWindow(impl_->capture, SW_HIDE);
            ShowWindow(impl_->clean, SW_HIDE);
            impl_->status.capture_layer_visible = false;
            impl_->status.clean_layer_visible = false;
            return;
        }
    }

    RECT client{};
    GetClientRect(target, &client);
    POINT origin{client.left, client.top};
    ClientToScreen(target, &origin);
    const int x = origin.x;
    const int y = origin.y;

    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        if (impl_->status.state != RenderSessionState::running) {
            impl_->status.state = RenderSessionState::running;
        }
        impl_->status.detail = L"capturing clean and capture-only branches";
        impl_->status.width = width;
        impl_->status.height = height;
        impl_->status.dpi = dpi;
        impl_->width = width;
        impl_->height = height;
        impl_->dpi = dpi;
    }
    impl_->place_layers(x, y, width, height);

    DWORD affinity = 0;
    const bool affinity_ok = GetWindowDisplayAffinity(impl_->clean, &affinity) != FALSE;
    if (!affinity_ok || affinity != kCaptureOnlyWda) {
        if (!SetWindowDisplayAffinity(impl_->clean, kCaptureOnlyWda)) {
            const DWORD code = GetLastError();
            std::lock_guard<std::mutex> lock(impl_->status_mutex);
            impl_->status.state = RenderSessionState::failed;
            impl_->status.detail =
                L"clean layer lost WDA_EXCLUDEFROMCAPTURE: " +
                win32_text(code);
            impl_->status.last_error.domain = PipelineError::Domain::permission;
            impl_->status.last_error.win32_error = code;
            impl_->status.capture_layer_visible = false;
            impl_->status.clean_layer_visible = false;
            impl_->stop_requested = true;
            ShowWindow(impl_->capture, SW_HIDE);
            ShowWindow(impl_->clean, SW_HIDE);
            return;
        }
    }
    bool has_frame = false;
    {
        std::lock_guard<std::mutex> lock(impl_->store.mutex);
        has_frame = impl_->store.has_frame;
    }
    if (!has_frame) {
        // Do not flash black mirrors over the target before the first frame.
        ShowWindow(impl_->capture, SW_HIDE);
        ShowWindow(impl_->clean, SW_HIDE);
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status.capture_layer_visible = false;
        impl_->status.clean_layer_visible = false;
        if (impl_->pipeline) impl_->status.stats = impl_->pipeline->stats();
        return;
    }
    if (!IsWindowVisible(impl_->capture)) {
        ShowWindow(impl_->capture, SW_SHOWNOACTIVATE);
    }
    if (!IsWindowVisible(impl_->clean)) {
        ShowWindow(impl_->clean, SW_SHOWNOACTIVATE);
    }
    impl_->paint_layers(false);
    {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status.capture_layer_visible =
            IsWindowVisible(impl_->capture) != FALSE;
        impl_->status.clean_layer_visible =
            IsWindowVisible(impl_->clean) != FALSE;
        DWORD current = 0;
        if (GetWindowDisplayAffinity(impl_->clean, &current)) {
            impl_->status.clean_affinity = current;
            impl_->status.clean_layer_excluded = current == kCaptureOnlyWda;
        }
        if (impl_->pipeline) impl_->status.stats = impl_->pipeline->stats();
    }
    impl_->update_stats_from_pipeline();
}

CaptureOnlyRenderStatus CaptureOnlyRenderSession::status() const {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    return impl_->status;
}

bool CaptureOnlyRenderSession::dump_clean_png(const std::wstring& path,
    PipelineError& error) {
    if (!impl_->clean) {
        error.domain = PipelineError::Domain::validation;
        error.code = 110;
        error.detail = L"clean layer does not exist";
        return false;
    }
    Frame clean_frame;
    {
        std::lock_guard<std::mutex> lock(impl_->store.mutex);
        if (!impl_->store.has_frame || !impl_->store.latest.clean.valid()) {
            error.domain = PipelineError::Domain::validation;
            error.code = 111;
            error.detail = L"no clean frame has been captured yet";
            return false;
        }
        clean_frame = impl_->store.latest.clean;
    }
    DWORD before = 0;
    GetWindowDisplayAffinity(impl_->clean, &before);
    if (!SetWindowDisplayAffinity(impl_->clean, 0)) {
        const DWORD code = GetLastError();
        error.domain = PipelineError::Domain::permission;
        error.code = 112;
        error.win32_error = code;
        error.detail = L"temporary WDA clear failed: " + win32_text(code);
        return false;
    }
    Sleep(40);
    impl_->paint_layers(true);
    RECT client{};
    GetClientRect(impl_->clean, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    bool saved = false;
    if (w > 0 && h > 0) {
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS,
            &bits, nullptr, 0);
        if (bitmap) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            if (PrintWindow(impl_->clean, memory, PW_RENDERFULLCONTENT)) {
                ImageBuffer image(static_cast<std::uint32_t>(w),
                    static_cast<std::uint32_t>(h), 0, AlphaMode::opaque);
                if (image.valid()) {
                    std::memcpy(image.bytes().data(), bits,
                        image.bytes().size());
                    saved = save_png(path, image);
                }
            }
            SelectObject(memory, old);
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
    }
    SetWindowDisplayAffinity(impl_->clean, kCaptureOnlyWda);
    DWORD after = 0;
    GetWindowDisplayAffinity(impl_->clean, &after);
    if (!saved) {
        error.domain = PipelineError::Domain::capture;
        error.code = 113;
        error.detail = L"clean layer PrintWindow/PNG save failed";
        return false;
    }
    if (after != kCaptureOnlyWda) {
        error.domain = PipelineError::Domain::unsupported;
        error.code = 114;
        error.detail = L"clean layer WDA restore failed";
        return false;
    }
    error = {};
    return true;
}

bool CaptureOnlyRenderSession::write_status_json(const std::wstring& path,
    PipelineError& error) const {
    CaptureOnlyRenderStatus snapshot = status();
    std::string json;
    json += "{\"schema\":1,\"role\":\"t11_capture_only_session\",\"time_utc\":\"";
    json += utc_text();
    json += "\",\"pid\":" + std::to_string(GetCurrentProcessId());
    json += ",\"state\":\"" +
        std::string(utf8(render_session_state_name(snapshot.state))) + "\"";
    json += ",\"detail\":\"" + json_escape(utf8(snapshot.detail)) + "\"";
    json += ",\"target\":{\"hwnd\":\"" + [&] {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"0x%llX",
            static_cast<unsigned long long>(snapshot.target.handle));
        return std::string(utf8(buffer));
    }() + "\",\"pid\":" + std::to_string(snapshot.target.pid);
    json += ",\"created\":" + std::to_string(snapshot.target.process_created);
    json += ",\"title\":\"" + json_escape(utf8(snapshot.target.title)) + "\"";
    json += ",\"class_name\":\"" +
        json_escape(utf8(snapshot.target.class_name)) + "\"";
    json += ",\"path\":\"" + json_escape(utf8(snapshot.target.path)) + "\"}";
    auto hex = [](std::uintptr_t value) {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"0x%llX",
            static_cast<unsigned long long>(value));
        return std::string(utf8(buffer));
    };
    json += ",\"geometry\":{\"width\":" + std::to_string(snapshot.width) +
        ",\"height\":" + std::to_string(snapshot.height) +
        ",\"dpi\":" + std::to_string(snapshot.dpi) + "}";
    json += ",\"layers\":{\"owner_hwnd\":\"" + hex(snapshot.owner_layer) +
        "\",\"capture_hwnd\":\"" + hex(snapshot.capture_layer) +
        "\",\"clean_hwnd\":\"" + hex(snapshot.clean_layer) +
        "\",\"capture_visible\":" +
        (snapshot.capture_layer_visible ? "true" : "false") +
        ",\"clean_visible\":" +
        (snapshot.clean_layer_visible ? "true" : "false") +
        ",\"clean_affinity\":" + std::to_string(snapshot.clean_affinity) +
        ",\"clean_excluded\":" +
        (snapshot.clean_layer_excluded ? "true" : "false") +
        ",\"destroyed\":" +
        (snapshot.layers_destroyed ? "true" : "false") + "}";
    json += ",\"backend\":{\"requested\":\"" +
        std::string(utf8(capture_backend_kind_name(
            snapshot.backend.requested))) +
        "\",\"selected\":\"" +
        std::string(utf8(capture_backend_kind_name(
            snapshot.backend.selected))) +
        "\",\"fallback\":" + (snapshot.backend.fallback ? "true" : "false") +
        ",\"selected_state\":\"" +
        std::string(utf8(capture_backend_state_name(
            snapshot.backend.selected_capability.state))) +
        "\",\"os_version\":\"" +
        json_escape(utf8(snapshot.backend.selected_capability.os_version)) +
        "\",\"detail\":\"" +
        json_escape(utf8(snapshot.backend.selected_capability.detail)) + "\"}";
    json += ",\"affinity\":{\"known\":" +
        std::string(snapshot.target_affinity_known ? "true" : "false") +
        ",\"before\":" + std::to_string(snapshot.target_affinity_before) +
        ",\"after\":" + std::to_string(snapshot.target_affinity_after) + "}";
    json += ",\"hidden\":{\"requested\":" +
        std::string(snapshot.hidden_requested ? "true" : "false") +
        ",\"status\":\"" + json_escape(utf8(snapshot.hidden_status)) + "\"}";
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        json += ",\"memory\":{\"working_set_bytes\":" +
            std::to_string(memory.WorkingSetSize) +
            ",\"peak_working_set_bytes\":" +
            std::to_string(memory.PeakWorkingSetSize) +
            ",\"private_bytes\":" +
            std::to_string(memory.PrivateUsage) + "}";
    }
    const PipelineStats& stats = snapshot.stats;
    json += ",\"stats\":{\"frames\":" + std::to_string(stats.frames) +
        ",\"clean_frames\":" + std::to_string(stats.clean_frames) +
        ",\"watermarked_frames\":" + std::to_string(stats.watermarked_frames) +
        ",\"blank_frames\":" + std::to_string(stats.blank_frames) +
        ",\"failed_frames\":" + std::to_string(stats.failed_frames) +
        ",\"hidden_embedded\":" + std::to_string(stats.hidden_embedded) +
        ",\"hidden_skipped\":" + std::to_string(stats.hidden_skipped) +
        ",\"frame_bytes\":" + std::to_string(stats.frame_bytes) +
        ",\"peak_buffered_bytes\":" +
        std::to_string(stats.peak_buffered_bytes) +
        ",\"avg_capture_ms\":" + std::to_string(stats.avg_capture_ms) +
        ",\"avg_capture_wait_ms\":" +
        std::to_string(stats.avg_capture_wait_ms) +
        ",\"avg_normalize_ms\":" + std::to_string(stats.avg_normalize_ms) +
        ",\"avg_composite_ms\":" + std::to_string(stats.avg_composite_ms) +
        ",\"avg_hidden_ms\":" + std::to_string(stats.avg_hidden_ms) +
        ",\"avg_pipeline_ms\":" + std::to_string(stats.avg_pipeline_ms) +
        ",\"avg_total_ms\":" + std::to_string(stats.avg_total_ms) +
        ",\"max_total_ms\":" + std::to_string(stats.max_total_ms) +
        ",\"last_pipeline_ms\":" + std::to_string(stats.last_pipeline_ms) +
        ",\"last_capture_wait_ms\":" +
        std::to_string(stats.last_capture_wait_ms) +
        ",\"fps\":" + std::to_string(stats.fps) + "}";
    json += ",\"published_frames\":" +
        std::to_string(snapshot.published_frames) +
        ",\"last_publish_tick_ms\":" +
        std::to_string(snapshot.last_publish_tick_ms);
    wchar_t hr[16]{};
    swprintf_s(hr, L"0x%08X",
        static_cast<unsigned>(snapshot.last_error.hresult));
    json += ",\"error\":{\"domain\":\"" +
        std::string(utf8(pipeline_error_domain_name(
            snapshot.last_error.domain))) +
        "\",\"code\":" + std::to_string(snapshot.last_error.code) +
        ",\"hresult\":\"" + std::string(utf8(hr)) + "\",\"win32\":" +
        std::to_string(snapshot.last_error.win32_error) + ",\"detail\":\"" +
        json_escape(utf8(snapshot.last_error.detail)) + "\"}}";
    if (!write_text_atomic(path, json)) {
        error.domain = PipelineError::Domain::internal;
        error.code = 120;
        error.detail = L"status JSON write failed";
        return false;
    }
    error = {};
    return true;
}

}  // namespace av


