// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/process_watch.h"
#include "platform/windows/windows.h"
#include <tlhelp32.h>
#include <algorithm>
#include <set>

namespace av {
namespace {
thread_local HWND wake_window = nullptr;
thread_local UINT wake_message = 0;
thread_local ULONGLONG last_wake = 0;
thread_local std::atomic<std::uint64_t>* wake_counter = nullptr;
thread_local std::atomic<DWORD>* wake_error = nullptr;

void notify_wake() {
    if (!wake_window || !wake_message) return;
    const ULONGLONG now = GetTickCount64();
    if (last_wake && now - last_wake < 30) return;
    last_wake = now;
    if (!PostMessageW(wake_window, wake_message, 1, 0)) {
        if (wake_error) wake_error->store(GetLastError());
        return;
    }
    if (wake_counter) ++*wake_counter;
}

void CALLBACK win_event(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !hwnd) return;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    notify_wake();
}
}

std::vector<ProcessObservation> enumerate_processes(unsigned budget, DWORD& error, unsigned* denied) {
    std::vector<ProcessObservation> result;
    if (!budget) budget = 512;
    if (budget > 4096) budget = 4096;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { error = GetLastError(); return result; }
    PROCESSENTRY32W entry{sizeof(entry)};
    error = ERROR_SUCCESS;
    unsigned seen = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (seen++ >= budget) { error = ERROR_MORE_DATA; break; }
            if (!entry.th32ProcessID) continue;
            const auto created = process_created(entry.th32ProcessID);
            if (created) result.push_back({entry.th32ProcessID, created});
            else if (denied) ++*denied;
        } while (Process32NextW(snapshot, &entry));
    } else {
        error = GetLastError();
    }
    CloseHandle(snapshot);
    return result;
}

ProcessWatcher::~ProcessWatcher() { stop(); }

bool ProcessWatcher::start(HWND notify, UINT message, const WatchConfig& config, DWORD& error) {
    if (active_.load()) { error = ERROR_ALREADY_INITIALIZED; return false; }
    if (!notify || !message) { error = ERROR_INVALID_PARAMETER; return false; }
    stop();
    stopping_.store(false);
    last_error_.store(0);
    HANDLE initialized = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!initialized) { error = GetLastError(); return false; }
    thread_ = std::thread(&ProcessWatcher::run, this, notify, message, config, initialized);
    const DWORD waited = WaitForSingleObject(initialized, 5000);
    CloseHandle(initialized);
    if (waited != WAIT_OBJECT_0 || !active_.load()) {
        error = last_error_.load() ? last_error_.load() : ERROR_NOT_READY;
        stop();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

void ProcessWatcher::stop() {
    if (!thread_.joinable()) { active_.store(false); return; }
    stopping_.store(true);
    if (thread_id_) PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    thread_.join();
    thread_id_ = 0;
    active_.store(false);
    stopping_.store(false);
}

WatchStats ProcessWatcher::stats() const {
    WatchStats result;
    result.scans = scans_.load();
    result.wakes = wakes_.load();
    result.fallback_wakes = fallback_wakes_.load();
    result.added = added_.load();
    result.removed = removed_.load();
    result.peak_processes = peak_.load();
    result.denied_processes = denied_.load();
    result.last_error = last_error_.load();
    return result;
}

void ProcessWatcher::run(HWND notify, UINT message, WatchConfig config, HANDLE initialized) {
    thread_id_ = GetCurrentThreadId();
    if (!config.sweep_ms) config.sweep_ms = 1000;
    if (config.sweep_ms < 100) config.sweep_ms = 100;
    if (!config.process_budget) config.process_budget = 512;
    wake_window = notify;
    wake_message = message;
    last_wake = 0;
    wake_counter = &wakes_;
    wake_error = &last_error_;
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, win_event, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) last_error_.store(GetLastError());
    const UINT_PTR timer = SetTimer(nullptr, 1, config.sweep_ms, nullptr);
    if (!timer) {
        last_error_.store(GetLastError() ? GetLastError() : ERROR_NO_SYSTEM_RESOURCES);
        if (hook) UnhookWinEvent(hook);
        wake_window = nullptr;
        wake_counter = nullptr;
        wake_error = nullptr;
        SetEvent(initialized);
        return;
    }
    active_.store(true);
    SetEvent(initialized);

    std::set<ProcessKey> known;
    DWORD error = ERROR_SUCCESS;
    unsigned denied = 0;
    for (const auto& process : enumerate_processes(config.process_budget, error, &denied)) known.insert(process_key(process.pid, process.created));
    denied_ += denied;
    peak_.store(static_cast<unsigned>(known.size()));

    MSG msg{};
    while (!stopping_.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr && msg.wParam == timer) {
            ++scans_;
            error = ERROR_SUCCESS;
            unsigned denied_now = 0;
            const auto processes = enumerate_processes(config.process_budget, error, &denied_now);
            denied_ += denied_now;
            std::set<ProcessKey> next;
            for (const auto& process : processes) next.insert(process_key(process.pid, process.created));
            std::uint64_t added = 0, removed = 0;
            for (const auto& key : next) if (!known.count(key)) ++added;
            for (const auto& key : known) if (!next.count(key)) ++removed;
            if (added || removed) {
                added_ += added;
                removed_ += removed;
                ++fallback_wakes_;
                notify_wake();
            }
            known.swap(next);
            if (known.size() > peak_.load()) peak_.store(static_cast<unsigned>(known.size()));
            if (error && error != ERROR_MORE_DATA) last_error_.store(error);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hook) UnhookWinEvent(hook);
    KillTimer(nullptr, timer);
    wake_window = nullptr;
    wake_message = 0;
    wake_counter = nullptr;
    wake_error = nullptr;
    active_.store(false);
}
}
