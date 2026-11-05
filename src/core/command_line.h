// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace av {
enum class CommandLineAction { gui, hide, unhide, list, version, help };
enum class CommandLineTarget { none, process, window };
enum class CommandLineError {
    none,
    unknown_option,
    missing_value,
    invalid_process,
    invalid_window,
    duplicate_option,
    conflicting_targets,
    missing_target,
    missing_command,
    unexpected_argument,
    empty_profile
};

struct CommandLine {
    CommandLineAction action = CommandLineAction::gui;
    CommandLineTarget target = CommandLineTarget::none;
    std::uint32_t process_id = 0;
    std::uintptr_t window_handle = 0;
    std::wstring profile_path;
    bool json = false;
};

struct CommandLineResult {
    CommandLine command;
    CommandLineError error = CommandLineError::none;
    std::wstring token;
    bool ok() const noexcept { return error == CommandLineError::none; }
    explicit operator bool() const noexcept { return ok(); }
};

CommandLineResult parse_command_line(const std::vector<std::wstring>& arguments);
}
