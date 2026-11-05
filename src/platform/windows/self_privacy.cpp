// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/self_privacy.h"
#include <algorithm>
#include <map>

namespace av {
namespace {
constexpr DWORD self_affinity = 0x11;
constexpr wchar_t capture_proxy_property[] = L"AkashaVeil.CaptureProxy";
SelfPrivacy::Impl* active_hook = nullptr;

// Capture mirrors and the route-B proxy must keep their own display affinity.
// Applying the self-protection value to them would either hide the watermarked
// capture mirror or make WGC CreateForWindow(P) fail.
bool internal_capture_window(HWND hwnd) {
    if (GetPropW(hwnd, capture_proxy_property)) return true;
    wchar_t name[128]{};
    if (!GetClassNameW(hwnd, name, static_cast<int>(sizeof(name) / sizeof(name[0])))) return false;
    return wcscmp(name, L"Henglie.AkashaVeil.EffectLayer") == 0
        || wcscmp(name, L"Henglie.AkashaVeil.CaptureOnlyOwner") == 0
        || wcscmp(name, L"Henglie.AkashaVeil.CaptureOnlyLayer") == 0
        || wcscmp(name, L"AkashaVeil.CaptureProxy.Owner") == 0
        || wcscmp(name, L"AkashaVeil.CaptureProxy.Source") == 0;
}
}

struct SelfPrivacy::Impl {
    HWND owner = nullptr;
    UINT message = 0;
    ProblemSink sink = nullptr;
    void* context = nullptr;
    bool enabled = false;
    HWINEVENTHOOK hook = nullptr;
    std::vector<SelfWindowInfo> windows;
    std::map<HWND, bool> we_set;
    std::map<HWND, bool> pre_existing;
    std::size_t failures = 0;
    DWORD last_error = 0;

    static void CALLBACK event_proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object,
        LONG id_child, DWORD, DWORD) {
        (void)event; (void)id_child;
        auto* self = active_hook;
        if (!self || !self->owner || !self->message || !hwnd) return;
        if (id_object != OBJID_WINDOW) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
        PostMessageW(self->owner, self->message, 0, 0);
    }

    void publish_failure(DWORD error) {
        last_error = error;
        if (sink) sink(context, error);
    }
};

SelfPrivacy::SelfPrivacy() : impl_(std::make_unique<Impl>()) {}
SelfPrivacy::~SelfPrivacy() { detach(); }
SelfPrivacy::SelfPrivacy(SelfPrivacy&&) noexcept = default;
SelfPrivacy& SelfPrivacy::operator=(SelfPrivacy&&) noexcept = default;

bool SelfPrivacy::apply(HWND hwnd, bool enabled, DWORD& error) {
    error = 0;
    if (!hwnd || !IsWindow(hwnd)) { error = ERROR_INVALID_WINDOW_HANDLE; return false; }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) { error = ERROR_ACCESS_DENIED; return false; }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) { error = ERROR_INVALID_WINDOW_HANDLE; return false; }
    if (internal_capture_window(hwnd)) { error = ERROR_ACCESS_DENIED; return false; }
    if (!SetWindowDisplayAffinity(hwnd, enabled ? self_affinity : 0)) {
        error = GetLastError();
        return false;
    }
    return true;
}

std::vector<SelfWindowInfo> SelfPrivacy::enumerate(HWND main) {
    std::vector<SelfWindowInfo> result;
    struct Context { std::vector<SelfWindowInfo>* out; HWND main; } context{&result, main};
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        auto* ctx = reinterpret_cast<Context*>(data);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;
        if (internal_capture_window(hwnd)) return TRUE;
        SelfWindowInfo info;
        info.hwnd = hwnd;
        info.pid = pid;
        info.visible = IsWindowVisible(hwnd) != FALSE;
        info.owned = GetWindow(hwnd, GW_OWNER) != nullptr;
        wchar_t class_name[256]{};
        GetClassNameW(hwnd, class_name, 256);
        info.class_name = class_name;
        const int length = GetWindowTextLengthW(hwnd);
        if (length > 0) {
            info.title.resize(static_cast<std::size_t>(length) + 1);
            info.title.resize(GetWindowTextW(hwnd, info.title.data(), length + 1));
        }
        DWORD affinity = 0;
        const bool known = GetWindowDisplayAffinity(hwnd, &affinity) != FALSE;
        info.supported = known;
        info.protected_now = known && affinity == self_affinity;
        if (!known && info.visible) info.error = GetLastError();
        ctx->out->push_back(std::move(info));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return result;
}

