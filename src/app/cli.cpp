// SPDX-License-Identifier: Apache-2.0
#include "app/cli.h"
#include "core/i18n.h"
#include "core/model.h"
#include "version.h"
#include "platform/windows/capture.h"
#include "platform/windows/profile.h"
#include "platform/windows/profile_v4.h"
#include "platform/windows/windows.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace av {
namespace {
struct Streams {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE), error = GetStdHandle(STD_ERROR_HANDLE);
    bool owns_output = false, owns_error = false, attached = false;
    ~Streams() {
        if (owns_output) CloseHandle(output);
        if (owns_error) CloseHandle(error);
        if (attached) FreeConsole();
    }
};
bool usable(HANDLE handle) {
    return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
}
HANDLE attach_console(Streams& streams) {
    if (!streams.attached) streams.attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    return streams.attached ? CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr) : INVALID_HANDLE_VALUE;
}
HANDLE stream(Streams& streams, bool error) {
    HANDLE& handle = error ? streams.error : streams.output;
    bool& owned = error ? streams.owns_error : streams.owns_output;
    if (usable(handle)) return handle;
    handle = attach_console(streams);
    owned = handle != INVALID_HANDLE_VALUE;
    return handle;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size < 1) return {};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
    return bytes;
}
void write(HANDLE handle, const std::wstring& text) {
    if (handle == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        const std::string bytes = utf8(text);
        DWORD written = 0;
        WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }
}
void line(Streams& streams, bool error, const std::wstring& text) {
    write(stream(streams, error), text + L"\r\n");
}
std::wstring format(const wchar_t* pattern, std::initializer_list<std::wstring> values) {
    std::wstring result = pattern ? pattern : L"";
    std::size_t index = 0;
    for (const auto& value : values) {
        const std::wstring marker = L"{" + std::to_wstring(index++) + L"}";
        std::size_t position = 0;
        while ((position = result.find(marker, position)) != std::wstring::npos) {
            result.replace(position, marker.size(), value);
            position += value.size();
        }
    }
    return result;
}
std::wstring trimmed(std::wstring text) {
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}
std::wstring handle_text(std::uintptr_t handle) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%0*llX", static_cast<int>(sizeof(std::uintptr_t) * 2),
        static_cast<unsigned long long>(handle));
    return buffer;
}
std::wstring target_text(std::uintptr_t handle, DWORD pid) {
    return L"HWND " + handle_text(handle) + L" (PID " + std::to_wstring(pid) + L")";
}
std::wstring target_text(const CommandLine& command) {
    if (command.target == CommandLineTarget::window) return L"HWND " + handle_text(command.window_handle);
    return L"PID " + std::to_wstring(command.process_id);
}
bool bind_window(HWND hwnd, Window& window, DWORD& error) {
    error = ERROR_SUCCESS;
    if (!hwnd || !IsWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) {
        error = ERROR_INVALID_WINDOW_HANDLE;
        return false;
    }
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || !pid) {
        error = ERROR_INVALID_WINDOW_HANDLE;
        return false;
    }
    const std::uint64_t created = process_created(pid);
    if (!created) {
        error = ERROR_ACCESS_DENIED;
        return false;
    }
    window = Window{};
    window.handle = reinterpret_cast<std::uintptr_t>(hwnd);
    window.pid = pid;
    window.tid = tid;
    window.process_created = created;
    return true;
}
Text error_key(CommandLineError error) {
    switch (error) {
    case CommandLineError::unknown_option: return Text::cli_err_unknown_option;
    case CommandLineError::missing_value: return Text::cli_err_missing_value;
    case CommandLineError::invalid_process: return Text::cli_err_invalid_process;
    case CommandLineError::invalid_window: return Text::cli_err_invalid_window;
    case CommandLineError::duplicate_option: return Text::cli_err_duplicate_option;
    case CommandLineError::conflicting_targets: return Text::cli_err_conflicting_targets;
    case CommandLineError::missing_target: return Text::cli_err_missing_target;
    case CommandLineError::missing_command: return Text::cli_err_missing_command;
    case CommandLineError::unexpected_argument: return Text::cli_err_unexpected_argument;
    case CommandLineError::empty_profile: return Text::cli_err_empty_profile;
    default: return Text::cli_err_unexpected_argument;
    }
}
std::wstring error_message(const CommandLineResult& result, const std::wstring& language) {
    const wchar_t* pattern = text(error_key(result.error), language);
    switch (result.error) {
    case CommandLineError::unknown_option:
    case CommandLineError::missing_value:
    case CommandLineError::invalid_process:
    case CommandLineError::invalid_window:
    case CommandLineError::duplicate_option:
    case CommandLineError::unexpected_argument:
        return format(pattern, {result.token});
    default:
        return pattern;
    }
}
struct Target {
    Window window;
    std::uintptr_t handle = 0;
    DWORD pid = 0;
    DWORD bind_error = 0;
    bool bound = false;
};
std::wstring json_escape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 2);
    for (const wchar_t c : value) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\b': out += L"\\b"; break;
        case L'\f': out += L"\\f"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20) { wchar_t buffer[8]{}; swprintf_s(buffer, L"\\u%04X", static_cast<unsigned>(c)); out += buffer; }
            else out += c;
        }
    }
    return out;
}
const wchar_t* mode_token(const Window& window) {
    if (!window.affinity_known) return L"unknown";
    if (window.mode == Mode::exclude) return L"protected";
    if (window.mode == Mode::blank) return L"transparent";
    return L"normal";
}
int list_windows(const CommandLine& command) {
    Streams streams;
    const auto windows = enumerate_windows(true);
    if (command.json) {
        std::wstring json = L"[\n";
        for (std::size_t i = 0; i < windows.size(); ++i) {
            const auto& window = windows[i];
            json += L"  {\"handle\":\"" + handle_text(window.handle)
                + L"\",\"pid\":" + std::to_wstring(window.pid)
                + L",\"tid\":" + std::to_wstring(window.tid)
                + L",\"title\":\"" + json_escape(window.title)
                + L"\",\"class_name\":\"" + json_escape(window.class_name)
                + L"\",\"path\":\"" + json_escape(window.path)
                + L"\",\"architecture\":\"" + json_escape(window.architecture)
                + L"\",\"mode\":\"" + mode_token(window) + L"\"}";
            json += (i + 1 < windows.size()) ? L",\n" : L"\n";
        }
        write(stream(streams, false), json + L"]\r\n");
    } else {
        for (const auto& window : windows)
            line(streams, false, handle_text(window.handle) + L"\t" + std::to_wstring(window.pid)
                + L"\t" + mode_token(window) + L"\t" + window.title);
    }
    return 0;
}
}
int report_command_line_error(const CommandLineResult& result) {
    Streams streams;
    const std::wstring language = choose_language(L"auto", system_language());
    line(streams, true, std::wstring(text(Text::cli_error_prefix, language)) + error_message(result, language));
    line(streams, true, text(Text::cli_try_help, language));
    return 2;
}
int run_command_line(const CommandLine& command) {
    Streams streams;
    if (command.action == CommandLineAction::help) {
        write(stream(streams, false), std::wstring(text(Text::cli_usage, choose_language(L"auto", system_language()))) + L"\r\n");
        return 0;
    }
    if (command.action == CommandLineAction::version) {
        if (command.json)
            write(stream(streams, false), std::wstring(L"{\"name\":\"AkashaVeil\",\"version\":\"") + AV_VERSION
                + L"\",\"architecture\":\"" + AV_ARCH + L"\"}\r\n");
        else
            write(stream(streams, false), std::wstring(L"AkashaVeil " AV_VERSION L" (" AV_ARCH L")\r\n"));
        return 0;
    }
    if (command.action == CommandLineAction::list) return list_windows(command);
    const bool explicit_profile = !command.profile_path.empty();
    const std::wstring profile_path = explicit_profile ? command.profile_path : default_profile_path();
    ProfileV4 profile;
    DWORD profile_error = ERROR_FILE_NOT_FOUND;
    bool loaded = false;
    if (!profile_path.empty() && GetFileAttributesW(profile_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto result = load_profile_v4(profile_path, profile);
        loaded = result.ok();
        profile_error = result.error;
    }
    const std::wstring language = choose_language(loaded ? profile.language : std::wstring(L"auto"), system_language());
    if (explicit_profile && !loaded) {
        line(streams, true, format(text(Text::cli_profile_warning, language), {profile_path, trimmed(error_text(profile_error))}));
    }
    const Mode mode = command.action == CommandLineAction::hide ? Mode::exclude : Mode::none;
    std::vector<Target> targets;
    if (command.target == CommandLineTarget::window) {
        const HWND hwnd = reinterpret_cast<HWND>(command.window_handle);
        if (!hwnd || !IsWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) {
            line(streams, true, format(text(Text::cli_no_target, language), {target_text(command)}));
            return 3;
        }
        Target target;
        target.handle = command.window_handle;
        target.window.handle = command.window_handle;
        GetWindowThreadProcessId(hwnd, &target.pid);
        target.bound = bind_window(hwnd, target.window, target.bind_error);
        targets.push_back(target);
    } else {
        for (const auto& window : enumerate_windows(true)) {
            if (window.pid != command.process_id) continue;
            Target target;
            target.handle = window.handle;
            target.pid = window.pid;
            target.bound = bind_window(reinterpret_cast<HWND>(window.handle), target.window, target.bind_error);
            targets.push_back(target);
        }
        if (targets.empty()) {
            line(streams, true, format(text(Text::cli_no_target, language), {target_text(command)}));
            return 3;
        }
    }
    unsigned succeeded = 0, failed = 0, timeouts = 0;
    std::vector<std::wstring> records;
    for (const auto& target : targets) {
        const std::wstring label = target_text(target.handle, target.pid);
        const std::wstring record_prefix = L"{\"handle\":\"" + handle_text(target.handle)
            + L"\",\"pid\":" + std::to_wstring(target.pid) + L",\"ok\":";
        if (!target.bound) {
            ++failed;
            if (target.bind_error == ERROR_TIMEOUT) ++timeouts;
            if (command.json)
                records.push_back(record_prefix + L"false,\"error\":\"" + json_escape(trimmed(error_text(target.bind_error))) + L"\"}");
            else
                line(streams, true, format(text(Text::cli_target_failed, language), {label, trimmed(error_text(target.bind_error))}));
            continue;
        }
        DWORD error = ERROR_SUCCESS;
        if (apply_affinity(target.window, mode, error)) {
            ++succeeded;
            if (command.json) records.push_back(record_prefix + L"true,\"error\":\"\"}");
            else
                line(streams, false, format(text(command.action == CommandLineAction::hide ? Text::cli_success_hide : Text::cli_success_unhide,
                    language), {label}));
        } else {
            ++failed;
            if (error == ERROR_TIMEOUT) ++timeouts;
            if (command.json)
                records.push_back(record_prefix + L"false,\"error\":\"" + json_escape(trimmed(error_text(error))) + L"\"}");
            else
                line(streams, true, format(text(Text::cli_target_failed, language), {label, trimmed(error_text(error))}));
        }
    }
    if (command.json) {
        std::wstring json = L"{\"action\":\"";
        json += command.action == CommandLineAction::hide ? L"protect" : L"restore";
        json += L"\",\"succeeded\":" + std::to_wstring(succeeded) + L",\"failed\":" + std::to_wstring(failed)
            + L",\"timeouts\":" + std::to_wstring(timeouts) + L",\"targets\":[";
        for (std::size_t i = 0; i < records.size(); ++i) json += (i ? L"," : L"") + records[i];
        json += L"]}";
        line(streams, false, json);
    } else {
        line(streams, false, format(text(Text::cli_summary, language), {std::to_wstring(succeeded), std::to_wstring(failed)}));
    }
    if (!succeeded) return timeouts == failed ? 6 : 5;
    return failed ? 4 : 0;
}
}
