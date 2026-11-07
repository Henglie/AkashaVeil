// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/model.h"
#include "core/watermark.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace av {

// The capture branch is the only branch that may receive an explicit or a
// hidden watermark.  The clean branch is the exact top-down BGRA8 frame that
// was read from the capture backend; the local viewer mirrors this branch.
enum class CaptureSourceKind : unsigned {
    window = 0,
    display = 1
};

enum class CaptureBackendKind : unsigned {
    automatic = 0,
    wgc = 1,
    bitblt = 2
};

enum class CaptureBackendState : unsigned {
    available = 0,
    unsupported_os = 1,
    missing_api = 2,
    permission_denied = 3,
    unavailable = 4,
    unknown = 5
};

const wchar_t* capture_source_kind_name(CaptureSourceKind kind) noexcept;
const wchar_t* capture_backend_kind_name(CaptureBackendKind kind) noexcept;
const wchar_t* capture_backend_state_name(CaptureBackendState state) noexcept;

struct CaptureSource {
    CaptureSourceKind kind = CaptureSourceKind::window;
    HWND window = nullptr;
    HMONITOR monitor = nullptr;
    // Physical screen coordinates.  Empty means the monitor rectangle.
    RECT display_rect{};
};

struct CaptureCapability {
    CaptureBackendKind backend = CaptureBackendKind::automatic;
    CaptureSourceKind source = CaptureSourceKind::window;
    CaptureBackendState state = CaptureBackendState::unknown;
    bool supported = false;
    bool os_supported = false;
    bool api_present = false;
    bool permission_known = false;
    bool cross_integrity_tested = false;
    DWORD win32_error = 0;
    std::int32_t hresult = 0;
    std::wstring os_version;
    std::wstring detail;
};

struct PipelineError {
    enum class Domain : unsigned {
        none = 0,
        validation = 1,
        unsupported = 2,
        permission = 3,
        capture = 4,
        watermark = 5,
        hidden = 6,
        budget = 7,
        cancelled = 8,
        target_lost = 9,
        internal = 10
    };

    Domain domain = Domain::none;
    std::uint32_t code = 0;
    std::int32_t hresult = 0;
    DWORD win32_error = 0;
    std::wstring detail;

    bool ok() const noexcept { return domain == Domain::none; }
};

const wchar_t* pipeline_error_domain_name(PipelineError::Domain domain) noexcept;

struct TargetMetadata {
    std::uintptr_t handle = 0;
    unsigned long pid = 0;
    std::uint64_t process_created = 0;
    std::wstring title;
    std::wstring path;
    std::wstring class_name;
};

Window window_from_metadata(const TargetMetadata& target);

// The pure-memory seam is FrameView/MutableFrameView; the frozen BGRA8 layout
// is ImageView/MutableImageView. Keep both names for the watermark hand-off.
using FrameView = ImageView;
using MutableFrameView = MutableImageView;

struct Frame {
    ImageBuffer image;
    std::uint64_t sequence = 0;
    std::uint64_t capture_tick_ms = 0;

    bool valid() const noexcept { return image.valid(); }
};

struct FramePair {
    Frame clean;
    Frame watermarked;
    bool has_watermark = false;
    bool hidden_embedded = false;
    bool hidden_skipped = false;
};

enum class ExplicitLayout : unsigned {
    none = 0,
    tiled = 1,
    center = 2,
    corner = 3
};

struct ExplicitWatermarkSpec {
    bool enabled = false;
    ExplicitLayout layout = ExplicitLayout::tiled;
    unsigned opacity = 45;
    unsigned font_size = 22;
    unsigned spacing = 240;
    std::uint32_t foreground = 0xE0E0E0;
    std::uint32_t background = 0x282828;
    std::wstring text = L"CONFIDENTIAL\n{id}  {time}";
    std::wstring label = L"Protected content";
    std::wstring tag;
    unsigned dpi = 96;
    // Test/evidence only.  It draws a solid orange block that the automated
    // evidence checker can find.  Product configurations leave it false.
    bool evidence_block = false;
};

ExplicitWatermarkSpec explicit_watermark_for_capture(const Effects& effects,
    unsigned dpi, bool evidence_block = false);

