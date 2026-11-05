// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>

namespace av {

struct IdentitySettings {
    bool random_title = false;
    bool random_tooltip = false;
    bool random_icon = false;
    std::wstring brand_title;
    std::wstring brand_tooltip;
    int brand_icon = 0;
};

struct IdentityValues {
    std::wstring title;
    std::wstring tooltip;
    int icon_index = 0;
    bool title_random = false;
    bool tooltip_random = false;
    bool icon_random = false;
};

IdentityValues make_identity(const IdentitySettings& settings, std::uint64_t entropy);
std::wstring serialize_identity(const IdentityValues& values);
bool parse_identity(const std::wstring& text, IdentityValues& values);
std::wstring random_label(std::uint64_t entropy);

} // namespace av
