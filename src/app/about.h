// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <string>

namespace av {
// UI labels are supplied by the caller so the About module does not depend on
// a particular i18n implementation. Long offline texts (about/help/third-party
// notices/full license) are loaded from the generated RCDATA resources.
struct AboutLabels {
    const wchar_t* window_title;
    const wchar_t* version_label;
    const wchar_t* author_line;
    const wchar_t* copyright_line;
    const wchar_t* privacy_line;
    const wchar_t* tab_about;
    const wchar_t* tab_license;
    const wchar_t* tab_third_party;
    const wchar_t* tab_help;
    const wchar_t* contribute;
    const wchar_t* copy_info;
    const wchar_t* close;
    const wchar_t* copied;
    const wchar_t* contribution_title;
    const wchar_t* contribution_hint;
    const wchar_t* contribution_alt;
    const wchar_t* contribution_missing;
    const wchar_t* license_missing;
    const wchar_t* third_party_missing;
    const wchar_t* help_missing;
    const wchar_t* about_missing;
};

struct AboutOptions {
    HINSTANCE instance = nullptr;
    const wchar_t* version = L"";
    const wchar_t* architecture = L"";
    const wchar_t* language = L"en"; // "zh" selects Chinese resources; all else English
    bool dark = false;
    bool rtl = false;
    bool activate = true;
    COLORREF background = 0;
    COLORREF panel = 0;
    COLORREF foreground = 0;
    UINT dpi = 96;
};

// Loads the application icon at the requested pixel size. The caller owns the
// returned icon and must DestroyIcon it when it is no longer used.
HICON load_app_icon(HINSTANCE instance, int size);

// Shows a modal About window (About / License / Third party / Help tabs) with
// an offline contribution entry. Returns after the window closes and restores
// activation to owner when owner is still valid.
int show_about(HWND owner, const AboutOptions& options, const AboutLabels& labels, int initial_tab = 0);
}
