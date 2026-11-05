// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/command_line.h"

namespace av {
int report_command_line_error(const CommandLineResult& result);
int run_command_line(const CommandLine& command);
}
