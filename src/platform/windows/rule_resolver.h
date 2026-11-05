// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/identity.h"
#include "platform/windows/profile_v4.h"
#include <cstddef>
#include <vector>

namespace av {

enum class RuleResolutionState : unsigned {
    disabled = 0,
    invalid = 1,
    none = 2,
    matched = 3,
    ambiguous = 4
};

struct RuleResolution {
    std::size_t rule = 0;
    RuleResolutionState state = RuleResolutionState::invalid;
    std::size_t count = 0;
    std::vector<std::size_t> matches;
    DWORD error = ERROR_SUCCESS;
};

// Resolve persisted rules against a fresh enumeration.  No HWND/PID is taken
// from the profile; ambiguous matches keep every candidate and never pick one
// implicitly.  The caller must ask the user before applying layout/protection.
std::vector<RuleResolution> resolve_rules(const ProfileV4& profile,
    const std::vector<TargetIdentity>& targets);

bool resolution_applicable(const RuleResolution& resolution) noexcept;

}
