// SPDX-License-Identifier: Apache-2.0
// DWT family implementation. Independent C++17 code written from the public
// Haar/Mallat/QIM definitions and the frozen watermark protocol contract. No
// external image or transform library is used by the product algorithm core.
//
// Optimisation notes:
//  - Level-0 luminance conversion and the first Haar level are fused, so no
//    full-frame float luma plane is written and read again.
//  - The final inverse level writes RGB directly from the coefficient bands
//    (luma-delta path); no full reconstructed luma plane is materialised.
//  - BGRA hot loops use SSE2 intrinsics. The scalar path remains the reference
//    for other formats and for CPUs without SSE2.
//  - Row-parallel execution is bounded to 4 workers and only used for large
//    frames. Results are bit-identical to the serial path because every row is
//    independent and all arithmetic keeps the scalar operation order.
// The frozen packet/RS/sync bytes and the QIM maths are unchanged.
#include "av/watermark/dwt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include "av/watermark/ecc.h"

#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define AV_HAVE_SSE2 1
#include <emmintrin.h>
#else
#define AV_HAVE_SSE2 0
#endif

namespace av::watermark {

namespace {

constexpr float kInvSqrt2 = 0.70710678118654752440f;
constexpr int kMaxLevels = 8;
constexpr int kMaxWorkers = 4;
constexpr long long kParallelWorkThreshold = 65536;

int bytes_per_pixel(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::bgra8: return 4;
    case PixelFormat::bgr8: return 3;
    case PixelFormat::rgb8: return 3;
    case PixelFormat::gray8: return 1;
    }
    return 0;
}

bool valid_input(const ImageView& view) noexcept
{
    const int bpp = bytes_per_pixel(view.format);
    return view.pixels != nullptr && view.width > 0 && view.height > 0
        && bpp > 0 && view.stride >= view.width * bpp
        && view.stride > 0;
}

bool valid_output(const MutableImageView& view) noexcept
{
    const int bpp = bytes_per_pixel(view.format);
    return view.pixels != nullptr && view.width > 0 && view.height > 0
        && bpp > 0 && view.stride >= view.width * bpp
        && view.stride > 0;
}

inline float luma_at(const std::uint8_t* pixel, PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::bgra8:
        return 0.299f * pixel[2] + 0.587f * pixel[1] + 0.114f * pixel[0];
    case PixelFormat::bgr8:
        return 0.299f * pixel[2] + 0.587f * pixel[1] + 0.114f * pixel[0];
    case PixelFormat::rgb8:
        return 0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2];
    case PixelFormat::gray8:
        return static_cast<float>(pixel[0]);
    }
    return 0.0f;
}

inline std::uint8_t clamp_byte(float value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return static_cast<std::uint8_t>(std::floor(value + 0.5f));
}

int hardware_threads() noexcept
{
    static const int value = [] {
        const unsigned count = std::thread::hardware_concurrency();
        return count == 0 ? 1 : static_cast<int>(count);
    }();
    return value;
}

int effective_threads(int max_threads) noexcept
{
    const int limit = max_threads > 0 ? max_threads : hardware_threads();
    return std::max(1, std::min(limit, kMaxWorkers));
}

// Splits row range [0, rows) into at most effective_threads(max_threads)
// contiguous chunks. Small jobs stay serial to avoid thread-start overhead.
template <typename Fn>
void parallel_rows(int rows, int columns, int max_threads, Fn&& fn)
{
    if (rows <= 0) {
        return;
    }
    const int threads = effective_threads(max_threads);
    const long long work = static_cast<long long>(rows) * static_cast<long long>(columns);
    if (threads <= 1 || rows < 16 || work < kParallelWorkThreshold) {
        fn(0, rows);
        return;
    }
    const int chunk = (rows + threads - 1) / threads;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    try {
        for (int index = 0; index < threads; ++index) {
            const int begin = index * chunk;
            const int end = std::min(rows, begin + chunk);
            if (begin >= end) {
                break;
            }
            workers.emplace_back([&fn, begin, end] { fn(begin, end); });
        }
    } catch (...) {
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        fn(0, rows);
        return;
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

#if AV_HAVE_SSE2
inline __m128 luma4_bgra(const std::uint8_t* pixels) noexcept
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels));
    const __m128i low = _mm_unpacklo_epi8(packed, zero);
    const __m128i high = _mm_unpackhi_epi8(packed, zero);
    __m128 blue = _mm_cvtepi32_ps(_mm_unpacklo_epi16(low, zero));
    __m128 green = _mm_cvtepi32_ps(_mm_unpackhi_epi16(low, zero));
    __m128 red = _mm_cvtepi32_ps(_mm_unpacklo_epi16(high, zero));
    __m128 alpha = _mm_cvtepi32_ps(_mm_unpackhi_epi16(high, zero));
    _MM_TRANSPOSE4_PS(blue, green, red, alpha);
    return _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(red, _mm_set1_ps(0.299f)),
            _mm_mul_ps(green, _mm_set1_ps(0.587f))),
        _mm_mul_ps(blue, _mm_set1_ps(0.114f)));
}

inline __m128 luma4_gray(const std::uint8_t* pixels) noexcept
{
    int packed = 0;
    std::memcpy(&packed, pixels, sizeof(packed));
    const __m128i bytes = _mm_cvtsi32_si128(packed);
    const __m128i zero = _mm_setzero_si128();
    const __m128i words = _mm_unpacklo_epi8(bytes, zero);
    return _mm_cvtepi32_ps(_mm_unpacklo_epi16(words, zero));
}

inline void forward4(const float* row0, const float* row1, float* ll, float* lh,
    float* hl, float* hh, int x) noexcept
{
    const __m128 r0a = _mm_loadu_ps(row0 + 2 * x);
    const __m128 r0b = _mm_loadu_ps(row0 + 2 * x + 4);
    const __m128 r1a = _mm_loadu_ps(row1 + 2 * x);
    const __m128 r1b = _mm_loadu_ps(row1 + 2 * x + 4);
    const __m128 a = _mm_shuffle_ps(r0a, r0b, _MM_SHUFFLE(2, 0, 2, 0));
    const __m128 b = _mm_shuffle_ps(r0a, r0b, _MM_SHUFFLE(3, 1, 3, 1));
    const __m128 c = _mm_shuffle_ps(r1a, r1b, _MM_SHUFFLE(2, 0, 2, 0));
    const __m128 d = _mm_shuffle_ps(r1a, r1b, _MM_SHUFFLE(3, 1, 3, 1));
    const __m128 scale = _mm_set1_ps(kInvSqrt2);
    const __m128 low0 = _mm_mul_ps(_mm_add_ps(a, b), scale);
    const __m128 high0 = _mm_mul_ps(_mm_sub_ps(a, b), scale);
    const __m128 low1 = _mm_mul_ps(_mm_add_ps(c, d), scale);
    const __m128 high1 = _mm_mul_ps(_mm_sub_ps(c, d), scale);
    _mm_storeu_ps(ll + x, _mm_mul_ps(_mm_add_ps(low0, low1), scale));
    _mm_storeu_ps(lh + x, _mm_mul_ps(_mm_sub_ps(low0, low1), scale));
    _mm_storeu_ps(hl + x, _mm_mul_ps(_mm_add_ps(high0, high1), scale));
    _mm_storeu_ps(hh + x, _mm_mul_ps(_mm_sub_ps(high0, high1), scale));
}

