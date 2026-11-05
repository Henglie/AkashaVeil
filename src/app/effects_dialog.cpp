// SPDX-License-Identifier: Apache-2.0
#include "app/effects_dialog.h"
#include "app/rtl_layout.h"
#include "app/themed_controls.h"
#include "core/i18n.h"
#include "platform/windows/effects.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <cerrno>
#include <algorithm>
#include <iterator>
#include <vector>

namespace av {
namespace {
std::size_t utf8_bytes(const std::wstring& value) {
    if (value.empty()) return 0;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    return size > 0 ? static_cast<std::size_t>(size) : 0;
}
struct Entry { HWND hwnd=nullptr; int id=0; RECT base{}; bool fixed=false, dropdown=false; };
struct Dialog {
    HWND hwnd=nullptr; Window sample; Mode mode=Mode::exclude; Effects effects;
    std::wstring language; std::size_t targets=0; std::wstring hidden_key;
    HBRUSH background=nullptr,field=nullptr; COLORREF bg=0,fg=0;
    bool dark=false,updating=true,rtl=false;
    int theme_mode=0; COLORREF accent=CLR_INVALID;
    unsigned dpi=96;
    std::vector<Entry> entries;
    ui::DialogPlacement placement{};
    int scroll_offset=0;
    int content_bottom_=0, max_scroll_=0;
    HFONT dialog_font=nullptr;
    ui::ControlTheme controls_theme{};
    const wchar_t* tr(Text key) const { return text(key,language); }
    static unsigned window_dpi(HWND window) {
        using GetWindowDpi=UINT(WINAPI*)(HWND);
        if (const auto get=reinterpret_cast<GetWindowDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"))) {
            const UINT value=get(window); if (value) return value;
        }
        HDC dc=GetDC(nullptr); const UINT value=static_cast<UINT>(GetDeviceCaps(dc,LOGPIXELSX) ? GetDeviceCaps(dc,LOGPIXELSX) : 96); ReleaseDC(nullptr,dc); return value;
    }
    void create_font() {
        if (dialog_font) DeleteObject(dialog_font);
        dialog_font=CreateFontW(-MulDiv(9,static_cast<int>(dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(dialog_font),TRUE);
        for (const auto& entry : entries) SendMessageW(entry.hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(dialog_font),TRUE);
    }
    void record(HWND control,int id,const RECT& base,bool fixed=false,bool dropdown=false) {
        if (control) entries.push_back({control,id,base,fixed,dropdown});
    }
    void set_reading_styles() {
        const int technical[]={616,617,618,619,620,623,624,625,626};
        const auto technical_id=[&](int id){ return std::find(std::begin(technical),std::end(technical),id)!=std::end(technical); };
        for (const auto& entry : entries) {
            auto ex=GetWindowLongPtrW(entry.hwnd,GWL_EXSTYLE);
            ex = (rtl && !technical_id(entry.id)) ? (ex|WS_EX_RTLREADING) : (ex&~static_cast<LONG_PTR>(WS_EX_RTLREADING));
            SetWindowLongPtrW(entry.hwnd,GWL_EXSTYLE,ex);
            wchar_t cls[32]{}; GetClassNameW(entry.hwnd,cls,32);
            auto style=GetWindowLongPtrW(entry.hwnd,GWL_STYLE);
            if (lstrcmpW(cls,L"Static")==0 && (style&SS_TYPEMASK)!=SS_OWNERDRAW) {
                style=(style&~static_cast<LONG_PTR>(SS_TYPEMASK))|(rtl?SS_RIGHT:SS_LEFT);
                SetWindowLongPtrW(entry.hwnd,GWL_STYLE,style);
            }
        }
    }
    void apply_layout() {
        if (!hwnd) return;
        RECT client{}; GetClientRect(hwnd,&client);
        const int client_width=std::max(1,static_cast<int>(client.right));
        const int client_height=std::max(1,static_cast<int>(client.bottom));
        const int margin=static_cast<int>(ui::scale_dip(8,dpi));
        int fixed_height=0;
        for (const auto& entry : entries) if (entry.fixed) fixed_height=std::max(fixed_height,static_cast<int>(entry.base.bottom-entry.base.top));
        content_bottom_=std::max(1,client_height-fixed_height-margin);
        max_scroll_=std::max(0,placement.virtual_height-content_bottom_);
        scroll_offset=std::clamp(scroll_offset,0,max_scroll_);
        for (const auto& entry : entries) {
            const int w=entry.base.right-entry.base.left;
            const int h=entry.base.bottom-entry.base.top;
            const int x=rtl ? client_width-(entry.base.left+w) : entry.base.left;
            if (entry.fixed) {
                SetWindowPos(entry.hwnd,HWND_TOP,x,client_height-h-margin,w,h,SWP_NOACTIVATE);
                continue;
            }
            const int y=entry.base.top-scroll_offset;
            if (y+h<=0 || y>=content_bottom_) { ShowWindow(entry.hwnd,SW_HIDE); continue; }
            const int visible=entry.dropdown ? h : std::max(1,std::min(h,content_bottom_-y));
            ShowWindow(entry.hwnd,SW_SHOW);
            SetWindowPos(entry.hwnd,nullptr,x,y,w,visible,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        SCROLLINFO info{sizeof(info)};
        info.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
        info.nMin=0; info.nMax=std::max(0,placement.virtual_height-1);
        info.nPage=static_cast<UINT>(content_bottom_);
        info.nPos=scroll_offset;
        SetScrollInfo(hwnd,SB_VERT,&info,TRUE);
        auto style=GetWindowLongPtrW(hwnd,GWL_STYLE);
        const bool need_scroll=max_scroll_>0;
        style=need_scroll ? (style|WS_VSCROLL) : (style&~static_cast<LONG_PTR>(WS_VSCROLL));
        SetWindowLongPtrW(hwnd,GWL_STYLE,style);
        ShowScrollBar(hwnd,SB_VERT,need_scroll);
        InvalidateRect(hwnd,nullptr,TRUE);
    }
    void scroll_by(int delta) {
        if (!placement.scroll_y || max_scroll_<=0) return;
        const int next=std::clamp(scroll_offset+delta,0,max_scroll_);
        if (next==scroll_offset) return;
        scroll_offset=next; apply_layout();
    }
    void ensure_visible(HWND control) {
        if (!control || !placement.scroll_y || max_scroll_<=0) return;
        RECT rect{}; GetWindowRect(control,&rect); MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&rect),2);
        if (rect.top<0 || rect.bottom>content_bottom_) {
            scroll_offset=std::clamp(static_cast<int>(rect.bottom)-content_bottom_,0,max_scroll_);
            apply_layout();
        }
    }
    void prepare_layout() {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(GetParent(hwnd),MONITOR_DEFAULTTONEAREST),&monitor);
        const int margin=static_cast<int>(ui::scale_dip(8,dpi));
        RECT collapsed{0,0,0,14}; MapDialogRect(hwnd,&collapsed);
        int max_right=0, max_bottom=0, fixed_height=0;
        for (const auto& e : entries) {
            if (e.fixed) { fixed_height=std::max(fixed_height,static_cast<int>(e.base.bottom-e.base.top)); continue; }
            max_right=std::max(max_right,static_cast<int>(e.base.right));
            const int bottom=e.dropdown ? static_cast<int>(e.base.top)+collapsed.bottom : static_cast<int>(e.base.bottom);
            max_bottom=std::max(max_bottom,bottom);
        }
        const int client_w=std::max(1,max_right+margin);
        int client_h=std::max(1,max_bottom+margin+fixed_height);
        const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE));
        const DWORD exstyle=static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_EXSTYLE));
        RECT frame{0,0,client_w,client_h};
        AdjustWindowRectEx(&frame,style,FALSE,exstyle);
        const int work_left=static_cast<int>(monitor.rcWork.left), work_top=static_cast<int>(monitor.rcWork.top);
        const int work_w=std::max(1,static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
        const int work_h=std::max(1,static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));
        const int border_h=(frame.bottom-frame.top)-client_h;
        const int max_client_h=std::max(fixed_height+margin,work_h-2*margin-border_h);
        if (client_h>max_client_h) {
            client_h=max_client_h;
            frame.left=0; frame.top=0; frame.right=client_w; frame.bottom=client_h;
            AdjustWindowRectEx(&frame,style,FALSE,exstyle);
        }
        int outer_w=frame.right-frame.left;
        const int outer_h=frame.bottom-frame.top;
        placement.x=work_left+std::max(0,(work_w-outer_w)/2);
        placement.y=work_top+std::max(0,(work_h-outer_h)/2);
        placement.width=outer_w; placement.height=outer_h;
        placement.virtual_height=max_bottom;
        placement.page_height=client_h-fixed_height-margin;
        placement.max_scroll=std::max(0,placement.virtual_height-placement.page_height);
        placement.scroll_y=placement.max_scroll>0;
        if (placement.scroll_y) outer_w+=GetSystemMetrics(SM_CXVSCROLL);
        placement.width=outer_w;
        scroll_offset=0;
        SetWindowPos(hwnd,nullptr,placement.x,placement.y,outer_w,outer_h,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        set_reading_styles();
        apply_layout();
    }
    HWND make(const wchar_t* cls,int id,const wchar_t* text,DWORD style,int x,int y,int w,int h,bool dropdown=false) {
        RECT r{x,y,x+w,y+h}; MapDialogRect(hwnd,&r);
        HWND control=CreateWindowExW(lstrcmpW(cls,L"EDIT")==0 ? WS_EX_CLIENTEDGE : 0,cls,text,WS_CHILD|WS_VISIBLE|style,r.left,r.top,r.right-r.left,r.bottom-r.top,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(control,WM_SETFONT,SendMessageW(hwnd,WM_GETFONT,0,0),TRUE);
        controls_theme.apply(control);
        record(control,id,r,false,dropdown);
        return control;
    }
    HWND label(Text key,int x,int y,int width=238) { return make(L"STATIC",0,tr(key),SS_LEFT,x,y,width,12); }
    void combo(int id,const Text* choices,int count,int x,int y,int width) {
        HWND control=make(WC_COMBOBOXW,id,L"",CBS_DROPDOWNLIST|WS_TABSTOP,x,y,width,110,true);
        for (int i=0;i<count;++i) SendMessageW(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(tr(choices[i])));
    }
    int selection(int id) const { return static_cast<int>(SendDlgItemMessageW(hwnd,id,CB_GETCURSEL,0,0)); }
    void select(int id,int value) { SendDlgItemMessageW(hwnd,id,CB_SETCURSEL,value,0); }
    std::wstring input(int id) const { std::wstring value(1025,L'\0'); value.resize(GetDlgItemTextW(hwnd,id,value.data(),1025)); return value; }
    bool read(Mode& m,Effects& e) {
        e=effects;
        const int selected=selection(610);
        if (selected<0 || selected>4 || selection(611)<0 || selection(612)<0) return false;
        m=selected==0 ? Mode::none : selected==2 ? Mode::blank : Mode::exclude;
        e.cover=selected==3 ? Cover::solid : selected==4 ? Cover::label : Cover::none;
        e.mark=static_cast<Mark>(selection(611)); e.capture_only=selection(612)==1;
        e.text=input(613); e.label=input(614); e.tag=input(615);
        e.capture_route=selection(623)==1 ? 2u : 1u;
        const int hidden=selection(624);
        e.hidden_algorithm=hidden==1 ? 1u : hidden==2 ? 2u : 0u;
        e.hidden_payload=input(625);
        e.hidden_payload_bytes.clear();
        hidden_key=input(626);
        if (e.hidden_algorithm!=0) {
            if (!e.capture_only || e.cover!=Cover::none) return false;
            if (e.hidden_payload.empty() || utf8_bytes(e.hidden_payload)>32) return false;
            if (hidden_key.empty() || utf8_bytes(hidden_key)>128) return false;
        } else {
            e.hidden_payload.clear();
            hidden_key.clear();
        }
        auto number=[&](int id,unsigned& value) {
            const auto s=input(id); wchar_t* end=nullptr; errno=0;
            const auto n=std::wcstoull(s.c_str(),&end,10);
            if (s.empty() || *end || errno || n>10000) return false; value=static_cast<unsigned>(n); return true;
        };
        return number(616,e.opacity) && number(617,e.font_size) && number(618,e.spacing) && valid_effects(e);
    }
    void changed() {
        if (updating) return;
        const bool mark=selection(611)>0;
        const bool capture_only=selection(612)==1;
        const bool capture_original=capture_only && selection(610)<3;
        if (!capture_original) select(624,0);
        for (int id : {612,613,615,616,618,621}) EnableWindow(GetDlgItem(hwnd,id),mark);
        EnableWindow(GetDlgItem(hwnd,614),selection(610)==4);
        EnableWindow(GetDlgItem(hwnd,623),capture_original);
        EnableWindow(GetDlgItem(hwnd,624),capture_original);
        const bool hidden=capture_original && selection(624)>0;
        EnableWindow(GetDlgItem(hwnd,625),hidden);
        EnableWindow(GetDlgItem(hwnd,626),hidden);
        InvalidateRect(GetDlgItem(hwnd,620),nullptr,FALSE);
    }
    void create() {
        ui::use_manual_dialog_dpi(hwnd);
        dpi=window_dpi(hwnd);
        const auto palette=ui::resolve_palette(theme_mode,system_dark(),accent);
        bg=palette.window_bg; fg=palette.text;
        controls_theme.palette=palette; controls_theme.rtl=rtl;
        controls_theme.apply_dialog_frame(hwnd);
        background=CreateSolidBrush(bg); field=CreateSolidBrush(palette.field_bg);
        const BOOL enabled=dark; DwmSetWindowAttribute(hwnd,20,&enabled,sizeof(enabled));
        SetWindowTextW(hwnd,tr(Text::effects)); SetDlgItemTextW(hwnd,IDOK,tr(Text::apply)); SetDlgItemTextW(hwnd,IDCANCEL,tr(Text::cancel));
        controls_theme.apply_children(hwnd);
        create_font();
        for (int id : {IDOK,IDCANCEL}) {
            const HWND button=GetDlgItem(hwnd,id); RECT base{};
            if (button && GetWindowRect(button,&base)) {
                MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&base),2);
                record(button,id,base,true);
            }
        }
        label(Text::effect_mode,10,8);
        const Text modes[]={Text::original_content,Text::excluded,Text::blanked,Text::solid_cover,Text::label_cover}; combo(610,modes,5,10,22,238);
        label(Text::watermark,10,42); const Text marks[]={Text::mark_off,Text::mark_tiled,Text::mark_center,Text::mark_corner}; combo(611,marks,4,10,56,238);
        label(Text::audience,10,76,114); const Text audience[]={Text::audience_both,Text::audience_capture}; combo(612,audience,2,10,90,114);
        label(Text::capture_route,134,76,114); const Text routes[]={Text::route_a,Text::route_b}; combo(623,routes,2,134,90,114);
        label(Text::preset,10,110); const Text presets[]={Text::preset_custom,Text::preset_confidential,Text::preset_internal,Text::preset_tracking}; combo(621,presets,4,94,107,154);
        label(Text::mark_text,10,126); make(L"EDIT",613,effects.text.c_str(),WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL,10,140,238,28);
        label(Text::cover_text,10,168); make(L"EDIT",614,effects.label.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,10,182,238,14);
        label(Text::tracking_id,10,202); make(L"EDIT",615,effects.tag.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,10,216,238,14);
        for (int id : {613,614,615}) SendDlgItemMessageW(hwnd,id,EM_SETLIMITTEXT,1024,0);
        label(Text::hidden_watermark,10,230,114); const Text algorithms[]={Text::hidden_off,Text::algorithm_dct,Text::algorithm_dwt}; combo(624,algorithms,3,134,232,114);
        label(Text::hidden_payload,10,248,238); make(L"EDIT",625,effects.hidden_payload.c_str(),WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,10,262,238,20);
        label(Text::hidden_key,10,286,238); make(L"EDIT",626,hidden_key.c_str(),WS_TABSTOP|ES_AUTOHSCROLL|ES_PASSWORD,10,300,238,14);
        for (int id : {625,626}) SendDlgItemMessageW(hwnd,id,EM_SETLIMITTEXT,1024,0);
        label(Text::preview,262,8,204); make(L"STATIC",620,L"",SS_OWNERDRAW,262,22,204,108);
        const auto count=std::wstring(tr(Text::effect_targets))+L": "+std::to_wstring(targets);
        make(L"STATIC",0,count.c_str(),SS_LEFT,262,134,204,14);
        const Text numbers[]={Text::opacity,Text::font_size,Text::spacing}; const unsigned values[]={effects.opacity,effects.font_size,effects.spacing};
        for (int i=0;i<3;++i) { label(numbers[i],262,152+i*21,140); make(L"EDIT",616+i,L"",WS_TABSTOP|ES_NUMBER,412,150+i*21,54,14); SetDlgItemInt(hwnd,616+i,values[i],FALSE); }
        make(L"BUTTON",619,tr(Text::foreground),WS_TABSTOP,262,214,98,16); make(L"BUTTON",622,tr(Text::background),WS_TABSTOP,368,214,98,16);
        make(L"EDIT",0,tr(Text::hidden_help),WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,262,236,204,30);
        make(L"EDIT",0,tr(Text::effect_help),WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,262,272,204,42);
        select(610,effects.cover==Cover::solid ? 3 : effects.cover==Cover::label ? 4 : mode==Mode::none ? 0 : mode==Mode::blank ? 2 : 1);
        select(611,static_cast<int>(effects.mark)); select(612,effects.capture_only ? 1 : 0); select(621,0);
        select(623,effects.capture_route==2 ? 1 : 0);
        select(624,effects.hidden_algorithm==1 ? 1 : effects.hidden_algorithm==2 ? 2 : 0);
        updating=false; changed();
        create_font();
        prepare_layout();
    }
    void preview(DRAWITEMSTRUCT& draw) {
        const int w=draw.rcItem.right,h=draw.rcItem.bottom;
        Gdiplus::Bitmap bitmap(w,h,PixelFormat32bppPARGB); Gdiplus::Graphics g(&bitmap);
        Mode m; Effects e;
        if (!read(m,e)) { FillRect(draw.hDC,&draw.rcItem,field); return; }
        const Mode actual=effective_mode(m,e);
        g.Clear(Gdiplus::Color(actual==Mode::blank ? 0xFF000000 : 0xFF506075));
        if (actual==Mode::none) {
            Gdiplus::Font font(L"Segoe UI",14,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel); Gdiplus::SolidBrush brush(Gdiplus::Color(255,235,235,235));
            g.DrawString(sample.title.c_str(),-1,&font,Gdiplus::RectF(10,10,static_cast<float>(w-20),static_cast<float>(h-20)),nullptr,&brush);
        }
        // Scale the sample canvas, preserving the proportions of the live settings.
        const unsigned preview_dpi=static_cast<unsigned>(std::max(30,w*96/std::max(600,sample.width)));
        paint_effects(g,w,h,preview_dpi,sample,e,true); paint_effects(g,w,h,preview_dpi,sample,e,false);
        Gdiplus::Graphics output(draw.hDC); output.DrawImage(&bitmap,0,0,w,h);
    }
    ~Dialog() { if (dialog_font) DeleteObject(dialog_font); if (background) DeleteObject(background); if (field) DeleteObject(field); }
};
INT_PTR CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto* d=reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd,DWLP_USER));
    if (msg==WM_INITDIALOG) { d=reinterpret_cast<Dialog*>(lp); d->hwnd=hwnd; SetWindowLongPtrW(hwnd,DWLP_USER,lp); d->create(); return TRUE; }
    if (!d) return FALSE;
    if (msg==WM_CTLCOLORDLG || msg==WM_CTLCOLORSTATIC || msg==WM_CTLCOLOREDIT || msg==WM_CTLCOLORLISTBOX) {
        const bool field=msg==WM_CTLCOLOREDIT || msg==WM_CTLCOLORLISTBOX || ui::is_field_control(reinterpret_cast<HWND>(lp));
        SetTextColor(reinterpret_cast<HDC>(wp),d->fg); SetBkColor(reinterpret_cast<HDC>(wp),field ? (d->dark ? RGB(24,24,24) : GetSysColor(COLOR_WINDOW)) : d->bg);
        return reinterpret_cast<INT_PTR>(field ? d->field : d->background);
    }
    if (msg==WM_DRAWITEM) {
        auto& draw=*reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw.CtlID==620) { d->preview(draw); return TRUE; }
        return FALSE;
    }
    if (msg==WM_VSCROLL) {
        SCROLLINFO info{sizeof(info)}; info.fMask=SIF_ALL; GetScrollInfo(hwnd,SB_VERT,&info);
        int pos=info.nPos; const int page=static_cast<int>(info.nPage);
        const int line=static_cast<int>(ui::scale_dip(24,d->dpi));
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos-=line; break;
        case SB_LINEDOWN: pos+=line; break;
        case SB_PAGEUP: pos-=page; break;
        case SB_PAGEDOWN: pos+=page; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos=info.nTrackPos; break;
        case SB_TOP: pos=0; break;
        case SB_BOTTOM: pos=d->placement.virtual_height; break;
        default: return TRUE;
        }
        d->scroll_by(pos-d->scroll_offset);
        return TRUE;
    }
    if (msg==WM_MOUSEWHEEL) {
        const int delta=-GET_WHEEL_DELTA_WPARAM(wp)*static_cast<int>(ui::scale_dip(48,d->dpi))/WHEEL_DELTA;
        d->scroll_by(delta); return TRUE;
    }
    if (msg==WM_DPICHANGED) {
        const unsigned next=HIWORD(wp);
        if (next && next!=d->dpi) {
            const unsigned old=d->dpi;
            for (auto& entry : d->entries) {
                entry.base.left=MulDiv(entry.base.left,next,old); entry.base.top=MulDiv(entry.base.top,next,old);
                entry.base.right=MulDiv(entry.base.right,next,old); entry.base.bottom=MulDiv(entry.base.bottom,next,old);
            }
            d->dpi=next;
            d->create_font();
            d->prepare_layout();
        }
        return TRUE;
    }
    if (msg==WM_CLOSE) { EndDialog(hwnd,IDCANCEL); return TRUE; }
    if (msg!=WM_COMMAND) return FALSE;
    if (HIWORD(wp)==BN_SETFOCUS || HIWORD(wp)==EN_SETFOCUS || HIWORD(wp)==CBN_SETFOCUS)
        d->ensure_visible(reinterpret_cast<HWND>(lp));
    const int id=LOWORD(wp);
    if (id==IDCANCEL) { EndDialog(hwnd,IDCANCEL); return TRUE; }
    if (id==IDOK) {
        Mode mode; Effects effects;
        if (!d->read(mode,effects)) { MessageBoxW(hwnd,d->tr(Text::invalid_effects),d->tr(Text::error),MB_OK|MB_ICONERROR); return TRUE; }
        d->mode=mode; d->effects=std::move(effects); EndDialog(hwnd,IDOK); return TRUE;
    }
    if ((id==619 || id==622) && HIWORD(wp)==BN_CLICKED) {
        static COLORREF custom[16]{}; CHOOSECOLORW picker{sizeof(picker)};
        picker.hwndOwner=hwnd; picker.lpCustColors=custom; picker.rgbResult=id==619 ? d->effects.foreground : d->effects.background; picker.Flags=CC_FULLOPEN|CC_RGBINIT;
        if (ChooseColorW(&picker)) { if (id==619) d->effects.foreground=picker.rgbResult; else d->effects.background=picker.rgbResult; d->changed(); } return TRUE;
    }
    if (id==621 && HIWORD(wp)==CBN_SELCHANGE) {
        const Text keys[]={Text::preset_confidential,Text::preset_internal,Text::preset_tracking}; const int choice=d->selection(621);
        if (choice>=1 && choice<=3) { const auto value=std::wstring(d->tr(keys[choice-1]))+L"\r\n{id}  {time}"; SetDlgItemTextW(hwnd,613,value.c_str()); }
    }
    if (HIWORD(wp)==CBN_SELCHANGE || HIWORD(wp)==EN_CHANGE) d->changed();
    return FALSE;
}
}
bool show_effects_dialog(HWND owner,const Profile& profile,const Window& sample,std::size_t targets,Mode& mode,Effects& effects,std::wstring* hidden_key) {
    Dialog dialog; dialog.sample=sample; dialog.targets=targets; dialog.mode=mode; dialog.effects=effects;
    dialog.hidden_key=hidden_key ? *hidden_key : std::wstring();
    dialog.language=choose_language(profile.language,system_language()); dialog.dark=profile.theme==Theme::dark || (profile.theme==Theme::system && system_dark());
    dialog.rtl=ui::is_rtl_language(dialog.language); dialog.theme_mode=static_cast<int>(profile.theme); dialog.accent=profile.accent;
    const auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(600),owner,procedure,reinterpret_cast<LPARAM>(&dialog));
    if (result==IDOK) { mode=dialog.mode; effects=std::move(dialog.effects); if (hidden_key) *hidden_key=dialog.hidden_key; return true; }
    if (result==-1) { const auto message=error_text(GetLastError()); MessageBoxW(owner,message.c_str(),dialog.tr(Text::error),MB_OK|MB_ICONERROR); }
    return false;
}
}
