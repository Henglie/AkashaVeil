// SPDX-License-Identifier: Apache-2.0
#include "platform/windows/capture.h"
#include "platform/windows/payload.h"
#include "helper/protocol.h"
#include "version.h"
#include <dwmapi.h>

namespace av {
bool apply_affinity(const Window& w, Mode mode, DWORD& error, bool allow_worker) {
    if (!valid_mode(static_cast<unsigned>(mode))) { error = ERROR_INVALID_PARAMETER; return false; }
    if (!same_window(w)) { error = ERROR_INVALID_WINDOW_HANDLE; return false; }
    const auto hwnd = reinterpret_cast<HWND>(w.handle);
    BOOL composing = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&composing)) || !composing) { error = ERROR_NOT_SUPPORTED; return false; }
    const auto arch = architecture_of(w.pid);
    if (arch.empty()) { error = ERROR_ACCESS_DENIED; return false; }
    Payload payload;
    if (w.pid != GetCurrentProcessId() && !prepare_payload(arch, arch != AV_ARCH, payload, error)) return false;
    if (arch != AV_ARCH) {
        if (!allow_worker) { error = ERROR_BAD_EXE_FORMAT; return false; }
        if (payload.worker_path.empty() || payload.hook_path.empty()) { error = ERROR_FILE_NOT_FOUND; return false; }
        // The worker receives the exact verified hook path. It never derives a
        // fixed basename from its own module directory.
        const auto& worker = payload.worker_path;
        std::wstring command = L"\"" + worker + L"\" " + std::to_wstring(w.handle) + L" " + std::to_wstring(w.pid)
            + L" " + std::to_wstring(w.tid) + L" " + std::to_wstring(w.process_created) + L" " + std::to_wstring(static_cast<unsigned>(mode))
            + L" \"" + payload.hook_path + L"\"";
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &process)) { error = GetLastError(); return false; }
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &error);
        else { error = ERROR_TIMEOUT; TerminateProcess(process.hProcess, ERROR_TIMEOUT); WaitForSingleObject(process.hProcess, 1000); }
        CloseHandle(process.hProcess);
        return error == 0;
    }
    if (w.pid == GetCurrentProcessId()) {
        if (!SetWindowDisplayAffinity(hwnd, static_cast<DWORD>(mode))) { error = GetLastError(); return false; }
    } else {
        if (payload.hook_path.empty()) { error = ERROR_FILE_NOT_FOUND; return false; }
        const HMODULE module = LoadLibraryExW(payload.hook_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) { error = GetLastError(); return false; }
        const auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "AkashaVeilHook"));
        HHOOK hook = callback ? SetWindowsHookExW(WH_CALLWNDPROC, callback, module, w.tid) : nullptr;
        if (!hook) { error = GetLastError(); FreeLibrary(module); return false; }
        const UINT message = RegisterWindowMessageW(AV_CAPTURE_MESSAGE);
        DWORD_PTR ignored = 0;
        SetLastError(ERROR_SUCCESS);
        const auto sent = message && SendMessageTimeoutW(hwnd, message, w.pid, static_cast<LPARAM>(mode), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &ignored);
        error = sent ? ERROR_SUCCESS : GetLastError();
        if (!sent && !error) error = ERROR_TIMEOUT;
        UnhookWindowsHookEx(hook); FreeLibrary(module);
        if (!sent) return false;
    }
    if (!same_window(w)) { error = ERROR_INVALID_WINDOW_HANDLE; return false; }
    DWORD actual = 0;
    if (!GetWindowDisplayAffinity(hwnd, &actual)) { error = GetLastError(); return false; }
    // Pre-2004 Windows treats EXCLUDEFROMCAPTURE as MONITOR.
    const bool ok = actual == static_cast<DWORD>(mode) || (mode == Mode::exclude && actual == 1);
    error = ok ? ERROR_SUCCESS : ERROR_INVALID_STATE;
    return ok;
}
}