inline void forward4_luma(const __m128 luma0a, const __m128 luma0b,
    const __m128 luma1a, const __m128 luma1b, float* ll, float* lh, float* hl,
    float* hh, int x) noexcept
{
    const __m128 a = _mm_shuffle_ps(luma0a, luma0b, _MM_SHUFFLE(2, 0, 2, 0));
    const __m128 b = _mm_shuffle_ps(luma0a, luma0b, _MM_SHUFFLE(3, 1, 3, 1));
    const __m128 c = _mm_shuffle_ps(luma1a, luma1b, _MM_SHUFFLE(2, 0, 2, 0));
    const __m128 d = _mm_shuffle_ps(luma1a, luma1b, _MM_SHUFFLE(3, 1, 3, 1));
    const __m128 scale = _mm_set1_ps(kInvSqrt2);
    const __m128 low0 = _mm_mul_ps(_mm_add_ps(a, b), scale);
    const __m128 high0 = _mm_mul_ps(_mm_sub_ps(a, b), scale);
    const __m128 low1 = _mm_mul_ps(_mm_add_ps(c, d), scale);
    const __m128 high1 = _mm_mul_ps(_mm_sub_ps(c, d), scale);
    _mm_storeu_ps(ll + x, _mm_mul_ps(_mm_add_ps(low0, low1), scale));
    _mm_storeu_ps(lh + x, _mm_mul_ps(_mm_sub_ps(low0, low1), scale));
    _mm_storeu_ps(hl + x, _mm_mul_ps(_mm_add_ps(high0, high1), scale));
    _mm_storeu_ps(hh + x, _mm_mul_ps(_mm_sub_ps(high0, high1), scale));
}

inline void inverse4(const float* ll, const float* lh, const float* hl,
    const float* hh, float* row0, float* row1, int x) noexcept
{
    const __m128 scale = _mm_set1_ps(kInvSqrt2);
    const __m128 llv = _mm_loadu_ps(ll + x);
    const __m128 lhv = _mm_loadu_ps(lh + x);
    const __m128 hlv = _mm_loadu_ps(hl + x);
    const __m128 hhv = _mm_loadu_ps(hh + x);
    const __m128 low0 = _mm_mul_ps(_mm_add_ps(llv, lhv), scale);
    const __m128 low1 = _mm_mul_ps(_mm_sub_ps(llv, lhv), scale);
    const __m128 high0 = _mm_mul_ps(_mm_add_ps(hlv, hhv), scale);
    const __m128 high1 = _mm_mul_ps(_mm_sub_ps(hlv, hhv), scale);
    const __m128 top_even = _mm_mul_ps(_mm_add_ps(low0, high0), scale);
    const __m128 top_odd = _mm_mul_ps(_mm_sub_ps(low0, high0), scale);
    const __m128 bottom_even = _mm_mul_ps(_mm_add_ps(low1, high1), scale);
    const __m128 bottom_odd = _mm_mul_ps(_mm_sub_ps(low1, high1), scale);
    _mm_storeu_ps(row0 + 2 * x, _mm_unpacklo_ps(top_even, top_odd));
    _mm_storeu_ps(row0 + 2 * x + 4, _mm_unpackhi_ps(top_even, top_odd));
    _mm_storeu_ps(row1 + 2 * x, _mm_unpacklo_ps(bottom_even, bottom_odd));
    _mm_storeu_ps(row1 + 2 * x + 4, _mm_unpackhi_ps(bottom_even, bottom_odd));
}

inline __m128 clamp_bytes_sse(__m128 value) noexcept
{
    value = _mm_max_ps(value, _mm_setzero_ps());
    value = _mm_min_ps(value, _mm_set1_ps(255.0f));
    return _mm_cvtepi32_ps(_mm_cvttps_epi32(_mm_add_ps(value, _mm_set1_ps(0.5f))));
}

inline void store_bgra4(const __m128 blue, const __m128 green, const __m128 red,
    const __m128 alpha, std::uint8_t* target) noexcept
{
    const __m128i b = _mm_cvttps_epi32(blue);
    const __m128i g = _mm_cvttps_epi32(green);
    const __m128i r = _mm_cvttps_epi32(red);
    const __m128i a = _mm_cvttps_epi32(alpha);
    const __m128i bg0 = _mm_unpacklo_epi32(b, g);
    const __m128i ra0 = _mm_unpacklo_epi32(r, a);
    const __m128i bg1 = _mm_unpackhi_epi32(b, g);
    const __m128i ra1 = _mm_unpackhi_epi32(r, a);
    const __m128i p0 = _mm_unpacklo_epi64(bg0, ra0);
    const __m128i p1 = _mm_unpackhi_epi64(bg0, ra0);
    const __m128i p2 = _mm_unpacklo_epi64(bg1, ra1);
    const __m128i p3 = _mm_unpackhi_epi64(bg1, ra1);
    const __m128i words0 = _mm_packs_epi32(p0, p1);
    const __m128i words1 = _mm_packs_epi32(p2, p3);
    const __m128i bytes = _mm_packus_epi16(words0, words1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(target), bytes);
}

struct Bgra4 {
    __m128 blue;
    __m128 green;
    __m128 red;
    __m128 alpha;
};

inline Bgra4 load_bgra4(const std::uint8_t* pixels) noexcept
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels));
    const __m128i low = _mm_unpacklo_epi8(packed, zero);
    const __m128i high = _mm_unpackhi_epi8(packed, zero);
    Bgra4 value;
    value.blue = _mm_cvtepi32_ps(_mm_unpacklo_epi16(low, zero));
    value.green = _mm_cvtepi32_ps(_mm_unpackhi_epi16(low, zero));
    value.red = _mm_cvtepi32_ps(_mm_unpacklo_epi16(high, zero));
    value.alpha = _mm_cvtepi32_ps(_mm_unpackhi_epi16(high, zero));
    _MM_TRANSPOSE4_PS(value.blue, value.green, value.red, value.alpha);
    return value;
}

