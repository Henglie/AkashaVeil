// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/windows.h"

namespace av {
bool save_profile(const std::wstring& file, const Profile& profile, DWORD& error);
bool load_profile(const std::wstring& file, Profile& profile, DWORD& error);
bool export_windows(const std::wstring& file, const std::vector<Window>& windows, DWORD& error);
}
