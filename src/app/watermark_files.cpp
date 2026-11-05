// SPDX-License-Identifier: Apache-2.0
#include "app/watermark_files.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <utility>

#include "av/watermark/dct.h"
#include "av/watermark/dwt.h"
#include "av/watermark/types.h"

namespace av::watermark_files {

namespace {

std::wstring widen_ascii(const char* text)
{
    std::wstring out;
    if (text == nullptr) {
        return out;
    }
    while (*text != '\0') {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    }
    return out;
}

StatusCode map_watermark_status(av::watermark::Status status) noexcept
{
    using av::watermark::Status;
    switch (status) {
    case Status::success: return StatusCode::success;
    case Status::not_detected: return StatusCode::not_detected;
    case Status::corrupted: return StatusCode::corrupted;
    case Status::unsupported_version:
    case Status::unsupported_algorithm: return StatusCode::unsupported;
    case Status::wrong_key: return StatusCode::wrong_key;
    case Status::invalid_argument: return StatusCode::invalid_argument;
    case Status::cancelled: return StatusCode::cancelled;
    case Status::payload_too_large: return StatusCode::payload_too_large;
    case Status::capacity_exceeded: return StatusCode::capacity_exceeded;
    }
    return StatusCode::invalid_argument;
}

StatusCode map_codec_status(CodecStatus status) noexcept
{
    switch (status) {
    case CodecStatus::success: return StatusCode::success;
    case CodecStatus::unsupported: return StatusCode::unsupported;
    case CodecStatus::corrupted: return StatusCode::corrupted;
    case CodecStatus::io_error: return StatusCode::io_error;
    case CodecStatus::budget_exceeded: return StatusCode::capacity_exceeded;
    case CodecStatus::invalid_argument: return StatusCode::invalid_argument;
    }
    return StatusCode::invalid_argument;
}

bool cancel_requested(void* context)
{
    return static_cast<const CancelFlag*>(context)->cancelled();
}

std::wstring full_path(const std::wstring& path)
{
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return path;
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
    if (written == 0 || written >= needed) {
        return path;
    }
    buffer.resize(written);
    return buffer;
}

bool same_path(const std::wstring& left, const std::wstring& right)
{
    const std::wstring a = full_path(left);
    const std::wstring b = full_path(right);
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool output_exists(const std::wstring& path, bool& is_directory)
{
    is_directory = false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return !is_directory;
}

bool valid_delta(double delta, Algorithm algorithm) noexcept
{
    if (!std::isfinite(delta)) {
        return false;
    }
    if (algorithm == Algorithm::dwt) {
        return delta == 24.0;
    }
    return delta >= 1.0 && delta <= 255.0;
}

bool valid_extract_geometry(const ExtractOptions& options) noexcept
{
    if (!std::isfinite(options.scale) || options.scale <= 0.0) {
        return false;
    }
    if (options.scale != 1.0
        && (options.original_width == 0 || options.original_height == 0)) {
        return false;
    }
    if (options.phase_x < 0 || options.phase_x > 7 || options.phase_y < 0
        || options.phase_y > 7) {
        return false;
    }
    if (options.tile_x < 0 || options.tile_x > 31 || options.tile_y < 0
        || options.tile_y > 31) {
        return false;
    }
    return true;
}

int status_rank(StatusCode status) noexcept
{
    switch (status) {
    case StatusCode::wrong_key: return 70;
    case StatusCode::corrupted: return 60;
    case StatusCode::unsupported: return 50;
    case StatusCode::not_detected: return 40;
    case StatusCode::capacity_exceeded: return 30;
    case StatusCode::invalid_argument: return 20;
    case StatusCode::cancelled: return 100;
    case StatusCode::success: return 110;
    case StatusCode::payload_too_large: return 90;
    case StatusCode::io_error: return 80;
    case StatusCode::overwrite_required: return 0;
    }
    return 0;
}

void copy_protocol_payload(FileOutcome& outcome, const std::uint8_t* payload,
    std::size_t length, std::uint8_t algorithm)
{
    const std::size_t count = std::min(length, max_payload);
    if (count > 0) {
        outcome.payload.assign(payload, payload + count);
    }
    outcome.payload_bytes = static_cast<std::uint8_t>(count);
    outcome.algorithm_used = algorithm == av::watermark::kAlgorithmDwtLh3Qim
        ? Algorithm::dwt : Algorithm::dct;
}

FileOutcome extract_one(const LoadedImage& image,
    const ExtractOptions& options, const CancelFlag& cancel, Algorithm algorithm)
{
    FileOutcome outcome;
    outcome.input_path = options.input_path;
    outcome.algorithm_used = algorithm;
    outcome.width = image.width;
    outcome.height = image.height;

    if (algorithm == Algorithm::dct) {
        av::watermark::dct::ImageView view;
        view.width = image.width;
        view.height = image.height;
        view.stride = image.stride;
        view.pixels = image.pixels.data();
        av::watermark::dct::ExtractOptions extract_options;
        extract_options.delta = options.delta;
        extract_options.phase_x = options.phase_x;
        extract_options.phase_y = options.phase_y;
        extract_options.tile_x = options.tile_x;
        extract_options.tile_y = options.tile_y;
        extract_options.search_tile = options.search_tile;
        extract_options.search_pixel_phase = options.search_phase;
        extract_options.scale = options.scale;
        extract_options.original_width = options.original_width;
        extract_options.original_height = options.original_height;
        extract_options.cancel = {cancel_requested, const_cast<CancelFlag*>(&cancel)};
        const auto result = av::watermark::dct::extract(view, options.key.data(),
            options.key.size(), extract_options);
        outcome.status = map_watermark_status(result.status);
        outcome.sync_score = result.sync_score;
        outcome.corrected_bytes = result.corrected_bytes;
        if (result.status == av::watermark::Status::success) {
            copy_protocol_payload(outcome, result.payload, result.payload_length,
                result.algorithm);
        }
        return outcome;
    }

    av::watermark::ImageView view;
    view.pixels = image.pixels.data();
    view.width = static_cast<int>(image.width);
    view.height = static_cast<int>(image.height);
    view.stride = static_cast<int>(image.stride);
    view.format = av::watermark::PixelFormat::bgra8;
    av::watermark::DwtParameters parameters;
    parameters.delta = options.delta;
    av::watermark::WatermarkPosition position;
    position.phase_x = options.phase_x;
    position.phase_y = options.phase_y;
    position.tile_x = options.tile_x;
    position.tile_y = options.tile_y;
    position.scale = options.scale;
    position.original_width = static_cast<int>(options.original_width);
    position.original_height = static_cast<int>(options.original_height);
    position.tile_known = !options.search_tile;
    position.search_phase = options.search_phase;
    av::watermark::CancelToken token;
    token.callback = cancel_requested;
    token.context = const_cast<CancelFlag*>(&cancel);
    av::watermark::DwtExtractResult result;
    const auto status = av::watermark::dwt_extract(view, options.key.data(),
        options.key.size(), parameters, position, result, token);
    outcome.status = map_watermark_status(status);
    outcome.sync_score = static_cast<int>(result.sync_score);
    outcome.corrected_bytes = result.corrected_bytes;
    if (status == av::watermark::Status::success) {
        copy_protocol_payload(outcome, result.payload, result.payload_length,
            result.algorithm);
    }
    return outcome;
}

std::wstring temporary_payload_path(const std::wstring& path)
{
    const unsigned long long seed = static_cast<unsigned long long>(GetTickCount64())
        ^ (static_cast<unsigned long long>(GetCurrentProcessId()) << 32);
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const std::wstring candidate = path + L".avtmp-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(seed + attempt);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return std::wstring();
}

} // namespace

const wchar_t* status_key(StatusCode status) noexcept
{
    switch (status) {
    case StatusCode::success: return L"watermark.status.success";
    case StatusCode::not_detected: return L"watermark.status.not_detected";
    case StatusCode::corrupted: return L"watermark.status.corrupted";
    case StatusCode::unsupported: return L"watermark.status.unsupported";
    case StatusCode::wrong_key: return L"watermark.status.wrong_key";
    case StatusCode::invalid_argument: return L"watermark.status.invalid_argument";
    case StatusCode::cancelled: return L"watermark.status.cancelled";
    case StatusCode::payload_too_large: return L"watermark.status.payload_too_large";
    case StatusCode::capacity_exceeded: return L"watermark.status.capacity_exceeded";
    case StatusCode::io_error: return L"watermark.status.io_error";
    case StatusCode::overwrite_required: return L"watermark.status.overwrite_required";
    }
    return L"watermark.status.invalid_argument";
}

const wchar_t* algorithm_key(Algorithm algorithm) noexcept
{
    switch (algorithm) {
    case Algorithm::auto_detect: return L"watermark.algorithm.auto";
    case Algorithm::dct: return L"watermark.algorithm.dct";
    case Algorithm::dwt: return L"watermark.algorithm.dwt";
    }
    return L"watermark.algorithm.auto";
}

WatermarkFilesService::WatermarkFilesService(ImageCodec& codec)
    : codec_(codec)
{
}

std::wstring WatermarkFilesService::suggested_output_path(const std::wstring& input,
    Algorithm algorithm)
{
    (void)algorithm;
    const std::size_t separator = input.find_last_of(L"\\/");
    const std::size_t dot = input.find_last_of(L'.');
    if (dot == std::wstring::npos || (separator != std::wstring::npos && dot < separator + 2)) {
        return input + L"_wm.png";
    }
    return input.substr(0, dot) + L"_wm" + input.substr(dot);
}

std::size_t WatermarkFilesService::payload_bytes_used(const std::string& utf8) noexcept
{
    return utf8.size();
}

bool WatermarkFilesService::valid_utf8(const std::string& utf8) noexcept
{
    if (utf8.empty()) {
        return true;
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    return count > 0;
}

bool WatermarkFilesService::approve_overwrite(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(approval_mutex_);
    approved_path_ = full_path(path);
    return true;
}

void WatermarkFilesService::clear_overwrite_approval()
{
    std::lock_guard<std::mutex> lock(approval_mutex_);
    approved_path_.clear();
}

FileOutcome WatermarkFilesService::embed(const EmbedOptions& options, const CancelFlag& cancel)
{
    FileOutcome outcome;
    outcome.input_path = options.input_path;
    outcome.output_path = options.output_path.empty()
        ? suggested_output_path(options.input_path, options.algorithm)
        : options.output_path;

    if (options.input_path.empty() || outcome.output_path.empty()) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"input and output paths are required";
        return outcome;
    }
    if (options.algorithm != Algorithm::dct && options.algorithm != Algorithm::dwt) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"embedding needs an explicit DCT or DWT algorithm";
        return outcome;
    }
    if (options.payload.size() > max_payload) {
        outcome.status = StatusCode::payload_too_large;
        outcome.detail = L"payload exceeds 32 UTF-8 bytes";
        return outcome;
    }
    if (options.key.empty() || options.key.size() > max_key) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"key must be 1-128 bytes";
        return outcome;
    }
    if (!valid_delta(options.delta, options.algorithm)) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = options.algorithm == Algorithm::dwt
            ? L"DWT delta is frozen at 24" : L"DCT delta must be 1-255";
        return outcome;
    }
    ImageFormat format = ImageFormat::unknown;
    if (!format_from_path(outcome.output_path, format)) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"output extension must be .png, .jpg, .jpeg or .bmp";
        return outcome;
    }
    if (format == ImageFormat::jpeg
        && (options.jpeg_quality < 1 || options.jpeg_quality > 100)) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"JPEG quality must be 1-100";
        return outcome;
    }
    if (same_path(options.input_path, outcome.output_path)) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"the source file cannot be overwritten";
        return outcome;
    }
    if (cancel.cancelled()) {
        outcome.status = StatusCode::cancelled;
        return outcome;
    }
    bool is_directory = false;
    if (output_exists(outcome.output_path, is_directory)) {
        if (is_directory) {
            outcome.status = StatusCode::invalid_argument;
            outcome.detail = L"output path is a directory";
            return outcome;
        }
        bool approved = false;
        {
            std::lock_guard<std::mutex> lock(approval_mutex_);
            approved = !approved_path_.empty()
                && CompareStringOrdinal(approved_path_.c_str(), -1,
                       full_path(outcome.output_path).c_str(), -1, TRUE) == CSTR_EQUAL;
        }
        if (!approved) {
            outcome.status = StatusCode::overwrite_required;
            outcome.detail = L"the output file exists; explicit confirmation is required";
            return outcome;
        }
    }
    if (cancel.cancelled()) {
        outcome.status = StatusCode::cancelled;
        return outcome;
    }

    LoadedImage image;
    const CodecResult loaded = codec_.load(options.input_path, image);
    if (!loaded.ok()) {
        outcome.status = map_codec_status(loaded.status);
        outcome.detail = loaded.detail;
        return outcome;
    }
    outcome.width = image.width;
    outcome.height = image.height;
    if (static_cast<std::uint64_t>(image.width) * image.height > max_pixels) {
        outcome.status = StatusCode::capacity_exceeded;
        outcome.detail = L"image exceeds the 32 Mi pixel budget";
        return outcome;
    }

    std::vector<std::uint8_t> payload = options.payload;
    const std::uint8_t* payload_data = payload.empty() ? nullptr : payload.data();

    if (options.algorithm == Algorithm::dct) {
        av::watermark::dct::MutableImageView view;
        view.width = image.width;
        view.height = image.height;
        view.stride = image.stride;
        view.pixels = image.pixels.data();
        av::watermark::dct::EmbedOptions embed_options;
        embed_options.delta = options.delta;
        embed_options.cancel = {cancel_requested, const_cast<CancelFlag*>(&cancel)};
        av::watermark::dct::EmbedResult result;
        const auto status = av::watermark::dct::embed(view, options.key.data(),
            options.key.size(), payload_data, payload.size(), embed_options, result);
        outcome.status = map_watermark_status(status);
        if (status != av::watermark::Status::success) {
            outcome.detail = widen_ascii(av::watermark::status_name(status));
            return outcome;
        }
        outcome.algorithm_used = Algorithm::dct;
    } else {
        LoadedImage output = image;
        av::watermark::ImageView input_view;
        input_view.pixels = image.pixels.data();
        input_view.width = static_cast<int>(image.width);
        input_view.height = static_cast<int>(image.height);
        input_view.stride = static_cast<int>(image.stride);
        input_view.format = av::watermark::PixelFormat::bgra8;
        av::watermark::MutableImageView output_view;
        output_view.pixels = output.pixels.data();
        output_view.width = static_cast<int>(output.width);
        output_view.height = static_cast<int>(output.height);
        output_view.stride = static_cast<int>(output.stride);
        output_view.format = av::watermark::PixelFormat::bgra8;
        av::watermark::DwtParameters parameters;
        parameters.delta = options.delta;
        av::watermark::CancelToken token;
        token.callback = cancel_requested;
        token.context = const_cast<CancelFlag*>(&cancel);
        const auto status = av::watermark::dwt_embed(input_view, output_view,
            options.key.data(), options.key.size(), payload_data, payload.size(),
            parameters, token);
        outcome.status = map_watermark_status(status);
        if (status != av::watermark::Status::success) {
            outcome.detail = widen_ascii(av::watermark::status_name(status));
            return outcome;
        }
        image.pixels.swap(output.pixels);
        outcome.algorithm_used = Algorithm::dwt;
    }
    outcome.payload_bytes = static_cast<std::uint8_t>(payload.size());
    std::fill(payload.begin(), payload.end(), static_cast<std::uint8_t>(0));
    if (cancel.cancelled()) {
        outcome.status = StatusCode::cancelled;
        return outcome;
    }

    const CodecResult saved = codec_.save(outcome.output_path, image, options.jpeg_quality);
    if (!saved.ok()) {
        outcome.status = map_codec_status(saved.status);
        outcome.detail = saved.detail;
        return outcome;
    }
    clear_overwrite_approval();
    outcome.status = StatusCode::success;
    outcome.output_written = true;
    return outcome;
}

