// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/rule_resolver.h"

namespace av {

std::vector<RuleResolution> resolve_rules(const ProfileV4& profile,
    const std::vector<TargetIdentity>& targets) {
    std::vector<RuleResolution> results;
    results.reserve(profile.rules.size());
    for (std::size_t i = 0; i < profile.rules.size(); ++i) {
        const auto& rule = profile.rules[i];
        RuleResolution resolution;
        resolution.rule = i;
        if (!rule.enabled) {
            resolution.state = RuleResolutionState::disabled;
            results.push_back(std::move(resolution));
            continue;
        }
        MatchRule match = rule.to_match_rule();
        match.enabled = true;
        if (!valid_match_rule(match)) {
            resolution.state = RuleResolutionState::invalid;
            resolution.error = ERROR_INVALID_DATA;
            results.push_back(std::move(resolution));
            continue;
        }
        resolution.matches = matching_indices(match, targets);
        resolution.count = resolution.matches.size();
        if (resolution.matches.empty())
            resolution.state = RuleResolutionState::none;
        else if (resolution.matches.size() == 1)
            resolution.state = RuleResolutionState::matched;
        else
            resolution.state = RuleResolutionState::ambiguous;
        results.push_back(std::move(resolution));
    }
    return results;
}

bool resolution_applicable(const RuleResolution& resolution) noexcept {
    return resolution.state == RuleResolutionState::matched
        && resolution.matches.size() == 1;
}

}
