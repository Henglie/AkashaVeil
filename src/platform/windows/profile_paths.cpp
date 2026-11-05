// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/profile_paths.h"
#include <shlobj.h>

namespace av {

namespace {

std::wstring join(const std::wstring& directory, const wchar_t* name) {
    if (directory.empty()) return name;
    if (directory.back() == L'\\' || directory.back() == L'/')
        return directory + name;
    return directory + L"\\" + name;
}

std::wstring executable_directory_local() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

bool has_nul(const std::wstring& value) {
    return value.find(L'\0') != std::wstring::npos;
}

}

std::wstring profile_parent_directory(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path.size() > 2 && path[1] == L':')
        return path.substr(0, 3);
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

bool probe_directory_writable(const std::wstring& directory, DWORD& error) {
    if (directory.empty() || has_nul(directory)) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        return false;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = ERROR_DIRECTORY;
        return false;
    }
    static unsigned counter = 0;
    const std::wstring probe = join(directory,
        (L"AkashaVeil-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64()) + L"-"
            + std::to_wstring(++counter) + L".tmp").c_str());
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    CloseHandle(file);
    DeleteFileW(probe.c_str());
    error = ERROR_SUCCESS;
    return true;
}

ProfilePathChoice choose_profile_path(const std::wstring& explicit_path,
    const std::wstring& executable_dir, const std::wstring& appdata_dir,
    bool portable_marker) {
    ProfilePathChoice choice;
    if (!explicit_path.empty()) {
        if (has_nul(explicit_path) || explicit_path.size() > 32767) {
            choice.error = ERROR_INVALID_PARAMETER;
            return choice;
        }
        choice.path = explicit_path;
        choice.source = ProfilePathSource::explicit_path;
        choice.ok = true;
        choice.error = ERROR_SUCCESS;
        return choice;
    }
    if (portable_marker) {
        if (executable_dir.empty() || has_nul(executable_dir)) {
            choice.error = ERROR_PATH_NOT_FOUND;
            return choice;
        }
        choice.path = join(executable_dir, L"AkashaVeil.ini");
        choice.source = ProfilePathSource::portable;
        choice.ok = true;
        choice.error = ERROR_SUCCESS;
        return choice;
    }
    if (!appdata_dir.empty() && !has_nul(appdata_dir)) {
        choice.path = join(join(appdata_dir, L"AkashaVeil"), L"profile.ini");
        choice.source = ProfilePathSource::appdata;
        choice.ok = true;
        choice.error = ERROR_SUCCESS;
        return choice;
    }
    choice.error = ERROR_PATH_NOT_FOUND;
    return choice;
}

ProfilePathInfo resolve_profile_path(const std::wstring& explicit_path,
    const std::wstring& executable_dir, const std::wstring& appdata_dir,
    bool portable_marker) {
    ProfilePathInfo info;
    const auto choice = choose_profile_path(explicit_path, executable_dir,
        appdata_dir, portable_marker);
    info.path = choice.path;
    info.source = choice.source;
    if (!choice.ok) {
        info.error = choice.error;
        return info;
    }
    wchar_t absolute[32768]{};
    const DWORD count = GetFullPathNameW(choice.path.c_str(), 32768, absolute, nullptr);
    if (!count || count >= 32768) {
        info.error = GetLastError();
        if (!info.error) info.error = ERROR_INVALID_PARAMETER;
        return info;
    }
    info.path = absolute;

    if (info.source == ProfilePathSource::appdata) {
        const auto folder = profile_parent_directory(info.path);
        if (!CreateDirectoryW(folder.c_str(), nullptr)
            && GetLastError() != ERROR_ALREADY_EXISTS) {
            info.error = GetLastError();
            return info;
        }
    }

    const auto parent = profile_parent_directory(info.path);
    const DWORD attributes = GetFileAttributesW(parent.c_str());
    info.parent_exists = attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!info.parent_exists) {
        info.error = attributes == INVALID_FILE_ATTRIBUTES
            ? GetLastError() : ERROR_DIRECTORY;
        return info;
    }
    DWORD probe_error = ERROR_SUCCESS;
    info.writable = probe_directory_writable(parent, probe_error);
    info.error = info.writable ? ERROR_SUCCESS : probe_error;
    return info;
}

ProfilePathInfo resolve_profile_path(const std::wstring& explicit_path) {
    wchar_t appdata[MAX_PATH]{};
    std::wstring appdata_dir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
        SHGFP_TYPE_CURRENT, appdata)))
        appdata_dir = appdata;
    const auto executable_dir = executable_directory_local();
    const std::wstring marker = join(executable_dir, L"portable.ini");
    const bool portable_marker =
        !marker.empty() && GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    return resolve_profile_path(explicit_path, executable_dir, appdata_dir,
        portable_marker);
}

}
