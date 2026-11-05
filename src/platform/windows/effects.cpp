// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/effects.h"
#include "core/automation.h"
#include <dwmapi.h>
#include <algorithm>
#include <map>

namespace av {
std::wstring local_mark_time() {
    SYSTEMTIME t{}; GetLocalTime(&t); wchar_t buffer[32]{};
    swprintf_s(buffer,L"%04u-%02u-%02u %02u:%02u:%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    return buffer;
}
static Gdiplus::Color color(unsigned rgb,unsigned alpha=255) {
    return Gdiplus::Color(static_cast<BYTE>(alpha),GetRValue(rgb),GetGValue(rgb),GetBValue(rgb));
}
void paint_effects(Gdiplus::Graphics& g,int width,int height,unsigned dpi,const Window& target,const Effects& e,bool replacement) {
    using namespace Gdiplus;
    const REAL scale=static_cast<REAL>(dpi)/96;
    g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    StringFormat format; format.SetAlignment(StringAlignmentCenter); format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    const auto time=local_mark_time();
    if (replacement && e.cover!=Cover::none) {
        g.Clear(color(e.background));
        if (e.cover==Cover::label) {
            Font font(L"Segoe UI",e.font_size*scale,FontStyleRegular,UnitPixel);
            SolidBrush brush(color(e.foreground));
            const auto label=expand_mark(e.label,target,e.tag,time);
            g.DrawString(label.c_str(),static_cast<INT>(label.size()),&font,RectF(16*scale,12*scale,std::max(1.0f,width-32*scale),std::max(1.0f,height-24*scale)),&format,&brush);
        }
    }
    if (e.mark==Mark::none || (replacement && !e.capture_only) || (!replacement && e.capture_only)) return;
    Font font(L"Segoe UI",e.font_size*scale,FontStyleRegular,UnitPixel);
    SolidBrush brush(color(e.foreground,e.opacity*255/100));
    const auto text=expand_mark(e.text,target,e.tag,time);
    if (e.mark==Mark::tiled) {
        const REAL step=e.spacing*scale, line=std::max(80.0f*scale,step*0.6f);
        for (REAL y=-line;y<height+line;y+=line) for (REAL x=-step;x<width+step;x+=step) {
            const auto saved=g.Save(); g.TranslateTransform(x+step/2,y+line/2); g.RotateTransform(-25);
            g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,RectF(-step*0.47f,-line/2,step*0.94f,line),&format,&brush); g.Restore(saved);
        }
    } else {
        RectF box(12*scale,12*scale,std::max(1.0f,width-24*scale),std::max(1.0f,height-24*scale));
        if (e.mark==Mark::corner) { format.SetAlignment(StringAlignmentFar); format.SetLineAlignment(StringAlignmentFar); }
        g.DrawString(text.c_str(),static_cast<INT>(text.size()),&font,box,&format,&brush);
    }
}
struct EffectLayers::State {
    struct Entry {
        Window target; Effects effects;
        HWND behind=nullptr, above=nullptr;
        int width=0,height=0; unsigned dpi=96; ULONGLONG second=0;
        DWORD problem=0;
        ~Entry() { if (above) DestroyWindow(above); if (behind) DestroyWindow(behind); }
    };
    HWND control=nullptr;
    HWND observer=nullptr; UINT notification=0;
    HWINEVENTHOOK hook=nullptr;
    std::map<WindowKey,std::unique_ptr<Entry>> entries;
    static thread_local State* current;
    static constexpr std::uint64_t max_pixels=32ull*1024*1024;
    static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<State*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (msg==WM_NCCREATE) { self=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if (self && (msg==WM_TIMER || msg==WM_APP+30)) { self->sync(); return 0; }
        if (msg==WM_NCHITTEST) return HTTRANSPARENT;
        if (msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg==WM_ERASEBKGND) return 1;
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    static void CALLBACK event(HWINEVENTHOOK,DWORD,HWND hwnd,LONG object,LONG,DWORD,DWORD) {
        auto* self=current;
        if (!self || object!=OBJID_WINDOW) return;
        for (const auto& pair : self->entries) if (reinterpret_cast<HWND>(pair.second->target.handle)==hwnd) {
            PostMessageW(self->control,WM_APP+30,0,0); break;
        }
    }
    bool initialize(DWORD& error) {
        if (control) return true;
        WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc=proc; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"Henglie.AkashaVeil.EffectLayer";
        if (!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { error=GetLastError(); return false; }
        control=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);
        if (!control) { error=GetLastError(); return false; }
        current=this;
        hook=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_LOCATIONCHANGE,nullptr,event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        return true;
    }
    HWND layer() {
        return CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
            L"Henglie.AkashaVeil.EffectLayer",L"",WS_POPUP,0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    }
    static bool geometry(const Entry& e,RECT& r,unsigned& dpi) {
        const HWND target=reinterpret_cast<HWND>(e.target.handle);
        if (!same_window(e.target) || !GetClientRect(target,&r)) return false;
        POINT p{r.left,r.top}; if (!ClientToScreen(target,&p)) return false;
        OffsetRect(&r,p.x,p.y);
        using GetDpi=UINT(WINAPI*)(HWND);
        if (const auto get=reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"))) dpi=get(target);
        return r.right>r.left && r.bottom>r.top;
    }
    bool draw(HWND layer,const Entry& e,const RECT& r,unsigned dpi,bool replacement,DWORD& error) {
        const int w=r.right-r.left,h=r.bottom-r.top;
        HDC dc=CreateCompatibleDC(nullptr); if (!dc) { error=GetLastError(); return false; }
        BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=w; info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr; HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if (!bitmap) { error=GetLastError(); DeleteDC(dc); return false; }
        const auto old=SelectObject(dc,bitmap);
        bool ok=false;
        {
            Gdiplus::Bitmap surface(w,h,w*4,PixelFormat32bppPARGB,static_cast<BYTE*>(bits));
            Gdiplus::Graphics graphics(&surface); graphics.Clear(Gdiplus::Color(0,0,0,0));
            paint_effects(graphics,w,h,dpi,e.target,e.effects,replacement); graphics.Flush(Gdiplus::FlushIntentionSync);
            POINT position{r.left,r.top},source{}; SIZE size{w,h}; BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            ok=UpdateLayeredWindow(layer,nullptr,&position,&size,dc,&source,0,&blend,ULW_ALPHA)!=FALSE;
            if (!ok) error=GetLastError();
        }
        SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); return ok;
    }
    bool update(Entry& e,bool force,DWORD& error) {
        const HWND target=reinterpret_cast<HWND>(e.target.handle);
        RECT r{}; unsigned dpi=96; DWORD cloaked=0,affinity=0;
        DwmGetWindowAttribute(target,14,&cloaked,sizeof(cloaked));
        const bool requires_exclusion=e.behind!=nullptr;
        if (!IsWindowVisible(target) || IsIconic(target) || cloaked || !geometry(e,r,dpi)) {
            if (e.behind) ShowWindow(e.behind,SW_HIDE);
            if (e.above) ShowWindow(e.above,SW_HIDE);
            return true;
        }
        if (requires_exclusion && (!GetWindowDisplayAffinity(target,&affinity) || affinity!=0x11)) { error=ERROR_INVALID_STATE; return false; }
        const int length=GetWindowTextLengthW(target);
        bool title_changed=false;
        if (length>=0 && length<=32760) {
            std::wstring title(static_cast<std::size_t>(length)+1,L'\0'); title.resize(GetWindowTextW(target,title.data(),length+1));
            title_changed=title!=e.target.title; e.target.title=std::move(title);
        }
        const int w=r.right-r.left,h=r.bottom-r.top;
        std::uint64_t pixels=0;
        for (const auto& p : entries) {
            const auto& item=*p.second;
            pixels+=static_cast<std::uint64_t>(std::max(0,item.width))*std::max(0,item.height)*((item.above!=nullptr)+(item.behind!=nullptr));
        }
        pixels-=static_cast<std::uint64_t>(std::max(0,e.width))*std::max(0,e.height)*((e.above!=nullptr)+(e.behind!=nullptr));
        pixels+=static_cast<std::uint64_t>(w)*h*((e.above!=nullptr)+(e.behind!=nullptr));
        if (pixels>max_pixels) { error=ERROR_NOT_ENOUGH_MEMORY; if (e.behind) ShowWindow(e.behind,SW_HIDE); if (e.above) ShowWindow(e.above,SW_HIDE); return false; }
        const auto second=GetTickCount64()/1000;
        const bool dynamic=e.effects.text.find(L"{time}")!=std::wstring::npos || e.effects.label.find(L"{time}")!=std::wstring::npos;
        const bool redraw=force || title_changed || e.width!=w || e.height!=h || e.dpi!=dpi || (dynamic && second!=e.second);
        if (redraw) {
            if (e.behind && !draw(e.behind,e,r,dpi,true,error)) return false;
            if (e.above && !draw(e.above,e,r,dpi,false,error)) return false;
            e.width=w; e.height=h; e.dpi=dpi; e.second=second;
        }
        if (e.behind && !SetWindowPos(e.behind,target,r.left,r.top,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW)) { error=GetLastError(); return false; }
        if (e.above) {
            HWND previous=GetWindow(target,GW_HWNDPREV);
            while (previous && (previous==e.above || previous==e.behind)) previous=GetWindow(previous,GW_HWNDPREV);
            if (!SetWindowPos(e.above,previous ? previous : HWND_TOP,r.left,r.top,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW)) { error=GetLastError(); return false; }
        }
        return true;
    }
    void sync() {
        for (auto it=entries.begin();it!=entries.end();) {
            if (!same_window(it->second->target)) { it=entries.erase(it); continue; }
            auto& entry=*it->second; DWORD error=0;
            if (!update(entry,false,error)) { if (!error) error=ERROR_GEN_FAILURE; if (entry.above) ShowWindow(entry.above,SW_HIDE); if (entry.behind) ShowWindow(entry.behind,SW_HIDE); }
            if (entry.problem!=error) {
                entry.problem=error;
                if (observer && notification) PostMessageW(observer,notification,error,0);
            }
            ++it;
        }
        if (entries.empty() && control) KillTimer(control,1);
    }
    ~State() {
        entries.clear(); if (hook) UnhookWinEvent(hook); if (control) DestroyWindow(control); if (current==this) current=nullptr;
    }
};
thread_local EffectLayers::State* EffectLayers::State::current=nullptr;
EffectLayers::EffectLayers() : state(std::make_unique<State>()) {}
EffectLayers::~EffectLayers() = default;
bool EffectLayers::set(const Window& target,const Effects& effects,DWORD& error) {
    error=ERROR_SUCCESS;
    if (!valid_effects(effects)) { error=ERROR_INVALID_DATA; return false; }
    if (!same_window(target)) { error=ERROR_INVALID_WINDOW_HANDLE; return false; }
    if (!has_effects(effects)) { remove(target); return true; }
    if (!state->initialize(error)) return false;
    const auto key=window_key(target);
    if (!state->entries.count(key) && state->entries.size()>=16) { error=ERROR_NOT_ENOUGH_MEMORY; return false; }
    auto entry=std::make_unique<State::Entry>(); entry->target=target; entry->effects=effects;
    const bool behind=effects.cover!=Cover::none || (effects.mark!=Mark::none && effects.capture_only);
    if (behind) {
        DWORD affinity=0;
        if (!GetWindowDisplayAffinity(reinterpret_cast<HWND>(target.handle),&affinity) || affinity!=0x11) { error=ERROR_NOT_SUPPORTED; return false; }
        if (GetWindowLongPtrW(reinterpret_cast<HWND>(target.handle),GWL_EXSTYLE)&WS_EX_LAYERED) { error=ERROR_NOT_SUPPORTED; return false; }
        entry->behind=state->layer(); if (!entry->behind) { error=GetLastError(); return false; }
    }
    if (effects.mark!=Mark::none && !effects.capture_only) { entry->above=state->layer(); if (!entry->above) { error=GetLastError(); return false; } }
    // Include the candidate in the resource budget before allocating a surface.
    auto previous=state->entries.extract(key);
    state->entries[key]=std::move(entry);
    if (!state->update(*state->entries[key],true,error) || !SetTimer(state->control,1,100,nullptr)) {
        if (!error) error=ERROR_NO_SYSTEM_RESOURCES;
        state->entries.erase(key); if (!previous.empty()) state->entries.insert(std::move(previous)); return false;
    }
    return true;
}
const Effects* EffectLayers::get(const Window& target) const {
    const auto it=state->entries.find(window_key(target)); return it==state->entries.end() ? nullptr : &it->second->effects;
}
DWORD EffectLayers::problem(const Window& target) const { const auto it=state->entries.find(window_key(target)); return it==state->entries.end() ? 0 : it->second->problem; }
void EffectLayers::notify_errors(HWND window,UINT message) { state->observer=window; state->notification=message; }
void EffectLayers::remove(const Window& target) { state->entries.erase(window_key(target)); if (state->entries.empty() && state->control) KillTimer(state->control,1); }
void EffectLayers::clear() { state->entries.clear(); if (state->control) KillTimer(state->control,1); }
void EffectLayers::sync() { state->sync(); }
}
