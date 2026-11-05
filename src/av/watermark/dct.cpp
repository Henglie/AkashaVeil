// SPDX-License-Identifier: Apache-2.0
#include "av/watermark/dct.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "av/watermark/packet.h"
#include "av/watermark/sync.h"

namespace av::watermark::dct {

namespace {

constexpr std::size_t kResizeTempLimit = 256ull * 1024ull * 1024ull;
constexpr double kLumaScale = 65536.0;

struct Basis {
    // diff[p] = M[1,x]*M[2,y] - M[2,x]*M[1,y]; the pair difference is enough
    // for both scoring and the symmetric QIM update.
    double diff[64];
    double diff_scaled[64];
};

const Basis& basis() noexcept
{
    static const Basis value = [] {
        Basis result{};
        constexpr double pi = 3.14159265358979323846;
        double m1[8];
        double m2[8];
        for (int x = 0; x < 8; ++x) {
            m1[x] = 0.5 * std::cos((2.0 * x + 1.0) * pi / 16.0);
            m2[x] = 0.5 * std::cos((2.0 * x + 1.0) * 2.0 * pi / 16.0);
        }
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int p = y * 8 + x;
                const double a = m1[x] * m2[y];
                const double b = m2[x] * m1[y];
                result.diff[p] = a - b;
                result.diff_scaled[p] = (a - b) / kLumaScale;
            }
        }
        return result;
    }();
    return value;
}

// Fixed-point Rec.601 luminance. Weights sum to 65536, so the error is below
// 0.01 of one 8-bit level; the same value is used by embed and extract.
inline int luminance16(std::uint8_t b, std::uint8_t g, std::uint8_t r) noexcept
{
    return 19595 * static_cast<int>(r) + 38470 * static_cast<int>(g)
        + 7471 * static_cast<int>(b);
}

inline int round_shift(double value) noexcept
{
    return static_cast<int>(value >= 0.0 ? value + 0.5 : value - 0.5);
}

inline std::uint8_t clamp_byte(int value) noexcept
{
    if (value <= 0) {
        return 0;
    }
    if (value >= 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

inline double wrap_mod(double value, double modulus) noexcept
{
    double result = std::fmod(value, modulus);
    if (result < 0.0) {
        result += modulus;
    }
    return result;
}

bool block_capacity(std::uint32_t width, std::uint32_t height, int phase_x, int phase_y,
    std::uint32_t& blocks_x, std::uint32_t& blocks_y) noexcept
{
    if (static_cast<std::uint32_t>(phase_x) >= width || static_cast<std::uint32_t>(phase_y) >= height) {
        blocks_x = 0;
        blocks_y = 0;
        return false;
    }
    blocks_x = (width - static_cast<std::uint32_t>(phase_x)) / static_cast<std::uint32_t>(kBlock);
    blocks_y = (height - static_cast<std::uint32_t>(phase_y)) / static_cast<std::uint32_t>(kBlock);
    return static_cast<std::size_t>(blocks_x) * blocks_y >= kMinBlocks;
}

std::uint32_t resolve_threads(std::uint32_t requested, std::uint32_t rows) noexcept
{
    if (requested == 0) {
        requested = std::thread::hardware_concurrency();
    }
    if (requested == 0) {
        requested = 1;
    }
    if (requested > 8) {
        requested = 8;
    }
    if (requested > rows) {
        requested = rows;
    }
    return requested == 0 ? 1 : requested;
}

void split_rows(std::uint32_t rows, std::uint32_t threads, std::vector<std::uint32_t>& begin,
    std::vector<std::uint32_t>& end)
{
    begin.resize(threads);
    end.resize(threads);
    const std::uint32_t base = rows / threads;
    const std::uint32_t remainder = rows % threads;
    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i < threads; ++i) {
        const std::uint32_t count = base + (i < remainder ? 1u : 0u);
        begin[i] = cursor;
        end[i] = cursor + count;
        cursor += count;
    }
}

// Returns d = C[1,2] - C[2,1]. When y16 is non-null it also stores the
// fixed-point luminance values for the in-place update pass.
double measure_block(const std::uint8_t* base, std::size_t stride, const Basis& kernel,
    int* y16) noexcept
{
    double d = 0.0;
    for (int y = 0; y < kBlock; ++y) {
        const std::uint8_t* pixel = base + static_cast<std::size_t>(y) * stride;
        const double* coefficients = kernel.diff_scaled + y * kBlock;
        for (int x = 0; x < kBlock; ++x) {
            const int value = luminance16(pixel[0], pixel[1], pixel[2]);
            if (y16 != nullptr) {
                y16[y * kBlock + x] = value;
            }
            d += static_cast<double>(value) * coefficients[x];
            pixel += 4;
        }
    }
    return d;
}

void update_block(std::uint8_t* base, std::size_t stride, const int* y16, double half_change,
    const Basis& kernel) noexcept
{
    for (int y = 0; y < kBlock; ++y) {
        std::uint8_t* pixel = base + static_cast<std::size_t>(y) * stride;
        const double* coefficients = kernel.diff + y * kBlock;
        for (int x = 0; x < kBlock; ++x) {
            const double old_y = static_cast<double>(y16[y * kBlock + x]) / kLumaScale;
            double new_y = old_y + half_change * coefficients[x];
            if (new_y < 0.0) {
                new_y = 0.0;
            } else if (new_y > 255.0) {
                new_y = 255.0;
            }
            const int shift = round_shift(new_y - old_y);
            if (shift != 0) {
                pixel[2] = clamp_byte(static_cast<int>(pixel[2]) + shift);
                pixel[1] = clamp_byte(static_cast<int>(pixel[1]) + shift);
                pixel[0] = clamp_byte(static_cast<int>(pixel[0]) + shift);
            }
            pixel += 4;
        }
    }
}

void embed_rows(const MutableImageView& image, const std::uint8_t* bits, double delta,
    const Basis& kernel, std::uint32_t blocks_x, std::uint32_t by_begin, std::uint32_t by_end,
    const CancelToken& cancel, bool& cancelled, std::uint32_t* cell_counts) noexcept
{
    for (std::uint32_t by = by_begin; by < by_end; ++by) {
        if ((by & 1u) == 0 && cancel.requested()) {
            cancelled = true;
            return;
        }
        std::uint8_t* row = image.pixels
            + static_cast<std::size_t>(by) * static_cast<std::size_t>(kBlock) * image.stride;
        const std::uint32_t cell_base = (by & 31u) * static_cast<std::uint32_t>(kTile);
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            std::uint8_t* base = row + static_cast<std::size_t>(bx) * kBlock * 4;
            const std::uint32_t cell = cell_base + (bx & 31u);
            int y16[kBlock * kBlock];
            const double d = measure_block(base, image.stride, kernel, y16);
            const double quantized = std::floor(d / delta + 0.5);
            const double target = quantized * delta + (bits[cell] ? delta * 0.5 : 0.0);
            const double change = target - d;
            if (std::fabs(change) > 1e-12) {
                update_block(base, image.stride, y16, change * 0.5, kernel);
            }
            ++cell_counts[cell];
        }
    }
}

