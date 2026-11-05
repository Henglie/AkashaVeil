// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/job.h"
#include "core/protection.h"
#include <map>
#include <optional>
#include <vector>

namespace av {

class ProtectionBackend {
public:
    virtual ~ProtectionBackend() = default;

    virtual BackendCapabilities capabilities() const = 0;
    virtual ProtectionObserved apply(const TargetIdentity& target,
        const ProtectionRequest& request, StageMask stages,
        const CancellationToken& cancel) = 0;
    virtual ProtectionObserved restore(const TargetIdentity& target,
        const ProtectionRequest& request, StageMask stages,
        const CancellationToken& cancel) = 0;
    virtual std::vector<TargetIdentity> enumerate(const CancellationToken& cancel) = 0;
    virtual bool identity_current(const TargetIdentity& target) = 0;
};

class ProtectionController {
public:
    ProtectionController(ProtectionBackend& backend, JobBook& jobs);

    JobId protect(std::vector<TargetIdentity> targets,
        ProtectionRequest request, JobBudget budget = {});
    JobId restore_targets(std::vector<TargetIdentity> targets, JobBudget budget = {});
    JobId retry(JobId original, bool failed_only = true);
    bool cancel(JobId id);

    Acceptance complete(JobOutcome outcome);

    const ProtectionObserved* observed(const TargetIdentity& target) const;
    std::optional<JobContext> context(JobId id) const;
    std::optional<JobOutcome> last_outcome(JobId id) const;
    const std::vector<RejectedTarget>& rejected() const noexcept { return rejected_; }
    BackendCapabilities capabilities() const { return backend_.capabilities(); }

private:
    ProtectionBackend& backend_;
    JobBook& jobs_;
    std::map<TargetKey, ProtectionObserved> observed_;
    std::vector<RejectedTarget> rejected_;
};

}

