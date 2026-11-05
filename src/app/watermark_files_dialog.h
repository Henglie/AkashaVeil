// SPDX-License-Identifier: Apache-2.0
// Native Win32 entry for the local image watermark tool. Compiled but not executed by the headless
// candidate tests; visible layout/dialogs still need a desktop lease.
#pragma once

#include <windows.h>
#include <string>

namespace av {
void show_watermark_files(HWND owner, const std::wstring& language, int theme_mode = 0,
    unsigned long accent = 0xFFFFFFFFul);
}
