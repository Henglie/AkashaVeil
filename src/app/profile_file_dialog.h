// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <windows.h>

namespace av {

struct ProfileFileDialogStrings {
    const wchar_t* profile_filter = L"AkashaVeil profile (*.ini)\0*.ini\0All files (*.*)\0*.*\0\0";
    const wchar_t* csv_filter = L"CSV (*.csv)\0*.csv\0All files (*.*)\0*.*\0\0";
    const wchar_t* profile_extension = L"ini";
    const wchar_t* csv_extension = L"csv";
    const wchar_t* open_title = L"Load profile";
    const wchar_t* save_title = L"Save profile";
    const wchar_t* export_title = L"Export window list";
};

// Real common file dialogs are kept behind this adapter so the controller and
// tests never create a visible window.  Return empty on cancel; error carries
// CommDlgExtendedError when the dialog itself failed.
std::wstring pick_profile_file(HWND owner, bool opening,
    const ProfileFileDialogStrings& strings, DWORD& error);
std::wstring pick_csv_file(HWND owner, bool opening,
    const ProfileFileDialogStrings& strings, DWORD& error);

}
