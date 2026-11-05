// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/payload.h"
#include "payload_manifest.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <map>

extern "C" BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);

namespace av {
namespace {
constexpr std::size_t max_payload = 8 * 1024 * 1024;
SRWLOCK session_lock = SRWLOCK_INIT;
std::map<std::wstring, std::wstring> session_paths;
std::map<std::wstring, std::wstring> session_files;  // full path -> architecture
std::atomic_bool cache_cleaned{false};
PayloadPolicy configured_policy;
bool configured = false;

bool environment_bool(const wchar_t* name, bool fallback, bool& value) {
    wchar_t text[32]{};
    const DWORD length = GetEnvironmentVariableW(name, text, 32);
    if (!length) { value = fallback; return true; }
    if (length >= 32) return false;
    if (!lstrcmpiW(text, L"0") || !lstrcmpiW(text, L"false") || !lstrcmpiW(text, L"off")) { value = false; return true; }
    if (!lstrcmpiW(text, L"1") || !lstrcmpiW(text, L"true") || !lstrcmpiW(text, L"on")) { value = true; return true; }
    return false;
}

unsigned environment_number(const wchar_t* name, unsigned fallback, unsigned minimum, unsigned maximum) {
    wchar_t text[32]{};
    const DWORD length = GetEnvironmentVariableW(name, text, 32);
    if (!length || length >= 32) return fallback;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (!*text || !end || *end || value < minimum || value > maximum) return fallback;
    return static_cast<unsigned>(value);
}

PayloadPolicy load_policy() {
    PayloadPolicy policy;
    bool random = true;
    if (!environment_bool(L"AKASHAVEIL_PAYLOAD_RANDOM_NAMES", true, random)) random = true;
    policy.randomize_names = random;
    policy.stale_days = environment_number(L"AKASHAVEIL_PAYLOAD_STALE_DAYS", 7, 0, 3650);
    policy.cleanup_items = environment_number(L"AKASHAVEIL_PAYLOAD_CLEANUP_ITEMS", 64, 1, 1024);
    return policy;
}

bool directory(const std::wstring& path, DWORD& error) {
    if (path.empty() || path.size() >= 32700) { error = ERROR_BAD_PATHNAME; return false; }
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) return false;
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = ERROR_BAD_PATHNAME; return false;
    }
    return true;
}

std::wstring trim_root(const std::wstring& value) {
    std::wstring result = value;
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

bool cache_base(std::wstring& base, DWORD& error) {
    wchar_t root[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"AKASHAVEIL_RUNTIME_DIR", root, 32768);
    if (length) {
        if (length >= 32768) { error = ERROR_BAD_PATHNAME; return false; }
        base = trim_root(root);
    } else {
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, root))) { error = ERROR_PATH_NOT_FOUND; return false; }
        base = std::wstring(root) + L"\\AkashaVeil";
        if (!directory(base, error)) return false;
        base += L"\\Runtime";
    }
    if (base.empty() || base.size() > 32000) { error = ERROR_BAD_PATHNAME; return false; }
    return directory(base, error);
}

std::size_t architecture_index(const std::wstring& architecture) {
    if (architecture == L"x86") return 1;
    if (architecture == L"x64") return 2;
    if (architecture == L"arm64") return 3;
    return 0;
}

WORD architecture_machine(const std::wstring& architecture) {
    if (architecture == L"x86") return IMAGE_FILE_MACHINE_I386;
    if (architecture == L"x64") return IMAGE_FILE_MACHINE_AMD64;
    if (architecture == L"arm64") return IMAGE_FILE_MACHINE_ARM64;
    return IMAGE_FILE_MACHINE_UNKNOWN;
}

bool pe_machine(const BYTE* bytes, DWORD size, WORD& machine) {
    machine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!bytes || size < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(IMAGE_DOS_HEADER) || static_cast<DWORD>(dos.e_lfanew) > size - 6) return false;
    const DWORD offset = static_cast<DWORD>(dos.e_lfanew);
    DWORD signature = 0;
    std::memcpy(&signature, bytes + offset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE || offset > size - sizeof(DWORD) - sizeof(IMAGE_FILE_HEADER)) return false;
    IMAGE_FILE_HEADER header{};
    std::memcpy(&header, bytes + offset + sizeof(DWORD), sizeof(header));
    machine = header.Machine;
    return true;
}

