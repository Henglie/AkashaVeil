// SPDX-License-Identifier: Apache-2.0
#include "core/command_line.h"
#include <cwctype>
#include <limits>

namespace av {
namespace {
bool equals(const std::wstring& value, const wchar_t* expected) {
    std::size_t i = 0;
    for (; i < value.size() && expected[i]; ++i) {
        if (std::towlower(value[i]) != std::towlower(expected[i])) return false;
    }
    return i == value.size() && !expected[i];
}
bool is_digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
int hex_value(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
bool decimal(const std::wstring& text, std::uint32_t& value) {
    if (text.empty()) return false;
    std::uint64_t result = 0;
    for (const wchar_t c : text) {
        if (!is_digit(c)) return false;
        result = result * 10 + static_cast<unsigned>(c - L'0');
        if (result > 0xFFFFFFFFull) return false;
    }
    if (!result) return false;
    value = static_cast<std::uint32_t>(result);
    return true;
}
bool hexadecimal(const std::wstring& text, std::uintptr_t& value) {
    std::size_t offset = 0;
    if (text.size() >= 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) offset = 2;
    if (offset == text.size()) return false;
    std::uint64_t result = 0;
    for (std::size_t i = offset; i < text.size(); ++i) {
        const int digit = hex_value(text[i]);
        if (digit < 0) return false;
        if (result > (std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(digit)) / 16) return false;
        result = result * 16 + static_cast<unsigned>(digit);
    }
    if (!result || result > std::numeric_limits<std::uintptr_t>::max()) return false;
    value = static_cast<std::uintptr_t>(result);
    return true;
}
bool is_option(const std::wstring& text) { return text.size() > 1 && text[0] == L'-'; }
CommandLineResult failure(CommandLineError error, const std::wstring& token = {}) {
    CommandLineResult result;
    result.error = error;
    result.token = token;
    return result;
}
}
CommandLineResult parse_command_line(const std::vector<std::wstring>& arguments) {
    CommandLineResult result;
    if (arguments.empty()) return result;
    if (arguments.size() == 1 && (equals(arguments[0], L"help") || equals(arguments[0], L"--help")
        || equals(arguments[0], L"-h") || equals(arguments[0], L"-?") || equals(arguments[0], L"/?"))) {
        result.command.action = CommandLineAction::help;
        return result;
    }
    if (arguments.size() == 1 && (equals(arguments[0], L"--version") || equals(arguments[0], L"-v")
        || equals(arguments[0], L"version"))) {
        result.command.action = CommandLineAction::version;
        return result;
    }
    bool process_seen = false, window_seen = false, profile_seen = false, json_seen = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::wstring& argument = arguments[i];
        const bool hide = equals(argument, L"hide") || equals(argument, L"protect") || equals(argument, L"exclude");
        const bool unhide = equals(argument, L"unhide") || equals(argument, L"restore");
        const bool list = equals(argument, L"list") || equals(argument, L"windows");
        const bool version = equals(argument, L"--version") || equals(argument, L"-v") || equals(argument, L"version");
        if (hide || unhide || list || version) {
            if (result.command.action != CommandLineAction::gui) return failure(CommandLineError::unexpected_argument, argument);
            result.command.action = hide ? CommandLineAction::hide
                : unhide ? CommandLineAction::unhide
                : list ? CommandLineAction::list : CommandLineAction::version;
            continue;
        }
        if (equals(argument, L"--json")) {
            if (json_seen) return failure(CommandLineError::duplicate_option, argument);
            json_seen = true;
            result.command.json = true;
            continue;
        }
        const bool process_option = equals(argument, L"--process") || equals(argument, L"--pid");
        const bool window_option = equals(argument, L"--window") || equals(argument, L"--hwnd");
        const bool profile_option = equals(argument, L"--profile");
        if (!process_option && !window_option && !profile_option) {
            if (is_option(argument)) return failure(CommandLineError::unknown_option, argument);
            return failure(CommandLineError::unexpected_argument, argument);
        }
        if (i + 1 >= arguments.size()) return failure(CommandLineError::missing_value, argument);
        const std::wstring& value = arguments[++i];
        if (value.rfind(L"--", 0) == 0) return failure(CommandLineError::missing_value, argument);
        if (profile_option) {
            if (profile_seen) return failure(CommandLineError::duplicate_option, argument);
            profile_seen = true;
            if (value.empty()) return failure(CommandLineError::empty_profile, argument);
            result.command.profile_path = value;
        } else if (process_option) {
            if (process_seen) return failure(CommandLineError::duplicate_option, argument);
            if (window_seen) return failure(CommandLineError::conflicting_targets);
            process_seen = true;
            if (!decimal(value, result.command.process_id)) return failure(CommandLineError::invalid_process, value);
            result.command.target = CommandLineTarget::process;
        } else {
            if (window_seen) return failure(CommandLineError::duplicate_option, argument);
            if (process_seen) return failure(CommandLineError::conflicting_targets);
            window_seen = true;
            if (!hexadecimal(value, result.command.window_handle)) return failure(CommandLineError::invalid_window, value);
            result.command.target = CommandLineTarget::window;
        }
    }
    if (result.command.action == CommandLineAction::gui) {
        if (result.command.target != CommandLineTarget::none) return failure(CommandLineError::missing_command);
        return result;
    }
    if (result.command.action == CommandLineAction::list) {
        if (result.command.target != CommandLineTarget::none) return failure(CommandLineError::unexpected_argument);
        return result;
    }
    if (result.command.action == CommandLineAction::version) {
        if (result.command.target != CommandLineTarget::none) return failure(CommandLineError::unexpected_argument);
        return result;
    }
    if (result.command.target == CommandLineTarget::none) return failure(CommandLineError::missing_target);
    return result;
}
}
