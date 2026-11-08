// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ExposureFinderTable.hpp"
#include "LayerDocument.hpp"
#include "LayerRasterGeometry.hpp"

namespace Slic3r::sla {
struct ExposureFinderCellOrigin {
    size_t column, row;
    bool operator==(const ExposureFinderCellOrigin &other) const { return column==other.column && row==other.row; }
};
struct ExposureFinderRasterLayout {
    LayerRasterGeometry geometry;
    ExposureFinderLayoutParameters parameters;
    std::vector<ExposureFinderCellOrigin> origins;
    size_t grid_columns = 0, grid_rows = 0;
};
// Geometry is already in oriented raster axes. Margins/gaps round upward so
// physical minimum spacing is respected; fit includes every requested part.
ExposureFinderRasterLayout plan_exposure_finder_layout(const LayerRasterGeometry &, size_t part_count,
    const ExposureFinderLayoutParameters &, const LayerProgress &progress = {});
using ExposureFinderCellRenderer = std::function<png::ImageGreyscale(
    const ExposureFinderPart &, const ExposureFinderExposure &, const std::function<void()> &)>;
// All cells must match the planned footprint. Inputs and the renderer callable
// are captured before callbacks. Return only after final validation succeeds.
// This compositor does not substitute a generic shape for a missing renderer.
LayerDocument compose_exposure_finder_layers(const ExposureFinderSchedule &, const ExposureFinderRasterLayout &,
    const ExposureFinderCellRenderer &, const LayerProgress &progress = {});
} // namespace Slic3r::sla
