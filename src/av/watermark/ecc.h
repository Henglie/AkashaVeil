// SPDX-License-Identifier: Apache-2.0
// Reed-Solomon RS(124,52) over GF(256), primitive polynomial 0x11D,
// generator roots alpha^0..alpha^71, systematic layout. Corrects 36 bytes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "av/watermark/types.h"

namespace av::watermark {

// Systematic encoder. out[0..51] = message, out[52..123] = parity.
void rs_encode(const std::uint8_t message[kCodewordDataSize],
    std::uint8_t out[kCodewordSize]) noexcept;

// Bounded-distance decoder. Returns true on success; corrected receives the
// number of corrected bytes (0..36). False means uncorrectable.
bool rs_decode(const std::uint8_t codeword[kCodewordSize],
    std::uint8_t out_message[kCodewordDataSize], std::uint8_t& corrected) noexcept;

} // namespace av::watermark
