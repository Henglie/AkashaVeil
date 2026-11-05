// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace av {

constexpr std::size_t watermark_max_payload = 4096;
constexpr std::size_t watermark_max_parameters = 2048;
constexpr std::size_t watermark_max_key = 128;

struct WatermarkPacket {
    std::uint16_t format_version = 1;
    std::uint16_t algorithm = 0;
    std::uint16_t flags = 0;
    std::uint16_t error_correction = 0;
    std::uint32_t sync_marker = 0;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> parameters;
    std::uint32_t crc32 = 0;
};

std::uint32_t watermark_crc32(const std::uint8_t* data, std::size_t size) noexcept;
bool watermark_packet_valid(const WatermarkPacket& packet) noexcept;
WatermarkPacket make_watermark_packet(std::uint16_t algorithm,
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::uint8_t>& parameters = {},
    std::uint16_t flags = 0,
    std::uint16_t error_correction = 0,
    std::uint32_t sync_marker = 0);

std::vector<std::uint8_t> serialize_watermark_packet(const WatermarkPacket& packet);

enum class ExtractionStatus : unsigned {
    success = 0,
    not_detected = 1,
    corrupted = 2,
    unsupported_algorithm = 3,
    wrong_key = 4,
    invalid_argument = 5,
    budget_exceeded = 6
};

const wchar_t* extraction_status_name(ExtractionStatus status) noexcept;

ExtractionStatus parse_watermark_packet(const std::uint8_t* data,
    std::size_t size, WatermarkPacket& packet) noexcept;

struct ExtractionResult {
    ExtractionStatus status = ExtractionStatus::not_detected;
    WatermarkPacket packet;
};

enum class PixelFormat : unsigned {
    bgra8 = 0
};

enum class AlphaMode : unsigned {
    straight = 0,
    premultiplied = 1,
    opaque = 2
};

constexpr std::size_t bgra8_bytes_per_pixel = 4;

struct ImageView {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
    PixelFormat format = PixelFormat::bgra8;
    AlphaMode alpha = AlphaMode::straight;
    const std::uint8_t* pixels = nullptr;
};

struct MutableImageView {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
    PixelFormat format = PixelFormat::bgra8;
    AlphaMode alpha = AlphaMode::straight;
    std::uint8_t* pixels = nullptr;
};

bool valid_image_view(const ImageView& image) noexcept;
bool valid_image_view(const MutableImageView& image) noexcept;
bool image_views_match(const ImageView& left, const ImageView& right) noexcept;
std::size_t image_view_bytes(const ImageView& image) noexcept;

class ImageBuffer {
public:
    ImageBuffer() = default;
    ImageBuffer(std::uint32_t width, std::uint32_t height,
        std::size_t stride = 0, AlphaMode alpha = AlphaMode::straight);

    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::size_t stride() const noexcept { return stride_; }
    AlphaMode alpha() const noexcept { return alpha_; }
    bool valid() const noexcept;
    ImageView view() const noexcept;
    MutableImageView mutable_view() noexcept;
    const std::vector<std::uint8_t>& bytes() const noexcept { return pixels_; }
    std::vector<std::uint8_t>& bytes() noexcept { return pixels_; }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t stride_ = 0;
    AlphaMode alpha_ = AlphaMode::straight;
    std::vector<std::uint8_t> pixels_;
};

}
