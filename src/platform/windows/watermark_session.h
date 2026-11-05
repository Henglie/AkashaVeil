// SPDX-License-Identifier: Apache-2.0
// Composition/lifecycle orchestration for explicit + hidden watermarks.
//
// The module is deliberately split from the capture frame pipeline and the
// blind algorithm cores:
//   * Explicit watermark parameters use ExplicitWatermarkSpec (form/text/
//     colour/font/spacing/opacity/dpi).
//   * Hidden watermark parameters use the control-layer packet plus a separate
//     protocol V1 algorithm id, payload and key. The two parameter families are
//     never inferred from one another.
//   * Order is always: explicit composition first, hidden embedding second,
//     branch output last.  A hidden failure discards the whole composed frame;
//     there is no fallback to "explicit only".
#pragma once

#include "core/identity.h"
#include "core/model.h"
#include "core/watermark.h"
#include "platform/windows/frame_pipeline.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace av {

constexpr std::uint16_t kHiddenAlgorithmDctPairQim = 1;
constexpr std::uint16_t kHiddenAlgorithmDwtLh3Qim = 2;
constexpr std::size_t kHiddenPayloadMax = 32;
constexpr std::size_t kHiddenKeyMax = 128;

const wchar_t* hidden_algorithm_name(std::uint16_t algorithm) noexcept;

// The user-visible combination matrix.  Capture-only modes always keep the
// local branch byte-identical to the captured source and require an A or B
// route capability; they never silently turn into a screen-visible overlay.
enum class WatermarkVisibility : unsigned {
    screen_and_capture = 0,
    capture_only_explicit = 1,
    capture_only_hidden = 2,
    capture_only_explicit_hidden = 3
};

enum class CaptureOnlyRoute : unsigned {
    none = 0,
    display_capture_a = 1,
    proxy_source_b = 2
};

struct CaptureRouteCapabilities {
    bool display_capture_a = false;
    bool proxy_source_b = false;
    std::wstring display_capture_detail;
    std::wstring proxy_source_detail;
};

struct CompositionLimits {
    std::size_t max_pixels = 32ull * 1024ull * 1024ull;
    std::size_t max_frame_bytes = 256ull * 1024ull * 1024ull;
    std::size_t max_hidden_payload = kHiddenPayloadMax;
    std::size_t max_hidden_key = kHiddenKeyMax;
};

// Algorithm id, packet and key are separate fields on purpose.  A plan cannot
// produce a valid hidden layer when the id and packet disagree.
struct HiddenWatermarkOptions {
    bool enabled = false;
    std::uint16_t algorithm = 0;
    WatermarkPacket packet;
    std::vector<std::uint8_t> key;
};

HiddenWatermarkOptions make_hidden_watermark(std::uint16_t algorithm,
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::uint8_t>& key);

struct WatermarkPlan {
    WatermarkVisibility visibility = WatermarkVisibility::screen_and_capture;
    CaptureOnlyRoute capture_only_route = CaptureOnlyRoute::none;
    ExplicitWatermarkSpec explicit_layer;
    HiddenWatermarkOptions hidden_layer;
    CompositionLimits limits;
};

struct CompositionError {
    enum class Domain : unsigned {
        none = 0,
        validation = 1,
        unsupported = 2,
        hidden = 3,
        budget = 4,
        cancelled = 5,
        target = 6,
        state = 7,
        internal = 8
    };

    Domain domain = Domain::none;
    std::uint32_t code = 0;
    std::wstring detail;

    bool ok() const noexcept { return domain == Domain::none; }
};

const wchar_t* composition_error_domain_name(CompositionError::Domain domain) noexcept;

struct ComposedFrame {
    ImageBuffer local;
    ImageBuffer capture;
    std::uint64_t generation = 0;
    bool local_explicit = false;
    bool capture_explicit = false;
    bool local_hidden = false;
    bool capture_hidden = false;
    // Capture-only modes: the local branch is the untouched source and the
    // capture branch keeps the original dynamic content under the watermark.
    bool local_original_preserved = false;
    bool capture_original_preserved = false;

    bool valid() const noexcept;
};

enum class HiddenExtractionStatus : unsigned {
    success = 0,
    not_detected = 1,
    corrupted = 2,
    unsupported_version = 3,
    unsupported_algorithm = 4,
    wrong_key = 5,
    invalid_argument = 6,
    cancelled = 7,
    payload_too_large = 8,
    capacity_exceeded = 9,
    unavailable = 10
};

const wchar_t* hidden_extraction_status_name(HiddenExtractionStatus status) noexcept;

struct HiddenExtraction {
    HiddenExtractionStatus status = HiddenExtractionStatus::not_detected;
    std::uint16_t algorithm = 0;
    std::vector<std::uint8_t> payload;
    std::uint8_t corrected_bytes = 0;
    unsigned sync_score = 0;
    int phase_x = 0;
    int phase_y = 0;
    int tile_x = 0;
    int tile_y = 0;
    std::wstring detail;

    bool success() const noexcept {
        return status == HiddenExtractionStatus::success;
    }
};

struct HiddenEmbedderSet {
    std::shared_ptr<BlindWatermarkEmbedder> dct;
    std::shared_ptr<BlindWatermarkEmbedder> dwt;
};

std::shared_ptr<BlindWatermarkEmbedder> make_dct_embedder(
    std::shared_ptr<std::atomic<bool>> cancel = {});
