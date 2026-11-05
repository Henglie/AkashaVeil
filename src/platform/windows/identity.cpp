// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/identity.h"
#include <algorithm>
#include <vector>

namespace av {
namespace {

std::uint64_t mix(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

std::wstring hex_encode(const std::wstring& value) {
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    result.reserve(value.size() * 4);
    for (const wchar_t unit : value) {
        for (int shift = 12; shift >= 0; shift -= 4) result += digits[(unit >> shift) & 0xF];
    }
    return result;
}

bool hex_decode(const std::wstring& text, std::wstring& value) {
    if (text.size() % 4) return false;
    value.clear();
    value.reserve(text.size() / 4);
    auto digit = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int unit = 0;
        for (int j = 0; j < 4; ++j) {
            const int value_digit = digit(text[i + j]);
            if (value_digit < 0) return false;
            unit = (unit << 4) | value_digit;
        }
        value.push_back(static_cast<wchar_t>(unit));
    }
    return true;
}

std::vector<std::wstring> split(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(L'|', start);
        parts.push_back(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

} // namespace

std::wstring random_label(std::uint64_t entropy) {
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring label = L"AV-";
    for (int shift = 28; shift >= 0; shift -= 4) label += digits[(entropy >> shift) & 0xF];
    return label;
}

IdentityValues make_identity(const IdentitySettings& settings, std::uint64_t entropy) {
    const auto title_seed = mix(entropy ^ 0x41C64E6DA0E1F13Bull);
    const auto tooltip_seed = mix(entropy ^ 0x9E3779B97F4A7C15ull);
    const auto icon_seed = mix(entropy ^ 0xD1B54A32D192ED03ull);
    IdentityValues values;
    values.title_random = settings.random_title;
    values.tooltip_random = settings.random_tooltip;
    values.icon_random = settings.random_icon;
    values.title = settings.random_title ? random_label(title_seed) : settings.brand_title;
    values.tooltip = settings.random_tooltip ? random_label(tooltip_seed) : settings.brand_tooltip;
    values.icon_index = settings.random_icon ? static_cast<int>(icon_seed % 5) : settings.brand_icon;
    return values;
}

std::wstring serialize_identity(const IdentityValues& values) {
    return L"AVI1|" + std::wstring(1, values.title_random ? L'1' : L'0')
        + L"|" + std::wstring(1, values.tooltip_random ? L'1' : L'0')
        + L"|" + std::wstring(1, values.icon_random ? L'1' : L'0')
        + L"|" + std::to_wstring(values.icon_index)
        + L"|" + hex_encode(values.title)
        + L"|" + hex_encode(values.tooltip);
}

bool parse_identity(const std::wstring& text, IdentityValues& values) {
    const auto parts = split(text);
    if (parts.size() != 7 || parts[0] != L"AVI1") return false;
    if (parts[1].size() != 1 || parts[2].size() != 1 || parts[3].size() != 1) return false;
    if ((parts[1][0] != L'0' && parts[1][0] != L'1') || (parts[2][0] != L'0' && parts[2][0] != L'1')
        || (parts[3][0] != L'0' && parts[3][0] != L'1')) return false;
    wchar_t* end = nullptr;
    const auto index = std::wcstol(parts[4].c_str(), &end, 10);
    if (parts[4].empty() || *end || index < 0 || index > 4) return false;
    IdentityValues parsed;
    if (!hex_decode(parts[5], parsed.title) || !hex_decode(parts[6], parsed.tooltip)) return false;
    if (parsed.title.size() > 256 || parsed.tooltip.size() > 256) return false;
    parsed.title_random = parts[1][0] == L'1';
    parsed.tooltip_random = parts[2][0] == L'1';
    parsed.icon_random = parts[3][0] == L'1';
    parsed.icon_index = static_cast<int>(index);
    values = std::move(parsed);
    return true;
}

} // namespace av
