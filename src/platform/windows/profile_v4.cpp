// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/profile_v4.h"
#include "core/watermark.h"
#include "platform/windows/profile.h"
#include <cerrno>
#include <cwchar>
#include <limits>
#include <utility>

namespace av {

namespace {

constexpr std::size_t max_file = 4 * 1024 * 1024;
constexpr std::size_t max_rules = 512;
constexpr std::size_t max_programs = 256;

bool utf8_from_wide(const std::wstring& value, std::vector<std::uint8_t>& out) {
    out.clear();
    if (value.empty()) return true;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    out.resize(static_cast<std::size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), reinterpret_cast<char*>(out.data()), size,
        nullptr, nullptr) == size;
}

bool wide_from_utf8(const std::vector<std::uint8_t>& value, std::wstring& out) {
    out.clear();
    if (value.empty()) return true;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(value.data()), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) return false;
    out.resize(static_cast<std::size_t>(size));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(value.data()), static_cast<int>(value.size()),
        out.data(), size) == size;
}

std::wstring payload_hex(const std::vector<std::uint8_t>& value) {
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(value.size() * 2);
    for (const auto byte : value) {
        out += hex[(byte >> 4) & 15];
        out += hex[byte & 15];
    }
    return out;
}

bool supported_language(const std::wstring& language) {
    static const std::wstring languages[] = {L"auto", L"zh", L"en", L"es", L"fr",
        L"ru", L"ar", L"tr", L"ja", L"ko"};
    for (const auto& item : languages)
        if (language == item) return true;
    return false;
}

bool text_field_ok(const std::wstring& value, std::size_t limit) {
    return value.size() <= limit && value.find(L'\0') == std::wstring::npos;
}

bool valid_preferences(const Preferences& preferences) {
    if (preferences.refresh_ms < 100 || preferences.refresh_ms > 60000
        || preferences.window_wait_ms > 60000
        || (preferences.auto_mode != Mode::blank && preferences.auto_mode != Mode::exclude)
        || preferences.programs.size() > max_programs)
        return false;
    if (!preferences.tray && (preferences.close_to_tray || preferences.hide_taskbar))
        return false;
    for (const auto& program : preferences.programs)
        if (program.empty() || program.size() > 4096
            || program.find_first_of(L"\r\n") != std::wstring::npos)
            return false;
    return true;
}

std::wstring encode_text(const std::wstring& value) {
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    for (const auto c : value)
        for (int shift = 12; shift >= 0; shift -= 4)
            out += hex[(static_cast<unsigned>(c) >> shift) & 15];
    return out;
}

bool decode_text(const std::wstring& value, std::wstring& out) {
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

std::wstring encode_bytes(const std::vector<std::uint8_t>& value) {
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(value.size() * 2);
    for (const auto byte : value) {
        out += hex[(byte >> 4) & 15];
        out += hex[byte & 15];
    }
    return out;
}

bool decode_bytes(const std::wstring& value, std::size_t limit,
    std::vector<std::uint8_t>& out) {
    if (value.size() % 2 || value.size() / 2 > limit) return false;
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    out.clear();
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        unsigned byte = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const auto p = std::wcschr(digits, value[i + j]);
            if (!p || value[i + j] == L'\0') return false;
            byte = byte * 16 + static_cast<unsigned>(p - digits);
        }
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return true;
}

bool atomic_write(const std::wstring& name, const std::string& data, DWORD& error) {
    wchar_t absolute[32768]{};
    const DWORD n = GetFullPathNameW(name.c_str(), 32768, absolute, nullptr);
    if (!n || n >= 32768 || data.size() > max_file) { error = ERROR_INVALID_PARAMETER; return false; }
    const std::wstring path(absolute);
    const auto parent = path.substr(0, path.find_last_of(L"\\/"));
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(parent.c_str(), L"AKV", 0, temporary)) { error = GetLastError(); return false; }
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        DeleteFileW(temporary);
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
        && written == data.size() && FlushFileBuffers(file);
    error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary);
        return false;
    }
    if (!MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temporary);
        return false;
    }
    return true;
}

