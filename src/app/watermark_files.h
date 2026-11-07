// SPDX-License-Identifier: Apache-2.0
// GUI-independent local image watermark entry: open/choose output, overwrite
// confirmation, DCT/DWT embed and blind extract, copy/export payload, real
// cancellation and status classification. The dialog/file picker is injected
// so the whole flow can be exercised headless.
#pragma once

#include "platform/windows/image_codec.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace av::watermark_files {

constexpr std::size_t max_payload = 32;
constexpr std::size_t max_key = 128;

enum class Algorithm : unsigned {
    auto_detect = 0,
    dct = 1,
    dwt = 2,
    robust_dct = 3
};

enum class StatusCode : unsigned {
    success = 0,
    not_detected = 1,
    corrupted = 2,
    unsupported = 3,
    wrong_key = 4,
    invalid_argument = 5,
    cancelled = 6,
    payload_too_large = 7,
    capacity_exceeded = 8,
    io_error = 9,
    overwrite_required = 10
};

const wchar_t* status_key(StatusCode status) noexcept;
const wchar_t* algorithm_key(Algorithm algorithm) noexcept;

struct CancelFlag {
    CancelFlag() = default;
    CancelFlag(const CancelFlag&) = delete;
    CancelFlag& operator=(const CancelFlag&) = delete;
    void cancel() noexcept { flag_.store(true, std::memory_order_release); }
    bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }

private:
    std::atomic_bool flag_{false};
};

struct EmbedOptions {
    std::wstring input_path;
    std::wstring output_path; // empty -> suggested_output_path(input, algorithm)
    Algorithm algorithm = Algorithm::dct;
    std::vector<std::uint8_t> payload; // UTF-8/binary, 0..32 bytes
    std::vector<std::uint8_t> key;     // 1..128 bytes, never persisted
    double delta = 24.0;
    int jpeg_quality = 90;
};

struct ExtractOptions {
    std::wstring input_path;
    Algorithm algorithm = Algorithm::auto_detect;
    std::vector<std::uint8_t> key;
    double delta = 24.0;
    bool search_tile = true;
    bool search_phase = true;
    double scale = 1.0;
    std::uint32_t original_width = 0;
    std::uint32_t original_height = 0;
    int phase_x = 0;
    int phase_y = 0;
    int tile_x = 0;
    int tile_y = 0;
};

struct FileOutcome {
    StatusCode status = StatusCode::invalid_argument;
    Algorithm algorithm_used = Algorithm::auto_detect;
    std::wstring input_path;
    std::wstring output_path;
    std::uint64_t generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t payload_bytes = 0;
    std::uint8_t corrected_bytes = 0;
    int sync_score = 0;
    bool output_written = false;
    std::vector<std::uint8_t> payload;
    std::wstring detail;

    bool ok() const noexcept { return status == StatusCode::success; }
};

class FileDialogs {
public:
    virtual ~FileDialogs() = default;
    virtual bool open_image(std::wstring& path) = 0;
    virtual bool save_output(std::wstring& path, const std::wstring& suggested_name) = 0;
    virtual bool save_payload(std::wstring& path, const std::wstring& suggested_name) = 0;
    virtual bool confirm_overwrite(const std::wstring& path) = 0;
};

class WatermarkFilesService {
public:
    explicit WatermarkFilesService(ImageCodec& codec);

    FileOutcome embed(const EmbedOptions& options, const CancelFlag& cancel);
    FileOutcome extract(const ExtractOptions& options, const CancelFlag& cancel);

    // Overwrite confirmation state machine. Approval is bound to one exact
    // target path; changing the output requires a new confirmation.
    bool approve_overwrite(const std::wstring& path);
    void clear_overwrite_approval();

    // Writes extracted payload bytes verbatim (not an image, no metadata).
    bool export_payload(const std::wstring& path, const std::vector<std::uint8_t>& payload,
        std::wstring& detail);

    static std::wstring suggested_output_path(const std::wstring& input, Algorithm algorithm);
    static std::size_t payload_bytes_used(const std::string& utf8) noexcept;
    static bool valid_utf8(const std::string& utf8) noexcept;

private:
    ImageCodec& codec_;
    std::mutex approval_mutex_;
    std::wstring approved_path_;
};

class WatermarkFilesController {
public:
    WatermarkFilesController(WatermarkFilesService& service, FileDialogs& dialogs);

    bool open_input();
    bool choose_output();
    FileOutcome embed(const EmbedOptions& options, const CancelFlag& cancel);
    FileOutcome extract(const ExtractOptions& options, const CancelFlag& cancel);
    bool export_payload(const std::vector<std::uint8_t>& payload, std::wstring& detail);

    const std::wstring& input() const noexcept { return input_; }
    const std::wstring& output() const noexcept { return output_; }
    void set_input(std::wstring path) { input_ = std::move(path); }
    void set_output(std::wstring path) { output_ = std::move(path); }

private:
    WatermarkFilesService& service_;
    FileDialogs& dialogs_;
    std::wstring input_;
    std::wstring output_;
};

// Monotonic intent generation. A completion from an older generation must not
// replace the image/result of the newest user intent.
class WatermarkFilesSession {
public:
    WatermarkFilesSession() = default;
    std::uint64_t begin() noexcept { return ++generation_; }
    std::uint64_t current() const noexcept { return generation_; }
    bool apply(FileOutcome outcome);
    std::optional<FileOutcome> last() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> generation_{0};
    std::optional<FileOutcome> last_;
};

enum class JobKind : unsigned {
    embed = 0,
    extract = 1
};

struct JobResult {
    JobKind kind = JobKind::embed;
    std::uint64_t generation = 0;
    FileOutcome outcome;
};

// Runs one watermark job at a time on a worker thread. Starting a new job
// cancels and joins the previous one. `notify` is called on the worker thread
// after the result is stored; a UI must post a message instead of touching
// windows directly.
class WatermarkJobRunner {
public:
    using Notify = void (*)(void* context);

    explicit WatermarkJobRunner(WatermarkFilesService& service);
    ~WatermarkJobRunner();
    WatermarkJobRunner(const WatermarkJobRunner&) = delete;
    WatermarkJobRunner& operator=(const WatermarkJobRunner&) = delete;

    std::uint64_t start_embed(const EmbedOptions& options, std::uint64_t generation,
        Notify notify, void* context);
    std::uint64_t start_extract(const ExtractOptions& options, std::uint64_t generation,
        Notify notify, void* context);
    bool cancel();
    bool busy() const noexcept { return running_.load(std::memory_order_acquire); }
    std::optional<JobResult> take_result();
    void wait();

private:
    void stop_current();
    std::uint64_t start(JobKind kind, EmbedOptions embed, ExtractOptions extract,
        std::uint64_t generation, Notify notify, void* context);

    WatermarkFilesService& service_;
    std::thread worker_;
    std::shared_ptr<CancelFlag> cancel_;
    std::mutex cancel_mutex_;
    std::atomic_bool running_{false};
    std::mutex result_mutex_;
    std::optional<JobResult> result_;
};

} // namespace av::watermark_files
