// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/identity.h"
#include "platform/windows/profile_v4.h"
#include <cstddef>

namespace av {

enum class RuleEditStatus : unsigned {
    ok = 0,
    invalid = 1,
    duplicate = 2,
    not_found = 3,
    limit = 4
};

struct RuleEditResult {
    RuleEditStatus status = RuleEditStatus::invalid;
    std::size_t index = 0;
    DWORD error = ERROR_INVALID_DATA;
    bool ok() const noexcept { return status == RuleEditStatus::ok; }
};

bool valid_rule_v4(const RuleV4& rule, DWORD& error) noexcept;

// Exact persisted identity: path is case/slash-insensitive; class and title are
// exact. Runtime HWND/PID are deliberately absent from RuleV4 and never saved.
std::size_t find_rule_index(const ProfileV4& profile, const RuleV4& rule) noexcept;

RuleEditResult add_rule(ProfileV4& profile, const RuleV4& rule);
RuleEditResult upsert_rule(ProfileV4& profile, const RuleV4& rule);
RuleEditResult update_rule(ProfileV4& profile, std::size_t index, const RuleV4& rule);
RuleEditResult remove_rule(ProfileV4& profile, std::size_t index);
RuleEditResult set_rule_enabled(ProfileV4& profile, std::size_t index, bool enabled);

struct RuleGeometry {
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
    int show = 1;
};

RuleV4 rule_from_target(const TargetIdentity& target, const RuleGeometry& geometry,
    const ProtectionRequest& request);

}
