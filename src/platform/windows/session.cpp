// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/session.h"
#include <winternl.h>

namespace av {

namespace {

DWORD windows_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    if (!module) return 0;
    const auto function = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(module, "RtlGetVersion"));
    if (!function) return 0;
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return function(&info) == 0 ? info.dwBuildNumber : 0;
}

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

void fail_remaining(ProtectionObserved& observed, StageMask stages,
    StageState state, ErrorDomain domain, std::uint32_t code) {
    for (std::size_t i = 0; i < stage_count; ++i) {
        const auto stage = static_cast<Stage>(i);
        if (!(stages & stage_bit(stage))) continue;
        if (observed.stages[i].state != StageState::not_requested) continue;
        set_stage(observed, stage, state, {domain, code});
    }
}

}

WindowsSessionBackend::WindowsSessionBackend(SessionLedger& ledger)
    : ledger_(ledger) {}

BackendCapabilities WindowsSessionBackend::capabilities() const {
    BackendCapabilities capabilities;
    capabilities.affinity_basic = true;
    capabilities.affinity_exclude = windows_build() >= 19041;
    capabilities.visible_screen = false;
    capabilities.visible_capture_only_original_preserved = false;
    capabilities.replacement_solid = false;
    capabilities.replacement_label = false;
    capabilities.taskbar_target = false;
    capabilities.capture_route_automatic = false;
    return capabilities;
}

bool WindowsSessionBackend::identity_current(const TargetIdentity& target) {
    if (!target.has_runtime_identity()) return false;
    return same_window(legacy_window(target));
}

ProtectionObserved WindowsSessionBackend::apply(const TargetIdentity& target,
    const ProtectionRequest& request, StageMask stages,
    const CancellationToken& cancel) {
    ProtectionObserved observed = make_observed(target, request);
    if (cancel.cancelled()) {
        fail_remaining(observed, stages, StageState::cancelled, ErrorDomain::cancelled, 1);
        return observed;
    }
    if (!identity_current(target)) {
        observed.stale = true;
        fail_remaining(observed, stages, StageState::failed, ErrorDomain::stale_identity, 1);
        return observed;
    }
    const auto plan = plan_protection(request, capabilities());
    for (std::size_t i = 0; i < stage_count; ++i) {
        const auto stage = static_cast<Stage>(i);
        if (!(stages & stage_bit(stage))) continue;
        if (cancel.cancelled()) {
            set_stage(observed, stage, StageState::cancelled, {ErrorDomain::cancelled, 1});
            continue;
        }
        if (plan.has_issue(stage)) {
            set_stage(observed, stage, StageState::unsupported, {ErrorDomain::unsupported, 1});
            continue;
        }
        if (stage != Stage::affinity) {
            set_stage(observed, stage, StageState::unsupported, {ErrorDomain::unsupported, 2});
            continue;
        }
        if (plan.affinity == AffinityMode::none) {
            set_stage(observed, stage, StageState::skipped);
            continue;
        }
        const auto hwnd = reinterpret_cast<HWND>(target.window);
        DWORD original = 0;
        if (!GetWindowDisplayAffinity(hwnd, &original)) {
            set_stage(observed, stage, StageState::failed, {ErrorDomain::win32, GetLastError()});
            continue;
        }
        if (!SetWindowDisplayAffinity(hwnd, static_cast<DWORD>(plan.affinity))) {
            set_stage(observed, stage, StageState::failed, {ErrorDomain::win32, GetLastError()});
            continue;
        }
        DWORD actual = 0;
        observed.affinity_known = GetWindowDisplayAffinity(hwnd, &actual) != FALSE;
        if (observed.affinity_known && valid_affinity(actual))
            observed.actual_affinity = static_cast<AffinityMode>(actual);
        const bool requested_actual = observed.affinity_known
            && actual == static_cast<DWORD>(plan.affinity);
        ledger_.add(ResourceKind::affinity, target, original,
            observed.affinity_known ? actual : static_cast<DWORD>(plan.affinity));
        if (!identity_current(target)) {
            observed.stale = true;
            set_stage(observed, stage, StageState::failed, {ErrorDomain::stale_identity, 2});
        } else if (!requested_actual) {
            set_stage(observed, stage, StageState::failed, {ErrorDomain::win32, ERROR_INVALID_STATE});
        } else {
            set_stage(observed, stage, StageState::succeeded);
        }
    }
    return observed;
}