inline void write_delta_bgra4(const float* luma, const std::uint8_t* source,
    std::uint8_t* target) noexcept
{
    const Bgra4 channels = load_bgra4(source);
    const __m128 new_luma = _mm_loadu_ps(luma);
    const __m128 old_luma = _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(channels.red, _mm_set1_ps(0.299f)),
            _mm_mul_ps(channels.green, _mm_set1_ps(0.587f))),
        _mm_mul_ps(channels.blue, _mm_set1_ps(0.114f)));
    const __m128 delta = _mm_sub_ps(new_luma, old_luma);
    store_bgra4(
        clamp_bytes_sse(_mm_add_ps(channels.blue, delta)),
        clamp_bytes_sse(_mm_add_ps(channels.green, delta)),
        clamp_bytes_sse(_mm_add_ps(channels.red, delta)),
        clamp_bytes_sse(channels.alpha), target);
}

inline void write_gray4(const float* luma, std::uint8_t* target) noexcept
{
    const __m128 value = clamp_bytes_sse(_mm_loadu_ps(luma));
    const __m128i ints = _mm_cvttps_epi32(value);
    const __m128i words = _mm_packs_epi32(ints, ints);
    const __m128i bytes = _mm_packus_epi16(words, words);
    int packed = _mm_cvtsi128_si32(bytes);
    std::memcpy(target, &packed, sizeof(packed));
}
#endif // AV_HAVE_SSE2

void allocate_level(DwtLevel& level, int half_width, int half_height)
{
    level.ll.width = half_width;
    level.ll.height = half_height;
    level.lh.width = half_width;
    level.lh.height = half_height;
    level.hl.width = half_width;
    level.hl.height = half_height;
    level.hh.width = half_width;
    level.hh.height = half_height;
    const std::size_t count = static_cast<std::size_t>(half_width)
        * static_cast<std::size_t>(half_height);
    level.ll.samples.resize(count);
    level.lh.samples.resize(count);
    level.hl.samples.resize(count);
    level.hh.samples.resize(count);
}

void forward_level(const float* source, int width, int height, DwtLevel& level,
    int max_threads)
{
    const int half_width = width / 2;
    const int half_height = height / 2;
    allocate_level(level, half_width, half_height);
    auto process = [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            const float* row0 = source
                + static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(width);
            const float* row1 = row0 + width;
            float* ll = level.ll.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            float* lh = level.lh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            float* hl = level.hl.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            float* hh = level.hh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            int x = 0;
#if AV_HAVE_SSE2
            for (; x + 4 <= half_width; x += 4) {
                forward4(row0, row1, ll, lh, hl, hh, x);
            }
#endif
            for (; x < half_width; ++x) {
                const float a = row0[2 * x];
                const float b = row0[2 * x + 1];
                const float c = row1[2 * x];
                const float d = row1[2 * x + 1];
                const float low0 = (a + b) * kInvSqrt2;
                const float high0 = (a - b) * kInvSqrt2;
                const float low1 = (c + d) * kInvSqrt2;
                const float high1 = (c - d) * kInvSqrt2;
                ll[x] = (low0 + low1) * kInvSqrt2;
                lh[x] = (low0 - low1) * kInvSqrt2;
                hl[x] = (high0 + high1) * kInvSqrt2;
                hh[x] = (high0 - high1) * kInvSqrt2;
            }
        }
    };
    parallel_rows(half_height, half_width, max_threads, process);
}

// Fuses BGRA/scalar luminance conversion with the first Haar level. The
// padded border replicates the last available pixel, exactly like the
// standalone make_luma path.
bool build_level0(const ImageView& image, int phase_x, int phase_y,
    DwtLevel& level, int& padded_width, int& padded_height, int max_threads)
{
    const int bpp = bytes_per_pixel(image.format);
    if (bpp == 0 || phase_x < 0 || phase_y < 0
        || phase_x >= image.width || phase_y >= image.height) {
        return false;
    }
    const int cropped_width = image.width - phase_x;
    const int cropped_height = image.height - phase_y;
    const int width = (cropped_width + 7) & ~7;
    const int height = (cropped_height + 7) & ~7;
    if (width <= 0 || height <= 0
        || static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(height)) {
        return false;
    }
    padded_width = width;
    padded_height = height;
    const int half_width = width / 2;
    const int half_height = height / 2;
    allocate_level(level, half_width, half_height);

    auto luma_pixel = [&](int source_y, int x) -> float {
        const int px = std::min(x, cropped_width - 1);
        const std::uint8_t* pixel = image.pixels
            + static_cast<std::size_t>(source_y) * static_cast<std::size_t>(image.stride)
            + static_cast<std::size_t>(phase_x + px) * static_cast<std::size_t>(bpp);
        return luma_at(pixel, image.format);
    };

    auto process = [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            const int row0_index = std::min(phase_y + 2 * y, image.height - 1);
            const int row1_index = std::min(phase_y + 2 * y + 1, image.height - 1);
            const std::uint8_t* row0 = image.pixels
                + static_cast<std::size_t>(row0_index) * static_cast<std::size_t>(image.stride);
            const std::uint8_t* row1 = image.pixels
                + static_cast<std::size_t>(row1_index) * static_cast<std::size_t>(image.stride);
            float* ll = level.ll.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            float* lh = level.lh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            float* hl = level.hl.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            float* hh = level.hh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            int x = 0;
#if AV_HAVE_SSE2
            const bool full_rows = phase_y + 2 * y + 1 < image.height;
            if (full_rows && image.format == PixelFormat::bgra8) {
                for (; x + 4 <= half_width && 2 * x + 8 <= cropped_width; x += 4) {
                    const std::uint8_t* p0 = row0
                        + static_cast<std::size_t>(phase_x + 2 * x) * 4u;
                    const std::uint8_t* p1 = row1
                        + static_cast<std::size_t>(phase_x + 2 * x) * 4u;
                    const __m128 l0a = luma4_bgra(p0);
                    const __m128 l0b = luma4_bgra(p0 + 16);
                    const __m128 l1a = luma4_bgra(p1);
                    const __m128 l1b = luma4_bgra(p1 + 16);
                    forward4_luma(l0a, l0b, l1a, l1b, ll, lh, hl, hh, x);
                }
            } else if (full_rows && image.format == PixelFormat::gray8) {
                for (; x + 4 <= half_width && 2 * x + 8 <= cropped_width; x += 4) {
                    const std::uint8_t* p0 = row0 + phase_x + 2 * x;
                    const std::uint8_t* p1 = row1 + phase_x + 2 * x;
                    const __m128 l0a = luma4_gray(p0);
                    const __m128 l0b = luma4_gray(p0 + 4);
                    const __m128 l1a = luma4_gray(p1);
                    const __m128 l1b = luma4_gray(p1 + 4);
                    forward4_luma(l0a, l0b, l1a, l1b, ll, lh, hl, hh, x);
                }
            }
#endif
            for (; x < half_width; ++x) {
                const float a = luma_pixel(row0_index, 2 * x);
                const float b = luma_pixel(row0_index, 2 * x + 1);
                const float c = luma_pixel(row1_index, 2 * x);
                const float d = luma_pixel(row1_index, 2 * x + 1);
                const float low0 = (a + b) * kInvSqrt2;
                const float high0 = (a - b) * kInvSqrt2;
                const float low1 = (c + d) * kInvSqrt2;
                const float high1 = (c - d) * kInvSqrt2;
                ll[x] = (low0 + low1) * kInvSqrt2;
                lh[x] = (low0 - low1) * kInvSqrt2;
                hl[x] = (high0 + high1) * kInvSqrt2;
                hh[x] = (high0 - high1) * kInvSqrt2;
            }
        }
    };
    parallel_rows(half_height, half_width, max_threads, process);
    return true;
}

