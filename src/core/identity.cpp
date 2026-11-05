// SPDX-License-Identifier: Apache-2.0
#include "core/identity.h"
#include <cwctype>

namespace av {

bool TargetIdentity::has_runtime_identity() const noexcept {
    return window != 0 && pid != 0 && process_created != 0;
}

bool TargetIdentity::same_runtime(const TargetIdentity& other) const noexcept {
    return window == other.window && pid == other.pid && thread == other.thread
        && process_created == other.process_created;
}

TargetKey target_key(const TargetIdentity& target) noexcept {
    return {target.window, target.pid, target.process_created};
}

IdentityFreshness compare_identity(const TargetIdentity& snapshot,
    const TargetIdentity& current) noexcept {
    if (!snapshot.has_runtime_identity() || !current.has_runtime_identity())
        return IdentityFreshness::incomplete;
    return snapshot.same_runtime(current) ? IdentityFreshness::current : IdentityFreshness::expired;
}

static bool has_nul(const std::wstring& value) noexcept {
    return value.find(L'\0') != std::wstring::npos;
}

bool valid_match_rule(const MatchRule& rule) noexcept {
    return !rule.path.empty() && rule.path.size() <= 32768 && !has_nul(rule.path)
        && rule.class_name.size() <= 1024 && !has_nul(rule.class_name)
        && rule.title.size() <= 32768 && !has_nul(rule.title);
}

static bool equal_path(const std::wstring& left, const std::wstring& right, bool insensitive) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        wchar_t a = left[i] == L'/' ? L'\\' : left[i];
        wchar_t b = right[i] == L'/' ? L'\\' : right[i];
        if (insensitive) {
            a = static_cast<wchar_t>(std::towlower(a));
            b = static_cast<wchar_t>(std::towlower(b));
        }
        if (a != b) return false;
    }
    return true;
}

bool rule_matches(const MatchRule& rule, const TargetIdentity& target) noexcept {
    if (!rule.enabled || !valid_match_rule(rule)) return false;
    if (target.path.empty()) return false;
    if (!equal_path(rule.path, target.path, rule.path_case_insensitive)) return false;
    if (!rule.class_name.empty() && rule.class_name != target.class_name) return false;
    if (!rule.title.empty() && rule.title != target.title) return false;
    return true;
}

std::vector<std::size_t> matching_indices(const MatchRule& rule,
    const std::vector<TargetIdentity>& targets) {
    std::vector<std::size_t> result;
    if (!rule.enabled || !valid_match_rule(rule)) return result;
    for (std::size_t i = 0; i < targets.size(); ++i)
        if (rule_matches(rule, targets[i])) result.push_back(i);
    return result;
}

RuleMatch match_rule(const MatchRule& rule,
    const std::vector<TargetIdentity>& targets) {
    RuleMatch result;
    if (!valid_match_rule(rule)) {
        result.status = RuleMatchStatus::invalid;
        return result;
    }
    const auto indices = matching_indices(rule, targets);
    result.count = indices.size();
    if (indices.empty()) result.status = RuleMatchStatus::none;
    else if (indices.size() == 1) {
        result.status = RuleMatchStatus::matched;
        result.index = indices.front();
    } else {
        result.status = RuleMatchStatus::ambiguous;
        result.index = indices.front();
    }
    return result;
}

}

