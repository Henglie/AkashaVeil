// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/taskbar.h"
#include <algorithm>
#include <atomic>
#include <utility>

namespace av {
namespace {

constexpr LONG_PTR kTaskbarBits = WS_EX_TOOLWINDOW | WS_EX_APPWINDOW;

bool same_identity(const Window& a, const Window& b) {
    return a.handle == b.handle && a.pid == b.pid && a.tid == b.tid
        && a.process_created != 0 && a.process_created == b.process_created;
}

std::uintptr_t make_token() {
    static std::atomic<std::uintptr_t> counter{1};
    const std::uintptr_t value = static_cast<std::uintptr_t>(GetCurrentProcessId()) * 2654435761u
        + counter.fetch_add(1, std::memory_order_relaxed);
    return value ? value : 1;
}

bool read_style(HWND hwnd, LONG_PTR& value, DWORD& error) {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR result = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const DWORD failure = GetLastError();
    if (!result && failure) { error = failure; return false; }
    value = result;
    error = ERROR_SUCCESS;
    return true;
}

// Styles are authoritative.  SWP_FRAMECHANGED asks the shell to refresh any
// taskbar state, but a failure to send it is not treated as a style failure.
bool write_style(HWND hwnd, LONG_PTR desired, LONG_PTR owned, LONG_PTR& actual, DWORD& error) {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, desired);
    const DWORD failure = GetLastError();
    if (!previous && failure) { error = failure; return false; }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR current = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const DWORD readback = GetLastError();
    if (!current && readback) { error = readback; return false; }
    actual = current;
    if ((current & owned) != (desired & owned)) {
        // UIPI may block SetWindowLongPtr and return FALSE/zero with no last
        // error; report a permission failure instead of a generic state error.
        error = failure ? failure : ERROR_ACCESS_DENIED;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

DWORD place_marker(HWND hwnd, std::uintptr_t token) {
    SetLastError(ERROR_SUCCESS);
    if (SetPropW(hwnd, kTaskbarMarker, reinterpret_cast<HANDLE>(token))) return ERROR_SUCCESS;
    const DWORD failure = GetLastError();
    return failure ? failure : ERROR_ACCESS_DENIED;
}

DWORD clear_marker(HWND hwnd, std::uintptr_t token) {
    if (reinterpret_cast<std::uintptr_t>(GetPropW(hwnd, kTaskbarMarker)) != token)
        return ERROR_INVALID_STATE;
    SetLastError(ERROR_SUCCESS);
    if (RemovePropW(hwnd, kTaskbarMarker)) return ERROR_SUCCESS;
    const DWORD failure = GetLastError();
    return failure ? failure : ERROR_INVALID_STATE;
}

bool invalid_identity_error(DWORD error) {
    return error == ERROR_INVALID_WINDOW_HANDLE || error == ERROR_INVALID_PARAMETER;
}

void record_batch_error(TaskbarBatchResult& batch, std::uintptr_t handle, DWORD error) {
    if (batch.first_error == ERROR_SUCCESS) batch.first_error = error;
    batch.errors.emplace_back(handle, error);
    if (invalid_identity_error(error)) ++batch.invalid;
    else ++batch.failed;
}

} // namespace

TaskbarLedger::TaskbarLedger() = default;
TaskbarLedger::TaskbarLedger(IdentityCheck check) : check_(check) {}
TaskbarLedger::~TaskbarLedger() { restore_all(); }

DWORD TaskbarLedger::identity_error(const Window& target) const {
    if (!target.handle || !target.pid || !target.tid || !target.process_created)
        return ERROR_INVALID_PARAMETER;
    const HWND hwnd = reinterpret_cast<HWND>(target.handle);
    if (!IsWindow(hwnd)) return ERROR_INVALID_WINDOW_HANDLE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return ERROR_INVALID_PARAMETER;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || pid != target.pid || tid != target.tid) return ERROR_INVALID_WINDOW_HANDLE;
    if (check_) return check_(target) ? ERROR_SUCCESS : ERROR_INVALID_WINDOW_HANDLE;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        const DWORD failure = GetLastError();
        return failure == ERROR_ACCESS_DENIED ? ERROR_ACCESS_DENIED : ERROR_INVALID_WINDOW_HANDLE;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    if (!ok) return ERROR_INVALID_WINDOW_HANDLE;
    const std::uint64_t value = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return value == target.process_created ? ERROR_SUCCESS : ERROR_INVALID_WINDOW_HANDLE;
}

TaskbarResult TaskbarLedger::hide(const Window& target) {
    TaskbarResult result;
    result.tracked = entries_.size();
    const DWORD identity = identity_error(target);
    if (identity) {
        result.error = identity;
        result.state = TaskbarState::lost;
        return result;
    }
    const auto found = entries_.find(target.handle);
    if (found == entries_.end()) return hide_new(target);
    if (!same_identity(found->second.identity, target)) {
        result.error = ERROR_INVALID_WINDOW_HANDLE;
        result.state = TaskbarState::lost;
        return result;
    }
    return hide_tracked(target, found->second);
}

TaskbarResult TaskbarLedger::hide_new(const Window& target) {
    TaskbarResult result;
    result.tracked = entries_.size();
    const HWND hwnd = reinterpret_cast<HWND>(target.handle);
    LONG_PTR original = 0;
    DWORD error = ERROR_SUCCESS;
    if (!read_style(hwnd, original, error)) {
        result.error = error;
        result.state = TaskbarState::lost;
        return result;
    }
    const LONG_PTR desired = (original & ~kTaskbarBits) | WS_EX_TOOLWINDOW;
    const LONG_PTR owned = (desired ^ original) & kTaskbarBits;
    if (!owned) {
        result.state = TaskbarState::foreign;
        return result;
    }
    if (GetPropW(hwnd, kTaskbarMarker)) {
        result.error = ERROR_ALREADY_EXISTS;
        result.state = TaskbarState::conflict;
        return result;
    }
    const std::uintptr_t token = make_token();
    error = place_marker(hwnd, token);
    if (error) {
        result.error = error;
        result.state = TaskbarState::lost;
        return result;
    }
    LONG_PTR actual = 0;
    if (!write_style(hwnd, desired, owned, actual, error)) {
        clear_marker(hwnd, token);
        result.error = error;
        result.state = TaskbarState::lost;
        return result;
    }
    Entry entry;
    entry.identity = target;
    entry.original_exstyle = original;
    entry.applied_exstyle = actual;
    entry.owned_mask = owned;
    entry.token = token;
    entries_[target.handle] = std::move(entry);
    result.state = TaskbarState::hidden;
    result.changed = true;
    result.changed_bits = owned;
    result.tracked = entries_.size();
    if ((actual & owned) != (desired & owned)) {
        result.error = ERROR_INVALID_STATE;
    }
    return result;
}

TaskbarResult TaskbarLedger::hide_tracked(const Window& target, Entry& entry) {
    TaskbarResult result;
    result.tracked = entries_.size();
    const HWND hwnd = reinterpret_cast<HWND>(target.handle);
    if (reinterpret_cast<std::uintptr_t>(GetPropW(hwnd, kTaskbarMarker)) != entry.token) {
        result.error = ERROR_INVALID_STATE;
        result.state = TaskbarState::lost;
        return result;
    }
    LONG_PTR current = 0;
    DWORD error = ERROR_SUCCESS;
    if (!read_style(hwnd, current, error)) {
        result.error = error;
        result.state = TaskbarState::lost;
        return result;
    }
    const LONG_PTR difference = (current ^ entry.applied_exstyle) & entry.owned_mask;
    if (!difference) {
        result.state = TaskbarState::hidden;
        return result;
    }
    // The target changed one of the owned bits after our write.  Treat that as
    // the newer intent instead of fighting it; an explicit restore() can still
    // write back the remembered original values.
    result.state = TaskbarState::conflict;
    result.conflict_bits = difference;
    return result;
}

TaskbarResult TaskbarLedger::show(const Window& target) {
    TaskbarResult result;
    result.tracked = entries_.size();
    const auto found = entries_.find(target.handle);
    if (found == entries_.end()) {
        const DWORD identity = identity_error(target);
        if (identity) {
            result.error = identity;
            result.state = TaskbarState::lost;
        }
        return result;
    }
    if (!same_identity(found->second.identity, target)) {
        result.error = ERROR_INVALID_WINDOW_HANDLE;
        result.state = TaskbarState::lost;
        return result;
    }
    return show_tracked(target, found);
}

TaskbarResult TaskbarLedger::show_tracked(const Window& target,
    std::map<std::uintptr_t, Entry>::iterator entry) {
    TaskbarResult result;
    result.tracked = entries_.size();
    const DWORD identity = identity_error(entry->second.identity);
    if (identity) {
        if (identity != ERROR_ACCESS_DENIED) {
            entries_.erase(entry);
            result.tracked = entries_.size();
        }
        result.error = identity;
        result.state = TaskbarState::lost;
        return result;
    }
    const HWND hwnd = reinterpret_cast<HWND>(target.handle);
    const std::uintptr_t marker =
        reinterpret_cast<std::uintptr_t>(GetPropW(hwnd, kTaskbarMarker));
    if (marker != entry->second.token) {
        // The marker was removed or belongs to someone else.  The HWND may have
        // been reused inside this process, so never write to it.
        entries_.erase(entry);
        result.tracked = entries_.size();
        result.error = ERROR_INVALID_WINDOW_HANDLE;
        result.state = TaskbarState::lost;
        return result;
    }
    LONG_PTR current = 0;
    DWORD error = ERROR_SUCCESS;
    if (!read_style(hwnd, current, error)) {
        result.error = error;
        result.state = TaskbarState::lost;
        return result;
    }
    result.conflict_bits = (current ^ entry->second.applied_exstyle) & entry->second.owned_mask;
    const LONG_PTR desired = (current & ~entry->second.owned_mask)
        | (entry->second.original_exstyle & entry->second.owned_mask);
    if (desired != current) {
        LONG_PTR actual = 0;
        if (!write_style(hwnd, desired, entry->second.owned_mask, actual, error)) {
            result.error = error;
            result.state = TaskbarState::hidden;
            return result;
        }
        result.changed = true;
        result.changed_bits = desired ^ current;
        if ((actual & entry->second.owned_mask) != (desired & entry->second.owned_mask)) {
            result.error = ERROR_INVALID_STATE;
            result.state = TaskbarState::hidden;
            result.tracked = entries_.size();
            return result;
        }
    }
    const std::uintptr_t token = entry->second.token;
    entries_.erase(entry);
    result.tracked = entries_.size();
    result.state = TaskbarState::normal;
    const DWORD marker_error = clear_marker(hwnd, token);
    if (marker_error) result.error = marker_error;
    return result;
}

TaskbarResult TaskbarLedger::query(const Window& target) const {
    TaskbarResult result;
    result.tracked = entries_.size();
    const DWORD identity = identity_error(target);
    if (identity) {
        result.error = identity;
        result.state = TaskbarState::lost;
        return result;
    }
    LONG_PTR current = 0;
    DWORD error = ERROR_SUCCESS;
    if (!read_style(reinterpret_cast<HWND>(target.handle), current, error)) {
        result.error = error;
        result.state = TaskbarState::lost;
        return result;
    }
    const auto found = entries_.find(target.handle);
    if (found != entries_.end()) {
        if (!same_identity(found->second.identity, target)
            || reinterpret_cast<std::uintptr_t>(GetPropW(reinterpret_cast<HWND>(target.handle), kTaskbarMarker))
                != found->second.token) {
            result.error = ERROR_INVALID_WINDOW_HANDLE;
            result.state = TaskbarState::lost;
            return result;
        }
        result.conflict_bits = (current ^ found->second.applied_exstyle) & found->second.owned_mask;
        result.state = result.conflict_bits ? TaskbarState::conflict : TaskbarState::hidden;
        return result;
    }
    const bool tool = (current & WS_EX_TOOLWINDOW) != 0;
    const bool app = (current & WS_EX_APPWINDOW) != 0;
    result.state = tool && !app ? TaskbarState::foreign : TaskbarState::normal;
    return result;
}

TaskbarResult TaskbarLedger::restore_all() {
    TaskbarResult result;
    std::vector<std::uintptr_t> handles;
    handles.reserve(entries_.size());
    for (const auto& item : entries_) handles.push_back(item.first);
    for (const std::uintptr_t handle : handles) {
        const auto found = entries_.find(handle);
        if (found == entries_.end()) continue;
        const TaskbarResult item = show_tracked(found->second.identity, found);
        result.changed = result.changed || item.changed;
        result.conflict_bits |= item.conflict_bits;
        if (item.error && result.error == ERROR_SUCCESS) result.error = item.error;
    }
    result.state = entries_.empty() ? TaskbarState::normal : TaskbarState::hidden;
    result.tracked = entries_.size();
    return result;
}

std::size_t TaskbarLedger::forget_invalid() {
    std::size_t removed = 0;
    for (auto item = entries_.begin(); item != entries_.end();) {
        const DWORD error = identity_error(item->second.identity);
        if (invalid_identity_error(error)) {
            item = entries_.erase(item);
            ++removed;
        } else {
            ++item;
        }
    }
    return removed;
}

bool TaskbarLedger::tracked(const Window& target) const {
    return entries_.find(target.handle) != entries_.end();
}

std::size_t TaskbarLedger::size() const { return entries_.size(); }

std::vector<TaskbarEntry> TaskbarLedger::snapshot() const {
    std::vector<TaskbarEntry> result;
    result.reserve(entries_.size());
    for (const auto& item : entries_) {
        result.push_back({item.first, item.second.identity.pid, item.second.identity.tid,
            item.second.original_exstyle, item.second.applied_exstyle, item.second.owned_mask});
    }
    return result;
}

std::vector<Window> TaskbarLedger::select_process(const std::vector<Window>& windows, DWORD pid) {
    std::vector<Window> result;
    for (const auto& window : windows) {
        if (window.pid != pid || !window.handle) continue;
        const bool duplicate = std::any_of(result.begin(), result.end(),
            [&](const Window& existing) { return existing.handle == window.handle; });
        if (!duplicate) result.push_back(window);
    }
    return result;
}

TaskbarBatchResult TaskbarLedger::hide_all(TaskbarLedger& ledger, const std::vector<Window>& targets) {
    TaskbarBatchResult batch;
    batch.requested = static_cast<unsigned>(targets.size());
    for (const auto& target : targets) {
        const TaskbarResult result = ledger.hide(target);
        if (result.error) {
            record_batch_error(batch, target.handle, result.error);
            continue;
        }
        switch (result.state) {
        case TaskbarState::hidden: result.changed ? ++batch.changed : ++batch.unchanged; break;
        case TaskbarState::foreign: ++batch.foreign; break;
        case TaskbarState::conflict: ++batch.conflicts; break;
        case TaskbarState::lost: ++batch.invalid; break;
        case TaskbarState::normal: ++batch.unchanged; break;
        }
    }
    return batch;
}

TaskbarBatchResult TaskbarLedger::show_all(TaskbarLedger& ledger, const std::vector<Window>& targets) {
    TaskbarBatchResult batch;
    batch.requested = static_cast<unsigned>(targets.size());
    for (const auto& target : targets) {
        const TaskbarResult result = ledger.show(target);
        if (result.error) {
            record_batch_error(batch, target.handle, result.error);
            continue;
        }
        if (result.conflict_bits) ++batch.conflicts;
        result.changed ? ++batch.changed : ++batch.unchanged;
    }
    return batch;
}

} // namespace av