FileOutcome WatermarkFilesService::extract(const ExtractOptions& options,
    const CancelFlag& cancel)
{
    FileOutcome outcome;
    outcome.input_path = options.input_path;
    if (options.input_path.empty()) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"input path is required";
        return outcome;
    }
    if (options.key.empty() || options.key.size() > max_key) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"key must be 1-128 bytes";
        return outcome;
    }
    if (options.algorithm != Algorithm::auto_detect && options.algorithm != Algorithm::dct
        && options.algorithm != Algorithm::dwt) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"unknown extraction algorithm";
        return outcome;
    }
    if (!valid_delta(options.delta, options.algorithm == Algorithm::dwt
            ? Algorithm::dwt : Algorithm::dct)) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"invalid delta";
        return outcome;
    }
    if (!valid_extract_geometry(options)) {
        outcome.status = StatusCode::invalid_argument;
        outcome.detail = L"invalid extraction geometry";
        return outcome;
    }
    if (cancel.cancelled()) {
        outcome.status = StatusCode::cancelled;
        return outcome;
    }

    LoadedImage image;
    const CodecResult loaded = codec_.load(options.input_path, image);
    if (!loaded.ok()) {
        outcome.status = map_codec_status(loaded.status);
        outcome.detail = loaded.detail;
        return outcome;
    }
    outcome.width = image.width;
    outcome.height = image.height;
    if (static_cast<std::uint64_t>(image.width) * image.height > max_pixels) {
        outcome.status = StatusCode::capacity_exceeded;
        outcome.detail = L"image exceeds the 32 Mi pixel budget";
        return outcome;
    }

    if (options.algorithm == Algorithm::dct || options.algorithm == Algorithm::dwt) {
        return extract_one(image, options, cancel, options.algorithm);
    }

    FileOutcome dct = extract_one(image, options, cancel, Algorithm::dct);
    if (dct.status == StatusCode::cancelled) {
        return dct;
    }
    FileOutcome dwt = extract_one(image, options, cancel, Algorithm::dwt);
    if (dwt.status == StatusCode::cancelled) {
        return dwt;
    }
    if (dct.status == StatusCode::success) {
        return dct;
    }
    if (dwt.status == StatusCode::success) {
        return dwt;
    }
    return status_rank(dwt.status) > status_rank(dct.status) ? dwt : dct;
}

