// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/image_codec.h"

#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <vector>

namespace av::watermark_files {

namespace {

std::wstring lower_extension(const std::wstring& path)
{
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (separator != std::wstring::npos && dot < separator)) {
        return std::wstring();
    }
    std::wstring extension = path.substr(dot + 1);
    if (extension.size() > 8) {
        return std::wstring();
    }
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return extension;
}

bool read_signature(const std::wstring& path, std::uint8_t bytes[16], DWORD& error)
{
    error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes, 16, &read, nullptr);
    const DWORD read_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        error = read_error;
        return false;
    }
    if (read < 2) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    return true;
}

ImageFormat sniff_format(const std::uint8_t bytes[16])
{
    static const std::uint8_t png[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(bytes, png, sizeof(png)) == 0) {
        return ImageFormat::png;
    }
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return ImageFormat::jpeg;
    }
    if (bytes[0] == 'B' && bytes[1] == 'M') {
        return ImageFormat::bmp;
    }
    return ImageFormat::unknown;
}

bool foreign_image_signature(const std::uint8_t bytes[16])
{
    if (std::memcmp(bytes, "GIF87a", 6) == 0 || std::memcmp(bytes, "GIF89a", 6) == 0) {
        return true;
    }
    if ((bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 0x2A && bytes[3] == 0x00)
        || (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0x00 && bytes[3] == 0x2A)) {
        return true;
    }
    if (std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0) {
        return true;
    }
    if (bytes[0] == 0x00 && bytes[1] == 0x00 && (bytes[2] == 0x01 || bytes[2] == 0x02)
        && bytes[3] == 0x00) {
        return true;
    }
    if (bytes[0] == 'P' && bytes[1] >= '1' && bytes[1] <= '6') {
        return true;
    }
    if (std::memcmp(bytes + 4, "ftyp", 4) == 0) {
        return true;
    }
    return false;
}

bool find_encoder_clsid(const wchar_t* mime, CLSID& clsid)
{
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0) {
        return false;
    }
    std::vector<std::uint8_t> buffer(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, size, codecs) != Gdiplus::Ok) {
        return false;
    }
    for (UINT index = 0; index < count; ++index) {
        if (std::wcscmp(codecs[index].MimeType, mime) == 0) {
            clsid = codecs[index].Clsid;
            return true;
        }
    }
    return false;
}

std::wstring temporary_path(const std::wstring& path)
{
    const unsigned long long seed = static_cast<unsigned long long>(GetTickCount64())
        ^ (static_cast<unsigned long long>(GetCurrentProcessId()) << 32);
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const std::wstring candidate = path + L".avtmp-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(seed + attempt);
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return std::wstring();
}

std::wstring error_text(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = size > 0 && buffer != nullptr ? std::wstring(buffer, size) : std::wstring();
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

} // namespace

bool format_from_path(const std::wstring& path, ImageFormat& format) noexcept
{
    format = ImageFormat::unknown;
    const std::wstring extension = lower_extension(path);
    if (extension == L"png") {
        format = ImageFormat::png;
    } else if (extension == L"jpg" || extension == L"jpeg") {
        format = ImageFormat::jpeg;
    } else if (extension == L"bmp") {
        format = ImageFormat::bmp;
    }
    return format != ImageFormat::unknown;
}

const wchar_t* format_name(ImageFormat format) noexcept
{
    switch (format) {
    case ImageFormat::png: return L"PNG";
    case ImageFormat::jpeg: return L"JPEG";
    case ImageFormat::bmp: return L"BMP";
    case ImageFormat::unknown: break;
    }
    return L"unknown";
}

GdiplusImageCodec::GdiplusImageCodec()
{
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
        token_ = token;
        started_ = true;
    }
}

GdiplusImageCodec::~GdiplusImageCodec()
{
    if (started_) {
        Gdiplus::GdiplusShutdown(static_cast<ULONG_PTR>(token_));
    }
}