void inverse_level(const DwtLevel& level, const float* ll, float* output,
    int width, int height, int max_threads)
{
    const int half_width = width / 2;
    const int half_height = height / 2;
    auto process = [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            const float* row_ll = ll
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            const float* row_lh = level.lh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            const float* row_hl = level.hl.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            const float* row_hh = level.hh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            float* row0 = output
                + static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(width);
            float* row1 = row0 + width;
            int x = 0;
#if AV_HAVE_SSE2
            for (; x + 4 <= half_width; x += 4) {
                inverse4(row_ll, row_lh, row_hl, row_hh, row0, row1, x);
            }
#endif
            for (; x < half_width; ++x) {
                const float l0 = (row_ll[x] + row_lh[x]) * kInvSqrt2;
                const float l1 = (row_ll[x] - row_lh[x]) * kInvSqrt2;
                const float h0 = (row_hl[x] + row_hh[x]) * kInvSqrt2;
                const float h1 = (row_hl[x] - row_hh[x]) * kInvSqrt2;
                row0[2 * x] = (l0 + h0) * kInvSqrt2;
                row0[2 * x + 1] = (l0 - h0) * kInvSqrt2;
                row1[2 * x] = (l1 + h1) * kInvSqrt2;
                row1[2 * x + 1] = (l1 - h1) * kInvSqrt2;
            }
        }
    };
    parallel_rows(half_height, half_width, max_threads, process);
}

// Final inverse level fused with the luma-delta RGB write. This is
// mathematically the same as reconstructing luma and re-applying the old
// Cb/Cr, but it avoids two full-frame float passes.
void inverse_level0_write(const DwtLevel& level, const float* ll,
    const ImageView& input, const MutableImageView& output, int width, int height,
    int max_threads)
{
    const int half_width = width / 2;
    const int half_height = height / 2;
    const int bpp = bytes_per_pixel(input.format);
    auto process = [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            const int target_row0 = 2 * y;
            const int target_row1 = target_row0 + 1;
            if (target_row0 >= output.height) {
                continue;
            }
            const float* row_ll = ll
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            const float* row_lh = level.lh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            const float* row_hl = level.hl.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            const float* row_hh = level.hh.samples.data()
                + static_cast<std::size_t>(y) * static_cast<std::size_t>(half_width);
            const std::uint8_t* source0 = input.pixels
                + static_cast<std::size_t>(target_row0) * static_cast<std::size_t>(input.stride);
            std::uint8_t* target0 = output.pixels
                + static_cast<std::size_t>(target_row0) * static_cast<std::size_t>(output.stride);
            const std::uint8_t* source1 = input.pixels
                + static_cast<std::size_t>(std::min(target_row1, input.height - 1))
                    * static_cast<std::size_t>(input.stride);
            std::uint8_t* target1 = nullptr;
            if (target_row1 < output.height) {
                target1 = output.pixels
                    + static_cast<std::size_t>(target_row1)
                        * static_cast<std::size_t>(output.stride);
            }

            int x = 0;
#if AV_HAVE_SSE2
            if (input.format == PixelFormat::bgra8) {
                const int max_start = std::min(half_width - 4, (output.width - 8) / 2);
                for (; x <= max_start; x += 4) {
                    const __m128 scale = _mm_set1_ps(kInvSqrt2);
                    const __m128 llv = _mm_loadu_ps(row_ll + x);
                    const __m128 lhv = _mm_loadu_ps(row_lh + x);
                    const __m128 hlv = _mm_loadu_ps(row_hl + x);
                    const __m128 hhv = _mm_loadu_ps(row_hh + x);
                    const __m128 low0 = _mm_mul_ps(_mm_add_ps(llv, lhv), scale);
                    const __m128 low1 = _mm_mul_ps(_mm_sub_ps(llv, lhv), scale);
                    const __m128 high0 = _mm_mul_ps(_mm_add_ps(hlv, hhv), scale);
                    const __m128 high1 = _mm_mul_ps(_mm_sub_ps(hlv, hhv), scale);
                    const __m128 top_even = _mm_mul_ps(_mm_add_ps(low0, high0), scale);
                    const __m128 top_odd = _mm_mul_ps(_mm_sub_ps(low0, high0), scale);
                    const __m128 bottom_even = _mm_mul_ps(_mm_add_ps(low1, high1), scale);
                    const __m128 bottom_odd = _mm_mul_ps(_mm_sub_ps(low1, high1), scale);
                    alignas(16) float top[8];
                    alignas(16) float bottom[8];
                    _mm_store_ps(top, _mm_unpacklo_ps(top_even, top_odd));
                    _mm_store_ps(top + 4, _mm_unpackhi_ps(top_even, top_odd));
                    _mm_store_ps(bottom, _mm_unpacklo_ps(bottom_even, bottom_odd));
                    _mm_store_ps(bottom + 4, _mm_unpackhi_ps(bottom_even, bottom_odd));
                    write_delta_bgra4(top, source0 + static_cast<std::size_t>(2 * x) * 4u,
                        target0 + static_cast<std::size_t>(2 * x) * 4u);
                    write_delta_bgra4(top + 4,
                        source0 + static_cast<std::size_t>(2 * x + 4) * 4u,
                        target0 + static_cast<std::size_t>(2 * x + 4) * 4u);
                    if (target1 != nullptr) {
                        write_delta_bgra4(bottom,
                            source1 + static_cast<std::size_t>(2 * x) * 4u,
                            target1 + static_cast<std::size_t>(2 * x) * 4u);
                        write_delta_bgra4(bottom + 4,
                            source1 + static_cast<std::size_t>(2 * x + 4) * 4u,
                            target1 + static_cast<std::size_t>(2 * x + 4) * 4u);
                    }
                }
            }
#endif
            for (; x < half_width; ++x) {
                const float l0 = (row_ll[x] + row_lh[x]) * kInvSqrt2;
                const float l1 = (row_ll[x] - row_lh[x]) * kInvSqrt2;
                const float h0 = (row_hl[x] + row_hh[x]) * kInvSqrt2;
                const float h1 = (row_hl[x] - row_hh[x]) * kInvSqrt2;
                const float top0 = (l0 + h0) * kInvSqrt2;
                const float top1 = (l0 - h0) * kInvSqrt2;
                const float bottom0 = (l1 + h1) * kInvSqrt2;
                const float bottom1 = (l1 - h1) * kInvSqrt2;
                const int px = 2 * x;
                if (px < output.width) {
                    auto write_pixel = [&](const std::uint8_t* source,
                                           std::uint8_t* target, float new_luma) {
                        if (input.format == PixelFormat::gray8) {
                            target[0] = clamp_byte(new_luma);
                            return;
                        }
                        if (input.format == PixelFormat::rgb8) {
                            const float delta = new_luma
                                - luma_at(source, input.format);
                            target[0] = clamp_byte(static_cast<float>(source[0]) + delta);
                            target[1] = clamp_byte(static_cast<float>(source[1]) + delta);
                            target[2] = clamp_byte(static_cast<float>(source[2]) + delta);
                            return;
                        }
                        if (input.format == PixelFormat::bgr8) {
                            const float delta = new_luma
                                - luma_at(source, input.format);
                            target[0] = clamp_byte(static_cast<float>(source[0]) + delta);
                            target[1] = clamp_byte(static_cast<float>(source[1]) + delta);
                            target[2] = clamp_byte(static_cast<float>(source[2]) + delta);
                            return;
                        }
                        const float delta = new_luma - luma_at(source, input.format);
                        target[0] = clamp_byte(static_cast<float>(source[0]) + delta);
                        target[1] = clamp_byte(static_cast<float>(source[1]) + delta);
                        target[2] = clamp_byte(static_cast<float>(source[2]) + delta);
                        target[3] = source[3];
                    };
                    write_pixel(source0 + static_cast<std::size_t>(px) * bpp,
                        target0 + static_cast<std::size_t>(px) * bpp, top0);
                    if (px + 1 < output.width) {
                        write_pixel(source0 + static_cast<std::size_t>(px + 1) * bpp,
                            target0 + static_cast<std::size_t>(px + 1) * bpp, top1);
                    }
                    if (target1 != nullptr) {
                        write_pixel(source1 + static_cast<std::size_t>(px) * bpp,
                            target1 + static_cast<std::size_t>(px) * bpp, bottom0);
                        if (px + 1 < output.width) {
                            write_pixel(source1 + static_cast<std::size_t>(px + 1) * bpp,
                                target1 + static_cast<std::size_t>(px + 1) * bpp, bottom1);
                        }
                    }
                }
            }
        }
    };
    parallel_rows(half_height, half_width, max_threads, process);
}

