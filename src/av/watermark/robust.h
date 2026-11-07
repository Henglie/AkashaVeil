// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "av/watermark/dct.h"

namespace av::watermark::robust {

// R-DCT: differential luminance QIM, independent keyed 2D pilot, byte-local
// spatial interleaving and reliability-guided RS errors/erasures decoding.
// The packet is protocol V1, algorithm 3; legacy carriers remain independent.
// Strength uses the same 24-based UI units: data step = delta * 8/3,
// pilot step = delta * 2. Default steps are 64 and 48 luminance units.
using dct::ImageView;
using dct::MutableImageView;
using dct::EmbedOptions;
using dct::EmbedResult;
using dct::ExtractOptions;
using dct::ExtractResult;

// Embedding requires at least one complete 256x256 tile. A cancelled in-place
// operation may be partial; always supply a disposable working frame.
Status embed(const MutableImageView& image, const std::uint8_t* key,
    std::size_t key_length, const std::uint8_t* payload, std::size_t payload_length,
    const EmbedOptions& options, EmbedResult& result) noexcept;

// Pixel/tile phase is found from the distributed pilot when the corresponding
// search option is enabled. scale/original_* are explicit geometry hints.
// sync_score is pilot correlation in 0..1000 (not legacy matching bits /32).
ExtractResult extract(const ImageView& image, const std::uint8_t* key,
    std::size_t key_length, const ExtractOptions& options) noexcept;

} // namespace av::watermark::robust