CodecResult GdiplusImageCodec::load(const std::wstring& path, LoadedImage& image)
{
    CodecResult result;
    image = LoadedImage{};
    if (path.empty()) {
        result.status = CodecStatus::invalid_argument;
        result.detail = L"empty input path";
        return result;
    }
    if (!started_) {
        result.status = CodecStatus::io_error;
        result.detail = L"GDI+ could not be started";
        return result;
    }

    std::uint8_t signature[16] = {};
    DWORD error = ERROR_SUCCESS;
    if (!read_signature(path, signature, error)) {
        result.status = CodecStatus::io_error;
        result.detail = L"cannot read input file: " + error_text(error);
        return result;
    }
    const ImageFormat sniffed = sniff_format(signature);
    if (sniffed == ImageFormat::unknown) {
        result.status = foreign_image_signature(signature)
            ? CodecStatus::unsupported : CodecStatus::corrupted;
        result.detail = result.status == CodecStatus::unsupported
            ? L"only PNG, JPEG and BMP are accepted"
            : L"the file signature is damaged or not an image";
        return result;
    }
    result.format = sniffed;

    Gdiplus::Bitmap source(path.c_str());
    if (source.GetLastStatus() != Gdiplus::Ok) {
        result.status = CodecStatus::corrupted;
        result.detail = L"the image container is damaged or cannot be decoded";
        return result;
    }
    const UINT width = source.GetWidth();
    const UINT height = source.GetHeight();
    if (width == 0 || height == 0) {
        result.status = CodecStatus::corrupted;
        result.detail = L"image has zero width or height";
        return result;
    }
    if (static_cast<std::uint64_t>(width) * height > max_pixels) {
        result.status = CodecStatus::budget_exceeded;
        result.detail = L"image exceeds the 32 Mi pixel budget";
        return result;
    }
    if (static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max()
            / bytes_per_pixel) {
        result.status = CodecStatus::budget_exceeded;
        result.detail = L"image stride overflows";
        return result;
    }

    Gdiplus::Bitmap converted(static_cast<INT>(width), static_cast<INT>(height),
        PixelFormat32bppARGB);
    if (converted.GetLastStatus() != Gdiplus::Ok) {
        result.status = CodecStatus::io_error;
        result.detail = L"cannot allocate the decoded image";
        return result;
    }
    {
        Gdiplus::Graphics graphics(&converted);
        if (graphics.GetLastStatus() != Gdiplus::Ok
            || graphics.DrawImage(&source, 0, 0, static_cast<INT>(width),
                   static_cast<INT>(height)) != Gdiplus::Ok) {
            result.status = CodecStatus::corrupted;
            result.detail = L"cannot convert the image to BGRA8";
            return result;
        }
    }

    image.width = width;
    image.height = height;
    image.stride = static_cast<std::size_t>(width) * bytes_per_pixel;
    image.pixels.assign(image.stride * static_cast<std::size_t>(height), 0);

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData data{};
    if (converted.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data)
        != Gdiplus::Ok) {
        image = LoadedImage{};
        result.status = CodecStatus::io_error;
        result.detail = L"cannot lock the decoded pixels";
        return result;
    }
    const auto* base = static_cast<const std::uint8_t*>(data.Scan0);
    for (UINT y = 0; y < height; ++y) {
        const std::uint8_t* row = base + static_cast<std::ptrdiff_t>(y) * data.Stride;
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * image.stride, row,
            static_cast<std::size_t>(width) * bytes_per_pixel);
    }
    converted.UnlockBits(&data);
    result.status = CodecStatus::success;
    return result;
}

CodecResult GdiplusImageCodec::save(const std::wstring& path, const LoadedImage& image,
    int jpeg_quality)
{
    CodecResult result;
    if (path.empty() || !image.valid()) {
        result.status = CodecStatus::invalid_argument;
        result.detail = L"empty output path or invalid image";
        return result;
    }
    if (!started_) {
        result.status = CodecStatus::io_error;
        result.detail = L"GDI+ could not be started";
        return result;
    }
    ImageFormat format = ImageFormat::unknown;
    if (!format_from_path(path, format)) {
        result.status = CodecStatus::invalid_argument;
        result.detail = L"output extension must be .png, .jpg, .jpeg or .bmp";
        return result;
    }
    result.format = format;
    if (format == ImageFormat::jpeg && (jpeg_quality < 1 || jpeg_quality > 100)) {
        result.status = CodecStatus::invalid_argument;
        result.detail = L"JPEG quality must be 1-100";
        return result;
    }
    if (static_cast<std::uint64_t>(image.width) * image.height > max_pixels) {
        result.status = CodecStatus::budget_exceeded;
        result.detail = L"image exceeds the 32 Mi pixel budget";
        return result;
    }

    Gdiplus::Bitmap bitmap(static_cast<INT>(image.width), static_cast<INT>(image.height),
        static_cast<INT>(image.stride), PixelFormat32bppARGB,
        const_cast<BYTE*>(image.pixels.data()));
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        result.status = CodecStatus::io_error;
        result.detail = L"cannot allocate the output image";
        return result;
    }

    CLSID encoder{};
    const wchar_t* mime = format == ImageFormat::png ? L"image/png"
        : (format == ImageFormat::jpeg ? L"image/jpeg" : L"image/bmp");
    if (!find_encoder_clsid(mime, encoder)) {
        result.status = CodecStatus::unsupported;
        result.detail = L"the system image encoder is missing";
        return result;
    }
    Gdiplus::EncoderParameters parameters{};
    LONG quality = jpeg_quality;
    if (format == ImageFormat::jpeg) {
        parameters.Count = 1;
        parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
        parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        parameters.Parameter[0].NumberOfValues = 1;
        parameters.Parameter[0].Value = &quality;
    }

    const std::wstring temporary = temporary_path(path);
    if (temporary.empty()) {
        result.status = CodecStatus::io_error;
        result.detail = L"cannot create a temporary output name";
        return result;
    }
    if (bitmap.Save(temporary.c_str(), &encoder,
            format == ImageFormat::jpeg ? &parameters : nullptr)
        != Gdiplus::Ok) {
        DeleteFileW(temporary.c_str());
        result.status = CodecStatus::io_error;
        result.detail = L"the image could not be written (check folder permissions)";
        return result;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        result.status = CodecStatus::io_error;
        result.detail = L"the output could not replace the target: " + error_text(error);
        return result;
    }
    result.status = CodecStatus::success;
    return result;
}

} // namespace av::watermark_files
