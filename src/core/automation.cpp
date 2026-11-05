// SPDX-License-Identifier: Apache-2.0
#include "core/automation.h"
#include <set>

namespace av {
namespace {
bool valid_identity(const Window& window) {
    return window.handle && window.pid && window.process_created;
}
bool valid_process(const ProcessObservation& process) {
    return process.pid && process.created;
}
}

std::uint64_t AutoTracker::monotonic(std::uint64_t now) {
    if (now < last_now_) now = last_now_;
    else last_now_ = now;
    return now;
}

AutoTracker::ProcessState& AutoTracker::process_for(const Window& window, std::uint64_t now) {
    const auto key = process_key(window.pid, window.process_created);
    const auto found = processes_.find(key);
    if (found != processes_.end()) return found->second;
    ProcessState state;
    state.first_seen = now;
    state.deadline = now > UINT64_MAX - config_.first_window_ms ? UINT64_MAX : now + config_.first_window_ms;
    const auto inserted = processes_.emplace(key, state);
    return inserted.first->second;
}


void AutoTracker::seed(const std::vector<Window>& windows, bool protect_existing, std::uint64_t now) {
    processes_.clear();
    windows_.clear();
    legacy_.clear();
    last_now_ = 0;
    now = monotonic(now);
    for (const auto& window : windows) {
        if (!valid_identity(window)) continue;
        auto& process = process_for(window, now);
        process.baseline = true;
        WindowState state;
        state.first_seen = now;
        state.next_attempt = now;
        state.done = !protect_existing;
        windows_[window_key(window)] = state;
    }
}

void AutoTracker::reset(const std::vector<Window>& windows, bool protect_existing, std::uint64_t now) {
    seed(windows, protect_existing, now);
}

void AutoTracker::mark_baseline_processes(const std::vector<ProcessObservation>& processes, std::uint64_t now) {
    now = monotonic(now);
    for (const auto& process : processes) {
        if (!valid_process(process)) continue;
        const auto key = process_key(process.pid, process.created);
        const auto found = processes_.find(key);
        if (found == processes_.end()) {
            ProcessState state;
            state.first_seen = now;
            state.deadline = now + config_.first_window_ms;
            state.baseline = true;
            state.expired = false;
            processes_.emplace(key, state);
        } else {
            found->second.baseline = true;
            found->second.exited = false;
            found->second.exited_at = 0;
            found->second.expired = false;
        }
    }
}

void AutoTracker::observe_processes(const std::vector<ProcessObservation>& processes, std::uint64_t now) {
    now = monotonic(now);
    std::set<ProcessKey> live;
    for (const auto& process : processes) {
        if (!valid_process(process)) continue;
        const auto key = process_key(process.pid, process.created);
        live.insert(key);
        const auto found = processes_.find(key);
        if (found == processes_.end()) {
            ProcessState state;
            state.first_seen = now;
            state.deadline = now > UINT64_MAX - config_.first_window_ms ? UINT64_MAX : now + config_.first_window_ms;
            processes_.emplace(key, state);
        } else {
            found->second.exited = false;
            found->second.exited_at = 0;
        }
    }
    for (auto& entry : processes_) {
        if (!live.count(entry.first) && !entry.second.exited) {
            entry.second.exited = true;
            entry.second.exited_at = now;
        }
    }
}

AutoDisposition AutoTracker::decide(const Window& window, std::uint64_t now) {
    if (!valid_identity(window)) return AutoDisposition::none;
    now = monotonic(now);
    auto& process = process_for(window, now);
    if (process.exited) return AutoDisposition::process_exited;

    const auto key = window_key(window);
    const auto found = windows_.find(key);
    if (found != windows_.end()) {
        auto& state = found->second;
        if (state.done) return AutoDisposition::already_done;
        if (state.failed) return AutoDisposition::none;
        if (state.late) return AutoDisposition::late_window;
        if (now < state.next_attempt) return AutoDisposition::none;
        if (state.attempts >= 1u + config_.retry_limit) { state.failed = true; return AutoDisposition::none; }
        ++state.attempts;
        process.window_seen = true;
        return AutoDisposition::attempt;
    }

    if (!process.baseline && (process.expired || now > process.deadline)) {
        WindowState state;
        state.first_seen = now;
        state.late = true;
        windows_[key] = state;
        return AutoDisposition::late_window;
    }

    WindowState state;
    state.first_seen = now;
    state.next_attempt = now;
    state.attempts = 1;
    windows_[key] = state;
    process.window_seen = true;
    return AutoDisposition::attempt;
}

void AutoTracker::complete(const Window& window, bool success, std::uint64_t now) {
    now = monotonic(now);
    const auto found = windows_.find(window_key(window));
    if (found == windows_.end()) return;
    auto& state = found->second;
    if (success) {
        state.done = true;
        state.failed = false;
        return;
    }
    if (state.attempts >= 1u + config_.retry_limit) {
        state.failed = true;
        return;
    }
    state.next_attempt = now > UINT64_MAX - config_.retry_interval_ms ? UINT64_MAX : now + config_.retry_interval_ms;
}

void AutoTracker::expire(std::uint64_t now) {
    now = monotonic(now);
    for (auto& entry : processes_) {
        auto& state = entry.second;
        if (!state.baseline && !state.exited && !state.window_seen && now > state.deadline) state.expired = true;
    }
}

void AutoTracker::prune(const std::vector<Window>& windows, const std::vector<ProcessObservation>& processes, std::uint64_t now) {
    now = monotonic(now);
    expire(now);
    std::set<WindowKey> live_windows;
    for (const auto& window : windows) if (valid_identity(window)) live_windows.insert(window_key(window));
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!live_windows.count(it->first)) it = windows_.erase(it);
        else ++it;
    }
    std::set<ProcessKey> live_processes;
    for (const auto& process : processes) if (valid_process(process)) live_processes.insert(process_key(process.pid, process.created));
    for (auto it = processes_.begin(); it != processes_.end();) {
        auto& state = it->second;
        if (!live_processes.count(it->first) && !state.exited) { state.exited = true; state.exited_at = now; }
        const bool expired = state.exited && now >= state.exited_at && now - state.exited_at > 60000;
        if (expired) it = processes_.erase(it); else ++it;
    }
}

