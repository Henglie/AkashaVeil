// SPDX-License-Identifier: Apache-2.0
#include "av/watermark/types.h"

#include <array>
#include <cstring>

namespace av::watermark {

const std::uint8_t kMagic[4] = {'A', 'V', 'W', '1'};

namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t kSha256H0[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

inline std::uint32_t rotr(std::uint32_t x, int n) noexcept
{
    return (x >> n) | (x << (32 - n));
}

void sha256_transform(std::uint32_t state[8], const std::uint8_t block[64]) noexcept
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
            | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
            | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
            | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

struct Sha256Context {
    std::uint32_t state[8];
    std::uint8_t buffer[64];
    std::size_t used = 0;
    std::uint64_t total = 0;
};

void sha256_init(Sha256Context& context) noexcept
{
    std::memcpy(context.state, kSha256H0, sizeof(context.state));
    context.used = 0;
    context.total = 0;
}

void sha256_absorb(Sha256Context& context, const std::uint8_t* data, std::size_t size) noexcept
{
    if (data == nullptr || size == 0) {
        return;
    }
    context.total += size;
    std::size_t offset = 0;
    if (context.used > 0) {
        const std::size_t take = (size < 64 - context.used) ? size : (64 - context.used);
        std::memcpy(context.buffer + context.used, data, take);
        context.used += take;
        offset += take;
        if (context.used == 64) {
            sha256_transform(context.state, context.buffer);
            context.used = 0;
        }
    }
    while (size - offset >= 64) {
        sha256_transform(context.state, data + offset);
        offset += 64;
    }
    if (offset < size) {
        context.used = size - offset;
        std::memcpy(context.buffer, data + offset, context.used);
    }
}

void sha256_finish(Sha256Context& context, std::uint8_t out[32]) noexcept
{
    const std::uint64_t bit_length = context.total * 8u;
    std::uint8_t padding[72];
    std::memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;
    const std::size_t tail = (context.used < 56) ? (56 - context.used) : (120 - context.used);
    sha256_absorb(context, padding, tail);
    std::uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (8 * (7 - i)));
    }
    sha256_absorb(context, length_bytes, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(context.state[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(context.state[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(context.state[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(context.state[i]);
    }
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t size) noexcept
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            values[i] = value;
        }
        return values;
    }();
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

void derive_labeled(const char* label, std::size_t label_size,
    const std::uint8_t* key, std::size_t key_length, std::uint8_t out[32]) noexcept
{
    Sha256Context context;
    sha256_init(context);
    sha256_absorb(context, reinterpret_cast<const std::uint8_t*>(label), label_size);
    sha256_absorb(context, key, key_length);
    sha256_finish(context, out);
}

} // namespace

const char* status_name(Status status) noexcept
{
    switch (status) {
    case Status::success: return "SUCCESS";
    case Status::not_detected: return "NOT_DETECTED";
    case Status::corrupted: return "CORRUPTED";
    case Status::unsupported_version: return "UNSUPPORTED_VERSION";
    case Status::unsupported_algorithm: return "UNSUPPORTED_ALGORITHM";
    case Status::wrong_key: return "WRONG_KEY";
    case Status::invalid_argument: return "INVALID_ARGUMENT";
    case Status::cancelled: return "CANCELLED";
    case Status::payload_too_large: return "PAYLOAD_TOO_LARGE";
    case Status::capacity_exceeded: return "CAPACITY_EXCEEDED";
    }
    return "UNKNOWN";
}

bool known_algorithm(std::uint8_t algorithm) noexcept
{
    return algorithm == kAlgorithmDctPairQim || algorithm == kAlgorithmDwtLh3Qim
        || algorithm == kAlgorithmRobustDct;
}

bool valid_key(const std::uint8_t* key, std::size_t key_length) noexcept
{
    return key != nullptr && key_length > 0 && key_length <= kKeyMax;
}

void sha256(const std::uint8_t* data, std::size_t size, std::uint8_t out[32]) noexcept
{
    if (out == nullptr) {
        return;
    }
    Sha256Context context;
    sha256_init(context);
    sha256_absorb(context, data, size);
    sha256_finish(context, out);
}

void hmac_sha256(const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t* data, std::size_t size, std::uint8_t out[32]) noexcept
{
    if (out == nullptr) {
        return;
    }
    std::uint8_t normalized[64];
    std::memset(normalized, 0, sizeof(normalized));
    if (key != nullptr && key_length > 64) {
        sha256(key, key_length, normalized);
    } else if (key != nullptr && key_length > 0) {
        std::memcpy(normalized, key, key_length);
    }
    std::uint8_t inner_pad[64];
    std::uint8_t outer_pad[64];
    for (int i = 0; i < 64; ++i) {
        inner_pad[i] = static_cast<std::uint8_t>(normalized[i] ^ 0x36u);
        outer_pad[i] = static_cast<std::uint8_t>(normalized[i] ^ 0x5cu);
    }
    Sha256Context inner;
    sha256_init(inner);
    sha256_absorb(inner, inner_pad, 64);
    sha256_absorb(inner, data, size);
    std::uint8_t digest[32];
    sha256_finish(inner, digest);

    Sha256Context outer;
    sha256_init(outer);
    sha256_absorb(outer, outer_pad, 64);
    sha256_absorb(outer, digest, 32);
    sha256_finish(outer, out);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept
{
    return crc32_update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

void derive_key_id(const std::uint8_t* key, std::size_t key_length, std::uint8_t out[4]) noexcept
{
    std::uint8_t digest[32];
    static const char label[] = "AV-KEY-ID";
    derive_labeled(label, sizeof(label) - 1, key, key_length, digest);
    std::memcpy(out, digest, 4);
}

void derive_payload_stream(const std::uint8_t* key, std::size_t key_length,
    std::uint8_t out[kPayloadField]) noexcept
{
    Sha256Context context;
    sha256_init(context);
    static const char label[] = "AV-PAYLOAD";
    sha256_absorb(context, reinterpret_cast<const std::uint8_t*>(label), sizeof(label) - 1);
    sha256_absorb(context, key, key_length);
    const std::uint8_t counter[4] = {0, 0, 0, 0};
    sha256_absorb(context, counter, sizeof(counter));
    sha256_finish(context, out);
}

void derive_mac_key(const std::uint8_t* key, std::size_t key_length, std::uint8_t out[32]) noexcept
{
    static const char label[] = "AV-MAC-KEY";
    derive_labeled(label, sizeof(label) - 1, key, key_length, out);
}

void derive_sync_word(std::uint8_t out[4]) noexcept
{
    static const char label[] = "AV-SYNC-V1";
    std::uint8_t digest[32];
    sha256(reinterpret_cast<const std::uint8_t*>(label), sizeof(label) - 1, digest);
    std::memcpy(out, digest, 4);
}

} // namespace av::watermark
