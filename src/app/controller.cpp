// SPDX-License-Identifier: Apache-2.0
#include "app/controller.h"
#include <algorithm>

namespace av {

ProtectionController::ProtectionController(ProtectionBackend& backend, JobBook& jobs)
    : backend_(backend), jobs_(jobs) {}

JobId ProtectionController::protect(std::vector<TargetIdentity> targets,
    ProtectionRequest request, JobBudget budget) {
    return jobs_.begin(JobKind::protect, std::move(targets), std::move(request), budget);
}

JobId ProtectionController::restore_targets(std::vector<TargetIdentity> targets,
    JobBudget budget) {
    std::vector<RetryTarget> retry_targets;
    retry_targets.reserve(targets.size());
    ProtectionRequest request;
    for (const auto& target : targets) {
        const auto found = observed_.find(target_key(target));
        if (found != observed_.end()) {
            request = found->second.request;
            break;
        }
    }
    for (auto& target : targets)
        retry_targets.push_back({std::move(target), all_stages});
    return jobs_.begin(JobKind::restore, std::move(retry_targets), request, budget);
}

JobId ProtectionController::retry(JobId original, bool failed_only) {
    const auto record = jobs_.record(original);
    const auto outcome = jobs_.outcome(original);
    if (!record || !outcome) return no_job;
    std::vector<RetryTarget> targets;
    for (const auto& target : record->targets) {
        StageMask stages = target.stages;
        if (failed_only) {
            stages = 0;
            for (const auto& observed : outcome->targets) {
                if (observed.target.same_runtime(target.target)) {
                    stages = observed.retryable_stages();
                    break;
                }
            }
        }
        if (stages) targets.push_back({target.target, stages});
    }
    if (targets.empty()) return no_job;
    return jobs_.begin(record->kind, std::move(targets), record->request);
}

bool ProtectionController::cancel(JobId id) {
    return jobs_.cancel(id);
}

Acceptance ProtectionController::complete(JobOutcome outcome) {
    auto acceptance = jobs_.accept(std::move(outcome));
    for (const auto& observed : acceptance.accepted)
        observed_[target_key(observed.target)] = observed;
    rejected_.insert(rejected_.end(), acceptance.rejected.begin(), acceptance.rejected.end());
    return acceptance;
}

const ProtectionObserved* ProtectionController::observed(const TargetIdentity& target) const {
    const auto found = observed_.find(target_key(target));
    return found == observed_.end() ? nullptr : &found->second;
}

std::optional<JobContext> ProtectionController::context(JobId id) const {
    return jobs_.context(id);
}

std::optional<JobOutcome> ProtectionController::last_outcome(JobId id) const {
    return jobs_.outcome(id);
}

}

