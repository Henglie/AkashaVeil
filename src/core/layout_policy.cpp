// SPDX-License-Identifier: Apache-2.0
#include "core/layout_policy.h"
#include <algorithm>
#include <cstdint>

namespace av {

namespace {

constexpr int max_dimension = 65535;
constexpr int min_visible_width = 160;
constexpr int min_visible_height = 32;

using Wide = std::int64_t;

Wide area(const LayoutRect& rect) noexcept {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return 0;
    return static_cast<Wide>(layout_width(rect))
        * static_cast<Wide>(layout_height(rect));
}

LayoutRect intersection(const LayoutRect& left, const LayoutRect& right) noexcept {
    LayoutRect result{
        std::max(left.left, right.left),
        std::max(left.top, right.top),
        std::min(left.right, right.right),
        std::min(left.bottom, right.bottom)};
    if (result.right <= result.left || result.bottom <= result.top)
        return {};
    return result;
}

std::size_t primary_index(const std::vector<LayoutMonitor>& monitors) noexcept {
    for (std::size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i].primary) return i;
    return 0;
}

LayoutRect centered(const LayoutRect& work, int width, int height) noexcept {
    const int w = std::min(width, std::max(1, layout_width(work)));
    const int h = std::min(height, std::max(1, layout_height(work)));
    return make_layout_rect(
        work.left + (layout_width(work) - w) / 2,
        work.top + (layout_height(work) - h) / 2, w, h);
}

bool visible_enough(const LayoutRect& rect, const LayoutRect& work,
    int width, int height) noexcept {
    const auto visible = intersection(rect, work);
    return layout_width(visible) >= std::min(min_visible_width, width)
        && layout_height(visible) >= std::min(min_visible_height, height);
}

}

LayoutPlan plan_layout(int x, int y, int width, int height, int show,
    const std::vector<LayoutMonitor>& monitors) noexcept {
    LayoutPlan plan;
    if (monitors.empty() || width < 1 || height < 1
        || width > max_dimension || height > max_dimension
        || x < -1000000 || x > 1000000 || y < -1000000 || y > 1000000
        || show < 1 || show > 3)
        return plan;

    const LayoutRect desired = make_layout_rect(x, y, width, height);
    plan.show = show;
    plan.normal = desired;

    std::size_t best = primary_index(monitors);
    Wide best_area = 0;
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const auto hit = area(intersection(desired, monitors[i].work));
        if (hit > best_area) {
            best_area = hit;
            best = i;
        }
    }
    plan.monitor = best;
    const LayoutRect work = monitors[best].work;

    if (best_area == 0) {
        plan.normal = centered(work, width, height);
        plan.action = LayoutAction::moved_to_monitor;
        plan.visible = intersection(plan.normal, work);
        plan.reachable = visible_enough(plan.normal, work,
            layout_width(plan.normal), layout_height(plan.normal));
        return plan;
    }

    const bool fully_visible =
        best_area == static_cast<Wide>(width) * static_cast<Wide>(height);
    if (fully_visible) {
        plan.action = LayoutAction::unchanged;
        plan.visible = desired;
        plan.reachable = true;
        return plan;
    }

    int final_width = width;
    int final_height = height;
    bool resized = false;
    if (final_width > layout_width(work)) {
        final_width = layout_width(work);
        resized = true;
    }
    if (final_height > layout_height(work)) {
        final_height = layout_height(work);
        resized = true;
    }

    int left = x;
    int top = y;
    const int minimum_width = std::min(min_visible_width, final_width);
    const int minimum_height = std::min(min_visible_height, final_height);

    if (left + final_width < work.left + minimum_width)
        left = work.left + minimum_width - final_width;
    if (left > work.right - minimum_width)
        left = work.right - minimum_width;
    if (top < work.top)
        top = work.top;
    if (top > work.bottom - minimum_height)
        top = work.bottom - minimum_height;

    LayoutRect adjusted = make_layout_rect(left, top, final_width, final_height);
    if (!visible_enough(adjusted, work, final_width, final_height)) {
        adjusted = centered(work, final_width, final_height);
        resized = resized || layout_width(adjusted) != width
            || layout_height(adjusted) != height;
    }

    plan.normal = adjusted;
    plan.visible = intersection(adjusted, work);
    plan.reachable = visible_enough(adjusted, work,
        layout_width(adjusted), layout_height(adjusted));
    if (!plan.reachable) {
        plan.action = LayoutAction::invalid;
        return plan;
    }
    if (resized) plan.action = LayoutAction::resized;
    else if (adjusted.left != x || adjusted.top != y)
        plan.action = LayoutAction::shifted;
    else
        plan.action = LayoutAction::unchanged;
    return plan;
}

bool layout_rect_reachable(const LayoutRect& rect,
    const std::vector<LayoutMonitor>& monitors,
    int minimum_width, int minimum_height) noexcept {
    const int width = layout_width(rect);
    const int height = layout_height(rect);
    if (monitors.empty() || width < 1 || height < 1) return false;
    const int required_width = std::clamp(minimum_width, 1, width);
    const int required_height = std::clamp(minimum_height, 1, height);
    for (const auto& monitor : monitors) {
        const auto visible = intersection(rect, monitor.work);
        if (layout_width(visible) >= required_width
            && layout_height(visible) >= required_height)
            return true;
    }
    return false;
}

}
