// SPDX-License-Identifier: Apache-2.0
#include "av/watermark/robust.h"

#include "av/watermark/ecc.h"
#include "av/watermark/packet.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace av::watermark::robust {
namespace {

constexpr int side = 32;
constexpr int cells = side * side;
constexpr double pi = 3.14159265358979323846;

struct Basis {
    double data[64]{};
    double pilot[64]{};
    Basis() noexcept {
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            const double a = 0.5 * std::cos((2 * x + 1) * pi / 16);
            const double b = 0.5 * std::cos((2 * y + 1) * pi / 16);
            data[y * 8 + x] = a * 0.5 * std::cos((2 * y + 1) * 2 * pi / 16)
                - b * 0.5 * std::cos((2 * x + 1) * 2 * pi / 16);
            pilot[y * 8 + x] = a * b;
        }
    }
};

const Basis& basis() noexcept { static const Basis value; return value; }

double luma(const std::uint8_t* p) noexcept {
    return (19595 * p[2] + 38470 * p[1] + 7471 * p[0]) / 65536.0;
}

std::uint8_t byte(double value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(std::floor(value + 0.5), 0.0, 255.0));
}

void measure(const std::uint8_t* p, std::size_t stride, double& data, double& pilot) noexcept {
    const auto& b = basis();
    data = pilot = 0;
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        const double v = luma(p + y * stride + x * 4);
        data += v * b.data[y * 8 + x];
        pilot += v * b.pilot[y * 8 + x];
    }
}

double quantize(double value, double step, bool bit) noexcept {
    const double offset = bit ? 0.5 : 0;
    return (std::floor(value / step - offset + 0.5) + offset) * step;
}

double evidence(double value, double step) noexcept {
    double r = std::fmod(value / step, 1.0);
    if (r < 0) r += 1;
    return 1 - 4 * std::min(r, 1 - r); // +1 = zero, -1 = one
}

// Eight adjacent bits form a 4x2 rectangle. Missing image regions therefore
// erase whole RS symbols instead of spraying one bad bit into every symbol.
int data_cell(int symbol, int bit) noexcept {
    const int group = (37 * symbol + 11) & 127;
    return ((group / 8) * 2 + bit / 4) * side + (group % 8) * 4 + bit % 4;
}

void pilot_bits(const std::uint8_t* key, std::size_t key_length,
    std::array<std::uint8_t, cells>& bits) noexcept {
    std::uint8_t domain[] = {'A','V','/','R','-','D','C','T','/','p','i','l','o','t','/','1',0};
    for (int i = 0; i < 4; ++i) {
        domain[16] = static_cast<std::uint8_t>(i);
        std::uint8_t digest[32];
        hmac_sha256(key, key_length, domain, sizeof(domain), digest);
        for (int j = 0; j < 256; ++j) bits[i * 256 + j] = (digest[j / 8] >> (7 - j % 8)) & 1;
        std::memset(digest, 0, sizeof(digest));
    }
}

struct Tile {
    std::array<double, cells> data{};
    std::array<double, cells> pilot{};
    std::array<unsigned, cells> count{};
};

void remove_host_bias(Tile& tile) noexcept {
    double data_mean=0, pilot_mean=0;
    unsigned observed=0;
    for (int i=0;i<cells;++i) if (tile.count[i]) {
        data_mean+=tile.data[i]/tile.count[i];
        pilot_mean+=tile.pilot[i]/tile.count[i];
        ++observed;
    }
    if (!observed) return;
    data_mean/=observed; pilot_mean/=observed;
    for (int i=0;i<cells;++i) if (tile.count[i]) {
        tile.data[i]-=data_mean*tile.count[i];
        tile.pilot[i]-=pilot_mean*tile.count[i];
    }
}

struct Candidate {
    double correlation = 0;
    int x = 0, y = 0;
};

