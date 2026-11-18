// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef slic3r_GUI_LayerCanvasGeometry_hpp_
#define slic3r_GUI_LayerCanvasGeometry_hpp_

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Slic3r::GUI {

// Map one canvas-local coordinate to a raster pixel. Mouse capture delivers
// negative positions when a stroke leaves the canvas. Clamp those before
// scaling: mixing a negative event coordinate with size_t image dimensions
// otherwise wraps it to a large positive value, painting the opposite edge.
inline int layer_canvas_pixel(int position, int canvas_extent, int image_extent)
{
    if (position <= 0 || canvas_extent <= 0 || image_extent <= 0)
        return 0;
    if (position >= canvas_extent)
        return image_extent - 1;
    // Both dimensions are positive int values, so their product fits int64_t.
    return int(int64_t(position) * image_extent / canvas_extent);
}

// ---- Zoom and pan (pure, unit-tested) --------------------------------------
constexpr double layer_zoom_min = 0.05;
constexpr double layer_zoom_max = 16.;

// One wheel notch / key press changes the scale by this factor (geometric, so
// zooming in then out returns to the same scale).
constexpr double layer_zoom_step = 1.25;

inline double layer_zoom_clamp(double scale)
{
    if (!std::isfinite(scale)) return 1.;
    return std::min(layer_zoom_max, std::max(layer_zoom_min, scale));
}

inline double layer_zoom_after_steps(double scale, int steps)
{
    return layer_zoom_clamp(scale * std::pow(layer_zoom_step, steps));
}

// Scroll offset (px, >= 0) that keeps the image point under the cursor fixed
// when the scale changes from old_scale to new_scale.
//   canvas_cursor   cursor position inside the canvas at old_scale (px)
//   viewport_cursor cursor position inside the scrolled viewport's client area (px)
//   canvas_offset   canvas origin inside the scrolled content (border, px)
inline double layer_zoom_anchor_scroll(double canvas_cursor, double viewport_cursor,
                                       double old_scale, double new_scale, double canvas_offset = 0.)
{
    if (old_scale <= 0. || new_scale <= 0.) return 0.;
    return std::max(0., canvas_cursor * (new_scale / old_scale) + canvas_offset - viewport_cursor);
}

// Scroll offset after dragging the content by `drag_delta` px from a start offset.
inline double layer_pan_scroll(double start_scroll, double drag_delta)
{
    return std::max(0., start_scroll - drag_delta);
}

// ---- Canvas <-> pixel mapping ----------------------------------------------
// Left/top edge of a raster pixel on a canvas of the given extent.
inline double layer_pixel_to_canvas(int pixel, int canvas_extent, int image_extent)
{
    if (image_extent <= 0) return 0.;
    return double(pixel) * canvas_extent / image_extent;
}

// Physical position of a pixel's centre (mm) given the raster's physical size.
inline double layer_pixel_to_mm(int pixel, int image_extent, double extent_mm)
{
    if (image_extent <= 0) return 0.;
    return (double(pixel) + 0.5) * extent_mm / image_extent;
}

// True when the (unclamped) canvas position lies on the drawn bitmap.
inline bool layer_canvas_contains(int position, int canvas_extent)
{
    return position >= 0 && position < canvas_extent;
}

} // namespace Slic3r::GUI
#endif
