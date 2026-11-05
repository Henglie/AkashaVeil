// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/tray.h"
#include <shellapi.h>
#include <wincrypt.h>

namespace av {
bool Tray::update(bool enabled,HICON icon,const std::wstring& tooltip) {
    if (!enabled) { remove(); return true; }
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd=owner; data.uID=1;
    data.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP; data.uCallbackMessage=callback; data.hIcon=icon;
    lstrcpynW(data.szTip,tooltip.c_str(),128);
    if (!Shell_NotifyIconW(present ? NIM_MODIFY : NIM_ADD,&data)) {
        if (present || !Shell_NotifyIconW(NIM_MODIFY,&data)) { present=false; return false; }
    }
    present=true; data.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&data); return true;
}
void Tray::remove() {
    if (!present) return;
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd=owner; data.uID=1; Shell_NotifyIconW(NIM_DELETE,&data); present=false;
}
bool Tray::notify(const std::wstring& title,const std::wstring& message) {
    if (!present) return false;
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd=owner; data.uID=1; data.uFlags=NIF_INFO;
    data.dwInfoFlags=NIIF_INFO|NIIF_RESPECT_QUIET_TIME;
    lstrcpynW(data.szInfoTitle,title.c_str(),64); lstrcpynW(data.szInfo,message.c_str(),256);
    return Shell_NotifyIconW(NIM_MODIFY,&data) != FALSE;
}
std::wstring random_label() {
    DWORD value=GetTickCount()^GetCurrentProcessId(); HCRYPTPROV provider=0;
    if (CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(provider,sizeof(value),reinterpret_cast<BYTE*>(&value)); CryptReleaseContext(provider,0);
    }
    static constexpr wchar_t digits[]=L"0123456789ABCDEF"; std::wstring label=L"AV-";
    for (int shift=28;shift>=0;shift-=4) label+=digits[(value>>shift)&15];
    return label;
}
}
