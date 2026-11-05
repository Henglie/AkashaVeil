// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace av {

struct PreferenceDraft {
    bool auto_refresh = false;
    bool self_protect = false;
    bool tray = true;
    bool close_to_tray = true;
    bool hide_taskbar = false;
    bool random_title = false;
    bool random_tray_icon = false;
    bool random_tooltip = false;
    bool auto_hide = false;
    bool notify_auto_hide = true;
    bool protect_on_start = false;
    unsigned refresh_ms = 1000;
    unsigned window_wait_ms = 3000;
    std::vector<std::wstring> programs;
};

struct PreferenceValidation {
    bool ok = false;
    const wchar_t* field = nullptr;
};

enum class PreferenceDialogResult { none, applied, ok, cancelled };

class PreferencesController {
public:
    explicit PreferencesController(const PreferenceDraft& applied);
    void begin();
    void mark_controls_ready();
    bool ready() const { return ready_; }

    PreferenceDraft& draft() { return draft_; }
    const PreferenceDraft& draft() const { return draft_; }
    const PreferenceDraft& applied() const { return applied_; }
    const PreferenceValidation& validation() const { return validation_; }
    bool dirty() const;

    void set_tray(bool enabled);
    PreferenceValidation set_refresh_ms(unsigned value);
    PreferenceValidation set_window_wait_ms(unsigned value);
    PreferenceValidation add_program(std::wstring value);
    bool remove_program(std::size_t index);
    PreferenceValidation browse_result(std::wstring path);

    void request_reset();
    bool reset_pending() const { return reset_pending_; }
    void confirm_reset(bool yes);

    PreferenceDialogResult apply();
    PreferenceDialogResult ok();
    PreferenceDialogResult cancel();

    static PreferenceDraft defaults();
    static PreferenceValidation validate(const PreferenceDraft& value);

private:
    PreferenceDraft applied_;
    PreferenceDraft draft_;
    PreferenceValidation validation_{};
    bool ready_ = false;
    bool reset_pending_ = false;
    PreferenceDialogResult result_ = PreferenceDialogResult::none;
};

} // namespace av
