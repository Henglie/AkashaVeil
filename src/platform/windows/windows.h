// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include "core/model.h"

namespace av {
std::wstring executable_directory();
std::wstring architecture_of(DWORD pid);
std::uint64_t process_created(DWORD pid);
bool same_window(const Window& window);
std::vector<Window> enumerate_windows(bool include_untitled=false);
bool program_matches(const std::wstring& pattern,const Window& window);
bool restore_geometry(const Window& window, const Rule& rule, DWORD& error);
std::wstring system_language();
std::wstring default_profile_path();
std::wstring error_text(DWORD error);
bool system_dark();
bool is_administrator();
}
