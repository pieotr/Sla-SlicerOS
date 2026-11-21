// SPDX-License-Identifier: AGPL-3.0-or-later
// Small OpenCV-backed drawing primitives for the pixel editor engine (LayerDocument::dig_column). OpenCV stays private to
// PixelDrawing.cpp; the interface is a raw 8-bit buffer.
#pragma once

#include <cstdint>

namespace Slic3r::sla::pixel_drawing {

// cv::circle(..., thickness = -1): filled disc. anti_alias = LINE_AA (grey edge), otherwise LINE_8. Clipped to the image.
void filled_circle(uint8_t *data, int rows, int cols, int cx, int cy, int radius, uint8_t value, bool anti_alias);

} // namespace Slic3r::sla::pixel_drawing