// Keyed normalized pilot correlation. Only observed cells enter the statistic;
// a cropped-out pilot is not interpreted as a zero bit.
Candidate locate(const Tile& tile, const std::array<std::uint8_t, cells>& pilot,
    bool search, int hint_x, int hint_y, const dct::CancelToken& cancel) noexcept {
    Candidate best;
    double energy = 0;
    unsigned observed = 0;
    for (int i = 0; i < cells; ++i) if (tile.count[i]) {
        const double v = tile.pilot[i] / tile.count[i];
        energy += v * v;
        ++observed;
    }
    if (observed < 128 || energy < 1e-9) return best;
    const double norm = std::sqrt(energy * observed);
    for (int dy = 0; dy < (search ? side : 1); ++dy) {
        if (cancel.requested()) return best;
        for (int dx = 0; dx < (search ? side : 1); ++dx) {
            const int sx = search ? dx : hint_x;
            const int sy = search ? dy : hint_y;
            double sum = 0;
            for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
                const int i = y * side + x;
                if (tile.count[i]) {
                    const int expected = ((y + sy) & 31) * side + ((x + sx) & 31);
                    sum += (pilot[expected] ? -1 : 1) * tile.pilot[i] / tile.count[i];
                }
            }
            const double correlation = sum / norm;
            if (correlation > best.correlation) best = {correlation, sx, sy};
        }
    }
    return best;
}

Status decode(const Tile& tile, const Candidate& shift, const std::uint8_t* key,
    std::size_t key_length, ExtractResult& result) noexcept {
    std::uint8_t word[kCodewordSize]{};
    std::array<double, kCodewordSize> confidence{};
    std::array<bool, kCodewordSize> missing{};
    std::array<std::uint8_t, kCodewordSize> order{};
    for (int s = 0; s < static_cast<int>(kCodewordSize); ++s) {
        order[s] = static_cast<std::uint8_t>(s);
        confidence[s] = 1;
        for (int bit = 0; bit < 8; ++bit) {
            const int cell = data_cell(s, bit);
            const int i = (((cell / side - shift.y) & 31) * side)
                + ((cell % side - shift.x) & 31);
            if (!tile.count[i]) { missing[s] = true; confidence[s] = -1; }
            else confidence[s] = std::min(confidence[s], std::fabs(tile.data[i]) / tile.count[i]);
            if (tile.data[i] < 0) word[s] |= static_cast<std::uint8_t>(1 << (7 - bit));
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return confidence[a] < confidence[b]; });
    std::size_t required = 0;
    for (bool value : missing) if (value) ++required;
    if (required > kParitySize) return Status::capacity_exceeded;

    // Generalized minimum-distance style list: start with geometric erasures,
    // then erase increasingly many low-confidence symbols. Every candidate
    // must pass the full packet CRC, algorithm and keyed MAC checks.
    Status status = Status::corrupted;
    for (std::size_t extra = 0; required + extra <= kParitySize; extra += 4) {
        std::uint8_t packet[kPacketSize]{};
        std::uint8_t corrected = 0;
        if (!rs_decode_erasures(word, order.data(), required + extra, packet, corrected)) continue;
        DecodeResult decoded;
        status = decode_packet(key, key_length, packet, kAlgorithmRobustDct, decoded);
        std::memset(packet, 0, sizeof(packet));
        if (status == Status::success) {
            result.version = decoded.version;
            result.algorithm = decoded.algorithm;
            result.payload_length = decoded.payload_length;
            result.corrected_bytes = corrected;
            std::memcpy(result.payload, decoded.payload, decoded.payload_length);
            std::memset(decoded.payload, 0, sizeof(decoded.payload));
            std::memset(word, 0, sizeof(word));
            return status;
        }
    }
    std::memset(word, 0, sizeof(word));
    return status;
}

