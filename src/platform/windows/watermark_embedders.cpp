// SPDX-License-Identifier: Apache-2.0
// Blind watermark adapters: capture seam -> legacy DCT / DWT and R-DCT cores.
#include "platform/windows/watermark_session.h"

#include "av/watermark/dct.h"
#include "av/watermark/dwt.h"
#include "av/watermark/robust.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>

namespace av {
namespace {

bool cancel_requested(void* context) {
    if (!context) return false;
    return static_cast<std::atomic<bool>*>(context)->load(
        std::memory_order_relaxed);
}

PipelineError pipeline_error(PipelineError::Domain domain, std::uint32_t code,
    std::wstring detail) {
    PipelineError value;
    value.domain = domain;
    value.code = code;
    value.detail = std::move(detail);
    return value;
}

PipelineError map_status(av::watermark::Status status, const wchar_t* operation) {
    if (status == av::watermark::Status::success) return {};
    if (status == av::watermark::Status::cancelled) {
        return pipeline_error(PipelineError::Domain::cancelled, 30,
            std::wstring(operation) + L" was cancelled");
    }
    if (status == av::watermark::Status::invalid_argument) {
        return pipeline_error(PipelineError::Domain::validation, 31,
            std::wstring(operation) + L" rejected the image, key or packet");
    }
    if (status == av::watermark::Status::payload_too_large) {
        return pipeline_error(PipelineError::Domain::hidden, 32,
            std::wstring(operation) + L" payload exceeds 32 bytes");
    }
    if (status == av::watermark::Status::capacity_exceeded) {
        return pipeline_error(PipelineError::Domain::budget, 33,
            std::wstring(operation) + L" frame has insufficient carriers");
    }
    if (status == av::watermark::Status::unsupported_algorithm) {
        return pipeline_error(PipelineError::Domain::unsupported, 34,
            std::wstring(operation) + L" algorithm is not supported");
    }
    return pipeline_error(PipelineError::Domain::hidden, 35,
        std::wstring(operation) + L" failed with status " +
            std::to_wstring(static_cast<unsigned>(status)));
}

bool copy_rows(const ImageView& source, const MutableImageView& target) {
    if (!valid_image_view(source) || !valid_image_view(target)) return false;
    if (source.width != target.width || source.height != target.height) {
        return false;
    }
    if (source.stride == target.stride &&
        source.pixels == target.pixels) {
        return true;
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(source.width) * bgra8_bytes_per_pixel;
    for (std::uint32_t y = 0; y < source.height; ++y) {
        std::memcpy(target.pixels + static_cast<std::size_t>(y) * target.stride,
            source.pixels + static_cast<std::size_t>(y) * source.stride,
            row_bytes);
    }
    return true;
}

class DctEmbedder final : public BlindWatermarkEmbedder {
public:
    explicit DctEmbedder(std::shared_ptr<std::atomic<bool>> cancel, bool robust = false)
        : cancel_(std::move(cancel)), robust_(robust) {}

    std::uint16_t algorithm() const noexcept override {
        return robust_ ? kHiddenAlgorithmRobustDct : kHiddenAlgorithmDctPairQim;
    }
    bool available() const noexcept override { return true; }

    bool embed(const ImageView& source, const MutableImageView& target,
        const WatermarkPacket& packet, const std::vector<std::uint8_t>& key,
        PipelineError& error) override {
        if (!valid_image_view(source) || !valid_image_view(target) ||
            !image_views_match(source,
                ImageView{target.width, target.height, target.stride,
                    target.format, target.alpha, target.pixels})) {
            error = pipeline_error(PipelineError::Domain::validation, 40,
                L"DCT embed received invalid or mismatched image views");
            return false;
        }
        if (packet.algorithm != algorithm()) {
            error = pipeline_error(PipelineError::Domain::unsupported, 41,
                L"DCT embed received a non-DCT algorithm id");
            return false;
        }
        if (packet.payload.empty() || packet.payload.size() > kHiddenPayloadMax ||
            !packet.parameters.empty() || !watermark_packet_valid(packet)) {
            error = pipeline_error(PipelineError::Domain::hidden, 42,
                L"DCT embed requires a 1..32 byte V1 packet and empty parameters");
            return false;
        }
        if (key.empty() || key.size() > kHiddenKeyMax) {
            error = pipeline_error(PipelineError::Domain::validation, 43,
                L"DCT embed key must be 1..128 bytes");
            return false;
        }
        if (source.width > static_cast<std::uint32_t>(INT_MAX) ||
            source.height > static_cast<std::uint32_t>(INT_MAX) ||
            target.stride > static_cast<std::size_t>(INT_MAX)) {
            error = pipeline_error(PipelineError::Domain::budget, 44,
                L"DCT embed frame is too large for the algorithm ABI");
            return false;
        }
        if (!copy_rows(source, target)) {
            error = pipeline_error(PipelineError::Domain::validation, 45,
                L"DCT embed could not stage the source into the working frame");
            return false;
        }

        av::watermark::dct::MutableImageView view;
        view.width = target.width;
        view.height = target.height;
        view.stride = target.stride;
        view.pixels = target.pixels;

        av::watermark::dct::EmbedOptions options;
        options.delta = av::watermark::dct::kDefaultDelta;
        if (robust_) options.worker_threads = 2;
        if (cancel_) {
            options.cancel.fn = &cancel_requested;
            options.cancel.context = cancel_.get();
        }
        av::watermark::dct::EmbedResult result;
        const auto embed = robust_ ? &av::watermark::robust::embed : &av::watermark::dct::embed;
        const auto status = embed(view, key.data(),
            key.size(), packet.payload.data(), packet.payload.size(), options,
            result);
        if (status != av::watermark::Status::success) {
            error = map_status(status, L"DCT embed");
            return false;
        }
        error = {};
        return true;
    }

private:
    std::shared_ptr<std::atomic<bool>> cancel_;
    bool robust_ = false;
};

class DwtEmbedder final : public BlindWatermarkEmbedder {
public:
    explicit DwtEmbedder(std::shared_ptr<std::atomic<bool>> cancel)
        : cancel_(std::move(cancel)) {}

    std::uint16_t algorithm() const noexcept override {
        return kHiddenAlgorithmDwtLh3Qim;
    }
    bool available() const noexcept override { return true; }

    bool embed(const ImageView& source, const MutableImageView& target,
        const WatermarkPacket& packet, const std::vector<std::uint8_t>& key,
        PipelineError& error) override {
        if (!valid_image_view(source) || !valid_image_view(target) ||
            !image_views_match(source,
                ImageView{target.width, target.height, target.stride,
                    target.format, target.alpha, target.pixels})) {
            error = pipeline_error(PipelineError::Domain::validation, 50,
                L"DWT embed received invalid or mismatched image views");
            return false;
        }
        if (packet.algorithm != kHiddenAlgorithmDwtLh3Qim) {
            error = pipeline_error(PipelineError::Domain::unsupported, 51,
                L"DWT embed received a non-DWT algorithm id");
            return false;
        }
        if (packet.payload.empty() || packet.payload.size() > kHiddenPayloadMax ||
            !packet.parameters.empty() || !watermark_packet_valid(packet)) {
            error = pipeline_error(PipelineError::Domain::hidden, 52,
                L"DWT embed requires a 1..32 byte V1 packet and empty parameters");
            return false;
        }
        if (key.empty() || key.size() > kHiddenKeyMax) {
            error = pipeline_error(PipelineError::Domain::validation, 53,
                L"DWT embed key must be 1..128 bytes");
            return false;
        }
        if (source.width > static_cast<std::uint32_t>(INT_MAX) ||
            source.height > static_cast<std::uint32_t>(INT_MAX) ||
            source.stride > static_cast<std::size_t>(INT_MAX) ||
            target.stride > static_cast<std::size_t>(INT_MAX)) {
            error = pipeline_error(PipelineError::Domain::budget, 54,
                L"DWT embed frame is too large for the algorithm ABI");
            return false;
        }

        av::watermark::ImageView input;
        input.pixels = source.pixels;
        input.width = static_cast<int>(source.width);
        input.height = static_cast<int>(source.height);
        input.stride = static_cast<int>(source.stride);
        input.format = av::watermark::PixelFormat::bgra8;

        av::watermark::MutableImageView output;
        output.pixels = target.pixels;
        output.width = static_cast<int>(target.width);
        output.height = static_cast<int>(target.height);
        output.stride = static_cast<int>(target.stride);
        output.format = av::watermark::PixelFormat::bgra8;

        av::watermark::DwtParameters parameters;
        av::watermark::CancelToken cancel;
        if (cancel_) {
            cancel.callback = &cancel_requested;
            cancel.context = cancel_.get();
        }
        const auto status = av::watermark::dwt_embed(input, output,
            key.data(), key.size(), packet.payload.data(), packet.payload.size(),
            parameters, cancel);
        if (status != av::watermark::Status::success) {
            error = map_status(status, L"DWT embed");
            return false;
        }
        error = {};
        return true;
    }

private:
    std::shared_ptr<std::atomic<bool>> cancel_;
};

}  // namespace

std::shared_ptr<BlindWatermarkEmbedder> make_dct_embedder(
    std::shared_ptr<std::atomic<bool>> cancel) {
    if (!cancel) cancel = std::make_shared<std::atomic<bool>>(false);
    return std::make_shared<DctEmbedder>(std::move(cancel));
}

std::shared_ptr<BlindWatermarkEmbedder> make_dwt_embedder(
    std::shared_ptr<std::atomic<bool>> cancel) {
    if (!cancel) cancel = std::make_shared<std::atomic<bool>>(false);
    return std::make_shared<DwtEmbedder>(std::move(cancel));
}

std::shared_ptr<BlindWatermarkEmbedder> make_robust_embedder(
    std::shared_ptr<std::atomic<bool>> cancel) {
    if (!cancel) cancel = std::make_shared<std::atomic<bool>>(false);
    return std::make_shared<DctEmbedder>(std::move(cancel), true);
}

}  // namespace av