std::size_t AutoTracker::waiting_processes() const {
    std::size_t count = 0;
    for (const auto& entry : processes_) if (!entry.second.baseline && !entry.second.exited && !entry.second.window_seen && !entry.second.expired) ++count;
    return count;
}

std::size_t AutoTracker::pending_windows() const {
    std::size_t count = 0;
    for (const auto& entry : windows_) if (!entry.second.done && !entry.second.failed && !entry.second.late) ++count;
    return count;
}

std::size_t AutoTracker::protected_windows() const {
    std::size_t count = 0;
    for (const auto& entry : windows_) if (entry.second.done) ++count;
    return count;
}

std::size_t AutoTracker::late_windows() const {
    std::size_t count = 0;
    for (const auto& entry : windows_) if (entry.second.late) ++count;
    return count;
}

std::size_t AutoTracker::failed_windows() const {
    std::size_t count = 0;
    for (const auto& entry : windows_) if (entry.second.failed) ++count;
    return count;
}

std::size_t AutoTracker::exited_processes() const {
    std::size_t count = 0;
    for (const auto& entry : processes_) if (entry.second.exited) ++count;
    return count;
}

std::size_t AutoTracker::expired_processes() const {
    std::size_t count = 0;
    for (const auto& entry : processes_) if (entry.second.expired) ++count;
    return count;
}

bool AutoTracker::process_exited(unsigned long pid, std::uint64_t created) const {
    const auto found = processes_.find(process_key(pid, created));
    return found != processes_.end() && found->second.exited;
}

// --- B0 compatibility surface -------------------------------------------------

void AutoTracker::seed(const std::vector<Window>& windows) {
    for (const auto& window : windows) legacy_[window_key(window)] = {0, true};
}

bool AutoTracker::needs_attempt(const Window& window, std::uint64_t now, unsigned retry_ms) {
    if (!window.handle || !window.pid || !window.process_created) return false;
    const auto key = window_key(window);
    const auto found = legacy_.find(key);
    if (found == legacy_.end()) { legacy_.emplace(key, LegacyAttempt{now, false}); return true; }
    auto& attempt = found->second;
    if (attempt.done) return false;
    if (!retry_ms || now - attempt.first >= retry_ms) { attempt.done = true; return false; }
    return true;
}

void AutoTracker::complete(const Window& window, bool success) {
    const auto found = legacy_.find(window_key(window));
    if (found != legacy_.end() && success) found->second.done = true;
}

void AutoTracker::prune(const std::vector<Window>& windows) {
    std::set<WindowKey> live;
    for (const auto& window : windows) if (valid_identity(window)) live.insert(window_key(window));
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!live.count(it->first)) it = windows_.erase(it); else ++it;
    }
    for (auto it = legacy_.begin(); it != legacy_.end();) {
        if (!live.count(it->first)) it = legacy_.erase(it); else ++it;
    }
}

void AutoTracker::clear() {
    processes_.clear();
    windows_.clear();
    legacy_.clear();
    last_now_ = 0;
}
}


