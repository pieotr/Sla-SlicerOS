// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "RasterBase.hpp"
#include <functional>
namespace Slic3r::sla {
// Identical pixel encoding to PNGRasterEncoder, with cancellation at PNG rows.
// A callback exception is rethrown after libpng is cleaned up; no PNG is returned.
EncodedRaster encode_png_raster(const void *pixels, size_t columns, size_t rows, size_t components,
    const std::function<void()> &checkpoint = {});
}
