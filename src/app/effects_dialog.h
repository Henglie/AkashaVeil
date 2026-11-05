// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/windows.h"
#include <string>
namespace av {
bool show_effects_dialog(HWND owner,const Profile& profile,const Window& sample,std::size_t targets,Mode& mode,Effects& effects,std::wstring* hidden_key=nullptr);
}
