// SPDX-License-Identifier: Apache-2.0
#include "core/lifecycle.h"
#include <algorithm>

namespace av {

SessionLedger::SessionLedger(std::uint64_t session_id) noexcept
    : session_id_(session_id) {}

SessionResource* SessionLedger::find(std::uint64_t id) noexcept {
    const auto found = std::find_if(resources_.begin(), resources_.end(),
        [id](const SessionResource& resource) { return resource.id == id; });
    return found == resources_.end() ? nullptr : &*found;
}

const SessionResource* SessionLedger::find(std::uint64_t id) const noexcept {
    const auto found = std::find_if(resources_.begin(), resources_.end(),
        [id](const SessionResource& resource) { return resource.id == id; });
    return found == resources_.end() ? nullptr : &*found;
}

std::uint64_t SessionLedger::add(ResourceKind kind, const TargetIdentity& target,
    std::uint64_t original_value, std::uint64_t applied_value) {
    if (target_expired(target)) return 0;
    SessionResource resource;
    resource.id = next_id_++;
    resource.kind = kind;
    resource.target = target;
    resource.original_value = original_value;
    resource.applied_value = applied_value;
    resources_.push_back(std::move(resource));
    return resources_.back().id;
}

bool SessionLedger::owned(std::uint64_t id) const noexcept {
    return find(id) != nullptr;
}

bool SessionLedger::active(std::uint64_t id) const noexcept {
    const auto* resource = find(id);
    return resource && resource->state == ResourceState::active;
}

bool SessionLedger::mark_restored(std::uint64_t id) {
    auto* resource = find(id);
    if (!resource || (resource->state != ResourceState::active
        && resource->state != ResourceState::restore_failed)) return false;
    resource->state = ResourceState::restored;
    return true;
}

bool SessionLedger::mark_lost(std::uint64_t id) {
    auto* resource = find(id);
    if (!resource || (resource->state != ResourceState::active
        && resource->state != ResourceState::restore_failed)) return false;
    resource->state = ResourceState::lost;
    return true;
}

bool SessionLedger::mark_restore_failed(std::uint64_t id) {
    auto* resource = find(id);
    if (!resource || (resource->state != ResourceState::active
        && resource->state != ResourceState::restore_failed)) return false;
    resource->state = ResourceState::restore_failed;
    ++resource->attempts;
    return true;
}

void SessionLedger::expire_target(const TargetIdentity& target) {
    expired_.insert(target_key(target));
    for (auto& resource : resources_) {
        if ((resource.state == ResourceState::active
            || resource.state == ResourceState::restore_failed)
            && resource.target.same_runtime(target))
            resource.state = ResourceState::lost;
    }
}

bool SessionLedger::target_expired(const TargetIdentity& target) const {
    return expired_.count(target_key(target)) != 0;
}

std::vector<SessionResource> SessionLedger::active_records() const {
    std::vector<SessionResource> result;
    for (const auto& resource : resources_)
        if (resource.state == ResourceState::active) result.push_back(resource);
    return result;
}

std::vector<SessionResource> SessionLedger::restore_plan() const {
    std::vector<SessionResource> result;
    for (const auto& resource : resources_) {
        if (resource.state != ResourceState::active
            && resource.state != ResourceState::restore_failed) continue;
        if (target_expired(resource.target)) continue;
        result.push_back(resource);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<SessionResource> SessionLedger::lost_records() const {
    std::vector<SessionResource> result;
    for (const auto& resource : resources_)
        if (resource.state == ResourceState::lost) result.push_back(resource);
    return result;
}

std::size_t SessionLedger::active_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(resources_.begin(), resources_.end(),
        [](const SessionResource& resource) { return resource.state == ResourceState::active; }));
}

bool valid_resource_transition(ResourceState from, ResourceState to) noexcept {
    if (from == ResourceState::active)
        return to == ResourceState::restored || to == ResourceState::lost
            || to == ResourceState::restore_failed;
    if (from == ResourceState::restore_failed)
        return to == ResourceState::restored || to == ResourceState::lost;
    return false;
}

}

