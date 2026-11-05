// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/layout_restore.h"
#include "platform/windows/profile_edit.h"
#include "platform/windows/windows.h"
#include <algorithm>

namespace av {

namespace {

Window legacy_window(const TargetIdentity& target) {
    Window window;
    window.handle = target.window;
    window.pid = target.pid;
    window.tid = target.thread;
    window.process_created = target.process_created;
    window.title = target.title;
    window.class_name = target.class_name;
    window.path = target.path;
    window.architecture = target.architecture;
    return window;
}

LayoutRect rect_from(const RECT& rect) {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

RECT rect_to(const LayoutRect& rect) {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

bool same_rect(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top
        && left.right == right.right && left.bottom == right.bottom;
}

unsigned monitor_dpi(HMONITOR monitor) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto function = []() -> GetDpiForMonitorFn {
        const HMODULE module = LoadLibraryW(L"shcore.dll");
        if (!module) return nullptr;
        return reinterpret_cast<GetDpiForMonitorFn>(
            GetProcAddress(module, "GetDpiForMonitor"));
    }();
    if (!function) return 96;
    UINT dpi_x = 96, dpi_y = 96;
    constexpr int effective_dpi = 0;
    if (SUCCEEDED(function(monitor, effective_dpi, &dpi_x, &dpi_y)) && dpi_x)
        return dpi_x;
    return 96;
}

}

LayoutWindowOps windows_layout_ops() {
    LayoutWindowOps ops;
    ops.identity_current = [](const TargetIdentity& target) {
        return target.has_runtime_identity() && same_window(legacy_window(target));
    };
    ops.get = [](HWND window, WINDOWPLACEMENT& placement) {
        placement.length = sizeof(placement);
        return GetWindowPlacement(window, &placement) != FALSE;
    };
    ops.set = [](HWND window, const WINDOWPLACEMENT& placement) {
        return SetWindowPlacement(window, &placement) != FALSE;
    };
    return ops;
}

std::vector<LayoutMonitor> enumerate_layout_monitors() {
    std::vector<LayoutMonitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto* out = reinterpret_cast<std::vector<LayoutMonitor>*>(data);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) return TRUE;
            LayoutMonitor value;
            value.work = rect_from(info.rcWork);
            value.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            value.dpi = monitor_dpi(monitor);
            out->push_back(value);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

LayoutApplyOutcome restore_layout(const TargetIdentity& target, const RuleV4& rule,
    const std::vector<LayoutMonitor>& monitors, const LayoutWindowOps& ops) {
    LayoutApplyOutcome outcome;
    DWORD error = ERROR_SUCCESS;
    if (!valid_rule_v4(rule, error)) {
        outcome.status = LayoutApplyStatus::invalid;
        outcome.error = error;
        return outcome;
    }
    outcome.plan = plan_layout(rule.x, rule.y, rule.width, rule.height,
        rule.show, monitors);
    if (outcome.plan.action == LayoutAction::invalid
        || !outcome.plan.reachable) {
        outcome.status = LayoutApplyStatus::invalid;
        outcome.error = ERROR_INVALID_DATA;
        return outcome;
    }

    auto identity = ops.identity_current;
    auto get = ops.get;
    auto set = ops.set;
    if (!identity || !get || !set) {
        const auto fallback = windows_layout_ops();
        if (!identity) identity = fallback.identity_current;
        if (!get) get = fallback.get;
        if (!set) set = fallback.set;
    }
    if (!identity(target)) {
        outcome.status = LayoutApplyStatus::stale;
        outcome.error = ERROR_INVALID_WINDOW_HANDLE;
        return outcome;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!get(reinterpret_cast<HWND>(target.window), placement)) {
        outcome.status = LayoutApplyStatus::failed;
        outcome.error = GetLastError();
        if (!outcome.error) outcome.error = ERROR_INVALID_WINDOW_HANDLE;
        return outcome;
    }
    outcome.original = rect_from(placement.rcNormalPosition);
    outcome.original_show = static_cast<int>(placement.showCmd);

    placement.rcNormalPosition = rect_to(outcome.plan.normal);
    placement.showCmd = static_cast<UINT>(outcome.plan.show);
    if (!set(reinterpret_cast<HWND>(target.window), placement)) {
        outcome.status = LayoutApplyStatus::failed;
        outcome.error = GetLastError();
        if (!outcome.error) outcome.error = ERROR_INVALID_WINDOW_HANDLE;
        return outcome;
    }
    if (!identity(target)) {
        outcome.status = LayoutApplyStatus::stale;
        outcome.error = ERROR_INVALID_WINDOW_HANDLE;
        return outcome;
    }

    WINDOWPLACEMENT applied{};
    applied.length = sizeof(applied);
    if (!get(reinterpret_cast<HWND>(target.window), applied)) {
        outcome.status = LayoutApplyStatus::failed;
        outcome.error = GetLastError();
        if (!outcome.error) outcome.error = ERROR_INVALID_WINDOW_HANDLE;
        return outcome;
    }
    outcome.applied = rect_from(applied.rcNormalPosition);
    outcome.applied_show = static_cast<int>(applied.showCmd);
    if (!same_rect(applied.rcNormalPosition,
        rect_to(outcome.plan.normal))) {
        outcome.status = LayoutApplyStatus::failed;
        outcome.error = ERROR_INVALID_STATE;
        return outcome;
    }
    if (!layout_rect_reachable(outcome.applied, monitors)) {
        outcome.status = LayoutApplyStatus::failed;
        outcome.error = ERROR_INVALID_STATE;
        return outcome;
    }

    const bool adjusted = outcome.plan.action != LayoutAction::unchanged
        || outcome.applied_show != outcome.plan.show;
    outcome.status = adjusted ? LayoutApplyStatus::adjusted : LayoutApplyStatus::applied;
    outcome.error = ERROR_SUCCESS;
    return outcome;
}

}