bool valid_utf16_file(const std::wstring& name, DWORD& error) {
    HANDLE file = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    LARGE_INTEGER size{};
    WORD bom = 0;
    DWORD read = 0;
    const bool valid = GetFileSizeEx(file, &size) && size.QuadPart >= 2
        && size.QuadPart <= static_cast<LONGLONG>(max_file) && size.QuadPart % 2 == 0
        && ReadFile(file, &bom, 2, &read, nullptr) && read == 2 && bom == 0xFEFF;
    CloseHandle(file);
    error = valid ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    return valid;
}

int read_schema(const std::wstring& file) {
    wchar_t value[32]{};
    const auto count = GetPrivateProfileStringW(L"AkashaVeil", L"schema", L"",
        value, static_cast<DWORD>(std::size(value)), file.c_str());
    if (!count) return 0;
    wchar_t* end = nullptr;
    errno = 0;
    const auto schema = std::wcstoll(value, &end, 10);
    if (errno || *end || schema < 1 || schema > 1000) return 0;
    return static_cast<int>(schema);
}

struct Reader {
    std::wstring file;
    bool good = true;

    std::wstring get(const std::wstring& section, const wchar_t* key) {
        std::wstring value(32768, L'\0');
        const auto count = GetPrivateProfileStringW(section.c_str(), key, L"",
            value.data(), static_cast<DWORD>(value.size()), file.c_str());
        if (count >= value.size() - 1) good = false;
        value.resize(count);
        return value;
    }

    bool required_number(const std::wstring& section, const wchar_t* key,
        int low, int high, int& out) {
        const auto value = get(section, key);
        wchar_t* end = nullptr;
        errno = 0;
        const auto parsed = std::wcstoll(value.c_str(), &end, 10);
        if (value.empty() || errno || *end || parsed < low || parsed > high) {
            good = false;
            return false;
        }
        out = static_cast<int>(parsed);
        return true;
    }

    bool optional_number(const std::wstring& section, const wchar_t* key,
        int fallback, int low, int high, int& out) {
        const auto value = get(section, key);
        if (value.empty()) { out = fallback; return true; }
        wchar_t* end = nullptr;
        errno = 0;
        const auto parsed = std::wcstoll(value.c_str(), &end, 10);
        if (errno || *end || parsed < low || parsed > high) {
            good = false;
            return false;
        }
        out = static_cast<int>(parsed);
        return true;
    }

    bool bytes(const std::wstring& section, const wchar_t* key,
        std::size_t limit, std::vector<std::uint8_t>& out) {
        std::vector<std::uint8_t> parsed;
        if (!decode_bytes(get(section, key), limit, parsed)) {
            good = false;
            return false;
        }
        out = std::move(parsed);
        return true;
    }
};

void parse_effects(Reader& reader, const std::wstring& section, Effects& effects) {
    int cover = 0, mark = 0, capture = 0, opacity = 0, font = 0, spacing = 0;
    int foreground = 0, background = 0;
    reader.required_number(section, L"cover", 0, 2, cover);
    reader.required_number(section, L"mark", 0, 3, mark);
    reader.required_number(section, L"capture_only", 0, 1, capture);
    if (!decode_text(reader.get(section, L"text"), effects.text)) reader.good = false;
    if (!decode_text(reader.get(section, L"label"), effects.label)) reader.good = false;
    if (!decode_text(reader.get(section, L"tag"), effects.tag)) reader.good = false;
    reader.required_number(section, L"opacity", 5, 100, opacity);
    reader.required_number(section, L"font_size", 10, 96, font);
    reader.required_number(section, L"spacing", 100, 600, spacing);
    reader.required_number(section, L"foreground", 0, 0xFFFFFF, foreground);
    reader.required_number(section, L"background", 0, 0xFFFFFF, background);
    effects.cover = static_cast<Cover>(cover);
    effects.mark = static_cast<Mark>(mark);
    effects.capture_only = capture != 0;
    effects.opacity = static_cast<unsigned>(opacity);
    effects.font_size = static_cast<unsigned>(font);
    effects.spacing = static_cast<unsigned>(spacing);
    effects.foreground = static_cast<unsigned>(foreground);
    effects.background = static_cast<unsigned>(background);
    if (!valid_effects(effects)) reader.good = false;
}

