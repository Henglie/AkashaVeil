// SPDX-License-Identifier: Apache-2.0
#include "app/preferences.h"
#include "app/rtl_layout.h"
#include "app/themed_controls.h"
#include "core/i18n.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <vector>

namespace av {
namespace {
struct Check { int id; bool Preferences::* member; Text label; int page,y; };
constexpr Check checks[]={
    {500,&Preferences::auto_refresh,Text::pref_refresh,0,6}, {502,&Preferences::self_protect,Text::pref_self,0,24},
    {503,&Preferences::tray,Text::pref_tray,0,42}, {504,&Preferences::close_to_tray,Text::pref_close,0,60},
    {505,&Preferences::hide_taskbar,Text::pref_taskbar,0,78}, {506,&Preferences::random_title,Text::pref_random_title,0,96},
    {507,&Preferences::random_tray_icon,Text::pref_random_icon,0,114}, {508,&Preferences::random_tooltip,Text::pref_random_tip,0,132},
    {520,&Preferences::auto_hide,Text::pref_auto,1,6}, {521,&Preferences::notify_auto_hide,Text::pref_notify,1,24},
    {522,&Preferences::protect_on_start,Text::pref_existing,1,42}
};
struct Entry { HWND hwnd=nullptr; int id=0; RECT base{}; bool fixed=false; };
struct State {
    HWND dialog=nullptr; Profile draft; void* context=nullptr; ApplyPreferences apply=nullptr;
    std::array<std::vector<HWND>,3> pages;
    std::wstring language; HBRUSH background=nullptr, field=nullptr; COLORREF bg=0,fg=0;
    bool dark=false,rtl=false;
    unsigned dpi=96;
    int client_width=0,client_height=0;
    std::vector<Entry> entries;
    ui::DialogPlacement placement{};
    HFONT dialog_font=nullptr;
    ui::ControlTheme controls_theme{};
    const wchar_t* tr(Text key) const { return text(key,language); }
    void record(HWND control,int id,const RECT& base,bool fixed=false) { if (control) entries.push_back({control,id,base,fixed}); }
    void create_font() {
        if (dialog_font) DeleteObject(dialog_font);
        dialog_font=CreateFontW(-MulDiv(9,static_cast<int>(dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        SendMessageW(dialog,WM_SETFONT,reinterpret_cast<WPARAM>(dialog_font),TRUE);
        for (const auto& entry : entries) SendMessageW(entry.hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(dialog_font),TRUE);
    }
    void set_reading_styles() {
        const int technical[]={501,523,525,526};
        for (const auto& entry : entries) {
            const bool is_technical=std::find(std::begin(technical),std::end(technical),entry.id)!=std::end(technical);
            auto ex=GetWindowLongPtrW(entry.hwnd,GWL_EXSTYLE);
            ex=(rtl&&!is_technical)?(ex|WS_EX_RTLREADING):(ex&~static_cast<LONG_PTR>(WS_EX_RTLREADING));
            SetWindowLongPtrW(entry.hwnd,GWL_EXSTYLE,ex);
            wchar_t cls[32]{}; GetClassNameW(entry.hwnd,cls,32);
            auto style=GetWindowLongPtrW(entry.hwnd,GWL_STYLE);
            if (lstrcmpW(cls,L"Static")==0 && (style&SS_TYPEMASK)!=SS_OWNERDRAW) {
                style=(style&~static_cast<LONG_PTR>(SS_TYPEMASK))|(rtl?SS_RIGHT:SS_LEFT);
                SetWindowLongPtrW(entry.hwnd,GWL_STYLE,style);
            }
            if (lstrcmpW(cls,L"Button")==0 && (style&BS_TYPEMASK)==BS_AUTOCHECKBOX) {
                style = rtl ? (style|BS_LEFTTEXT) : (style&~static_cast<LONG_PTR>(BS_LEFTTEXT));
                SetWindowLongPtrW(entry.hwnd,GWL_STYLE,style);
            }
        }
    }
    void apply_layout() {
        RECT client{}; GetClientRect(dialog,&client);
        const int width=std::max(1,static_cast<int>(client.right));
        const int height=std::max(1,static_cast<int>(client.bottom));
        for (const auto& entry : entries) {
            const int w=entry.base.right-entry.base.left, h=entry.base.bottom-entry.base.top;
            const int x=rtl ? width-(entry.base.left+w) : entry.base.left;
            const int y=entry.fixed ? height-h-static_cast<int>(ui::scale_dip(6,dpi)) : entry.base.top;
            SetWindowPos(entry.hwnd,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
        }
    }
    void prepare_layout() {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(dialog,MONITOR_DEFAULTTONEAREST),&monitor);
        const int client_w=std::max(1,client_width);
        const int client_h=std::max(1,client_height);
        const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(dialog,GWL_STYLE));
        const DWORD exstyle=static_cast<DWORD>(GetWindowLongPtrW(dialog,GWL_EXSTYLE));
        RECT frame{0,0,client_w,client_h}; AdjustWindowRectEx(&frame,style,FALSE,exstyle);
        const int outer_w=frame.right-frame.left, outer_h=frame.bottom-frame.top;
        const int work_left=static_cast<int>(monitor.rcWork.left), work_top=static_cast<int>(monitor.rcWork.top);
        const int work_w=std::max(1,static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
        const int work_h=std::max(1,static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));
        const int margin=static_cast<int>(ui::scale_dip(8,dpi));
        placement.x=std::clamp(work_left+std::max(0,(work_w-outer_w)/2),work_left+margin,std::max(work_left+margin,work_left+work_w-outer_w-margin));
        placement.y=std::clamp(work_top+std::max(0,(work_h-outer_h)/2),work_top+margin,std::max(work_top+margin,work_top+work_h-outer_h-margin));
        placement.width=outer_w; placement.height=outer_h;
        placement.virtual_height=client_h; placement.page_height=client_h; placement.max_scroll=0; placement.scroll_y=false;
        SetWindowPos(dialog,nullptr,placement.x,placement.y,outer_w,outer_h,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        set_reading_styles();
        apply_layout();
    }
    static unsigned window_dpi(HWND window) {
        using GetWindowDpi=UINT(WINAPI*)(HWND);
        if (const auto get=reinterpret_cast<GetWindowDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"))) {
            const UINT value=get(window); if (value) return value;
        }
        HDC dc=GetDC(nullptr); const UINT value=static_cast<UINT>(GetDeviceCaps(dc,LOGPIXELSX)?GetDeviceCaps(dc,LOGPIXELSX):96); ReleaseDC(nullptr,dc); return value;
    }
    HWND make(int page,const wchar_t* cls,int id,const wchar_t* caption,DWORD style,int x,int y,int width,int height) {
        RECT area{8,26,0,0}; MapDialogRect(dialog,&area);
        RECT r{x,y,x+width,y+height}; MapDialogRect(dialog,&r);
        HWND control=CreateWindowExW(lstrcmpW(cls,WC_EDITW)==0 ? WS_EX_CLIENTEDGE : 0,cls,caption,WS_CHILD|style,r.left+area.left,r.top+area.top,r.right-r.left,r.bottom-r.top,dialog,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(control,WM_SETFONT,SendMessageW(dialog,WM_GETFONT,0,0),TRUE);
        controls_theme.apply(control);
        record(control,id,RECT{r.left+area.left,r.top+area.top,r.right+area.left,r.bottom+area.top});
        pages[page].push_back(control); return control;
    }
    void switch_page() {
        const int selected=TabCtrl_GetCurSel(GetDlgItem(dialog,401));
        for (int i=0;i<3;++i) for (HWND control : pages[i]) ShowWindow(control,i==selected ? SW_SHOW : SW_HIDE);
    }
    void programs() {
        const HWND list=GetDlgItem(dialog,525); SendMessageW(list,LB_RESETCONTENT,0,0);
        HDC dc=GetDC(list); const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(dialog,WM_GETFONT,0,0))); LONG width=0;
        for (const auto& program : draft.preferences.programs) {
            SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(program.c_str())); SIZE size{};
            GetTextExtentPoint32W(dc,program.c_str(),static_cast<int>(program.size()),&size); if (size.cx>width) width=size.cx;
        }
        SelectObject(dc,old); ReleaseDC(list,dc); SendMessageW(list,LB_SETHORIZONTALEXTENT,width+16,0);
    }
    void dependencies() {
        const bool tray=IsDlgButtonChecked(dialog,503)==BST_CHECKED;
        if (!tray) { CheckDlgButton(dialog,504,BST_UNCHECKED); CheckDlgButton(dialog,505,BST_UNCHECKED); }
        EnableWindow(GetDlgItem(dialog,504),tray); EnableWindow(GetDlgItem(dialog,505),tray);
    }
    void populate() {
        for (const auto& check : checks) CheckDlgButton(dialog,check.id,draft.preferences.*(check.member) ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dialog,501,draft.preferences.refresh_ms,FALSE); SetDlgItemInt(dialog,523,draft.preferences.window_wait_ms,FALSE);
        SendDlgItemMessageW(dialog,524,CB_SETCURSEL,draft.preferences.auto_mode==Mode::blank ? 0 : 1,0);
        programs(); dependencies();
    }
    bool read(Profile& candidate) {
        candidate=draft;
        for (const auto& check : checks) candidate.preferences.*(check.member)=IsDlgButtonChecked(dialog,check.id)==BST_CHECKED;
        auto number=[&](int id,unsigned minimum,unsigned maximum,unsigned& value) {
            wchar_t input[32]{}; GetDlgItemTextW(dialog,id,input,32); wchar_t* end=nullptr; errno=0;
            const auto n=std::wcstoull(input,&end,10);
            if (!*input || errno || *end || n<minimum || n>maximum) return false;
            value=static_cast<unsigned>(n); return true;
        };
        if (!number(501,100,60000,candidate.preferences.refresh_ms) || !number(523,0,60000,candidate.preferences.window_wait_ms)) return false;
        candidate.preferences.auto_mode=SendDlgItemMessageW(dialog,524,CB_GETCURSEL,0,0)==0 ? Mode::blank : Mode::exclude;
        return candidate.preferences.programs.size()<=256;
    }
    void create() {
        ui::use_manual_dialog_dpi(dialog);
        dpi=window_dpi(dialog);
        RECT content{0,0,384,260}; MapDialogRect(dialog,&content);
        client_width=content.right; client_height=content.bottom;
        RECT tab_rect{}; GetWindowRect(GetDlgItem(dialog,401),&tab_rect);
        MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&tab_rect),2);
        record(GetDlgItem(dialog,401),401,tab_rect);
        language=choose_language(draft.language,system_language());
        rtl=ui::is_rtl_language(language);
        dark=draft.theme==Theme::dark || (draft.theme==Theme::system && system_dark());
        const auto palette=ui::resolve_palette(static_cast<int>(draft.theme),system_dark(),draft.accent);
        bg=palette.window_bg; fg=palette.text;
        controls_theme.palette=palette; controls_theme.rtl=rtl;
        background=CreateSolidBrush(bg); field=CreateSolidBrush(palette.field_bg);
        const BOOL enabled=dark; DwmSetWindowAttribute(dialog,20,&enabled,sizeof(enabled));
        SetWindowTextW(dialog,tr(Text::preferences)); SetDlgItemTextW(dialog,IDOK,tr(Text::ok)); SetDlgItemTextW(dialog,IDCANCEL,tr(Text::cancel)); SetDlgItemTextW(dialog,402,tr(Text::apply));
        const Text page_labels[]={Text::general,Text::auto_protection,Text::advanced};
        for (int i=0;i<3;++i) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(tr(page_labels[i])); TabCtrl_InsertItem(GetDlgItem(dialog,401),i,&item); }
        controls_theme.apply_children(dialog);
        create_font();
        for (const auto& check : checks) make(check.page,L"BUTTON",check.id,tr(check.label),BS_AUTOCHECKBOX|BS_MULTILINE|WS_TABSTOP,8,check.y,check.id==500 ? 200 : 342,13);
        make(0,WC_EDITW,501,L"",WS_TABSTOP|ES_NUMBER,215,6,62,14);
        make(0,L"STATIC",0,tr(Text::milliseconds),0,284,7,52,14);
        make(1,L"STATIC",0,tr(Text::pref_mode),0,8,62,124,14);
        HWND mode=make(1,WC_COMBOBOXW,524,L"",WS_TABSTOP|CBS_DROPDOWNLIST,138,60,196,70);
        SendMessageW(mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(tr(Text::blanked))); SendMessageW(mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(tr(Text::excluded)));
        make(1,L"STATIC",0,tr(Text::pref_wait),0,8,82,200,14);
        make(1,WC_EDITW,523,L"",WS_TABSTOP|ES_NUMBER,215,80,62,14); make(1,L"STATIC",0,tr(Text::milliseconds),0,284,81,52,14);
        make(1,L"STATIC",0,tr(Text::pref_programs),0,8,101,330,14);
        make(1,L"LISTBOX",525,L"",WS_BORDER|WS_VSCROLL|WS_HSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,8,114,326,44);
        HWND input=make(1,WC_EDITW,526,L"",WS_TABSTOP|ES_AUTOHSCROLL,8,165,220,14);
        SendMessageW(input,EM_SETLIMITTEXT,4096,0); SendMessageW(input,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(tr(Text::pref_program_hint)));
        make(1,L"BUTTON",527,tr(Text::browse),WS_TABSTOP,234,164,100,16);
        make(1,L"BUTTON",528,tr(Text::add),WS_TABSTOP,8,186,75,16); make(1,L"BUTTON",529,tr(Text::remove),WS_TABSTOP,90,186,75,16);
        make(2,L"BUTTON",540,tr(Text::reset),WS_TABSTOP,8,10,150,18);
        make(2,L"STATIC",0,tr(Text::preferences_help),SS_LEFT,8,42,326,145);
        for (int id : {IDOK,IDCANCEL,402}) {
            const HWND control=GetDlgItem(dialog,id); RECT base{};
            if (control && GetWindowRect(control,&base)) {
                MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&base),2);
                record(control,id,base,true);
            }
        }
        populate(); switch_page();
        create_font();
        prepare_layout();
    }
    ~State() { if (dialog_font) DeleteObject(dialog_font); if (background) DeleteObject(background); if (field) DeleteObject(field); }
};
INT_PTR CALLBACK procedure(HWND dialog,UINT message,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if (message==WM_INITDIALOG) { state=reinterpret_cast<State*>(lp); state->dialog=dialog; SetWindowLongPtrW(dialog,DWLP_USER,lp); state->create(); return TRUE; }
    if (!state) return FALSE;
    if (message==WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->idFrom==401 && reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE) { state->switch_page(); return TRUE; }
    if (message==WM_CTLCOLORDLG || message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN || message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX) {
        SetTextColor(reinterpret_cast<HDC>(wp),state->fg); SetBkColor(reinterpret_cast<HDC>(wp),message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX ? (state->dark ? RGB(24,24,24) : GetSysColor(COLOR_WINDOW)) : state->bg);
        return reinterpret_cast<INT_PTR>(message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX ? state->field : state->background);
    }
    if (message==WM_DPICHANGED) {
        const unsigned next=HIWORD(wp);
        if (next && next!=state->dpi) {
            const unsigned old=state->dpi;
            state->client_width=MulDiv(state->client_width,next,old);
            state->client_height=MulDiv(state->client_height,next,old);
            for (auto& entry : state->entries) {
                entry.base.left=MulDiv(entry.base.left,next,old); entry.base.top=MulDiv(entry.base.top,next,old);
                entry.base.right=MulDiv(entry.base.right,next,old); entry.base.bottom=MulDiv(entry.base.bottom,next,old);
            }
            state->dpi=next;
            state->create_font(); state->prepare_layout();
        }
        return TRUE;
    }
    if (message==WM_SETTINGCHANGE || message==WM_THEMECHANGED) {
        state->dark=state->draft.theme==Theme::dark || (state->draft.theme==Theme::system && system_dark());
        const auto palette=ui::resolve_palette(static_cast<int>(state->draft.theme),system_dark(),state->draft.accent);
        state->bg=palette.window_bg; state->fg=palette.text;
        state->controls_theme.palette=palette;
        state->controls_theme.apply_children(dialog);
        if (state->background) DeleteObject(state->background);
        if (state->field) DeleteObject(state->field);
        state->background=CreateSolidBrush(state->bg); state->field=CreateSolidBrush(palette.field_bg);
        const BOOL dark_title=state->dark; DwmSetWindowAttribute(dialog,20,&dark_title,sizeof(dark_title));
        InvalidateRect(dialog,nullptr,TRUE); return TRUE;
    }
    if (message==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
    if (message!=WM_COMMAND) return FALSE;
    const int id=LOWORD(wp);
    if (id==IDCANCEL) { EndDialog(dialog,IDCANCEL); return TRUE; }
    if (id==503) { state->dependencies(); return TRUE; }
    if (id==540) {
        if (MessageBoxW(dialog,state->tr(Text::reset_confirm),state->tr(Text::reset),MB_YESNO|MB_ICONWARNING)==IDYES) { state->draft=Profile{}; state->populate(); }
        return TRUE;
    }
    if (id==527) {
        wchar_t file[32768]{}; OPENFILENAMEW picker{sizeof(picker)}; picker.hwndOwner=dialog; picker.lpstrFile=file; picker.nMaxFile=32768;
        picker.lpstrFilter=state->tr(Text::program_filter); picker.Flags=OFN_FILEMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&picker)) SetDlgItemTextW(dialog,526,file);
        return TRUE;
    }
    if (id==528) {
        std::wstring value(4097,L'\0'); value.resize(GetDlgItemTextW(dialog,526,value.data(),4097));
        const auto first=value.find_first_not_of(L" \t"); const auto last=value.find_last_not_of(L" \t");
        if (first==std::wstring::npos) return TRUE;
        value=value.substr(first,last-first+1);
        if (value.size()>1 && value.front()==L'\"' && value.back()==L'\"') value=value.substr(1,value.size()-2);
        if (value.empty() || value.find_first_of(L"\r\n")!=std::wstring::npos || state->draft.preferences.programs.size()>=256) { MessageBoxW(dialog,state->tr(Text::invalid_preferences),state->tr(Text::error),MB_OK|MB_ICONERROR); return TRUE; }
        state->draft.preferences.programs.push_back(std::move(value)); state->programs(); SetDlgItemTextW(dialog,526,L""); return TRUE;
    }
    if (id==529) {
        const auto selected=SendDlgItemMessageW(dialog,525,LB_GETCURSEL,0,0);
        if (selected!=LB_ERR && static_cast<std::size_t>(selected)<state->draft.preferences.programs.size()) {
            state->draft.preferences.programs.erase(state->draft.preferences.programs.begin()+selected); state->programs();
        }
        return TRUE;
    }
    if (id==IDOK || id==402) {
        Profile candidate;
        if (!state->read(candidate)) { MessageBoxW(dialog,state->tr(Text::invalid_preferences),state->tr(Text::error),MB_OK|MB_ICONERROR); return TRUE; }
        const DWORD error=state->apply(state->context,candidate);
        if (error) { const auto text=error_text(error); MessageBoxW(dialog,text.c_str(),state->tr(Text::error),MB_OK|MB_ICONERROR); return TRUE; }
        state->draft=std::move(candidate);
        if (id==IDOK) EndDialog(dialog,IDOK);
        return TRUE;
    }
    return FALSE;
}
}
void show_preferences(HWND owner,const Profile& profile,void* context,ApplyPreferences apply) {
    State state; state.draft=profile; state.context=context; state.apply=apply;
    if (DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(400),owner,procedure,reinterpret_cast<LPARAM>(&state))==-1) {
        const auto message=error_text(GetLastError()); MessageBoxW(owner,message.c_str(),L"AkashaVeil",MB_OK|MB_ICONERROR);
    }
}
}
