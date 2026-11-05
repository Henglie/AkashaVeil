// SPDX-License-Identifier: Apache-2.0
#include "app/match_choice.h"
#include <string>

namespace av {

namespace {

constexpr int dialog_id = 800;
constexpr int choice_list = 801;
constexpr int prompt_text = 802;

struct Context {
    const std::vector<TargetIdentity>* candidates = nullptr;
    const MatchChoiceStrings* strings = nullptr;
    std::size_t* selected = nullptr;
};

std::wstring line(const TargetIdentity& target) {
    std::wstring value = target.title.empty() ? L"(untitled)" : target.title;
    if (!target.class_name.empty()) value += L" | " + target.class_name;
    if (!target.path.empty()) value += L" | " + target.path;
    return value;
}

void set_text(HWND dialog, int id, const wchar_t* text) {
    if (const HWND control = GetDlgItem(dialog, id))
        SetWindowTextW(control, text ? text : L"");
}

INT_PTR CALLBACK choice_proc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        context = reinterpret_cast<Context*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
        SetWindowTextW(dialog, context->strings->title);
        set_text(dialog, prompt_text, context->strings->prompt);
        set_text(dialog, IDOK, context->strings->ok);
        set_text(dialog, IDCANCEL, context->strings->cancel);
        const HWND list = GetDlgItem(dialog, choice_list);
        for (const auto& candidate : *context->candidates)
            SendMessageW(list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(line(candidate).c_str()));
        if (!context->candidates->empty())
            SendMessageW(list, LB_SETCURSEL, 0, 0);
        RECT self{}, owner{};
        GetWindowRect(dialog, &self);
        if (GetWindowRect(GetParent(dialog), &owner))
            SetWindowPos(dialog, nullptr,
                (owner.left + owner.right - self.right + self.left) / 2,
                (owner.top + owner.bottom - self.bottom + self.top) / 2,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return TRUE;
    }
    case WM_COMMAND: {
        if (!context) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == IDOK || (id == choice_list && code == LBN_DBLCLK)) {
            const int index = static_cast<int>(SendMessageW(
                GetDlgItem(dialog, choice_list), LB_GETCURSEL, 0, 0));
            if (index < 0 || static_cast<std::size_t>(index) >= context->candidates->size()) {
                set_text(dialog, prompt_text, context->strings->no_selection);
                return TRUE;
            }
            *context->selected = static_cast<std::size_t>(index);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL || id == IDCLOSE) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

}

bool choose_match(HWND owner, const std::vector<TargetIdentity>& candidates,
    const MatchChoiceStrings& strings, std::size_t& selected) {
    if (candidates.empty()) return false;
    Context context{&candidates, &strings, &selected};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(dialog_id), owner, choice_proc,
        reinterpret_cast<LPARAM>(&context));
    return result == IDOK;
}

}