bool same_contents(HANDLE file, const BYTE* expected, DWORD size, WORD machine, DWORD& error) {
    LARGE_INTEGER actual{};
    if (!GetFileSizeEx(file, &actual)) { error = GetLastError(); return false; }
    if (actual.QuadPart != size) { error = ERROR_INVALID_DATA; return false; }
    BYTE buffer[4096];
    DWORD offset = 0;
    while (offset < size) {
        DWORD read = 0;
        const DWORD count = std::min<DWORD>(sizeof(buffer), size - offset);
        if (!ReadFile(file, buffer, count, &read, nullptr) || read != count || std::memcmp(buffer, expected + offset, count)) {
            error = ERROR_INVALID_DATA; return false;
        }
        offset += count;
    }
    WORD actual_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!pe_machine(expected, size, actual_machine) || actual_machine != machine) { error = ERROR_BAD_EXE_FORMAT; return false; }
    error = ERROR_SUCCESS;
    return true;
}

std::wstring long_path(const std::wstring& path) {
    if (path.size() >= 248 && path.compare(0, 4, L"\\\\?\\") != 0) return L"\\\\?\\" + path;
    return path;
}

bool move_aside(const std::wstring& path, std::wstring& backup, DWORD& error) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(path.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }
    for (unsigned attempt = 0; attempt < 80; ++attempt) {
        wchar_t token[16]{};
        swprintf_s(token, L"%08x", static_cast<unsigned>((GetCurrentProcessId() * 2654435761u) ^ static_cast<unsigned>(GetTickCount64()) ^ attempt));
        backup = path + L".bad-" + token;
        if (MoveFileW(long_path(path).c_str(), long_path(backup).c_str())) { error = ERROR_SUCCESS; return true; }
        error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) { Sleep(20); continue; }
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return false;
    }
    return false;
}

bool materialize(UINT id, const std::wstring& path, WORD expected_machine, HANDLE& lock, DWORD& error) {
    const HMODULE self = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(self, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) { error = ERROR_RESOURCE_DATA_NOT_FOUND; return false; }
    const auto bytes = static_cast<const BYTE*>(LockResource(LoadResource(self, resource)));
    const DWORD size = SizeofResource(self, resource);
    if (!bytes || !size || size > max_payload) { error = ERROR_INVALID_DATA; return false; }
    WORD embedded_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!pe_machine(bytes, size, embedded_machine) || embedded_machine != expected_machine) { error = ERROR_BAD_EXE_FORMAT; return false; }
    lock = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 80; ++attempt) {
        HANDLE existing = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (existing != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(existing, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
                CloseHandle(existing); error = ERROR_BAD_PATHNAME; return false;
            }
            if (same_contents(existing, bytes, size, expected_machine, error)) { lock = existing; error = ERROR_SUCCESS; return true; }
            CloseHandle(existing);
            std::wstring backup;
            if (!move_aside(path, backup, error)) return false;
            continue;
        }
        error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION) { Sleep(20); continue; }
        if (error != ERROR_FILE_NOT_FOUND) return false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_SHARING_VIOLATION) { Sleep(20); continue; }
            return false;
        }
        DWORD written = 0;
        const bool good = WriteFile(file, bytes, size, &written, nullptr) && written == size && FlushFileBuffers(file);
        error = good ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!good) return false;
    }
    error = ERROR_TIMEOUT;
    return false;
}

bool random_token(std::wstring& token) {
    BYTE bytes[16]{};
    if (!SystemFunction036(bytes, static_cast<ULONG>(sizeof(bytes)))) return false;
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    token.clear();
    token.reserve(sizeof(bytes) * 2);
    for (const BYTE byte : bytes) { token.push_back(hex[byte >> 4]); token.push_back(hex[byte & 15]); }
    return true;
}

