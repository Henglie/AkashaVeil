// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/model.h"
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace av {
using WindowKey = std::tuple<std::uintptr_t, unsigned long, std::uint64_t>;
inline WindowKey window_key(const Window& window) { return {window.handle, window.pid, window.process_created}; }
using ProcessKey = std::tuple<unsigned long, std::uint64_t>;
inline ProcessKey process_key(unsigned long pid, std::uint64_t created) { return {pid, created}; }

struct ProcessObservation {
    unsigned long pid = 0;
    std::uint64_t created = 0;
};

struct AutoConfig {
    std::uint64_t first_window_ms = 3000;
    std::uint64_t retry_interval_ms = 250;
    unsigned retry_limit = 3;
};

enum class AutoDisposition {
    none,
    attempt,
    late_window,
    expired,
    process_exited,
    already_done
};

// Monotonic-clock state machine. All deadlines are derived from the caller's
// uint64_t tick (GetTickCount64); a backwards clock is clamped, never allowed
// to reopen a deadline. The first-window deadline is per new process identity
// (pid + process creation time). A window that appears after that deadline is
// reported late and is not automatically protected. Existing processes are
// marked baseline and do not expire, so their later windows remain eligible.
class AutoTracker {
public:
    AutoTracker() = default;
    explicit AutoTracker(const AutoConfig& config) : config_(config) {}

    void configure(const AutoConfig& config) { config_ = config; }
    AutoConfig config() const { return config_; }

    // Startup/policy reset. `protect_existing=false` records current windows as
    // an already-decided baseline; later windows from those baseline processes
    // are still candidates. `protect_existing=true` makes current windows
    // candidates immediately.
    void seed(const std::vector<Window>& windows, bool protect_existing, std::uint64_t now);
    void reset(const std::vector<Window>& windows, bool protect_existing, std::uint64_t now);

    // Process discovery is separate from window discovery: a process that has
    // not created a window yet starts its first-window deadline here.
    void observe_processes(const std::vector<ProcessObservation>& processes, std::uint64_t now);
    // Existing processes without a window at startup are baseline: they never
    // expire, so a window created later by them is still eligible.
    void mark_baseline_processes(const std::vector<ProcessObservation>& processes, std::uint64_t now);

    AutoDisposition decide(const Window& window, std::uint64_t now);
    void complete(const Window& window, bool success, std::uint64_t now);
    void expire(std::uint64_t now);
    void prune(const std::vector<Window>& windows, const std::vector<ProcessObservation>& processes, std::uint64_t now);

    std::size_t waiting_processes() const;
    std::size_t pending_windows() const;
    std::size_t protected_windows() const;
    std::size_t late_windows() const;
    std::size_t failed_windows() const;
    std::size_t exited_processes() const;
    std::size_t expired_processes() const;
    bool process_exited(unsigned long pid, std::uint64_t created) const;

    // Compatibility surface for the B0 tests. New integration must use the
    // process/window state API above; these methods are the old readiness
    // retry only and are not the first-window implementation.
    void seed(const std::vector<Window>& windows);
    bool needs_attempt(const Window& window, std::uint64_t now, unsigned retry_ms);
    void complete(const Window& window, bool success);
    void prune(const std::vector<Window>& windows);
    void clear();

private:
    struct ProcessState {
        std::uint64_t first_seen = 0;
        std::uint64_t deadline = 0;
        std::uint64_t exited_at = 0;
        bool exited = false;
        bool baseline = false;
        bool window_seen = false;
        bool expired = false;
    };
    struct WindowState {
        std::uint64_t first_seen = 0;
        std::uint64_t next_attempt = 0;
        unsigned attempts = 0;
        bool done = false;
        bool failed = false;
        bool late = false;
    };
    struct LegacyAttempt {
        std::uint64_t first = 0;
        bool done = false;
    };

    std::uint64_t monotonic(std::uint64_t now);
    ProcessState& process_for(const Window& window, std::uint64_t now);

    AutoConfig config_;
    std::map<ProcessKey, ProcessState> processes_;
    std::map<WindowKey, WindowState> windows_;
    std::map<WindowKey, LegacyAttempt> legacy_;
    std::uint64_t last_now_ = 0;
};
}

