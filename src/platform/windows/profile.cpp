// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/profile.h"
#include <cerrno>
#include <cwchar>
#include <limits>

namespace av {
static constexpr std::size_t max_file = 4 * 1024 * 1024;
static bool atomic_write(const std::wstring& name, const std::string& data, DWORD& error) {
    wchar_t absolute[32768]{};
    const DWORD n = GetFullPathNameW(name.c_str(), 32768, absolute, nullptr);
    if (!n || n >= 32768 || data.size() > max_file) { error = ERROR_INVALID_PARAMETER; return false; }
    const std::wstring path(absolute);
    const auto parent = path.substr(0, path.find_last_of(L"\\/"));
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(parent.c_str(), L"AKV", 0, temporary)) { error = GetLastError(); return false; }
    HANDLE f = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size() && FlushFileBuffers(f);
    error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(f);
    if (!ok) return false;
    if (!MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { error = GetLastError(); return false; }
    return true;
}
static std::wstring encode(const std::wstring& value) {
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    for (const auto c : value) for (int shift = 12; shift >= 0; shift -= 4) out += hex[(static_cast<unsigned>(c) >> shift) & 15];
    return out;
}
static bool decode(const std::wstring& value, std::wstring& out) {
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    if (value.size() % 4) return false;
    out.clear();
    for (std::size_t i = 0; i < value.size(); i += 4) {
        unsigned c = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto p = std::wcschr(digits, value[i + j]);
            if (!p || value[i + j] == L'\0') return false;
            c = c * 16 + static_cast<unsigned>(p - digits);
        }
        if (!c) return false;
        out += static_cast<wchar_t>(c);
    }
    return true;
}
bool save_profile(const std::wstring& file, const Profile& p, DWORD& error) {
    const auto& preferences=p.preferences;
    if (p.rules.size() > 512 || preferences.programs.size()>256 || preferences.refresh_ms<100 || preferences.refresh_ms>60000
        || preferences.window_wait_ms>60000 || (preferences.auto_mode!=Mode::blank && preferences.auto_mode!=Mode::exclude)
        || ((!preferences.tray) && (preferences.close_to_tray || preferences.hide_taskbar))) { error = ERROR_INVALID_DATA; return false; }
    for (const auto& program : preferences.programs) if (program.empty() || program.size()>4096 || program.find_first_of(L"\r\n")!=std::wstring::npos) { error=ERROR_INVALID_DATA; return false; }
    if (!valid_mode(static_cast<unsigned>(p.effect_mode)) || !valid_effects(p.effects)) { error=ERROR_INVALID_DATA; return false; }
    for (const auto& r : p.rules) if (!valid_mode(static_cast<unsigned>(r.mode)) || !valid_effects(r.effects)) { error=ERROR_INVALID_DATA; return false; }
    std::wstring out = L"\xFEFF[AkashaVeil]\r\nschema=3\r\nlanguage=" + p.language
        + L"\r\ntheme=" + std::to_wstring(static_cast<int>(p.theme)) + L"\r\naccent=" + std::to_wstring(p.accent)
        + L"\r\ncount=" + std::to_wstring(p.rules.size()) + L"\r\n";
    out+=L"\r\n[Preferences]\r\n";
    for (const auto& setting : bool_preferences) out+=std::wstring(setting.key)+L"="+std::to_wstring(preferences.*(setting.member))+L"\r\n";
    out+=L"refresh_ms="+std::to_wstring(preferences.refresh_ms)+L"\r\nwindow_wait_ms="+std::to_wstring(preferences.window_wait_ms)
        +L"\r\nauto_mode="+std::to_wstring(static_cast<unsigned>(preferences.auto_mode))+L"\r\nprogram_count="+std::to_wstring(preferences.programs.size())+L"\r\n";
    for (std::size_t i=0;i<preferences.programs.size();++i) out+=L"program_"+std::to_wstring(i)+L"="+encode(preferences.programs[i])+L"\r\n";
    auto effects=[&](const Effects& e) {
        out+=L"cover="+std::to_wstring(static_cast<unsigned>(e.cover))+L"\r\nmark="+std::to_wstring(static_cast<unsigned>(e.mark))
            +L"\r\ncapture_only="+std::to_wstring(e.capture_only)+L"\r\ntext="+encode(e.text)+L"\r\nlabel="+encode(e.label)+L"\r\ntag="+encode(e.tag)
            +L"\r\nopacity="+std::to_wstring(e.opacity)+L"\r\nfont_size="+std::to_wstring(e.font_size)+L"\r\nspacing="+std::to_wstring(e.spacing)
            +L"\r\nforeground="+std::to_wstring(e.foreground)+L"\r\nbackground="+std::to_wstring(e.background)+L"\r\n";
    };
    out+=L"\r\n[Effects]\r\nmode="+std::to_wstring(static_cast<unsigned>(p.effect_mode))+L"\r\n"; effects(p.effects);
    for (std::size_t i = 0; i < p.rules.size(); ++i) {
        const auto& r = p.rules[i];
        out += L"\r\n[Window." + std::to_wstring(i) + L"]\r\npath=" + encode(r.path)
            + L"\r\nclass=" + encode(r.class_name) + L"\r\ntitle=" + encode(r.title)
            + L"\r\nmode=" + std::to_wstring(static_cast<unsigned>(r.mode))
            + L"\r\nx=" + std::to_wstring(r.x) + L"\r\ny=" + std::to_wstring(r.y)
            + L"\r\nwidth=" + std::to_wstring(r.width) + L"\r\nheight=" + std::to_wstring(r.height)
            + L"\r\nshow=" + std::to_wstring(r.show) + L"\r\n";
        effects(r.effects);
    }
    return atomic_write(file, std::string(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(wchar_t)), error);
}
bool load_profile(const std::wstring& name, Profile& profile, DWORD& error) {
    wchar_t absolute[32768]{};
    if (!GetFullPathNameW(name.c_str(), 32768, absolute, nullptr)) { error = GetLastError(); return false; }
    HANDLE f = CreateFileW(absolute, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    LARGE_INTEGER size{}; WORD bom = 0; DWORD read = 0;
    const bool valid = GetFileSizeEx(f, &size) && size.QuadPart >= 2 && size.QuadPart <= max_file && size.QuadPart % 2 == 0
        && ReadFile(f, &bom, 2, &read, nullptr) && read == 2 && bom == 0xFEFF;
    CloseHandle(f);
    error = ERROR_INVALID_DATA;
    if (!valid) return false;
    bool good = true;
    auto get = [&](const std::wstring& section, const wchar_t* key) {
        std::wstring s(32768, L'\0');
        const auto count = GetPrivateProfileStringW(section.c_str(), key, L"", s.data(), static_cast<DWORD>(s.size()), absolute);
        if (count >= s.size() - 1) good = false;
        s.resize(count); return s;
    };
    auto number = [&](const std::wstring& section, const wchar_t* key, int lo, int hi) {
        const auto s = get(section, key); wchar_t* end = nullptr;
        errno = 0;
        const auto v = std::wcstoll(s.c_str(), &end, 10);
        if (s.empty() || errno || *end || v < lo || v > hi) { good = false; return 0; }
        return static_cast<int>(v);
    };
    Profile candidate;
    const int schema=number(L"AkashaVeil",L"schema",1,3);
    if (!good) return false;
    if (schema==1) { candidate.preferences.tray=false; candidate.preferences.close_to_tray=false; }
    else {
        auto& preferences=candidate.preferences;
        for (const auto& setting : bool_preferences) preferences.*(setting.member)=number(L"Preferences",setting.key,0,1)!=0;
        preferences.refresh_ms=static_cast<unsigned>(number(L"Preferences",L"refresh_ms",100,60000));
        preferences.window_wait_ms=static_cast<unsigned>(number(L"Preferences",L"window_wait_ms",0,60000));
        const int mode=number(L"Preferences",L"auto_mode",1,17);
        if (mode!=1 && mode!=17) good=false;
        preferences.auto_mode=static_cast<Mode>(mode);
        const int programs=number(L"Preferences",L"program_count",0,256);
        for (int i=0;i<programs && good;++i) {
            const auto key=L"program_"+std::to_wstring(i); std::wstring program;
            good=decode(get(L"Preferences",key.c_str()),program) && !program.empty() && program.size()<=4096 && program.find_first_of(L"\r\n")==std::wstring::npos;
            preferences.programs.push_back(std::move(program));
        }
        if (!preferences.tray && (preferences.close_to_tray || preferences.hide_taskbar)) good=false;
    }
    candidate.language = get(L"AkashaVeil", L"language");
    const std::wstring languages[] = {L"auto", L"zh", L"en", L"es", L"fr", L"ru", L"ar", L"tr", L"ja", L"ko"};
    bool supported = false;
    for (const auto& language : languages) supported |= candidate.language == language;
    if (!supported) return false;
    candidate.theme = static_cast<Theme>(number(L"AkashaVeil", L"theme", 0, 2));
    candidate.accent = static_cast<unsigned>(number(L"AkashaVeil", L"accent", 0, 0xFFFFFF));
    auto effects=[&](const std::wstring& section,Effects& e) {
        e.cover=static_cast<Cover>(number(section,L"cover",0,2)); e.mark=static_cast<Mark>(number(section,L"mark",0,3));
        e.capture_only=number(section,L"capture_only",0,1)!=0;
        good&=decode(get(section,L"text"),e.text); good&=decode(get(section,L"label"),e.label); good&=decode(get(section,L"tag"),e.tag);
        e.opacity=static_cast<unsigned>(number(section,L"opacity",5,100)); e.font_size=static_cast<unsigned>(number(section,L"font_size",10,96));
        e.spacing=static_cast<unsigned>(number(section,L"spacing",100,600));
        e.foreground=static_cast<unsigned>(number(section,L"foreground",0,0xFFFFFF)); e.background=static_cast<unsigned>(number(section,L"background",0,0xFFFFFF));
        good&=valid_effects(e);
    };
    if (schema>=3) {
        const auto mode=static_cast<unsigned>(number(L"Effects",L"mode",0,17)); good&=valid_mode(mode); candidate.effect_mode=static_cast<Mode>(mode);
        effects(L"Effects",candidate.effects);
    }
    const int count = number(L"AkashaVeil", L"count", 0, 512);
    for (int i = 0; i < count && good; ++i) {
        const auto section = L"Window." + std::to_wstring(i);
        Rule r;
        good &= decode(get(section, L"path"), r.path) && !r.path.empty();
        good &= decode(get(section, L"class"), r.class_name);
        good &= decode(get(section, L"title"), r.title);
        const auto mode = static_cast<unsigned>(number(section, L"mode", 0, 0x11));
        good &= valid_mode(mode); r.mode = static_cast<Mode>(mode);
        r.x = number(section, L"x", -1000000, 1000000); r.y = number(section, L"y", -1000000, 1000000);
        r.width = number(section, L"width", 1, 65535); r.height = number(section, L"height", 1, 65535);
        r.show = number(section, L"show", 1, 3);
        if (schema>=3) effects(section,r.effects);
        candidate.rules.push_back(std::move(r));
    }
    if (!good) return false;
    profile = std::move(candidate); error = ERROR_SUCCESS; return true;
}
static std::wstring cell(std::wstring value) {
    // Prevent spreadsheet formulas while preserving the underlying text in CSV.
    if (!value.empty() && (value[0] == L'=' || value[0] == L'+' || value[0] == L'-' || value[0] == L'@')) value.insert(0, 1, L'\'');
    std::wstring out = L"\"";
    for (auto c : value) { out += c; if (c == L'\"') out += c; }
    return out + L"\"";
}
bool export_windows(const std::wstring& file, const std::vector<Window>& windows, DWORD& error) {
    std::wstring out = L"HWND,PID,Title,Class,Path,Architecture,X,Y,Width,Height,Show,Affinity\r\n";
    for (const auto& w : windows) out += std::to_wstring(w.handle) + L"," + std::to_wstring(w.pid) + L"," + cell(w.title) + L"," + cell(w.class_name)
        + L"," + cell(w.path) + L"," + cell(w.architecture) + L"," + std::to_wstring(w.x) + L"," + std::to_wstring(w.y)
        + L"," + std::to_wstring(w.width) + L"," + std::to_wstring(w.height) + L"," + std::to_wstring(w.show)
        + L"," + (w.affinity_known ? std::to_wstring(static_cast<unsigned>(w.mode)) : L"") + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), nullptr, 0, nullptr, nullptr);
    if (!n) { error = GetLastError(); return false; }
    std::string bytes = "\xEF\xBB\xBF"; bytes.resize(static_cast<std::size_t>(n) + 3);
    WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), bytes.data() + 3, n, nullptr, nullptr);
    return atomic_write(file, bytes, error);
}
}
