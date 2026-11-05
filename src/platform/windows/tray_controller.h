// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <string>

namespace av {

enum class TrayVisualKind { brand, random };

struct TrayVisual {
    TrayVisualKind kind = TrayVisualKind::brand;
    int brand_icon_id = 0;
    int random_icon_index = 0;
    std::wstring tooltip;
    bool tooltip_is_random = false;
};

struct TrayOptions {
    bool enabled = true;
    bool close_to_tray = true;
    bool hide_taskbar = false;
};

enum class CloseAction { hide_to_tray, stay_visible };
enum class TrayProblem { none, disabled, add_failed, removed_by_shell };

struct TrayStatus {
    bool requested = false;
    bool active = false;
    bool add_failed = false;
    bool removed_by_shell = false;
    unsigned attempts = 0;
    DWORD last_error = 0;
};

class TrayBackend {
public:
    virtual ~TrayBackend() = default;
    virtual bool add(const TrayVisual& visual, DWORD& error) = 0;
    virtual bool modify(const TrayVisual& visual, DWORD& error) = 0;
    virtual bool remove() = 0;
    virtual bool notify(const std::wstring& title, const std::wstring& message, DWORD& error) = 0;
};

class TrayController {
public:
    explicit TrayController(TrayBackend& backend);
    bool configure(const TrayOptions& options, const TrayVisual& visual);
    bool update_visual(const TrayVisual& visual);
    void on_shell_restart();
    void on_shell_unavailable();
    void on_shell_ready();
    bool notify(const std::wstring& title, const std::wstring& message);
    CloseAction close_action() const;
    bool taskbar_may_hide() const;
    bool has_recovery_entry() const { return true; }
    const TrayOptions& options() const { return options_; }
    const TrayVisual& visual() const { return visual_; }
    const TrayStatus& status() const { return status_; }

private:
    bool add_or_modify(bool modify);
    TrayBackend& backend_;
    TrayOptions options_{};
    TrayVisual visual_{};
    TrayStatus status_{};
};

} // namespace av
