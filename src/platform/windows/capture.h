// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/windows.h"

namespace av {
bool apply_affinity(const Window& window, Mode mode, DWORD& error, bool allow_worker = true);
}
