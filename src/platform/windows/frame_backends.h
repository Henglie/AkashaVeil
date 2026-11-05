// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/frame_pipeline.h"

#include <memory>

namespace av {
namespace detail {

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual CaptureBackendKind kind() const noexcept = 0;
    virtual bool start(const CaptureSource& source, PipelineError& error) = 0;
    virtual bool next(Frame& out, std::uint32_t timeout_ms,
        PipelineError& error) = 0;
    virtual void stop() = 0;
    virtual unsigned width() const noexcept = 0;
    virtual unsigned height() const noexcept = 0;
    // Time spent copying/processing the frame, excluding waiting for the
    // compositor to produce the next frame.
    virtual double last_processing_ms() const noexcept { return 0.0; }
};

std::unique_ptr<CaptureBackend> make_wgc_backend();
std::unique_ptr<CaptureBackend> make_bitblt_backend();
CaptureCapability probe_wgc(const CaptureSource& source);
CaptureCapability probe_bitblt(const CaptureSource& source);
std::wstring windows_version_text();

}  // namespace detail
}  // namespace av
