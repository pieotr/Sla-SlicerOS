// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ExposureFinderPattern.hpp"
#include "LayerJobParameters.hpp"
#include <cstdint>
namespace Slic3r::sla {
// ASCII integer lists accept whitespace, comma or semicolon separators.
// Feature sizes become canonical sorted/unique. Brightness deliberately keeps
// original order, duplicates and zero (even while its option is disabled).
std::vector<unsigned> parse_exposure_finder_features(const std::string &);
std::vector<std::array<unsigned,2>> parse_exposure_finder_rings(const std::string &);
std::vector<uint8_t> parse_exposure_finder_brightness(const std::string &);
// One height,bottom,normal,brightness record per line; blank lines are ignored.
// Exact requested double values, invalid rows, duplicates and insertion order
// remain visible. Normalization/sanitizing are separate explicit operations.
std::vector<ExposureFinderTableRow> parse_exposure_finder_manual_rows(const std::string &);
struct ExposureFinderGenerationPlan {
    LayerRasterGeometry geometry;
    ExposureFinderPatternParameters requested_pattern, effective_pattern;
    size_t cell_columns=0, cell_rows=0, grid_columns=0, grid_rows=0;
    double cell_width_mm=0., cell_height_mm=0.;
    size_t spatial_parts=0, exposure_records=0, physical_layers=0;
    double max_z=0.;
    uint64_t raster_pixels=0, logical_mask_bytes=0;
    // Runtime compositor limit, not a prediction of compressed output size.
    uint64_t encoded_mask_limit_bytes=uint64_t(1)<<30;
};
// Preflight renders only the two cell planes, never all LCD exposure masks.
// Request/config are copied before invoking callbacks; all units remain native.
ExposureFinderGenerationPlan plan_exposure_finder_generation(
    const DynamicPrintConfig &, const ExposureFinderGenerationParameters &, const LayerProgress & = {});
std::string describe_exposure_finder_generation_plan(const ExposureFinderGenerationPlan &);
// Shared early validation used by both preflight and the real factory.
LayerJobParameters validate_exposure_finder_configuration(const DynamicPrintConfig &);
// Resolve schedule overrides once and validate effective moving phases before
// allocating cells/LCD masks. Used unchanged by preview and actual factory.
std::vector<LayerParameters> make_exposure_finder_rows(const LayerJobParameters &,
    const ExposureFinderSchedule &, const LayerProgress & = {});
} // namespace Slic3r::sla
