// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ExposureFinderRaster.hpp"

namespace Slic3r::sla {
// The two native pattern planes precede per-layer staircase/erosion and labels.
// Brightness belongs to the LCD compositor, after these operations and labels.
struct ExposureFinderCellParameters {
    double base_height = .1, features_height = .1;
    size_t staircase_width = 0, staircase_thickness = 0;
    unsigned bottom_erosion = 0, chamfer_layers = 0;
};
// Optional native primitive painter runs before erosion; the default paints a
// binary rectangle. Arguments are left column, width and thickness in pixels.
using ExposureFinderStaircasePainter = std::function<void(png::ImageGreyscale &,size_t,size_t,size_t,const std::function<void()> &)>;
ExposureFinderCellRenderer make_exposure_finder_cell_renderer(
    const png::ImageGreyscale &base, const png::ImageGreyscale &features,
    const ExposureFinderCellParameters &, const ExposureFinderStaircasePainter & = {});
} // namespace Slic3r::sla
