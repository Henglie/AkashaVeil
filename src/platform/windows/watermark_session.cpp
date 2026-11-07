// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/watermark_session.h"

#include "av/watermark/dct.h"
#include "av/watermark/dwt.h"
#include "av/watermark/robust.h"

#include <algorithm>
#include <climits>
#include <mutex>
#include <utility>

namespace av {
namespace {

CompositionError make_error(CompositionError::Domain domain,
    std::uint32_t code, std::wstring detail) {
    CompositionError value;
    value.domain = domain;
    value.code = code;
    value.detail = std::move(detail);
    return value;
}

Mark mark_from_layout(ExplicitLayout layout) {
    switch (layout) {
    case ExplicitLayout::tiled: return Mark::tiled;
    case ExplicitLayout::center: return Mark::center;
    case ExplicitLayout::corner: return Mark::corner;
    case ExplicitLayout::none: break;
    }
    return Mark::none;
}

bool validate_explicit(const WatermarkPlan& plan, CompositionError& error) {
    const ExplicitWatermarkSpec& spec = plan.explicit_layer;
    if (!spec.enabled) return true;
    if (spec.layout == ExplicitLayout::none) {
        error = make_error(CompositionError::Domain::validation, 1,
            L"explicit watermark is enabled but its form is none");
        return false;
    }
    if (spec.dpi != 0 && (spec.dpi < 48 || spec.dpi > 960)) {
        error = make_error(CompositionError::Domain::validation, 2,
            L"explicit watermark DPI must be 0 or 48..960");
        return false;
    }
    Effects effects;
    effects.cover = Cover::none;
    effects.mark = mark_from_layout(spec.layout);
    effects.capture_only = plan.visibility != WatermarkVisibility::screen_and_capture;
    effects.text = spec.text;
    effects.label = spec.label;
    effects.tag = spec.tag;
    effects.opacity = spec.opacity;
    effects.font_size = spec.font_size;
    effects.spacing = spec.spacing;
    effects.foreground = spec.foreground;
    effects.background = spec.background;
    if (!valid_effects(effects)) {
        error = make_error(CompositionError::Domain::validation, 3,
            L"explicit watermark text/form/opacity/font/spacing/colour is outside the product range");
        return false;
    }
    return true;
}

const BlindWatermarkEmbedder* find_embedder(
    const HiddenEmbedderSet& embedders, std::uint16_t algorithm) {
    if (algorithm == kHiddenAlgorithmDctPairQim) return embedders.dct.get();
    if (algorithm == kHiddenAlgorithmDwtLh3Qim) return embedders.dwt.get();
    if (algorithm == kHiddenAlgorithmRobustDct) return embedders.robust.get();
    return nullptr;
}

bool validate_hidden(const WatermarkPlan& plan,
    const HiddenEmbedderSet& embedders, CompositionError& error) {
    const HiddenWatermarkOptions& hidden = plan.hidden_layer;
    if (!hidden.enabled) return true;
    if (hidden.algorithm != kHiddenAlgorithmDctPairQim &&
        hidden.algorithm != kHiddenAlgorithmDwtLh3Qim &&
        hidden.algorithm != kHiddenAlgorithmRobustDct) {
        error = make_error(CompositionError::Domain::unsupported, 10,
            L"hidden watermark algorithm must be DCT (1), DWT (2) or R-DCT (3)");
        return false;
    }
    if (hidden.packet.format_version != 1 ||
        hidden.packet.algorithm != hidden.algorithm) {
        error = make_error(CompositionError::Domain::validation, 11,
            L"hidden watermark packet version/algorithm does not match the selected protocol V1 algorithm");
        return false;
    }
    if (hidden.packet.payload.empty()) {
        error = make_error(CompositionError::Domain::validation, 12,
            L"hidden watermark payload must contain 1..32 bytes");
        return false;
    }
    const std::size_t payload_max = std::min(plan.limits.max_hidden_payload,
        kHiddenPayloadMax);
    if (hidden.packet.payload.size() > payload_max) {
        error = make_error(CompositionError::Domain::validation, 13,
            L"hidden watermark payload exceeds the configured 32-byte V1 limit");
        return false;
    }
    if (!hidden.packet.parameters.empty()) {
        error = make_error(CompositionError::Domain::unsupported, 14,
            L"hidden watermark protocol V1 does not accept user parameters in the packet");
        return false;
    }
    if (hidden.packet.flags != 0 || hidden.packet.error_correction != 0 ||
        hidden.packet.sync_marker != 0 || hidden.packet.crc32 != 0) {
        error = make_error(CompositionError::Domain::unsupported, 15,
            L"hidden watermark flags/error-correction/sync/CRC are fixed by protocol V1 and must be zero in the plan");
        return false;
    }
    const std::size_t key_max = std::min(plan.limits.max_hidden_key,
        kHiddenKeyMax);
    if (hidden.key.empty() || hidden.key.size() > key_max) {
        error = make_error(CompositionError::Domain::validation, 16,
            L"hidden watermark key must be 1..128 bytes");
        return false;
    }
    if (!watermark_packet_valid(hidden.packet)) {
        error = make_error(CompositionError::Domain::validation, 17,
            L"hidden watermark packet is not a valid control-layer packet");
        return false;
    }
    const BlindWatermarkEmbedder* embedder = find_embedder(embedders,
        hidden.algorithm);
    if (!embedder || !embedder->available()) {
        error = make_error(CompositionError::Domain::unsupported, 18,
            L"selected hidden embedder is not registered; refusing to silently drop the hidden payload");
        return false;
    }
    return true;
}

bool validate_route(const WatermarkPlan& plan,
    const CaptureRouteCapabilities& routes, CompositionError& error) {
    if (plan.visibility == WatermarkVisibility::screen_and_capture) {
        if (plan.capture_only_route != CaptureOnlyRoute::none) {
            error = make_error(CompositionError::Domain::validation, 20,
                L"screen-and-capture mode must not select a capture-only route");
            return false;
        }
        return true;
    }
    if (plan.capture_only_route == CaptureOnlyRoute::display_capture_a) {
        if (!routes.display_capture_a) {
            error = make_error(CompositionError::Domain::unsupported, 21,
                routes.display_capture_detail.empty()
                    ? L"capture-only A route is unavailable (WDA_EXCLUDEFROMCAPTURE was not proven); no silent fallback"
                    : L"capture-only A route is unavailable: " +
                          routes.display_capture_detail);
            return false;
        }
        return true;
    }
    if (plan.capture_only_route == CaptureOnlyRoute::proxy_source_b) {
        if (!routes.proxy_source_b) {
            error = make_error(CompositionError::Domain::unsupported, 22,
                routes.proxy_source_detail.empty()
                    ? L"capture-only B proxy-source route is unavailable; no silent fallback"
                    : L"capture-only B proxy-source route is unavailable: " +
                          routes.proxy_source_detail);
            return false;
        }
        return true;
    }
    error = make_error(CompositionError::Domain::validation, 23,
        L"capture-only mode requires the A display route or the B proxy-source route");
    return false;
}

CompositionError map_pipeline_error(const PipelineError& source) {
    CompositionError result;
    result.code = source.code;
    result.detail = source.detail;
    switch (source.domain) {
    case PipelineError::Domain::none: result.domain = CompositionError::Domain::none; break;
    case PipelineError::Domain::validation: result.domain = CompositionError::Domain::validation; break;
    case PipelineError::Domain::unsupported:
    case PipelineError::Domain::permission: result.domain = CompositionError::Domain::unsupported; break;
    case PipelineError::Domain::budget: result.domain = CompositionError::Domain::budget; break;
    case PipelineError::Domain::cancelled: result.domain = CompositionError::Domain::cancelled; break;
    case PipelineError::Domain::target_lost: result.domain = CompositionError::Domain::target; break;
    case PipelineError::Domain::hidden: result.domain = CompositionError::Domain::hidden; break;
    case PipelineError::Domain::watermark:
    case PipelineError::Domain::capture:
    case PipelineError::Domain::internal:
    default: result.domain = CompositionError::Domain::internal; break;
    }
    return result;
}

HiddenExtractionStatus map_extraction_status(av::watermark::Status status) {
    switch (status) {
    case av::watermark::Status::success: return HiddenExtractionStatus::success;
    case av::watermark::Status::not_detected: return HiddenExtractionStatus::not_detected;
    case av::watermark::Status::corrupted: return HiddenExtractionStatus::corrupted;
    case av::watermark::Status::unsupported_version: return HiddenExtractionStatus::unsupported_version;
    case av::watermark::Status::unsupported_algorithm: return HiddenExtractionStatus::unsupported_algorithm;
    case av::watermark::Status::wrong_key: return HiddenExtractionStatus::wrong_key;
    case av::watermark::Status::invalid_argument: return HiddenExtractionStatus::invalid_argument;
    case av::watermark::Status::cancelled: return HiddenExtractionStatus::cancelled;
    case av::watermark::Status::payload_too_large: return HiddenExtractionStatus::payload_too_large;
    case av::watermark::Status::capacity_exceeded: return HiddenExtractionStatus::capacity_exceeded;
    }
    return HiddenExtractionStatus::unavailable;
}

}  // namespace

const wchar_t* hidden_algorithm_name(std::uint16_t algorithm) noexcept {
    switch (algorithm) {
    case kHiddenAlgorithmDctPairQim: return L"dct_qim";
    case kHiddenAlgorithmDwtLh3Qim: return L"dwt_lh3_qim";
    case kHiddenAlgorithmRobustDct: return L"robust_dct";
    default: return L"none";
    }
}

const wchar_t* composition_error_domain_name(
    CompositionError::Domain domain) noexcept {
    switch (domain) {
    case CompositionError::Domain::none: return L"none";
    case CompositionError::Domain::validation: return L"validation";
    case CompositionError::Domain::unsupported: return L"unsupported";
    case CompositionError::Domain::hidden: return L"hidden";
    case CompositionError::Domain::budget: return L"budget";
    case CompositionError::Domain::cancelled: return L"cancelled";
    case CompositionError::Domain::target: return L"target";
    case CompositionError::Domain::state: return L"state";
    case CompositionError::Domain::internal: return L"internal";
    }
    return L"unknown";
}

const wchar_t* hidden_extraction_status_name(
    HiddenExtractionStatus status) noexcept {
    switch (status) {
    case HiddenExtractionStatus::success: return L"success";
    case HiddenExtractionStatus::not_detected: return L"not_detected";
    case HiddenExtractionStatus::corrupted: return L"corrupted";
    case HiddenExtractionStatus::unsupported_version: return L"unsupported_version";
    case HiddenExtractionStatus::unsupported_algorithm: return L"unsupported_algorithm";
    case HiddenExtractionStatus::wrong_key: return L"wrong_key";
    case HiddenExtractionStatus::invalid_argument: return L"invalid_argument";
    case HiddenExtractionStatus::cancelled: return L"cancelled";
    case HiddenExtractionStatus::payload_too_large: return L"payload_too_large";
    case HiddenExtractionStatus::capacity_exceeded: return L"capacity_exceeded";
    case HiddenExtractionStatus::unavailable: return L"unavailable";
    }
    return L"unavailable";
}

HiddenWatermarkOptions make_hidden_watermark(std::uint16_t algorithm,
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::uint8_t>& key) {
    HiddenWatermarkOptions options;
    options.enabled = true;
    options.algorithm = algorithm;
    options.packet = make_watermark_packet(algorithm, payload);
    options.key = key;
    return options;
}



bool ComposedFrame::valid() const noexcept {
    return local.valid() && capture.valid();
}

const wchar_t* watermark_session_state_name(
    WatermarkSessionState state) noexcept {
    switch (state) {
    case WatermarkSessionState::idle: return L"idle";
    case WatermarkSessionState::configured: return L"configured";
    case WatermarkSessionState::running: return L"running";
    case WatermarkSessionState::cancelling: return L"cancelling";
    case WatermarkSessionState::stopped: return L"stopped";
    case WatermarkSessionState::target_lost: return L"target_lost";
    case WatermarkSessionState::failed: return L"failed";
    }
    return L"unknown";
}

struct WatermarkFrameComposer::Impl {
    WatermarkPlan plan;
    HiddenEmbedderSet embedders;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::uint64_t generation = 0;
    bool configured = false;
};

WatermarkFrameComposer::WatermarkFrameComposer() : impl_(std::make_unique<Impl>()) {
    impl_->cancel = std::make_shared<std::atomic<bool>>(false);
    impl_->embedders.dct = make_dct_embedder(impl_->cancel);
    impl_->embedders.dwt = make_dwt_embedder(impl_->cancel);
    impl_->embedders.robust = make_robust_embedder(impl_->cancel);
}

WatermarkFrameComposer::WatermarkFrameComposer(HiddenEmbedderSet embedders,
    std::shared_ptr<std::atomic<bool>> cancel)
    : impl_(std::make_unique<Impl>()) {
    impl_->cancel = cancel ? std::move(cancel)
                           : std::make_shared<std::atomic<bool>>(false);
    impl_->embedders = std::move(embedders);
}

WatermarkFrameComposer::~WatermarkFrameComposer() = default;
WatermarkFrameComposer::WatermarkFrameComposer(WatermarkFrameComposer&&) noexcept = default;
WatermarkFrameComposer& WatermarkFrameComposer::operator=(
    WatermarkFrameComposer&&) noexcept = default;

bool WatermarkFrameComposer::validate_plan(const WatermarkPlan& plan,
    const HiddenEmbedderSet& embedders, CompositionError& error) {
    const bool explicit_enabled = plan.explicit_layer.enabled;
    const bool hidden_enabled = plan.hidden_layer.enabled;
    if (!explicit_enabled && !hidden_enabled) {
        error = make_error(CompositionError::Domain::validation, 30,
            L"watermark plan enables neither an explicit nor a hidden watermark");
        return false;
    }
    switch (plan.visibility) {
    case WatermarkVisibility::screen_and_capture:
        if (plan.capture_only_route != CaptureOnlyRoute::none) {
            error = make_error(CompositionError::Domain::validation, 37,
                L"screen-and-capture mode must not select a capture-only route");
            return false;
        }
        break;
    case WatermarkVisibility::capture_only_explicit:
        if (!explicit_enabled || hidden_enabled) {
            error = make_error(CompositionError::Domain::validation, 31,
                L"capture-only explicit mode requires explicit on and hidden off");
            return false;
        }
        break;
    case WatermarkVisibility::capture_only_hidden:
        if (explicit_enabled || !hidden_enabled) {
            error = make_error(CompositionError::Domain::validation, 32,
                L"capture-only hidden mode requires hidden on and explicit off");
            return false;
        }
        break;
    case WatermarkVisibility::capture_only_explicit_hidden:
        if (!explicit_enabled || !hidden_enabled) {
            error = make_error(CompositionError::Domain::validation, 33,
                L"capture-only overlay mode requires both explicit and hidden on");
            return false;
        }
        break;
    default:
        error = make_error(CompositionError::Domain::validation, 34,
            L"unknown watermark visibility mode");
        return false;
    }
    if (plan.visibility != WatermarkVisibility::screen_and_capture &&
        plan.capture_only_route == CaptureOnlyRoute::none) {
        error = make_error(CompositionError::Domain::validation, 35,
            L"capture-only mode requires an A or B route; a surrogate/cover is not an acceptable substitute");
        return false;
    }
    if (plan.limits.max_pixels == 0 || plan.limits.max_frame_bytes == 0 ||
        plan.limits.max_hidden_payload == 0 || plan.limits.max_hidden_key == 0) {
        error = make_error(CompositionError::Domain::validation, 36,
            L"composition budget limits must be non-zero");
        return false;
    }
    if (!validate_explicit(plan, error)) return false;
    if (!validate_hidden(plan, embedders, error)) return false;
    error = {};
    return true;
}

bool WatermarkFrameComposer::configure(const WatermarkPlan& plan,
    CompositionError& error) {
    if (!validate_plan(plan, impl_->embedders, error)) return false;
    impl_->plan = plan;
    impl_->configured = true;
    ++impl_->generation;
    impl_->cancel->store(false, std::memory_order_relaxed);
    error = {};
    return true;
}

std::shared_ptr<BlindWatermarkEmbedder> WatermarkFrameComposer::embedder_for(
    std::uint16_t algorithm) const {
    if (algorithm == kHiddenAlgorithmDctPairQim) return impl_->embedders.dct;
    if (algorithm == kHiddenAlgorithmDwtLh3Qim) return impl_->embedders.dwt;
    if (algorithm == kHiddenAlgorithmRobustDct) return impl_->embedders.robust;
    return {};
}

bool WatermarkFrameComposer::compose(const ImageBuffer& source,
    const Window& target, ComposedFrame& out, CompositionError& error) {
    out = {};
    if (!impl_->configured) {
        error = make_error(CompositionError::Domain::state, 40,
            L"watermark composer is not configured");
        return false;
    }
    if (impl_->cancel && impl_->cancel->load(std::memory_order_relaxed)) {
        error = make_error(CompositionError::Domain::cancelled, 41,
            L"watermark composition was cancelled");
        return false;
    }
    if (!source.valid()) {
        error = make_error(CompositionError::Domain::validation, 42,
            L"source frame is not a valid BGRA8 image");
        return false;
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(source.width()) * source.height();
    if (pixels > impl_->plan.limits.max_pixels) {
        error = make_error(CompositionError::Domain::budget, 43,
            L"source frame exceeds the composition pixel budget");
        return false;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(source.stride()) * source.height();
    if (bytes > impl_->plan.limits.max_frame_bytes) {
        error = make_error(CompositionError::Domain::budget, 44,
            L"source frame exceeds the composition byte budget");
        return false;
    }

    const bool explicit_enabled = impl_->plan.explicit_layer.enabled;
    const bool hidden_enabled = impl_->plan.hidden_layer.enabled;

    auto apply_explicit = [&](ImageBuffer& frame) -> bool {
        if (!explicit_enabled) return true;
        PipelineError pipeline_error;
        if (!compose_explicit_watermark(frame, target,
                impl_->plan.explicit_layer, pipeline_error)) {
            error = map_pipeline_error(pipeline_error);
            if (error.ok()) {
                error = make_error(CompositionError::Domain::internal, 45,
                    L"explicit watermark composition failed without a diagnostic");
            }
            return false;
        }
        return true;
    };

    auto apply_hidden = [&](ImageBuffer& frame) -> bool {
        if (!hidden_enabled) return true;
        auto embedder = embedder_for(impl_->plan.hidden_layer.algorithm);
        if (!embedder) {
            error = make_error(CompositionError::Domain::hidden, 46,
                L"hidden watermark embedder disappeared after configuration");
            return false;
        }
        BlindWatermarkRequest request;
        request.enabled = true;
        request.packet = impl_->plan.hidden_layer.packet;
        request.key = impl_->plan.hidden_layer.key;
        PipelineError pipeline_error;
        if (!apply_blind_watermark(frame.view(), frame.mutable_view(),
                request, embedder, pipeline_error)) {
            error = map_pipeline_error(pipeline_error);
            if (error.ok()) {
                error = make_error(CompositionError::Domain::hidden, 47,
                    L"hidden watermark embedding failed without a diagnostic; composed frame discarded");
            }
            return false;
        }
        return true;
    };

    ComposedFrame composed;
    if (impl_->plan.visibility == WatermarkVisibility::screen_and_capture) {
        ImageBuffer frame = source;
        if (!apply_explicit(frame)) return false;
        if (!apply_hidden(frame)) return false;
        composed.local = frame;
        composed.capture = std::move(frame);
        composed.local_explicit = explicit_enabled;
        composed.capture_explicit = explicit_enabled;
        composed.local_hidden = hidden_enabled;
        composed.capture_hidden = hidden_enabled;
        composed.local_original_preserved = false;
        composed.capture_original_preserved = false;
    } else {
        composed.local = source;
        ImageBuffer capture = source;
        if (!apply_explicit(capture)) return false;
        if (!apply_hidden(capture)) return false;
        composed.capture = std::move(capture);
        composed.local_explicit = false;
        composed.capture_explicit = explicit_enabled;
        composed.local_hidden = false;
        composed.capture_hidden = hidden_enabled;
        composed.local_original_preserved = true;
        composed.capture_original_preserved = true;
    }
    composed.generation = impl_->generation;
    if (!composed.valid()) {
        error = make_error(CompositionError::Domain::internal, 48,
            L"composed frame is empty");
        return false;
    }
    out = std::move(composed);
    error = {};
    return true;
}

void WatermarkFrameComposer::reset() {
    impl_->plan = {};
    impl_->configured = false;
    impl_->cancel->store(false, std::memory_order_relaxed);
    ++impl_->generation;
}

void WatermarkFrameComposer::request_cancel() {
    if (impl_->cancel) impl_->cancel->store(true, std::memory_order_relaxed);
}

void WatermarkFrameComposer::clear_cancel() {
    if (impl_->cancel) impl_->cancel->store(false, std::memory_order_relaxed);
}

bool WatermarkFrameComposer::configured() const noexcept {
    return impl_->configured;
}

std::uint64_t WatermarkFrameComposer::generation() const noexcept {
    return impl_->generation;
}

const WatermarkPlan& WatermarkFrameComposer::plan() const noexcept {
    return impl_->plan;
}

bool WatermarkFrameComposer::extract(const ImageBuffer& image,
    std::uint16_t algorithm, const std::vector<std::uint8_t>& key,
    HiddenExtraction& result, CompositionError& error) {
    result = {};
    result.algorithm = algorithm;
    if (!valid_image_view(image.view())) {
        error = make_error(CompositionError::Domain::validation, 60,
            L"extraction source is not a valid BGRA8 image");
        result.status = HiddenExtractionStatus::invalid_argument;
        result.detail = L"invalid image";
        return false;
    }
    if (algorithm != kHiddenAlgorithmDctPairQim &&
        algorithm != kHiddenAlgorithmDwtLh3Qim &&
        algorithm != kHiddenAlgorithmRobustDct) {
        error = make_error(CompositionError::Domain::unsupported, 61,
            L"extraction algorithm must be DCT (1), DWT (2) or R-DCT (3)");
        result.status = HiddenExtractionStatus::unsupported_algorithm;
        result.detail = L"unsupported algorithm";
        return false;
    }
    if (key.empty() || key.size() > kHiddenKeyMax) {
        error = make_error(CompositionError::Domain::validation, 62,
            L"extraction key must be 1..128 bytes");
        result.status = HiddenExtractionStatus::invalid_argument;
        result.detail = L"invalid key length";
        return false;
    }

    if (algorithm == kHiddenAlgorithmDctPairQim || algorithm == kHiddenAlgorithmRobustDct) {
        av::watermark::dct::ImageView view;
        view.width = image.width();
        view.height = image.height();
        view.stride = image.stride();
        view.pixels = image.bytes().data();
        av::watermark::dct::ExtractOptions options;
        options.search_pixel_phase = algorithm == kHiddenAlgorithmRobustDct;
        const auto extract = algorithm == kHiddenAlgorithmRobustDct
            ? &av::watermark::robust::extract : &av::watermark::dct::extract;
        const auto extracted = extract(view,
            key.data(), key.size(), options);
        result.status = map_extraction_status(extracted.status);
        result.algorithm = algorithm;
        result.corrected_bytes = extracted.corrected_bytes;
        result.sync_score = static_cast<unsigned>(
            std::max(0, extracted.sync_score));
        result.phase_x = extracted.phase_x;
        result.phase_y = extracted.phase_y;
        result.tile_x = extracted.tile_x;
        result.tile_y = extracted.tile_y;
        if (extracted.status == av::watermark::Status::success) {
            result.payload.assign(extracted.payload,
                extracted.payload + extracted.payload_length);
            result.detail = L"DCT payload recovered";
            error = {};
            return true;
        }
        result.detail = hidden_extraction_status_name(result.status);
        error = make_error(CompositionError::Domain::hidden, 63,
            L"DCT extraction did not return a validated payload");
        return false;
    }

    av::watermark::ImageView view;
    view.pixels = image.bytes().data();
    view.width = static_cast<int>(image.width());
    view.height = static_cast<int>(image.height());
    view.stride = static_cast<int>(image.stride());
    view.format = av::watermark::PixelFormat::bgra8;
    av::watermark::DwtParameters parameters;
    av::watermark::WatermarkPosition position;
    av::watermark::DwtExtractResult extracted;
    const auto status = av::watermark::dwt_extract(view, key.data(),
        key.size(), parameters, position, extracted);
    result.status = map_extraction_status(status);
    result.algorithm = kHiddenAlgorithmDwtLh3Qim;
    result.corrected_bytes = extracted.corrected_bytes;
    result.sync_score = extracted.sync_score;
    result.phase_x = extracted.phase_x;
    result.phase_y = extracted.phase_y;
    result.tile_x = extracted.tile_x;
    result.tile_y = extracted.tile_y;
    if (status == av::watermark::Status::success) {
        result.payload.assign(extracted.payload,
            extracted.payload + extracted.payload_length);
        result.detail = L"DWT payload recovered";
        error = {};
        return true;
    }
    result.detail = hidden_extraction_status_name(result.status);
    error = make_error(CompositionError::Domain::hidden, 64,
        L"DWT extraction did not return a validated payload");
    return false;
}

struct WatermarkSession::Impl {
    mutable std::mutex mutex;
    WatermarkFrameComposer composer;
    WatermarkPlan plan;
    CaptureRouteCapabilities routes;
    WatermarkSessionState state = WatermarkSessionState::idle;
    std::uint64_t generation = 0;
    WatermarkSessionStats stats;

    struct Slot {
        TargetKey key;
        Window window;
        WatermarkTargetState state = WatermarkTargetState::active;
        std::uint64_t generation = 0;
        std::uint64_t frames = 0;
        std::uint64_t failures = 0;
        bool watermark_ready = false;
        bool explicit_local = false;
        bool explicit_capture = false;
        bool hidden_local = false;
        bool hidden_capture = false;
        CompositionError last_error;
    };
    std::vector<Slot> slots;

    Slot* find(const TargetKey& key) {
        for (auto& slot : slots) if (slot.key == key) return &slot;
        return nullptr;
    }
    const Slot* find(const TargetKey& key) const {
        for (const auto& slot : slots) if (slot.key == key) return &slot;
        return nullptr;
    }
    void bump_epoch() {
        ++generation;
        for (auto& slot : slots) {
            slot.generation = generation;
            slot.watermark_ready = false;
            slot.explicit_local = false;
            slot.explicit_capture = false;
            slot.hidden_local = false;
            slot.hidden_capture = false;
        }
    }
    WatermarkTargetStatus status_for(const Slot* slot, const TargetKey& key) const {
        WatermarkTargetStatus status;
        status.key = key;
        if (!slot) {
            status.attached = false;
            status.state = WatermarkTargetState::lost;
            status.last_error = make_error(CompositionError::Domain::target, 70,
                L"target is not attached to this watermark session");
            return status;
        }
        status.attached = true;
        status.state = slot->state;
        status.generation = slot->generation;
        status.frames = slot->frames;
        status.failures = slot->failures;
        status.watermark_ready = slot->watermark_ready;
        status.explicit_local = slot->explicit_local;
        status.explicit_capture = slot->explicit_capture;
        status.hidden_local = slot->hidden_local;
        status.hidden_capture = slot->hidden_capture;
        status.last_error = slot->last_error;
        return status;
    }
};

WatermarkSession::WatermarkSession() : impl_(std::make_unique<Impl>()) {}
WatermarkSession::WatermarkSession(HiddenEmbedderSet embedders)
    : impl_(std::make_unique<Impl>()) {
    impl_->composer = WatermarkFrameComposer(std::move(embedders));
}
WatermarkSession::~WatermarkSession() = default;

bool WatermarkSession::configure(const WatermarkPlan& plan,
    const CaptureRouteCapabilities& routes, CompositionError& error) {
    if (!validate_route(plan, routes, error)) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->composer.configure(plan, error)) return false;
    impl_->plan = plan;
    impl_->routes = routes;
    ++impl_->generation;
    if (impl_->state == WatermarkSessionState::running) {
        ++impl_->stats.mode_switches;
        for (auto& slot : impl_->slots) {
            slot.generation = impl_->generation;
            slot.watermark_ready = false;
            slot.explicit_local = false;
            slot.explicit_capture = false;
            slot.hidden_local = false;
            slot.hidden_capture = false;
        }
    } else {
        impl_->slots.clear();
        impl_->state = WatermarkSessionState::configured;
    }
    error = {};
    return true;
}

bool WatermarkSession::start(CompositionError& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state == WatermarkSessionState::running) {
        error = {};
        return true;
    }
    if (impl_->state != WatermarkSessionState::configured) {
        error = make_error(CompositionError::Domain::state, 80,
            L"watermark session must be configured before start");
        return false;
    }
    impl_->composer.clear_cancel();
    impl_->state = WatermarkSessionState::running;
    error = {};
    return true;
}

void WatermarkSession::request_cancel() {
    impl_->composer.request_cancel();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state == WatermarkSessionState::running ||
        impl_->state == WatermarkSessionState::configured) {
        impl_->state = WatermarkSessionState::cancelling;
    }
}

