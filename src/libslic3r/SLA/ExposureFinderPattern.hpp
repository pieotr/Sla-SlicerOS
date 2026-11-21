// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ExposureFinderCell.hpp"
#include <array>
#include <string>
#include <memory>
namespace Slic3r {
class DynamicPrintConfig;
class SLAPrint;
} // namespace Slic3r
namespace Slic3r::sla {
struct ExposureFinderPattern {
    png::ImageGreyscale base{}, features{};
    ExposureFinderPatternParameters parameters;
    std::array<int, 2> positive_marking{}, negative_marking{};
    size_t staircase_width = 0;
};
ExposureFinderPattern make_exposure_finder_pattern(const LayerRasterGeometry &,
                                                   const ExposureFinderPatternParameters &,
                                                   const std::function<void()> &checkpoint = {});
// Finishing precedes readable dose labels; LCD brightness is applied afterwards
// by compose_exposure_finder_layers. Dose labels use a stable two-decimal format.
ExposureFinderCellRenderer make_exposure_finder_pattern_renderer(const ExposureFinderPattern &,
                                                                 ExposureFinderCellParameters,
                                                                 bool multiple_brightness);
struct ExposureFinderGenerationParameters {
    ExposureFinderRecipe recipe;
    ExposureFinderPatternParameters pattern;
    // Zero cell dimensions are inferred from the pattern; explicit sizes must
    // match. Mirrors are additional to the oriented printer's mirror flags.
    ExposureFinderLayoutParameters layout;
    unsigned bottom_erosion = 0, chamfer_layers = 0;
};
// Complete native generation into an owned raster job: no mesh or external
// process. The supplied recipe must contain an explicit exposure schedule.
std::unique_ptr<SLAPrint> create_exposure_finder_calibration(const DynamicPrintConfig &,
                                                             const ExposureFinderGenerationParameters &,
                                                             const LayerProgress & = {});
} // namespace Slic3r::sla
