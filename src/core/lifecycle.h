// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/identity.h"
#include <cstdint>
#include <set>
#include <vector>

namespace av {

enum class ResourceKind : unsigned {
    affinity = 0,
    taskbar_style = 1,
    overlay_layer = 2,
    hook_module = 3,
    capture_pipeline = 4
};

enum class ResourceState : unsigned {
    active = 0,
    restored = 1,
    lost = 2,
    restore_failed = 3
};

struct SessionResource {
    std::uint64_t id = 0;
    ResourceKind kind = ResourceKind::affinity;
    TargetIdentity target;
    std::uint64_t original_value = 0;
    std::uint64_t applied_value = 0;
    ResourceState state = ResourceState::active;
    unsigned attempts = 0;
};

class SessionLedger {
public:
    explicit SessionLedger(std::uint64_t session_id) noexcept;

    std::uint64_t session_id() const noexcept { return session_id_; }
    std::uint64_t add(ResourceKind kind, const TargetIdentity& target,
        std::uint64_t original_value, std::uint64_t applied_value);
    bool owned(std::uint64_t id) const noexcept;
    bool active(std::uint64_t id) const noexcept;
    bool mark_restored(std::uint64_t id);
    bool mark_lost(std::uint64_t id);
    bool mark_restore_failed(std::uint64_t id);
    void expire_target(const TargetIdentity& target);
    bool target_expired(const TargetIdentity& target) const;
    std::vector<SessionResource> active_records() const;
    std::vector<SessionResource> restore_plan() const;
    std::vector<SessionResource> lost_records() const;
    std::size_t active_count() const noexcept;
    std::size_t size() const noexcept { return resources_.size(); }

private:
    SessionResource* find(std::uint64_t id) noexcept;
    const SessionResource* find(std::uint64_t id) const noexcept;

    std::uint64_t session_id_ = 0;
    std::uint64_t next_id_ = 1;
    std::vector<SessionResource> resources_;
    std::set<TargetKey> expired_;
};

bool valid_resource_transition(ResourceState from, ResourceState to) noexcept;

}