void WatermarkSession::stop() {
    impl_->composer.request_cancel();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->composer.reset();
    impl_->slots.clear();
    ++impl_->generation;
    impl_->state = WatermarkSessionState::stopped;
}

bool WatermarkSession::attach_target(const TargetKey& key,
    const Window& target, CompositionError& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state != WatermarkSessionState::running) {
        error = make_error(CompositionError::Domain::state, 81,
            L"targets can only be attached while the watermark session is running");
        return false;
    }
    if (std::get<0>(key) == 0) {
        error = make_error(CompositionError::Domain::validation, 82,
            L"target key has an empty HWND");
        return false;
    }
    if (auto* slot = impl_->find(key)) {
        slot->window = target;
        slot->state = WatermarkTargetState::active;
        slot->generation = impl_->generation;
        slot->watermark_ready = false;
        slot->explicit_local = false;
        slot->explicit_capture = false;
        slot->hidden_local = false;
        slot->hidden_capture = false;
        slot->last_error = {};
        error = {};
        return true;
    }
    Impl::Slot slot;
    slot.key = key;
    slot.window = target;
    slot.generation = impl_->generation;
    impl_->slots.push_back(std::move(slot));
    ++impl_->stats.targets_attached;
    error = {};
    return true;
}

bool WatermarkSession::update_target_window(const TargetKey& key,
    const Window& target, CompositionError& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* slot = impl_->find(key);
    if (!slot) {
        error = make_error(CompositionError::Domain::target, 83,
            L"cannot update a target that is not attached");
        return false;
    }
    slot->window = target;
    error = {};
    return true;
}

