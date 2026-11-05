// SPDX-License-Identifier: Apache-2.0
// 32x32 tile mapping and 32-bit synchronisation word for protocol V1.
#pragma once

#include <cstddef>
#include <cstdint>

#include "av/watermark/types.h"

namespace av::watermark {

// Aggregated per-cell soft evidence from all carriers. Positive score means
// bit 0; a negative score means bit 1.
struct SoftTile {
    double score[kTileCells] = {};
    std::uint32_t count[kTileCells] = {};
};

void soft_tile_reset(SoftTile& tile) noexcept;
inline bool soft_bit(double score) noexcept { return score < 0.0; }

// bit 0..1023 -> cell (row * 32 + col), with optional tile shift.
void tile_cell(std::uint32_t bit, int tile_y, int tile_x, std::uint32_t& cell) noexcept;

// Number of matching sync bits (0..32); cells with zero carriers are skipped.
int sync_score(const SoftTile& tile, int tile_y, int tile_x) noexcept;

// Exhaustive 32x32 tile-shift search. First maximum wins (row-major order).
int best_tile_shift(const SoftTile& tile, int& tile_y, int& tile_x) noexcept;

// Packs 992 soft decisions MSB-first into the 124-byte codeword.
void codeword_from_soft(const SoftTile& tile, int tile_y, int tile_x,
    std::uint8_t out_codeword[kCodewordSize]) noexcept;

// MSB-first serialisation of the 124-byte codeword plus the 32 sync bits.
void codeword_to_bits(const std::uint8_t codeword[kCodewordSize],
    std::uint8_t out_bits[kTileCells]) noexcept;

// Inverse of the payload part only (992 bits -> 124 bytes).
bool bits_to_codeword(const std::uint8_t bits[kPayloadBits],
    std::uint8_t out_codeword[kCodewordSize]) noexcept;

} // namespace av::watermark