bool WatermarkFilesService::export_payload(const std::wstring& path,
    const std::vector<std::uint8_t>& payload, std::wstring& detail)
{
    detail.clear();
    if (path.empty()) {
        detail = L"export path is required";
        return false;
    }
    const std::wstring temporary = temporary_payload_path(path);
    if (temporary.empty()) {
        detail = L"cannot create a temporary export name";
        return false;
    }
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        detail = L"cannot create the export file";
        return false;
    }
    bool ok = true;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            payload.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, payload.data() + offset, chunk, &written, nullptr)
            || written != chunk) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && payload.empty()) {
        ok = true;
    }
    if (ok) {
        ok = FlushFileBuffers(file) != FALSE;
    }
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        detail = L"the export could not be written";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        detail = L"the export could not replace the target";
        return false;
    }
    return true;
}

WatermarkFilesController::WatermarkFilesController(WatermarkFilesService& service,
    FileDialogs& dialogs)
    : service_(service)
    , dialogs_(dialogs)
{
}

bool WatermarkFilesController::open_input()
{
    std::wstring path;
    if (!dialogs_.open_image(path) || path.empty()) {
        return false;
    }
    input_ = std::move(path);
    service_.clear_overwrite_approval();
    return true;
}

bool WatermarkFilesController::choose_output()
{
    if (input_.empty()) {
        return false;
    }
    std::wstring suggested = WatermarkFilesService::suggested_output_path(input_,
        Algorithm::dct);
    const std::size_t separator = suggested.find_last_of(L"\\/");
    const std::wstring name = separator == std::wstring::npos
        ? suggested : suggested.substr(separator + 1);
    std::wstring path;
    if (!dialogs_.save_output(path, name) || path.empty()) {
        return false;
    }
    output_ = std::move(path);
    service_.clear_overwrite_approval();
    return true;
}

