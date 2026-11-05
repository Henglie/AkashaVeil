// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/profile_v4.h"
#include <windows.h>

namespace av {

struct RulesDialogStrings {
    const wchar_t* title = L"Rules";
    const wchar_t* path = L"Executable path";
    const wchar_t* class_name = L"Window class";
    const wchar_t* window_title = L"Window title";
    const wchar_t* enabled = L"Enabled";
    const wchar_t* mode = L"Protection mode";
    const wchar_t* replacement = L"Replacement";
    const wchar_t* visible = L"Visible watermark";
    const wchar_t* audience = L"Audience";
    const wchar_t* taskbar = L"Taskbar";
    const wchar_t* geometry = L"Geometry";
    const wchar_t* show = L"Show";
    const wchar_t* opacity = L"Opacity";
    const wchar_t* font_size = L"Font";
    const wchar_t* spacing = L"Spacing";
    const wchar_t* foreground = L"Foreground";
    const wchar_t* background = L"Background";
    const wchar_t* text = L"Text";
    const wchar_t* label = L"Label";
    const wchar_t* tag = L"Tracking tag";
    const wchar_t* replacement_label = L"Replacement label";
    const wchar_t* add = L"Add";
    const wchar_t* update = L"Update";
    const wchar_t* remove = L"Delete";
    const wchar_t* toggle = L"Enable / disable";
    const wchar_t* ok = L"OK";
    const wchar_t* cancel = L"Cancel";
    const wchar_t* invalid = L"Invalid or incomplete rule fields";
    const wchar_t* duplicate = L"A rule with this path, class and title already exists";
    const wchar_t* no_selection = L"Select a rule first";
    const wchar_t* mode_none = L"None";
    const wchar_t* mode_blank = L"Blank";
    const wchar_t* mode_exclude = L"Exclude from capture";
    const wchar_t* replacement_none = L"None";
    const wchar_t* replacement_solid = L"Solid";
    const wchar_t* replacement_label_kind = L"Label";
    const wchar_t* mark_none = L"Off";
    const wchar_t* mark_tiled = L"Tiled";
    const wchar_t* mark_center = L"Center";
    const wchar_t* mark_corner = L"Corner";
    const wchar_t* audience_both = L"Screen and capture";
    const wchar_t* audience_capture = L"Capture only";
    const wchar_t* taskbar_leave = L"Leave unchanged";
    const wchar_t* taskbar_hide = L"Hide";
    const wchar_t* x = L"X";
    const wchar_t* y = L"Y";
    const wchar_t* width = L"W";
    const wchar_t* height = L"H";
};

const RulesDialogStrings& default_rules_dialog_strings();

// Modal editor candidate. It is never invoked by the headless test runner; the
// platform integration calls it from a user action.  Cancel leaves output
// untouched; OK validates the whole draft first.
bool show_rules_dialog(HWND owner, const RulesDialogStrings& strings,
    const ProfileV4& input, ProfileV4& output);

}
