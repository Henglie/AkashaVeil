// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/windows.h"

namespace av {
using ApplyPreferences=DWORD(*)(void*,const Profile&);
void show_preferences(HWND owner,const Profile& profile,void* context,ApplyPreferences apply);
}