inline double qim_residual(float value, double delta) noexcept
{
    const double v = static_cast<double>(value);
    return v - std::floor(v / delta) * delta;
}

void accumulate_soft(const DwtPlane& band, double delta, SoftTile& tile)
{
    soft_tile_reset(tile);
    const std::size_t count = static_cast<std::size_t>(band.width)
        * static_cast<std::size_t>(band.height);
    for (std::size_t index = 0; index < count; ++index) {
        const int x = static_cast<int>(index % static_cast<std::size_t>(band.width));
        const int y = static_cast<int>(index / static_cast<std::size_t>(band.width));
        const double r = qim_residual(band.samples[index], delta);
        const double dist0 = std::min(r, delta - r);
        const double dist1 = std::fabs(r - delta * 0.5);
        const std::uint32_t cell = static_cast<std::uint32_t>(y & 31) * 32u
            + static_cast<std::uint32_t>(x & 31);
        tile.score[cell] += dist1 - dist0;
        tile.count[cell] += 1;
    }
}

bool check_phase(const WatermarkPosition& position) noexcept
{
    return position.phase_x >= 0 && position.phase_x < 8
        && position.phase_y >= 0 && position.phase_y < 8
        && position.tile_x >= 0 && position.tile_x < 32
        && position.tile_y >= 0 && position.tile_y < 32
        && position.scale > 0.0;
}

bool check_rotation(const WatermarkPosition& position) noexcept
{
    return position.rotation == 0 || position.rotation == 90
        || position.rotation == 180 || position.rotation == 270;
}

bool validate_parameters(const DwtParameters& parameters) noexcept
{
    // The protocol contract freezes these values.  A different family needs a
    // new protocol version, not a silent parameter change.
    return parameters.levels == 3 && parameters.tile == 32
        && parameters.delta == 24.0 && parameters.max_threads >= 0;
}

double cubic_kernel(double x) noexcept
{
    constexpr double a = -0.5;
    x = std::fabs(x);
    if (x <= 1.0) {
        return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    }
    if (x < 2.0) {
        return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    }
    return 0.0;
}

struct AxisTaps {
    int first = 0;
    std::vector<double> weights;
};

void build_taps(int source_size, int destination_size, std::vector<AxisTaps>& taps)
{
    const double scale = static_cast<double>(source_size)
        / static_cast<double>(destination_size);
    const double filter_scale = std::max(scale, 1.0);
    const double support = 2.0 * filter_scale;
    taps.resize(static_cast<std::size_t>(destination_size));
    for (int i = 0; i < destination_size; ++i) {
        const double center = (static_cast<double>(i) + 0.5) * scale;
        int first = static_cast<int>(std::floor(center - support + 0.5));
        int last = static_cast<int>(std::ceil(center + support - 0.5));
        first = std::max(first, 0);
        last = std::min(last, source_size - 1);
        AxisTaps& item = taps[static_cast<std::size_t>(i)];
        item.first = first;
        item.weights.resize(static_cast<std::size_t>(std::max(0, last - first + 1)));
        double sum = 0.0;
        for (int source = first; source <= last; ++source) {
            const double weight = cubic_kernel(
                (static_cast<double>(source) - center + 0.5) / filter_scale);
            item.weights[static_cast<std::size_t>(source - first)] = weight;
            sum += weight;
        }
        if (sum == 0.0) {
            item.first = std::min(std::max(0, static_cast<int>(std::floor(center))),
                source_size - 1);
            item.weights.assign(1, 1.0);
            continue;
        }
        for (double& weight : item.weights) {
            weight /= sum;
        }
    }
}

