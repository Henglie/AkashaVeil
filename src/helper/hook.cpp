// SPDX-License-Identifier: Apache-2.0
#include <windows.h>
#include "helper/protocol.h"

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:AkashaVeilHook=_AkashaVeilHook@12")
#endif
extern "C" __declspec(dllexport) LRESULT CALLBACK AkashaVeilHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
        const UINT id = RegisterWindowMessageW(AV_CAPTURE_MESSAGE);
        if (id && message->message == id && message->wParam == GetCurrentProcessId()
            && (message->lParam == 0 || message->lParam == 1 || message->lParam == 0x11)
            && GetAncestor(message->hwnd, GA_ROOT) == message->hwnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(message->hwnd, &pid);
            if (pid == GetCurrentProcessId()) SetWindowDisplayAffinity(message->hwnd, static_cast<DWORD>(message->lParam));
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
