// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace av {

struct TargetIdentity {
    std::uintptr_t window = 0;
    std::uint32_t pid = 0;
    std::uint32_t thread = 0;
    std::uint64_t process_created = 0;
    std::wstring title;
    std::wstring class_name;
    std::wstring path;
    std::wstring architecture;

    bool has_runtime_identity() const noexcept;
    bool same_runtime(const TargetIdentity& other) const noexcept;
};

using TargetKey = std::tuple<std::uintptr_t, std::uint32_t, std::uint64_t>;

TargetKey target_key(const TargetIdentity& target) noexcept;

enum class IdentityFreshness : unsigned {
    current = 0,
    expired = 1,
    incomplete = 2
};

IdentityFreshness compare_identity(const TargetIdentity& snapshot,
    const TargetIdentity& current) noexcept;

struct MatchRule {
    std::wstring path;
    std::wstring class_name;
    std::wstring title;
    bool enabled = true;
    bool path_case_insensitive = true;
};

enum class RuleMatchStatus : unsigned {
    none = 0,
    matched = 1,
    ambiguous = 2,
    invalid = 3
};

struct RuleMatch {
    RuleMatchStatus status = RuleMatchStatus::none;
    std::size_t index = 0;
    std::size_t count = 0;
};

bool valid_match_rule(const MatchRule& rule) noexcept;
bool rule_matches(const MatchRule& rule, const TargetIdentity& target) noexcept;
std::vector<std::size_t> matching_indices(const MatchRule& rule,
    const std::vector<TargetIdentity>& targets);
RuleMatch match_rule(const MatchRule& rule,
    const std::vector<TargetIdentity>& targets);

}
