// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/proxy_source.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace av {
namespace {

constexpr wchar_t kOwnerClass[] = L"AkashaVeil.CaptureProxy.Owner";
constexpr wchar_t kProxyClass[] = L"AkashaVeil.CaptureProxy.Source";
constexpr wchar_t kProxyProperty[] = L"AkashaVeil.CaptureProxy";
constexpr UINT kPresentMessage = WM_APP + 101;
constexpr int kDwmaCloak = 13;
constexpr int kDwmaCloaked = 14;


bool outside_virtual_screen(const RECT& rect) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect.right <= left || rect.left >= right || rect.bottom <= top
        || rect.top >= bottom;
}

bool cloaked(HWND hwnd) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, kDwmaCloaked, &value,
        sizeof(value))) && value != 0;
}

struct ProxyView {
    ImageBuffer* frame = nullptr;
    std::mutex* mutex = nullptr;
};

void paint_frame(HWND hwnd, HDC dc, ImageBuffer& frame, std::mutex& mutex) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (!frame.valid()) {
        HBRUSH brush = CreateSolidBrush(RGB(8, 8, 12));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(frame.width());
    info.bmiHeader.biHeight = -static_cast<LONG>(frame.height());
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (static_cast<int>(frame.width()) == width &&
        static_cast<int>(frame.height()) == height) {
        SetDIBitsToDevice(dc, 0, 0, width, height, 0, 0, 0,
            static_cast<UINT>(height), frame.bytes().data(), &info,
            DIB_RGB_COLORS);
    } else {
        StretchDIBits(dc, 0, 0, width, height, 0, 0,
            static_cast<int>(frame.width()), static_cast<int>(frame.height()),
            frame.bytes().data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
}

void force_present(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    if (memory && bitmap) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        PrintWindow(hwnd, memory, PW_CLIENTONLY);
        SelectObject(memory, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK proxy_proc(HWND hwnd, UINT message, WPARAM wparam,
    LPARAM lparam) {
    auto* view = reinterpret_cast<ProxyView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<ProxyView*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    }
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        if (view && view->frame && view->mutex) paint_frame(hwnd, dc, *view->frame, *view->mutex);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINT:
    case WM_PRINTCLIENT:
        if (view && view->frame && view->mutex)
            paint_frame(hwnd, reinterpret_cast<HDC>(wparam), *view->frame, *view->mutex);
        return 0;
    case kPresentMessage:
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        force_present(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        return TRUE;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

std::wstring proxy_title(const std::wstring& configured, const TargetMetadata& target) {
    std::wstring title = configured.empty() ? L"AkashaVeil Capture Proxy" : configured;
    if (!target.title.empty()) {
        title += L" - ";
        title.append(target.title, 0, std::min<std::size_t>(target.title.size(), 80));
    }
    title += L" [" + std::to_wstring(GetCurrentProcessId()) + L"]";
    return title;
}

}  // namespace

struct ProxySourceSession::Impl {
    mutable std::mutex mutex;
    ProxySourceStatus status;
    std::atomic<bool> stop{false};
    std::thread worker;
    HWND owner = nullptr;
    HWND proxy = nullptr;
    ImageBuffer frame;
    std::mutex frame_mutex;
    std::unique_ptr<WatermarkSession> session;
    std::unique_ptr<FramePipeline> pipeline;
    TargetKey key;

    void set_state(ProxySourceState state, std::wstring detail = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        status.state = state;
        if (!detail.empty()) status.detail = std::move(detail);
    }
    void fail(std::wstring detail) {
        std::lock_guard<std::mutex> lock(mutex);
        status.state = ProxySourceState::failed;
        status.detail = std::move(detail);
        ++status.failures;
    }
    void cleanup_windows() {
        if (proxy) {
            RemovePropW(proxy, kProxyProperty);
            DestroyWindow(proxy);
            proxy = nullptr;
        }
        if (owner) {
            DestroyWindow(owner);
            owner = nullptr;
        }
    }
};

ProxySourceSession::ProxySourceSession() : impl_(std::make_unique<Impl>()) {}
ProxySourceSession::~ProxySourceSession() { stop(); }

bool ProxySourceSession::start(const ProxySourceConfig& config,
    CompositionError& error) {
    error = {};
    if (impl_->status.state == ProxySourceState::capturing) return true;
    if (!config.target.handle || !IsWindow(reinterpret_cast<HWND>(config.target.handle))) {
        error.domain = CompositionError::Domain::validation;
        error.detail = L"capture proxy target is not a valid window";
        return false;
    }
    if (config.plan.visibility == WatermarkVisibility::screen_and_capture ||
        config.plan.capture_only_route != CaptureOnlyRoute::proxy_source_b) {
        error.domain = CompositionError::Domain::validation;
        error.detail = L"capture proxy requires a capture-only plan on route B";
        return false;
    }
    if (config.fps == 0) {
        error.domain = CompositionError::Domain::validation;
        error.detail = L"capture proxy frame rate must be non-zero";
        return false;
    }

    impl_->status = {};
    impl_->status.state = ProxySourceState::starting;
    impl_->status.target = config.target;

    const Window target = window_from_metadata(config.target);
    impl_->key = target_key_from_window(target);
    HiddenEmbedderSet embedders{make_dct_embedder(), make_dwt_embedder(), make_robust_embedder()}; impl_->session = std::make_unique<WatermarkSession>(embedders);
    CaptureRouteCapabilities routes;
    routes.proxy_source_b = true;
    routes.proxy_source_detail = L"controlled capture-source window"; if (!impl_->session->configure(config.plan, routes, error)
        || !impl_->session->start(error)
        || !impl_->session->attach_target(impl_->key, target, error)) {
        impl_->status.state = ProxySourceState::failed;
        impl_->status.detail = error.detail;
        impl_->session.reset();
        return false;
    }

    WNDCLASSEXW owner_class{sizeof(owner_class)};
    owner_class.lpfnWndProc = DefWindowProcW;
    owner_class.hInstance = GetModuleHandleW(nullptr);
    owner_class.lpszClassName = kOwnerClass;
    if (!RegisterClassExW(&owner_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error.domain = CompositionError::Domain::internal;
        error.detail = L"capture proxy owner class registration failed";
        impl_->session.reset();
        return false;
    }
    WNDCLASSEXW proxy_class{sizeof(proxy_class)};
    proxy_class.lpfnWndProc = proxy_proc;
    proxy_class.hInstance = GetModuleHandleW(nullptr);
    proxy_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    proxy_class.lpszClassName = kProxyClass;
    if (!RegisterClassExW(&proxy_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error.domain = CompositionError::Domain::internal;
        error.detail = L"capture proxy window class registration failed";
        impl_->session.reset();
        return false;
    } impl_->owner = CreateWindowExW(0, kOwnerClass, L"", WS_POPUP,
        -7000, -7000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!impl_->owner) {
        error.domain = CompositionError::Domain::internal;
        error.detail = L"capture proxy owner window creation failed";
        impl_->cleanup_windows();
        impl_->session.reset();
        return false;
    }

    RECT target_rect{};
    if (!GetClientRect(reinterpret_cast<HWND>(config.target.handle), &target_rect)) {
        error.domain = CompositionError::Domain::internal;
        error.detail = L"capture proxy could not read the target client size";
        impl_->cleanup_windows();
        impl_->session.reset();
        return false;
    }
    int width = target_rect.right - target_rect.left;
    int height = target_rect.bottom - target_rect.top;
    if (width <= 0 || height <= 0) {
        RECT window_rect{};
        if (GetWindowRect(reinterpret_cast<HWND>(config.target.handle), &window_rect)) {
            width = window_rect.right - window_rect.left;
            height = window_rect.bottom - window_rect.top;
        }
    }
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        error.domain = CompositionError::Domain::budget;
        error.detail = L"capture proxy target size is outside the supported range";
        impl_->cleanup_windows();
        impl_->session.reset();
        return false;
    }

    auto* view = new ProxyView{&impl_->frame, &impl_->frame_mutex};
    const std::wstring title = proxy_title(config.title, config.target); impl_->proxy = CreateWindowExW(0, kProxyClass, title.c_str(),
        WS_POPUP | WS_VISIBLE, -4000, -4000, width, height, impl_->owner,
        nullptr, GetModuleHandleW(nullptr), view);
    if (!impl_->proxy) {
        delete view;
        error.domain = CompositionError::Domain::internal;
        error.detail = L"capture proxy window creation failed";
        impl_->cleanup_windows();
        impl_->session.reset();
        return false;
    }
    SetPropW(impl_->proxy, kProxyProperty, reinterpret_cast<HANDLE>(1));
    ShowWindow(impl_->proxy, SW_SHOWNOACTIVATE);
    if (config.cloak) {
        BOOL value = TRUE;
        DwmSetWindowAttribute(impl_->proxy, kDwmaCloak, &value, sizeof(value));
    }
    UpdateWindow(impl_->proxy);

    RECT proxy_rect{};
    GetWindowRect(impl_->proxy, &proxy_rect);
    DWORD affinity = 0;
    const bool affinity_known = GetWindowDisplayAffinity(impl_->proxy, &affinity) != FALSE;
    const bool is_cloaked = cloaked(impl_->proxy);
    const bool is_offscreen = outside_virtual_screen(proxy_rect);
    const LONG_PTR exstyle = GetWindowLongPtrW(impl_->proxy, GWL_EXSTYLE);
    const bool visible = IsWindowVisible(impl_->proxy) != FALSE;
    const bool normal_style = (exstyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) == 0;
    const bool wda_zero = !affinity_known || affinity == 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->status.proxy_window = reinterpret_cast<std::uintptr_t>(impl_->proxy);
        impl_->status.owner_window = reinterpret_cast<std::uintptr_t>(impl_->owner);
        impl_->status.visible = visible;
        impl_->status.cloaked = is_cloaked;
        impl_->status.offscreen = is_offscreen;
        impl_->status.local_clean = is_cloaked || is_offscreen;
        impl_->status.wda_zero = wda_zero;
        impl_->status.normal_style = normal_style;
    } if (!visible || !normal_style || !wda_zero || !(is_cloaked || is_offscreen)) {
        error.domain = CompositionError::Domain::unsupported;
        error.detail = L"capture proxy failed its hidden-window safety checks";
        impl_->cleanup_windows();
        impl_->session.reset();
        return false;
    }

    FramePipelineConfig pipeline_config;
    pipeline_config.source.kind = CaptureSourceKind::window;
    pipeline_config.source.window = reinterpret_cast<HWND>(config.target.handle);
    pipeline_config.target = config.target;
    pipeline_config.preferred_backend = CaptureBackendKind::automatic;
    pipeline_config.capture_timeout_ms = config.capture_timeout_ms;
    pipeline_config.target_fps = config.fps;
    pipeline_config.include_clean = true;
    pipeline_config.watermark.enabled = false;
    pipeline_config.hidden.enabled = false; impl_->pipeline = std::make_unique<FramePipeline>();
    PipelineError pipeline_error; if (!impl_->pipeline->start(pipeline_config, pipeline_error)) {
        error.domain = CompositionError::Domain::internal;
        error.detail = pipeline_error.detail.empty()
            ? L"capture proxy could not start the capture backend"
            : pipeline_error.detail;
        impl_->pipeline.reset();
        impl_->cleanup_windows();
        impl_->session.reset();
        return false;
    }
    impl_->stop.store(false);
    impl_->status.backend = impl_->pipeline->backend_selection();
    impl_->status.state = ProxySourceState::capturing;
    impl_->status.detail = L"capture proxy is publishing the composed capture frame";

    Impl* state = impl_.get();
    const TargetKey key = impl_->key; impl_->worker = std::thread([state, key, fps = config.fps]() {
        const auto interval = std::chrono::milliseconds(1000 / std::max(1u, fps));
        while (!state->stop.load()) {
            const auto begin = std::chrono::steady_clock::now();
            FramePair pair;
            PipelineError pipeline_error;
            const auto next = state->pipeline->next(pair, 200, pipeline_error);
            if (next == FramePipeline::NextResult::failed ||
                next == FramePipeline::NextResult::cancelled) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->status.state = pipeline_error.domain == PipelineError::Domain::target_lost
                        ? ProxySourceState::target_gone : ProxySourceState::failed;
                    state->status.detail = pipeline_error.detail.empty()
                        ? L"capture proxy capture stopped" : pipeline_error.detail;
                    ++state->status.failures;
                }
                break;
            }
            if (next == FramePipeline::NextResult::timeout) continue;
            if (!pair.clean.valid()) continue;
            ComposedFrame composed;
            CompositionError compose_error;
            if (!state->session->compose(key, pair.clean.image, composed,
                    compose_error)) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->status.state = ProxySourceState::failed;
                    state->status.detail = compose_error.detail.empty()
                        ? L"capture proxy composition failed" : compose_error.detail;
                    ++state->status.failures;
                }
                break;
            }
            {
                std::lock_guard<std::mutex> lock(state->frame_mutex);
                state->frame = std::move(composed.capture);
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->status.published_frames;
                state->status.pipeline_stats = state->pipeline->stats();
                state->status.watermark_stats = state->session->stats();
            }
            if (state->proxy) PostMessageW(state->proxy, kPresentMessage, 0, 0);
            const auto spent = std::chrono::steady_clock::now() - begin;
            if (spent < interval) {
                std::this_thread::sleep_for(interval - spent);
            }
        }
    });
    error = {};
    return true;
}

void ProxySourceSession::stop() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->status.state == ProxySourceState::idle ||
            impl_->status.state == ProxySourceState::stopped) {
            return;
        }
    }
    impl_->stop.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->pipeline) {
        impl_->pipeline->stop();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->status.pipeline_stats = impl_->pipeline->stats();
        impl_->status.backend = impl_->pipeline->backend_selection();
        impl_->pipeline.reset();
    }
    if (impl_->session) {
        impl_->session->stop();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->status.watermark_stats = impl_->session->stats();
        impl_->session.reset();
    }
    impl_->cleanup_windows();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->status.state = ProxySourceState::stopped;
    impl_->status.detail = L"capture proxy stopped";
    impl_->status.local_clean = true;
}

ProxySourceStatus ProxySourceSession::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status;
}

bool ProxySourceSession::running() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status.state == ProxySourceState::starting ||
        impl_->status.state == ProxySourceState::capturing;
}

}  // namespace av







