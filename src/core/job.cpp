// SPDX-License-Identifier: Apache-2.0
#include "core/job.h"
#include <algorithm>
#include <utility>

namespace av {

JobBook::Record* JobBook::find(JobId id) noexcept {
    const auto found = jobs_.find(id);
    return found == jobs_.end() ? nullptr : &found->second;
}

const JobBook::Record* JobBook::find(JobId id) const noexcept {
    const auto found = jobs_.find(id);
    return found == jobs_.end() ? nullptr : &found->second;
}

JobId JobBook::begin(JobKind kind, std::vector<RetryTarget> targets,
    ProtectionRequest request, JobBudget budget) {
    if (targets.empty() || !valid_protection_request(request)) return no_job;
    for (const auto& target : targets)
        if (!target.target.has_runtime_identity()) return no_job;
    Record record;
    record.view.id = next_id_++;
    record.view.kind = kind;
    record.view.generation = ++generation_;
    record.view.progress.total = static_cast<std::uint32_t>(targets.size());
    record.view.request = std::move(request);
    record.view.targets = std::move(targets);
    record.budget = budget;
    const auto id = record.view.id;
    for (const auto& target : record.view.targets)
        latest_[target_key(target.target)] = id;
    jobs_.emplace(id, std::move(record));
    return id;
}

JobId JobBook::begin(JobKind kind, std::vector<TargetIdentity> targets,
    ProtectionRequest request, JobBudget budget) {
    std::vector<RetryTarget> retry_targets;
    retry_targets.reserve(targets.size());
    const auto stages = requested_stages(request);
    for (auto& target : targets)
        retry_targets.push_back({std::move(target), stages});
    return begin(kind, std::move(retry_targets), std::move(request), budget);
}

std::optional<JobContext> JobBook::context(JobId id) const {
    const auto* record = find(id);
    if (!record) return std::nullopt;
    JobContext context;
    context.id = record->view.id;
    context.kind = record->view.kind;
    context.generation = record->view.generation;
    context.request = record->view.request;
    context.targets = record->view.targets;
    context.cancel = record->cancel.token();
    context.budget = record->budget;
    return context;
}

std::optional<JobRecordView> JobBook::record(JobId id) const {
    const auto* entry = find(id);
    if (!entry) return std::nullopt;
    return entry->view;
}

std::optional<JobOutcome> JobBook::outcome(JobId id) const {
    const auto found = outcomes_.find(id);
    if (found == outcomes_.end()) return std::nullopt;
    return found->second;
}

std::optional<Progress> JobBook::progress(JobId id) const {
    const auto* record = find(id);
    if (!record) return std::nullopt;
    return record->view.progress;
}

bool JobBook::update_progress(JobId id, Progress progress) {
    auto* record = find(id);
    if (!record || record->finished) return false;
    if (progress.total == 0) progress.total = record->view.progress.total;
    if (progress.completed > progress.total) progress.completed = progress.total;
    progress.cancelled = record->cancel.cancelled();
    if (record->view.progress.cancelled && !progress.cancelled) return false;
    record->view.progress = progress;
    return true;
}

std::optional<JobId> JobBook::latest_job(const TargetIdentity& target) const {
    const auto found = latest_.find(target_key(target));
    if (found == latest_.end()) return std::nullopt;
    return found->second;
}

bool JobBook::cancel(JobId id) {
    auto* record = find(id);
    if (!record || record->finished) return false;
    record->cancel.cancel();
    record->view.progress.cancelled = true;
    return true;
}

bool JobBook::is_cancelled(JobId id) const {
    const auto* record = find(id);
    return record && record->cancel.cancelled();
}

bool JobBook::finished(JobId id) const {
    const auto* record = find(id);
    return record && record->finished;
}

static void reject_all(Acceptance& acceptance, const JobOutcome& outcome,
    RejectionReason reason) {
    for (const auto& target : outcome.targets)
        acceptance.rejected.push_back({target.target, reason});
}

Acceptance JobBook::accept(JobOutcome outcome) {
    Acceptance acceptance;
    auto* record = find(outcome.id);
    if (!record) {
        reject_all(acceptance, outcome, RejectionReason::unknown_job);
        return acceptance;
    }
    if (record->finished) {
        reject_all(acceptance, outcome, RejectionReason::stale_job);
        return acceptance;
    }
    if (outcome.state != JobState::completed && outcome.state != JobState::failed
        && outcome.state != JobState::cancelled) {
        reject_all(acceptance, outcome, RejectionReason::incomplete_job);
        return acceptance;
    }

    const bool cancelled = record->cancel.cancelled()
        || outcome.state == JobState::cancelled;
    acceptance.cancelled = cancelled;
    for (const auto& observed : outcome.targets) {
        const auto owned = std::any_of(record->view.targets.begin(), record->view.targets.end(),
            [&observed](const RetryTarget& target) {
                return target.target.same_runtime(observed.target);
            });
        if (!owned) {
            acceptance.rejected.push_back({observed.target, RejectionReason::unknown_target});
            continue;
        }
        if (observed.stale) {
            acceptance.rejected.push_back({observed.target, RejectionReason::stale_job});
            continue;
        }
        if (observed.target.has_runtime_identity()) {
            const auto latest = latest_.find(target_key(observed.target));
            if (latest == latest_.end() || latest->second != outcome.id) {
                acceptance.rejected.push_back({observed.target, RejectionReason::stale_job});
                continue;
            }
        }
        // A cancelled job still preserves per-target actual state.  The caller
        // can distinguish "cancelled with partial effect" from "clean cancel"
        // through Acceptance::cancelled plus the observed stage table.
        acceptance.accepted.push_back(observed);
    }

    if (!cancelled) {
        for (const auto& target : record->view.targets) {
            const auto seen = std::any_of(outcome.targets.begin(), outcome.targets.end(),
                [&target](const ProtectionObserved& observed) {
                    return observed.target.same_runtime(target.target);
                });
            if (!seen)
                acceptance.rejected.push_back({target.target, RejectionReason::missing_target});
        }
    }

    record->finished = true;
    if (outcome.progress.total == 0)
        outcome.progress = record->view.progress;
    outcome.progress.cancelled = cancelled;
    if (cancelled) outcome.state = JobState::cancelled;
    outcomes_[outcome.id] = std::move(outcome);
    return acceptance;
}

}