void parse_hidden(Reader& reader, const std::wstring& section,
    HiddenWatermark& hidden, bool inherit) {
    const auto algorithm = reader.get(section, L"hidden_algorithm");
    if (algorithm.empty()) {
        if (!inherit) hidden = HiddenWatermark{};
        return;
    }
    HiddenWatermark parsed;
    wchar_t* end = nullptr;
    errno = 0;
    const auto value = std::wcstoll(algorithm.c_str(), &end, 10);
    if (errno || *end || value < 0 || value > 0xFFFF) {
        reader.good = false;
        return;
    }
    parsed.algorithm = static_cast<std::uint16_t>(value);
    int flags = 0;
    reader.optional_number(section, L"hidden_flags", 0, 0, 0xFFFF, flags);
    parsed.flags = static_cast<std::uint16_t>(flags);
    reader.bytes(section, L"hidden_payload", watermark_max_payload, parsed.payload);
    reader.bytes(section, L"hidden_params", watermark_max_parameters, parsed.parameters);
    reader.bytes(section, L"hidden_key", watermark_max_key, parsed.key);
    if (parsed.algorithm != 0 && parsed.payload.empty()) reader.good = false;
    if (reader.good) hidden = std::move(parsed);
}

void parse_request_overrides(Reader& reader, const std::wstring& section,
    ProtectionRequest& request) {
    int value = 0;
    reader.optional_number(section, L"replacement_kind",
        static_cast<int>(request.replacement.kind), 0, 2, value);
    request.replacement.kind = static_cast<ReplacementKind>(value);
    if (!decode_text(reader.get(section, L"replacement_label"), request.replacement.label))
        reader.good = false;
    reader.optional_number(section, L"replacement_opacity",
        static_cast<int>(request.replacement.opacity), 5, 100, value);
    request.replacement.opacity = static_cast<unsigned>(value);
    reader.optional_number(section, L"replacement_foreground",
        static_cast<int>(request.replacement.foreground), 0, 0xFFFFFF, value);
    request.replacement.foreground = static_cast<std::uint32_t>(value);
    reader.optional_number(section, L"replacement_background",
        static_cast<int>(request.replacement.background), 0, 0xFFFFFF, value);
    request.replacement.background = static_cast<std::uint32_t>(value);

    int enabled = request.visible.enabled ? 1 : 0;
    reader.optional_number(section, L"visible_enabled", enabled, 0, 1, enabled);
    request.visible.enabled = enabled != 0;
    reader.optional_number(section, L"visible_layout",
        static_cast<int>(request.visible.layout), 0, 3, value);
    request.visible.layout = static_cast<MarkLayout>(value);
    if (!decode_text(reader.get(section, L"visible_text"), request.visible.text))
        reader.good = false;
    if (!decode_text(reader.get(section, L"visible_label"), request.visible.label))
        reader.good = false;
    if (!decode_text(reader.get(section, L"visible_tag"), request.visible.tag))
        reader.good = false;
    reader.optional_number(section, L"visible_opacity",
        static_cast<int>(request.visible.opacity), 5, 100, value);
    request.visible.opacity = static_cast<unsigned>(value);
    reader.optional_number(section, L"visible_font_size",
        static_cast<int>(request.visible.font_size), 10, 96, value);
    request.visible.font_size = static_cast<unsigned>(value);
    reader.optional_number(section, L"visible_spacing",
        static_cast<int>(request.visible.spacing), 100, 600, value);
    request.visible.spacing = static_cast<unsigned>(value);
    reader.optional_number(section, L"visible_foreground",
        static_cast<int>(request.visible.foreground), 0, 0xFFFFFF, value);
    request.visible.foreground = static_cast<std::uint32_t>(value);
    reader.optional_number(section, L"visible_background",
        static_cast<int>(request.visible.background), 0, 0xFFFFFF, value);
    request.visible.background = static_cast<std::uint32_t>(value);
}

