// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/profile_v4.h"
#include <cstddef>
#include <functional>
#include <string>

namespace av {

enum class ProfileSaveStage : unsigned {
    none = 0,
    validate = 1,
    resolve = 2,
    staging = 3,
    replace = 4,
    verify = 5,
    cleanup = 6,
    completed = 7
};

struct ProfileSaveOutcome {
    bool ok = false;
    DWORD error = ERROR_SUCCESS;
    ProfileSaveStage stage = ProfileSaveStage::none;
    bool backup_created = false;
    bool recovered = false;
    bool staging_orphan = false;
    std::wstring staging_path;
    std::wstring backup_path;
};

enum class ProfileFaultAction : unsigned {
    proceed = 0,
    fail = 1,
    abort_keep = 2
};

using ProfileSaveHook = std::function<ProfileFaultAction(ProfileSaveStage stage,
    const std::wstring& path, DWORD& error)>;

// Save through a same-directory staging file, replace with backup, verify the
// persisted bytes and parseability, then delete the backup.  Any failure before
// replacement leaves the original file untouched; a verify failure restores the
// backup.  Hooks exist for deterministic fault injection in tests.
ProfileSaveOutcome save_profile_guarded(const std::wstring& file,
    const ProfileV4& profile, const ProfileSaveHook& hook = {});

std::size_t clean_profile_staging(const std::wstring& directory, DWORD& error);

class ProfileRepository {
public:
    ProfileRepository() = default;
    explicit ProfileRepository(std::wstring path);

    const ProfileV4& current() const noexcept { return current_; }
    const std::wstring& path() const noexcept { return path_; }
    int source_schema() const noexcept { return source_schema_; }
    const ProfileSaveOutcome& last_save() const noexcept { return last_save_; }
    DWORD last_error() const noexcept { return last_error_; }

    // Commit only after a complete, validated load.  Invalid/corrupt/large files
    // and unsupported schemas leave current() and the source file untouched.
    ProfileLoadResult load(const std::wstring& path);
    ProfileLoadResult reload();
    ProfileLoadResult load_default();

    ProfileSaveOutcome save(const ProfileSaveHook& hook = {});
    ProfileSaveOutcome save_as(const std::wstring& path,
        const ProfileSaveHook& hook = {});
    ProfileSaveOutcome save_candidate(const ProfileV4& candidate,
        const ProfileSaveHook& hook = {});

    void reset();
    std::size_t recover_staging(DWORD& error);

private:
    ProfileV4 current_;
    std::wstring path_;
    int source_schema_ = 4;
    ProfileSaveOutcome last_save_;
    DWORD last_error_ = ERROR_SUCCESS;
};

}
