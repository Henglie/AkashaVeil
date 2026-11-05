// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/identity.h"
#include <cstddef>
#include <vector>
#include <windows.h>

namespace av {

struct MatchChoiceStrings {
    const wchar_t* title = L"Multiple matching windows";
    const wchar_t* prompt = L"Choose the exact window to apply the saved layout to:";
    const wchar_t* ok = L"Use selected";
    const wchar_t* cancel = L"Cancel";
    const wchar_t* no_selection = L"Select a window first.";
};

// Modal ambiguity chooser candidate.  It never picks candidates[0] implicitly:
// a valid selection is required before returning true.  Not invoked by tests.
bool choose_match(HWND owner, const std::vector<TargetIdentity>& candidates,
    const MatchChoiceStrings& strings, std::size_t& selected);

}
