// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/identity.h"
#include "core/layout_policy.h"
#include "platform/windows/profile_v4.h"
#include <functional>
#include <vector>

namespace av {

enum class LayoutApplyStatus : unsigned {
    applied = 0,
    adjusted = 1,
    stale = 2,
    invalid = 3,
    failed = 4
};

struct LayoutApplyOutcome {
    LayoutApplyStatus status = LayoutApplyStatus::failed;
    DWORD error = ERROR_INVALID_DATA;
    LayoutPlan plan;
    LayoutRect original;
    LayoutRect applied;
    int original_show = 0;
    int applied_show = 0;
};

// Injection seam: production uses windows_layout_ops(); tests use a fake
// placement store so no visible or hidden window is created.
struct LayoutWindowOps {
    std::function<bool(const TargetIdentity&)> identity_current;
    std::function<bool(HWND, WINDOWPLACEMENT&)> get;
    std::function<bool(HWND, const WINDOWPLACEMENT&)> set;
};

LayoutWindowOps windows_layout_ops();
std::vector<LayoutMonitor> enumerate_layout_monitors();

// Re-locates an already resolved, current target.  The persisted rule contains
// no HWND/PID; identity_current() rejects a reused handle before any write.
LayoutApplyOutcome restore_layout(const TargetIdentity& target, const RuleV4& rule,
    const std::vector<LayoutMonitor>& monitors,
    const LayoutWindowOps& ops = windows_layout_ops());

}