void score_rows(const ImageView& image, int phase_x, int phase_y, double delta,
    const Basis& kernel, std::uint32_t blocks_x, std::uint32_t by_begin, std::uint32_t by_end,
    const CancelToken& cancel, bool& cancelled, SoftTile& tile) noexcept
{
    for (std::uint32_t by = by_begin; by < by_end; ++by) {
        if ((by & 3u) == 0 && cancel.requested()) {
            cancelled = true;
            return;
        }
        const std::uint8_t* row = image.pixels
            + static_cast<std::size_t>(phase_y + static_cast<int>(by) * kBlock) * image.stride
            + static_cast<std::size_t>(phase_x) * 4;
        const std::uint32_t cell_base = (by & 31u) * static_cast<std::uint32_t>(kTile);
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            const std::uint8_t* base = row + static_cast<std::size_t>(bx) * kBlock * 4;
            const double d = measure_block(base, image.stride, kernel, nullptr);
            const double remainder = wrap_mod(d, delta);
            const double distance_zero = (remainder < delta - remainder)
                ? remainder
                : (delta - remainder);
            const double distance_one = std::fabs(remainder - delta * 0.5);
            const std::uint32_t cell = cell_base + (bx & 31u);
            tile.score[cell] += distance_one - distance_zero;
            ++tile.count[cell];
        }
    }
}