bool WatermarkSession::mark_target_minimized(const TargetKey& key,
    bool minimized, CompositionError& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* slot = impl_->find(key);
    if (!slot) {
        error = make_error(CompositionError::Domain::target, 84,
            L"cannot change the state of a target that is not attached");
        return false;
    }
    slot->state = minimized ? WatermarkTargetState::minimized
                            : WatermarkTargetState::active;
    impl_->bump_epoch();
    error = {};
    return true;
}

bool WatermarkSession::fail_target(const TargetKey& key,
    const CompositionError& reason, CompositionError& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* slot = impl_->find(key);
    if (!slot) {
        error = make_error(CompositionError::Domain::target, 85,
            L"cannot fail a target that is not attached");
        return false;
    }
    slot->state = WatermarkTargetState::failed;
    slot->watermark_ready = false;
    slot->last_error = reason;
    ++impl_->stats.targets_failed;
    error = {};
    return true;
}

bool WatermarkSession::retry_target(const TargetKey& key,
    CompositionError& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto* slot = impl_->find(key);
    if (!slot) {
        error = make_error(CompositionError::Domain::target, 86,
            L"cannot retry a target that is not attached");
        return false;
    }
    impl_->bump_epoch();
    slot->state = WatermarkTargetState::active;
    slot->last_error = {};
    ++impl_->stats.target_retries;
    error = {};
    return true;
}