ProtectionObserved WindowsSessionBackend::restore(const TargetIdentity& target,
    const ProtectionRequest& request, StageMask stages,
    const CancellationToken& cancel) {
    ProtectionObserved observed = make_observed(target, request);
    if (cancel.cancelled()) {
        fail_remaining(observed, stages, StageState::cancelled, ErrorDomain::cancelled, 1);
        return observed;
    }
    if (!identity_current(target)) {
        observed.stale = true;
        ledger_.expire_target(target);
        fail_remaining(observed, stages, StageState::failed, ErrorDomain::stale_identity, 1);
        return observed;
    }
    // Restore newest records first.  The final write is the oldest captured
    // original value, which is the only value safe to leave behind when the
    // same target was protected more than once in this session.
    bool restored_any = false;
    bool restore_failed = false;
    Error first_error;
    for (const auto& resource : ledger_.restore_plan()) {
        if (!resource.target.same_runtime(target)) continue;
        if (resource.kind != ResourceKind::affinity) {
            if (stages & stage_bit(Stage::taskbar))
                set_stage(observed, Stage::taskbar, StageState::unsupported,
                    {ErrorDomain::unsupported, 3});
            continue;
        }
        const auto hwnd = reinterpret_cast<HWND>(target.window);
        if (!SetWindowDisplayAffinity(hwnd, static_cast<DWORD>(resource.original_value))) {
            ledger_.mark_restore_failed(resource.id);
            if (!restore_failed) first_error = {ErrorDomain::win32, GetLastError()};
            restore_failed = true;
            continue;
        }
        DWORD actual = 0;
        observed.affinity_known = GetWindowDisplayAffinity(hwnd, &actual) != FALSE;
        if (observed.affinity_known && valid_affinity(actual))
            observed.actual_affinity = static_cast<AffinityMode>(actual);
        if (observed.affinity_known
            && actual != static_cast<DWORD>(resource.original_value)) {
            ledger_.mark_restore_failed(resource.id);
            if (!restore_failed) first_error = {ErrorDomain::win32, ERROR_INVALID_STATE};
            restore_failed = true;
            continue;
        }
        ledger_.mark_restored(resource.id);
        restored_any = true;
    }
    if (stages & stage_bit(Stage::affinity)) {
        if (restore_failed)
            set_stage(observed, Stage::affinity, StageState::failed, first_error);
        else if (restored_any)
            set_stage(observed, Stage::affinity, StageState::succeeded);
    }
    for (std::size_t i = 0; i < stage_count; ++i) {
        const auto stage = static_cast<Stage>(i);
        if (!(stages & stage_bit(stage))) continue;
        if (observed.stages[i].state == StageState::not_requested)
            set_stage(observed, stage, StageState::skipped);
    }
    return observed;
}

std::vector<TargetIdentity> WindowsSessionBackend::enumerate(const CancellationToken& cancel) {
    std::vector<TargetIdentity> result;
    for (const auto& window : av::enumerate_windows(true)) {
        if (cancel.cancelled()) break;
        TargetIdentity target;
        target.window = window.handle;
        target.pid = window.pid;
        target.thread = window.tid;
        target.process_created = window.process_created;
        target.title = window.title;
        target.class_name = window.class_name;
        target.path = window.path;
        target.architecture = window.architecture;
        result.push_back(std::move(target));
    }
    return result;
}

bool WindowsSessionBackend::restore_ledger(DWORD& error) {
    bool success = true;
    error = ERROR_SUCCESS;
    for (const auto& resource : ledger_.restore_plan()) {
        if (resource.kind != ResourceKind::affinity) {
            success = false;
            if (!error) error = ERROR_NOT_SUPPORTED;
            continue;
        }
        if (!identity_current(resource.target)) {
            ledger_.expire_target(resource.target);
            success = false;
            if (!error) error = ERROR_INVALID_WINDOW_HANDLE;
            continue;
        }
        const auto hwnd = reinterpret_cast<HWND>(resource.target.window);
        if (!SetWindowDisplayAffinity(hwnd, static_cast<DWORD>(resource.original_value))) {
            ledger_.mark_restore_failed(resource.id);
            success = false;
            if (!error) error = GetLastError();
            continue;
        }
        DWORD actual = 0;
        if (!GetWindowDisplayAffinity(hwnd, &actual)
            || actual != static_cast<DWORD>(resource.original_value)) {
            ledger_.mark_restore_failed(resource.id);
            success = false;
            if (!error) error = ERROR_INVALID_STATE;
        } else {
            ledger_.mark_restored(resource.id);
        }
    }
    return success;
}

}

