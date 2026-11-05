// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>
#include "core/model.h"

namespace av {

// Marker property written onto a target window while this ledger owns its
// taskbar style.  The property is removed on restore and disappears with the
// window; it guards against same-process HWND reuse that a PID/TID/creation
// time check alone cannot detect.
inline constexpr wchar_t kTaskbarMarker[] = L"AkashaVeil.Taskbar.Track.v1";

enum class TaskbarState : unsigned {
    normal = 0,   // no ledger ownership and no foreign taskbar style
    hidden = 1,   // ledger owns the style and the hidden bits are still applied
    foreign = 2,  // the window was already hidden by something else; not claimed
    conflict = 3, // the target changed one of the owned bits after we wrote it
    lost = 4      // the target identity is no longer valid; nothing was written
};

struct TaskbarResult {
    DWORD error = ERROR_SUCCESS;
    TaskbarState state = TaskbarState::normal;
    bool changed = false;
    LONG_PTR changed_bits = 0;  // bits written by this call
    LONG_PTR conflict_bits = 0; // owned bits the target changed before restore
    std::size_t tracked = 0;    // ledger entries after the call
    explicit operator bool() const { return error == ERROR_SUCCESS; }
};

struct TaskbarEntry {
    std::uintptr_t handle = 0;
    unsigned long pid = 0, tid = 0;
    LONG_PTR original_exstyle = 0;
    LONG_PTR applied_exstyle = 0;
    LONG_PTR owned_mask = 0;
};

struct TaskbarBatchResult {
    unsigned requested = 0, changed = 0, unchanged = 0, foreign = 0, conflicts = 0;
    unsigned invalid = 0, failed = 0;
    DWORD first_error = ERROR_SUCCESS;
    std::vector<std::pair<std::uintptr_t, DWORD>> errors;
};

// Session-scoped ownership ledger for taskbar style bits.
//
// hide() records the original values and clears/sets only WS_EX_TOOLWINDOW and
// WS_EX_APPWINDOW bits that actually change.  show() writes back only those
// recorded bits, preserving every unrelated bit (including bits the target set
// after hide).  Repeated hide/show calls are idempotent.  A target whose
// identity or marker is invalid is never written.  Not thread-safe; call from
// the UI thread that owns the window message loop.
class TaskbarLedger {
public:
    using IdentityCheck = bool (*)(const Window&);

    TaskbarLedger();
    explicit TaskbarLedger(IdentityCheck check);
    ~TaskbarLedger();
    TaskbarLedger(const TaskbarLedger&) = delete;
    TaskbarLedger& operator=(const TaskbarLedger&) = delete;

    TaskbarResult hide(const Window& target);
    TaskbarResult show(const Window& target);
    TaskbarResult query(const Window& target) const;
    TaskbarResult restore_all();
    std::size_t forget_invalid();
    bool tracked(const Window& target) const;
    std::size_t size() const;
    std::vector<TaskbarEntry> snapshot() const;

    // Selection is deliberately separate from capture affinity: callers feed
    // the enumerated windows and pick one process (or individual windows).
    static std::vector<Window> select_process(const std::vector<Window>& windows, DWORD pid);
    static TaskbarBatchResult hide_all(TaskbarLedger& ledger, const std::vector<Window>& targets);
    static TaskbarBatchResult show_all(TaskbarLedger& ledger, const std::vector<Window>& targets);

private:
    struct Entry {
        Window identity;
        LONG_PTR original_exstyle = 0;
        LONG_PTR applied_exstyle = 0;
        LONG_PTR owned_mask = 0;
        std::uintptr_t token = 0;
    };

    DWORD identity_error(const Window& target) const;
    TaskbarResult hide_new(const Window& target);
    TaskbarResult hide_tracked(const Window& target, Entry& entry);
    TaskbarResult show_tracked(const Window& target, std::map<std::uintptr_t, Entry>::iterator entry);

    IdentityCheck check_ = nullptr;
    std::map<std::uintptr_t, Entry> entries_;
};

} // namespace av
