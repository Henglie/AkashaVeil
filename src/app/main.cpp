// SPDX-License-Identifier: Apache-2.0
#include "core/i18n.h"
#include "core/automation.h"
#include "core/command_line.h"
#include "app/cli.h"
#include "app/layout.h"
#include "app/rtl_layout.h"
#include "app/preferences.h"
#include "app/effects_dialog.h"
#include "app/watermark_files_dialog.h"
#include "app/rules_dialog.h"
#include "app/about.h"
#include "app/i18n_ui_strings.h"
#include "platform/windows/effects.h"
#include "platform/windows/profile.h"
#include "platform/windows/profile_v4.h"
#include "platform/windows/capture.h"
#include "platform/windows/frame_render.h"
#include "platform/windows/payload.h"
#include "platform/windows/proxy_source.h"
#include "platform/windows/taskbar.h"
#include "platform/windows/self_privacy.h"
#include "platform/windows/tray.h"
#include "platform/windows/watermark_session.h"
#include "version.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace {
using av::Text;
av::Profile legacy_profile(const av::ProfileV4& source) {
    av::Profile profile;
    profile.language = source.language;
    profile.theme = source.theme;
    profile.accent = source.accent;
    profile.preferences = source.preferences;
    profile.effect_mode = static_cast<av::Mode>(static_cast<unsigned>(source.defaults.affinity));
    profile.effects = av::effects_from_request(source.defaults);
    for (const auto& item : source.rules) {
        if (!item.enabled) continue;
        av::Rule rule;
        rule.path = item.path;
        rule.class_name = item.class_name;
        rule.title = item.title;
        rule.mode = static_cast<av::Mode>(static_cast<unsigned>(item.request.affinity));
        rule.x = item.x;
        rule.y = item.y;
        rule.width = item.width;
        rule.height = item.height;
        rule.show = item.show;
        rule.effects = av::effects_from_request(item.request);
        profile.rules.push_back(std::move(rule));
    }
    return profile;
}
av::ProfileV4 modern_profile(const av::Profile& source) {
    return av::migrate_profile_v3(source, 3);
}
constexpr UINT completed = WM_APP + 1;
constexpr UINT tray_event = WM_APP + 2;
constexpr UINT layer_problem = WM_APP + 3;
enum Id {
    filter = 100, refresh, exclude, blank, restore, remember, save, load, geometry, export_list, apply_rules,
    exit_app = 200, about, theme_system, theme_light, theme_dark, accent, lang_auto, lang_zh, lang_en,
    lang_es, lang_fr, lang_ru, lang_ar, lang_tr, lang_ja, lang_ko,
    self_exclude, self_restore, select_all, clear_selection, view_windows, view_processes,
    settings = 300, tabs, action_group, profile_group, exclude_all, restore_all, preferences, show_window,
    effects, selection_summary, batch_group, clear_filter, watermark_files,
    rules_dialog, target_taskbar_hide, target_taskbar_show, help = 400
};
struct Result {
    bool enumeration = false;
    bool automatic = false, publish_windows = true;
    av::AutoTracker tracker;
    std::vector<av::Window> windows;
    unsigned ok = 0, failed = 0;
    DWORD error = 0;
    std::wstring target;
    std::vector<std::pair<av::Window,av::Effects>> applied;
};
LRESULT CALLBACK header_procedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK tab_procedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
struct App {
    HWND hwnd = nullptr, list = nullptr, edit = nullptr, status = nullptr, tab = nullptr, tips = nullptr;
    HINSTANCE instance = nullptr;
    HFONT font = nullptr;
    HBRUSH background = nullptr, surface = nullptr;
    COLORREF bg = 0, fg = 0, panel = 0;
    RECT action_rect{}, profile_rect{};
    UINT dpi = 96;
    bool rtl = false;
    int side_scroll = 0;
    av::Profile profile;
    std::wstring language, config_path;
    std::vector<av::Window> windows;
    std::vector<std::size_t> rows;
    std::vector<HWND> buttons;
    std::thread worker;
    std::unique_ptr<Result> pending;
    std::atomic_bool cancel{false};
    bool working = false, process_view = false, dark = false;
    bool force_exit = false, preferences_open = false, shown = false, auto_initialized = false;
    bool taskbar_style_dirty = false, initial_dpi_pending = true;
    av::AutoTracker auto_tracker;
    av::Tray tray;
    av::TaskbarLedger taskbar;
    av::SelfPrivacy privacy;
    UINT taskbar_created = 0;
    ULONGLONG last_refresh = 0, last_auto = 0;
    std::wstring randomized_title, randomized_tip;
    struct MenuLabel { const wchar_t* text; bool submenu; bool checked; };
    std::array<MenuLabel, 32> menu_labels{};
    unsigned menu_label_count = 0;
    int sort_column = 0;
    bool descending = false;
    bool rendering = false;
    HIMAGELIST images = nullptr;
    std::map<std::wstring,int> icon_cache;
    av::EffectLayers layers;
    struct CaptureSession {
        std::unique_ptr<av::CaptureOnlyRenderSession> session;
        av::Window window;
        av::Effects effects;
    };
    struct ProxySession {
        std::unique_ptr<av::ProxySourceSession> session;
        av::Window window;
        av::Effects effects;
    };
    std::map<av::WindowKey, CaptureSession> capture_sessions;
    std::map<av::WindowKey, ProxySession> proxy_sessions;
    std::map<av::WindowKey, std::wstring> session_keys;
    std::wstring filters[2];
    std::wstring rendered_query;
    const wchar_t* tr(Text key) const { return av::text(key, language); }
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    av::AboutLabels about_labels() const {
        return {tr(Text::about_title), tr(Text::about_version_label), tr(Text::about_author_line),
            tr(Text::about_copyright_line), tr(Text::about_privacy_line), tr(Text::about_tab),
            tr(Text::license_tab), tr(Text::third_party_tab), tr(Text::help_tab), tr(Text::contribute),
            tr(Text::copy_info), tr(Text::close), tr(Text::copied), tr(Text::contribution_title),
            tr(Text::contribution_hint), tr(Text::contribution_alt), tr(Text::contribution_missing),
            tr(Text::license_missing), tr(Text::third_party_missing), tr(Text::help_missing),
            tr(Text::about_missing)};
    }
    void show_about_dialog(int initial_tab) const {
        av::AboutOptions options;
        options.instance = instance;
        options.version = AV_VERSION;
        options.architecture = AV_ARCH;
        options.language = language.c_str();
        options.dark = dark;
        options.rtl = rtl;
        options.background = bg;
        options.panel = panel;
        options.foreground = fg;
        options.dpi = dpi;
        av::show_about(hwnd, options, about_labels(), initial_tab);
    }
    ~App() {
        stop_all_sessions();
        if (worker.joinable()) worker.join();
        if (images) ImageList_Destroy(images);
        if (font) DeleteObject(font);
        if (background) DeleteObject(background);
        if (surface) DeleteObject(surface);
    }
    void message(const std::wstring& s) { SetWindowTextW(status, s.c_str()); }
    void selection_changed() {
        if (rendering) return;
        const auto targets=selected();
        std::wstring text = tr(Text::list_selection_summary);
        auto put = [&](std::size_t index, std::size_t value) {
            const std::wstring marker = L"{" + std::to_wstring(index) + L"}";
            const auto position = text.find(marker);
            if (position != std::wstring::npos) text.replace(position, marker.size(), std::to_wstring(value));
        };
        put(0, rows.size()); put(1, static_cast<std::size_t>(ListView_GetSelectedCount(list))); put(2, targets.size());
        SetWindowTextW(GetDlgItem(hwnd,selection_summary),text.c_str());
        if (working) return;
        for (const int id : {exclude,blank,restore,remember,effects}) EnableWindow(GetDlgItem(hwnd,id),!targets.empty());
        for (const int id : {exclude_all,restore_all,select_all,export_list}) EnableWindow(GetDlgItem(hwnd,id),!rows.empty());
        EnableWindow(GetDlgItem(hwnd,clear_selection),!targets.empty());
        for (const int id : {apply_rules,geometry}) EnableWindow(GetDlgItem(hwnd,id),!profile.rules.empty());
    }
    int icon_for(const av::Window& window) {
        const auto key=window.path.empty() ? window.class_name : window.path;
        const auto found=icon_cache.find(key); if (found!=icon_cache.end()) return found->second;
        if (icon_cache.size()>=256) return 0;
        HICON icon=nullptr; bool owned=false;
        const HWND target=reinterpret_cast<HWND>(window.handle);
        DWORD_PTR result=0;
        if (SendMessageTimeoutW(target,WM_GETICON,ICON_SMALL2,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,30,&result)) icon=reinterpret_cast<HICON>(result);
        if (!icon) icon=reinterpret_cast<HICON>(GetClassLongPtrW(target,GCLP_HICONSM));
        if (!icon && !window.path.empty() && window.path.rfind(L"\\\\",0)!=0) {
            SHFILEINFOW info{};
            if (SHGetFileInfoW(window.path.c_str(),0,&info,sizeof(info),SHGFI_ICON|SHGFI_SMALLICON)) { icon=info.hIcon; owned=true; }
        }
        if (!icon) icon=LoadIconW(nullptr,IDI_APPLICATION);
        HDC screen=GetDC(hwnd),dc=CreateCompatibleDC(screen); HBITMAP bitmap=CreateCompatibleBitmap(screen,px(20),px(30));
        const auto old=SelectObject(dc,bitmap); RECT rect{0,0,px(20),px(30)};
        HBRUSH mask=CreateSolidBrush(RGB(255,0,255)); FillRect(dc,&rect,mask); DeleteObject(mask);
        DrawIconEx(dc,px(2),px(7),icon,px(16),px(16),0,nullptr,DI_NORMAL);
        SelectObject(dc,old);
        const int index=ImageList_AddMasked(images,bitmap,RGB(255,0,255));
        DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(hwnd,screen); if (owned) DestroyIcon(icon);
        icon_cache[key]=std::max(0,index); return std::max(0,index);
    }
    static bool needs_capture_session(const av::Effects& effects) {
        return effects.capture_only && effects.cover == av::Cover::none
            && (effects.mark != av::Mark::none || effects.hidden_algorithm != 0);
    }
    static av::TargetMetadata target_metadata(const av::Window& window) {
        av::TargetMetadata metadata;
        metadata.handle = window.handle;
        metadata.pid = window.pid;
        metadata.process_created = window.process_created;
        metadata.title = window.title;
        metadata.path = window.path;
        metadata.class_name = window.class_name;
        return metadata;
    }
    static unsigned window_dpi(HWND window) {
        using GetDpi = UINT(WINAPI*)(HWND);
        if (const auto get = reinterpret_cast<GetDpi>(GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))) {
            const UINT value = get(window);
            if (value >= 48 && value <= 960) return value;
        }
        HDC dc = GetDC(window);
        const int value = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(window, dc);
        return value >= 48 && value <= 960 ? static_cast<unsigned>(value) : 96;
    }
    static std::vector<std::uint8_t> utf8_text(const std::wstring& value) {
        std::vector<std::uint8_t> out;
        if (value.empty()) return out;
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return out;
        out.resize(static_cast<std::size_t>(size));
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), reinterpret_cast<char*>(out.data()),
                size, nullptr, nullptr) != size)
            out.clear();
        return out;
    }
    static av::WatermarkPlan watermark_plan(const av::Effects& effects,
        const std::wstring& key, unsigned dpi) {
        av::WatermarkPlan plan;
        const bool hidden = effects.hidden_algorithm != 0;
        if (effects.mark != av::Mark::none)
            plan.visibility = hidden
                ? av::WatermarkVisibility::capture_only_explicit_hidden
                : av::WatermarkVisibility::capture_only_explicit;
        else
            plan.visibility = av::WatermarkVisibility::capture_only_hidden;
        plan.capture_only_route = effects.capture_route == 2
            ? av::CaptureOnlyRoute::proxy_source_b
            : av::CaptureOnlyRoute::display_capture_a;
        plan.explicit_layer = av::explicit_watermark_for_capture(effects, dpi);
        if (hidden) {
            const auto payload = effects.hidden_payload_bytes.empty()
                ? utf8_text(effects.hidden_payload) : effects.hidden_payload_bytes;
            plan.hidden_layer = av::make_hidden_watermark(
                static_cast<std::uint16_t>(effects.hidden_algorithm), payload,
                utf8_text(key));
        }
        return plan;
    }
    bool has_session(const av::WindowKey& key) const {
        return capture_sessions.count(key) || proxy_sessions.count(key);
    }
    void update_session_timer() {
        if (!hwnd || !IsWindow(hwnd)) return;
        if (capture_sessions.empty() && proxy_sessions.empty())
            KillTimer(hwnd, 8);
        else if (!SetTimer(hwnd, 8, 100, nullptr))
            fail(ERROR_NO_SYSTEM_RESOURCES);
    }
    void stop_capture_session(const av::WindowKey& key) {
        if (const auto found = capture_sessions.find(key); found != capture_sessions.end()) {
            if (found->second.session) found->second.session->stop();
            capture_sessions.erase(found);
        }
        if (const auto found = proxy_sessions.find(key); found != proxy_sessions.end()) {
            if (found->second.session) found->second.session->stop();
            proxy_sessions.erase(found);
        }
        session_keys.erase(key);
        update_session_timer();
    }
    void stop_capture_sessions(const std::vector<av::Window>& targets) {
        for (const auto& window : targets) stop_capture_session(av::window_key(window));
    }
    void stop_all_sessions() {
        std::vector<av::WindowKey> keys;
        for (const auto& item : capture_sessions) keys.push_back(item.first);
        for (const auto& item : proxy_sessions) keys.push_back(item.first);
        for (const auto& key : keys) stop_capture_session(key);
        update_session_timer();
    }
    bool start_capture_session(const av::Window& window, const av::Effects& effects,
        const std::wstring& key, std::wstring& detail) {
        detail.clear();
        if (!av::same_window(window)) {
            detail = tr(Text::capture_target_unavailable);
            return false;
        }
        const auto key_id = av::window_key(window);
        stop_capture_session(key_id);
        if (capture_sessions.size() + proxy_sessions.size() >= 8) {
            detail = tr(Text::capture_limit);
            return false;
        }
        DWORD affinity_error = 0;
        if (!av::apply_affinity(window, av::Mode::none, affinity_error)) {
            detail = av::error_text(affinity_error);
            return false;
        }
        layers.remove(window);
        const unsigned target_dpi = window_dpi(reinterpret_cast<HWND>(window.handle));
        const auto plan = watermark_plan(effects, key, target_dpi);
        av::HiddenEmbedderSet embedders{
            av::make_dct_embedder(), av::make_dwt_embedder()};
        av::CompositionError composition_error;
        if (!av::WatermarkFrameComposer::validate_plan(plan, embedders,
                composition_error)) {
            detail = composition_error.detail.empty()
                ? tr(Text::watermark_plan_failed) : composition_error.detail;
            return false;
        }
        if (effects.capture_route == 2) {
            auto session = std::make_unique<av::ProxySourceSession>();
            av::ProxySourceConfig config;
            config.target = target_metadata(window);
            config.plan = plan;
            config.title = tr(Text::capture_proxy_title);
            config.cloak = true;
            config.fps = 30;
            if (!session->start(config, composition_error)) {
                detail = composition_error.detail.empty()
                    ? tr(Text::capture_proxy_failed) : composition_error.detail;
                return false;
            }
            proxy_sessions[key_id] = ProxySession{std::move(session), window, effects};
        } else {
            auto session = std::make_unique<av::CaptureOnlyRenderSession>();
            av::CaptureOnlyRenderConfig config;
            config.pipeline.source.kind = av::CaptureSourceKind::window;
            config.pipeline.source.window = reinterpret_cast<HWND>(window.handle);
            config.pipeline.target = target_metadata(window);
            config.pipeline.preferred_backend = av::CaptureBackendKind::automatic;
            config.pipeline.watermark = plan.explicit_layer;
            config.pipeline.hidden.enabled = effects.hidden_algorithm != 0;
            config.pipeline.hidden.packet = plan.hidden_layer.packet;
            config.pipeline.hidden.key = plan.hidden_layer.key;
            config.pipeline.hidden_embedder = effects.hidden_algorithm == 2
                ? embedders.dwt : embedders.dct;
            config.sync_interval_ms = 100;
            av::PipelineError pipeline_error;
            if (!session->start(config, pipeline_error)) {
                detail = pipeline_error.detail.empty()
                    ? tr(Text::display_capture_failed)
                    : pipeline_error.detail;
                return false;
            }
            capture_sessions[key_id] = CaptureSession{std::move(session), window, effects};
        }
        session_keys[key_id] = key;
        update_session_timer();
        return true;
    }
    void poll_sessions() {
        for (auto it = capture_sessions.begin(); it != capture_sessions.end();) {
            std::map<av::WindowKey, CaptureSession>::iterator current = it++;
            auto& entry = current->second;
            entry.session->sync();
            const auto snapshot = entry.session->status();
            if (snapshot.state == av::RenderSessionState::target_lost
                || snapshot.state == av::RenderSessionState::failed
                || snapshot.state == av::RenderSessionState::stopped) {
                const auto detail = snapshot.detail;
                entry.session->stop();
                session_keys.erase(current->first);
                capture_sessions.erase(current);
                message(std::wstring(tr(Text::capture_session_end)) + L" " + detail);
            }
        }
        for (auto it = proxy_sessions.begin(); it != proxy_sessions.end();) {
            std::map<av::WindowKey, ProxySession>::iterator current = it++;
            auto& entry = current->second;
            const auto snapshot = entry.session->status();
            if (snapshot.state == av::ProxySourceState::target_gone
                || snapshot.state == av::ProxySourceState::failed
                || snapshot.state == av::ProxySourceState::stopped) {
                const auto detail = snapshot.detail;
                entry.session->stop();
                session_keys.erase(current->first);
                proxy_sessions.erase(current);
                message(std::wstring(tr(Text::capture_session_failed)) + L" " + detail);
            }
        }
        update_session_timer();
    }
    std::wstring state_for(const av::Window& w) const {
        auto result=std::wstring(tr(!w.affinity_known ? Text::unknown : w.mode==av::Mode::exclude ? Text::excluded : w.mode==av::Mode::blank ? Text::blanked : Text::normal));
        if (const auto* e=layers.get(w)) {
            if (layers.problem(w)) return result+L" + "+tr(Text::error);
            if (e->cover!=av::Cover::none) result=tr(e->cover==av::Cover::solid ? Text::solid_cover : Text::label_cover);
            if (e->mark!=av::Mark::none) result+=L" + "+std::wstring(tr(Text::effect_active));
        }
        if (has_session(av::window_key(w))) result+=L" + "+std::wstring(tr(Text::effect_active));
        return result;
    }
    void draw_row(NMLVCUSTOMDRAW& draw) {
        const int index=static_cast<int>(draw.nmcd.dwItemSpec);
        if (index<0 || static_cast<std::size_t>(index)>=rows.size()) return;
        const auto& window=windows[rows[index]];
        const UINT state=ListView_GetItemState(list,index,LVIS_SELECTED|LVIS_FOCUSED);
        const bool selected=(state&LVIS_SELECTED)!=0;
        COLORREF fill=panel,ink=fg;
        if (index%2) {
            const int shift=dark ? 9 : -10;
            fill=RGB(std::clamp<int>(GetRValue(panel)+shift,0,255),std::clamp<int>(GetGValue(panel)+shift,0,255),std::clamp<int>(GetBValue(panel)+shift,0,255));
        }
        if ((window.affinity_known && window.mode!=av::Mode::none) || layers.get(window)) {
            const auto color=profile.accent;
            fill=RGB((3*GetRValue(fill)+GetRValue(color))/4,(3*GetGValue(fill)+GetGValue(color))/4,(3*GetBValue(fill)+GetBValue(color))/4);
        }
        if (selected) { fill=dark ? RGB(57,83,112) : GetSysColor(COLOR_HIGHLIGHT); ink=dark ? RGB(255,255,255) : GetSysColor(COLOR_HIGHLIGHTTEXT); }
        RECT row{}; ListView_GetItemRect(list,index,&row,LVIR_BOUNDS);
        const HDC dc=draw.nmcd.hdc; const int saved=SaveDC(dc); IntersectClipRect(dc,row.left,row.top,row.right,row.bottom);
        const HBRUSH brush=CreateSolidBrush(fill); FillRect(dc,&row,brush); DeleteObject(brush);
        SetTextColor(dc,ink); SetBkMode(dc,TRANSPARENT); SelectObject(dc,font);
        const auto direction = rtl ? av::ui::TextDirection::rtl : av::ui::TextDirection::ltr;
        for (int column=0;column<5;++column) {
            RECT cell{}; ListView_GetSubItemRect(list,index,column,LVIR_BOUNDS,&cell);
            if (!column) {
                // Subitem zero returns the whole row; its column can be at either edge.
                RECT heading{}; const HWND header=ListView_GetHeader(list);
                Header_GetItemRect(header,0,&heading);
                MapWindowPoints(header,list,reinterpret_cast<POINT*>(&heading),2);
                cell.left=heading.left; cell.right=heading.right;
                const int icon_x = rtl ? cell.right-px(22) : cell.left+px(3);
                ImageList_Draw(images,icon_for(window),dc,icon_x,cell.top+(cell.bottom-cell.top-px(30))/2,ILD_TRANSPARENT);
                if (rtl) cell.right-=px(26); else cell.left+=px(26);
            } else cell.left+=px(6);
            cell.right-=px(6);
            wchar_t value[4096]{}; ListView_GetItemText(list,index,column,value,4096);
            const auto kind = static_cast<av::ui::ColumnKind>(column);
            const UINT flags = av::ui::field_text_flags(kind, direction,
                DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            DrawTextW(dc,av::ui::field_text(kind,direction,value).c_str(),-1,&cell,flags);
        }
        if ((state&LVIS_FOCUSED) && GetFocus()==list) { auto focus=row; InflateRect(&focus,-1,-1); DrawFocusRect(dc,&focus); }
        RestoreDC(dc,saved);
    }
    void scroll_to(const av::WindowKey& key) {
        RECT row_rect{};
        if (!ListView_GetItemRect(list,0,&row_rect,LVIR_BOUNDS)) return;
        for (std::size_t row=0;row<rows.size();++row) if (av::window_key(windows[rows[row]])==key) {
            ListView_Scroll(list,0,(static_cast<int>(row)-ListView_GetTopIndex(list))*(row_rect.bottom-row_rect.top)); break;
        }
    }
    void fail(DWORD error) { message(std::wstring(tr(Text::error)) + L" " + av::error_text(error)); }
    void restore_main() {
        ShowWindow(hwnd,SW_RESTORE); SetForegroundWindow(hwnd);
        const HWND popup=GetLastActivePopup(hwnd);
        if (popup!=hwnd && IsWindowVisible(popup)) SetForegroundWindow(popup);
    }
    void update_runtime() {
        const auto& options=profile.preferences;
        if (randomized_tip.empty()) randomized_tip=av::random_label();
        if (randomized_title.empty()) randomized_title=av::random_label();
        const LPCWSTR icons[]={IDI_APPLICATION,IDI_INFORMATION,IDI_WARNING,IDI_QUESTION,IDI_ERROR};
        const std::size_t selected_icon=options.random_tray_icon ? static_cast<std::size_t>(randomized_tip.back())%5 : 0;
        static HICON brand_icon=nullptr;
        if (!brand_icon) brand_icon=av::load_app_icon(instance,GetSystemMetrics(SM_CXSMICON));
        const HICON icon=options.random_tray_icon ? LoadIconW(nullptr,icons[selected_icon]) : (brand_icon ? brand_icon : LoadIconW(nullptr,icons[0]));
        if (!tray.update(options.tray,icon,options.random_tooltip ? randomized_tip : tr(Text::title))) fail(ERROR_NOT_READY);
        const auto old_style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
        const auto style=(old_style & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW|WS_EX_TOOLWINDOW))
            | (options.hide_taskbar && tray.active() ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW);
        if (style!=old_style || (taskbar_style_dirty && !preferences_open)) {
            const bool visible=IsWindowVisible(hwnd)!=FALSE;
            const bool cycle_visibility=visible && !preferences_open;
            if (cycle_visibility) ShowWindow(hwnd,SW_HIDE);
            SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style);
            SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED|(cycle_visibility ? SWP_SHOWWINDOW : 0));
            taskbar_style_dirty=preferences_open;
        }
        if (shown && !tray.active() && !IsWindowVisible(hwnd)) restore_main();
        DWORD actual=0;
        const bool known=GetWindowDisplayAffinity(hwnd,&actual)!=FALSE;
        const DWORD desired=options.self_protect ? 0x11 : 0;
        if ((options.self_protect || (known && actual)) && actual!=desired)
            if (!SetWindowDisplayAffinity(hwnd,desired)) fail(GetLastError());
        privacy.set_enabled(options.self_protect);
        if (options.auto_refresh || options.auto_hide) {
            if (!SetTimer(hwnd,7,250,nullptr)) fail(ERROR_NO_SYSTEM_RESOURCES);
        } else KillTimer(hwnd,7);
    }
    DWORD commit_profile(const av::Profile& candidate) {
        DWORD error=0;
        if (config_path.empty()) return ERROR_PATH_NOT_FOUND;
        if (!av::save_profile_v4(config_path,modern_profile(candidate),error)) return error;
        bool policy_changed=profile.preferences.auto_hide!=candidate.preferences.auto_hide
            || profile.preferences.auto_mode!=candidate.preferences.auto_mode || profile.preferences.programs!=candidate.preferences.programs
            || profile.preferences.window_wait_ms!=candidate.preferences.window_wait_ms || profile.rules.size()!=candidate.rules.size();
        if (!policy_changed) for (std::size_t i=0;i<profile.rules.size();++i) {
            const auto& a=profile.rules[i]; const auto& b=candidate.rules[i];
            if (a.path!=b.path || a.class_name!=b.class_name || a.title!=b.title || a.mode!=b.mode || a.effects!=b.effects) { policy_changed=true; break; }
        }
        profile=candidate; if (policy_changed) auto_tracker.clear(); auto_initialized=true;
        appearance(); update_runtime(); return ERROR_SUCCESS;
    }
    void open_preferences() {
        if (preferences_open) { restore_main(); return; }
        preferences_open=true;
        av::show_preferences(hwnd,profile,this,[](void* context,const av::Profile& candidate) -> DWORD { return static_cast<App*>(context)->commit_profile(candidate); });
        preferences_open=false;
        if (IsWindow(hwnd)) update_runtime();
    }
    void tray_menu() {
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,show_window,tr(Text::show_window));
        AppendMenuW(menu,MF_STRING|(working ? MF_GRAYED : 0),preferences,tr(Text::preferences));
        AppendMenuW(menu,MF_STRING,help,tr(Text::help_menu));
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr); AppendMenuW(menu,MF_STRING,exit_app,tr(Text::exit));
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
        const UINT id=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON|(rtl?TPM_LAYOUTRTL:0),point.x,point.y,hwnd,nullptr);
        DestroyMenu(menu); PostMessageW(hwnd,WM_NULL,0,0); if (id) command(static_cast<int>(id));
    }
    void show_settings() {
        const HMENU menu = CreatePopupMenu(), themes = CreatePopupMenu(), languages = CreatePopupMenu();
        menu_label_count = 0;
        auto item = [&](HMENU parent, UINT_PTR id, Text key, bool checked = false, bool submenu = false) {
            const UINT flags = (dark ? MF_OWNERDRAW : MF_STRING) | (checked ? MF_CHECKED : 0) | (submenu ? MF_POPUP : 0);
            auto& label = menu_labels[menu_label_count++]; label = {tr(key), submenu, checked};
            AppendMenuW(parent, flags, id, dark ? reinterpret_cast<LPCWSTR>(&label) : label.text);
        };
        auto named = [&](HMENU parent, UINT_PTR id, const wchar_t* text, bool checked = false) {
            const UINT flags = (dark ? MF_OWNERDRAW : MF_STRING) | (checked ? MF_CHECKED : 0);
            auto& label = menu_labels[menu_label_count++]; label = {text, false, checked};
            AppendMenuW(parent, flags, id, dark ? reinterpret_cast<LPCWSTR>(&label) : label.text);
        };
        item(menu,preferences,Text::preferences); item(menu,help,Text::help_menu); item(menu,watermark_files,Text::watermark_files); item(menu,rules_dialog,Text::rules); item(menu,target_taskbar_hide,Text::target_taskbar_hide); item(menu,target_taskbar_show,Text::target_taskbar_show); AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        item(themes,theme_system,Text::system,profile.theme == av::Theme::system);
        item(themes,theme_light,Text::light,profile.theme == av::Theme::light);
        item(themes,theme_dark,Text::dark,profile.theme == av::Theme::dark);
        AppendMenuW(themes,MF_SEPARATOR,0,nullptr); item(themes,accent,Text::accent);
        item(languages,lang_auto,Text::automatic,profile.language == L"auto");
        item(languages,lang_zh,Text::chinese,profile.language == L"zh"); item(languages,lang_en,Text::english,profile.language == L"en");
        named(languages,lang_es,L"Español",profile.language == L"es");
        named(languages,lang_fr,L"Français",profile.language == L"fr");
        named(languages,lang_ru,L"Русский",profile.language == L"ru");
        named(languages,lang_ar,L"العربية",profile.language == L"ar");
        named(languages,lang_tr,L"Türkçe",profile.language == L"tr");
        named(languages,lang_ja,L"日本語",profile.language == L"ja");
        named(languages,lang_ko,L"한국어",profile.language == L"ko");
        item(menu,reinterpret_cast<UINT_PTR>(themes),Text::theme,false,true);
        item(menu,reinterpret_cast<UINT_PTR>(languages),Text::language,false,true);
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        item(menu,self_exclude,Text::self_exclude); item(menu,self_restore,Text::self_restore);
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr); item(menu,exit_app,Text::exit);
        if (dark) {
            MENUINFO info{sizeof(info)}; info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS; info.hbrBack = surface;
            SetMenuInfo(menu,&info);
        }
        RECT button{}; GetWindowRect(GetDlgItem(hwnd,settings),&button);
        UINT menu_flags = TPM_RETURNCMD|TPM_BOTTOMALIGN;
        int menu_x = button.right;
        if (rtl) { menu_flags |= TPM_LEFTALIGN|TPM_LAYOUTRTL; menu_x = button.left; }
        else menu_flags |= TPM_RIGHTALIGN;
        const UINT selected_id = TrackPopupMenuEx(menu,menu_flags,menu_x,button.top,hwnd,nullptr);
        DestroyMenu(menu);
        if (selected_id) command(static_cast<int>(selected_id));
    }
    av::ui::MainLayoutSpec layout_spec(int client_width, int client_height) {
        av::ui::MainLayoutSpec spec;
        spec.client_width = client_width;
        spec.client_height = client_height;
        spec.dpi = dpi;
        HDC dc = GetDC(hwnd);
        const auto old = dc ? SelectObject(dc, font) : nullptr;
        for (const auto button : buttons) {
            wchar_t label[256]{}; GetWindowTextW(button, label, 256);
            SIZE size{}; if (dc) GetTextExtentPoint32W(dc, label, static_cast<int>(std::wcslen(label)), &size);
            spec.buttons.push_back({GetDlgCtrlID(button), size.cx});
        }
        auto measure = [&](Text key) {
            SIZE size{};
            if (dc) GetTextExtentPoint32W(dc, tr(key), lstrlenW(tr(key)), &size);
            return size.cx + px(12);
        };
        spec.action_label_width = measure(Text::actions);
        spec.profile_label_width = measure(Text::profiles);
        spec.batch_label_width = measure(Text::filtered_actions);
        if (dc) { if (old) SelectObject(dc, old); ReleaseDC(hwnd, dc); }
        return spec;
    }
    av::ui::MainLayout current_layout(int client_width, int client_height) {
        return av::ui::resolve_main_layout(layout_spec(client_width, client_height),
            rtl ? av::ui::TextDirection::rtl : av::ui::TextDirection::ltr);
    }
    int minimum_window_width() {
        av::ui::MainLayoutSpec spec = layout_spec(0, 0);
        spec.client_width = av::ui::minimum_client_width(spec,
            rtl ? av::ui::TextDirection::rtl : av::ui::TextDirection::ltr);
        spec.client_height = px(540);
        RECT frame{0, 0, spec.client_width, spec.client_height};
        AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0);
        return std::max<int>(px(760), frame.right - frame.left);
    }
    void layout() {
        RECT r{}; GetClientRect(hwnd, &r);
        if (!list) return;
        const auto layout = current_layout(r.right, r.bottom);
        side_scroll = std::clamp(side_scroll, 0, std::max(0, layout.max_side_scroll));
        if (!layout.side_scroll_needed) side_scroll = 0;
        const int shift = side_scroll;
        MoveWindow(tab, layout.tab.left, layout.tab.top, layout.tab.width(), layout.tab.height(), TRUE);
        MoveWindow(GetDlgItem(hwnd,clear_filter), layout.clear_filter.left, layout.clear_filter.top,
            layout.clear_filter.width(), layout.clear_filter.height(), TRUE);
        MoveWindow(edit, layout.filter.left, layout.filter.top, layout.filter.width(), layout.filter.height(), TRUE);
        MoveWindow(list, layout.list.left, layout.list.top, layout.list.width(), layout.list.height(), TRUE);
        MoveWindow(GetDlgItem(hwnd,selection_summary), layout.summary.left, layout.summary.top,
            layout.summary.width(), layout.summary.height(), TRUE);
        MoveWindow(status, layout.status.left, layout.status.top, layout.status.width(), layout.status.height(), TRUE);
        auto place = [&](int id, const av::ui::RectI& rect, bool scrolled) {
            const int top = rect.top - (scrolled ? shift : 0);
            MoveWindow(GetDlgItem(hwnd,id), rect.left, top, rect.width(), rect.height(), TRUE);
        };
        for (int i = 0; i < layout.button_count; ++i) place(layout.ids[i], layout.buttons[i], true);
        place(action_group, layout.group_labels[0], true);
        place(profile_group, layout.group_labels[1], true);
        place(batch_group, layout.group_labels[2], true);
        action_rect = {layout.action_bounds.left, layout.action_bounds.top - shift,
            layout.action_bounds.right, layout.action_bounds.bottom - shift};
        profile_rect = {layout.profile_bounds.left, layout.profile_bounds.top - shift,
            layout.profile_bounds.right, layout.profile_bounds.bottom - shift};
        const int left_width = std::max<int>(1, layout.list.width());
        const int title_width = std::max(px(155), left_width*36/100);
        const int widths[] = {title_width, px(56), px(84), px(152), std::max(px(155), left_width-title_width-px(304))};
        for (int i=0;i<5;++i) ListView_SetColumnWidth(list,i,widths[i]);
        int order[5]{};
        const auto visual = av::ui::visual_column_order(rtl ? av::ui::TextDirection::rtl : av::ui::TextDirection::ltr, 5);
        for (int i=0;i<5;++i) order[i]=visual[static_cast<std::size_t>(i)];
        SendMessageW(list, LVM_SETCOLUMNORDERARRAY, 5, reinterpret_cast<LPARAM>(order));
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    bool scroll_side_buttons(int delta) {
        RECT r{}; GetClientRect(hwnd, &r);
        const auto main_layout = current_layout(r.right, r.bottom);
        if (main_layout.max_side_scroll <= 0) return false;
        const int next = av::ui::scroll_side(side_scroll, delta, main_layout);
        if (next == side_scroll) return false;
        side_scroll = next; layout(); return true;
    }
    void appearance() {
        language = av::choose_language(profile.language, av::system_language());
        dark = profile.theme == av::Theme::dark || (profile.theme == av::Theme::system && av::system_dark());
        rtl = av::ui::is_rtl_language(language);
        const auto palette = av::ui::resolve_palette(static_cast<int>(profile.theme), av::system_dark(), profile.accent);
        bg = palette.window_bg;
        panel = palette.panel_bg;
        fg = palette.text;
        if (background) DeleteObject(background);
        if (surface) DeleteObject(surface);
        background = CreateSolidBrush(bg); surface = CreateSolidBrush(panel);
        ListView_SetBkColor(list, panel); ListView_SetTextBkColor(list, panel); ListView_SetTextColor(list, fg);
        SetWindowTheme(list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        const BOOL dark_title = dark; DwmSetWindowAttribute(hwnd, 20, &dark_title, sizeof(dark_title));
        if (randomized_title.empty()) randomized_title=av::random_label();
        const std::wstring title = profile.preferences.random_title ? randomized_title : std::wstring(tr(Text::title)) + L"  " AV_VERSION + (av::is_administrator() ? L" [" + std::wstring(tr(Text::administrator)) + L"]" : L"");
        SetWindowTextW(hwnd, title.c_str()); SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(Text::filter)));
        const std::pair<Id, Text> labels[] = {{refresh,Text::refresh},{exclude,Text::exclude},{blank,Text::blank},{restore,Text::restore},
            {remember,Text::remember},{save,Text::save},{load,Text::load},{geometry,Text::geometry},{apply_rules,Text::apply_rules},
            {export_list,Text::export_list},{select_all,Text::select_all},{clear_selection,Text::clear_selection},
            {exclude_all,Text::exclude_all},{restore_all,Text::restore_all},{settings,Text::settings},{about,Text::about},
            {action_group,Text::actions},{profile_group,Text::profiles},{effects,Text::effects},{batch_group,Text::filtered_actions}};
        for (auto [id, key] : labels) SetWindowTextW(GetDlgItem(hwnd, id), tr(key));
        SetWindowTextW(GetDlgItem(hwnd,clear_filter),L"×");
        if (tips) {
            for (auto [id,key] : {std::pair<int,Text>{exclude,Text::protection_tip},{blank,Text::protection_tip},{effects,Text::effects},{restore,Text::restore_tip},{exclude_all,Text::all_tip},{restore_all,Text::all_tip},{clear_filter,Text::clear_filter}}) {
                TOOLINFOW tool{sizeof(tool)}; tool.hwnd=hwnd; tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(hwnd,id)); tool.lpszText=const_cast<wchar_t*>(tr(key));
                SendMessageW(tips,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
            }
        }
        for (const auto button : buttons) {
            auto style = GetWindowLongPtrW(button,GWL_STYLE);
            style = (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | (dark ? BS_OWNERDRAW : BS_PUSHBUTTON);
            SetWindowLongPtrW(button,GWL_STYLE,style); SetWindowTheme(button,dark ? L"" : L"Explorer",nullptr);
            InvalidateRect(button,nullptr,TRUE);
        }
        auto set_reading = [&](HWND control, bool value) {
            if (!control) return;
            auto ex = GetWindowLongPtrW(control, GWL_EXSTYLE);
            ex = value ? (ex | WS_EX_RTLREADING) : (ex & ~static_cast<LONG_PTR>(WS_EX_RTLREADING));
            SetWindowLongPtrW(control, GWL_EXSTYLE, ex);
        };
        set_reading(edit, rtl);
        set_reading(status, rtl);
        set_reading(GetDlgItem(hwnd, selection_summary), rtl);
        for (const auto button : buttons) set_reading(button, rtl);
        for (const HWND control : {status, GetDlgItem(hwnd, selection_summary)}) {
            auto style = GetWindowLongPtrW(control, GWL_STYLE);
            style = (style & ~static_cast<LONG_PTR>(SS_TYPEMASK)) | (rtl ? SS_RIGHT : SS_LEFTNOWORDWRAP);
            SetWindowLongPtrW(control, GWL_STYLE, style);
        }
        for (int i=0;i<2;++i) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(tr(i ? Text::processes : Text::windows)); TabCtrl_SetItem(tab,i,&item); }
        TabCtrl_SetCurSel(tab,process_view ? 1 : 0); InvalidateRect(tab,nullptr,TRUE);
        const Text columns[] = {process_view ? Text::process_name : Text::window_title, Text::process, Text::architecture, Text::capture, Text::path};
        for (int i = 0; i < 5; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT; c.pszText = const_cast<wchar_t*>(tr(columns[i])); ListView_SetColumn(list, i, &c); }
        render(); layout();
        if (!working) message(std::wstring(tr(Text::ready))+L" · "+std::to_wstring(windows.size())+L" · "+tr(Text::rules)+L": "+std::to_wstring(profile.rules.size()));
    }
    std::vector<av::Window> selected() const {
        std::vector<av::Window> out;
        for (int index = ListView_GetNextItem(list, -1, LVNI_SELECTED); index >= 0; index = ListView_GetNextItem(list, index, LVNI_SELECTED)) {
            if (static_cast<std::size_t>(index) >= rows.size()) continue;
            const auto& w = windows[rows[index]];
            if (process_view) { for (const auto& other : windows) if (other.pid == w.pid && other.process_created == w.process_created) out.push_back(other); }
            else out.push_back(w);
        }
        return out;
    }
    void render() {
        rendering=true;
        const int top=ListView_GetTopIndex(list);
        av::WindowKey top_key{};
        if (top>=0 && static_cast<std::size_t>(top)<rows.size()) top_key=av::window_key(windows[rows[top]]);
        const int focused=ListView_GetNextItem(list,-1,LVNI_FOCUSED); av::WindowKey focus_key{};
        if (focused>=0 && static_cast<std::size_t>(focused)<rows.size()) focus_key=av::window_key(windows[rows[focused]]);
        const auto previous = selected();
        std::set<std::tuple<std::uintptr_t, DWORD, std::uint64_t>> identities;
        for (const auto& w : previous) identities.emplace(w.handle, w.pid, w.process_created);
        wchar_t query[1024]{}; GetWindowTextW(edit, query, 1024);
        auto lower = [](std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); }); return s; };
        const auto needle = lower(query);
        const bool same_query=needle==rendered_query; rendered_query=needle;
        rows.clear(); std::set<DWORD> seen;
        for (std::size_t i = 0; i < windows.size(); ++i) {
            const auto& w = windows[i];
            if (!needle.empty() && lower(w.title + L" " + w.path + L" " + std::to_wstring(w.pid)).find(needle) == std::wstring::npos) continue;
            if (process_view && !seen.insert(w.pid).second) continue;
            rows.push_back(i);
        }
        std::stable_sort(rows.begin(), rows.end(), [&](std::size_t a, std::size_t b) {
            const auto& left = windows[a]; const auto& right = windows[b];
            int order = 0;
            if (sort_column == 1) order = left.pid < right.pid ? -1 : left.pid > right.pid;
            else if (sort_column == 3) order = static_cast<int>(left.mode) - static_cast<int>(right.mode);
            else if (sort_column==0 && process_view) {
                const auto l=left.path.substr(left.path.find_last_of(L"\\/")+1),r=right.path.substr(right.path.find_last_of(L"\\/")+1); order=l.compare(r);
            } else { const auto& l = sort_column == 4 ? left.path : sort_column == 2 ? left.architecture : left.title;
                const auto& r = sort_column == 4 ? right.path : sort_column == 2 ? right.architecture : right.title; order = l.compare(r); }
            return descending ? order > 0 : order < 0;
        });
        SendMessageW(list, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(list);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const auto& w = windows[rows[row]];
            std::wstring name = process_view ? w.path.substr(w.path.find_last_of(L"\\/") + 1) : w.title;
            if (name.empty()) name = w.title;
            LVITEMW item{}; item.mask = LVIF_TEXT|LVIF_IMAGE; item.iImage=icon_for(w); item.iItem = static_cast<int>(row); item.pszText = name.data();
            const int index = ListView_InsertItem(list, &item);
            auto set = [&](int column, const std::wstring& s) { ListView_SetItemText(list, index, column, const_cast<wchar_t*>(s.c_str())); };
            set(1, std::to_wstring(w.pid)); set(2, w.architecture.empty() ? tr(Text::unknown) : w.architecture);
            auto state=state_for(w);
            if (process_view) for (const auto& other : windows) if (other.pid==w.pid && other.process_created==w.process_created && state_for(other)!=state) { state=tr(Text::mixed); break; }
            set(3,state);
            set(4, w.path);
            if (identities.count({w.handle,w.pid,w.process_created})) ListView_SetItemState(list, index, LVIS_SELECTED, LVIS_SELECTED);
            if (av::window_key(w)==focus_key) ListView_SetItemState(list,index,LVIS_FOCUSED,LVIS_FOCUSED);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
        if (same_query) scroll_to(top_key);
        rendering=false; selection_changed();
    }
    template<class Work> void start(Work work) {
        if (working) return;
        if (worker.joinable()) worker.join();
        working = true; cancel = false; message(tr(Text::busy));
        for (const auto button : buttons) EnableWindow(button,FALSE);
        EnableWindow(GetDlgItem(hwnd,refresh),TRUE); SetWindowTextW(GetDlgItem(hwnd,refresh),tr(Text::cancel));
        EnableWindow(tab,FALSE);
        worker = std::thread([this, work] {
            auto result = std::make_unique<Result>();
            try { work(*result); } catch (...) { result->error = ERROR_NOT_ENOUGH_MEMORY; ++result->failed; }
            pending = std::move(result); PostMessageW(hwnd, completed, 0, 0);
        });
    }
    void refresh_list(bool automatic=false,bool publish=true) {
        const auto settings_snapshot=profile;
        const auto tracker=auto_tracker;
        const bool initial=!auto_initialized;
        start([this,settings_snapshot,tracker,automatic,publish,initial](Result& r) {
            r.enumeration=true; r.automatic=automatic; r.publish_windows=publish;
            r.windows=av::enumerate_windows(automatic || initial); r.tracker=tracker;
            if (!automatic) return;
            r.tracker.prune(r.windows);
            for (auto& window : r.windows) {
                if (cancel) break;
                if (window.pid==GetCurrentProcessId()) continue;
                av::Mode requested=settings_snapshot.preferences.auto_mode; av::Effects effects; bool matched=false;
                for (const auto& rule : settings_snapshot.rules) if (av::matches(rule,window)) { matched=true; requested=rule.mode; effects=rule.effects; break; }
                if (!matched) for (const auto& program : settings_snapshot.preferences.programs) if (av::program_matches(program,window)) { matched=true; break; }
                if (!matched || (requested==av::Mode::none && !av::has_effects(effects)) || !r.tracker.needs_attempt(window,GetTickCount64(),settings_snapshot.preferences.window_wait_ms)) continue;
                const bool session=needs_capture_session(effects);
                DWORD error=0;
                const bool ok=session ? av::same_window(window)
                    : av::apply_affinity(window,av::effective_mode(requested,effects),error);
                if (session && !ok) error=ERROR_INVALID_WINDOW_HANDLE;
                r.tracker.complete(window,ok);
                if (ok) {
                    ++r.ok; r.applied.emplace_back(window,effects); DWORD actual=0;
                    window.affinity_known=GetWindowDisplayAffinity(reinterpret_cast<HWND>(window.handle),&actual) && av::valid_mode(actual);
                    if (window.affinity_known) window.mode=static_cast<av::Mode>(actual);
                } else { ++r.failed; if (!r.error) { r.error=error; r.target=window.title; } }
            }
        });
    }
    void timer() {
        if (working || preferences_open) return;
        const ULONGLONG now=GetTickCount64();
        const auto& options=profile.preferences;
        const bool refresh_due=options.auto_refresh && now-last_refresh>=options.refresh_ms;
        const bool auto_due=options.auto_hide && now-last_auto>=500;
        if (!refresh_due && !auto_due) return;
        if (refresh_due) last_refresh=now;
        if (auto_due) last_auto=now;
        refresh_list(auto_due,refresh_due);
    }
    void finish() {
        if (worker.joinable()) worker.join();
        working = false; auto result = std::move(pending);
        for (const auto button : buttons) EnableWindow(button,TRUE);
        EnableWindow(tab,TRUE);
        SetWindowTextW(GetDlgItem(hwnd,refresh),tr(Text::refresh));
        if (!result) return;
        for (const auto& [target,effects] : result->applied) {
            const auto key=av::window_key(target);
            if (needs_capture_session(effects)) {
                bool same=false;
                if (const auto found=capture_sessions.find(key); found!=capture_sessions.end())
                    same=found->second.effects==effects;
                else if (const auto proxy_entry=proxy_sessions.find(key); proxy_entry!=proxy_sessions.end())
                    same=proxy_entry->second.effects==effects;
                if (same) continue;
                std::wstring secret;
                if (const auto secret_entry=session_keys.find(key); secret_entry!=session_keys.end()) secret=secret_entry->second;
                std::wstring detail;
                if (!start_capture_session(target,effects,secret,detail)) {
                    if (result->automatic) result->tracker.complete(target,false);
                    if (result->ok) --result->ok; ++result->failed;
                    if (!result->error) { result->error=ERROR_GEN_FAILURE; result->target=target.title+L" · "+detail; }
                }
                continue;
            }
            stop_capture_session(key);
            DWORD error=0;
            if (!layers.set(target,effects,error)) { if (result->automatic) result->tracker.complete(target,false); if (result->ok) --result->ok; ++result->failed; if (!result->error) { result->error=error; result->target=target.title; } }
        }
        if (result->enumeration) {
            if (!auto_initialized) {
                if (!profile.preferences.protect_on_start) auto_tracker.seed(result->windows);
                auto_initialized=true;
            }
            if (result->automatic) auto_tracker=std::move(result->tracker);
            if (result->publish_windows) {
                av::WindowKey top_key{},focus_key{};
                const int top=ListView_GetTopIndex(list),focus=ListView_GetNextItem(list,-1,LVNI_FOCUSED);
                if (top>=0 && static_cast<std::size_t>(top)<rows.size()) top_key=av::window_key(windows[rows[top]]);
                if (focus>=0 && static_cast<std::size_t>(focus)<rows.size()) focus_key=av::window_key(windows[rows[focus]]);
                const auto previous=selected(); std::set<av::WindowKey> selected_keys;
                for (const auto& window : previous) selected_keys.insert(av::window_key(window));
                ListView_DeleteAllItems(list); rows.clear(); windows=std::move(result->windows);
                windows.erase(std::remove_if(windows.begin(),windows.end(),[](const av::Window& window) { return window.title.empty(); }),windows.end());
                render();
                for (std::size_t row=0;row<rows.size();++row) if (selected_keys.count(av::window_key(windows[rows[row]]))) ListView_SetItemState(list,static_cast<int>(row),LVIS_SELECTED,LVIS_SELECTED);
                for (std::size_t row=0;row<rows.size();++row) if (focus_key==av::window_key(windows[rows[row]])) ListView_SetItemState(list,static_cast<int>(row),LVIS_FOCUSED,LVIS_FOCUSED);
                scroll_to(top_key);
            } else {
                std::map<av::WindowKey,std::pair<av::Mode,bool>> states;
                for (const auto& window : result->windows) states[av::window_key(window)]={window.mode,window.affinity_known};
                for (auto& window : windows) {
                    const auto found=states.find(av::window_key(window));
                    if (found!=states.end()) { window.mode=found->second.first; window.affinity_known=found->second.second; }
                }
                render();
            }
            if (result->automatic && result->ok) {
                const auto notification=std::wstring(tr(Text::auto_protected))+L": "+std::to_wstring(result->ok);
                message(notification);
                if (profile.preferences.notify_auto_hide) tray.notify(tr(Text::auto_protection),notification);
            } else if (!result->automatic) message(std::wstring(tr(Text::ready)) + L" · " + std::to_wstring(windows.size()) + L" · " + tr(Text::rules) + L": " + std::to_wstring(profile.rules.size()));
        } else {
            message(std::wstring(tr(Text::result)) + L": " + std::to_wstring(result->ok) + L" / " + std::to_wstring(result->failed)
                + (result->error ? L" · " + result->target + L" · " + av::error_text(result->error) : L""));
            // Read back independently without overwriting operation feedback.
            for (auto& w : windows) {
                DWORD mode = 0;
                w.affinity_known = av::same_window(w) && GetWindowDisplayAffinity(reinterpret_cast<HWND>(w.handle), &mode) && av::valid_mode(mode);
                if (w.affinity_known) w.mode = static_cast<av::Mode>(mode);
            }
            render();
        }
        if (result->enumeration && result->error) fail(result->error);
        selection_changed();
    }
    void protect(av::Mode mode, bool all = false, av::Effects effects = {}) {
        auto targets = selected();
        if (all) {
            targets.clear();
            for (const auto index : rows) {
                const auto& selected_window = windows[index];
                if (process_view) {
                    for (const auto& w : windows) if (w.pid == selected_window.pid && w.process_created == selected_window.process_created) targets.push_back(w);
                } else targets.push_back(selected_window);
            }
        }
        if (targets.empty()) { message(tr(Text::no_selection)); return; }
        if (all) stop_all_sessions(); else stop_capture_sessions(targets);
        start([this, targets, mode, effects](Result& r) {
            for (const auto& w : targets) {
                if (cancel) break;
                DWORD error = 0;
                if (av::apply_affinity(w, av::effective_mode(mode,effects), error)) { ++r.ok; r.applied.emplace_back(w,effects); }
                else { ++r.failed; if (!r.error) { r.error = error; r.target = w.title; } }
            }
        });
    }
    std::wstring choose_file(bool opening, bool csv = false) {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = hwnd; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
        dialog.lpstrFilter = csv ? tr(Text::csv_filter) : tr(Text::profile_filter);
        dialog.lpstrDefExt = csv ? L"csv" : L"ini";
        dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (opening ? OFN_FILEMUSTEXIST : OFN_OVERWRITEPROMPT);
        const bool ok = opening ? GetOpenFileNameW(&dialog) : GetSaveFileNameW(&dialog);
        if (!ok) { const DWORD error = CommDlgExtendedError(); if (error) fail(error); }
        return ok ? path : L"";
    }
    bool persist() {
        DWORD error = 0;
        if (config_path.empty() || !av::save_profile_v4(config_path, modern_profile(profile), error)) { fail(error ? error : ERROR_PATH_NOT_FOUND); return false; }
        return true;
    }
    void apply_saved(bool layout_only) {
        const auto rules = profile.rules; const auto snapshot = windows;
        start([this, rules, snapshot, layout_only](Result& r) {
            for (const auto& rule : rules) {
                if (cancel) break;
                std::vector<av::Window> found;
                for (const auto& w : snapshot) if (av::matches(rule, w)) found.push_back(w);
                DWORD error = found.empty() ? ERROR_NOT_FOUND : found.size() > 1 ? ERROR_DUP_NAME : ERROR_SUCCESS;
                if (!error) {
                    if (layout_only) av::restore_geometry(found.front(), rule, error);
                    else if (needs_capture_session(rule.effects)) {
                        if (av::same_window(found.front())) r.applied.emplace_back(found.front(),rule.effects);
                        else error = ERROR_INVALID_WINDOW_HANDLE;
                    } else if (av::apply_affinity(found.front(), av::effective_mode(rule.mode,rule.effects), error)) r.applied.emplace_back(found.front(),rule.effects);
                }
                if (error) { ++r.failed; if (!r.error) { r.error = error; r.target = rule.title; } }
                else ++r.ok;
            }
        });
    }
    void command(int id) {
        if (id == exit_app) { force_exit=true; SendMessageW(hwnd, WM_CLOSE, 0, 0); return; }
        if (id == show_window) { restore_main(); return; }
        if (working) { if (id==refresh) { cancel=true; message(tr(Text::cancelling)); } return; }
        switch (id) {
        case refresh: refresh_list(); break;
        case exclude: protect(av::Mode::exclude); break;
        case blank: protect(av::Mode::blank); break;
        case restore: protect(av::Mode::none); break;
        case exclude_all: protect(av::Mode::exclude,true); break;
        case restore_all: protect(av::Mode::none,true); break;
        case effects: {
            const auto targets=selected(); if (targets.empty()) { message(tr(Text::no_selection)); break; }
            auto effect=profile.effects; auto mode=profile.effect_mode;
            std::wstring secret;
            if (const auto found=session_keys.find(av::window_key(targets.front())); found!=session_keys.end()) secret=found->second;
            if (const auto* active=layers.get(targets.front())) { effect=*active; mode=targets.front().mode; }
            else if (const auto capture_entry=capture_sessions.find(av::window_key(targets.front())); capture_entry!=capture_sessions.end()) effect=capture_entry->second.effects;
            else if (const auto proxy_entry=proxy_sessions.find(av::window_key(targets.front())); proxy_entry!=proxy_sessions.end()) effect=proxy_entry->second.effects;
            preferences_open=true;
            const bool accepted=av::show_effects_dialog(hwnd,profile,targets.front(),targets.size(),mode,effect,&secret);
            preferences_open=false;
            if (accepted) {
                profile.effects=effect; profile.effect_mode=mode; const bool saved=persist();
                if (needs_capture_session(effect)) {
                    unsigned ok=0, failed=0; std::wstring detail;
                    for (const auto& target : targets) {
                        std::wstring current;
                        if (!start_capture_session(target,effect,secret,current)) {
                            ++failed; if (detail.empty()) detail=current;
                        } else ++ok;
                    }
                    if (failed) message(std::wstring(tr(Text::capture_session_failed))+L" "+detail+L" ("+std::to_wstring(ok)+L"/"+std::to_wstring(failed)+L")");
                    else if (!saved) message(tr(Text::error));
                    else message(tr(Text::effect_active));
                } else if (saved) protect(mode,false,effect);
            }
            break;
        }
        case clear_filter: SetWindowTextW(edit,L""); SetFocus(edit); break;
        case watermark_files:
            preferences_open=true;
            av::show_watermark_files(hwnd,language,static_cast<int>(profile.theme),profile.accent);
            preferences_open=false;
            if (IsWindow(hwnd)) update_runtime();
            break;
        case rules_dialog: {
            const auto input = modern_profile(profile);
            auto result = input;
            preferences_open = true;
            const bool accepted = av::show_rules_dialog(hwnd,
                av::rules_dialog_strings(language), input, result);
            preferences_open = false;
            if (accepted) {
                const DWORD error = commit_profile(legacy_profile(result));
                if (error) fail(error);
                else { message(tr(Text::saved)); refresh_list(); }
            }
            break;
        }
        case target_taskbar_hide: case target_taskbar_show: {
            const auto targets = selected();
            if (targets.empty()) { message(tr(Text::no_selection)); break; }
            const auto batch = id == target_taskbar_hide
                ? av::TaskbarLedger::hide_all(taskbar, targets)
                : av::TaskbarLedger::show_all(taskbar, targets);
            if (batch.first_error) fail(batch.first_error);
            else message(tr(Text::saved));
            break;
        }
        case settings: show_settings(); break;
        case preferences: open_preferences(); break;
        case select_all: ListView_SetItemState(list, -1, LVIS_SELECTED, LVIS_SELECTED); break;
        case clear_selection: ListView_SetItemState(list, -1, 0, LVIS_SELECTED); break;
        case view_windows: case view_processes: {
            wchar_t value[1024]{}; GetWindowTextW(edit,value,1024); filters[process_view ? 1 : 0]=value;
            process_view=id==view_processes; ListView_DeleteAllItems(list); rows.clear(); SetWindowTextW(edit,filters[process_view ? 1 : 0].c_str()); appearance(); break;
        }
        case remember: {
            const auto targets = selected(); if (targets.empty()) { message(tr(Text::no_selection)); break; }
            for (const auto& w : targets) {
                if (w.path.empty() || w.width < 1 || w.height < 1) { fail(ERROR_ACCESS_DENIED); return; }
                auto rule = av::remember(w, w.mode == av::Mode::none ? av::Mode::exclude : w.mode);
                if (const auto* effect=layers.get(w)) { rule.effects=*effect; rule.mode=w.mode; }
                auto existing = std::find_if(profile.rules.begin(), profile.rules.end(), [&](const av::Rule& item) { return item.path == rule.path && item.class_name == rule.class_name && item.title == rule.title; });
                if (existing == profile.rules.end()) profile.rules.push_back(rule); else *existing = rule;
            }
            if (persist()) message(tr(Text::remembered)); selection_changed(); break;
        }
        case save: case load: case export_list: {
            const auto file = choose_file(id == load, id == export_list); if (file.empty()) break;
            DWORD error = 0; bool ok = false;
            if (id == load) { av::ProfileV4 candidate; const auto loaded=av::load_profile_v4(file,candidate); ok=loaded.ok(); error=loaded.error; if (ok) { error=commit_profile(legacy_profile(candidate)); ok=error==ERROR_SUCCESS; } }
            else if (id == save) ok = av::save_profile_v4(file, modern_profile(profile), error);
            else { std::vector<av::Window> visible; for (const auto index : rows) visible.push_back(windows[index]); ok = av::export_windows(file, visible, error); }
            if (!ok) fail(error); else message(tr(id == load ? Text::loaded : Text::saved)); break;
        }
        case geometry: apply_saved(true); break;
        case apply_rules: apply_saved(false); break;
        case theme_system: case theme_light: case theme_dark:
            profile.theme = static_cast<av::Theme>(id - theme_system); appearance(); update_runtime(); persist(); break;
        case lang_auto: case lang_zh: case lang_en: case lang_es: case lang_fr: case lang_ru:
        case lang_ar: case lang_tr: case lang_ja: case lang_ko: {
            static const wchar_t* codes[] = {L"auto", L"zh", L"en", L"es", L"fr", L"ru", L"ar", L"tr", L"ja", L"ko"};
            profile.language = codes[id - lang_auto]; appearance(); update_runtime(); persist(); break;
        }
        case accent: {
            static COLORREF custom[16]{}; CHOOSECOLORW color{sizeof(color)};
            color.hwndOwner = hwnd; color.rgbResult = profile.accent; color.lpCustColors = custom; color.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&color)) { profile.accent = color.rgbResult; appearance(); persist(); } break;
        }
        case self_exclude: case self_restore: {
            profile.preferences.self_protect=id==self_exclude; update_runtime(); persist();
            refresh_list(); break;
        }
        case about:
            preferences_open = true; show_about_dialog(0); preferences_open = false;
            if (IsWindow(hwnd)) { privacy.sync(); update_runtime(); }
            break;
        case help:
            preferences_open = true; show_about_dialog(3); preferences_open = false;
            if (IsWindow(hwnd)) { privacy.sync(); update_runtime(); }
            break;
        }
    }
    void update_fonts() {
        const HFONT old_font = font;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0)) {
            metrics.lfMessageFont.lfHeight = -px(12); metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
            font = CreateFontIndirectW(&metrics.lfMessageFont);
        } else font = CreateFontW(-px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for (const auto control : {edit,list,status,tab,GetDlgItem(hwnd,action_group),GetDlgItem(hwnd,profile_group),GetDlgItem(hwnd,batch_group),GetDlgItem(hwnd,selection_summary)}) if (control) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        for (const auto button : buttons) SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        if (old_font) DeleteObject(old_font);
        const auto old_images=images; images=ImageList_Create(px(20),px(30),ILC_COLOR24|ILC_MASK,16,16); icon_cache.clear();
        ListView_SetImageList(list,images,LVSIL_SMALL); if (old_images) ImageList_Destroy(old_images);
        av::Window generic; icon_for(generic);
    }
    void create() {
        HDC dc = GetDC(hwnd); dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(hwnd, dc);
        using GetWindowDpi = UINT(WINAPI*)(HWND);
        if (const auto get_dpi = reinterpret_cast<GetWindowDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"))) dpi = get_dpi(hwnd);
        tab = CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd,reinterpret_cast<HMENU>(tabs),instance,nullptr);
        SetWindowSubclass(tab,tab_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        for (int i=0;i<2;++i) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(L""); TabCtrl_InsertItem(tab,i,&item); }
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0,0,0,0,hwnd,reinterpret_cast<HMENU>(filter),instance,nullptr);
        SendMessageW(edit, EM_SETLIMITTEXT, 1023, 0);
        list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 0,0,0,0,hwnd,nullptr,instance,nullptr);
        SetWindowSubclass(ListView_GetHeader(list), header_procedure, 1, reinterpret_cast<DWORD_PTR>(this));
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const int widths[] = {290,80,90,110,480};
        for (int i = 0; i < 5; ++i) { LVCOLUMNW col{}; col.mask = LVCF_WIDTH; col.cx = px(widths[i]); ListView_InsertColumn(list, i, &col); }
        status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0,0,0,0,hwnd,nullptr,instance,nullptr);
        for (const int id : {action_group,profile_group,batch_group}) CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFTNOWORDWRAP,0,0,0,0,hwnd,reinterpret_cast<HMENU>(selection_summary),instance,nullptr);
        const int ids[] = {exclude,blank,effects,restore,exclude_all,restore_all,select_all,clear_selection,refresh,remember,apply_rules,geometry,save,load,export_list,settings,about,clear_filter};
        for (const int id : ids) buttons.push_back(CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr));
        tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,hwnd,nullptr,instance,nullptr);
        SendMessageW(tips,TTM_SETMAXTIPWIDTH,0,px(360));
        for (int id : {exclude,blank,effects,restore,exclude_all,restore_all,clear_filter}) {
            TOOLINFOW tool{sizeof(tool)}; tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS; tool.hwnd=hwnd; tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(hwnd,id)); tool.lpszText=const_cast<wchar_t*>(L"");
            SendMessageW(tips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
        }
        update_fonts();
        tray.initialize(hwnd,tray_event); taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
        layers.notify_errors(hwnd,layer_problem);
        appearance(); update_runtime(); refresh_list();
    }
    void paint_header(HWND header) {
        PAINTSTRUCT p{}; const HDC dc = BeginPaint(header,&p); RECT client{}; GetClientRect(header,&client);
        FillRect(dc,&client,surface); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,fg);
        const auto old = SelectObject(dc,font);
        const Text labels[] = {process_view ? Text::process_name : Text::window_title,Text::process,Text::architecture,Text::capture,Text::path};
        const auto direction = rtl ? av::ui::TextDirection::rtl : av::ui::TextDirection::ltr;
        for (int i = 0; i < 5; ++i) {
            RECT rect{}; Header_GetItemRect(header,i,&rect); rect.left += px(6); rect.right -= px(6);
            const auto kind = static_cast<av::ui::ColumnKind>(i);
            const UINT flags = av::ui::field_text_flags(kind, direction,
                DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            DrawTextW(dc,tr(labels[i]),-1,&rect,flags);
            if (i==sort_column) {
                if (rtl) { rect.right=rect.left+px(10); DrawTextW(dc,descending ? L"▾" : L"▴",1,&rect,DT_SINGLELINE|DT_VCENTER|DT_LEFT); }
                else { rect.left=rect.right-px(10); DrawTextW(dc,descending ? L"▾" : L"▴",1,&rect,DT_SINGLELINE|DT_VCENTER|DT_RIGHT); }
            }
        }
        SelectObject(dc,old); EndPaint(header,&p);
    }
    void paint() {
        PAINTSTRUCT p{}; const HDC dc = BeginPaint(hwnd, &p); RECT r{}; GetClientRect(hwnd, &r); FillRect(dc, &r, background);
        const HPEN pen = CreatePen(PS_SOLID,1,dark ? RGB(65,65,65) : GetSysColor(COLOR_3DSHADOW));
        const auto old = SelectObject(dc,pen), old_brush = SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        MoveToEx(dc,px(8),r.bottom-px(25),nullptr); LineTo(dc,r.right-px(8),r.bottom-px(25));
        for (const auto& group : {action_rect,profile_rect}) Rectangle(dc,group.left,group.top+px(8),group.right,group.bottom);
        SelectObject(dc,old_brush); SelectObject(dc,old); DeleteObject(pen); EndPaint(hwnd, &p);
    }
    void paint_tabs(HWND control, HDC supplied) {
        PAINTSTRUCT paint{}; HDC dc = supplied ? supplied : BeginPaint(control,&paint);
        RECT client{}; GetClientRect(control,&client); FillRect(dc,&client,background);
        const HPEN border=CreatePen(PS_SOLID,1,dark ? RGB(95,95,95) : GetSysColor(COLOR_3DSHADOW));
        const auto old_pen=SelectObject(dc,border), old_font=SelectObject(dc,font), old_brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        const int selected_index=TabCtrl_GetCurSel(control);
        SetTextColor(dc,fg); SetBkMode(dc,TRANSPARENT);
        for (int i=0;i<2;++i) {
            RECT item{}; TabCtrl_GetItemRect(control,i,&item);
            FillRect(dc,&item,i == selected_index ? surface : background);
            Rectangle(dc,item.left,item.top,item.right,item.bottom);
            DrawTextW(dc,tr(i ? Text::processes : Text::windows),-1,&item,
                DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|(rtl?DT_RTLREADING:0));
        }
        SelectObject(dc,old_brush); SelectObject(dc,old_font); SelectObject(dc,old_pen); DeleteObject(border);
        if (!supplied) EndPaint(control,&paint);
    }
};
LRESULT CALLBACK tab_procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* app=reinterpret_cast<App*>(data);
    if (message == WM_PAINT || message == WM_PRINTCLIENT) { app->paint_tabs(hwnd,message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wp) : nullptr); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd,tab_procedure,id);
    return DefSubclassProc(hwnd,message,wp,lp);
}
LRESULT CALLBACK header_procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    if (message == WM_PAINT) { app->paint_header(hwnd); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd,header_procedure,id);
    return DefSubclassProc(hwnd,message,wp,lp);
}
LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) { app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); app->hwnd = hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(hwnd,msg,wp,lp);
    if (app->taskbar_created && msg==app->taskbar_created) { app->tray.removed_by_shell(); app->update_runtime(); return 0; }
    switch (msg) {
    case WM_CREATE: app->create(); return 0;
    case WM_TIMER: if (wp==7) app->timer(); else if (wp==8) app->poll_sessions(); return 0;
    case layer_problem: if (wp) app->fail(static_cast<DWORD>(wp)); app->render(); return 0;
    case tray_event:
        if (LOWORD(lp)==NIN_SELECT || LOWORD(lp)==NIN_KEYSELECT || LOWORD(lp)==WM_LBUTTONDBLCLK || LOWORD(lp)==NIN_BALLOONUSERCLICK) app->restore_main();
        else if (LOWORD(lp)==WM_CONTEXTMENU || LOWORD(lp)==WM_RBUTTONUP) app->tray_menu();
        return 0;
    case WM_SIZE: app->layout(); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {app->minimum_window_width(),app->px(540)}; return 0;
    case WM_MOUSEWHEEL: {
        if (!app->rtl) break;
        POINT cursor{static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd,&cursor);
        const bool over_side = app->action_rect.right > app->action_rect.left
            && cursor.x <= app->action_rect.right;
        if (!over_side) break;
        const int delta = -GET_WHEEL_DELTA_WPARAM(wp) * app->px(36) / WHEEL_DELTA;
        if (app->scroll_side_buttons(delta)) return 0;
        break;
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        if (app->list) { app->update_fonts(); app->appearance(); } return 0;
    case WM_DPICHANGED: {
        const UINT old_dpi = app->dpi; app->dpi = HIWORD(wp); app->update_fonts();
        for (int i = 0; i < 5; ++i) ListView_SetColumnWidth(app->list,i,MulDiv(ListView_GetColumnWidth(app->list,i),app->dpi,old_dpi));
        const auto* r = reinterpret_cast<RECT*>(lp);
        int width = r->right - r->left, height = r->bottom - r->top;
        if (app->initial_dpi_pending) {
            app->initial_dpi_pending = false;
            width = app->px(980); height = app->px(650);
        } else if (old_dpi == 96 && app->dpi != 96) {
            RECT current{}; GetWindowRect(hwnd, &current);
            width = MulDiv(static_cast<int>(current.right - current.left), static_cast<int>(app->dpi), 96);
            height = MulDiv(static_cast<int>(current.bottom - current.top), static_cast<int>(app->dpi), 96);
        }
        SetWindowPos(hwnd,nullptr,r->left,r->top,width,height,SWP_NOZORDER|SWP_NOACTIVATE); app->layout(); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == filter && HIWORD(wp) == EN_CHANGE) app->render();
        else app->command(LOWORD(wp)); return 0;
    case WM_NOTIFY: {
        const auto* n = reinterpret_cast<NMHDR*>(lp);
        if (n->hwndFrom == app->tab && n->code == TCN_SELCHANGE) {
            app->command(TabCtrl_GetCurSel(app->tab) == 1 ? view_processes : view_windows); return 0;
        }
        if (n->hwndFrom == app->list && n->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT) { app->draw_row(*draw); return CDRF_SKIPDEFAULT; }
            return CDRF_DODEFAULT;
        }
        if (n->hwndFrom==app->list && n->code==LVN_ITEMCHANGED) { app->selection_changed(); return 0; }
        if (n->hwndFrom==app->list && n->code==LVN_GETEMPTYMARKUP) {
            auto* markup=reinterpret_cast<NMLVEMPTYMARKUP*>(lp); markup->dwFlags=EMF_CENTERED; lstrcpynW(markup->szMarkup,app->tr(Text::empty_list),static_cast<int>(std::size(markup->szMarkup))); return TRUE;
        }
        if (n->hwndFrom == app->list && n->code == LVN_COLUMNCLICK) {
            const auto column = reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem;
            app->descending = app->sort_column == column ? !app->descending : false; app->sort_column = column; app->render(); return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wp), app->fg); SetBkColor(reinterpret_cast<HDC>(wp), msg == WM_CTLCOLOREDIT ? app->panel : app->bg);
        return reinterpret_cast<LRESULT>(msg == WM_CTLCOLOREDIT ? app->surface : app->background);
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (measure->CtlType != ODT_MENU) break;
        if (!measure->itemData) { measure->itemWidth = app->px(100); measure->itemHeight = app->px(7); return TRUE; }
        const auto* label = reinterpret_cast<App::MenuLabel*>(measure->itemData);
        HDC dc = GetDC(hwnd); const auto old = SelectObject(dc,app->font); SIZE size{};
        GetTextExtentPoint32W(dc,label->text,lstrlenW(label->text),&size);
        SelectObject(dc,old); ReleaseDC(hwnd,dc);
        measure->itemWidth = size.cx + app->px(48); measure->itemHeight = std::max<int>(size.cy+app->px(8),app->px(24)); return TRUE;
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw->CtlType == ODT_MENU) {
            const auto* label = reinterpret_cast<App::MenuLabel*>(draw->itemData);
            const HBRUSH fill = CreateSolidBrush(draw->itemState & ODS_SELECTED ? RGB(65,75,88) : app->panel);
            FillRect(draw->hDC,&draw->rcItem,fill); DeleteObject(fill);
            if (!label) return TRUE;
            SetBkMode(draw->hDC,TRANSPARENT); SetTextColor(draw->hDC,app->fg);
            const auto old = SelectObject(draw->hDC,app->font); RECT text = draw->rcItem;
            if (app->rtl) { text.left += app->px(20); text.right -= app->px(25); }
            else { text.left += app->px(25); text.right -= app->px(20); }
            DrawTextW(draw->hDC,label->text,-1,&text,
                av::ui::draw_text_flags(app->rtl?av::ui::TextDirection::rtl:av::ui::TextDirection::ltr,
                    DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX));
            SelectObject(draw->hDC,old);
            const int middle = (draw->rcItem.top+draw->rcItem.bottom)/2;
            if (label->checked) {
                const int mark = app->rtl ? draw->rcItem.right-app->px(16) : draw->rcItem.left+app->px(7);
                const HPEN pen = CreatePen(PS_SOLID,app->px(2),app->fg); const auto previous=SelectObject(draw->hDC,pen);
                MoveToEx(draw->hDC,mark,middle,nullptr); LineTo(draw->hDC,mark+app->px(3),middle+app->px(3));
                LineTo(draw->hDC,mark+app->px(9),middle-app->px(4)); SelectObject(draw->hDC,previous); DeleteObject(pen);
            }
            if (label->submenu) {
                const int x=app->rtl ? draw->rcItem.left+app->px(10) : draw->rcItem.right-app->px(10);
                const int dir=app->rtl ? 1 : -1;
                POINT points[]={{x+dir*app->px(3),middle-app->px(4)},{x-dir*app->px(1),middle},{x+dir*app->px(3),middle+app->px(4)}};
                const HBRUSH arrow=CreateSolidBrush(app->fg); const auto old_brush=SelectObject(draw->hDC,arrow), old_pen=SelectObject(draw->hDC,GetStockObject(NULL_PEN));
                Polygon(draw->hDC,points,3); SelectObject(draw->hDC,old_brush); SelectObject(draw->hDC,old_pen); DeleteObject(arrow);
            }
            return TRUE;
        }
        if (draw->CtlType == ODT_STATIC) {
            FillRect(draw->hDC,&draw->rcItem,app->background);
            auto edge=draw->rcItem;
            SetBkMode(draw->hDC,TRANSPARENT); SetTextColor(draw->hDC,app->fg); const auto old=SelectObject(draw->hDC,app->font);
            wchar_t caption[128]{}; GetWindowTextW(draw->hwndItem,caption,128);
            if (app->rtl) edge.right -= app->px(3); else edge.left += app->px(3);
            DrawTextW(draw->hDC,caption,-1,&edge,
                av::ui::draw_text_flags(app->rtl?av::ui::TextDirection::rtl:av::ui::TextDirection::ltr,
                    DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX));
            SelectObject(draw->hDC,old); return TRUE;
        }
        if (draw->CtlType != ODT_BUTTON) break;
        const HBRUSH fill = CreateSolidBrush(draw->itemState & ODS_SELECTED ? RGB(65,65,65) : RGB(48,48,48));
        FillRect(draw->hDC,&draw->rcItem,fill); DeleteObject(fill);
        const HBRUSH border = CreateSolidBrush(RGB(100,100,100)); FrameRect(draw->hDC,&draw->rcItem,border); DeleteObject(border);
        SetBkMode(draw->hDC, TRANSPARENT); SetTextColor(draw->hDC,draw->itemState & ODS_DISABLED ? RGB(135,135,135) : app->fg);
        auto r = draw->rcItem; if (draw->itemState & ODS_SELECTED) OffsetRect(&r, 1, 1);
        wchar_t text[256]{}; GetWindowTextW(draw->hwndItem, text, 256); const auto old = SelectObject(draw->hDC, app->font);
        DrawTextW(draw->hDC,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX|(app->rtl?DT_RTLREADING:0));
        SelectObject(draw->hDC, old); if ((draw->itemState & ODS_FOCUS) && !(draw->itemState & ODS_NOFOCUSRECT)) { InflateRect(&r,-3,-3); DrawFocusRect(draw->hDC,&r); } return TRUE;
    }
    case WM_PAINT: app->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case completed: app->finish(); return 0;
    case WM_APP + 50: app->privacy.sync(); return 0;
    case WM_CLOSE:
        if (!app->force_exit && app->profile.preferences.close_to_tray) {
            if (app->tray.active()) ShowWindow(hwnd,SW_HIDE); else app->fail(ERROR_NOT_READY);
            return 0;
        }
        KillTimer(hwnd,7); KillTimer(hwnd,8); app->stop_all_sessions(); app->privacy.detach(); app->tray.remove(); app->layers.clear(); app->taskbar.restore_all();
        app->cancel = true; if (app->worker.joinable()) app->worker.join(); app->pending.reset(); av::cleanup_session_payloads(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    std::vector<std::wstring> arguments;
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        LocalFree(argv);
    }
    const av::CommandLineResult command = av::parse_command_line(arguments);
    if (!command) return av::report_command_line_error(command);
    if (command.command.action != av::CommandLineAction::gui) return av::run_command_line(command.command);
    Gdiplus::GdiplusStartupInput graphics_input; ULONG_PTR graphics_token=0;
    if (Gdiplus::GdiplusStartup(&graphics_token,&graphics_input,nullptr)!=Gdiplus::Ok) return 1;
    int result = 1;
    {
        App app; app.instance = instance;
        HDC screen = GetDC(nullptr); app.dpi = static_cast<UINT>(GetDeviceCaps(screen,LOGPIXELSX)); ReleaseDC(nullptr,screen);
        using GetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (const auto monitor_dpi = reinterpret_cast<GetDpiForMonitor>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"))) {
            UINT x = 0, y = 0;
            POINT origin{0, 0};
            if (SUCCEEDED(monitor_dpi(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), 0, &x, &y)) && x) app.dpi = x;
        }
        app.config_path = command.command.profile_path.empty() ? av::default_profile_path() : command.command.profile_path;
        DWORD error = 0;
        const bool has_config = !app.config_path.empty() && GetFileAttributesW(app.config_path.c_str()) != INVALID_FILE_ATTRIBUTES;
        bool loaded = !has_config;
        if (has_config) {
            av::ProfileV4 modern;
            const auto load_result = av::load_profile_v4(app.config_path, modern);
            loaded = load_result.ok();
            error = load_result.error;
            if (loaded) app.profile = legacy_profile(modern);
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES|ICC_TAB_CLASSES}; InitCommonControlsEx(&controls);
        WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr,IDC_ARROW); cls.lpszClassName = L"Henglie.AkashaVeil.Main";
        cls.hIcon = av::load_app_icon(instance, GetSystemMetrics(SM_CXICON)); cls.hIconSm = av::load_app_icon(instance, GetSystemMetrics(SM_CXSMICON));
        if (RegisterClassExW(&cls)) {
            const HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"AkashaVeil",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,app.px(980),app.px(650),nullptr,nullptr,instance,&app);
            if (hwnd) {
                app.privacy.attach(hwnd, WM_APP + 50, nullptr, nullptr);
                ShowWindow(hwnd,show); app.shown=true;
                UINT actual_dpi = app.dpi;
                using GetWindowDpi = UINT(WINAPI*)(HWND);
                if (const auto window_dpi = reinterpret_cast<GetWindowDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))) {
                    const UINT value = window_dpi(hwnd);
                    if (value) actual_dpi = value;
                }
                if (actual_dpi != app.dpi) { app.dpi = actual_dpi; app.update_fonts(); app.appearance(); }
                SetWindowPos(hwnd,nullptr,0,0,MulDiv(980, static_cast<int>(actual_dpi), 96),
                    MulDiv(650, static_cast<int>(actual_dpi), 96), SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                UpdateWindow(hwnd); if (!loaded) app.fail(error);
                MSG msg{};
                while (GetMessageW(&msg,nullptr,0,0) > 0) {
                    if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
                        if (msg.wParam == 'F') { SetFocus(app.edit); SendMessageW(app.edit,EM_SETSEL,0,-1); continue; }
                        if (GetFocus()!=app.edit && msg.wParam == 'H') { app.command(exclude); continue; }
                        if (GetFocus()!=app.edit && msg.wParam == 'U') { app.command(restore); continue; }
                        if (msg.wParam == 'A' && GetFocus() == app.list) { app.command(select_all); continue; }
                        if (msg.wParam == 'S') { app.command(save); continue; }
                        if (msg.wParam == 'O') { app.command(load); continue; }
                        if (msg.wParam == 'E') { app.command(export_list); continue; }
                        if (msg.wParam == 'R') { app.command(refresh); continue; }
                        if (msg.wParam == 'P' || msg.wParam == VK_OEM_COMMA) { app.command(preferences); continue; }
                        if (msg.wParam == 'W') { app.command(effects); continue; }
                    }
                    if (msg.message==WM_KEYDOWN && msg.wParam==VK_F1) { app.command(help); continue; }
                    if (msg.message==WM_KEYDOWN && msg.wParam==VK_F5) { app.command(refresh); continue; }
                    if (msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE && GetFocus()==app.edit) { app.command(clear_filter); continue; }
                    if (!IsDialogMessageW(hwnd,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                }
                result = static_cast<int>(msg.wParam);
            }
        }
    }
    Gdiplus::GdiplusShutdown(graphics_token);
    return result;
}

