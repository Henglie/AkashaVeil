// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/frame_pipeline.h"

#include <memory>
#include <string>

namespace av {

enum class RenderSessionState : unsigned {
    idle = 0,
    starting = 1,
    running = 2,
    target_unavailable = 3,
    target_lost = 4,
    failed = 5,
    stopped = 6
};

const wchar_t* render_session_state_name(RenderSessionState state) noexcept;

struct CaptureOnlyRenderConfig {
    FramePipelineConfig pipeline;
    // Off-screen test harnesses force DWM to present the mirror surfaces even
    // though no physical monitor contains them.  Product use leaves this false.
    bool force_present_for_offscreen_test = false;
    unsigned sync_interval_ms = 100;
};

struct CaptureOnlyRenderStatus {
    RenderSessionState state = RenderSessionState::idle;
    std::wstring detail;
    TargetMetadata target;
    std::uintptr_t owner_layer = 0;
    std::uintptr_t capture_layer = 0;
    std::uintptr_t clean_layer = 0;
    bool capture_layer_visible = false;
    bool clean_layer_visible = false;
    bool clean_layer_excluded = false;
    DWORD clean_affinity = 0;
    bool target_affinity_known = false;
    DWORD target_affinity_before = 0;
    DWORD target_affinity_after = 0;
    int width = 0;
    int height = 0;
    unsigned dpi = 96;
    bool layers_destroyed = true;
    bool hidden_requested = false;
    std::wstring hidden_status = L"disabled";
    BackendSelection backend;
    PipelineStats stats;
    PipelineError last_error;
    std::uint64_t published_frames = 0;
    std::uint64_t last_publish_tick_ms = 0;
};

// Capture-only A route: the target is never modified.  A clean live mirror is
// the local-facing surface (WDA_EXCLUDEFROMCAPTURE); a watermarked live mirror
// is the capture-facing surface.  External display capture sees the
// watermarked mirror, local display and window DC read the clean mirror.
class CaptureOnlyRenderSession {
public:
    CaptureOnlyRenderSession();
    ~CaptureOnlyRenderSession();
    CaptureOnlyRenderSession(const CaptureOnlyRenderSession&) = delete;
    CaptureOnlyRenderSession& operator=(const CaptureOnlyRenderSession&) = delete;

    bool start(const CaptureOnlyRenderConfig& config, PipelineError& error);
    void request_stop();
    void stop();
    // Called on the owning thread.  It validates the target identity, keeps
    // the mirror geometry in sync, paints the latest frames and makes the
    // target-unavailable decision.  It is safe to call before start.
    void sync();
    CaptureOnlyRenderStatus status() const;

    // Test/evidence helper.  It temporarily clears WDA on the clean layer,
    // reads its actual GDI pixels with PrintWindow, restores WDA and verifies
    // the exclusion value before returning.
    bool dump_clean_png(const std::wstring& path, PipelineError& error);
    bool write_status_json(const std::wstring& path, PipelineError& error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace av
