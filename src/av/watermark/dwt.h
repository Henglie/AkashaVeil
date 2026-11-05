// SPDX-License-Identifier: Apache-2.0
// AkashaVeil DWT blind watermark: 3-level orthonormal Haar, LH3 detail-band QIM.
// Protocol layer is packet/RS/sync; this file is the DWT-specific
// transform, payload mapping, extraction and image plumbing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "av/watermark/packet.h"
#include "av/watermark/sync.h"
#include "av/watermark/types.h"

namespace av::watermark {

enum class PixelFormat : std::uint8_t {
    bgra8 = 0,
    bgr8 = 1,
    rgb8 = 2,
    gray8 = 3
};

struct ImageView {
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::bgra8;
};

struct MutableImageView {
    std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::bgra8;
};

using CancelCallback = bool (*)(void* context);

struct CancelToken {
    CancelCallback callback = nullptr;
    void* context = nullptr;

    bool requested() const noexcept
    {
        return callback != nullptr && callback(context);
    }
};

// Frozen protocol parameters. Levels and tile are validated, not silently
// reinterpreted; a future change needs a new protocol decision.
struct DwtParameters {
    double delta = 24.0;
    int levels = 3;
    int tile = 32;
    // 0 = implementation default (bounded row parallelism for large frames),
    // 1 = serial. This is a scheduling knob only; the frozen maths and packet
    // bytes are unchanged.
    int max_threads = 0;
};

// Extraction geometry. The caller supplies scale/crop/rotation facts
// explicitly; the extractor never reads the original image. phase_x/phase_y are
// the 8x8 pixel grid phase (0..7). tile_x/tile_y are the 32x32 tile phase
// (0..31). rotation is the clockwise rotation in degrees that the attacker
// applied to the watermarked image (0, 90, 180 or 270); the extractor undoes
// it before reading coefficients. Arbitrary-angle rotation is not claimed.
struct WatermarkPosition {
    int phase_x = 0;
    int phase_y = 0;
    int tile_x = 0;
    int tile_y = 0;
    double scale = 1.0;
    int original_width = 0;
    int original_height = 0;
    int crop_x = 0;
    int crop_y = 0;
    int rotation = 0;
    bool tile_known = true;
    bool search_phase = false;
    bool search_rotation = false;
};

struct DwtExtractResult {
    Status status = Status::not_detected;
    std::uint8_t payload[kPayloadMax] = {};
    std::size_t payload_length = 0;
    std::uint8_t version = 0;
    std::uint8_t algorithm = 0;
    std::uint8_t corrected_bytes = 0;
    std::uint32_t sync_score = 0;
    int phase_x = 0;
    int phase_y = 0;
    int tile_x = 0;
    int tile_y = 0;
    int coefficient_width = 0;
    int coefficient_height = 0;
};

// Embeds payload_length (0..32) bytes into a copy of input. Input and output
// must use the same pixel format; output is fully written on success. Alpha is
// copied, never used as a carrier. On failure output content is unspecified,
// but input is never modified.
Status dwt_embed(const ImageView& input, const MutableImageView& output,
    const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t* payload, std::size_t payload_length,
    const DwtParameters& parameters = {}, const CancelToken& cancel = {});

// Blind extraction. Only key and geometry are supplied; no original image.
Status dwt_extract(const ImageView& input, const std::uint8_t* key, std::size_t key_length,
    const DwtParameters& parameters, const WatermarkPosition& position,
    DwtExtractResult& result, const CancelToken& cancel = {});

// Builds extraction geometry for an axis-aligned region of the original
// watermarked image. region_x/region_y/region_width/region_height are in
// original (pre-scale) pixels; scale is the factor applied before the region
// was cropped. The attacked image is expected to be the scaled region.
// The helper aligns the 8x8 pixel grid and the 32x32 tile phase to the
// original embedding grid. Rotation/search flags are reset to their defaults.
bool watermark_position_for_region(int region_x, int region_y,
    int region_width, int region_height, double scale,
    WatermarkPosition& position);

// Number of LH3 coefficients available after padding to a multiple of 8.
bool dwt_capacity(int width, int height, const DwtParameters& parameters,
    std::size_t& coefficients);

// Algorithm-owned peak working memory estimate in bytes (does not include the
// caller's input/output pixel buffers).
std::size_t dwt_working_memory_bytes(int width, int height,
    const DwtParameters& parameters);

// Exposed for independent vector tests and future SIMD work. Input dimensions
// must be even at every used level. The sample layout is row-major.
struct DwtPlane {
    int width = 0;
    int height = 0;
    std::vector<float> samples;
};

struct DwtLevel {
    DwtPlane ll;
    DwtPlane lh;
    DwtPlane hl;
    DwtPlane hh;
};

bool dwt_forward(const float* samples, int width, int height, int level_count,
    std::vector<DwtLevel>& levels);
bool dwt_inverse(const std::vector<DwtLevel>& levels, float* samples,
    int width, int height);

// Bicubic resize with the Pillow/libjpeg-style kernel used by the reference pilot
// (a = -0.5, antialiased support for downscaling). Formats must match.
bool resize_bicubic(const ImageView& source, const MutableImageView& destination);

} // namespace av::watermark
