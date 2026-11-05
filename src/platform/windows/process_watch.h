// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/automation.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace av {
struct WatchConfig {
    unsigned sweep_ms = 1000;
    unsigned process_budget = 512;
};

struct WatchStats {
    std::uint64_t scans = 0;
    std::uint64_t wakes = 0;
    std::uint64_t fallback_wakes = 0;
    std::uint64_t added = 0;
    std::uint64_t removed = 0;
    unsigned peak_processes = 0;
    unsigned denied_processes = 0;
    DWORD last_error = 0;
};

std::vector<ProcessObservation> enumerate_processes(unsigned budget, DWORD& error, unsigned* denied = nullptr);

// Event-driven process discovery with a bounded fallback sweep. The object is
// inert until start(); stop() joins the thread, removes the WinEvent hook and
// kills the timer, so an idle application has no watcher thread or polling.
class ProcessWatcher {
public:
    ProcessWatcher() = default;
    ProcessWatcher(const ProcessWatcher&) = delete;
    ProcessWatcher& operator=(const ProcessWatcher&) = delete;
    ~ProcessWatcher();

    bool start(HWND notify, UINT message, const WatchConfig& config, DWORD& error);
    void stop();
    bool active() const { return active_.load(); }
    WatchStats stats() const;

private:
    void run(HWND notify, UINT message, WatchConfig config, HANDLE initialized);

    std::thread thread_;
    DWORD thread_id_ = 0;
    std::atomic_bool active_{false};
    std::atomic_bool stopping_{false};
    std::atomic<std::uint64_t> scans_{0}, wakes_{0}, fallback_wakes_{0}, added_{0}, removed_{0};
    std::atomic<unsigned> peak_{0};
    std::atomic<unsigned> denied_{0};
    std::atomic<DWORD> last_error_{0};
};
}
