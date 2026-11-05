// SPDX-License-Identifier: Apache-2.0
#include "app/preferences_controller.h"
#include <windows.h>
#include <algorithm>
#include <cstddef>

namespace av {
namespace {

bool same(const PreferenceDraft& a, const PreferenceDraft& b) {
    return a.auto_refresh == b.auto_refresh && a.self_protect == b.self_protect
        && a.tray == b.tray && a.close_to_tray == b.close_to_tray && a.hide_taskbar == b.hide_taskbar
        && a.random_title == b.random_title && a.random_tray_icon == b.random_tray_icon
        && a.random_tooltip == b.random_tooltip && a.auto_hide == b.auto_hide
        && a.notify_auto_hide == b.notify_auto_hide && a.protect_on_start == b.protect_on_start
        && a.refresh_ms == b.refresh_ms && a.window_wait_ms == b.window_wait_ms
        && a.programs == b.programs;
}

void sanitize(PreferenceDraft& value) {
    if (!value.tray) {
        value.close_to_tray = false;
        value.hide_taskbar = false;
    }
}

std::wstring trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t");
    const auto last = value.find_last_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    value = value.substr(first, last - first + 1);
    if (value.size() > 1 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool same_program(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

PreferencesController::PreferencesController(const PreferenceDraft& applied) : applied_(applied), draft_(applied) {}

PreferenceDraft PreferencesController::defaults() { return PreferenceDraft{}; }

PreferenceValidation PreferencesController::validate(const PreferenceDraft& value) {
    if (value.refresh_ms < 100 || value.refresh_ms > 60000) return {false, L"refresh_ms"};
    if (value.window_wait_ms > 60000) return {false, L"window_wait_ms"};
    if (!value.tray && (value.close_to_tray || value.hide_taskbar)) return {false, L"tray.dependency"};
    if (value.programs.size() > 256) return {false, L"programs.count"};
    for (std::size_t i = 0; i < value.programs.size(); ++i) {
        if (value.programs[i].empty() || value.programs[i].find_first_of(L"\r\n") != std::wstring::npos) {
            return {false, L"programs.entry"};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (same_program(value.programs[i], value.programs[j])) return {false, L"programs.duplicate"};
        }
    }
    return {true, nullptr};
}

void PreferencesController::begin() {
    draft_ = applied_;
    validation_ = {};
    ready_ = false;
    reset_pending_ = false;
    result_ = PreferenceDialogResult::none;
}

void PreferencesController::mark_controls_ready() { ready_ = true; }
bool PreferencesController::dirty() const { return !same(draft_, applied_); }

void PreferencesController::set_tray(bool enabled) {
    draft_.tray = enabled;
    sanitize(draft_);
}

PreferenceValidation PreferencesController::set_refresh_ms(unsigned value) {
    draft_.refresh_ms = value;
    validation_ = validate(draft_);
    return validation_;
}

PreferenceValidation PreferencesController::set_window_wait_ms(unsigned value) {
    draft_.window_wait_ms = value;
    validation_ = validate(draft_);
    return validation_;
}

PreferenceValidation PreferencesController::add_program(std::wstring value) {
    auto normalized = trim(std::move(value));
    if (normalized.empty() || normalized.find_first_of(L"\r\n") != std::wstring::npos) {
        validation_ = {false, L"programs.entry"};
        return validation_;
    }
    PreferenceDraft candidate = draft_;
    candidate.programs.push_back(std::move(normalized));
    validation_ = validate(candidate);
    if (!validation_.ok) return validation_;
    draft_ = std::move(candidate);
    return validation_;
}

bool PreferencesController::remove_program(std::size_t index) {
    if (index >= draft_.programs.size()) return false;
    draft_.programs.erase(draft_.programs.begin() + static_cast<std::ptrdiff_t>(index));
    validation_ = validate(draft_);
    return true;
}

PreferenceValidation PreferencesController::browse_result(std::wstring path) {
    return add_program(std::move(path));
}

void PreferencesController::request_reset() { reset_pending_ = true; }

void PreferencesController::confirm_reset(bool yes) {
    if (!reset_pending_) return;
    reset_pending_ = false;
    if (!yes) return;
    draft_ = defaults();
    validation_ = {};
}

PreferenceDialogResult PreferencesController::apply() {
    result_ = PreferenceDialogResult::none;
    if (!ready_) {
        validation_ = {false, L"dialog.not_ready"};
        return result_;
    }
    sanitize(draft_);
    validation_ = validate(draft_);
    if (!validation_.ok) return result_;
    applied_ = draft_;
    result_ = PreferenceDialogResult::applied;
    return result_;
}

PreferenceDialogResult PreferencesController::ok() {
    if (apply() != PreferenceDialogResult::applied) return PreferenceDialogResult::none;
    result_ = PreferenceDialogResult::ok;
    return result_;
}

PreferenceDialogResult PreferencesController::cancel() {
    draft_ = applied_;
    validation_ = {};
    reset_pending_ = false;
    result_ = PreferenceDialogResult::cancelled;
    return result_;
}

} // namespace av
