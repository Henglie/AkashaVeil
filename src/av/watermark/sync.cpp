// SPDX-License-Identifier: Apache-2.0
#include "av/watermark/sync.h"

#include <cstring>

namespace av::watermark {

namespace {

struct SyncWord {
    std::uint8_t bytes[4];
    std::uint8_t bits[kSyncBits];
};

const SyncWord& sync_word() noexcept
{
    static const SyncWord value = [] {
        SyncWord result{};
        derive_sync_word(result.bytes);
        for (int i = 0; i < 4; ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                result.bits[i * 8 + bit] =
                    static_cast<std::uint8_t>((result.bytes[i] >> (7 - bit)) & 1u);
            }
        }
        return result;
    }();
    return value;
}

} // namespace

void soft_tile_reset(SoftTile& tile) noexcept
{
    std::memset(&tile, 0, sizeof(tile));
}

void tile_cell(std::uint32_t bit, int tile_y, int tile_x, std::uint32_t& cell) noexcept
{
    const std::uint32_t row = (bit / kTileSize + static_cast<std::uint32_t>(tile_y)) % kTileSize;
    const std::uint32_t column = (bit % kTileSize + static_cast<std::uint32_t>(tile_x)) % kTileSize;
    cell = row * kTileSize + column;
}

int sync_score(const SoftTile& tile, int tile_y, int tile_x) noexcept
{
    const SyncWord& word = sync_word();
    int good = 0;
    for (std::uint32_t i = 0; i < kSyncBits; ++i) {
        std::uint32_t cell = 0;
        tile_cell(static_cast<std::uint32_t>(kPayloadBits) + i, tile_y, tile_x, cell);
        if (tile.count[cell] == 0) {
            continue;
        }
        if (soft_bit(tile.score[cell]) == (word.bits[i] != 0)) {
            ++good;
        }
    }
    return good;
}

int best_tile_shift(const SoftTile& tile, int& tile_y, int& tile_x) noexcept
{
    int best = -1;
    tile_y = 0;
    tile_x = 0;
    for (int y = 0; y < static_cast<int>(kTileSize); ++y) {
        for (int x = 0; x < static_cast<int>(kTileSize); ++x) {
            const int score = sync_score(tile, y, x);
            if (score > best) {
                best = score;
                tile_y = y;
                tile_x = x;
            }
        }
    }
    return best;
}

void codeword_from_soft(const SoftTile& tile, int tile_y, int tile_x,
    std::uint8_t out_codeword[kCodewordSize]) noexcept
{
    if (out_codeword == nullptr) {
        return;
    }
    std::memset(out_codeword, 0, kCodewordSize);
    for (std::uint32_t i = 0; i < kPayloadBits; ++i) {
        std::uint32_t cell = 0;
        tile_cell(i, tile_y, tile_x, cell);
        if (soft_bit(tile.score[cell])) {
            out_codeword[i / 8] |= static_cast<std::uint8_t>(1u << (7 - (i % 8)));
        }
    }
}

void codeword_to_bits(const std::uint8_t codeword[kCodewordSize],
    std::uint8_t out_bits[kTileCells]) noexcept
{
    if (codeword == nullptr || out_bits == nullptr) {
        return;
    }
    const SyncWord& word = sync_word();
    for (std::uint32_t i = 0; i < kPayloadBits; ++i) {
        out_bits[i] = static_cast<std::uint8_t>((codeword[i / 8] >> (7 - (i % 8))) & 1u);
    }
    for (std::uint32_t i = 0; i < kSyncBits; ++i) {
        out_bits[static_cast<std::size_t>(kPayloadBits) + i] = word.bits[i];
    }
}

bool bits_to_codeword(const std::uint8_t bits[kPayloadBits],
    std::uint8_t out_codeword[kCodewordSize]) noexcept
{
    if (bits == nullptr || out_codeword == nullptr) {
        return false;
    }
    std::memset(out_codeword, 0, kCodewordSize);
    for (std::uint32_t i = 0; i < kPayloadBits; ++i) {
        if (bits[i] & 1u) {
            out_codeword[i / 8] |= static_cast<std::uint8_t>(1u << (7 - (i % 8)));
        }
    }
    return true;
}

} // namespace av::watermark
