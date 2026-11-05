// SPDX-License-Identifier: Apache-2.0
// AkashaVeil DCT-family blind watermark: 8x8 DCT-II, C[1,2]/C[2,1] pair QIM,
// 32x32 tile shared with the watermark protocol. Pure C++17, no OS/GUI dependency.
#pragma once

#include <cstddef>
#include <cstdint>

#include "av/watermark/types.h"

namespace av::watermark::dct {

constexpr int kBlock = 8;
constexpr int kTile = 32;
constexpr std::size_t kTileCells = 1024;
constexpr std::size_t kMinBlocks = 2 * kTile * kTile;
constexpr double kDefaultDelta = 24.0;
constexpr std::size_t kMaxPixels = 32ull * 1024ull * 1024ull;

struct ImageView {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
    const std::uint8_t* pixels = nullptr;
};

struct MutableImageView {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
    std::uint8_t* pixels = nullptr;
};

struct CancelToken {
    bool (*fn)(void*) = nullptr;
    void* context = nullptr;
    bool requested() const noexcept { return fn != nullptr && fn(context); }
};

struct EmbedOptions {
    double delta = kDefaultDelta;
    CancelToken cancel;
    // 1 = caller thread only. >1 splits independent 8x8 block rows across
    // workers. The cancellation callback must be thread-safe when >1.
    std::uint32_t worker_threads = 1;
};

struct EmbedResult {
    Status status = Status::not_detected;
    std::uint32_t blocks_x = 0;
    std::uint32_t blocks_y = 0;
    std::uint32_t min_carriers = 0;
    std::uint32_t max_carriers = 0;
};

struct ExtractOptions {
    double delta = kDefaultDelta;
    int phase_x = 0;
    int phase_y = 0;
    int tile_x = 0;
    int tile_y = 0;
    bool search_tile = true;
    bool search_pixel_phase = false;
    double scale = 1.0;
    std::uint32_t original_width = 0;
    std::uint32_t original_height = 0;
    // Optional unsharp-mask strength (0..2) applied to BGR before geometry.
    // Useful for print/scan-style softening; 0 keeps the input untouched.
    double sharpen = 0.0;
    // Clockwise degrees applied to the input before extraction (undo an
    // attack). Multiples of 90 are exact; other angles use bicubic sampling.
    double rotation_degrees = 0.0;
    // Try a small fixed set of rotation angles around zero. Expensive; off by
    // default. When search_pixel_phase is also true the two searches multiply.
    bool search_rotation = false;
    // 1 = caller thread only. >1 splits block rows for each phase. The
    // cancellation callback must be thread-safe when >1.
    std::uint32_t worker_threads = 1;
    CancelToken cancel;
};

struct ExtractResult {
    Status status = Status::not_detected;
    std::uint8_t version = 0;
    std::uint8_t algorithm = 0;
    std::uint8_t payload[kPayloadMax] = {};
    std::uint8_t payload_length = 0;
    std::uint8_t corrected_bytes = 0;
    int sync_score = 0;
    int tile_x = 0;
    int tile_y = 0;
    int phase_x = 0;
    int phase_y = 0;
    double rotation_degrees = 0.0;
};

bool valid_image(const ImageView& image) noexcept;
bool valid_image(const MutableImageView& image) noexcept;
bool valid_delta(double delta) noexcept;

// Embed into an in-place BGRA8 image. Validates packet, capacity and delta
// before touching pixels. A mid-run cancellation (or worker startup failure)
// can leave partial writes; callers must pass a disposable working frame
// (documented in integration.md).
Status embed(const MutableImageView& image, const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t* payload, std::size_t payload_length,
    const EmbedOptions& options, EmbedResult& result) noexcept;

// Blind extraction. Never uses the original image. scale/original_* are
// explicit geometry hints; the image is upsampled internally when scale != 1.
ExtractResult extract(const ImageView& image, const std::uint8_t* key, std::size_t key_length,
    const ExtractOptions& options) noexcept;

// Keys cubic (a = -0.5) BGRA8 resampler with antialiasing on downscale.
// Used by the scale attack and by extract() for scale != 1.
bool resize_bicubic_bgra(const ImageView& source, std::uint32_t width, std::uint32_t height,
    std::uint8_t* destination, std::size_t destination_stride) noexcept;

// Rotated destination dimensions. 90/270 swap width and height; other angles
// keep the source dimensions (centre crop). Exactly 90/180/270 use a lossless
// pixel remap. Other angles use Keys cubic sampling. destination must be a
// distinct buffer; in-place rotation is rejected.
void rotated_dimensions(std::uint32_t width, std::uint32_t height, double degrees,
    std::uint32_t& out_width, std::uint32_t& out_height) noexcept;
bool rotate_bicubic_bgra(const ImageView& source, double degrees,
    std::uint8_t* destination, std::size_t destination_stride) noexcept;

} // namespace av::watermark::dct