bool undo_rotation(const ImageView& input, int rotation,
    std::vector<std::uint8_t>& buffer, ImageView& output)
{
    output = input;
    if (rotation == 0) {
        return true;
    }
    const int bpp = bytes_per_pixel(input.format);
    if (bpp == 0 || (rotation != 90 && rotation != 180 && rotation != 270)) {
        return false;
    }
    const bool swap = rotation == 90 || rotation == 270;
    const int width = swap ? input.height : input.width;
    const int height = swap ? input.width : input.height;
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(bpp);
    if (static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / stride) {
        return false;
    }
    buffer.assign(stride * static_cast<std::size_t>(height), 0);
    for (int y = 0; y < height; ++y) {
        std::uint8_t* target = buffer.data() + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            int source_x = 0;
            int source_y = 0;
            if (rotation == 90) {
                source_y = x;
                source_x = height - 1 - y;
            } else if (rotation == 180) {
                source_x = input.width - 1 - x;
                source_y = input.height - 1 - y;
            } else {
                source_y = width - 1 - x;
                source_x = y;
            }
            std::memcpy(target + static_cast<std::size_t>(x) * static_cast<std::size_t>(bpp),
                input.pixels + static_cast<std::size_t>(source_y)
                    * static_cast<std::size_t>(input.stride)
                    + static_cast<std::size_t>(source_x) * static_cast<std::size_t>(bpp),
                static_cast<std::size_t>(bpp));
        }
    }
    output.pixels = buffer.data();
    output.width = width;
    output.height = height;
    output.stride = static_cast<int>(stride);
    output.format = input.format;
    return true;
}

bool forward_levels(const ImageView& image, int phase_x, int phase_y,
    const DwtParameters& parameters, int max_threads, int& width, int& height,
    std::vector<DwtLevel>& levels)
{
    levels.clear();
    DwtLevel level0;
    if (!build_level0(image, phase_x, phase_y, level0, width, height, max_threads)) {
        return false;
    }
    levels.reserve(static_cast<std::size_t>(parameters.levels));
    levels.push_back(std::move(level0));
    while (static_cast<int>(levels.size()) < parameters.levels) {
        const DwtLevel& previous = levels.back();
        if ((previous.ll.width & 1) != 0 || (previous.ll.height & 1) != 0) {
            levels.clear();
            return false;
        }
        DwtLevel level;
        forward_level(previous.ll.samples.data(), previous.ll.width, previous.ll.height,
            level, max_threads);
        levels.push_back(std::move(level));
    }
    return true;
}

Status extract_once(const ImageView& input, const std::uint8_t* key,
    std::size_t key_length, const DwtParameters& parameters,
    const WatermarkPosition& position, DwtExtractResult& result,
    const CancelToken& cancel)
{
    result = DwtExtractResult{};
    if (!valid_input(input) || !valid_key(key, key_length)
        || !validate_parameters(parameters) || !check_phase(position)) {
        result.status = Status::invalid_argument;
        return result.status;
    }
    if (position.scale != 1.0
        && (position.original_width <= 0 || position.original_height <= 0)) {
        result.status = Status::invalid_argument;
        return result.status;
    }

    std::vector<std::uint8_t> rotated;
    ImageView effective;
    if (!undo_rotation(input, position.rotation, rotated, effective)) {
        result.status = Status::invalid_argument;
        return result.status;
    }

    std::vector<std::uint8_t> resized;
    if (position.scale != 1.0) {
        const int bpp = bytes_per_pixel(effective.format);
        const std::size_t stride = static_cast<std::size_t>(position.original_width)
            * static_cast<std::size_t>(bpp);
        resized.resize(stride * static_cast<std::size_t>(position.original_height));
        MutableImageView destination;
        destination.pixels = resized.data();
        destination.width = position.original_width;
        destination.height = position.original_height;
        destination.stride = static_cast<int>(stride);
        destination.format = effective.format;
        if (!resize_bicubic(effective, destination)) {
            result.status = Status::invalid_argument;
            return result.status;
        }
        effective.pixels = resized.data();
        effective.width = destination.width;
        effective.height = destination.height;
        effective.stride = destination.stride;
        effective.format = destination.format;
    }
    if (cancel.requested()) {
        result.status = Status::cancelled;
        return result.status;
    }

    int best_sync = -1;
    int best_phase_x = position.phase_x;
    int best_phase_y = position.phase_y;
    int best_tile_x = position.tile_x;
    int best_tile_y = position.tile_y;
    int best_coefficients_width = 0;
    int best_coefficients_height = 0;
    SoftTile best_tile;
    soft_tile_reset(best_tile);
    bool found = false;

    const int first_phase_y = position.search_phase ? 0 : position.phase_y;
    const int last_phase_y = position.search_phase ? 7 : position.phase_y;
    const int first_phase_x = position.search_phase ? 0 : position.phase_x;
    const int last_phase_x = position.search_phase ? 7 : position.phase_x;
    const int transform_threads = position.search_phase ? 1 : parameters.max_threads;
    for (int phase_y = first_phase_y; phase_y <= last_phase_y; ++phase_y) {
        for (int phase_x = first_phase_x; phase_x <= last_phase_x; ++phase_x) {
            int width = 0;
            int height = 0;
            std::vector<DwtLevel> levels;
            if (!forward_levels(effective, phase_x, phase_y, parameters,
                    transform_threads, width, height, levels)) {
                continue;
            }
            std::size_t coefficients = 0;
            dwt_capacity(width, height, parameters, coefficients);
            if (coefficients < 2u * static_cast<std::size_t>(parameters.tile)
                    * static_cast<std::size_t>(parameters.tile)) {
                continue;
            }
            if (cancel.requested()) {
                result.status = Status::cancelled;
                return result.status;
            }
            SoftTile tile;
            accumulate_soft(levels.back().lh, parameters.delta, tile);

            int tile_y = position.tile_y;
            int tile_x = position.tile_x;
            int score = 0;
            if (position.tile_known) {
                score = sync_score(tile, tile_y, tile_x);
            } else {
                score = best_tile_shift(tile, tile_y, tile_x);
            }
            if (!found || score > best_sync) {
                found = true;
                best_sync = score;
                best_phase_x = phase_x;
                best_phase_y = phase_y;
                best_tile_x = tile_x;
                best_tile_y = tile_y;
                best_coefficients_width = levels.back().lh.width;
                best_coefficients_height = levels.back().lh.height;
                best_tile = tile;
            }
        }
    }

    if (!found) {
        result.status = Status::capacity_exceeded;
        return result.status;
    }
    result.sync_score = static_cast<std::uint32_t>(best_sync);
    result.phase_x = best_phase_x;
    result.phase_y = best_phase_y;
    result.tile_x = best_tile_x;
    result.tile_y = best_tile_y;
    result.coefficient_width = best_coefficients_width;
    result.coefficient_height = best_coefficients_height;
    if (best_sync < kSyncThreshold) {
        result.status = Status::not_detected;
        return result.status;
    }
    if (cancel.requested()) {
        result.status = Status::cancelled;
        return result.status;
    }

    std::uint8_t codeword[kCodewordSize];
    codeword_from_soft(best_tile, best_tile_y, best_tile_x, codeword);
    DecodeResult decoded;
    Status status = read_codeword(key, key_length, codeword,
        kAlgorithmDwtLh3Qim, decoded);
    std::memset(codeword, 0, sizeof(codeword));
    if (status == Status::not_detected) {
        // Synchronisation was already confirmed; a missing packet magic means
        // the payload was damaged rather than absent.
        status = Status::corrupted;
    }
    result.status = status;
    if (status == Status::success) {
        std::memcpy(result.payload, decoded.payload, kPayloadMax);
        result.payload_length = decoded.payload_length;
        result.version = decoded.version;
        result.algorithm = decoded.algorithm;
        result.corrected_bytes = decoded.corrected_bytes;
    }
    return result.status;
}

} // namespace

