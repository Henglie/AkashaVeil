// SPDX-License-Identifier: Apache-2.0
// Local PNG/JPEG/BMP image I/O for the file watermark entry.
// Decoding/encoding only; watermark logic lives in app/watermark_files.*.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace av::watermark_files {

constexpr std::size_t bytes_per_pixel = 4;
constexpr std::uint64_t max_pixels = 32ull * 1024ull * 1024ull;

enum class ImageFormat : unsigned {
    unknown = 0,
    png = 1,
    jpeg = 2,
    bmp = 3
};

struct LoadedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
    std::vector<std::uint8_t> pixels;

    bool valid() const noexcept
    {
        return width > 0 && height > 0 && stride >= static_cast<std::size_t>(width) * bytes_per_pixel
            && pixels.size() >= stride * static_cast<std::size_t>(height);
    }
};

enum class CodecStatus : unsigned {
    success = 0,
    unsupported = 1,
    corrupted = 2,
    io_error = 3,
    budget_exceeded = 4,
    invalid_argument = 5
};

struct CodecResult {
    CodecStatus status = CodecStatus::invalid_argument;
    ImageFormat format = ImageFormat::unknown;
    std::wstring detail;

    bool ok() const noexcept { return status == CodecStatus::success; }
};

// Extension without dot, lower-cased. Returns false when the path has no
// extension or the extension is not a supported output container.
bool format_from_path(const std::wstring& path, ImageFormat& format) noexcept;
const wchar_t* format_name(ImageFormat format) noexcept;

class ImageCodec {
public:
    virtual ~ImageCodec() = default;
    virtual CodecResult load(const std::wstring& path, LoadedImage& image) = 0;
    virtual CodecResult save(const std::wstring& path, const LoadedImage& image,
        int jpeg_quality = 90) = 0;
};

// GDI+ implementation. PNG/JPEG/BMP containers are accepted by signature;
// other containers are rejected as unsupported even if the system codec can
// decode them. Existing output files are replaced through a same-directory
// temporary file, so a failed save does not truncate the previous output.
class GdiplusImageCodec final : public ImageCodec {
public:
    GdiplusImageCodec();
    ~GdiplusImageCodec() override;
    GdiplusImageCodec(const GdiplusImageCodec&) = delete;
    GdiplusImageCodec& operator=(const GdiplusImageCodec&) = delete;

    CodecResult load(const std::wstring& path, LoadedImage& image) override;
    CodecResult save(const std::wstring& path, const LoadedImage& image,
        int jpeg_quality = 90) override;

private:
    unsigned long long token_ = 0;
    bool started_ = false;
};

} // namespace av::watermark_files