std::shared_ptr<BlindWatermarkEmbedder> make_dwt_embedder(
    std::shared_ptr<std::atomic<bool>> cancel = {});

// Pure in-memory composer. It never creates a window and never reads the
// desktop. The integration layer owns the capture source and local/capture sinks.
class WatermarkFrameComposer {
public:
    WatermarkFrameComposer();
    explicit WatermarkFrameComposer(HiddenEmbedderSet embedders,
        std::shared_ptr<std::atomic<bool>> cancel = {});
    ~WatermarkFrameComposer();
    WatermarkFrameComposer(const WatermarkFrameComposer&) = delete;
    WatermarkFrameComposer& operator=(const WatermarkFrameComposer&) = delete;
    WatermarkFrameComposer(WatermarkFrameComposer&&) noexcept;
    WatermarkFrameComposer& operator=(WatermarkFrameComposer&&) noexcept;

    static bool validate_plan(const WatermarkPlan& plan,
        const HiddenEmbedderSet& embedders, CompositionError& error);

    bool configure(const WatermarkPlan& plan, CompositionError& error);
    bool compose(const ImageBuffer& source, const Window& target,
        ComposedFrame& out, CompositionError& error);
    void reset();
    void request_cancel();
    void clear_cancel();
    bool configured() const noexcept;
    std::uint64_t generation() const noexcept;
    const WatermarkPlan& plan() const noexcept;

    static bool extract(const ImageBuffer& image, std::uint16_t algorithm,
        const std::vector<std::uint8_t>& key, HiddenExtraction& result,
        CompositionError& error);

private:
    std::shared_ptr<BlindWatermarkEmbedder> embedder_for(
        std::uint16_t algorithm) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline TargetKey target_key_from_window(const Window& window) {
    return {window.handle, static_cast<std::uint32_t>(window.pid),
        window.process_created};
}

enum class WatermarkSessionState : unsigned {
    idle = 0,
    configured = 1,
    running = 2,
    cancelling = 3,
    stopped = 4,
    target_lost = 5,
    failed = 6
};

const wchar_t* watermark_session_state_name(WatermarkSessionState state) noexcept;

enum class WatermarkTargetState : unsigned {
    active = 0,
    minimized = 1,
    failed = 2,
    lost = 3
};

struct WatermarkTargetStatus {
    TargetKey key;
    bool attached = false;
    WatermarkTargetState state = WatermarkTargetState::lost;
    std::uint64_t generation = 0;
    std::uint64_t frames = 0;
    std::uint64_t failures = 0;
    // Set only after a frame was actually composed and handed back.  Setting a
    // target affinity or attaching a plan must never report watermark success.
    bool watermark_ready = false;
    bool explicit_local = false;
    bool explicit_capture = false;
    bool hidden_local = false;
    bool hidden_capture = false;
    CompositionError last_error;
};

struct WatermarkSessionStats {
    std::uint64_t frames = 0;
    std::uint64_t failures = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t explicit_local_frames = 0;
    std::uint64_t explicit_capture_frames = 0;
    std::uint64_t hidden_local_frames = 0;
    std::uint64_t hidden_capture_frames = 0;
    std::uint64_t targets_attached = 0;
    std::uint64_t targets_closed = 0;
    std::uint64_t targets_failed = 0;
    std::uint64_t mode_switches = 0;
    std::uint64_t target_retries = 0;
};

// UI-thread-owned session. It does not create windows, hooks or threads. The
// capture/render integration owns the real target handles, WGC/BitBlt sessions,
// the display-capture A route and the proxy-source B route.
class WatermarkSession {
public:
    WatermarkSession();
    explicit WatermarkSession(HiddenEmbedderSet embedders);
    ~WatermarkSession();
    WatermarkSession(const WatermarkSession&) = delete;
    WatermarkSession& operator=(const WatermarkSession&) = delete;

    bool configure(const WatermarkPlan& plan,
        const CaptureRouteCapabilities& routes, CompositionError& error);
    bool start(CompositionError& error);
    void request_cancel();
    void stop();

    bool attach_target(const TargetKey& key, const Window& target,
        CompositionError& error);
    bool update_target_window(const TargetKey& key, const Window& target,
        CompositionError& error);
    bool mark_target_minimized(const TargetKey& key, bool minimized,
        CompositionError& error);
    bool fail_target(const TargetKey& key, const CompositionError& reason,
        CompositionError& error);
    bool retry_target(const TargetKey& key, CompositionError& error);
    bool close_target(const TargetKey& key, CompositionError& error);

    bool compose(const TargetKey& key, const ImageBuffer& source,
        ComposedFrame& out, CompositionError& error);
    // expected_generation == 0 accepts the current generation.  A non-zero
    // mismatch is rejected as stale so a worker result cannot overwrite a new
    // mode/payload intent.
    bool compose(const TargetKey& key, const ImageBuffer& source,
        std::uint64_t expected_generation, ComposedFrame& out,
        CompositionError& error);

    WatermarkSessionState state() const;
    std::uint64_t generation() const;
    WatermarkPlan plan() const;
    CaptureRouteCapabilities route_capabilities() const;
    WatermarkSessionStats stats() const;
    WatermarkTargetStatus target_status(const TargetKey& key) const;
    std::vector<WatermarkTargetStatus> targets() const;

    WatermarkFrameComposer& composer() noexcept;
    const WatermarkFrameComposer& composer() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace av