bool compute_soft_tile(const ImageView& image, int phase_x, int phase_y, double delta,
    std::uint32_t worker_threads, const CancelToken& cancel, SoftTile& tile,
    std::uint32_t& blocks_x, std::uint32_t& blocks_y, Status& status) noexcept
{
    if (!block_capacity(image.width, image.height, phase_x, phase_y, blocks_x, blocks_y)) {
        status = Status::capacity_exceeded;
        return false;
    }
    soft_tile_reset(tile);
    const Basis& kernel = basis();
    const std::uint32_t threads = resolve_threads(worker_threads, blocks_y);
    if (threads <= 1) {
        bool cancelled = false;
        score_rows(image, phase_x, phase_y, delta, kernel, blocks_x, 0, blocks_y, cancel,
            cancelled, tile);
        if (cancelled) {
            status = Status::cancelled;
            return false;
        }
        status = Status::success;
        return true;
    }

    std::vector<std::uint32_t> begin;
    std::vector<std::uint32_t> end;
    split_rows(blocks_y, threads, begin, end);
    std::vector<SoftTile> local(threads);
    for (SoftTile& item : local) {
        soft_tile_reset(item);
    }
    std::vector<char> cancelled(threads, 0);
    std::vector<std::thread> workers;
    bool spawn_failed = false;
    auto worker = [&](std::uint32_t index) {
        bool local_cancelled = false;
        score_rows(image, phase_x, phase_y, delta, kernel, blocks_x, begin[index], end[index],
            cancel, local_cancelled, local[index]);
        if (local_cancelled) {
            cancelled[index] = 1;
        }
    };
    try {
        for (std::uint32_t i = 1; i < threads; ++i) {
            workers.emplace_back(worker, i);
        }
    } catch (...) {
        spawn_failed = true;
    }
    if (spawn_failed) {
        for (std::thread& item : workers) {
            if (item.joinable()) {
                item.join();
            }
        }
        soft_tile_reset(tile);
        bool fallback_cancelled = false;
        score_rows(image, phase_x, phase_y, delta, kernel, blocks_x, 0, blocks_y, cancel,
            fallback_cancelled, tile);
        if (fallback_cancelled) {
            status = Status::cancelled;
            return false;
        }
        status = Status::success;
        return true;
    }
    worker(0);
    for (std::thread& item : workers) {
        item.join();
    }
    bool any_cancelled = false;
    for (char flag : cancelled) {
        any_cancelled = any_cancelled || flag != 0;
    }
    if (any_cancelled) {
        status = Status::cancelled;
        return false;
    }
    for (const SoftTile& item : local) {
        for (std::size_t cell = 0; cell < kTileCells; ++cell) {
            tile.score[cell] += item.score[cell];
            tile.count[cell] += item.count[cell];
        }
    }
    status = Status::success;
    return true;
}

inline double cubic_kernel(double x) noexcept
{
    x = std::fabs(x);
    if (x <= 1.0) {
        return ((1.5 * x - 2.5) * x) * x + 1.0;
    }
    if (x < 2.0) {
        return (((-0.5 * x + 2.5) * x) - 4.0) * x + 2.0;
    }
    return 0.0;
}

inline std::size_t reflect_index(std::ptrdiff_t index, std::size_t size) noexcept
{
    if (size == 1) {
        return 0;
    }
    const std::ptrdiff_t period = static_cast<std::ptrdiff_t>(size * 2 - 2);
    std::ptrdiff_t value = index % period;
    if (value < 0) {
        value += period;
    }
    if (value >= static_cast<std::ptrdiff_t>(size)) {
        value = period - value;
    }
    return static_cast<std::size_t>(value);
}

void compute_weights(std::size_t destination_size, std::size_t source_size,
    std::vector<std::vector<double>>& weights, std::vector<std::vector<std::size_t>>& indices)
{
    const double scale = static_cast<double>(source_size) / static_cast<double>(destination_size);
    const double filter_scale = (scale > 1.0) ? scale : 1.0;
    const double support = 2.0 * filter_scale;
    weights.resize(destination_size);
    indices.resize(destination_size);
    for (std::size_t i = 0; i < destination_size; ++i) {
        const double center = (static_cast<double>(i) + 0.5) * scale - 0.5;
        const auto left = static_cast<std::ptrdiff_t>(std::floor(center - support + 0.5));
        const auto right = static_cast<std::ptrdiff_t>(std::floor(center + support + 0.5));
        double sum = 0.0;
        for (std::ptrdiff_t j = left; j < right; ++j) {
            const double weight = cubic_kernel((static_cast<double>(j) - center) / filter_scale);
            weights[i].push_back(weight);
            indices[i].push_back(reflect_index(j, source_size));
            sum += weight;
        }
        if (sum != 0.0) {
            for (double& weight : weights[i]) {
                weight /= sum;
            }
        }
    }
}

void sample_bicubic(const ImageView& source, double x, double y, std::uint8_t out[4]) noexcept
{
    const auto x0 = static_cast<std::ptrdiff_t>(std::floor(x));
    const auto y0 = static_cast<std::ptrdiff_t>(std::floor(y));
    const double fx = x - static_cast<double>(x0);
    const double fy = y - static_cast<double>(y0);
    double channels[4] = {0.0, 0.0, 0.0, 0.0};
    for (int j = 0; j < 4; ++j) {
        const double wy = cubic_kernel(fy - static_cast<double>(j - 1));
        if (wy == 0.0) {
            continue;
        }
        const std::size_t sy = reflect_index(y0 + j - 1, source.height);
        const std::uint8_t* row = source.pixels + sy * source.stride;
        for (int i = 0; i < 4; ++i) {
            const double wx = cubic_kernel(fx - static_cast<double>(i - 1));
            if (wx == 0.0) {
                continue;
            }
            const std::size_t sx = reflect_index(x0 + i - 1, source.width);
            const std::uint8_t* pixel = row + sx * 4;
            const double weight = wx * wy;
            channels[0] += weight * static_cast<double>(pixel[0]);
            channels[1] += weight * static_cast<double>(pixel[1]);
            channels[2] += weight * static_cast<double>(pixel[2]);
            channels[3] += weight * static_cast<double>(pixel[3]);
        }
    }
    out[0] = clamp_byte(round_shift(channels[0]));
    out[1] = clamp_byte(round_shift(channels[1]));
    out[2] = clamp_byte(round_shift(channels[2]));
    out[3] = clamp_byte(round_shift(channels[3]));
}

