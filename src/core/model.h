// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace av {
enum class Theme { system, light, dark };
enum class Mode : unsigned { none = 0, blank = 1, exclude = 0x11 };
enum class Cover : unsigned { none, solid, label };
enum class Mark : unsigned { none, tiled, center, corner };
struct Window;
struct Effects {
    Cover cover = Cover::none;
    Mark mark = Mark::none;
    bool capture_only = false;
    std::wstring text = L"CONFIDENTIAL\n{id}  {time}", label = L"Protected content", tag;
    unsigned opacity = 45, font_size = 22, spacing = 240;
    unsigned foreground = 0xE0E0E0, background = 0x282828;
    // Hidden watermark (DCT=1, DWT=2).  The key is deliberately not part of
    // the persisted model; callers supply it for each session or file job.
    unsigned hidden_algorithm = 0;
    std::wstring hidden_payload;
    // Exact bytes loaded from schema 4.  Cleared when the user edits the text
    // field, so non-UTF-8 external payloads are never silently rewritten.
    std::vector<std::uint8_t> hidden_payload_bytes;
    // Capture-only route: 1 = display capture (A), 2 = controlled proxy
    // source window (B).  Screen-and-capture overlays ignore this value.
    unsigned capture_route = 1;
};
bool valid_effects(const Effects& effects);
bool has_effects(const Effects& effects);
bool operator==(const Effects& a,const Effects& b);
inline bool operator!=(const Effects& a,const Effects& b) { return !(a==b); }
Mode effective_mode(Mode mode, const Effects& effects);
std::wstring expand_mark(const std::wstring& input, const Window& window, const std::wstring& tag, const std::wstring& time);
struct Window {
    std::uintptr_t handle = 0;
    unsigned long pid = 0, tid = 0;
    std::uint64_t process_created = 0;
    std::wstring title, class_name, path, architecture;
    int x = 0, y = 0, width = 0, height = 0, show = 1;
    Mode mode = Mode::none;
    bool affinity_known = false;
};
struct Rule {
    std::wstring path, class_name, title;
    Mode mode = Mode::exclude;
    int x = 0, y = 0, width = 0, height = 0, show = 1;
    Effects effects;
};
struct Preferences {
    bool auto_refresh = false, self_protect = false;
    bool tray = true, close_to_tray = true, hide_taskbar = false;
    bool random_title = false, random_tray_icon = false, random_tooltip = false;
    bool auto_hide = false, notify_auto_hide = true, protect_on_start = false;
    unsigned refresh_ms = 1000, window_wait_ms = 3000;
    Mode auto_mode = Mode::exclude;
    std::vector<std::wstring> programs;
};
struct Profile {
    std::wstring language = L"auto";
    Theme theme = Theme::system;
    unsigned accent = 0xA66B37;
    std::vector<Rule> rules;
    Preferences preferences;
    Mode effect_mode = Mode::exclude;
    Effects effects;
};
struct BoolPreference { const wchar_t* key; bool Preferences::* member; };
inline constexpr BoolPreference bool_preferences[] = {
    {L"auto_refresh",&Preferences::auto_refresh}, {L"self_protect",&Preferences::self_protect},
    {L"tray",&Preferences::tray}, {L"close_to_tray",&Preferences::close_to_tray},
    {L"hide_taskbar",&Preferences::hide_taskbar}, {L"random_title",&Preferences::random_title},
    {L"random_tray_icon",&Preferences::random_tray_icon}, {L"random_tooltip",&Preferences::random_tooltip},
    {L"auto_hide",&Preferences::auto_hide}, {L"notify_auto_hide",&Preferences::notify_auto_hide},
    {L"protect_on_start",&Preferences::protect_on_start}
};
bool matches(const Rule& rule, const Window& window);
Rule remember(const Window& window, Mode mode);
bool valid_mode(unsigned value);
}
