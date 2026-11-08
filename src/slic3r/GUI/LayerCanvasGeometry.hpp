// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef slic3r_GUI_LayerCanvasGeometry_hpp_
#define slic3r_GUI_LayerCanvasGeometry_hpp_

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

} // namespace Slic3r::GUI
#endif
