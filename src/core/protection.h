// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/identity.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace av {

enum class AffinityMode : unsigned {
    none = 0,
    monitor = 1,
    exclude_from_capture = 0x11
};

bool valid_affinity(unsigned value) noexcept;
AffinityMode affinity_from_unsigned(unsigned value) noexcept;

enum class ReplacementKind : unsigned {
    none = 0,
    solid = 1,
    label = 2
};

enum class MarkLayout : unsigned {
    none = 0,
    tiled = 1,
    center = 2,
    corner = 3
};

enum class WatermarkAudience : unsigned {
    screen_and_capture = 0,
    capture_only = 1
};

enum class TaskbarIntent : unsigned {
    leave_unchanged = 0,
    hide = 1
};

enum class CaptureRoutePreference : unsigned {
    automatic = 0,
    display_capture_a = 1,
    proxy_source_b = 2
};

struct VisibleWatermark {
    bool enabled = false;
    MarkLayout layout = MarkLayout::none;
    WatermarkAudience audience = WatermarkAudience::screen_and_capture;
    std::wstring text = L"CONFIDENTIAL\n{id}  {time}";
    std::wstring label = L"Protected content";
    std::wstring tag;
    unsigned opacity = 45;
    unsigned font_size = 22;
    unsigned spacing = 240;
    std::uint32_t foreground = 0xE0E0E0;
    std::uint32_t background = 0x282828;
};

struct Replacement {
    ReplacementKind kind = ReplacementKind::none;
    std::wstring label = L"Protected content";
    unsigned opacity = 45;
    std::uint32_t foreground = 0xE0E0E0;
    std::uint32_t background = 0x282828;
};

struct HiddenWatermark {
    std::uint16_t algorithm = 0;
    std::uint16_t flags = 0;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> parameters;
    std::vector<std::uint8_t> key;
    bool enabled() const noexcept { return algorithm != 0 && !payload.empty(); }
};

struct ProtectionRequest {
    AffinityMode affinity = AffinityMode::none;
    Replacement replacement;
    VisibleWatermark visible;
    HiddenWatermark hidden;
    TaskbarIntent taskbar = TaskbarIntent::leave_unchanged;
    CaptureRoutePreference capture = CaptureRoutePreference::automatic;
    std::uint32_t extension_flags = 0;
};

enum class RequestIssue : std::uint32_t {
    none = 0,
    affinity = 1u << 0,
    replacement = 1u << 1,
    visible = 1u << 2,
    hidden = 1u << 3,
    taskbar = 1u << 4,
    capture = 1u << 5,
    extension = 1u << 6
};

using RequestIssues = std::uint32_t;

RequestIssues protection_request_issues(const ProtectionRequest& request) noexcept;
bool valid_protection_request(const ProtectionRequest& request) noexcept;
bool has_replacement(const ProtectionRequest& request) noexcept;
bool has_visible(const ProtectionRequest& request) noexcept;
bool has_hidden(const ProtectionRequest& request) noexcept;
AffinityMode required_affinity(const ProtectionRequest& request) noexcept;

enum class Stage : unsigned {
    affinity = 0,
    replacement = 1,
    visible_watermark = 2,
    hidden_watermark = 3,
    taskbar = 4
};

constexpr std::size_t stage_count = 5;
using StageMask = std::uint8_t;
constexpr StageMask stage_bit(Stage stage) noexcept {
    return static_cast<StageMask>(1u << static_cast<unsigned>(stage));
}
constexpr StageMask all_stages =
    static_cast<StageMask>((1u << stage_count) - 1u);

StageMask requested_stages(const ProtectionRequest& request) noexcept;
const wchar_t* stage_name(Stage stage) noexcept;

enum class StageState : unsigned {
    not_requested = 0,
    pending = 1,
    succeeded = 2,
    failed = 3,
    unsupported = 4,
    skipped = 5,
    cancelled = 6
};

enum class ErrorDomain : std::uint32_t {
    none = 0,
    validation = 1,
    win32 = 2,
    unsupported = 3,
    stale_identity = 4,
    cancelled = 5,
    budget = 6,
    internal = 7
};

struct Error {
    ErrorDomain domain = ErrorDomain::none;
    std::uint32_t code = 0;
    bool ok() const noexcept { return domain == ErrorDomain::none; }
};

bool retryable(const Error& error) noexcept;
const wchar_t* error_domain_name(ErrorDomain domain) noexcept;

struct StageDiagnostic {
    Stage stage = Stage::affinity;
    StageState state = StageState::not_requested;
    Error error;
};

struct ProtectionObserved {
    TargetIdentity target;
    ProtectionRequest request;
    AffinityMode required_affinity = AffinityMode::none;
    AffinityMode actual_affinity = AffinityMode::none;
    bool affinity_known = false;
    bool stale = false;
    std::array<StageDiagnostic, stage_count> stages{};

    bool was_requested(Stage stage) const noexcept;
    bool succeeded(Stage stage) const noexcept;
    bool failed(Stage stage) const noexcept;
    bool all_requested_succeeded() const noexcept;
    bool partial_success() const noexcept;
    StageMask failed_stages() const noexcept;
    StageMask retryable_stages() const noexcept;
};

ProtectionObserved make_observed(const TargetIdentity& target,
    const ProtectionRequest& request);
void set_stage(ProtectionObserved& observed, Stage stage, StageState state,
    Error error = {});

struct BackendCapabilities {
    bool affinity_basic = false;
    bool affinity_exclude = false;
    bool replacement_solid = false;
    bool replacement_label = false;
    bool visible_screen = false;
    bool visible_capture_only_original_preserved = false;
    bool taskbar_target = false;
    bool capture_route_automatic = false;
    std::vector<std::uint16_t> hidden_algorithms;

    bool supports_hidden(std::uint16_t algorithm) const noexcept;
};

struct PlanIssue {
    Stage stage = Stage::affinity;
    Error error;
};

struct ProtectionPlan {
    AffinityMode affinity = AffinityMode::none;
    StageMask stages = 0;
    std::vector<PlanIssue> issues;

    bool viable() const noexcept { return issues.empty(); }
    bool has_issue(Stage stage) const noexcept;
};

ProtectionPlan plan_protection(const ProtectionRequest& request,
    const BackendCapabilities& capabilities);

}

