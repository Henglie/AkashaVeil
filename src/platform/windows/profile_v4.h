// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/identity.h"
#include "core/protection.h"
#include "platform/windows/windows.h"
#include <string>
#include <vector>

namespace av {

struct RuleV4 {
    std::wstring path;
    std::wstring class_name;
    std::wstring title;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int show = 1;
    bool enabled = true;
    ProtectionRequest request;

    MatchRule to_match_rule() const;
};

struct ProfileV4 {
    std::wstring language = L"auto";
    Theme theme = Theme::system;
    unsigned accent = 0xA66B37;
    std::vector<RuleV4> rules;
    Preferences preferences;
    ProtectionRequest defaults;
    int source_schema = 4;
};

enum class ProfileLoadStatus : unsigned {
    ok = 0,
    not_found = 1,
    invalid_data = 2,
    unsupported_schema = 3,
    io_error = 4
};

struct ProfileLoadResult {
    ProfileLoadStatus status = ProfileLoadStatus::invalid_data;
    DWORD error = ERROR_INVALID_DATA;
    int source_schema = 0;
    bool ok() const noexcept { return status == ProfileLoadStatus::ok; }
};

ProtectionRequest request_from_effects(Mode affinity, const Effects& effects);
Effects effects_from_request(const ProtectionRequest& request);
ProfileV4 migrate_profile_v3(const Profile& legacy, int source_schema);
bool valid_profile_v4(const ProfileV4& profile, DWORD& error) noexcept;

ProfileLoadResult load_profile_v4(const std::wstring& file, ProfileV4& profile);
bool save_profile_v4(const std::wstring& file, const ProfileV4& profile, DWORD& error);

}