ExtractResult scan(const ImageView& image, const std::uint8_t* key,
    std::size_t key_length, const ExtractOptions& options) {
    ExtractResult best;
    std::array<std::uint8_t, cells> pilot{};
    pilot_bits(key, key_length, pilot);
    // Cache both orthogonal projections once per pixel phase. Strength retries
    // do not repeat the expensive image traversal or allocate full-size frames.
    std::vector<std::array<double, 2>> measured;
    const int phases = options.search_pixel_phase ? 8 : 1;
    for (int py = 0; py < phases; ++py) for (int px = 0; px < phases; ++px) {
        const int phase_x = options.search_pixel_phase ? px : options.phase_x;
        const int phase_y = options.search_pixel_phase ? py : options.phase_y;
        const unsigned nx = image.width > static_cast<unsigned>(phase_x) ? (image.width - phase_x) / 8 : 0;
        const unsigned ny = image.height > static_cast<unsigned>(phase_y) ? (image.height - phase_y) / 8 : 0;
        if (static_cast<std::size_t>(nx) * ny < 416) { best.status = Status::capacity_exceeded; continue; }
        measured.resize(static_cast<std::size_t>(nx) * ny);
        for (unsigned y = 0; y < ny; ++y) {
            if (options.cancel.requested()) { best.status = Status::cancelled; return best; }
            for (unsigned x = 0; x < nx; ++x) {
                auto& v = measured[y * nx + x];
                measure(image.pixels + (phase_y + y * 8) * image.stride + (phase_x + x * 8) * 4,
                    image.stride, v[0], v[1]);
            }
        }
        static constexpr double gains[] = {1, 0.9, 1.1, 0.8, 1.2, 0.7};
        for (double gain : gains) {
            if (options.cancel.requested()) { best.status = Status::cancelled; return best; }
            Tile tile;
            for (unsigned y = 0; y < ny; ++y) for (unsigned x = 0; x < nx; ++x) {
                const int i = (y & 31) * side + (x & 31);
                const auto& v = measured[y * nx + x];
                tile.data[i] += evidence(v[0], options.delta * (8.0 / 3.0) * gain);
                tile.pilot[i] += evidence(v[1], options.delta * 2 * gain);
                ++tile.count[i];
            }
            // Unwatermarked desktop surrounds often vote almost constantly
            // for zero. Remove that host bias before keyed synchronization
            // and payload decisions instead of drowning a smaller window.
            remove_host_bias(tile);
            const auto shift = locate(tile, pilot, options.search_tile,
                options.tile_x, options.tile_y, options.cancel);
            if (options.cancel.requested()) { best.status = Status::cancelled; return best; }
            const int score = static_cast<int>(shift.correlation * 1000);
            if (score > best.sync_score) {
                best.sync_score = score;
                best.phase_x = phase_x; best.phase_y = phase_y;
                best.tile_x = shift.x; best.tile_y = shift.y;
            }
            if (shift.correlation < 0.35) continue;
            ExtractResult candidate;
            candidate.sync_score = score;
            candidate.phase_x = phase_x; candidate.phase_y = phase_y;
            candidate.tile_x = shift.x; candidate.tile_y = shift.y;
            candidate.status = decode(tile, shift, key, key_length, candidate);
            if (candidate.status == Status::success) return candidate;
            if (score >= best.sync_score) best = candidate;
        }
    }
    return best;
}

} // namespace

