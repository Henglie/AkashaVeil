// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/frame_pipeline.h"

#include "platform/windows/frame_backends.h"

#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>

namespace av {

const wchar_t* capture_source_kind_name(CaptureSourceKind kind) noexcept {
    switch (kind) {
    case CaptureSourceKind::window: return L"window";
    case CaptureSourceKind::display: return L"display";
    }
    return L"unknown";
}

const wchar_t* capture_backend_kind_name(CaptureBackendKind kind) noexcept {
    switch (kind) {
    case CaptureBackendKind::automatic: return L"automatic";
    case CaptureBackendKind::wgc: return L"wgc";
    case CaptureBackendKind::bitblt: return L"bitblt";
    }
    return L"unknown";
}

const wchar_t* capture_backend_state_name(CaptureBackendState state) noexcept {
    switch (state) {
    case CaptureBackendState::available: return L"available";
    case CaptureBackendState::unsupported_os: return L"unsupported_os";
    case CaptureBackendState::missing_api: return L"missing_api";
    case CaptureBackendState::permission_denied: return L"permission_denied";
    case CaptureBackendState::unavailable: return L"unavailable";
    case CaptureBackendState::unknown: return L"unknown";
    }
    return L"unknown";
}

const wchar_t* pipeline_error_domain_name(PipelineError::Domain domain) noexcept {
    switch (domain) {
    case PipelineError::Domain::none: return L"none";
    case PipelineError::Domain::validation: return L"validation";
    case PipelineError::Domain::unsupported: return L"unsupported";
    case PipelineError::Domain::permission: return L"permission";
    case PipelineError::Domain::capture: return L"capture";
    case PipelineError::Domain::watermark: return L"watermark";
    case PipelineError::Domain::hidden: return L"hidden";
    case PipelineError::Domain::budget: return L"budget";
    case PipelineError::Domain::cancelled: return L"cancelled";
    case PipelineError::Domain::target_lost: return L"target_lost";
    case PipelineError::Domain::internal: return L"internal";
    }
    return L"unknown";
}

namespace {

PipelineError error(PipelineError::Domain domain, std::uint32_t code,
    std::wstring detail, std::int32_t hresult = 0, DWORD win32_error = 0) {
    PipelineError value;
    value.domain = domain;
    value.code = code;
    value.hresult = hresult;
    value.win32_error = win32_error;
    value.detail = std::move(detail);
    return value;
}

std::wstring local_mark_time() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond);
    return buffer;
}

std::wstring format_hr(std::int32_t value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned>(value));
    return buffer;
}

class GdiplusUse {
public:
    GdiplusUse() {
        std::lock_guard<std::mutex> lock(mutex());
        if (references()++ == 0) {
            Gdiplus::GdiplusStartupInput input;
            if (Gdiplus::GdiplusStartup(&token(), &input, nullptr) !=
                Gdiplus::Ok) {
                references() = 0;
                token() = 0;
            }
        }
    }
    ~GdiplusUse() {
        std::lock_guard<std::mutex> lock(mutex());
        if (references() > 0 && --references() == 0 && token() != 0) {
            Gdiplus::GdiplusShutdown(token());
            token() = 0;
        }
    }
    bool ok() const { return token() != 0; }

private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }
    static int& references() {
        static int value = 0;
        return value;
    }
    static ULONG_PTR& token() {
        static ULONG_PTR value = 0;
        return value;
    }
};

