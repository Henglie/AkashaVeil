// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "platform/windows/windows.h"
#include <memory>
#include <objidl.h>
#include <gdiplus.h>

namespace av {
std::wstring local_mark_time();
void paint_effects(Gdiplus::Graphics& graphics, int width, int height, unsigned dpi,
    const Window& target, const Effects& effects, bool replacement);
// All methods run on the owning UI thread. Target affinity is applied separately.
class EffectLayers {
public:
    EffectLayers();
    ~EffectLayers();
    EffectLayers(const EffectLayers&) = delete;
    EffectLayers& operator=(const EffectLayers&) = delete;
    bool set(const Window& target, const Effects& effects, DWORD& error);
    const Effects* get(const Window& target) const;
    DWORD problem(const Window& target) const;
    void notify_errors(HWND window,UINT message);
    void remove(const Window& target);
    void clear();
    void sync();
private:
    struct State;
    std::unique_ptr<State> state;
};
}
