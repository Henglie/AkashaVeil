// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace av::ui {

struct AccessibleNode {
    std::wstring name;
    std::wstring description;
    long role = 0;
    long state = 0;
    long child_count = 0;
    bool valid = false;
};

bool annotate_control(HWND hwnd, const wchar_t* name, const wchar_t* description, long role);
bool annotate_state(HWND hwnd, long role, long state);
void notify_name(HWND hwnd);
void notify_state(HWND hwnd);

AccessibleNode read_self(HWND hwnd);
bool read_child(HWND hwnd, long child_id, AccessibleNode& node);
std::vector<AccessibleNode> read_children(HWND hwnd, long limit = 64);

class FocusScope {
public:
    explicit FocusScope(HWND popup);
    ~FocusScope();
    FocusScope(const FocusScope&) = delete;
    FocusScope& operator=(const FocusScope&) = delete;
    bool enter();
    void restore();
    HWND previous() const { return previous_; }
private:
    HWND popup_ = nullptr;
    HWND owner_ = nullptr;
    HWND previous_ = nullptr;
    bool restored_ = false;
};

} // namespace av::ui
