// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include "helper/protocol.h"
#include "version.h"

namespace {
constexpr DWORD max_helper = 8 * 1024 * 1024;
constexpr DWORD path_chars = 32768;

bool parse(const wchar_t* text, ULONGLONG& value) {
    value = 0;
    if (!*text) return false;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9') return false;
        const unsigned digit = static_cast<unsigned>(*text - L'0');
        if (value > 1844674407370955161ull || (value == 1844674407370955161ull && digit > 5)) return false;
        value = value * 10 + digit;
    }
    return true;
}

WORD own_machine() {
#if defined(_M_IX86)
    return IMAGE_FILE_MACHINE_I386;
#elif defined(_M_ARM64)
    return IMAGE_FILE_MACHINE_ARM64;
#else
    return IMAGE_FILE_MACHINE_AMD64;
#endif
}

bool file_machine(HANDLE file, WORD& machine) {
    machine = IMAGE_FILE_MACHINE_UNKNOWN;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    IMAGE_DOS_HEADER dos;
    DWORD read = 0;
    if (!ReadFile(file, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos)
        || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || dos.e_lfanew > 1048576) return false;
    LARGE_INTEGER offset;
    offset.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) return false;
    DWORD signature = 0;
    IMAGE_FILE_HEADER header;
    if (!ReadFile(file, &signature, sizeof(signature), &read, nullptr) || read != sizeof(signature)
        || signature != IMAGE_NT_SIGNATURE
        || !ReadFile(file, &header, sizeof(header), &read, nullptr) || read != sizeof(header)) return false;
    machine = header.Machine;
    return true;
}

bool same_parent(const wchar_t* a, const wchar_t* b) {
    const wchar_t* a_slash = nullptr;
    const wchar_t* b_slash = nullptr;
    for (const wchar_t* p = a; *p; ++p) if (*p == L'\\' || *p == L'/') a_slash = p;
    for (const wchar_t* p = b; *p; ++p) if (*p == L'\\' || *p == L'/') b_slash = p;
    if (!a_slash || !b_slash) return false;
    const int a_length = static_cast<int>(a_slash - a), b_length = static_cast<int>(b_slash - b);
    if (a_length != b_length) return false;
    return CompareStringOrdinal(a, a_length, b, b_length, TRUE) == CSTR_EQUAL;
}

