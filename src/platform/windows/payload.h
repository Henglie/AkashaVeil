// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <string>

namespace av {
// Runtime policy. The random-name switch is deliberately independent from the
// content-addressed cache directory: the directory identifies payload bytes,
// while the file name is a session-generated random name.
struct PayloadPolicy {
    bool randomize_names = true;
    unsigned stale_days = 7;
    unsigned cleanup_items = 64;
};
PayloadPolicy payload_policy();
void set_payload_policy(const PayloadPolicy& policy);

struct Payload {
    std::wstring directory, worker_path, hook_path;
    HANDLE worker = INVALID_HANDLE_VALUE, hook = INVALID_HANDLE_VALUE;
    Payload() = default;
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    void release() {
        if (worker != INVALID_HANDLE_VALUE) { CloseHandle(worker); worker = INVALID_HANDLE_VALUE; }
        if (hook != INVALID_HANDLE_VALUE) { CloseHandle(hook); hook = INVALID_HANDLE_VALUE; }
    }
    ~Payload() { release(); }
};

bool prepare_payload(const std::wstring& architecture, bool need_worker, Payload& payload, DWORD& error);

// Session lifecycle: names are stable inside one process and are regenerated
// only through this function (used when the random-name policy changes).
void regenerate_payload_session();

// Recycle-bin cleanup of files this process family generated. Unknown files are
// never deleted. If the shell cannot recycle an item, it is retained and
// reported as failed instead of being hard-deleted.
struct PayloadCleanupReport {
    unsigned scanned = 0;
    unsigned recycled = 0;
    unsigned retained = 0;
    unsigned failed = 0;
    DWORD last_error = 0;
};
PayloadCleanupReport cleanup_payload_cache(const std::wstring& architecture, unsigned max_items = 64);
// Explicit session-end cleanup: recycle only the random files materialized by
// this process, never fixed legacy names or unknown files. Locked files are
// retained and reported; abnormal exits are covered by the stale sweep.
PayloadCleanupReport cleanup_session_payloads();
}