bool dwt_forward(const float* samples, int width, int height, int level_count,
    std::vector<DwtLevel>& levels)
{
    levels.clear();
    if (samples == nullptr || width <= 0 || height <= 0
        || level_count <= 0 || level_count > kMaxLevels) {
        return false;
    }
    int current_width = width;
    int current_height = height;
    const float* current = samples;
    levels.reserve(static_cast<std::size_t>(level_count));
    const int threads = effective_threads(0);
    for (int level_index = 0; level_index < level_count; ++level_index) {
        if (((current_width & 1) != 0) || ((current_height & 1) != 0)) {
            levels.clear();
            return false;
        }
        DwtLevel level;
        forward_level(current, current_width, current_height, level, threads);
        current_width /= 2;
        current_height /= 2;
        levels.push_back(std::move(level));
        current = levels.back().ll.samples.data();
    }
    return true;
}

bool dwt_inverse(const std::vector<DwtLevel>& levels, float* samples,
    int width, int height)
{
    if (samples == nullptr || width <= 0 || height <= 0 || levels.empty()) {
        return false;
    }
    const int threads = effective_threads(0);
    const DwtLevel& last = levels.back();
    std::vector<float> current = last.ll.samples;
    int current_width = last.ll.width * 2;
    int current_height = last.ll.height * 2;
    for (int index = static_cast<int>(levels.size()) - 1; index >= 0; --index) {
        const DwtLevel& level = levels[static_cast<std::size_t>(index)];
        if (level.ll.width * 2 != current_width || level.ll.height * 2 != current_height) {
            return false;
        }
        if (index == 0) {
            if (current_width != width || current_height != height) {
                return false;
            }
            inverse_level(level, current.data(), samples, width, height, threads);
        } else {
            std::vector<float> next(static_cast<std::size_t>(current_width)
                * static_cast<std::size_t>(current_height));
            inverse_level(level, current.data(), next.data(), current_width,
                current_height, threads);
            current = std::move(next);
            current_width *= 2;
            current_height *= 2;
        }
    }
    return current_width == width && current_height == height;
}

bool dwt_capacity(int width, int height, const DwtParameters& parameters,
    std::size_t& coefficients)
{
    coefficients = 0;
    if (width <= 0 || height <= 0 || !validate_parameters(parameters)) {
        return false;
    }
    const int padded_width = (width + 7) & ~7;
    const int padded_height = (height + 7) & ~7;
    coefficients = static_cast<std::size_t>(padded_width / 8)
        * static_cast<std::size_t>(padded_height / 8);
    return true;
}

std::size_t dwt_working_memory_bytes(int width, int height,
    const DwtParameters& parameters)
{
    if (width <= 0 || height <= 0 || !validate_parameters(parameters)) {
        return 0;
    }
    const std::size_t padded_width = static_cast<std::size_t>((width + 7) & ~7);
    const std::size_t padded_height = static_cast<std::size_t>((height + 7) & ~7);
    const std::size_t pixels = padded_width * padded_height;
    // Level planes (1.3125 px floats), reconstruction scratch (0.25 px floats)
    // and the optional scaled extraction buffer (4 px bytes). The old 3.5625
    // px float estimate belonged to the two-full-frame implementation.
    const double floats = static_cast<double>(pixels) * 1.5625;
    const std::size_t bytes = static_cast<std::size_t>(floats * sizeof(float))
        + pixels * 4u;
    return (bytes + 4095u) & ~static_cast<std::size_t>(4095u);
}

bool watermark_position_for_region(int region_x, int region_y,
    int region_width, int region_height, double scale,
    WatermarkPosition& position)
{
    if (region_x < 0 || region_y < 0 || region_width <= 0 || region_height <= 0
        || !(scale > 0.0) || !std::isfinite(scale)) {
        return false;
    }
    const long long phase_x = ((-static_cast<long long>(region_x)) % 8 + 8) % 8;
    const long long phase_y = ((-static_cast<long long>(region_y)) % 8 + 8) % 8;
    const long long block_x = (static_cast<long long>(region_x) + phase_x) / 8;
    const long long block_y = (static_cast<long long>(region_y) + phase_y) / 8;
    position = WatermarkPosition{};
    position.phase_x = static_cast<int>(phase_x);
    position.phase_y = static_cast<int>(phase_y);
    position.tile_x = static_cast<int>(((-block_x) % 32 + 32) % 32);
    position.tile_y = static_cast<int>(((-block_y) % 32 + 32) % 32);
    position.scale = scale;
    position.original_width = region_width;
    position.original_height = region_height;
    position.crop_x = region_x;
    position.crop_y = region_y;
    position.tile_known = true;
    position.search_phase = false;
    position.search_rotation = false;
    position.rotation = 0;
    return true;
}