void unsharp_bgra(const ImageView& source, double amount, std::uint8_t* destination,
    std::size_t destination_stride) noexcept
{
    for (std::uint32_t y = 0; y < source.height; ++y) {
        std::uint8_t* out_row = destination + static_cast<std::size_t>(y) * destination_stride;
        const std::uint8_t* row = source.pixels + static_cast<std::size_t>(y) * source.stride;
        const std::uint32_t above = y > 0 ? y - 1 : 0;
        const std::uint32_t below = y + 1 < source.height ? y + 1 : source.height - 1;
        const std::uint8_t* row_above = source.pixels + static_cast<std::size_t>(above) * source.stride;
        const std::uint8_t* row_below = source.pixels + static_cast<std::size_t>(below) * source.stride;
        for (std::uint32_t x = 0; x < source.width; ++x) {
            const std::uint32_t left = x > 0 ? x - 1 : 0;
            const std::uint32_t right = x + 1 < source.width ? x + 1 : source.width - 1;
            for (int c = 0; c < 3; ++c) {
                const int blurred = row_above[left * 4 + c] + 2 * row_above[x * 4 + c]
                    + row_above[right * 4 + c] + 2 * row[left * 4 + c] + 4 * row[x * 4 + c]
                    + 2 * row[right * 4 + c] + row_below[left * 4 + c]
                    + 2 * row_below[x * 4 + c] + row_below[right * 4 + c];
                const double original = static_cast<double>(row[x * 4 + c]);
                const double value = original + amount * (original - blurred / 16.0);
                out_row[x * 4 + c] = clamp_byte(round_shift(value));
            }
            out_row[x * 4 + 3] = row[x * 4 + 3];
        }
    }
}

void exact_rotation(const ImageView& source, int quarter, std::uint8_t* destination,
    std::size_t destination_stride) noexcept
{
    const std::uint32_t width = source.width;
    const std::uint32_t height = source.height;
    if (quarter == 0) {
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memmove(destination + static_cast<std::size_t>(y) * destination_stride,
                source.pixels + static_cast<std::size_t>(y) * source.stride,
                static_cast<std::size_t>(width) * 4);
        }
        return;
    }
    if (quarter == 1) {
        for (std::uint32_t y = 0; y < width; ++y) {
            std::uint8_t* row = destination + static_cast<std::size_t>(y) * destination_stride;
            for (std::uint32_t x = 0; x < height; ++x) {
                const std::uint8_t* pixel = source.pixels
                    + static_cast<std::size_t>(height - 1 - x) * source.stride
                    + static_cast<std::size_t>(y) * 4;
                std::memcpy(row + static_cast<std::size_t>(x) * 4, pixel, 4);
            }
        }
        return;
    }
    if (quarter == 2) {
        for (std::uint32_t y = 0; y < height; ++y) {
            std::uint8_t* row = destination + static_cast<std::size_t>(y) * destination_stride;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t* pixel = source.pixels
                    + static_cast<std::size_t>(height - 1 - y) * source.stride
                    + static_cast<std::size_t>(width - 1 - x) * 4;
                std::memcpy(row + static_cast<std::size_t>(x) * 4, pixel, 4);
            }
        }
        return;
    }
    for (std::uint32_t y = 0; y < width; ++y) {
        std::uint8_t* row = destination + static_cast<std::size_t>(y) * destination_stride;
        for (std::uint32_t x = 0; x < height; ++x) {
            const std::uint8_t* pixel = source.pixels
                + static_cast<std::size_t>(x) * source.stride
                + static_cast<std::size_t>(width - 1 - y) * 4;
            std::memcpy(row + static_cast<std::size_t>(x) * 4, pixel, 4);
        }
    }
}

bool quarter_turns(double degrees, int& quarter) noexcept
{
    if (!std::isfinite(degrees)) {
        return false;
    }
    const double turns = degrees / 90.0;
    const double nearest = std::floor(turns + 0.5);
    if (std::fabs(turns - nearest) > 1e-9) {
        return false;
    }
    int value = static_cast<int>(std::fmod(nearest, 4.0));
    if (value < 0) {
        value += 4;
    }
    quarter = value;
    return true;
}