bool SelfPrivacy::attach(HWND owner, UINT message, ProblemSink sink, void* context) {
    impl_->owner = owner;
    impl_->message = message;
    impl_->sink = sink;
    impl_->context = context;
    active_hook = impl_.get();
    if (!impl_->hook) {
        impl_->hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr,
            Impl::event_proc, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    }
    return impl_->hook != nullptr;
}

void SelfPrivacy::detach() {
    if (!impl_) return;
    if (impl_->enabled) set_enabled(false);
    if (impl_->hook) {
        UnhookWinEvent(impl_->hook);
        impl_->hook = nullptr;
    }
    if (active_hook == impl_.get()) active_hook = nullptr;
    impl_->owner = nullptr;
    impl_->sink = nullptr;
    impl_->context = nullptr;
}

bool SelfPrivacy::enabled() const { return impl_ && impl_->enabled; }
UINT SelfPrivacy::sync_message() const { return impl_ ? impl_->message : 0; }

bool SelfPrivacy::set_enabled(bool value) {
    if (!impl_) return false;
    if (impl_->enabled == value) {
        sync();
        return true;
    }
    impl_->enabled = value;
    sync();
    return impl_->failures == 0;
}

void SelfPrivacy::sync() {
    if (!impl_) return;
    auto current = enumerate(impl_->owner);
    std::map<HWND, bool> we_set;
    std::map<HWND, bool> pre_existing;
    std::size_t failures = 0;
    DWORD first_error = 0;

    if (impl_->enabled) {
        for (auto& window : current) {
            const auto old_set = impl_->we_set.find(window.hwnd);
            const auto old_pre = impl_->pre_existing.find(window.hwnd);
            if (window.protected_now) {
                window.protected_by_us = old_set != impl_->we_set.end() && old_set->second;
                window.pre_existing = (old_pre != impl_->pre_existing.end()) ? old_pre->second
                    : !window.protected_by_us;
                we_set[window.hwnd] = window.protected_by_us;
                pre_existing[window.hwnd] = window.pre_existing;
                continue;
            }
            DWORD error = 0;
            if (apply(window.hwnd, true, error)) {
                window.protected_now = true;
                window.protected_by_us = true;
                we_set[window.hwnd] = true;
                pre_existing[window.hwnd] = false;
            } else {
                window.error = error;
                window.supported = false;
                window.protected_by_us = false;
                if (window.visible) {
                    ++failures;
                    if (!first_error) first_error = error;
                }
            }
        }
    } else {
        for (const auto& entry : impl_->we_set) {
            if (!entry.second) continue;
            const HWND hwnd = entry.first;
            if (!IsWindow(hwnd)) continue;
            DWORD affinity = 0;
            if (!GetWindowDisplayAffinity(hwnd, &affinity) || affinity != self_affinity) continue;
            DWORD error = 0;
            apply(hwnd, false, error);
        }
        for (auto& window : current) {
            const auto old_pre = impl_->pre_existing.find(window.hwnd);
            window.pre_existing = old_pre != impl_->pre_existing.end() && old_pre->second;
        }
    }

    const DWORD previous_error = impl_->last_error;
    impl_->windows = std::move(current);
    impl_->we_set = std::move(we_set);
    impl_->pre_existing = std::move(pre_existing);
    impl_->failures = failures;
    impl_->last_error = first_error;
    if (impl_->enabled && first_error && first_error != previous_error) impl_->publish_failure(first_error);
}

const std::vector<SelfWindowInfo>& SelfPrivacy::windows() const {
    static const std::vector<SelfWindowInfo> empty;
    return impl_ ? impl_->windows : empty;
}
std::size_t SelfPrivacy::failure_count() const { return impl_ ? impl_->failures : 0; }
DWORD SelfPrivacy::last_error() const { return impl_ ? impl_->last_error : 0; }

} // namespace av
