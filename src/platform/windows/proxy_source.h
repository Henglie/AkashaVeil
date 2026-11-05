// SPDX-License-Identifier: Apache-2.0
// Controlled capture-source window (F08 safe route B).
//
// A capture thread reads the target through the shared capture backend,
// composes the explicit watermark first and the hidden watermark second through
// the composition layer, and presents the resulting capture frame in an off-screen and/or
// DWM-cloaked proxy window P. The user selects P as the OBS/meeting window
// capture source. No process is injected; the original target window is never
// modified and no display affinity is applied to P.
#pragma once
#include "platform/windows/frame_pipeline.h"
#include "platform/windows/watermark_session.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace av {

enum class ProxySourceState : unsigned {
    idle = 0,
    starting = 1,
    capturing = 2,
    target_gone = 3,
    failed = 4,
    stopped = 5
};

struct ProxySourceConfig {
    TargetMetadata target;
    WatermarkPlan plan;
    std::wstring title;
    bool cloak = true;
    unsigned fps = 30;
    std::uint32_t capture_timeout_ms = 250;
};

struct ProxySourceStatus {
    ProxySourceState state = ProxySourceState::idle;
    std::wstring detail;
    TargetMetadata target;
    std::uintptr_t proxy_window = 0;
    std::uintptr_t owner_window = 0;
    bool visible = false;
    bool cloaked = false;
    bool offscreen = false;
    // True when P cannot be seen on the local desktop (cloaked or fully
    // outside the virtual screen).
    bool local_clean = false;
    bool wda_zero = false;
    bool normal_style = false;
    BackendSelection backend;
    PipelineStats pipeline_stats;
    WatermarkSessionStats watermark_stats;
    std::uint64_t published_frames = 0;
    std::uint64_t failures = 0;
};

class ProxySourceSession {
public:
    ProxySourceSession();
    ~ProxySourceSession();
    ProxySourceSession(const ProxySourceSession&) = delete;
    ProxySourceSession& operator=(const ProxySourceSession&) = delete;

    bool start(const ProxySourceConfig& config, CompositionError& error);
    void stop();
    ProxySourceStatus status() const;
    bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace av