ExtractResult extract_once(const ImageView& image, const std::uint8_t* key,
    std::size_t key_length, const ExtractOptions& options, double rotation_degrees) noexcept
{
    ExtractResult result;
    result.status = Status::invalid_argument;
    result.rotation_degrees = rotation_degrees;
    if (!std::isfinite(rotation_degrees) || std::fabs(rotation_degrees) > 360.0) {
        return result;
    }

    ImageView source = image;
    std::unique_ptr<std::uint8_t[]> sharpened;
    std::unique_ptr<std::uint8_t[]> rotated;
    std::unique_ptr<std::uint8_t[]> scaled;

    if (options.sharpen > 0.0) {
        const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
        if (stride > std::numeric_limits<std::size_t>::max() / image.height) {
            result.status = Status::capacity_exceeded;
            return result;
        }
        const std::size_t total = stride * image.height;
        sharpened.reset(new (std::nothrow) std::uint8_t[total]);
        if (!sharpened) {
            result.status = Status::capacity_exceeded;
            return result;
        }
        unsharp_bgra(image, options.sharpen, sharpened.get(), stride);
        source.width = image.width;
        source.height = image.height;
        source.stride = stride;
        source.pixels = sharpened.get();
    }

    if (rotation_degrees != 0.0) {
        std::uint32_t rotated_width = 0;
        std::uint32_t rotated_height = 0;
        rotated_dimensions(source.width, source.height, rotation_degrees,
            rotated_width, rotated_height);
        if (rotated_width == 0 || rotated_height == 0 || rotated_width > kMaxPixels
            || rotated_height > kMaxPixels
            || static_cast<std::size_t>(rotated_width) * rotated_height > kMaxPixels) {
            result.status = Status::capacity_exceeded;
            return result;
        }
        const std::size_t stride = static_cast<std::size_t>(rotated_width) * 4;
        if (stride > std::numeric_limits<std::size_t>::max() / rotated_height) {
            result.status = Status::capacity_exceeded;
            return result;
        }
        const std::size_t total = stride * rotated_height;
        rotated.reset(new (std::nothrow) std::uint8_t[total]);
        if (!rotated) {
            result.status = Status::capacity_exceeded;
            return result;
        }
        if (!rotate_bicubic_bgra(source, rotation_degrees, rotated.get(), stride)) {
            result.status = Status::invalid_argument;
            return result;
        }
        source.width = rotated_width;
        source.height = rotated_height;
        source.stride = stride;
        source.pixels = rotated.get();
    }

    if (options.scale != 1.0) {
        if (options.original_width == 0 || options.original_height == 0) {
            return result;
        }
        if (options.original_width > kMaxPixels || options.original_height > kMaxPixels
            || static_cast<std::size_t>(options.original_width) * options.original_height
                > kMaxPixels) {
            return result;
        }
        const std::size_t stride = static_cast<std::size_t>(options.original_width) * 4;
        if (stride > std::numeric_limits<std::size_t>::max() / options.original_height) {
            return result;
        }
        const std::size_t total = stride * options.original_height;
        scaled.reset(new (std::nothrow) std::uint8_t[total]);
        if (!scaled) {
            result.status = Status::capacity_exceeded;
            return result;
        }
        if (!resize_bicubic_bgra(source, options.original_width, options.original_height,
                scaled.get(), stride)) {
            result.status = Status::invalid_argument;
            return result;
        }
        source.width = options.original_width;
        source.height = options.original_height;
        source.stride = stride;
        source.pixels = scaled.get();
    }

    const int first_x = options.search_pixel_phase ? 0 : options.phase_x;
    const int first_y = options.search_pixel_phase ? 0 : options.phase_y;
    const int last_x = options.search_pixel_phase ? 7 : options.phase_x;
    const int last_y = options.search_pixel_phase ? 7 : options.phase_y;

    bool have_tile = false;
    int best_sync = -1;
    int best_tile_x = 0;
    int best_tile_y = 0;
    int best_phase_x = 0;
    int best_phase_y = 0;
    SoftTile best_tile;
    soft_tile_reset(best_tile);
    bool searched = false;

    for (int phase_y = first_y; phase_y <= last_y; ++phase_y) {
        for (int phase_x = first_x; phase_x <= last_x; ++phase_x) {
            if (options.cancel.requested()) {
                result.status = Status::cancelled;
                return result;
            }
            SoftTile tile;
            std::uint32_t blocks_x = 0;
            std::uint32_t blocks_y = 0;
            Status status = Status::not_detected;
            if (!compute_soft_tile(source, phase_x, phase_y, options.delta,
                    options.worker_threads, options.cancel, tile, blocks_x, blocks_y, status)) {
                if (status == Status::cancelled) {
                    result.status = status;
                    return result;
                }
                continue;
            }
            searched = true;
            int tile_x = 0;
            int tile_y = 0;
            int score = 0;
            if (options.search_tile) {
                score = best_tile_shift(tile, tile_y, tile_x);
            } else {
                tile_x = options.tile_x;
                tile_y = options.tile_y;
                score = sync_score(tile, tile_y, tile_x);
            }
            if (!have_tile || score > best_sync) {
                have_tile = true;
                best_sync = score;
                best_tile = tile;
                best_tile_x = tile_x;
                best_tile_y = tile_y;
                best_phase_x = phase_x;
                best_phase_y = phase_y;
            }
        }
    }

    if (!searched || !have_tile) {
        result.status = Status::capacity_exceeded;
        return result;
    }
    result.sync_score = best_sync;
    result.tile_x = best_tile_x;
    result.tile_y = best_tile_y;
    result.phase_x = best_phase_x;
    result.phase_y = best_phase_y;
    if (best_sync < kSyncThreshold) {
        result.status = Status::not_detected;
        return result;
    }

    std::uint8_t codeword[kCodewordSize];
    codeword_from_soft(best_tile, best_tile_y, best_tile_x, codeword);
    DecodeResult decoded;
    Status status = read_codeword(key, key_length, codeword, kAlgorithmDctPairQim, decoded);
    std::memset(codeword, 0, sizeof(codeword));
    if (status == Status::not_detected) {
        status = Status::corrupted;
    }
    result.status = status;
    if (status == Status::success) {
        result.version = decoded.version;
        result.algorithm = decoded.algorithm;
        result.payload_length = decoded.payload_length;
        result.corrected_bytes = decoded.corrected_bytes;
        std::memcpy(result.payload, decoded.payload, decoded.payload_length);
    }
    std::memset(decoded.payload, 0, sizeof(decoded.payload));
    return result;
}

} // namespace

