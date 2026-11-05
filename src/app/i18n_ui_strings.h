// SPDX-License-Identifier: Apache-2.0
// Localized label adapters for the dialog structs.
#pragma once

#include "app/match_choice.h"
#include "app/profile_file_dialog.h"
#include "app/rules_dialog.h"
#include "core/i18n.h"

#include <string>

namespace av {

inline RulesDialogStrings rules_dialog_strings(const std::wstring& language) {
    RulesDialogStrings s;
    s.title = text(Text::rules_title, language);
    s.path = text(Text::rules_path, language);
    s.class_name = text(Text::rules_class_name, language);
    s.window_title = text(Text::rules_window_title, language);
    s.enabled = text(Text::rules_enabled, language);
    s.mode = text(Text::rules_mode, language);
    s.replacement = text(Text::rules_replacement, language);
    s.visible = text(Text::rules_visible, language);
    s.audience = text(Text::rules_audience, language);
    s.taskbar = text(Text::rules_taskbar, language);
    s.geometry = text(Text::rules_geometry, language);
    s.show = text(Text::rules_show, language);
    s.opacity = text(Text::rules_opacity, language);
    s.font_size = text(Text::rules_font_size, language);
    s.spacing = text(Text::rules_spacing, language);
    s.foreground = text(Text::rules_foreground, language);
    s.background = text(Text::rules_background, language);
    s.text = text(Text::rules_text, language);
    s.label = text(Text::rules_label, language);
    s.tag = text(Text::rules_tag, language);
    s.replacement_label = text(Text::rules_replacement_label, language);
    s.add = text(Text::rules_add, language);
    s.update = text(Text::rules_update, language);
    s.remove = text(Text::rules_remove, language);
    s.toggle = text(Text::rules_toggle, language);
    s.ok = text(Text::ok, language);
    s.cancel = text(Text::cancel, language);
    s.invalid = text(Text::rules_invalid, language);
    s.duplicate = text(Text::rules_duplicate, language);
    s.no_selection = text(Text::rules_no_selection, language);
    s.mode_none = text(Text::rules_mode_none, language);
    s.mode_blank = text(Text::rules_mode_blank, language);
    s.mode_exclude = text(Text::rules_mode_exclude, language);
    s.replacement_none = text(Text::rules_replacement_none, language);
    s.replacement_solid = text(Text::rules_replacement_solid, language);
    s.replacement_label_kind = text(Text::rules_replacement_label_kind, language);
    s.mark_none = text(Text::rules_mark_none, language);
    s.mark_tiled = text(Text::rules_mark_tiled, language);
    s.mark_center = text(Text::rules_mark_center, language);
    s.mark_corner = text(Text::rules_mark_corner, language);
    s.audience_both = text(Text::rules_audience_both, language);
    s.audience_capture = text(Text::rules_audience_capture, language);
    s.taskbar_leave = text(Text::rules_taskbar_leave, language);
    s.taskbar_hide = text(Text::rules_taskbar_hide, language);
    s.x = text(Text::rules_x, language);
    s.y = text(Text::rules_y, language);
    s.width = text(Text::rules_width, language);
    s.height = text(Text::rules_height, language);
    return s;
}

inline MatchChoiceStrings match_choice_strings(const std::wstring& language) {
    MatchChoiceStrings s;
    s.title = text(Text::match_choice_title, language);
    s.prompt = text(Text::match_choice_prompt, language);
    s.ok = text(Text::match_choice_use_selected, language);
    s.cancel = text(Text::cancel, language);
    s.no_selection = text(Text::match_choice_no_selection, language);
    return s;
}

inline ProfileFileDialogStrings profile_file_dialog_strings(const std::wstring& language) {
    ProfileFileDialogStrings s;
    s.profile_filter = text(Text::profile_filter, language);
    s.csv_filter = text(Text::csv_filter, language);
    s.profile_extension = L"ini";
    s.csv_extension = L"csv";
    s.open_title = text(Text::profile_open_title, language);
    s.save_title = text(Text::profile_save_title, language);
    s.export_title = text(Text::csv_export_title, language);
    return s;
}

} // namespace av