// The hidden watermark seam. The blind algorithm cores provide implementations;
// the capture pipeline only owns the ordering and the failure semantics.
struct BlindWatermarkRequest {
    bool enabled = false;
    WatermarkPacket packet;
    std::vector<std::uint8_t> key;
};

class BlindWatermarkEmbedder {
public:
    virtual ~BlindWatermarkEmbedder() = default;
    virtual std::uint16_t algorithm() const noexcept = 0;
    virtual bool available() const noexcept = 0;
    virtual bool embed(const ImageView& source, const MutableImageView& target,
        const WatermarkPacket& packet, const std::vector<std::uint8_t>& key,
        PipelineError& error) = 0;
};

struct FramePipelineConfig {
    CaptureSource source;
    TargetMetadata target;
    CaptureBackendKind preferred_backend = CaptureBackendKind::automatic;
    // When true, an unavailable preferred backend is an error instead of a
    // diagnosed fallback.
    bool require_preferred_backend = false;
    std::uint32_t capture_timeout_ms = 500;
    std::uint32_t target_fps = 30;
    std::size_t max_frame_bytes = 256ull * 1024ull * 1024ull;
    bool force_opaque = true;
    bool window_client_only = false;
    bool include_clean = true;
    ExplicitWatermarkSpec watermark;
    BlindWatermarkRequest hidden;
    std::shared_ptr<BlindWatermarkEmbedder> hidden_embedder;
};

struct PipelineStats {
    std::uint64_t frames = 0;
    std::uint64_t clean_frames = 0;
    std::uint64_t watermarked_frames = 0;
    std::uint64_t hidden_embedded = 0;
    std::uint64_t hidden_skipped = 0;
    std::uint64_t hidden_failures = 0;
    std::uint64_t blank_frames = 0;
    std::uint64_t failed_frames = 0;
    std::uint64_t frame_bytes = 0;
    std::uint64_t peak_buffered_bytes = 0;
    std::uint64_t first_tick_ms = 0;
    std::uint64_t last_tick_ms = 0;
    double last_capture_ms = 0.0;
    double last_capture_wait_ms = 0.0;
    double last_normalize_ms = 0.0;
    double last_composite_ms = 0.0;
    double last_hidden_ms = 0.0;
    double last_pipeline_ms = 0.0;
    double last_total_ms = 0.0;
    double avg_capture_ms = 0.0;
    double avg_capture_wait_ms = 0.0;
    double avg_normalize_ms = 0.0;
    double avg_composite_ms = 0.0;
    double avg_hidden_ms = 0.0;
    double avg_pipeline_ms = 0.0;
    double avg_total_ms = 0.0;
    double max_total_ms = 0.0;
    double fps = 0.0;
};

struct BackendSelection {
    CaptureBackendKind requested = CaptureBackendKind::automatic;
    CaptureBackendKind selected = CaptureBackendKind::automatic;
    bool fallback = false;
    CaptureCapability requested_capability;
    CaptureCapability selected_capability;
    std::vector<std::wstring> attempts;
};

class FramePipeline {
public:
    FramePipeline();
    ~FramePipeline();
    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;

    bool start(const FramePipelineConfig& config, PipelineError& error);
    enum class NextResult { frame, timeout, cancelled, failed };
    NextResult next(FramePair& out, std::uint32_t timeout_ms, PipelineError& error);
    void cancel();
    void stop();
    bool running() const noexcept;

    const FramePipelineConfig& config() const noexcept;
    const PipelineStats& stats() const noexcept;
    const BackendSelection& backend_selection() const noexcept;

    static CaptureCapability probe(CaptureBackendKind backend,
        const CaptureSource& source);
    static std::vector<CaptureCapability> probe_all(const CaptureSource& source);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Exposed for the blind watermark cores and independent unit checks.
bool normalize_frame(const ImageBuffer& input, ImageBuffer& output,
    PipelineError& error);
void force_opaque(ImageBuffer& frame);
bool is_blank_frame(const ImageBuffer& frame);
bool compose_explicit_watermark(ImageBuffer& frame, const Window& target,
    const ExplicitWatermarkSpec& spec, PipelineError& error);
bool apply_blind_watermark(const ImageView& source,
    const MutableImageView& target,
    const BlindWatermarkRequest& request,
    const std::shared_ptr<BlindWatermarkEmbedder>& embedder,
    PipelineError& error);

}  // namespace av
