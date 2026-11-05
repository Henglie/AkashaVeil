// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <windows.h>

namespace av {

enum class ProfilePathSource : unsigned {
    none = 0,
    explicit_path = 1,
    portable = 2,
    appdata = 3
};

struct ProfilePathChoice {
    std::wstring path;
    ProfilePathSource source = ProfilePathSource::none;
    bool ok = false;
    DWORD error = ERROR_INVALID_PARAMETER;
};

// Pure decision used by tests and by the platform resolver.  Portable wins over
// AppData; an explicit --profile path wins over both.  A read-only portable
// directory is still selected so the caller can show an error and a Save As
// entry instead of silently moving the user's profile.
ProfilePathChoice choose_profile_path(const std::wstring& explicit_path,
    const std::wstring& executable_dir, const std::wstring& appdata_dir,
    bool portable_marker);

struct ProfilePathInfo {
    std::wstring path;
    ProfilePathSource source = ProfilePathSource::none;
    bool parent_exists = false;
    bool writable = false;
    DWORD error = ERROR_SUCCESS;
};

ProfilePathInfo resolve_profile_path(const std::wstring& explicit_path);
ProfilePathInfo resolve_profile_path(const std::wstring& explicit_path,
    const std::wstring& executable_dir, const std::wstring& appdata_dir,
    bool portable_marker);

bool probe_directory_writable(const std::wstring& directory, DWORD& error);
std::wstring profile_parent_directory(const std::wstring& path);

}