bool valid_delta(double delta) noexcept
{
    return std::isfinite(delta) && delta >= 1.0 && delta <= 255.0;
}

bool valid_image(const ImageView& image) noexcept
{
    if (image.pixels == nullptr || image.width == 0 || image.height == 0) {
        return false;
    }
    if (image.width > kMaxPixels || image.height > kMaxPixels) {
        return false;
    }
    if (static_cast<std::size_t>(image.width) * image.height > kMaxPixels) {
        return false;
    }
    if (image.stride < static_cast<std::size_t>(image.width) * 4) {
        return false;
    }
    if (image.stride > std::numeric_limits<std::size_t>::max() / image.height) {
        return false;
    }
    return true;
}

bool valid_image(const MutableImageView& image) noexcept
{
    ImageView view;
    view.width = image.width;
    view.height = image.height;
    view.stride = image.stride;
    view.pixels = image.pixels;
    return valid_image(view);
}

Status embed(const MutableImageView& image, const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t* payload, std::size_t payload_length,
    const EmbedOptions& options, EmbedResult& result) noexcept
{
    result = EmbedResult{};
    if (!valid_image(image)) {
        result.status = Status::invalid_argument;
        return result.status;
    }
    if (!valid_delta(options.delta)) {
        result.status = Status::invalid_argument;
        return result.status;
    }
    std::uint32_t blocks_x = 0;
    std::uint32_t blocks_y = 0;
    if (!block_capacity(image.width, image.height, 0, 0, blocks_x, blocks_y)) {
        result.status = Status::capacity_exceeded;
        return result.status;
    }
    if (options.cancel.requested()) {
        result.status = Status::cancelled;
        return result.status;
    }

    std::uint8_t codeword[kCodewordSize];
    const Status packet_status = make_codeword(key, key_length, kAlgorithmDctPairQim,
        payload, payload_length, codeword);
    if (packet_status != Status::success) {
        std::memset(codeword, 0, sizeof(codeword));
        result.status = packet_status;
        return result.status;
    }
    std::uint8_t bits[kTileCells];
    codeword_to_bits(codeword, bits);
    std::memset(codeword, 0, sizeof(codeword));

    const Basis& kernel = basis();
    const std::uint32_t threads = resolve_threads(options.worker_threads, blocks_y);
    std::vector<std::vector<std::uint32_t>> local_counts(
        threads, std::vector<std::uint32_t>(kTileCells, 0));
    std::vector<char> cancelled(threads, 0);
    bool ok = true;

    if (threads <= 1) {
        bool local_cancelled = false;
        embed_rows(image, bits, options.delta, kernel, blocks_x, 0, blocks_y,
            options.cancel, local_cancelled, local_counts[0].data());
        cancelled[0] = local_cancelled ? 1 : 0;
    } else {
        std::vector<std::uint32_t> begin;
        std::vector<std::uint32_t> end;
        split_rows(blocks_y, threads, begin, end);
        std::atomic<bool> start{false};
        std::atomic<bool> abort{false};
        std::vector<std::thread> workers;
        bool spawn_failed = false;
        auto worker = [&](std::uint32_t index) {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (abort.load(std::memory_order_acquire)) {
                return;
            }
            bool local_cancelled = false;
            embed_rows(image, bits, options.delta, kernel, blocks_x, begin[index], end[index],
                options.cancel, local_cancelled, local_counts[index].data());
            if (local_cancelled) {
                cancelled[index] = 1;
            }
        };
        try {
            for (std::uint32_t i = 1; i < threads; ++i) {
                workers.emplace_back(worker, i);
            }
        } catch (...) {
            spawn_failed = true;
            ok = false;
        }
        if (spawn_failed) {
            abort.store(true, std::memory_order_release);
            start.store(true, std::memory_order_release);
            for (std::thread& item : workers) {
                if (item.joinable()) {
                    item.join();
                }
            }
            // No worker touched pixels while abort was set; retry on the
            // caller thread so a thread-allocation failure is not fatal.
            bool local_cancelled = false;
            for (std::vector<std::uint32_t>& counts : local_counts) {
                std::fill(counts.begin(), counts.end(), 0);
            }
            embed_rows(image, bits, options.delta, kernel, blocks_x, 0, blocks_y,
                options.cancel, local_cancelled, local_counts[0].data());
            cancelled[0] = local_cancelled ? 1 : 0;
            for (std::uint32_t i = 1; i < threads; ++i) {
                cancelled[i] = 0;
            }
            ok = true;
        } else {
            start.store(true, std::memory_order_release);
            worker(0);
            for (std::thread& item : workers) {
                item.join();
            }
        }
    }

    bool any_cancelled = false;
    for (char flag : cancelled) {
        any_cancelled = any_cancelled || flag != 0;
    }
    if (!ok || any_cancelled) {
        std::memset(bits, 0, sizeof(bits));
        result.status = Status::cancelled;
        return result.status;
    }

    std::uint32_t minimum = 0xFFFFFFFFu;
    std::uint32_t maximum = 0;
    for (std::size_t cell = 0; cell < kTileCells; ++cell) {
        std::uint32_t total = 0;
        for (const std::vector<std::uint32_t>& counts : local_counts) {
            total += counts[cell];
        }
        if (total < minimum) {
            minimum = total;
        }
        if (total > maximum) {
            maximum = total;
        }
    }
    std::memset(bits, 0, sizeof(bits));
    result.status = Status::success;
    result.blocks_x = blocks_x;
    result.blocks_y = blocks_y;
    result.min_carriers = minimum;
    result.max_carriers = maximum;
    return result.status;
}

