// SPDX-License-Identifier: Apache-2.0
#include "app/profile_file_dialog.h"
#include <commdlg.h>

namespace av {

namespace {

std::wstring choose(HWND owner, bool opening, bool csv,
    const ProfileFileDialogStrings& strings, DWORD& error) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrFilter = csv ? strings.csv_filter : strings.profile_filter;
    dialog.lpstrDefExt = csv ? strings.csv_extension : strings.profile_extension;
    dialog.lpstrTitle = csv
        ? (opening ? nullptr : strings.export_title)
        : (opening ? strings.open_title : strings.save_title);
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST
        | (opening ? OFN_FILEMUSTEXIST : OFN_OVERWRITEPROMPT);
    const BOOL ok = opening ? GetOpenFileNameW(&dialog) : GetSaveFileNameW(&dialog);
    if (!ok) {
        error = CommDlgExtendedError();
        return {};
    }
    error = ERROR_SUCCESS;
    return path;
}

}

std::wstring pick_profile_file(HWND owner, bool opening,
    const ProfileFileDialogStrings& strings, DWORD& error) {
    return choose(owner, opening, false, strings, error);
}

std::wstring pick_csv_file(HWND owner, bool opening,
    const ProfileFileDialogStrings& strings, DWORD& error) {
    return choose(owner, opening, true, strings, error);
}

}