void write_request_fields(std::wstring& out, const ProtectionRequest& request) {
    out += L"replacement_kind=" + std::to_wstring(static_cast<unsigned>(request.replacement.kind))
        + L"\r\nreplacement_label=" + encode_text(request.replacement.label)
        + L"\r\nreplacement_opacity=" + std::to_wstring(request.replacement.opacity)
        + L"\r\nreplacement_foreground=" + std::to_wstring(request.replacement.foreground)
        + L"\r\nreplacement_background=" + std::to_wstring(request.replacement.background)
        + L"\r\nvisible_enabled=" + std::to_wstring(request.visible.enabled)
        + L"\r\nvisible_layout=" + std::to_wstring(static_cast<unsigned>(request.visible.layout))
        + L"\r\nvisible_text=" + encode_text(request.visible.text)
        + L"\r\nvisible_label=" + encode_text(request.visible.label)
        + L"\r\nvisible_tag=" + encode_text(request.visible.tag)
        + L"\r\nvisible_opacity=" + std::to_wstring(request.visible.opacity)
        + L"\r\nvisible_font_size=" + std::to_wstring(request.visible.font_size)
        + L"\r\nvisible_spacing=" + std::to_wstring(request.visible.spacing)
        + L"\r\nvisible_foreground=" + std::to_wstring(request.visible.foreground)
        + L"\r\nvisible_background=" + std::to_wstring(request.visible.background) + L"\r\n";
}

void write_effects(std::wstring& out, const Effects& effects) {
    out += L"cover=" + std::to_wstring(static_cast<unsigned>(effects.cover))
        + L"\r\nmark=" + std::to_wstring(static_cast<unsigned>(effects.mark))
        + L"\r\ncapture_only=" + std::to_wstring(effects.capture_only)
        + L"\r\ntext=" + encode_text(effects.text)
        + L"\r\nlabel=" + encode_text(effects.label)
        + L"\r\ntag=" + encode_text(effects.tag)
        + L"\r\nopacity=" + std::to_wstring(effects.opacity)
        + L"\r\nfont_size=" + std::to_wstring(effects.font_size)
        + L"\r\nspacing=" + std::to_wstring(effects.spacing)
        + L"\r\nforeground=" + std::to_wstring(effects.foreground)
        + L"\r\nbackground=" + std::to_wstring(effects.background) + L"\r\n";
}

void write_hidden(std::wstring& out, const HiddenWatermark& hidden) {
    out += L"hidden_algorithm=" + std::to_wstring(hidden.algorithm)
        + L"\r\nhidden_flags=" + std::to_wstring(hidden.flags)
        + L"\r\nhidden_payload=" + encode_bytes(hidden.payload)
        + L"\r\nhidden_params=" + encode_bytes(hidden.parameters)
        + L"\r\nhidden_key=" + encode_bytes(hidden.key) + L"\r\n";
}

void write_protection_fields(std::wstring& out, const ProtectionRequest& request) {
    out += L"visible_audience=" + std::to_wstring(static_cast<unsigned>(request.visible.audience))
        + L"\r\ntaskbar=" + std::to_wstring(static_cast<unsigned>(request.taskbar))
        + L"\r\ncapture_route=" + std::to_wstring(static_cast<unsigned>(request.capture))
        + L"\r\nflags=" + std::to_wstring(request.extension_flags) + L"\r\n";
    write_hidden(out, request.hidden);
}

}

MatchRule RuleV4::to_match_rule() const {
    MatchRule rule;
    rule.path = path;
    rule.class_name = class_name;
    rule.title = title;
    rule.enabled = enabled;
    rule.path_case_insensitive = true;
    return rule;
}