Status embed(const MutableImageView& image, const std::uint8_t* key,
    std::size_t key_length, const std::uint8_t* payload, std::size_t payload_length,
    const EmbedOptions& options, EmbedResult& result) noexcept {
    result = {};
    if (!dct::valid_image(image) || !dct::valid_delta(options.delta) || !valid_key(key, key_length))
        return result.status = Status::invalid_argument;
    if (image.width < 256 || image.height < 256) return result.status = Status::capacity_exceeded;
    std::uint8_t word[kCodewordSize]{};
    const Status status = make_codeword(key, key_length, kAlgorithmRobustDct, payload, payload_length, word);
    if (status != Status::success) return result.status = status;
    std::array<std::uint8_t, cells> data{}, pilot{};
    for (int s = 0; s < static_cast<int>(kCodewordSize); ++s)
        for (int bit = 0; bit < 8; ++bit) data[data_cell(s, bit)] = (word[s] >> (7 - bit)) & 1;
    std::memset(word, 0, sizeof(word));
    pilot_bits(key, key_length, pilot);
    const auto& b = basis();
    const unsigned nx = image.width / 8, ny = image.height / 8;
    const double data_step = options.delta * (8.0 / 3.0), pilot_step = options.delta * 2;
    std::atomic<bool> cancelled{false};
    const auto rows = [&](unsigned begin, unsigned end) {
    for (unsigned by = begin; by < end; ++by) {
        if (options.cancel.requested()) { cancelled.store(true, std::memory_order_relaxed); return; }
        for (unsigned bx = 0; bx < nx; ++bx) {
            auto* p = image.pixels + by * 8 * image.stride + bx * 8 * 4;
            const unsigned i = (by & 31) * side + (bx & 31);
            // Alternating projections restore both cosets after integer
            // rounding and saturation, important for white/black desktop UI.
            for (int pass = 0; pass < 3; ++pass) {
                double d = 0, s = 0;
                measure(p, image.stride, d, s);
                const double dd = (quantize(d, data_step, data[i] != 0) - d) * 0.5;
                const double ds = quantize(s, pilot_step, pilot[i] != 0) - s;
                if (std::fabs(dd) < 0.3 && std::fabs(ds) < 0.3) break;
                for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
                    auto* pixel = p + y * image.stride + x * 4;
                    const double change = dd * b.data[y * 8 + x] + ds * b.pilot[y * 8 + x];
                    for (int c = 0; c < 3; ++c) pixel[c] = byte(pixel[c] + change);
                }
            }
        }
    }
    };
    unsigned threads = options.worker_threads;
    if (!threads) threads = std::thread::hardware_concurrency();
    threads = std::clamp(threads, 1u, std::min(8u, ny));
    if (threads == 1) rows(0, ny);
    else {
        std::vector<std::thread> workers;
        std::atomic<bool> start{false}, abort{false};
        bool failed = false;
        try {
            workers.reserve(threads - 1);
            for (unsigned i = 1; i < threads; ++i) workers.emplace_back([&, i] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                if (!abort.load(std::memory_order_acquire)) rows(ny * i / threads, ny * (i + 1) / threads);
            });
        } catch (...) { failed = true; abort.store(true, std::memory_order_release); }
        start.store(true, std::memory_order_release);
        if (!failed) rows(0, ny / threads);
        for (auto& worker : workers) worker.join();
        if (failed) rows(0, ny); // gated startup: no partial duplicate embedding
    }
    data.fill(0);
    pilot.fill(0);
    if (cancelled.load(std::memory_order_relaxed)) return result.status = Status::cancelled;
    result.blocks_x = nx; result.blocks_y = ny;
    result.min_carriers = (nx / side) * (ny / side);
    result.max_carriers = ((nx + side - 1) / side) * ((ny + side - 1) / side);
    return result.status = Status::success;
}

ExtractResult extract(const ImageView& image, const std::uint8_t* key,
    std::size_t key_length, const ExtractOptions& options) noexcept {
    ExtractResult invalid;
    invalid.status = Status::invalid_argument;
    if (!dct::valid_image(image) || !dct::valid_delta(options.delta) || !valid_key(key, key_length)
        || options.phase_x < 0 || options.phase_x > 7 || options.phase_y < 0 || options.phase_y > 7
        || options.tile_x < 0 || options.tile_x > 31 || options.tile_y < 0 || options.tile_y > 31
        || !std::isfinite(options.scale) || options.scale <= 0
        || !std::isfinite(options.rotation_degrees) || std::fabs(options.rotation_degrees) > 360
        || options.sharpen != 0 || options.search_rotation) return invalid;
    try {
        ImageView source = image;
        std::vector<std::uint8_t> rotated, scaled;
        if (options.rotation_degrees != 0) {
            unsigned w = 0, h = 0;
            dct::rotated_dimensions(image.width, image.height, options.rotation_degrees, w, h);
            rotated.resize(static_cast<std::size_t>(w) * h * 4);
            if (!dct::rotate_bicubic_bgra(image, options.rotation_degrees, rotated.data(), w * 4ull, options.cancel)) {
                if (options.cancel.requested()) invalid.status = Status::cancelled;
                return invalid;
            }
            source = {w, h, w * 4ull, rotated.data()};
        }
        if (options.cancel.requested()) { invalid.status = Status::cancelled; return invalid; }
        if (options.scale != 1) {
            const auto w = options.original_width, h = options.original_height;
            if (!w || !h || static_cast<std::uint64_t>(w) * h > dct::kMaxPixels) return invalid;
            scaled.resize(static_cast<std::size_t>(w) * h * 4);
            if (!dct::resize_bicubic_bgra(source, w, h, scaled.data(), w * 4ull, options.cancel)) {
                if (options.cancel.requested()) invalid.status = Status::cancelled;
                return invalid;
            }
            source = {w, h, w * 4ull, scaled.data()};
        }
        auto result = scan(source, key, key_length, options);
        result.rotation_degrees = options.rotation_degrees;
        return result;
    } catch (const std::bad_alloc&) {
        invalid.status = Status::capacity_exceeded;
        return invalid;
    } catch (...) {
        return invalid;
    }
}

} // namespace av::watermark::robust
