// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/windows.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>

namespace av {
std::wstring executable_directory() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return {};
    path.resize(n);
    return path.substr(0, path.find_last_of(L"\\/"));
}
std::uint64_t process_created(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(p, &created, &exited, &kernel, &user);
    CloseHandle(p);
    return ok ? (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}
std::wstring architecture_of(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    using Wow64Process2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto wow2 = reinterpret_cast<Wow64Process2>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    USHORT machine = 0, native = 0;
    if (wow2 && wow2(p, &machine, &native)) {
        CloseHandle(p);
        const auto m = machine ? machine : native;
        if (m == IMAGE_FILE_MACHINE_I386) return L"x86";
        if (m == IMAGE_FILE_MACHINE_AMD64) return L"x64";
        if (m == IMAGE_FILE_MACHINE_ARM64) return L"arm64";
        return {};
    }
    BOOL wow = FALSE;
    const BOOL ok = IsWow64Process(p, &wow);
    CloseHandle(p);
    if (!ok) return {};
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (wow || si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return L"x86";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) return L"x64";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) return L"arm64";
    return {};
}
bool same_window(const Window& w) {
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(reinterpret_cast<HWND>(w.handle), &pid);
    return tid != 0 && tid == w.tid && pid == w.pid && w.process_created != 0 && process_created(pid) == w.process_created;
}
std::vector<Window> enumerate_windows(bool include_untitled) {
    std::vector<Window> windows;
    struct Enumeration { std::vector<Window>* windows; bool include_untitled; } enumeration{&windows,include_untitled};
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;
        Window w;
        w.handle = reinterpret_cast<std::uintptr_t>(hwnd);
        w.tid = GetWindowThreadProcessId(hwnd, &w.pid);
        const int length = GetWindowTextLengthW(hwnd);
        const auto* context=reinterpret_cast<const Enumeration*>(data);
        if (length > 32760 || (length<=0 && !context->include_untitled)) return TRUE;
        w.title.resize(static_cast<std::size_t>(length) + 1);
        w.title.resize(GetWindowTextW(hwnd, w.title.data(), length + 1));
        wchar_t cls[512]{};
        GetClassNameW(hwnd, cls, 512);
        w.class_name = cls;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, w.pid);
        if (p) {
            w.path.resize(32768);
            DWORD n = static_cast<DWORD>(w.path.size());
            if (QueryFullProcessImageNameW(p, 0, w.path.data(), &n)) w.path.resize(n);
            else w.path.clear();
            CloseHandle(p);
        }
        w.process_created = process_created(w.pid);
        w.architecture = architecture_of(w.pid);
        WINDOWPLACEMENT place{sizeof(place)};
        if (GetWindowPlacement(hwnd, &place)) {
            const auto r = place.rcNormalPosition;
            w.x = r.left; w.y = r.top; w.width = r.right - r.left; w.height = r.bottom - r.top;
            w.show = static_cast<int>(place.showCmd);
        }
        DWORD affinity = 0;
        w.affinity_known = GetWindowDisplayAffinity(hwnd, &affinity) != FALSE;
        if (valid_mode(affinity)) w.mode = static_cast<Mode>(affinity);
        else w.affinity_known = false;
        context->windows->push_back(std::move(w));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enumeration));
    return windows;
}
bool program_matches(const std::wstring& pattern,const Window& window) {
    if (pattern.empty() || window.path.empty()) return false;
    std::wstring expected=pattern, actual=window.path;
    std::replace(expected.begin(),expected.end(),L'/',L'\\');
    std::replace(actual.begin(),actual.end(),L'/',L'\\');
    if (expected.find_first_of(L"\\:")==std::wstring::npos) actual=actual.substr(actual.find_last_of(L'\\')+1);
    return CompareStringOrdinal(expected.c_str(),static_cast<int>(expected.size()),actual.c_str(),static_cast<int>(actual.size()),TRUE)==CSTR_EQUAL;
}
bool restore_geometry(const Window& w, const Rule& r, DWORD& error) {
    if (!same_window(w)) { error = ERROR_INVALID_WINDOW_HANDLE; return false; }
    if (r.width < 1 || r.height < 1 || r.width > 65535 || r.height > 65535) { error = ERROR_INVALID_DATA; return false; }
    WINDOWPLACEMENT p{sizeof(p)};
    if (!GetWindowPlacement(reinterpret_cast<HWND>(w.handle), &p)) { error = GetLastError(); return false; }
    p.rcNormalPosition = {r.x, r.y, r.x + r.width, r.y + r.height};
    p.showCmd = static_cast<UINT>(r.show);
    if (!SetWindowPlacement(reinterpret_cast<HWND>(w.handle), &p)) { error = GetLastError(); return false; }
    error = 0; return true;
}
std::wstring system_language() {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    return GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) ? locale : L"en";
}
std::wstring default_profile_path() {
    const auto portable = executable_directory() + L"\\portable.ini";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return executable_directory() + L"\\AkashaVeil.ini";
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata))) return {};
    std::wstring folder = std::wstring(appdata) + L"\\AkashaVeil";
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    return folder + L"\\profile.ini";
}
std::wstring error_text(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result = length ? std::wstring(buffer, length) : std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    return std::to_wstring(error) + L": " + result;
}
bool system_dark() {
    DWORD light = 1, size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && !light;
}
bool is_administrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr; BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &member); FreeSid(group);
    }
    return member != FALSE;
}
}