ProtectionRequest request_from_effects(Mode affinity, const Effects& effects) {
    ProtectionRequest request;
    request.affinity = affinity_from_unsigned(
        static_cast<unsigned>(effective_mode(affinity, effects)));
    request.capture = effects.capture_only
        ? (effects.capture_route == 2 ? CaptureRoutePreference::proxy_source_b
                                      : CaptureRoutePreference::display_capture_a)
        : CaptureRoutePreference::automatic;
    request.replacement.kind = static_cast<ReplacementKind>(effects.cover);
    request.replacement.label = effects.label;
    request.replacement.opacity = effects.opacity;
    request.replacement.foreground = effects.foreground;
    request.replacement.background = effects.background;
    request.visible.enabled = effects.mark != Mark::none;
    request.visible.layout = static_cast<MarkLayout>(effects.mark);
    request.visible.audience = effects.capture_only
        ? WatermarkAudience::capture_only : WatermarkAudience::screen_and_capture;
    request.visible.text = effects.text;
    request.visible.label = effects.label;
    request.visible.tag = effects.tag;
    request.visible.opacity = effects.opacity;
    request.visible.font_size = effects.font_size;
    request.visible.spacing = effects.spacing;
    request.visible.foreground = effects.foreground;
    request.visible.background = effects.background;
    request.hidden.algorithm = static_cast<std::uint16_t>(effects.hidden_algorithm);
    request.hidden.flags = 0;
    request.hidden.parameters.clear();
    // The product never persists a hidden-watermark key.  The schema still
    // accepts a key supplied by an external tool, but the UI path leaves it
    // empty so saving a profile cannot retain it.
    request.hidden.key.clear();
    if (effects.hidden_algorithm != 0) {
        if (!effects.hidden_payload_bytes.empty())
            request.hidden.payload = effects.hidden_payload_bytes;
        else
            utf8_from_wide(effects.hidden_payload, request.hidden.payload);
    }
    return request;
}

Effects effects_from_request(const ProtectionRequest& request) {
    Effects effects;
    effects.cover = static_cast<Cover>(request.replacement.kind);
    effects.mark = request.visible.enabled ? static_cast<Mark>(request.visible.layout) : Mark::none;
    effects.capture_only = request.visible.audience == WatermarkAudience::capture_only;
    effects.text = request.visible.text;
    effects.label = request.replacement.kind != ReplacementKind::none
        ? request.replacement.label : request.visible.label;
    effects.tag = request.visible.tag;
    effects.opacity = request.visible.enabled ? request.visible.opacity : request.replacement.opacity;
    effects.font_size = request.visible.font_size;
    effects.spacing = request.visible.spacing;
    effects.foreground = request.visible.enabled
        ? request.visible.foreground : request.replacement.foreground;
    effects.background = request.visible.enabled
        ? request.visible.background : request.replacement.background;
    effects.hidden_algorithm = request.hidden.algorithm;
    effects.hidden_payload_bytes = request.hidden.payload;
    effects.hidden_payload.clear();
    if (!request.hidden.payload.empty()
        && !wide_from_utf8(request.hidden.payload, effects.hidden_payload))
        effects.hidden_payload = payload_hex(request.hidden.payload);
    effects.capture_route = static_cast<unsigned>(request.capture) == 2 ? 2u : 1u;
    return effects;
}

ProfileV4 migrate_profile_v3(const Profile& legacy, int source_schema) {
    ProfileV4 profile;
    profile.language = legacy.language;
    profile.theme = legacy.theme;
    profile.accent = legacy.accent;
    profile.preferences = legacy.preferences;
    profile.defaults = request_from_effects(legacy.effect_mode, legacy.effects);
    profile.rules.reserve(legacy.rules.size());
    for (const auto& rule : legacy.rules) {
        RuleV4 migrated;
        migrated.path = rule.path;
        migrated.class_name = rule.class_name;
        migrated.title = rule.title;
        migrated.x = rule.x;
        migrated.y = rule.y;
        migrated.width = rule.width;
        migrated.height = rule.height;
        migrated.show = rule.show;
        migrated.enabled = true;
        migrated.request = request_from_effects(rule.mode, rule.effects);
        profile.rules.push_back(std::move(migrated));
    }
    profile.source_schema = source_schema;
    return profile;
}

