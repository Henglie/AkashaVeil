// SPDX-License-Identifier: Apache-2.0
#include "av/watermark/ecc.h"

#include <array>
#include <cstring>

namespace av::watermark {

namespace {

constexpr std::uint16_t kPrimitive = 0x11D;
constexpr int kRootCount = static_cast<int>(kParitySize);

struct GfTables {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};
};

const GfTables& tables() noexcept
{
    static const GfTables value = [] {
        GfTables result;
        std::uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            result.exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            result.log[x] = static_cast<std::uint8_t>(i);
            x = static_cast<std::uint16_t>(x << 1);
            if (x & 0x100u) {
                x = static_cast<std::uint16_t>(x ^ kPrimitive);
            }
        }
        for (int i = 255; i < 512; ++i) {
            result.exp[static_cast<std::size_t>(i)] = result.exp[static_cast<std::size_t>(i - 255)];
        }
        return result;
    }();
    return value;
}

inline std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) noexcept
{
    if (a == 0 || b == 0) {
        return 0;
    }
    const GfTables& t = tables();
    return t.exp[static_cast<std::size_t>(t.log[a]) + t.log[b]];
}

inline std::uint8_t gf_div(std::uint8_t a, std::uint8_t b) noexcept
{
    if (b == 0) {
        return 0;
    }
    if (a == 0) {
        return 0;
    }
    const GfTables& t = tables();
    int index = static_cast<int>(t.log[a]) - static_cast<int>(t.log[b]);
    if (index < 0) {
        index += 255;
    }
    return t.exp[static_cast<std::size_t>(index)];
}

inline std::uint8_t gf_pow(std::uint8_t a, int n) noexcept
{
    if (a == 0) {
        return (n == 0) ? 1 : 0;
    }
    const GfTables& t = tables();
    int exponent = (static_cast<int>(t.log[a]) * n) % 255;
    if (exponent < 0) {
        exponent += 255;
    }
    return t.exp[static_cast<std::size_t>(exponent)];
}

inline std::uint8_t gf_inv(std::uint8_t a) noexcept
{
    if (a == 0) {
        return 0;
    }
    const GfTables& t = tables();
    return t.exp[static_cast<std::size_t>((255 - static_cast<int>(t.log[a])) % 255)];
}

struct Generator {
    std::array<std::uint8_t, kParitySize + 1> coefficients{};
    int size = 1;
};

const Generator& generator() noexcept
{
    static const Generator value = [] {
        Generator result;
        result.coefficients[0] = 1;
        result.size = 1;
        std::array<std::uint8_t, kParitySize + 2> next{};
        for (int root = 0; root < kRootCount; ++root) {
            const std::uint8_t term = gf_pow(2, root);
            for (auto& item : next) {
                item = 0;
            }
            for (int i = 0; i < result.size; ++i) {
                next[static_cast<std::size_t>(i)] ^= result.coefficients[static_cast<std::size_t>(i)];
                next[static_cast<std::size_t>(i + 1)] ^= gf_mul(result.coefficients[static_cast<std::size_t>(i)], term);
            }
            std::memcpy(result.coefficients.data(), next.data(), static_cast<std::size_t>(result.size + 1));
            ++result.size;
        }
        return result;
    }();
    return value;
}

std::uint8_t poly_eval_desc(const std::uint8_t* poly, int size, std::uint8_t x) noexcept
{
    std::uint8_t y = 0;
    for (int i = 0; i < size; ++i) {
        y = static_cast<std::uint8_t>(gf_mul(y, x) ^ poly[i]);
    }
    return y;
}

std::uint8_t poly_eval_asc(const std::uint8_t* poly, int size, std::uint8_t x) noexcept
{
    std::uint8_t y = 0;
    for (int i = size - 1; i >= 0; --i) {
        y = static_cast<std::uint8_t>(gf_mul(y, x) ^ poly[i]);
    }
    return y;
}