ExtractResult extract(const ImageView& image, const std::uint8_t* key, std::size_t key_length,
    const ExtractOptions& options) noexcept
{
    ExtractResult invalid;
    invalid.status = Status::invalid_argument;
    if (!valid_image(image) || !valid_delta(options.delta) || !valid_key(key, key_length)) {
        return invalid;
    }
    if (options.phase_x < 0 || options.phase_x > 7 || options.phase_y < 0 || options.phase_y > 7) {
        return invalid;
    }
    if (options.tile_x < 0 || options.tile_x > 31 || options.tile_y < 0 || options.tile_y > 31) {
        return invalid;
    }
    if (!std::isfinite(options.scale) || options.scale <= 0.0) {
        return invalid;
    }
    if (!std::isfinite(options.sharpen) || options.sharpen < 0.0 || options.sharpen > 2.0) {
        return invalid;
    }
    if (!std::isfinite(options.rotation_degrees)
        || std::fabs(options.rotation_degrees) > 360.0) {
        return invalid;
    }

    if (!options.search_rotation) {
        return extract_once(image, key, key_length, options, options.rotation_degrees);
    }

    ExtractResult best = extract_once(image, key, key_length, options, options.rotation_degrees);
    if (best.status == Status::success || best.status == Status::cancelled) {
        return best;
    }
    int best_sync = best.sync_score;
    static const double kRotationCandidates[] = {
        -0.25, 0.25, -0.5, 0.5, -0.75, 0.75, -1.0, 1.0, -1.5, 1.5,
        -2.0, 2.0, -3.0, 3.0, 90.0, 180.0, 270.0, -90.0, -180.0, -270.0};
    for (double angle : kRotationCandidates) {
        if (angle == options.rotation_degrees) {
            continue;
        }
        ExtractResult candidate = extract_once(image, key, key_length, options, angle);
        if (candidate.status == Status::success) {
            return candidate;
        }
        if (candidate.status == Status::cancelled) {
            return candidate;
        }
        if (candidate.sync_score > best_sync) {
            best = candidate;
            best_sync = candidate.sync_score;
        }
    }
    return best;
}