bool WatermarkSession::close_target(const TargetKey& key,
    CompositionError& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->slots.begin(); it != impl_->slots.end(); ++it) {
        if (it->key == key) {
            impl_->slots.erase(it);
            ++impl_->stats.targets_closed;
            error = {};
            return true;
        }
    }
    error = make_error(CompositionError::Domain::target, 87,
        L"cannot close a target that is not attached");
    return false;
}

bool WatermarkSession::compose(const TargetKey& key,
    const ImageBuffer& source, ComposedFrame& out, CompositionError& error) {
    return compose(key, source, 0, out, error);
}

bool WatermarkSession::compose(const TargetKey& key,
    const ImageBuffer& source, std::uint64_t expected_generation,
    ComposedFrame& out, CompositionError& error) {
    out = {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state == WatermarkSessionState::cancelling) {
        error = make_error(CompositionError::Domain::cancelled, 90,
            L"watermark session is cancelling");
        ++impl_->stats.cancelled;
        return false;
    }
    if (impl_->state != WatermarkSessionState::running) {
        error = make_error(CompositionError::Domain::state, 91,
            L"watermark session is not running");
        return false;
    }
    auto* slot = impl_->find(key);
    if (!slot) {
        error = make_error(CompositionError::Domain::target, 92,
            L"target is not attached to this watermark session");
        return false;
    }
    if (slot->state == WatermarkTargetState::minimized) {
        error = make_error(CompositionError::Domain::target, 93,
            L"target is minimized; no watermark frame is produced");
        return false;
    }
    if (slot->state != WatermarkTargetState::active) {
        error = slot->last_error.ok()
            ? make_error(CompositionError::Domain::target, 94,
                  L"target is not active")
            : slot->last_error;
        return false;
    }
    if (expected_generation != 0 && expected_generation != impl_->generation) {
        error = make_error(CompositionError::Domain::state, 95,
            L"stale composition result: the session generation changed");
        return false;
    }

    ComposedFrame candidate;
    CompositionError compose_error;
    if (!impl_->composer.compose(source, slot->window, candidate,
            compose_error)) {
        ++slot->failures;
        slot->watermark_ready = false;
        slot->explicit_local = false;
        slot->explicit_capture = false;
        slot->hidden_local = false;
        slot->hidden_capture = false;
        slot->last_error = compose_error;
        ++impl_->stats.failures;
        if (compose_error.domain == CompositionError::Domain::cancelled) {
            ++impl_->stats.cancelled;
        }
        error = compose_error;
        return false;
    }
    if (candidate.generation != impl_->composer.generation()) {
        error = make_error(CompositionError::Domain::state, 96,
            L"composer returned a frame from an unexpected generation");
        return false;
    }
    candidate.generation = impl_->generation;
    out = std::move(candidate);
    ++slot->frames;
    slot->generation = impl_->generation;
    slot->watermark_ready = true;
    slot->explicit_local = out.local_explicit;
    slot->explicit_capture = out.capture_explicit;
    slot->hidden_local = out.local_hidden;
    slot->hidden_capture = out.capture_hidden;
    slot->last_error = {};
    ++impl_->stats.frames;
    if (out.local_explicit) ++impl_->stats.explicit_local_frames;
    if (out.capture_explicit) ++impl_->stats.explicit_capture_frames;
    if (out.local_hidden) ++impl_->stats.hidden_local_frames;
    if (out.capture_hidden) ++impl_->stats.hidden_capture_frames;
    error = {};
    return true;
}

WatermarkSessionState WatermarkSession::state() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}

std::uint64_t WatermarkSession::generation() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->generation;
}

WatermarkPlan WatermarkSession::plan() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->plan;
}

CaptureRouteCapabilities WatermarkSession::route_capabilities() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->routes;
}

WatermarkSessionStats WatermarkSession::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

WatermarkTargetStatus WatermarkSession::target_status(
    const TargetKey& key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status_for(impl_->find(key), key);
}

std::vector<WatermarkTargetStatus> WatermarkSession::targets() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<WatermarkTargetStatus> result;
    result.reserve(impl_->slots.size());
    for (const auto& slot : impl_->slots) {
        result.push_back(impl_->status_for(&slot, slot.key));
    }
    return result;
}

WatermarkFrameComposer& WatermarkSession::composer() noexcept {
    return impl_->composer;
}

const WatermarkFrameComposer& WatermarkSession::composer() const noexcept {
    return impl_->composer;
}

}  // namespace av