bool session_name(const std::wstring& architecture, bool hook, bool randomize, std::wstring& name, DWORD& error) {
    const std::wstring key = architecture + (hook ? L"|hook|" : L"|worker|") + (randomize ? L"random" : L"fixed");
    AcquireSRWLockExclusive(&session_lock);
    const auto found = session_paths.find(key);
    if (found != session_paths.end()) { name = found->second; ReleaseSRWLockExclusive(&session_lock); error = ERROR_SUCCESS; return true; }
    std::wstring generated;
    if (randomize) {
        std::wstring token;
        if (!random_token(token)) { ReleaseSRWLockExclusive(&session_lock); error = ERROR_NO_SYSTEM_RESOURCES; return false; }
        generated = L"av-" + architecture + L"-" + token + (hook ? L".dll" : L".exe");
    } else {
        generated = hook ? L"AkashaVeilHook-" + architecture + L".dll" : L"AkashaVeilWorker.exe";
    }
    session_paths.emplace(key, generated);
    ReleaseSRWLockExclusive(&session_lock);
    name = std::move(generated);
    error = ERROR_SUCCESS;
    return true;
}

bool generated_name(const std::wstring& name, const std::wstring& architecture) {
    const std::wstring prefix = L"av-" + architecture + L"-";
    if (name.size() < prefix.size() + 32 + 4 || name.compare(0, prefix.size(), prefix) != 0) return false;
    std::size_t offset = prefix.size();
    for (unsigned i = 0; i < 32; ++i, ++offset) {
        const wchar_t c = name[offset];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    const std::wstring suffix = name.substr(offset);
    const std::wstring extensions[] = {L".dll", L".exe"};
    for (const auto& extension : extensions) {
        if (suffix == extension || (suffix.size() > extension.size() && suffix.compare(0, extension.size(), extension) == 0 && suffix.compare(extension.size(), 1, L".") == 0)) return true;
    }
    return false;
}

bool older_than(const FILETIME& time, const FILETIME& cutoff) {
    const ULARGE_INTEGER a{{time.dwLowDateTime, time.dwHighDateTime}}, b{{cutoff.dwLowDateTime, cutoff.dwHighDateTime}};
    return a.QuadPart < b.QuadPart;
}

void recycle_one(const std::wstring& path, const std::wstring& architecture, PayloadCleanupReport& report, unsigned& processed) {
    const auto found = path.find_last_of(L"\\/");
    if (found == std::wstring::npos || !generated_name(path.substr(found + 1), architecture)) { ++report.retained; return; }
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { ++report.retained; return; }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto policy = payload_policy();
    ULARGE_INTEGER cutoff_value{{now.dwLowDateTime, now.dwHighDateTime}};
    cutoff_value.QuadPart -= static_cast<ULONGLONG>(policy.stale_days) * 24 * 60 * 60 * 10000000ull;
    FILETIME cutoff{cutoff_value.LowPart, cutoff_value.HighPart};
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) || !older_than(info.ftLastWriteTime, cutoff)) { ++report.retained; return; }
    // A live payload keeps a read lock without write/delete sharing. Only
    // reclaim entries we can prove are not in use.
    HANDLE probe = CreateFileW(path.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) { ++report.retained; return; }
        if (error != ERROR_FILE_NOT_FOUND) { report.last_error = error; ++report.failed; }
        return;
    }
    CloseHandle(probe);
    if (!policy.stale_days) { ++report.retained; return; }
    ++processed;
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&operation);
    if (result == 0 && !operation.fAnyOperationsAborted) ++report.recycled;
    else { report.last_error = operation.fAnyOperationsAborted ? ERROR_CANCELLED : static_cast<DWORD>(result); ++report.failed; }
}
} // namespace

PayloadPolicy payload_policy() {
    AcquireSRWLockExclusive(&session_lock);
    if (!configured) { configured_policy = load_policy(); configured = true; }
    const PayloadPolicy policy = configured_policy;
    ReleaseSRWLockExclusive(&session_lock);
    return policy;
}

void set_payload_policy(const PayloadPolicy& policy) {
    AcquireSRWLockExclusive(&session_lock);
    session_files.clear();
    configured_policy = policy;
    if (configured_policy.stale_days > 3650) configured_policy.stale_days = 3650;
    if (!configured_policy.cleanup_items || configured_policy.cleanup_items > 1024) configured_policy.cleanup_items = 64;
    configured = true;
    session_paths.clear();
    ReleaseSRWLockExclusive(&session_lock);
}

void regenerate_payload_session() {
    AcquireSRWLockExclusive(&session_lock);
    session_paths.clear();
    session_files.clear();
    ReleaseSRWLockExclusive(&session_lock);
}