bool valid_profile_v4(const ProfileV4& profile, DWORD& error) noexcept {
    if (!supported_language(profile.language)
        || static_cast<unsigned>(profile.theme) > 2
        || profile.accent > 0xFFFFFF
        || profile.rules.size() > max_rules
        || !valid_preferences(profile.preferences)
        || !valid_protection_request(profile.defaults)) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    for (const auto& rule : profile.rules) {
        if (rule.path.empty() || !text_field_ok(rule.path, 32768)
            || !text_field_ok(rule.class_name, 1024)
            || !text_field_ok(rule.title, 32768)
            || rule.x < -1000000 || rule.x > 1000000
            || rule.y < -1000000 || rule.y > 1000000
            || rule.width < 1 || rule.width > 65535
            || rule.height < 1 || rule.height > 65535
            || rule.show < 1 || rule.show > 3
            || !valid_protection_request(rule.request)) {
            error = ERROR_INVALID_DATA;
            return false;
        }
    }
    error = ERROR_SUCCESS;
    return true;
}

ProfileLoadResult load_profile_v4(const std::wstring& file, ProfileV4& profile) {
    ProfileLoadResult result;
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.status = ProfileLoadStatus::not_found;
        result.error = GetLastError();
        return result;
    }
    DWORD file_error = 0;
    if (!valid_utf16_file(file, file_error)) {
        result.status = ProfileLoadStatus::invalid_data;
        result.error = file_error;
        return result;
    }
    const int schema = read_schema(file);
    if (schema == 0) {
        result.status = ProfileLoadStatus::invalid_data;
        return result;
    }
    if (schema > 4) {
        result.status = ProfileLoadStatus::unsupported_schema;
        result.error = ERROR_NOT_SUPPORTED;
        result.source_schema = schema;
        return result;
    }
    if (schema < 4) {
        Profile legacy;
        DWORD error = 0;
        if (!load_profile(file, legacy, error)) {
            result.status = error == ERROR_FILE_NOT_FOUND
                ? ProfileLoadStatus::not_found : ProfileLoadStatus::invalid_data;
            result.error = error;
            return result;
        }
        auto migrated = migrate_profile_v3(legacy, schema);
        DWORD validation = 0;
        if (!valid_profile_v4(migrated, validation)) {
            result.status = ProfileLoadStatus::invalid_data;
            result.error = validation;
            return result;
        }
        profile = std::move(migrated);
        result.status = ProfileLoadStatus::ok;
        result.error = ERROR_SUCCESS;
        result.source_schema = schema;
        return result;
    }

    Reader reader{file, true};
    ProfileV4 candidate;
    candidate.language = reader.get(L"AkashaVeil", L"language");
    if (!supported_language(candidate.language)) reader.good = false;
    int theme = 0, accent = 0, count = 0;
    reader.required_number(L"AkashaVeil", L"theme", 0, 2, theme);
    reader.required_number(L"AkashaVeil", L"accent", 0, 0xFFFFFF, accent);
    reader.required_number(L"AkashaVeil", L"count", 0, 512, count);
    candidate.theme = static_cast<Theme>(theme);
    candidate.accent = static_cast<unsigned>(accent);

    auto& preferences = candidate.preferences;
    for (const auto& setting : bool_preferences) {
        int value = 0;
        reader.required_number(L"Preferences", setting.key, 0, 1, value);
        preferences.*(setting.member) = value != 0;
    }
    int refresh = 0, wait = 0, auto_mode = 0, programs = 0;
    reader.required_number(L"Preferences", L"refresh_ms", 100, 60000, refresh);
    reader.required_number(L"Preferences", L"window_wait_ms", 0, 60000, wait);
    reader.required_number(L"Preferences", L"auto_mode", 1, 17, auto_mode);
    if (auto_mode != 1 && auto_mode != 17) reader.good = false;
    reader.required_number(L"Preferences", L"program_count", 0, 256, programs);
    preferences.refresh_ms = static_cast<unsigned>(refresh);
    preferences.window_wait_ms = static_cast<unsigned>(wait);
    preferences.auto_mode = static_cast<Mode>(auto_mode);
    for (int i = 0; i < programs && reader.good; ++i) {
        const auto key = L"program_" + std::to_wstring(i);
        std::wstring program;
        if (!decode_text(reader.get(L"Preferences", key.c_str()), program)
            || program.empty() || program.size() > 4096
            || program.find_first_of(L"\r\n") != std::wstring::npos)
            reader.good = false;
        preferences.programs.push_back(std::move(program));
    }
    if (!preferences.tray && (preferences.close_to_tray || preferences.hide_taskbar))
        reader.good = false;

    Effects default_effects;
    int effect_mode = 0;
    reader.required_number(L"Effects", L"mode", 0, 17, effect_mode);
    if (!valid_mode(static_cast<unsigned>(effect_mode))) reader.good = false;
    parse_effects(reader, L"Effects", default_effects);
    candidate.defaults = request_from_effects(static_cast<Mode>(effect_mode), default_effects);
    parse_request_overrides(reader, L"Protection", candidate.defaults);

    int affinity = static_cast<int>(candidate.defaults.affinity);
    reader.optional_number(L"Protection", L"affinity", affinity, 0, 17, affinity);
    if (!valid_affinity(static_cast<unsigned>(affinity))) reader.good = false;
    candidate.defaults.affinity = static_cast<AffinityMode>(affinity);
    int audience = static_cast<int>(candidate.defaults.visible.audience);
    reader.optional_number(L"Protection", L"visible_audience", audience, 0, 1, audience);
    candidate.defaults.visible.audience = static_cast<WatermarkAudience>(audience);
    int taskbar = static_cast<int>(candidate.defaults.taskbar);
    reader.optional_number(L"Protection", L"taskbar", taskbar, 0, 1, taskbar);
    candidate.defaults.taskbar = static_cast<TaskbarIntent>(taskbar);
    int capture = 0;
    reader.optional_number(L"Protection", L"capture_route", 0, 0, 2, capture);
    int flags = 0;
    reader.optional_number(L"Protection", L"flags", 0, 0, 0, flags);
    candidate.defaults.capture = static_cast<CaptureRoutePreference>(capture);
    candidate.defaults.extension_flags = static_cast<std::uint32_t>(flags);
    parse_hidden(reader, L"Protection", candidate.defaults.hidden, false);

    for (int i = 0; i < count && reader.good; ++i) {
        const auto section = L"Window." + std::to_wstring(i);
        RuleV4 rule;
        if (!decode_text(reader.get(section, L"path"), rule.path) || rule.path.empty())
            reader.good = false;
        if (!decode_text(reader.get(section, L"class"), rule.class_name))
            reader.good = false;
        if (!decode_text(reader.get(section, L"title"), rule.title))
            reader.good = false;
        int mode = 0;
        reader.required_number(section, L"mode", 0, 17, mode);
        if (!valid_mode(static_cast<unsigned>(mode))) reader.good = false;
        Effects effects;
        parse_effects(reader, section, effects);
        rule.request = request_from_effects(static_cast<Mode>(mode), effects);
        parse_request_overrides(reader, section, rule.request);
        int rule_affinity = static_cast<int>(rule.request.affinity);
        reader.optional_number(section, L"affinity", rule_affinity, 0, 17, rule_affinity);
        if (!valid_affinity(static_cast<unsigned>(rule_affinity))) reader.good = false;
        rule.request.affinity = static_cast<AffinityMode>(rule_affinity);
        int rule_enabled = 1;
        reader.optional_number(section, L"enabled", 1, 0, 1, rule_enabled);
        rule.enabled = rule_enabled != 0;
        int rule_audience = static_cast<int>(candidate.defaults.visible.audience);
        reader.optional_number(section, L"visible_audience", rule_audience, 0, 1, rule_audience);
        rule.request.visible.audience = static_cast<WatermarkAudience>(rule_audience);
        int rule_taskbar = static_cast<int>(candidate.defaults.taskbar);
        reader.optional_number(section, L"taskbar", rule_taskbar, 0, 1, rule_taskbar);
        rule.request.taskbar = static_cast<TaskbarIntent>(rule_taskbar);
        reader.optional_number(section, L"capture_route", 0, 0, 2, capture);
        reader.optional_number(section, L"flags", 0, 0, 0, flags);
        rule.request.capture = static_cast<CaptureRoutePreference>(capture);
        rule.request.extension_flags = static_cast<std::uint32_t>(flags);
        rule.request.hidden = candidate.defaults.hidden;
        parse_hidden(reader, section, rule.request.hidden, true);
        reader.required_number(section, L"x", -1000000, 1000000, rule.x);
        reader.required_number(section, L"y", -1000000, 1000000, rule.y);
        reader.required_number(section, L"width", 1, 65535, rule.width);
        reader.required_number(section, L"height", 1, 65535, rule.height);
        reader.required_number(section, L"show", 1, 3, rule.show);
        candidate.rules.push_back(std::move(rule));
    }
    if (!reader.good) {
        result.status = ProfileLoadStatus::invalid_data;
        return result;
    }
    DWORD validation = 0;
    if (!valid_profile_v4(candidate, validation)) {
        result.status = ProfileLoadStatus::invalid_data;
        result.error = validation;
        return result;
    }
    profile = std::move(candidate);
    result.status = ProfileLoadStatus::ok;
    result.error = ERROR_SUCCESS;
    result.source_schema = 4;
    return result;
}

