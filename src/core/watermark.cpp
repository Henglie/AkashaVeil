// SPDX-License-Identifier: Apache-2.0
#include "core/watermark.h"
#include <limits>

namespace av {

std::uint32_t watermark_crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

bool watermark_packet_valid(const WatermarkPacket& packet) noexcept {
    return packet.format_version == 1 && packet.algorithm != 0
        && !packet.payload.empty() && packet.payload.size() <= watermark_max_payload
        && packet.parameters.size() <= watermark_max_parameters;
}

WatermarkPacket make_watermark_packet(std::uint16_t algorithm,
    const std::vector<std::uint8_t>& payload,
    const std::vector<std::uint8_t>& parameters,
    std::uint16_t flags,
    std::uint16_t error_correction,
    std::uint32_t sync_marker) {
    WatermarkPacket packet;
    packet.format_version = 1;
    packet.algorithm = algorithm;
    packet.flags = flags;
    packet.error_correction = error_correction;
    packet.sync_marker = sync_marker;
    packet.payload = payload;
    packet.parameters = parameters;
    return packet;
}

static void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

static void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

static std::uint16_t read_u16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0])
        | static_cast<std::uint16_t>(data[1] << 8);
}

static std::uint32_t read_u32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::vector<std::uint8_t> serialize_watermark_packet(const WatermarkPacket& packet) {
    if (!watermark_packet_valid(packet)) return {};
    std::vector<std::uint8_t> out;
    out.reserve(28 + packet.payload.size() + packet.parameters.size());
    out.push_back('A'); out.push_back('V'); out.push_back('W'); out.push_back('1');
    append_u16(out, packet.format_version);
    append_u16(out, packet.algorithm);
    append_u16(out, packet.flags);
    append_u16(out, packet.error_correction);
    append_u32(out, packet.sync_marker);
    append_u32(out, static_cast<std::uint32_t>(packet.payload.size()));
    append_u32(out, static_cast<std::uint32_t>(packet.parameters.size()));
    out.insert(out.end(), packet.payload.begin(), packet.payload.end());
    out.insert(out.end(), packet.parameters.begin(), packet.parameters.end());
    const auto crc = watermark_crc32(out.data(), out.size());
    append_u32(out, crc);
    return out;
}

const wchar_t* extraction_status_name(ExtractionStatus status) noexcept {
    switch (status) {
    case ExtractionStatus::success: return L"success";
    case ExtractionStatus::not_detected: return L"not_detected";
    case ExtractionStatus::corrupted: return L"corrupted";
    case ExtractionStatus::unsupported_algorithm: return L"unsupported_algorithm";
    case ExtractionStatus::wrong_key: return L"wrong_key";
    case ExtractionStatus::invalid_argument: return L"invalid_argument";
    case ExtractionStatus::budget_exceeded: return L"budget_exceeded";
    }
    return L"invalid_argument";
}

ExtractionStatus parse_watermark_packet(const std::uint8_t* data,
    std::size_t size, WatermarkPacket& packet) noexcept {
    packet = {};
    if (!data || size == 0) return ExtractionStatus::not_detected;
    if (size < 4) return ExtractionStatus::corrupted;
    if (data[0] != 'A' || data[1] != 'V' || data[2] != 'W' || data[3] != '1')
        return ExtractionStatus::not_detected;
    if (size < 28) return ExtractionStatus::corrupted;
    const auto version = read_u16(data + 4);
    if (version != 1) return ExtractionStatus::unsupported_algorithm;
    const auto payload_size = read_u32(data + 16);
    const auto parameter_size = read_u32(data + 20);
    if (payload_size == 0 || payload_size > watermark_max_payload
        || parameter_size > watermark_max_parameters)
        return ExtractionStatus::corrupted;
    const auto expected = 28u + payload_size + parameter_size;
    if (size != expected) return ExtractionStatus::corrupted;
    const auto stored_crc = read_u32(data + size - 4);
    if (watermark_crc32(data, size - 4) != stored_crc) return ExtractionStatus::corrupted;

    WatermarkPacket candidate;
    candidate.format_version = version;
    candidate.algorithm = read_u16(data + 6);
    candidate.flags = read_u16(data + 8);
    candidate.error_correction = read_u16(data + 10);
    candidate.sync_marker = read_u32(data + 12);
    candidate.payload.assign(data + 24, data + 24 + payload_size);
    candidate.parameters.assign(data + 24 + payload_size, data + 24 + payload_size + parameter_size);
    candidate.crc32 = stored_crc;
    if (!watermark_packet_valid(candidate)) return ExtractionStatus::invalid_argument;
    packet = std::move(candidate);
    return ExtractionStatus::success;
}

bool valid_image_view(const ImageView& image) noexcept {
    if (image.format != PixelFormat::bgra8 || image.width == 0 || image.height == 0
        || !image.pixels)
        return false;
    const auto max = std::numeric_limits<std::size_t>::max();
    if (image.width > max / bgra8_bytes_per_pixel) return false;
    const auto minimum_stride = static_cast<std::size_t>(image.width) * bgra8_bytes_per_pixel;
    if (image.stride < minimum_stride) return false;
    return image.height <= max / image.stride;
}

bool valid_image_view(const MutableImageView& image) noexcept {
    return valid_image_view(ImageView{image.width, image.height, image.stride,
        image.format, image.alpha, image.pixels});
}

bool image_views_match(const ImageView& left, const ImageView& right) noexcept {
    return left.width == right.width && left.height == right.height
        && left.format == right.format && left.alpha == right.alpha;
}

std::size_t image_view_bytes(const ImageView& image) noexcept {
    if (!valid_image_view(image)) return 0;
    return image.stride * image.height;
}

ImageBuffer::ImageBuffer(std::uint32_t width, std::uint32_t height,
    std::size_t stride, AlphaMode alpha)
    : width_(width), height_(height), alpha_(alpha) {
    if (width == 0 || height == 0) return;
    const auto minimum = static_cast<std::size_t>(width) * bgra8_bytes_per_pixel;
    if (stride == 0) stride = minimum;
    if (stride < minimum) return;
    const auto max = std::numeric_limits<std::size_t>::max();
    if (height > max / stride) return;
    stride_ = stride;
    try {
        pixels_.assign(stride * height, 0);
    } catch (...) {
        pixels_.clear();
        stride_ = 0;
    }
}

bool ImageBuffer::valid() const noexcept {
    return valid_image_view(view());
}

ImageView ImageBuffer::view() const noexcept {
    return {width_, height_, stride_, PixelFormat::bgra8, alpha_, pixels_.data()};
}

MutableImageView ImageBuffer::mutable_view() noexcept {
    return {width_, height_, stride_, PixelFormat::bgra8, alpha_, pixels_.data()};
}

}

