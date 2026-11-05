// SPDX-License-Identifier: Apache-2.0
#include "core/model.h"
#include <cwctype>
#include <tuple>

namespace av {
static bool equal_path(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto left = a[i] == L'/' ? L'\\' : std::towlower(a[i]);
        const auto right = b[i] == L'/' ? L'\\' : std::towlower(b[i]);
        if (left != right) return false;
    }
    return true;
}
bool matches(const Rule& r, const Window& w) {
    return !r.path.empty() && !w.path.empty() && equal_path(r.path, w.path)
        && (r.class_name.empty() || r.class_name == w.class_name)
        && (r.title.empty() || r.title == w.title);
}
Rule remember(const Window& w, Mode mode) {
    return {w.path, w.class_name, w.title, mode, w.x, w.y, w.width, w.height, w.show};
}
bool valid_mode(unsigned v) { return v == 0 || v == 1 || v == 0x11; }
bool valid_effects(const Effects& e) {
    auto valid_text=[](const std::wstring& s) { return s.size()<=1024 && s.find(L'\0')==std::wstring::npos; };
    return static_cast<unsigned>(e.cover)<=2 && static_cast<unsigned>(e.mark)<=3
        && e.opacity>=5 && e.opacity<=100 && e.font_size>=10 && e.font_size<=96
        && e.spacing>=100 && e.spacing<=600 && e.foreground<=0xFFFFFF && e.background<=0xFFFFFF
        && e.hidden_algorithm<=0xFFFF && (e.capture_route==1 || e.capture_route==2)
        && e.hidden_payload.size()<=1024 && e.hidden_payload.find(L'\0')==std::wstring::npos
        && e.hidden_payload_bytes.size()<=4096
        && valid_text(e.text) && valid_text(e.label) && valid_text(e.tag)
        && (e.mark==Mark::none || e.text.find_first_not_of(L" \r\n\t")!=std::wstring::npos)
        && (e.cover!=Cover::label || e.label.find_first_not_of(L" \r\n\t")!=std::wstring::npos);
}
bool has_effects(const Effects& e) { return e.cover!=Cover::none || e.mark!=Mark::none || e.hidden_algorithm!=0; }
bool operator==(const Effects& a,const Effects& b) {
    return std::tie(a.cover,a.mark,a.capture_only,a.text,a.label,a.tag,a.opacity,a.font_size,a.spacing,a.foreground,a.background,a.hidden_algorithm,a.hidden_payload,a.hidden_payload_bytes,a.capture_route)
        ==std::tie(b.cover,b.mark,b.capture_only,b.text,b.label,b.tag,b.opacity,b.font_size,b.spacing,b.foreground,b.background,b.hidden_algorithm,b.hidden_payload,b.hidden_payload_bytes,b.capture_route);
}
Mode effective_mode(Mode mode,const Effects& e) {
    if (e.cover!=Cover::none) return Mode::exclude;
    if (e.capture_only && (e.mark!=Mark::none || e.hidden_algorithm!=0)) return Mode::none;
    return mode;
}
std::wstring expand_mark(const std::wstring& input,const Window& window,const std::wstring& tag,const std::wstring& time) {
    std::wstring out;
    for (std::size_t i=0;i<input.size() && out.size()<4096;) {
        if (input[i]==L'{') {
            const auto end=input.find(L'}',i+1);
            if (end!=std::wstring::npos) {
                const auto token=input.substr(i,end-i+1);
                const std::wstring* replacement=nullptr;
                const auto pid=std::to_wstring(window.pid);
                const auto process=window.path.substr(window.path.find_last_of(L"\\/")+1);
                if (token==L"{title}") replacement=&window.title;
                else if (token==L"{process}") replacement=&process;
                else if (token==L"{pid}") replacement=&pid;
                else if (token==L"{id}") replacement=&tag;
                else if (token==L"{time}") replacement=&time;
                if (replacement) { out.append(*replacement,0,4096-out.size()); i=end+1; continue; }
            }
        }
        out+=input[i++];
    }
    return out;
}
}
