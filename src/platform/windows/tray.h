// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <string>

namespace av {
class Tray {
    HWND owner=nullptr; UINT callback=0; bool present=false;
public:
    void initialize(HWND hwnd,UINT message) { owner=hwnd; callback=message; }
    bool update(bool enabled,HICON icon,const std::wstring& tooltip);
    void removed_by_shell() { present=false; }
    bool active() const { return present; }
    void remove();
    bool notify(const std::wstring& title,const std::wstring& message);
    ~Tray() { remove(); }
};
std::wstring random_label();
}
