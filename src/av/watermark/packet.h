// SPDX-License-Identifier: Apache-2.0
// Watermark packet V1: 52-byte plaintext packet, RS(124,52) codeword.
#pragma once

#include <cstddef>
#include <cstdint>

#include "av/watermark/types.h"

namespace av::watermark {

struct DecodeResult {
    Status status = Status::not_detected;
    std::uint8_t version = 0;
    std::uint8_t algorithm = 0;
    std::uint8_t flags = 0;
    std::uint8_t payload_length = 0;
    std::uint8_t key_id[4] = {0, 0, 0, 0};
    std::uint8_t corrected_bytes = 0;
    std::uint8_t payload[kPayloadMax] = {};
};

// Builds the 52-byte packet (header, encrypted payload, CRC-32, truncated MAC).
// expected_algorithm is not used for encoding; algorithm must be 1 or 2.
Status encode_packet(const std::uint8_t* key, std::size_t key_length,
    std::uint8_t algorithm, const std::uint8_t* payload, std::size_t payload_length,
    std::uint8_t out_packet[kPacketSize]) noexcept;

// Parses one 52-byte packet. expected_algorithm = 0 accepts either known
// algorithm id; 1 or 2 rejects a foreign algorithm id before CRC/MAC work.
Status decode_packet(const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t packet[kPacketSize], std::uint8_t expected_algorithm,
    DecodeResult& result) noexcept;

// Convenience: encode + Reed-Solomon.
Status make_codeword(const std::uint8_t* key, std::size_t key_length,
    std::uint8_t algorithm, const std::uint8_t* payload, std::size_t payload_length,
    std::uint8_t out_codeword[kCodewordSize]) noexcept;

// Convenience: Reed-Solomon + decode. RS failure -> corrupted. Missing magic
// after a successful RS decode -> not_detected (the caller may remap that to
// corrupted when synchronisation was already confirmed).
Status read_codeword(const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t codeword[kCodewordSize], std::uint8_t expected_algorithm,
    DecodeResult& result) noexcept;

} // namespace av::watermark