PayloadCleanupReport cleanup_payload_cache(const std::wstring& architecture, unsigned max_items) {
    PayloadCleanupReport report;
    if (!architecture_index(architecture)) { report.last_error = ERROR_BAD_EXE_FORMAT; return report; }
    std::wstring base;
    DWORD error = ERROR_SUCCESS;
    if (!cache_base(base, error)) { report.last_error = error; return report; }
    const std::wstring directory_path = base + L"\\" AV_PAYLOAD_ID L"\\" + architecture;
    if (GetFileAttributesW(directory_path.c_str()) == INVALID_FILE_ATTRIBUTES) return report;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory_path + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) { report.last_error = GetLastError(); return report; }
    unsigned processed = 0;
    const unsigned limit = max_items ? max_items : 64;
    do {
        if (data.cFileName[0] == L'.' && (!data.cFileName[1] || (data.cFileName[1] == L'.' && !data.cFileName[2]))) continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        ++report.scanned;
        if (processed >= limit) continue;
        recycle_one(directory_path + L"\\" + data.cFileName, architecture, report, processed);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return report;
}

void record_session_file(const std::wstring& path, const std::wstring& architecture) {
    AcquireSRWLockExclusive(&session_lock);
    session_files[path] = architecture;
    ReleaseSRWLockExclusive(&session_lock);
}

bool prepare_payload(const std::wstring& architecture, bool need_worker, Payload& payload, DWORD& error) {
    const std::size_t index = architecture_index(architecture);
    if (!index) { error = ERROR_BAD_EXE_FORMAT; return false; }
    const WORD machine = architecture_machine(architecture);
    if (machine == IMAGE_FILE_MACHINE_UNKNOWN) { error = ERROR_BAD_EXE_FORMAT; return false; }
    std::wstring base;
    if (!cache_base(base, error)) return false;
    base += L"\\" AV_PAYLOAD_ID;
    if (!directory(base, error)) return false;
    base += L"\\" + architecture;
    if (!directory(base, error)) return false;
    const PayloadPolicy policy = payload_policy();
    if (!cache_cleaned.exchange(true)) cleanup_payload_cache(architecture, policy.cleanup_items);
    std::wstring hook_name;
    if (!session_name(architecture, true, policy.randomize_names, hook_name, error)) return false;
    payload.directory = base;
    payload.hook_path = base + L"\\" + hook_name;
    if (!materialize(200u + static_cast<UINT>(index), payload.hook_path, machine, payload.hook, error)) return false;
    record_session_file(payload.hook_path, architecture);
    if (need_worker) {
        std::wstring worker_name;
        if (!session_name(architecture, false, policy.randomize_names, worker_name, error)) return false;
        payload.worker_path = base + L"\\" + worker_name;
        if (!materialize(100u + static_cast<UINT>(index), payload.worker_path, machine, payload.worker, error)) return false;
        record_session_file(payload.worker_path, architecture);
    }
    error = ERROR_SUCCESS;
    return true;
}

namespace {
void recycle_session_one(const std::wstring& path, const std::wstring& architecture, PayloadCleanupReport& report) {
    const auto found = path.find_last_of(L"\\/");
    if (found == std::wstring::npos || !generated_name(path.substr(found + 1), architecture)) { ++report.retained; return; }
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        ++report.retained; return;
    }
    HANDLE probe = CreateFileW(path.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_FILE_NOT_FOUND) {
            ++report.retained; return;
        }
        report.last_error = error; ++report.failed; return;
    }
    CloseHandle(probe);
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&operation);
    if (result == 0 && !operation.fAnyOperationsAborted) ++report.recycled;
    else { report.last_error = operation.fAnyOperationsAborted ? ERROR_CANCELLED : static_cast<DWORD>(result); ++report.failed; }
}
}  // namespace

PayloadCleanupReport cleanup_session_payloads() {
    PayloadCleanupReport report;
    std::map<std::wstring, std::wstring> files;
    AcquireSRWLockExclusive(&session_lock);
    files.swap(session_files);
    ReleaseSRWLockExclusive(&session_lock);
    for (const auto& entry : files) {
        ++report.scanned;
        recycle_session_one(entry.first, entry.second, report);
    }
    return report;
}
}
