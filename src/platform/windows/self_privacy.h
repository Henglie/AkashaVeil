// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace av {

struct SelfWindowInfo {
    HWND hwnd = nullptr;
    std::wstring class_name;
    std::wstring title;
    DWORD pid = 0;
    bool visible = false;
    bool owned = false;
    bool protected_now = false;
    bool protected_by_us = false;
    bool pre_existing = false;
    bool supported = true;
    DWORD error = 0;
};

class SelfPrivacy {
public:
    struct Impl;
    using ProblemSink = void (*)(void* context, DWORD error);

    SelfPrivacy();
    ~SelfPrivacy();
    SelfPrivacy(const SelfPrivacy&) = delete;
    SelfPrivacy& operator=(const SelfPrivacy&) = delete;
    SelfPrivacy(SelfPrivacy&&) noexcept;
    SelfPrivacy& operator=(SelfPrivacy&&) noexcept;

    bool attach(HWND owner, UINT sync_message, ProblemSink sink, void* context);
    void detach();
    bool enabled() const;
    bool set_enabled(bool value);
    void sync();

    const std::vector<SelfWindowInfo>& windows() const;
    std::size_t failure_count() const;
    DWORD last_error() const;
    UINT sync_message() const;

    static bool apply(HWND hwnd, bool enabled, DWORD& error);
    static std::vector<SelfWindowInfo> enumerate(HWND main);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace av
