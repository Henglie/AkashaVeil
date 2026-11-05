// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/cleanup.h"
#include <algorithm>

namespace av {

void CleanupChain::add(const wchar_t* name, Step step, void* context) {
    entries_.push_back({name ? name : L"", step, context});
}

bool CleanupChain::run() {
    if (ran_) return false;
    ran_ = true;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        order_.push_back(it->name);
        if (it->step) it->step(it->context);
    }
    return true;
}

void SessionLedger::record_affinity(HWND hwnd, DWORD original, DWORD applied) {
    affinities_.push_back({hwnd, original, applied});
}

void SessionLedger::record_taskbar_style(HWND hwnd, LONG_PTR original, LONG_PTR applied, LONG_PTR mask) {
    taskbars_.push_back({hwnd, original, applied, mask});
}

void SessionLedger::exit() {
    actions_.clear();
    for (const auto& record : taskbars_) {
        if (!record.hwnd || !IsWindow(record.hwnd)) {
            actions_.push_back(L"taskbar:missing");
            continue;
        }
        const auto current = GetWindowLongPtrW(record.hwnd, GWL_EXSTYLE);
        if ((current & record.mask) != (record.applied & record.mask)) {
            actions_.push_back(L"taskbar:user-changed");
            continue;
        }
        const auto restored = (current & ~record.mask) | (record.original & record.mask);
        if (restored != current) {
            SetLastError(0);
            SetWindowLongPtrW(record.hwnd, GWL_EXSTYLE, restored);
            SetWindowPos(record.hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            const auto verified = GetWindowLongPtrW(record.hwnd, GWL_EXSTYLE);
            if ((verified & record.mask) == (record.original & record.mask)) {
                actions_.push_back(L"taskbar:restored");
            } else {
                actions_.push_back(L"taskbar:failed");
            }
        } else {
            actions_.push_back(L"taskbar:unchanged");
        }
    }
    if (policy_ == ExitPolicy::restore_affinity) {
        for (const auto& record : affinities_) {
            if (!record.hwnd || !IsWindow(record.hwnd)) {
                actions_.push_back(L"affinity:missing");
                continue;
            }
            DWORD current = 0;
            if (!GetWindowDisplayAffinity(record.hwnd, &current) || current != record.applied) {
                actions_.push_back(L"affinity:user-changed");
                continue;
            }
            if (SetWindowDisplayAffinity(record.hwnd, record.original)) {
                actions_.push_back(L"affinity:restored");
            } else {
                actions_.push_back(L"affinity:failed");
            }
        }
    } else {
        for (const auto& record : affinities_) {
            (void)record;
        }
        if (!affinities_.empty()) actions_.push_back(L"affinity:preserved");
    }
    affinities_.clear();
    taskbars_.clear();
}

} // namespace av
