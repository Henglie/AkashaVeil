// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/profile_edit.h"
#include <cwctype>
#include <utility>

namespace av {

namespace {

constexpr std::size_t max_rules = 512;

std::wstring canonical_path(const std::wstring& value) {
    std::wstring out = value;
    for (auto& c : out) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return out;
}

bool same_rule_key(const RuleV4& left, const RuleV4& right) noexcept {
    return canonical_path(left.path) == canonical_path(right.path)
        && left.class_name == right.class_name
        && left.title == right.title;
}

RuleEditResult result(RuleEditStatus status, std::size_t index, DWORD error) {
    RuleEditResult value;
    value.status = status;
    value.index = index;
    value.error = error;
    return value;
}

}

bool valid_rule_v4(const RuleV4& rule, DWORD& error) noexcept {
    ProfileV4 probe;
    probe.rules.push_back(rule);
    return valid_profile_v4(probe, error);
}

std::size_t find_rule_index(const ProfileV4& profile, const RuleV4& rule) noexcept {
    for (std::size_t i = 0; i < profile.rules.size(); ++i)
        if (same_rule_key(profile.rules[i], rule)) return i;
    return static_cast<std::size_t>(-1);
}

RuleEditResult add_rule(ProfileV4& profile, const RuleV4& rule) {
    DWORD error = ERROR_SUCCESS;
    if (!valid_rule_v4(rule, error))
        return result(RuleEditStatus::invalid, 0, error);
    if (profile.rules.size() >= max_rules)
        return result(RuleEditStatus::limit, 0, ERROR_TOO_MANY_NAMES);
    const auto existing = find_rule_index(profile, rule);
    if (existing != static_cast<std::size_t>(-1))
        return result(RuleEditStatus::duplicate, existing, ERROR_ALREADY_EXISTS);
    profile.rules.push_back(rule);
    return result(RuleEditStatus::ok, profile.rules.size() - 1, ERROR_SUCCESS);
}

RuleEditResult upsert_rule(ProfileV4& profile, const RuleV4& rule) {
    DWORD error = ERROR_SUCCESS;
    if (!valid_rule_v4(rule, error))
        return result(RuleEditStatus::invalid, 0, error);
    const auto existing = find_rule_index(profile, rule);
    if (existing != static_cast<std::size_t>(-1)) {
        profile.rules[existing] = rule;
        return result(RuleEditStatus::ok, existing, ERROR_SUCCESS);
    }
    if (profile.rules.size() >= max_rules)
        return result(RuleEditStatus::limit, 0, ERROR_TOO_MANY_NAMES);
    profile.rules.push_back(rule);
    return result(RuleEditStatus::ok, profile.rules.size() - 1, ERROR_SUCCESS);
}

RuleEditResult update_rule(ProfileV4& profile, std::size_t index, const RuleV4& rule) {
    if (index >= profile.rules.size())
        return result(RuleEditStatus::not_found, index, ERROR_NOT_FOUND);
    DWORD error = ERROR_SUCCESS;
    if (!valid_rule_v4(rule, error))
        return result(RuleEditStatus::invalid, index, error);
    const auto existing = find_rule_index(profile, rule);
    if (existing != static_cast<std::size_t>(-1) && existing != index)
        return result(RuleEditStatus::duplicate, existing, ERROR_ALREADY_EXISTS);
    profile.rules[index] = rule;
    return result(RuleEditStatus::ok, index, ERROR_SUCCESS);
}

RuleEditResult remove_rule(ProfileV4& profile, std::size_t index) {
    if (index >= profile.rules.size())
        return result(RuleEditStatus::not_found, index, ERROR_NOT_FOUND);
    profile.rules.erase(profile.rules.begin() + static_cast<std::ptrdiff_t>(index));
    return result(RuleEditStatus::ok, index, ERROR_SUCCESS);
}

RuleEditResult set_rule_enabled(ProfileV4& profile, std::size_t index, bool enabled) {
    if (index >= profile.rules.size())
        return result(RuleEditStatus::not_found, index, ERROR_NOT_FOUND);
    profile.rules[index].enabled = enabled;
    return result(RuleEditStatus::ok, index, ERROR_SUCCESS);
}

RuleV4 rule_from_target(const TargetIdentity& target, const RuleGeometry& geometry,
    const ProtectionRequest& request) {
    RuleV4 rule;
    rule.path = target.path;
    rule.class_name = target.class_name;
    rule.title = target.title;
    rule.x = geometry.x;
    rule.y = geometry.y;
    rule.width = geometry.width;
    rule.height = geometry.height;
    rule.show = geometry.show;
    rule.enabled = true;
    rule.request = request;
    return rule;
}

}