bool syndromes_zero(const std::uint8_t* codeword) noexcept
{
    for (int i = 0; i < kRootCount; ++i) {
        if (poly_eval_desc(codeword, static_cast<int>(kCodewordSize), gf_pow(2, i)) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

void rs_encode(const std::uint8_t message[kCodewordDataSize],
    std::uint8_t out[kCodewordSize]) noexcept
{
    if (message == nullptr || out == nullptr) {
        return;
    }
    std::uint8_t work[kCodewordSize];
    std::memcpy(work, message, kCodewordDataSize);
    std::memset(work + kCodewordDataSize, 0, kParitySize);
    const Generator& gen = generator();
    for (std::size_t i = 0; i < kCodewordDataSize; ++i) {
        const std::uint8_t coefficient = work[i];
        if (coefficient == 0) {
            continue;
        }
        for (int j = 1; j < gen.size; ++j) {
            work[i + static_cast<std::size_t>(j)] ^=
                gf_mul(gen.coefficients[static_cast<std::size_t>(j)], coefficient);
        }
    }
    std::memcpy(out, message, kCodewordDataSize);
    std::memcpy(out + kCodewordDataSize, work + kCodewordDataSize, kParitySize);
}

bool rs_decode(const std::uint8_t codeword[kCodewordSize],
    std::uint8_t out_message[kCodewordDataSize], std::uint8_t& corrected) noexcept
{
    corrected = 0;
    if (codeword == nullptr || out_message == nullptr) {
        return false;
    }
    std::uint8_t received[kCodewordSize];
    std::memcpy(received, codeword, kCodewordSize);

    std::array<std::uint8_t, kParitySize> syndromes{};
    bool clean = true;
    for (int i = 0; i < kRootCount; ++i) {
        syndromes[static_cast<std::size_t>(i)] =
            poly_eval_desc(received, static_cast<int>(kCodewordSize), gf_pow(2, i));
        if (syndromes[static_cast<std::size_t>(i)] != 0) {
            clean = false;
        }
    }
    if (clean) {
        std::memcpy(out_message, received, kCodewordDataSize);
        return true;
    }

    // Berlekamp-Massey: locator polynomial C(x) in ascending order.
    std::array<std::uint8_t, kParitySize + 2> C{};
    std::array<std::uint8_t, kParitySize + 2> B{};
    std::array<std::uint8_t, kParitySize + 2> T{};
    std::array<std::uint8_t, kParitySize + 2> next{};
    C[0] = 1;
    B[0] = 1;
    int length = 0;
    int m = 1;
    std::uint8_t b = 1;
    for (int n = 0; n < kRootCount; ++n) {
        std::uint8_t discrepancy = syndromes[static_cast<std::size_t>(n)];
        for (int i = 1; i <= length; ++i) {
            discrepancy ^= gf_mul(C[static_cast<std::size_t>(i)],
                syndromes[static_cast<std::size_t>(n - i)]);
        }
        if (discrepancy == 0) {
            ++m;
            continue;
        }
        const std::uint8_t factor = gf_div(discrepancy, b);
        T = C;
        next.fill(0);
        for (int i = 0; i <= length; ++i) {
            next[static_cast<std::size_t>(i)] = C[static_cast<std::size_t>(i)];
        }
        for (int i = 0; i < kParitySize + 2 - m; ++i) {
            if (B[static_cast<std::size_t>(i)] == 0) {
                continue;
            }
            next[static_cast<std::size_t>(i + m)] ^=
                gf_mul(factor, B[static_cast<std::size_t>(i)]);
        }
        C = next;
        if (2 * length <= n) {
            length = n + 1 - length;
            B = T;
            b = discrepancy;
            m = 1;
        } else {
            ++m;
        }
    }
    if (length <= 0 || length > static_cast<int>(kCorrectableBytes)) {
        return false;
    }
    int locator_size = kParitySize + 2;
    while (locator_size > 1 && C[static_cast<std::size_t>(locator_size - 1)] == 0) {
        --locator_size;
    }

    int positions[kCorrectableBytes];
    std::uint8_t roots[kCorrectableBytes];
    int found = 0;
    for (int p = 0; p < static_cast<int>(kCodewordSize); ++p) {
        const std::uint8_t x = gf_pow(2, static_cast<int>(kCodewordSize) - 1 - p);
        if (poly_eval_asc(C.data(), locator_size, gf_inv(x)) == 0) {
            if (found >= static_cast<int>(kCorrectableBytes)) {
                return false;
            }
            positions[found] = p;
            roots[found] = x;
            ++found;
        }
    }
    if (found != length) {
        return false;
    }

    // Solve S_j = sum_i e_i * X_i^j for j = 0..length-1.
    std::array<std::array<std::uint8_t, kCorrectableBytes + 1>, kCorrectableBytes> matrix{};
    for (int row = 0; row < length; ++row) {
        for (int col = 0; col < length; ++col) {
            matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] =
                gf_pow(roots[col], row);
        }
        matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(length)] =
            syndromes[static_cast<std::size_t>(row)];
    }
    for (int col = 0; col < length; ++col) {
        int pivot = -1;
        for (int row = col; row < length; ++row) {
            if (matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] != 0) {
                pivot = row;
                break;
            }
        }
        if (pivot < 0) {
            return false;
        }
        if (pivot != col) {
            std::swap(matrix[static_cast<std::size_t>(col)], matrix[static_cast<std::size_t>(pivot)]);
        }
        const std::uint8_t inverse =
            gf_inv(matrix[static_cast<std::size_t>(col)][static_cast<std::size_t>(col)]);
        if (inverse == 0) {
            return false;
        }
        for (int k = col; k <= length; ++k) {
            matrix[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)] =
                gf_mul(matrix[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)], inverse);
        }
        for (int row = 0; row < length; ++row) {
            if (row == col) {
                continue;
            }
            const std::uint8_t factor = matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
            if (factor == 0) {
                continue;
            }
            for (int k = col; k <= length; ++k) {
                matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] ^=
                    gf_mul(factor, matrix[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)]);
            }
        }
    }
    for (int i = 0; i < length; ++i) {
        received[static_cast<std::size_t>(positions[i])] ^=
            matrix[static_cast<std::size_t>(i)][static_cast<std::size_t>(length)];
    }
    if (!syndromes_zero(received)) {
        return false;
    }
    std::memcpy(out_message, received, kCodewordDataSize);
    corrected = static_cast<std::uint8_t>(length);
    return true;
}

} // namespace av::watermark