FileOutcome WatermarkFilesController::embed(const EmbedOptions& request,
    const CancelFlag& cancel)
{
    EmbedOptions options = request;
    if (options.input_path.empty()) {
        options.input_path = input_;
    }
    if (options.output_path.empty()) {
        options.output_path = output_.empty()
            ? WatermarkFilesService::suggested_output_path(options.input_path, options.algorithm)
            : output_;
    }
    FileOutcome outcome = service_.embed(options, cancel);
    if (outcome.status != StatusCode::overwrite_required) {
        return outcome;
    }
    if (!dialogs_.confirm_overwrite(outcome.output_path)) {
        return outcome;
    }
    if (!service_.approve_overwrite(outcome.output_path)) {
        return outcome;
    }
    return service_.embed(options, cancel);
}

FileOutcome WatermarkFilesController::extract(const ExtractOptions& request,
    const CancelFlag& cancel)
{
    ExtractOptions options = request;
    if (options.input_path.empty()) {
        options.input_path = input_;
    }
    return service_.extract(options, cancel);
}

bool WatermarkFilesController::export_payload(const std::vector<std::uint8_t>& payload,
    std::wstring& detail)
{
    std::wstring path;
    if (!dialogs_.save_payload(path, L"watermark-payload.bin") || path.empty()) {
        detail = L"export cancelled";
        return false;
    }
    return service_.export_payload(path, payload, detail);
}

