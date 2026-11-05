// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <vector>

namespace av {

struct LayoutRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

constexpr int layout_width(const LayoutRect& rect) noexcept {
    return rect.right - rect.left;
}
constexpr int layout_height(const LayoutRect& rect) noexcept {
    return rect.bottom - rect.top;
}
constexpr LayoutRect make_layout_rect(int x, int y, int width, int height) noexcept {
    return {x, y, x + width, y + height};
}

struct LayoutMonitor {
    LayoutRect work;
    bool primary = false;
    unsigned dpi = 96;
};

enum class LayoutAction : unsigned {
    unchanged = 0,
    shifted = 1,
    resized = 2,
    moved_to_monitor = 3,
    invalid = 4
};

struct LayoutPlan {
    LayoutRect normal;
    int show = 1;
    LayoutAction action = LayoutAction::invalid;
    bool reachable = false;
    std::size_t monitor = 0;
    LayoutRect visible;
};

// Pure geometry decision. Coordinates are virtual-screen physical pixels; DPI is
// carried for diagnostics and must not rescale an already reachable rectangle.
LayoutPlan plan_layout(int x, int y, int width, int height, int show,
    const std::vector<LayoutMonitor>& monitors) noexcept;

bool layout_rect_reachable(const LayoutRect& rect,
    const std::vector<LayoutMonitor>& monitors,
    int minimum_width = 160, int minimum_height = 32) noexcept;

}
