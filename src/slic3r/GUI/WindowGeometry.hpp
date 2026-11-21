// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SLIC3R_GUI_WINDOW_GEOMETRY_HPP
#define SLIC3R_GUI_WINDOW_GEOMETRY_HPP

#include <algorithm>
#include <cstdint>

namespace Slic3r::GUI {
struct WindowGeometryRect { int x, y, width, height; };
struct WindowGeometryMinimum { int width, height; };

// The supplied minimum is the native top-level window minimum, not its
// current saved size. An unspecified/nonpositive minimum means one pixel.
// If that minimum exceeds the workarea, full fit is impossible: preserve
// the minimum and anchor the oversized axis at the workarea origin.
inline WindowGeometryRect clamp_window_to_workarea(WindowGeometryRect window,
                                                   WindowGeometryRect workarea,
                                                   WindowGeometryMinimum minimum)
{
    if (workarea.width <= 0 || workarea.height <= 0)
        return window;
    auto dimension = [](int requested, int available, int native_minimum) {
        const int floor = std::max(1, native_minimum);
        return std::clamp(requested, floor, std::max(available, floor));
    };
    window.width = dimension(window.width, workarea.width, minimum.width);
    window.height = dimension(window.height, workarea.height, minimum.height);
    auto position = [](int requested, int extent, int origin, int available) {
        if (extent > available) return origin;
        // Widen before adding: monitor origins may legitimately be negative.
        const int64_t last = int64_t(origin) + available - extent;
        // The result is either requested/origin (both int) or last<=requested,
        // so it is representable without narrowing an out-of-range endpoint.
        return int(std::clamp(int64_t(requested), int64_t(origin), last));
    };
    window.x = position(window.x, window.width, workarea.x, workarea.width);
    window.y = position(window.y, window.height, workarea.y, workarea.height);
    return window;
}
} // namespace Slic3r::GUI
#endif
