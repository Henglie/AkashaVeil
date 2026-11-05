// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

namespace av {

class CleanupChain {
public:
    using Step = void (*)(void* context);
    void add(const wchar_t* name, Step step, void* context);
    bool run();
    bool ran() const { return ran_; }
    const std::vector<std::wstring>& order() const { return order_; }

private:
    struct Entry { std::wstring name; Step step = nullptr; void* context = nullptr; };
    std::vector<Entry> entries_;
    std::vector<std::wstring> order_;
    bool ran_ = false;
};

class SessionLedger {
public:
    enum class ExitPolicy { preserve_affinity, restore_affinity };

    void record_affinity(HWND hwnd, DWORD original, DWORD applied);
    void record_taskbar_style(HWND hwnd, LONG_PTR original, LONG_PTR applied, LONG_PTR mask);
    void set_exit_policy(ExitPolicy policy) { policy_ = policy; }
    ExitPolicy exit_policy() const { return policy_; }
    void exit();
    const std::vector<std::wstring>& actions() const { return actions_; }
    std::size_t affinity_count() const { return affinities_.size(); }
    std::size_t taskbar_count() const { return taskbars_.size(); }

private:
    struct AffinityRecord { HWND hwnd; DWORD original; DWORD applied; };
    struct TaskbarRecord { HWND hwnd; LONG_PTR original; LONG_PTR applied; LONG_PTR mask; };
    std::vector<AffinityRecord> affinities_;
    std::vector<TaskbarRecord> taskbars_;
    std::vector<std::wstring> actions_;
    ExitPolicy policy_ = ExitPolicy::preserve_affinity;
};

} // namespace av
