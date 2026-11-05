// SPDX-License-Identifier: Apache-2.0
#include "core/protection.h"
#include <algorithm>

namespace av {

bool valid_affinity(unsigned value) noexcept {
    return value == 0 || value == 1 || value == 0x11;
}

AffinityMode affinity_from_unsigned(unsigned value) noexcept {
    if (!valid_affinity(value)) return AffinityMode::none;
    return static_cast<AffinityMode>(value);
}

static bool text_ok(const std::wstring& value, std::size_t limit) noexcept {
    return value.size() <= limit && value.find(L'\0') == std::wstring::npos;
}

static bool non_blank(const std::wstring& value) noexcept {
    return value.find_first_not_of(L" \r\n\t") != std::wstring::npos;
}

static bool color_ok(std::uint32_t value) noexcept {
    return value <= 0xFFFFFFu;
}

RequestIssues protection_request_issues(const ProtectionRequest& request) noexcept {
    RequestIssues issues = 0;
    if (!valid_affinity(static_cast<unsigned>(request.affinity)))
        issues |= static_cast<RequestIssues>(RequestIssue::affinity);

    const auto replacement = static_cast<unsigned>(request.replacement.kind);
    if (replacement > 2
        || request.replacement.opacity < 5 || request.replacement.opacity > 100
        || !color_ok(request.replacement.foreground) || !color_ok(request.replacement.background)
        || !text_ok(request.replacement.label, 1024)
        || (request.replacement.kind == ReplacementKind::label && !non_blank(request.replacement.label)))
        issues |= static_cast<RequestIssues>(RequestIssue::replacement);

    const auto layout = static_cast<unsigned>(request.visible.layout);
    const auto audience = static_cast<unsigned>(request.visible.audience);
    if (layout > 3 || audience > 1
        || request.visible.opacity < 5 || request.visible.opacity > 100
        || request.visible.font_size < 10 || request.visible.font_size > 96
        || request.visible.spacing < 100 || request.visible.spacing > 600
        || !color_ok(request.visible.foreground) || !color_ok(request.visible.background)
        || !text_ok(request.visible.text, 1024) || !text_ok(request.visible.label, 1024)
        || !text_ok(request.visible.tag, 1024)
        || (request.visible.enabled && request.visible.layout == MarkLayout::none)
        || (request.visible.enabled && !non_blank(request.visible.text)))
        issues |= static_cast<RequestIssues>(RequestIssue::visible);

    if (request.hidden.payload.size() > 4096
        || request.hidden.parameters.size() > 2048
        || request.hidden.key.size() > 128
        || (request.hidden.algorithm != 0 && request.hidden.payload.empty()))
        issues |= static_cast<RequestIssues>(RequestIssue::hidden);

    if (static_cast<unsigned>(request.taskbar) > 1)
        issues |= static_cast<RequestIssues>(RequestIssue::taskbar);
    if (static_cast<unsigned>(request.capture) > 2)
        issues |= static_cast<RequestIssues>(RequestIssue::capture);
    if (request.extension_flags != 0)
        issues |= static_cast<RequestIssues>(RequestIssue::extension);
    return issues;
}

bool valid_protection_request(const ProtectionRequest& request) noexcept {
    return protection_request_issues(request) == 0;
}

bool has_replacement(const ProtectionRequest& request) noexcept {
    return request.replacement.kind != ReplacementKind::none;
}

bool has_visible(const ProtectionRequest& request) noexcept {
    return request.visible.enabled && request.visible.layout != MarkLayout::none;
}

bool has_hidden(const ProtectionRequest& request) noexcept {
    return request.hidden.enabled();
}

AffinityMode required_affinity(const ProtectionRequest& request) noexcept {
    if (has_replacement(request)) return AffinityMode::exclude_from_capture;
    return request.affinity;
}

StageMask requested_stages(const ProtectionRequest& request) noexcept {
    StageMask mask = 0;
    if (required_affinity(request) != AffinityMode::none)
        mask |= stage_bit(Stage::affinity);
    if (has_replacement(request)) mask |= stage_bit(Stage::replacement);
    if (has_visible(request)) mask |= stage_bit(Stage::visible_watermark);
    if (has_hidden(request)) mask |= stage_bit(Stage::hidden_watermark);
    if (request.taskbar != TaskbarIntent::leave_unchanged) mask |= stage_bit(Stage::taskbar);
    return mask;
}

const wchar_t* stage_name(Stage stage) noexcept {
    switch (stage) {
    case Stage::affinity: return L"affinity";
    case Stage::replacement: return L"replacement";
    case Stage::visible_watermark: return L"visible_watermark";
    case Stage::hidden_watermark: return L"hidden_watermark";
    case Stage::taskbar: return L"taskbar";
    }
    return L"unknown";
}

bool retryable(const Error& error) noexcept {
    switch (error.domain) {
    case ErrorDomain::win32:
    case ErrorDomain::stale_identity:
    case ErrorDomain::budget:
    case ErrorDomain::internal:
        return true;
    case ErrorDomain::none:
    case ErrorDomain::validation:
    case ErrorDomain::unsupported:
    case ErrorDomain::cancelled:
        return false;
    }
    return false;
}

const wchar_t* error_domain_name(ErrorDomain domain) noexcept {
    switch (domain) {
    case ErrorDomain::none: return L"none";
    case ErrorDomain::validation: return L"validation";
    case ErrorDomain::win32: return L"win32";
    case ErrorDomain::unsupported: return L"unsupported";
    case ErrorDomain::stale_identity: return L"stale_identity";
    case ErrorDomain::cancelled: return L"cancelled";
    case ErrorDomain::budget: return L"budget";
    case ErrorDomain::internal: return L"internal";
    }
    return L"internal";
}

static const StageDiagnostic& diagnostic(const ProtectionObserved& observed, Stage stage) noexcept {
    return observed.stages[static_cast<std::size_t>(stage)];
}

bool ProtectionObserved::was_requested(Stage stage) const noexcept {
    return (requested_stages(request) & stage_bit(stage)) != 0;
}

bool ProtectionObserved::succeeded(Stage stage) const noexcept {
    return diagnostic(*this, stage).state == StageState::succeeded;
}

bool ProtectionObserved::failed(Stage stage) const noexcept {
    const auto state = diagnostic(*this, stage).state;
    return state == StageState::failed || state == StageState::unsupported
        || state == StageState::cancelled;
}

bool ProtectionObserved::all_requested_succeeded() const noexcept {
    const auto mask = requested_stages(request);
    for (std::size_t i = 0; i < stage_count; ++i) {
        if (!(mask & stage_bit(static_cast<Stage>(i)))) continue;
        const auto state = stages[i].state;
        if (state != StageState::succeeded && state != StageState::skipped) return false;
    }
    return true;
}

bool ProtectionObserved::partial_success() const noexcept {
    const auto mask = requested_stages(request);
    bool any_success = false;
    bool any_problem = false;
    for (std::size_t i = 0; i < stage_count; ++i) {
        if (!(mask & stage_bit(static_cast<Stage>(i)))) continue;
        const auto state = stages[i].state;
        if (state == StageState::succeeded) any_success = true;
        else if (state != StageState::skipped) any_problem = true;
    }
    return any_success && any_problem;
}

StageMask ProtectionObserved::failed_stages() const noexcept {
    StageMask mask = 0;
    for (std::size_t i = 0; i < stage_count; ++i) {
        const auto stage = static_cast<Stage>(i);
        if (!was_requested(stage)) continue;
        const auto state = stages[i].state;
        if (state == StageState::failed || state == StageState::unsupported
            || state == StageState::cancelled)
            mask |= stage_bit(stage);
    }
    return mask;
}

StageMask ProtectionObserved::retryable_stages() const noexcept {
    StageMask mask = 0;
    for (std::size_t i = 0; i < stage_count; ++i) {
        const auto stage = static_cast<Stage>(i);
        if (!was_requested(stage)) continue;
        if (retryable(stages[i].error)) mask |= stage_bit(stage);
    }
    return mask;
}

ProtectionObserved make_observed(const TargetIdentity& target,
    const ProtectionRequest& request) {
    ProtectionObserved observed;
    observed.target = target;
    observed.request = request;
    observed.required_affinity = required_affinity(request);
    for (std::size_t i = 0; i < stage_count; ++i) {
        observed.stages[i].stage = static_cast<Stage>(i);
        observed.stages[i].state = StageState::not_requested;
    }
    return observed;
}

void set_stage(ProtectionObserved& observed, Stage stage, StageState state,
    Error error) {
    auto& slot = observed.stages[static_cast<std::size_t>(stage)];
    slot.stage = stage;
    slot.state = state;
    slot.error = error;
}

bool BackendCapabilities::supports_hidden(std::uint16_t algorithm) const noexcept {
    if (!algorithm) return true;
    return std::find(hidden_algorithms.begin(), hidden_algorithms.end(), algorithm)
        != hidden_algorithms.end();
}

bool ProtectionPlan::has_issue(Stage stage) const noexcept {
    return std::any_of(issues.begin(), issues.end(), [stage](const PlanIssue& issue) {
        return issue.stage == stage;
    });
}

static void add_issue(ProtectionPlan& plan, Stage stage, ErrorDomain domain,
    std::uint32_t code) {
    plan.issues.push_back({stage, {domain, code}});
}

ProtectionPlan plan_protection(const ProtectionRequest& request,
    const BackendCapabilities& capabilities) {
    ProtectionPlan plan;
    plan.affinity = required_affinity(request);
    plan.stages = requested_stages(request);

    const auto mask = plan.stages;
    if (mask & stage_bit(Stage::affinity)) {
        if (!capabilities.affinity_basic)
            add_issue(plan, Stage::affinity, ErrorDomain::unsupported, 1);
        else if (plan.affinity == AffinityMode::exclude_from_capture
            && !capabilities.affinity_exclude)
            add_issue(plan, Stage::affinity, ErrorDomain::unsupported, 2);
    }
    if (mask & stage_bit(Stage::replacement)) {
        const bool supported = request.replacement.kind == ReplacementKind::solid
            ? capabilities.replacement_solid
            : request.replacement.kind == ReplacementKind::label
                ? capabilities.replacement_label : true;
        if (!supported)
            add_issue(plan, Stage::replacement, ErrorDomain::unsupported, 3);
        if (!capabilities.affinity_exclude)
            add_issue(plan, Stage::affinity, ErrorDomain::unsupported, 4);
    }
    if (mask & stage_bit(Stage::visible_watermark)) {
        if (request.visible.audience == WatermarkAudience::capture_only) {
            if (!capabilities.visible_capture_only_original_preserved)
                add_issue(plan, Stage::visible_watermark, ErrorDomain::unsupported, 5);
        } else if (!capabilities.visible_screen) {
            add_issue(plan, Stage::visible_watermark, ErrorDomain::unsupported, 6);
        }
    }
    if (mask & stage_bit(Stage::hidden_watermark)) {
        if (!capabilities.supports_hidden(request.hidden.algorithm))
            add_issue(plan, Stage::hidden_watermark, ErrorDomain::unsupported, 7);
    }
    if (mask & stage_bit(Stage::taskbar)) {
        if (!capabilities.taskbar_target)
            add_issue(plan, Stage::taskbar, ErrorDomain::unsupported, 8);
    }
    const bool needs_capture_route = has_replacement(request)
        || (has_visible(request) && request.visible.audience == WatermarkAudience::capture_only);
    if (needs_capture_route && !capabilities.capture_route_automatic)
        add_issue(plan, Stage::replacement, ErrorDomain::unsupported, 9);
    return plan;
}

}