// The caller owns `normalized` and `worker_path` heap buffers. No large stack
// arrays: /NODEFAULTLIB helper binaries have no CRT __chkstk/_memset.
bool validate_hook(const wchar_t* input, wchar_t* normalized, wchar_t* worker_path,
                   HANDLE& lock, DWORD& error) {
    lock = INVALID_HANDLE_VALUE;
    if (!input || !*input) { error = ERROR_INVALID_PARAMETER; return false; }
    const DWORD length = GetFullPathNameW(input, path_chars, normalized, nullptr);
    if (!length || length >= path_chars) { error = ERROR_BAD_PATHNAME; return false; }
    if (length <= 4 || CompareStringOrdinal(normalized + length - 4, 4, L".dll", 4, TRUE) != CSTR_EQUAL) {
        error = ERROR_BAD_EXE_FORMAT; return false;
    }
    const auto attributes = GetFileAttributesW(normalized);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        error = ERROR_BAD_PATHNAME; return false;
    }
    const DWORD worker_length = GetModuleFileNameW(nullptr, worker_path, path_chars);
    if (!worker_length || worker_length >= path_chars) { error = ERROR_BAD_PATHNAME; return false; }
    if (!same_parent(normalized, worker_path)) { error = ERROR_ACCESS_DENIED; return false; }
    lock = CreateFileW(normalized, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    LARGE_INTEGER size;
    WORD machine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!GetFileSizeEx(lock, &size) || size.QuadPart <= 0 || size.QuadPart > max_helper
        || !file_machine(lock, machine) || machine != own_machine()) {
        error = ERROR_BAD_EXE_FORMAT;
        CloseHandle(lock);
        lock = INVALID_HANDLE_VALUE;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool identity(HWND hwnd, DWORD pid, DWORD tid, HANDLE process, ULONGLONG expected) {
    DWORD actual_pid = 0;
    if (GetWindowThreadProcessId(hwnd, &actual_pid) != tid || actual_pid != pid || GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    return ((static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime) == expected;
}

DWORD apply(ULONGLONG* values, const wchar_t* hook_argument) {
    const ULONG_PTR pointer = static_cast<ULONG_PTR>(values[0]);
    if (static_cast<ULONGLONG>(pointer) != values[0] || values[1] > MAXDWORD || values[2] > MAXDWORD
        || (values[4] != 0 && values[4] != 1 && values[4] != 0x11)) return ERROR_INVALID_PARAMETER;
    const HWND hwnd = reinterpret_cast<HWND>(pointer);
    const DWORD pid = static_cast<DWORD>(values[1]), tid = static_cast<DWORD>(values[2]);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError();
    const bool valid = values[3] && identity(hwnd, pid, tid, process, values[3]);
    CloseHandle(process);
    if (!valid) return ERROR_INVALID_WINDOW_HANDLE;
    BOOL composition = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&composition)) || !composition) return ERROR_NOT_SUPPORTED;

    auto* hook_path = static_cast<wchar_t*>(LocalAlloc(LMEM_FIXED, path_chars * sizeof(wchar_t)));
    auto* worker_path = static_cast<wchar_t*>(LocalAlloc(LMEM_FIXED, path_chars * sizeof(wchar_t)));
    if (!hook_path || !worker_path) {
        if (hook_path) LocalFree(hook_path);
        if (worker_path) LocalFree(worker_path);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    HANDLE hook_lock = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_SUCCESS;
    if (!validate_hook(hook_argument, hook_path, worker_path, hook_lock, error)) {
        LocalFree(hook_path); LocalFree(worker_path);
        return error;
    }
    LocalFree(worker_path);
    HMODULE module = LoadLibraryExW(hook_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        error = GetLastError();
        CloseHandle(hook_lock);
        LocalFree(hook_path);
        return error;
    }
    const auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "AkashaVeilHook"));
    HHOOK hook = callback ? SetWindowsHookExW(WH_CALLWNDPROC, callback, module, tid) : nullptr;
    if (!hook) {
        error = GetLastError();
        FreeLibrary(module);
        CloseHandle(hook_lock);
        LocalFree(hook_path);
        return error;
    }
    const UINT message = RegisterWindowMessageW(AV_CAPTURE_MESSAGE);
    DWORD_PTR ignored = 0;
    SetLastError(ERROR_SUCCESS);
    const bool sent = message && SendMessageTimeoutW(hwnd, message, pid, static_cast<LPARAM>(values[4]),
                                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &ignored);
    error = sent ? ERROR_SUCCESS : GetLastError();
    if (!sent && !error) error = ERROR_TIMEOUT;
    UnhookWindowsHookEx(hook);
    FreeLibrary(module);
    CloseHandle(hook_lock);
    LocalFree(hook_path);
    if (error) return error;

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError();
    const bool still_valid = identity(hwnd, pid, tid, process, values[3]);
    CloseHandle(process);
    if (!still_valid) return ERROR_INVALID_WINDOW_HANDLE;
    DWORD actual = 0;
    if (!GetWindowDisplayAffinity(hwnd, &actual)) return GetLastError();
    return actual == values[4] || (values[4] == 0x11 && actual == 1) ? ERROR_SUCCESS : ERROR_INVALID_STATE;
}
}  // namespace

extern "C" void AvWorkerEntry() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD error = ERROR_INVALID_PARAMETER;
    // argv[1..5] identity/mode, argv[6] explicit verified hook path. The old
    // six-argument form is rejected so a missing path can never fall back to a
    // fixed DLL name in the worker directory.
    if (argv && argc == 7) {
        ULONGLONG values[5];
        bool good = true;
        for (int i = 0; i < 5; ++i) good = parse(argv[i + 1], values[i]) && good;
        if (good && argv[6] && *argv[6]) error = apply(values, argv[6]);
    }
    if (argv) LocalFree(argv);
    ExitProcess(error);
}
