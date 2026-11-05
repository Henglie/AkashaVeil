// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/profile_repository.h"
#include "platform/windows/profile_paths.h"
#include <utility>
#include <vector>

namespace av {

namespace {

constexpr std::size_t max_verify_file = 16 * 1024 * 1024;

bool read_bytes(const std::wstring& file, std::vector<unsigned char>& data) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool sized = GetFileSizeEx(handle, &size) && size.QuadPart >= 0
        && static_cast<unsigned long long>(size.QuadPart) <= max_verify_file;
    if (!sized) {
        CloseHandle(handle);
        return false;
    }
    data.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = data.empty() || (ReadFile(handle, data.data(),
        static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size());
    CloseHandle(handle);
    if (!ok) data.clear();
    return ok;
}

bool make_unique(const std::wstring& parent, const wchar_t* suffix,
    std::wstring& out) {
    static unsigned counter = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        out = parent + L"\\AkashaVeil-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()) + L"-"
            + std::to_wstring(++counter) + suffix;
        if (GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES
            && GetLastError() == ERROR_FILE_NOT_FOUND)
            return true;
    }
    out.clear();
    return false;
}

bool restore_backup(const std::wstring& target, const std::wstring& backup,
    DWORD& error) {
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = ERROR_FILE_NOT_FOUND;
        return false;
    }
    if (ReplaceFileW(target.c_str(), backup.c_str(), nullptr,
        REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        error = ERROR_SUCCESS;
        return true;
    }
    if (MoveFileExW(backup.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    return false;
}

bool verify_target(const std::wstring& target,
    const std::vector<unsigned char>& expected, DWORD& error) {
    std::vector<unsigned char> actual;
    if (!read_bytes(target, actual)) {
        error = GetLastError();
        if (!error) error = ERROR_READ_FAULT;
        return false;
    }
    if (actual != expected) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    ProfileV4 parsed;
    const auto loaded = load_profile_v4(target, parsed);
    if (!loaded.ok()) {
        error = loaded.error ? loaded.error : ERROR_INVALID_DATA;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

ProfileSaveOutcome failure(ProfileSaveOutcome outcome, ProfileSaveStage stage,
    DWORD error) {
    outcome.ok = false;
    outcome.stage = stage;
    outcome.error = error ? error : ERROR_CANCELLED;
    return outcome;
}

}

ProfileSaveOutcome save_profile_guarded(const std::wstring& file,
    const ProfileV4& profile, const ProfileSaveHook& hook) {
    ProfileSaveOutcome outcome;
    DWORD error = ERROR_SUCCESS;
    if (!valid_profile_v4(profile, error))
        return failure(outcome, ProfileSaveStage::validate, error);

    wchar_t absolute[32768]{};
    const DWORD count = GetFullPathNameW(file.c_str(), 32768, absolute, nullptr);
    if (!count || count >= 32768)
        return failure(outcome, ProfileSaveStage::resolve, GetLastError());
    const std::wstring target = absolute;
    const std::wstring parent = profile_parent_directory(target);
    const DWORD attributes = parent.empty() ? INVALID_FILE_ATTRIBUTES
        : GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return failure(outcome, ProfileSaveStage::resolve,
            parent.empty() ? ERROR_PATH_NOT_FOUND : GetLastError());
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return failure(outcome, ProfileSaveStage::resolve, ERROR_DIRECTORY);

    auto call_hook = [&](ProfileSaveStage stage, const std::wstring& path) {
        DWORD hook_error = ERROR_CANCELLED;
        if (!hook) return ProfileFaultAction::proceed;
        const auto action = hook(stage, path, hook_error);
        if (action != ProfileFaultAction::proceed) error = hook_error;
        return action;
    };

    std::wstring staging;
    if (!make_unique(parent, L".tmpav", staging))
        return failure(outcome, ProfileSaveStage::staging, ERROR_ALREADY_EXISTS);
    outcome.staging_path = staging;
    const auto before_write = call_hook(ProfileSaveStage::staging, staging);
    if (before_write != ProfileFaultAction::proceed) {
        if (before_write == ProfileFaultAction::abort_keep) {
            outcome.staging_orphan = true;
        } else {
            DeleteFileW(staging.c_str());
            outcome.staging_path.clear();
        }
        return failure(outcome, ProfileSaveStage::staging, error);
    }

    if (!save_profile_v4(staging, profile, error)) {
        DeleteFileW(staging.c_str());
        outcome.staging_path.clear();
        return failure(outcome, ProfileSaveStage::staging, error);
    }
    std::vector<unsigned char> staged;
    if (!read_bytes(staging, staged)) {
        DeleteFileW(staging.c_str());
        outcome.staging_path.clear();
        return failure(outcome, ProfileSaveStage::staging, GetLastError());
    }

    const auto before_replace = call_hook(ProfileSaveStage::replace, staging);
    if (before_replace != ProfileFaultAction::proceed) {
        if (before_replace == ProfileFaultAction::abort_keep) {
            outcome.staging_orphan = true;
        } else {
            DeleteFileW(staging.c_str());
            outcome.staging_path.clear();
        }
        return failure(outcome, ProfileSaveStage::replace, error);
    }

    const bool target_exists =
        GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!target_exists) {
        if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = GetLastError();
            DeleteFileW(staging.c_str());
            outcome.staging_path.clear();
            return failure(outcome, ProfileSaveStage::replace, error);
        }
        outcome.staging_path.clear();
        const auto before_verify = call_hook(ProfileSaveStage::verify, target);
        if (before_verify != ProfileFaultAction::proceed) {
            DeleteFileW(target.c_str());
            return failure(outcome, ProfileSaveStage::verify, error);
        }
        if (!verify_target(target, staged, error)) {
            DeleteFileW(target.c_str());
            return failure(outcome, ProfileSaveStage::verify, error);
        }
        outcome.ok = true;
        outcome.stage = ProfileSaveStage::completed;
        outcome.error = ERROR_SUCCESS;
        return outcome;
    }

    std::wstring backup;
    if (!make_unique(parent, L".bakav", backup)) {
        DeleteFileW(staging.c_str());
        outcome.staging_path.clear();
        return failure(outcome, ProfileSaveStage::replace, ERROR_ALREADY_EXISTS);
    }
    if (!ReplaceFileW(target.c_str(), staging.c_str(), backup.c_str(),
        REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        error = GetLastError();
        DeleteFileW(staging.c_str());
        DeleteFileW(backup.c_str());
        outcome.staging_path.clear();
        return failure(outcome, ProfileSaveStage::replace, error);
    }
    outcome.staging_path.clear();
    outcome.backup_created = true;
    outcome.backup_path = backup;

    const auto before_verify = call_hook(ProfileSaveStage::verify, target);
    if (before_verify != ProfileFaultAction::proceed) {
        DWORD restore_error = ERROR_SUCCESS;
        outcome.recovered = restore_backup(target, backup, restore_error);
        if (!outcome.recovered) {
            outcome.backup_path = backup;
            return failure(outcome, ProfileSaveStage::verify, restore_error);
        }
        outcome.backup_path.clear();
        return failure(outcome, ProfileSaveStage::verify, error);
    }

    if (!verify_target(target, staged, error)) {
        DWORD restore_error = ERROR_SUCCESS;
        outcome.recovered = restore_backup(target, backup, restore_error);
        if (!outcome.recovered) {
            outcome.backup_path = backup;
            return failure(outcome, ProfileSaveStage::verify, restore_error);
        }
        outcome.backup_path.clear();
        return failure(outcome, ProfileSaveStage::verify, error);
    }

    DeleteFileW(backup.c_str());
    outcome.backup_path.clear();
    outcome.ok = true;
    outcome.stage = ProfileSaveStage::completed;
    outcome.error = ERROR_SUCCESS;
    return outcome;
}

std::size_t clean_profile_staging(const std::wstring& directory, DWORD& error) {
    error = ERROR_SUCCESS;
    if (directory.empty()) {
        error = ERROR_INVALID_PARAMETER;
        return 0;
    }
    const std::wstring pattern = directory + L"\\AkashaVeil-*.tmpav";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD open_error = GetLastError();
        if (open_error == ERROR_FILE_NOT_FOUND || open_error == ERROR_PATH_NOT_FOUND)
            return 0;
        error = open_error;
        return 0;
    }
    std::size_t removed = 0;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring path = directory + L"\\" + data.cFileName;
        if (DeleteFileW(path.c_str())) ++removed;
        else if (error == ERROR_SUCCESS) error = GetLastError();
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return removed;
}

ProfileRepository::ProfileRepository(std::wstring path) : path_(std::move(path)) {}

ProfileLoadResult ProfileRepository::load(const std::wstring& path) {
    ProfileV4 candidate;
    const auto result = load_profile_v4(path, candidate);
    if (!result.ok()) {
        last_error_ = result.error;
        return result;
    }
    current_ = std::move(candidate);
    path_ = path;
    source_schema_ = result.source_schema;
    last_error_ = ERROR_SUCCESS;
    return result;
}

ProfileLoadResult ProfileRepository::reload() {
    if (path_.empty()) {
        ProfileLoadResult result;
        result.status = ProfileLoadStatus::not_found;
        result.error = ERROR_FILE_NOT_FOUND;
        last_error_ = result.error;
        return result;
    }
    return load(path_);
}

ProfileLoadResult ProfileRepository::load_default() {
    return reload();
}

ProfileSaveOutcome ProfileRepository::save(const ProfileSaveHook& hook) {
    if (path_.empty()) {
        last_save_ = failure(ProfileSaveOutcome{}, ProfileSaveStage::resolve,
            ERROR_PATH_NOT_FOUND);
        last_error_ = last_save_.error;
        return last_save_;
    }
    return save_candidate(current_, hook);
}

ProfileSaveOutcome ProfileRepository::save_as(const std::wstring& path,
    const ProfileSaveHook& hook) {
    if (path.empty()) {
        last_save_ = failure(ProfileSaveOutcome{}, ProfileSaveStage::resolve,
            ERROR_PATH_NOT_FOUND);
        last_error_ = last_save_.error;
        return last_save_;
    }
    last_save_ = save_profile_guarded(path, current_, hook);
    if (last_save_.ok) {
        path_ = path;
        source_schema_ = 4;
        last_error_ = ERROR_SUCCESS;
    } else {
        last_error_ = last_save_.error;
    }
    return last_save_;
}

ProfileSaveOutcome ProfileRepository::save_candidate(const ProfileV4& candidate,
    const ProfileSaveHook& hook) {
    if (path_.empty()) {
        last_save_ = failure(ProfileSaveOutcome{}, ProfileSaveStage::resolve,
            ERROR_PATH_NOT_FOUND);
        last_error_ = last_save_.error;
        return last_save_;
    }
    last_save_ = save_profile_guarded(path_, candidate, hook);
    if (last_save_.ok) {
        current_ = candidate;
        source_schema_ = 4;
        last_error_ = ERROR_SUCCESS;
    } else {
        last_error_ = last_save_.error;
    }
    return last_save_;
}

void ProfileRepository::reset() {
    current_ = ProfileV4{};
    source_schema_ = 4;
    last_save_ = {};
    last_error_ = ERROR_SUCCESS;
}

std::size_t ProfileRepository::recover_staging(DWORD& error) {
    if (path_.empty()) {
        error = ERROR_PATH_NOT_FOUND;
        return 0;
    }
    return clean_profile_staging(profile_parent_directory(path_), error);
}

}