Gdiplus::Color color(std::uint32_t rgb, unsigned alpha = 255) {
    return Gdiplus::Color(static_cast<BYTE>(std::min(255u, alpha)),
        GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

double millis(std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void enforce_opaque(ImageBuffer& frame) {
    auto* bytes = frame.bytes().data();
    const std::size_t count = frame.width() * frame.height();
    for (std::size_t i = 0; i < count; ++i) bytes[i * 4 + 3] = 255;
}

}  // namespace

Window window_from_metadata(const TargetMetadata& target) {
    Window window;
    window.handle = target.handle;
    window.pid = target.pid;
    window.process_created = target.process_created;
    window.title = target.title;
    window.path = target.path;
    window.class_name = target.class_name;
    return window;
}

ExplicitWatermarkSpec explicit_watermark_for_capture(const Effects& effects,
    unsigned dpi, bool evidence_block) {
    ExplicitWatermarkSpec spec;
    spec.enabled = effects.mark != Mark::none;
    switch (effects.mark) {
    case Mark::none: spec.layout = ExplicitLayout::none; break;
    case Mark::tiled: spec.layout = ExplicitLayout::tiled; break;
    case Mark::center: spec.layout = ExplicitLayout::center; break;
    case Mark::corner: spec.layout = ExplicitLayout::corner; break;
    }
    spec.opacity = effects.opacity;
    spec.font_size = effects.font_size;
    spec.spacing = effects.spacing;
    spec.foreground = effects.foreground;
    spec.background = effects.background;
    spec.text = effects.text;
    spec.label = effects.label;
    spec.tag = effects.tag;
    spec.dpi = dpi == 0 ? 96 : dpi;
    spec.evidence_block = evidence_block;
    return spec;
}

bool normalize_frame(const ImageBuffer& input, ImageBuffer& output,
    PipelineError& error_value) {
    if (!input.valid()) {
        error_value = error(PipelineError::Domain::validation, 1,
            L"capture frame is not a valid BGRA8 image");
        return false;
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(input.width()) * bgra8_bytes_per_pixel;
    if (input.stride() < row_bytes) {
        error_value = error(PipelineError::Domain::validation, 2,
            L"capture stride is smaller than the row width");
        return false;
    }
    ImageBuffer normalized(input.width(), input.height(), 0, input.alpha());
    if (!normalized.valid()) {
        error_value = error(PipelineError::Domain::budget, 3,
            L"cannot allocate the normalized frame");
        return false;
    }
    for (std::uint32_t row = 0; row < input.height(); ++row) {
        std::memcpy(normalized.bytes().data() +
                        static_cast<std::size_t>(row) * normalized.stride(),
            input.bytes().data() +
                static_cast<std::size_t>(row) * input.stride(),
            row_bytes);
    }
    output = std::move(normalized);
    error_value = {};
    return true;
}

void force_opaque(ImageBuffer& frame) {
    if (frame.valid()) enforce_opaque(frame);
}

bool is_blank_frame(const ImageBuffer& frame) {
    if (!frame.valid()) return true;
    const int step = std::max(1, static_cast<int>(frame.height() / 32));
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    for (std::uint32_t y = 0; y < frame.height();
         y += static_cast<std::uint32_t>(step)) {
        const std::uint8_t* row =
            frame.bytes().data() + static_cast<std::size_t>(y) * frame.stride();
        for (std::uint32_t x = 0; x < frame.width(); x += 16) {
            const std::uint8_t* pixel =
                row + static_cast<std::size_t>(x) * bgra8_bytes_per_pixel;
            sum += pixel[0] + pixel[1] + pixel[2];
            count += 3;
        }
    }
    if (count == 0) return true;
    const double average = static_cast<double>(sum) / static_cast<double>(count);
    return average > 250.0 || average < 3.0;
}

bool compose_explicit_watermark(ImageBuffer& frame, const Window& target,
    const ExplicitWatermarkSpec& spec, PipelineError& error_value) {
    if (!spec.enabled && !spec.evidence_block) {
        error_value = {};
        return true;
    }
    if (!frame.valid()) {
        error_value = error(PipelineError::Domain::validation, 10,
            L"cannot compose a watermark on an invalid frame");
        return false;
    }
    if (frame.stride() > static_cast<std::size_t>(INT_MAX) ||
        frame.width() > static_cast<std::uint32_t>(INT_MAX) ||
        frame.height() > static_cast<std::uint32_t>(INT_MAX)) {
        error_value = error(PipelineError::Domain::budget, 11,
            L"frame is too large for GDI+");
        return false;
    }
    GdiplusUse gdiplus;
    if (!gdiplus.ok()) {
        error_value = error(PipelineError::Domain::watermark, 12,
            L"GDI+ startup failed");
        return false;
    }

    const int width = static_cast<int>(frame.width());
    const int height = static_cast<int>(frame.height());
    Gdiplus::Bitmap bitmap(width, height, static_cast<INT>(frame.stride()),
        PixelFormat32bppARGB, frame.bytes().data());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        error_value = error(PipelineError::Domain::watermark, 13,
            L"GDI+ bitmap creation failed");
        return false;
    }
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    const Gdiplus::REAL scale =
        static_cast<Gdiplus::REAL>(spec.dpi == 0 ? 96 : spec.dpi) / 96.0f;
    const std::wstring time = local_mark_time();
    if (spec.enabled &&
        (spec.layout == ExplicitLayout::tiled ||
         spec.layout == ExplicitLayout::center ||
         spec.layout == ExplicitLayout::corner)) {
        const std::wstring text = expand_mark(spec.text, target, spec.tag, time);
        if (!text.empty()) {
            Gdiplus::Font font(L"Segoe UI",
                static_cast<Gdiplus::REAL>(spec.font_size) * scale,
                Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            if (font.GetLastStatus() != Gdiplus::Ok) {
                error_value = error(PipelineError::Domain::watermark, 14,
                    L"GDI+ font creation failed");
                return false;
            }
            Gdiplus::SolidBrush brush(
                color(spec.foreground, spec.opacity * 255 / 100));
            Gdiplus::StringFormat format;
            format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            if (spec.layout == ExplicitLayout::tiled) {
                format.SetAlignment(Gdiplus::StringAlignmentCenter);
                format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                const Gdiplus::REAL step =
                    std::max(40.0f, static_cast<Gdiplus::REAL>(spec.spacing) * scale);
                const Gdiplus::REAL line = std::max(80.0f * scale, step * 0.6f);
                for (Gdiplus::REAL y = -line; y < height + line; y += line) {
                    for (Gdiplus::REAL x = -step; x < width + step; x += step) {
                        const INT saved = graphics.Save();
                        graphics.TranslateTransform(x + step / 2, y + line / 2);
                        graphics.RotateTransform(-25.0f);
                        graphics.DrawString(text.c_str(),
                            static_cast<INT>(text.size()), &font,
                            Gdiplus::RectF(-step * 0.47f, -line / 2,
                                step * 0.94f, line),
                            &format, &brush);
                        graphics.Restore(saved);
                    }
                }
            } else {
                Gdiplus::RectF box(12.0f * scale, 12.0f * scale,
                    std::max(1.0f, width - 24.0f * scale),
                    std::max(1.0f, height - 24.0f * scale));
                if (spec.layout == ExplicitLayout::corner) {
                    format.SetAlignment(Gdiplus::StringAlignmentFar);
                    format.SetLineAlignment(Gdiplus::StringAlignmentFar);
                } else {
                    format.SetAlignment(Gdiplus::StringAlignmentCenter);
                    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                }
                graphics.DrawString(text.c_str(),
                    static_cast<INT>(text.size()), &font, box, &format, &brush);
            }
        }
    }

    if (spec.evidence_block) {
        const Gdiplus::REAL block = std::max(8.0f, 30.0f * scale);
        const Gdiplus::REAL x = width - block - 14.0f * scale;
        const Gdiplus::REAL y = height - block - 14.0f * scale;
        Gdiplus::SolidBrush orange(Gdiplus::Color(255, 255, 140, 0));
        Gdiplus::SolidBrush dark(Gdiplus::Color(255, 30, 30, 30));
        graphics.FillRectangle(&orange, x, y, block, block);
        graphics.FillRectangle(&dark, x, y, block, std::max(2.0f, 5.0f * scale));
    }

    graphics.Flush(Gdiplus::FlushIntentionSync);
    // The pipeline normalizes alpha on the backend boundary.  The caller owns
    // the frame and may keep the capture stream opaque for WIC/PNG output.
    error_value = {};
    return true;
}

bool apply_blind_watermark(const ImageView& source,
    const MutableImageView& target,
    const BlindWatermarkRequest& request,
    const std::shared_ptr<BlindWatermarkEmbedder>& embedder,
    PipelineError& error_value) {
    if (!request.enabled) {
        error_value = {};
        return true;
    }
    if (!valid_image_view(source) || !valid_image_view(target)) {
        error_value = error(PipelineError::Domain::validation, 20,
            L"hidden watermark received an invalid image view");
        return false;
    }
    if (!image_views_match(source,
            ImageView{target.width, target.height, target.stride,
                target.format, target.alpha, target.pixels})) {
        error_value = error(PipelineError::Domain::validation, 21,
            L"hidden watermark source and target dimensions differ");
        return false;
    }
    if (!watermark_packet_valid(request.packet)) {
        error_value = error(PipelineError::Domain::hidden, 22,
            L"hidden watermark packet is invalid");
        return false;
    }
    if (request.packet.algorithm == 0) {
        error_value = error(PipelineError::Domain::hidden, 23,
            L"hidden watermark algorithm id is zero");
        return false;
    }
    if (!embedder) {
        error_value = error(PipelineError::Domain::hidden, 24,
            L"hidden watermark requested but no blind watermark embedder is registered");
        return false;
    }
    if (!embedder->available()) {
        error_value = error(PipelineError::Domain::hidden, 25,
            L"hidden watermark embedder is not available");
        return false;
    }
    if (embedder->algorithm() != request.packet.algorithm) {
        error_value = error(PipelineError::Domain::unsupported, 26,
            L"hidden watermark algorithm is not provided by the registered embedder");
        return false;
    }
    PipelineError inner;
    const bool ok = embedder->embed(source, target, request.packet,
        request.key, inner);
    if (!ok) {
        error_value = inner.ok()
            ? error(PipelineError::Domain::hidden, 27,
                  L"hidden watermark embedder failed without a diagnostic")
            : inner;
        return false;
    }
    error_value = {};
    return true;
}

struct FramePipeline::Impl {
    FramePipelineConfig config;
    PipelineStats stats;
    BackendSelection selection;
    std::unique_ptr<detail::CaptureBackend> backend;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> running{false};
    std::uint64_t sequence = 0;
    double capture_sum = 0.0;
    double wait_sum = 0.0;
    double normalize_sum = 0.0;
    double composite_sum = 0.0;
    double hidden_sum = 0.0;
    double total_sum = 0.0;

    void record_timing(double capture_ms, double wait_ms, double normalize_ms,
        double composite_ms, double hidden_ms, double total_ms) {
        stats.last_capture_ms = capture_ms;
        stats.last_capture_wait_ms = wait_ms;
        stats.last_normalize_ms = normalize_ms;
        stats.last_composite_ms = composite_ms;
        stats.last_hidden_ms = hidden_ms;
        stats.last_pipeline_ms =
            capture_ms + normalize_ms + composite_ms + hidden_ms;
        stats.last_total_ms = total_ms;
        capture_sum += capture_ms;
        wait_sum += wait_ms;
        normalize_sum += normalize_ms;
        composite_sum += composite_ms;
        hidden_sum += hidden_ms;
        total_sum += total_ms;
        stats.max_total_ms = std::max(stats.max_total_ms, total_ms);
        const double frames = static_cast<double>(stats.frames);
        if (frames > 0.0) {
            stats.avg_capture_ms = capture_sum / frames;
            stats.avg_capture_wait_ms = wait_sum / frames;
            stats.avg_normalize_ms = normalize_sum / frames;
            stats.avg_composite_ms = composite_sum / frames;
            stats.avg_hidden_ms = hidden_sum / frames;
            stats.avg_pipeline_ms = stats.avg_capture_ms +
                stats.avg_normalize_ms + stats.avg_composite_ms +
                stats.avg_hidden_ms;
            stats.avg_total_ms = total_sum / frames;
        }
    }
};

FramePipeline::FramePipeline() : impl_(std::make_unique<Impl>()) {}
FramePipeline::~FramePipeline() { stop(); }

bool FramePipeline::running() const noexcept {
    return impl_->running.load();
}

const FramePipelineConfig& FramePipeline::config() const noexcept {
    return impl_->config;
}

const PipelineStats& FramePipeline::stats() const noexcept {
    return impl_->stats;
}

const BackendSelection& FramePipeline::backend_selection() const noexcept {
    return impl_->selection;
}

CaptureCapability FramePipeline::probe(CaptureBackendKind backend,
    const CaptureSource& source) {
    if (backend == CaptureBackendKind::wgc) return detail::probe_wgc(source);
    if (backend == CaptureBackendKind::bitblt) {
        return detail::probe_bitblt(source);
    }
    CaptureCapability capability;
    capability.backend = backend;
    capability.source = source.kind;
    capability.state = CaptureBackendState::unknown;
    capability.detail = L"automatic is not a concrete backend";
    return capability;
}

std::vector<CaptureCapability> FramePipeline::probe_all(
    const CaptureSource& source) {
    return {probe(CaptureBackendKind::wgc, source),
        probe(CaptureBackendKind::bitblt, source)};
}

bool FramePipeline::start(const FramePipelineConfig& config,
    PipelineError& error_value) {
    stop();
    if (config.source.kind == CaptureSourceKind::window &&
        !IsWindow(config.source.window)) {
        error_value = error(PipelineError::Domain::validation, 30,
            L"window capture source is not a valid HWND");
        return false;
    }
    if (config.source.kind == CaptureSourceKind::display) {
        const RECT rect = config.source.display_rect;
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            // A monitor handle or an empty rectangle is valid; a malformed
            // non-empty rectangle is not.
            if ((rect.left || rect.top || rect.right || rect.bottom) &&
                rect.right <= rect.left) {
                error_value = error(PipelineError::Domain::validation, 31,
                    L"display capture rectangle is invalid");
                return false;
            }
        }
    }
    if (config.capture_timeout_ms == 0 || config.target_fps == 0) {
        error_value = error(PipelineError::Domain::validation, 32,
            L"capture timeout and target fps must be non-zero");
        return false;
    }
    if (config.hidden.enabled && !config.hidden_embedder) {
        // Fail at start instead of reporting a successful chain that silently
        // drops the hidden payload.
        error_value = error(PipelineError::Domain::hidden, 33,
            L"hidden watermark requested but no blind watermark embedder is registered");
        return false;
    }
    if (config.watermark.enabled && config.watermark.layout == ExplicitLayout::none &&
        !config.watermark.evidence_block) {
        error_value = error(PipelineError::Domain::validation, 34,
            L"explicit watermark is enabled with layout none");
        return false;
    }

    impl_->config = config;
    impl_->stats = {};
    impl_->selection = {};
    impl_->sequence = 0;
    impl_->capture_sum = 0.0;
    impl_->wait_sum = 0.0;
    impl_->normalize_sum = 0.0;
    impl_->composite_sum = 0.0;
    impl_->hidden_sum = 0.0;
    impl_->total_sum = 0.0;
    impl_->cancelled = false;

    const CaptureBackendKind requested = config.preferred_backend;
    impl_->selection.requested = requested;
    std::vector<CaptureBackendKind> order;
    if (requested == CaptureBackendKind::automatic) {
        order = {CaptureBackendKind::wgc, CaptureBackendKind::bitblt};
    } else {
        order.push_back(requested);
        if (!config.require_preferred_backend) {
            if (requested == CaptureBackendKind::wgc) {
                order.push_back(CaptureBackendKind::bitblt);
            } else if (requested == CaptureBackendKind::bitblt) {
                order.push_back(CaptureBackendKind::wgc);
            }
        }
    }

    PipelineError last_error;
    for (CaptureBackendKind kind : order) {
        CaptureCapability capability = probe(kind, config.source);
        if (kind == requested) impl_->selection.requested_capability = capability;
        if (!capability.supported) {
            impl_->selection.attempts.push_back(
                std::wstring(capture_backend_kind_name(kind)) + L": " +
                capture_backend_state_name(capability.state) + L" (" +
                capability.detail + L")");
            last_error = error(PipelineError::Domain::unsupported, 35,
                std::wstring(capture_backend_kind_name(kind)) +
                    L" is not available: " + capability.detail,
                capability.hresult, capability.win32_error);
            if (config.require_preferred_backend && kind == requested) break;
            continue;
        }

        std::unique_ptr<detail::CaptureBackend> backend =
            kind == CaptureBackendKind::wgc ? detail::make_wgc_backend()
                                            : detail::make_bitblt_backend();
        if (!backend) {
            last_error = error(PipelineError::Domain::internal, 36,
                L"capture backend allocation failed");
            continue;
        }
        PipelineError start_error;
        if (!backend->start(config.source, start_error)) {
            impl_->selection.attempts.push_back(
                std::wstring(capture_backend_kind_name(kind)) + L": start failed (" +
                start_error.detail + L")");
            last_error = start_error;
            if (config.require_preferred_backend && kind == requested) break;
            continue;
        }
        impl_->backend = std::move(backend);
        impl_->selection.selected = kind;
        impl_->selection.selected_capability = capability;
        impl_->selection.fallback = requested != CaptureBackendKind::automatic &&
            kind != requested;
        impl_->selection.attempts.push_back(
            std::wstring(capture_backend_kind_name(kind)) + L": started");
        impl_->running = true;
        error_value = {};
        return true;
    }

    impl_->selection.selected = CaptureBackendKind::automatic;
    impl_->running = false;
    error_value = last_error.ok()
        ? error(PipelineError::Domain::capture, 37,
              L"no capture backend could be started")
        : last_error;
    return false;
}

FramePipeline::NextResult FramePipeline::next(FramePair& out,
    std::uint32_t timeout_ms, PipelineError& error_value) {
    out = {};
    if (!impl_->running || !impl_->backend) {
        error_value = error(PipelineError::Domain::validation, 40,
            L"frame pipeline is not running");
        return NextResult::failed;
    }
    if (impl_->cancelled.load()) {
        error_value = error(PipelineError::Domain::cancelled, 41,
            L"frame pipeline was cancelled");
        return NextResult::cancelled;
    }

    const auto capture_begin = std::chrono::steady_clock::now();
    Frame captured;
    PipelineError capture_error;
    if (!impl_->backend->next(captured, timeout_ms, capture_error)) {
        if (impl_->cancelled.load()) {
            error_value = error(PipelineError::Domain::cancelled, 42,
                L"frame pipeline was cancelled");
            return NextResult::cancelled;
        }
        if (capture_error.ok()) {
            error_value = {};
            return NextResult::timeout;
        }
        ++impl_->stats.failed_frames;
        error_value = capture_error;
        return NextResult::failed;
    }
    const auto capture_end = std::chrono::steady_clock::now();

    if (!captured.valid() || captured.image.width() == 0 ||
        captured.image.height() == 0) {
        ++impl_->stats.failed_frames;
        error_value = error(PipelineError::Domain::capture, 43,
            L"capture backend returned an empty frame");
        return NextResult::failed;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(captured.image.width()) *
        captured.image.height() * bgra8_bytes_per_pixel;
    if (bytes > impl_->config.max_frame_bytes) {
        ++impl_->stats.failed_frames;
        error_value = error(PipelineError::Domain::budget, 44,
            L"capture frame exceeds the configured byte budget");
        return NextResult::failed;
    }

    const auto normalize_begin = std::chrono::steady_clock::now();
    ImageBuffer normalized;
    const std::size_t tight_stride =
        static_cast<std::size_t>(captured.image.width()) * bgra8_bytes_per_pixel;
    if (captured.image.valid() && captured.image.stride() == tight_stride) {
        normalized = std::move(captured.image);
    } else {
        PipelineError normalize_error;
        if (!normalize_frame(captured.image, normalized, normalize_error)) {
            ++impl_->stats.failed_frames;
            error_value = normalize_error;
            return NextResult::failed;
        }
    }
    // Windows.Graphics.Capture produces opaque B8G8R8A8 frames.  GDI BitBlt
    // produces alpha=0 DIB bytes, so only that backend needs the alpha pass.
    if (impl_->config.force_opaque &&
        impl_->selection.selected == CaptureBackendKind::bitblt) {
        force_opaque(normalized);
    }
    if (impl_->config.window_client_only && impl_->config.source.kind==CaptureSourceKind::window) {
        error_value={};
        const HWND target=impl_->config.source.window;
        RECT client{}; POINT origin{};
        if (!GetClientRect(target,&client) || !ClientToScreen(target,&origin)) return NextResult::timeout;
        const int w=client.right, h=client.bottom;
        if (w<=0 || h<=0) return NextResult::timeout;
        if (normalized.width()!=static_cast<unsigned>(w) || normalized.height()!=static_cast<unsigned>(h)) {
            RECT bounds{};
            const auto matches=[&] { return std::abs((bounds.right-bounds.left)-static_cast<int>(normalized.width()))<=2
                && std::abs((bounds.bottom-bounds.top)-static_cast<int>(normalized.height()))<=2; };
            if (FAILED(DwmGetWindowAttribute(target,9,&bounds,sizeof(bounds))) || !matches()) {
                if (!GetWindowRect(target,&bounds) || !matches()) return NextResult::timeout;
            }
            const int x=origin.x-bounds.left, y=origin.y-bounds.top;
            if (x<0 || y<0 || x+w>static_cast<int>(normalized.width()) || y+h>static_cast<int>(normalized.height()))
                return NextResult::timeout;
            ImageBuffer cropped(static_cast<unsigned>(w),static_cast<unsigned>(h),0,normalized.alpha());
            for (int row=0;row<h;++row)
                std::memcpy(cropped.bytes().data()+row*cropped.stride(),
                    normalized.bytes().data()+(row+y)*normalized.stride()+x*4,static_cast<std::size_t>(w)*4);
            normalized=std::move(cropped);
        }
    }
    const auto normalize_end = std::chrono::steady_clock::now();

    const auto composite_begin = std::chrono::steady_clock::now();
    ImageBuffer watermarked = normalized;
    bool has_watermark = false;
    if (impl_->config.watermark.enabled ||
        impl_->config.watermark.evidence_block) {
        PipelineError watermark_error;
        const Window target = window_from_metadata(impl_->config.target);
        if (!compose_explicit_watermark(watermarked, target,
                impl_->config.watermark, watermark_error)) {
            ++impl_->stats.failed_frames;
            error_value = watermark_error;
            return NextResult::failed;
        }
        has_watermark = true;
    }
    const auto composite_end = std::chrono::steady_clock::now();

    const auto hidden_begin = std::chrono::steady_clock::now();
    bool hidden_embedded = false;
    bool hidden_skipped = false;
    if (impl_->config.hidden.enabled) {
        PipelineError hidden_error;
        MutableImageView target = watermarked.mutable_view();
        if (!apply_blind_watermark(watermarked.view(), target,
                impl_->config.hidden, impl_->config.hidden_embedder,
                hidden_error)) {
            ++impl_->stats.failed_frames;
            ++impl_->stats.hidden_failures;
            error_value = hidden_error;
            return NextResult::failed;
        }
        hidden_embedded = true;
        ++impl_->stats.hidden_embedded;
    } else {
        hidden_skipped = true;
        ++impl_->stats.hidden_skipped;
    }
    const auto hidden_end = std::chrono::steady_clock::now();
    const auto total_end = hidden_end;

    const std::uint64_t sequence = ++impl_->sequence;
    const std::uint64_t tick = captured.capture_tick_ms != 0
        ? captured.capture_tick_ms
        : GetTickCount64();
    const bool blank = is_blank_frame(normalized);
    if (impl_->config.include_clean) {
        out.clean.image = std::move(normalized);
    }
    out.clean.sequence = sequence;
    out.clean.capture_tick_ms = tick;
    out.watermarked.image = std::move(watermarked);
    out.watermarked.sequence = sequence;
    out.watermarked.capture_tick_ms = tick;
    out.has_watermark = has_watermark;
    out.hidden_embedded = hidden_embedded;
    out.hidden_skipped = hidden_skipped;

    ++impl_->stats.frames;
    if (out.clean.image.valid()) ++impl_->stats.clean_frames;
    if (out.watermarked.image.valid()) ++impl_->stats.watermarked_frames;
    if (blank) ++impl_->stats.blank_frames;
    impl_->stats.frame_bytes = bytes;
    const std::uint64_t buffered =
        bytes * (impl_->config.include_clean ? 2ull : 1ull);
    impl_->stats.peak_buffered_bytes =
        std::max(impl_->stats.peak_buffered_bytes, buffered);
    if (impl_->stats.first_tick_ms == 0) impl_->stats.first_tick_ms = tick;
    impl_->stats.last_tick_ms = tick;
    if (impl_->stats.last_tick_ms > impl_->stats.first_tick_ms) {
        const double seconds = static_cast<double>(
            impl_->stats.last_tick_ms - impl_->stats.first_tick_ms) / 1000.0;
        impl_->stats.fps = seconds > 0.0
            ? static_cast<double>(impl_->stats.frames) / seconds
            : 0.0;
    }
    const double capture_wall_ms = millis(capture_begin, capture_end);
    const double capture_processing_ms =
        impl_->backend ? impl_->backend->last_processing_ms() : 0.0;
    const double capture_ms = capture_processing_ms > 0.0
        ? capture_processing_ms : capture_wall_ms;
    const double capture_wait_ms =
        std::max(0.0, capture_wall_ms - capture_ms);
    impl_->record_timing(capture_ms, capture_wait_ms,
        millis(normalize_begin, normalize_end),
        millis(composite_begin, composite_end),
        millis(hidden_begin, hidden_end),
        millis(capture_begin, total_end));
    error_value = {};
    return NextResult::frame;
}

void FramePipeline::cancel() { impl_->cancelled = true; }

void FramePipeline::stop() {
    impl_->cancelled = true;
    if (impl_->backend) {
        impl_->backend->stop();
        impl_->backend.reset();
    }
    impl_->running = false;
}

}  // namespace av