Status dwt_embed(const ImageView& input, const MutableImageView& output,
    const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t* payload, std::size_t payload_length,
    const DwtParameters& parameters, const CancelToken& cancel)
{
    if (!valid_input(input) || !valid_output(output)) {
        return Status::invalid_argument;
    }
    if (input.width != output.width || input.height != output.height
        || input.format != output.format) {
        return Status::invalid_argument;
    }
    if (!valid_key(key, key_length) || !validate_parameters(parameters)) {
        return Status::invalid_argument;
    }
    if (payload_length > kPayloadMax) {
        return Status::payload_too_large;
    }
    if (payload == nullptr && payload_length != 0) {
        return Status::invalid_argument;
    }

    std::size_t coefficients = 0;
    if (!dwt_capacity(input.width, input.height, parameters, coefficients)) {
        return Status::invalid_argument;
    }
    if (coefficients < 2u * static_cast<std::size_t>(parameters.tile)
            * static_cast<std::size_t>(parameters.tile)) {
        return Status::capacity_exceeded;
    }
    if (cancel.requested()) {
        return Status::cancelled;
    }

    int width = 0;
    int height = 0;
    std::vector<DwtLevel> levels;
    if (!forward_levels(input, 0, 0, parameters, parameters.max_threads,
            width, height, levels)) {
        return Status::invalid_argument;
    }
    if (cancel.requested()) {
        return Status::cancelled;
    }

    std::uint8_t codeword[kCodewordSize];
    Status status = make_codeword(key, key_length, kAlgorithmDwtLh3Qim,
        payload, payload_length, codeword);
    if (status != Status::success) {
        std::memset(codeword, 0, sizeof(codeword));
        return status;
    }
    std::uint8_t bits[kTileCells];
    codeword_to_bits(codeword, bits);
    std::memset(codeword, 0, sizeof(codeword));

    DwtPlane& band = levels.back().lh;
    const double delta = parameters.delta;
    const double half = delta * 0.5;
    for (int y = 0; y < band.height; ++y) {
        float* row = band.samples.data()
            + static_cast<std::size_t>(y) * static_cast<std::size_t>(band.width);
        const std::uint32_t tile_row = static_cast<std::uint32_t>(y & 31) * 32u;
        for (int x = 0; x < band.width; ++x) {
            const double value = static_cast<double>(row[x]);
            const double quantized = std::floor(value / delta + 0.5) * delta;
            row[x] = static_cast<float>(quantized
                + (bits[tile_row + static_cast<std::uint32_t>(x & 31)] != 0 ? half : 0.0));
        }
    }
    std::memset(bits, 0, sizeof(bits));
    if (cancel.requested()) {
        return Status::cancelled;
    }

    // Reconstruct levels from coarse to fine, then fuse the final inverse
    // level with the RGB write. The largest scratch buffer is half-frame.
    std::vector<float> current = levels.back().ll.samples;
    int current_width = levels.back().ll.width * 2;
    int current_height = levels.back().ll.height * 2;
    for (int index = static_cast<int>(levels.size()) - 1; index >= 1; --index) {
        const DwtLevel& level = levels[static_cast<std::size_t>(index)];
        if (level.ll.width * 2 != current_width || level.ll.height * 2 != current_height) {
            return Status::invalid_argument;
        }
        std::vector<float> next(static_cast<std::size_t>(current_width)
            * static_cast<std::size_t>(current_height));
        inverse_level(level, current.data(), next.data(), current_width,
            current_height, parameters.max_threads);
        current = std::move(next);
        current_width *= 2;
        current_height *= 2;
    }
    if (current_width != width || current_height != height) {
        return Status::invalid_argument;
    }
    inverse_level0_write(levels[0], current.data(), input, output, width, height,
        parameters.max_threads);
    if (cancel.requested()) {
        return Status::cancelled;
    }
    return Status::success;
}

Status dwt_extract(const ImageView& input, const std::uint8_t* key,
    std::size_t key_length, const DwtParameters& parameters,
    const WatermarkPosition& position, DwtExtractResult& result,
    const CancelToken& cancel)
{
    result = DwtExtractResult{};
    if (!valid_input(input) || !valid_key(key, key_length)
        || !validate_parameters(parameters) || !check_phase(position)
        || !check_rotation(position)) {
        result.status = Status::invalid_argument;
        return result.status;
    }
    if (!position.search_rotation) {
        return extract_once(input, key, key_length, parameters, position, result, cancel);
    }

    DwtExtractResult best;
    int best_sync = -1;
    bool found = false;
    for (int rotation : {0, 90, 180, 270}) {
        WatermarkPosition attempt = position;
        attempt.rotation = rotation;
        attempt.search_rotation = false;
        DwtExtractResult local;
        const Status status = extract_once(input, key, key_length, parameters,
            attempt, local, cancel);
        if (status == Status::cancelled) {
            result = local;
            return status;
        }
        if (status == Status::success) {
            result = local;
            return status;
        }
        if (!found || static_cast<int>(local.sync_score) > best_sync) {
            found = true;
            best = local;
            best_sync = static_cast<int>(local.sync_score);
        }
    }
    result = best;
    return result.status;
}

bool resize_bicubic(const ImageView& source, const MutableImageView& destination)
{
    if (!valid_input(source) || !valid_output(destination)
        || source.format != destination.format) {
        return false;
    }
    const int channels = bytes_per_pixel(source.format);
    if (source.width == destination.width && source.height == destination.height) {
        const std::size_t row_bytes = static_cast<std::size_t>(source.width)
            * static_cast<std::size_t>(channels);
        for (int y = 0; y < source.height; ++y) {
            std::memcpy(destination.pixels
                    + static_cast<std::size_t>(y) * static_cast<std::size_t>(destination.stride),
                source.pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(source.stride),
                row_bytes);
        }
        return true;
    }

    std::vector<AxisTaps> horizontal;
    std::vector<AxisTaps> vertical;
    build_taps(source.width, destination.width, horizontal);
    build_taps(source.height, destination.height, vertical);

    const std::size_t intermediate_size = static_cast<std::size_t>(destination.width)
        * static_cast<std::size_t>(source.height) * static_cast<std::size_t>(channels);
    std::vector<float> intermediate(intermediate_size);
    for (int y = 0; y < source.height; ++y) {
        const std::uint8_t* row = source.pixels
            + static_cast<std::size_t>(y) * static_cast<std::size_t>(source.stride);
        for (int x = 0; x < destination.width; ++x) {
            const AxisTaps& taps = horizontal[static_cast<std::size_t>(x)];
            float* out = intermediate.data()
                + (static_cast<std::size_t>(y) * static_cast<std::size_t>(destination.width)
                    + static_cast<std::size_t>(x)) * static_cast<std::size_t>(channels);
            for (int channel = 0; channel < channels; ++channel) {
                double sum = 0.0;
                for (std::size_t k = 0; k < taps.weights.size(); ++k) {
                    const int source_x = taps.first + static_cast<int>(k);
                    sum += static_cast<double>(
                        row[static_cast<std::size_t>(source_x) * static_cast<std::size_t>(channels)
                            + static_cast<std::size_t>(channel)]) * taps.weights[k];
                }
                out[channel] = static_cast<float>(sum);
            }
        }
    }

    for (int y = 0; y < destination.height; ++y) {
        const AxisTaps& taps = vertical[static_cast<std::size_t>(y)];
        std::uint8_t* row = destination.pixels
            + static_cast<std::size_t>(y) * static_cast<std::size_t>(destination.stride);
        for (int x = 0; x < destination.width; ++x) {
            for (int channel = 0; channel < channels; ++channel) {
                double sum = 0.0;
                for (std::size_t k = 0; k < taps.weights.size(); ++k) {
                    const int source_y = taps.first + static_cast<int>(k);
                    sum += static_cast<double>(intermediate[
                        (static_cast<std::size_t>(source_y)
                            * static_cast<std::size_t>(destination.width)
                            + static_cast<std::size_t>(x))
                            * static_cast<std::size_t>(channels)
                        + static_cast<std::size_t>(channel)]) * taps.weights[k];
                }
                row[static_cast<std::size_t>(x) * static_cast<std::size_t>(channels)
                    + static_cast<std::size_t>(channel)] = clamp_byte(static_cast<float>(sum));
            }
        }
    }
    return true;
}

} // namespace av::watermark
