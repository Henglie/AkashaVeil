// SPDX-License-Identifier: Apache-2.0
// AkashaVeil watermark protocol V1 - shared constants, status and crypto primitives.
// Pure C++17, no OS/GUI dependency, no logging and no persistence of key material.
#pragma once

#include <cstddef>
#include <cstdint>

namespace av::watermark {

constexpr std::size_t kPayloadMax = 32;
constexpr std::size_t kPayloadField = 32;
constexpr std::size_t kPacketSize = 52;
constexpr std::size_t kCodewordSize = 124;
constexpr std::size_t kCodewordDataSize = 52;
constexpr std::size_t kParitySize = 72;
constexpr std::size_t kCorrectableBytes = 36;
constexpr std::size_t kTileSize = 32;
constexpr std::size_t kTileCells = 1024;
constexpr std::size_t kPayloadBits = 992;
constexpr std::size_t kSyncBits = 32;
constexpr std::size_t kKeyMax = 128;
constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::uint8_t kAlgorithmDctPairQim = 1;
constexpr std::uint8_t kAlgorithmDwtLh3Qim = 2;
constexpr std::uint8_t kAlgorithmRobustDct = 3;
constexpr int kSyncThreshold = 26;

extern const std::uint8_t kMagic[4];

enum class Status : std::uint8_t {
    success = 0,
    not_detected = 1,
    corrupted = 2,
    unsupported_version = 3,
    unsupported_algorithm = 4,
    wrong_key = 5,
    invalid_argument = 6,
    cancelled = 7,
    payload_too_large = 8,
    capacity_exceeded = 9
};

const char* status_name(Status status) noexcept;
bool known_algorithm(std::uint8_t algorithm) noexcept;
bool valid_key(const std::uint8_t* key, std::size_t key_length) noexcept;

// IEEE 802.3 / zlib CRC-32. Used for accidental-corruption detection only.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

// FIPS 180-4 SHA-256 and RFC 2104 HMAC-SHA256.
void sha256(const std::uint8_t* data, std::size_t size, std::uint8_t out[32]) noexcept;
void hmac_sha256(const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t* data, std::size_t size, std::uint8_t out[32]) noexcept;

// Key derivations. Keys are caller-owned, never copied into packets or results.
void derive_key_id(const std::uint8_t* key, std::size_t key_length, std::uint8_t out[4]) noexcept;
void derive_payload_stream(const std::uint8_t* key, std::size_t key_length,
    std::uint8_t out[kPayloadField]) noexcept;
void derive_mac_key(const std::uint8_t* key, std::size_t key_length, std::uint8_t out[32]) noexcept;
void derive_sync_word(std::uint8_t out[4]) noexcept;

} // namespace av::watermark
