// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/tray_controller.h"

namespace av {

TrayController::TrayController(TrayBackend& backend) : backend_(backend) {}

bool TrayController::add_or_modify(bool modify) {
    DWORD error = 0;
    const bool ok = modify ? backend_.modify(visual_, error) : backend_.add(visual_, error);
    status_.active = ok;
    status_.add_failed = !ok;
    status_.last_error = ok ? 0 : error;
    return ok;
}

bool TrayController::configure(const TrayOptions& options, const TrayVisual& visual) {
    options_ = options;
    visual_ = visual;
    status_ = {};
    status_.requested = options_.enabled;
    status_.removed_by_shell = false;
    if (!options_.enabled) {
        backend_.remove();
        status_.active = false;
        status_.add_failed = false;
        status_.last_error = 0;
        return true;
    }
    return add_or_modify(false);
}

bool TrayController::update_visual(const TrayVisual& visual) {
    visual_ = visual;
    if (!options_.enabled) return true;
    return add_or_modify(status_.active);
}

void TrayController::on_shell_restart() {
    status_.removed_by_shell = true;
    status_.active = false;
    if (!options_.enabled) return;
    ++status_.attempts;
    add_or_modify(false);
}

void TrayController::on_shell_unavailable() {
    status_.active = false;
    status_.add_failed = true;
    status_.last_error = ERROR_NOT_READY;
}

void TrayController::on_shell_ready() {
    if (!options_.enabled || status_.active) return;
    ++status_.attempts;
    add_or_modify(false);
}

bool TrayController::notify(const std::wstring& title, const std::wstring& message) {
    if (!options_.enabled || !status_.active) return false;
    DWORD error = 0;
    const bool ok = backend_.notify(title, message, error);
    if (!ok) status_.last_error = error;
    return ok;
}

CloseAction TrayController::close_action() const {
    return options_.enabled && options_.close_to_tray && status_.active
        ? CloseAction::hide_to_tray : CloseAction::stay_visible;
}

bool TrayController::taskbar_may_hide() const {
    return options_.hide_taskbar && status_.active;
}

} // namespace av