bool WatermarkFilesSession::apply(FileOutcome outcome)
{
    if (outcome.generation != generation_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    last_ = std::move(outcome);
    return true;
}

std::optional<FileOutcome> WatermarkFilesSession::last() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

void WatermarkFilesSession::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    last_.reset();
}

WatermarkJobRunner::WatermarkJobRunner(WatermarkFilesService& service)
    : service_(service)
{
}

WatermarkJobRunner::~WatermarkJobRunner()
{
    stop_current();
}

void WatermarkJobRunner::stop_current()
{
    std::shared_ptr<CancelFlag> current;
    {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        current = cancel_;
    }
    if (current) {
        current->cancel();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        result_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        cancel_.reset();
    }
}

void WatermarkJobRunner::wait()
{
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
}

std::uint64_t WatermarkJobRunner::start_embed(const EmbedOptions& options,
    std::uint64_t generation, Notify notify, void* context)
{
    return start(JobKind::embed, options, ExtractOptions{}, generation, notify, context);
}

std::uint64_t WatermarkJobRunner::start_extract(const ExtractOptions& options,
    std::uint64_t generation, Notify notify, void* context)
{
    return start(JobKind::extract, EmbedOptions{}, options, generation, notify, context);
}

std::uint64_t WatermarkJobRunner::start(JobKind kind, EmbedOptions embed,
    ExtractOptions extract, std::uint64_t generation, Notify notify, void* context)
{
    stop_current();
    auto cancel = std::make_shared<CancelFlag>();
    {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        cancel_ = cancel;
    }
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread([this, kind, embed = std::move(embed),
                                  extract = std::move(extract), generation, notify,
                                  context, cancel]() {
            FileOutcome outcome;
            try {
                outcome = kind == JobKind::embed ? service_.embed(embed, *cancel)
                                                  : service_.extract(extract, *cancel);
            } catch (const std::bad_alloc&) {
                outcome.status = StatusCode::io_error;
                outcome.detail = L"out of memory";
            } catch (const std::exception&) {
                outcome.status = StatusCode::invalid_argument;
                outcome.detail = L"unexpected processing failure";
            }
            outcome.generation = generation;
            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                result_ = JobResult{kind, generation, std::move(outcome)};
            }
            running_.store(false, std::memory_order_release);
            if (notify != nullptr) {
                notify(context);
            }
        });
    } catch (const std::exception&) {
        running_.store(false, std::memory_order_release);
        FileOutcome outcome;
        outcome.status = StatusCode::io_error;
        outcome.detail = L"cannot start the watermark worker";
        outcome.generation = generation;
        std::lock_guard<std::mutex> lock(result_mutex_);
        result_ = JobResult{kind, generation, std::move(outcome)};
    }
    return generation;
}

bool WatermarkJobRunner::cancel()
{
    const bool was_running = running_.load(std::memory_order_acquire);
    std::shared_ptr<CancelFlag> current;
    {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        current = cancel_;
    }
    if (current) {
        current->cancel();
    }
    return was_running;
}

std::optional<JobResult> WatermarkJobRunner::take_result()
{
    std::lock_guard<std::mutex> lock(result_mutex_);
    if (!result_) {
        return std::nullopt;
    }
    std::optional<JobResult> result = std::move(result_);
    result_.reset();
    return result;
}

} // namespace av::watermark_files