bool resize_bicubic_bgra(const ImageView& source, std::uint32_t width, std::uint32_t height,
    std::uint8_t* destination, std::size_t destination_stride) noexcept
{
    if (!valid_image(source) || destination == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (width > kMaxPixels || height > kMaxPixels
        || static_cast<std::size_t>(width) * height > kMaxPixels) {
        return false;
    }
    if (destination_stride < static_cast<std::size_t>(width) * 4) {
        return false;
    }
    if (destination_stride > std::numeric_limits<std::size_t>::max() / height) {
        return false;
    }
    const std::size_t temp_values = static_cast<std::size_t>(width) * source.height * 4;
    if (temp_values > kResizeTempLimit / sizeof(float)) {
        return false;
    }
    std::unique_ptr<float[]> temp(new (std::nothrow) float[temp_values]);
    if (!temp) {
        return false;
    }

    std::vector<std::vector<double>> weights_x;
    std::vector<std::vector<std::size_t>> indices_x;
    std::vector<std::vector<double>> weights_y;
    std::vector<std::vector<std::size_t>> indices_y;
    compute_weights(width, source.width, weights_x, indices_x);
    compute_weights(height, source.height, weights_y, indices_y);

    // Horizontal pass: destination_width x source_height.
    for (std::uint32_t y = 0; y < source.height; ++y) {
        const std::uint8_t* row = source.pixels + static_cast<std::size_t>(y) * source.stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            double channels[4] = {0.0, 0.0, 0.0, 0.0};
            const auto& weight = weights_x[x];
            const auto& index = indices_x[x];
            for (std::size_t i = 0; i < weight.size(); ++i) {
                const std::uint8_t* pixel = row + index[i] * 4;
                channels[0] += weight[i] * static_cast<double>(pixel[0]);
                channels[1] += weight[i] * static_cast<double>(pixel[1]);
                channels[2] += weight[i] * static_cast<double>(pixel[2]);
                channels[3] += weight[i] * static_cast<double>(pixel[3]);
            }
            float* target = temp.get() + (static_cast<std::size_t>(y) * width + x) * 4;
            target[0] = static_cast<float>(channels[0]);
            target[1] = static_cast<float>(channels[1]);
            target[2] = static_cast<float>(channels[2]);
            target[3] = static_cast<float>(channels[3]);
        }
    }

    // Vertical pass.
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = destination + static_cast<std::size_t>(y) * destination_stride;
        const auto& weight = weights_y[y];
        const auto& index = indices_y[y];
        for (std::uint32_t x = 0; x < width; ++x) {
            double channels[4] = {0.0, 0.0, 0.0, 0.0};
            for (std::size_t i = 0; i < weight.size(); ++i) {
                const float* source_pixel =
                    temp.get() + (index[i] * static_cast<std::size_t>(width) + x) * 4;
                channels[0] += weight[i] * source_pixel[0];
                channels[1] += weight[i] * source_pixel[1];
                channels[2] += weight[i] * source_pixel[2];
                channels[3] += weight[i] * source_pixel[3];
            }
            std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
            pixel[0] = clamp_byte(round_shift(channels[0]));
            pixel[1] = clamp_byte(round_shift(channels[1]));
            pixel[2] = clamp_byte(round_shift(channels[2]));
            pixel[3] = clamp_byte(round_shift(channels[3]));
        }
    }
    return true;
}

void rotated_dimensions(std::uint32_t width, std::uint32_t height, double degrees,
    std::uint32_t& out_width, std::uint32_t& out_height) noexcept
{
    out_width = width;
    out_height = height;
    int quarter = 0;
    if (quarter_turns(degrees, quarter) && (quarter == 1 || quarter == 3)) {
        out_width = height;
        out_height = width;
    }
}

bool rotate_bicubic_bgra(const ImageView& source, double degrees, std::uint8_t* destination,
    std::size_t destination_stride) noexcept
{
    if (!valid_image(source) || destination == nullptr || !std::isfinite(degrees)
        || std::fabs(degrees) > 360.0) {
        return false;
    }
    if (destination == source.pixels) {
        return false;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    rotated_dimensions(source.width, source.height, degrees, width, height);
    if (width == 0 || height == 0 || width > kMaxPixels || height > kMaxPixels
        || static_cast<std::size_t>(width) * height > kMaxPixels) {
        return false;
    }
    if (destination_stride < static_cast<std::size_t>(width) * 4) {
        return false;
    }
    if (destination_stride > std::numeric_limits<std::size_t>::max() / height) {
        return false;
    }

    int quarter = 0;
    if (quarter_turns(degrees, quarter)) {
        exact_rotation(source, quarter, destination, destination_stride);
        return true;
    }

    constexpr double pi = 3.14159265358979323846;
    const double angle = -degrees * pi / 180.0;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const double center_x = (static_cast<double>(source.width) - 1.0) * 0.5;
    const double center_y = (static_cast<double>(source.height) - 1.0) * 0.5;
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = destination + static_cast<std::size_t>(y) * destination_stride;
        const double dy = static_cast<double>(y) - center_y;
        for (std::uint32_t x = 0; x < width; ++x) {
            const double dx = static_cast<double>(x) - center_x;
            const double sx = center_x + cosine * dx - sine * dy;
            const double sy = center_y + sine * dx + cosine * dy;
            std::uint8_t pixel[4];
            sample_bicubic(source, sx, sy, pixel);
            std::memcpy(row + static_cast<std::size_t>(x) * 4, pixel, 4);
        }
    }
    return true;
}

} // namespace av::watermark::dct

