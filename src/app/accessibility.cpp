// SPDX-License-Identifier: Apache-2.0
#include "app/accessibility.h"
#include <initguid.h>
#include <oleacc.h>
#include <algorithm>
#include <vector>

namespace av::ui {
namespace {

IAccPropServices* property_service() {
    IAccPropServices* service = nullptr;
    if (FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
        IID_IAccPropServices, reinterpret_cast<void**>(&service)))) return nullptr;
    return service;
}

void release(IAccessible* accessible) { if (accessible) accessible->Release(); }

std::wstring take_bstr(BSTR value) {
    if (!value) return {};
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

VARIANT child_variant(long child_id) {
    VARIANT value{};
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = child_id;
    return value;
}

bool read_node(IAccessible* accessible, long child_id, AccessibleNode& node) {
    VARIANT child = child_variant(child_id);
    BSTR name = nullptr;
    BSTR description = nullptr;
    VARIANT role{}, state{};
    VariantInit(&role);
    VariantInit(&state);
    if (FAILED(accessible->get_accName(child, &name))) name = nullptr;
    if (FAILED(accessible->get_accDescription(child, &description))) description = nullptr;
    if (FAILED(accessible->get_accRole(child, &role))) VariantInit(&role);
    if (FAILED(accessible->get_accState(child, &state))) VariantInit(&state);
    node.name = take_bstr(name);
    node.description = take_bstr(description);
    node.role = role.vt == VT_I4 ? role.lVal : 0;
    node.state = state.vt == VT_I4 ? state.lVal : 0;
    node.valid = true;
    return true;
}

HWND first_tab_stop(HWND parent) {
    HWND child = GetWindow(parent, GW_CHILD);
    while (child) {
        const auto style = GetWindowLongPtrW(child, GWL_STYLE);
        if ((style & WS_TABSTOP) && IsWindowVisible(child) && IsWindowEnabled(child)) return child;
        if (HWND nested = first_tab_stop(child)) return nested;
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return nullptr;
}

} // namespace

bool annotate_control(HWND hwnd, const wchar_t* name, const wchar_t* description, long role) {
    if (!hwnd) return false;
    IAccPropServices* service = property_service();
    if (!service) return false;
    bool ok = true;
    if (name) ok = SUCCEEDED(service->SetHwndPropStr(hwnd, static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(CHILDID_SELF), PROPID_ACC_NAME, name)) && ok;
    if (description) ok = SUCCEEDED(service->SetHwndPropStr(hwnd, static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(CHILDID_SELF), PROPID_ACC_DESCRIPTION, description)) && ok;
    if (role) {
        VARIANT value{};
        VariantInit(&value);
        value.vt = VT_I4;
        value.lVal = role;
        ok = SUCCEEDED(service->SetHwndProp(hwnd, static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(CHILDID_SELF), PROPID_ACC_ROLE, value)) && ok;
    }
    service->Release();
    return ok;
}

bool annotate_state(HWND hwnd, long role, long state) {
    if (!hwnd) return false;
    IAccPropServices* service = property_service();
    if (!service) return false;
    VARIANT role_value{}, state_value{};
    VariantInit(&role_value);
    VariantInit(&state_value);
    role_value.vt = VT_I4;
    role_value.lVal = role;
    state_value.vt = VT_I4;
    state_value.lVal = state;
    bool ok = SUCCEEDED(service->SetHwndProp(hwnd, static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(CHILDID_SELF), PROPID_ACC_ROLE, role_value));
    ok = SUCCEEDED(service->SetHwndProp(hwnd, static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(CHILDID_SELF), PROPID_ACC_STATE, state_value)) && ok;
    service->Release();
    return ok;
}

void notify_name(HWND hwnd) {
    if (hwnd) NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
}
void notify_state(HWND hwnd) {
    if (hwnd) NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
}

AccessibleNode read_self(HWND hwnd) {
    AccessibleNode node;
    IAccessible* accessible = nullptr;
    if (!hwnd || FAILED(AccessibleObjectFromWindow(hwnd, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
        reinterpret_cast<void**>(&accessible)))) return node;
    read_node(accessible, CHILDID_SELF, node);
    long count = 0;
    if (SUCCEEDED(accessible->get_accChildCount(&count))) node.child_count = count;
    release(accessible);
    return node;
}

bool read_child(HWND hwnd, long child_id, AccessibleNode& node) {
    IAccessible* accessible = nullptr;
    if (!hwnd || FAILED(AccessibleObjectFromWindow(hwnd, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
        reinterpret_cast<void**>(&accessible)))) return false;
    const bool ok = read_node(accessible, child_id, node);
    release(accessible);
    return ok;
}

std::vector<AccessibleNode> read_children(HWND hwnd, long limit) {
    std::vector<AccessibleNode> result;
    IAccessible* accessible = nullptr;
    if (!hwnd || FAILED(AccessibleObjectFromWindow(hwnd, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
        reinterpret_cast<void**>(&accessible)))) return result;
    long count = 0;
    if (SUCCEEDED(accessible->get_accChildCount(&count))) {
        const long bounded = std::min(count, limit);
        for (long child = 1; child <= bounded; ++child) {
            AccessibleNode node;
            if (read_node(accessible, child, node)) result.push_back(std::move(node));
        }
    }
    release(accessible);
    return result;
}

FocusScope::FocusScope(HWND popup) : popup_(popup), owner_(popup ? GetWindow(popup, GW_OWNER) : nullptr),
    previous_(GetFocus()) {}

FocusScope::~FocusScope() { restore(); }

bool FocusScope::enter() {
    if (!popup_ || !IsWindow(popup_)) return false;
    if (HWND target = first_tab_stop(popup_)) return SetFocus(target) != nullptr;
    return SetFocus(popup_) != nullptr;
}

void FocusScope::restore() {
    if (restored_) return;
    restored_ = true;
    HWND target = previous_;
    if (!target || !IsWindow(target) || !IsWindowVisible(target)) target = owner_;
    if (target && IsWindow(target)) SetFocus(target);
}

} // namespace av::ui
