// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app/controller.h"
#include "core/lifecycle.h"
#include "platform/windows/windows.h"

namespace av {

class WindowsSessionBackend : public ProtectionBackend {
public:
    explicit WindowsSessionBackend(SessionLedger& ledger);

    BackendCapabilities capabilities() const override;
    ProtectionObserved apply(const TargetIdentity& target,
        const ProtectionRequest& request, StageMask stages,
        const CancellationToken& cancel) override;
    ProtectionObserved restore(const TargetIdentity& target,
        const ProtectionRequest& request, StageMask stages,
        const CancellationToken& cancel) override;
    std::vector<TargetIdentity> enumerate(const CancellationToken& cancel) override;
    bool identity_current(const TargetIdentity& target) override;

    bool restore_ledger(DWORD& error);

private:
    SessionLedger& ledger_;
};

}

