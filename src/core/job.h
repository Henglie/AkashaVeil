// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/protection.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace av {

using JobId = std::uint64_t;
constexpr JobId no_job = 0;

enum class JobKind : unsigned {
    refresh = 0,
    protect = 1,
    restore = 2,
    apply_rules = 3,
    watermark_embed = 4,
    watermark_extract = 5
};

enum class JobState : unsigned {
    queued = 0,
    running = 1,
    cancelling = 2,
    completed = 3,
    failed = 4,
    cancelled = 5
};

struct CancellationToken {
    std::shared_ptr<const std::atomic_bool> flag;
    bool cancelled() const noexcept { return flag && flag->load(std::memory_order_acquire); }
};

class CancellationSource {
public:
    CancellationSource() : flag_(std::make_shared<std::atomic_bool>(false)) {}
    CancellationToken token() const noexcept { return {flag_}; }
    void cancel() noexcept { flag_->store(true, std::memory_order_release); }
    bool cancelled() const noexcept { return flag_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic_bool> flag_;
};

struct JobBudget {
    std::uint64_t max_pixels = 0;
    std::uint64_t max_bytes = 0;
    unsigned max_concurrency = 1;
};

struct Progress {
    Stage stage = Stage::affinity;
    std::uint32_t completed = 0;
    std::uint32_t total = 0;
    bool cancelled = false;
};

struct JobError {
    Error error;
    std::wstring detail;
};

struct RetryTarget {
    TargetIdentity target;
    StageMask stages = 0;
};

struct JobContext {
    JobId id = no_job;
    JobKind kind = JobKind::protect;
    std::uint64_t generation = 0;
    ProtectionRequest request;
    std::vector<RetryTarget> targets;
    CancellationToken cancel;
    JobBudget budget;
};

struct JobOutcome {
    JobId id = no_job;
    JobKind kind = JobKind::protect;
    JobState state = JobState::queued;
    std::uint64_t generation = 0;
    Progress progress;
    std::vector<ProtectionObserved> targets;
    std::vector<JobError> errors;
};

struct JobRecordView {
    JobId id = no_job;
    JobKind kind = JobKind::protect;
    std::uint64_t generation = 0;
    Progress progress;
    ProtectionRequest request;
    std::vector<RetryTarget> targets;
};

enum class RejectionReason : unsigned {
    unknown_job = 0,
    stale_job = 1,
    cancelled_job = 2,
    incomplete_job = 3,
    unknown_target = 4,
    missing_target = 5
};

struct RejectedTarget {
    TargetIdentity target;
    RejectionReason reason = RejectionReason::unknown_job;
};

struct Acceptance {
    std::vector<ProtectionObserved> accepted;
    std::vector<RejectedTarget> rejected;
    bool cancelled = false;

    bool any_accepted() const noexcept { return !accepted.empty(); }
};

class JobBook {
public:
    JobBook() = default;
    JobBook(const JobBook&) = delete;
    JobBook& operator=(const JobBook&) = delete;

    JobId begin(JobKind kind, std::vector<RetryTarget> targets,
        ProtectionRequest request, JobBudget budget = {});
    JobId begin(JobKind kind, std::vector<TargetIdentity> targets,
        ProtectionRequest request, JobBudget budget = {});

    std::optional<JobContext> context(JobId id) const;
    std::optional<JobRecordView> record(JobId id) const;
    std::optional<JobOutcome> outcome(JobId id) const;
    std::optional<Progress> progress(JobId id) const;
    std::optional<JobId> latest_job(const TargetIdentity& target) const;

    bool update_progress(JobId id, Progress progress);
    bool cancel(JobId id);
    bool is_cancelled(JobId id) const;
    bool finished(JobId id) const;

    Acceptance accept(JobOutcome outcome);

private:
    struct Record {
        JobRecordView view;
        CancellationSource cancel;
        JobBudget budget;
        bool finished = false;
    };

    Record* find(JobId id) noexcept;
    const Record* find(JobId id) const noexcept;

    JobId next_id_ = 1;
    std::uint64_t generation_ = 0;
    std::map<JobId, Record> jobs_;
    std::map<JobId, JobOutcome> outcomes_;
    std::map<TargetKey, JobId> latest_;
};

}