bool save_profile_v4(const std::wstring& file, const ProfileV4& profile, DWORD& error) {
    if (!valid_profile_v4(profile, error)) return false;
    const auto& preferences = profile.preferences;
    std::wstring out = L"\xFEFF[AkashaVeil]\r\nschema=4\r\nlanguage=" + profile.language
        + L"\r\ntheme=" + std::to_wstring(static_cast<int>(profile.theme))
        + L"\r\naccent=" + std::to_wstring(profile.accent)
        + L"\r\ncount=" + std::to_wstring(profile.rules.size()) + L"\r\n";
    out += L"\r\n[Preferences]\r\n";
    for (const auto& setting : bool_preferences)
        out += std::wstring(setting.key) + L"=" + std::to_wstring(preferences.*(setting.member)) + L"\r\n";
    out += L"refresh_ms=" + std::to_wstring(preferences.refresh_ms)
        + L"\r\nwindow_wait_ms=" + std::to_wstring(preferences.window_wait_ms)
        + L"\r\nauto_mode=" + std::to_wstring(static_cast<unsigned>(preferences.auto_mode))
        + L"\r\nprogram_count=" + std::to_wstring(preferences.programs.size()) + L"\r\n";
    for (std::size_t i = 0; i < preferences.programs.size(); ++i)
        out += L"program_" + std::to_wstring(i) + L"=" + encode_text(preferences.programs[i]) + L"\r\n";
    out += L"\r\n[Protection]\r\naffinity="
        + std::to_wstring(static_cast<unsigned>(profile.defaults.affinity)) + L"\r\n";
    write_request_fields(out, profile.defaults);
    write_protection_fields(out, profile.defaults);
    const auto default_effects = effects_from_request(profile.defaults);
    out += L"\r\n[Effects]\r\nmode="
        + std::to_wstring(static_cast<unsigned>(profile.defaults.affinity)) + L"\r\n";
    write_effects(out, default_effects);
    for (std::size_t i = 0; i < profile.rules.size(); ++i) {
        const auto& rule = profile.rules[i];
        out += L"\r\n[Window." + std::to_wstring(i) + L"]\r\npath=" + encode_text(rule.path)
            + L"\r\nclass=" + encode_text(rule.class_name)
            + L"\r\ntitle=" + encode_text(rule.title)
            + L"\r\nmode=" + std::to_wstring(static_cast<unsigned>(rule.request.affinity))
            + L"\r\nenabled=" + std::to_wstring(rule.enabled)
            + L"\r\nx=" + std::to_wstring(rule.x)
            + L"\r\ny=" + std::to_wstring(rule.y)
            + L"\r\nwidth=" + std::to_wstring(rule.width)
            + L"\r\nheight=" + std::to_wstring(rule.height)
            + L"\r\nshow=" + std::to_wstring(rule.show) + L"\r\n";
        const auto effects = effects_from_request(rule.request);
        write_effects(out, effects);
        out += L"affinity=" + std::to_wstring(static_cast<unsigned>(rule.request.affinity)) + L"\r\n";
        write_request_fields(out, rule.request);
        write_protection_fields(out, rule.request);
    }
    return atomic_write(file, std::string(reinterpret_cast<const char*>(out.data()),
        out.size() * sizeof(wchar_t)), error);
}

}

