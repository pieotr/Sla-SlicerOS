// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerProgress.hpp"
#include "ExposureFinderPatternParameters.hpp"
#include <cstdint>
#include <vector>
#include <optional>

namespace Slic3r::sla {
enum class ExposureFinderSeries { Linear, Multiplier };
struct ExposureFinderTableParameters {
    double layer_height = .05, maximum_layer_height = .1, layer_height_step = .01;
    double bottom_exposure = 15., normal_exposure = 2.;
    double bottom_step = 0., normal_step = .2;
    unsigned tests = 4;
    bool multiple_layer_heights = false, include_base = true;
    ExposureFinderSeries series = ExposureFinderSeries::Linear;
};
struct ExposureFinderTableRow {
    double layer_height = 0., bottom_exposure = 0., normal_exposure = 0.;
    uint8_t brightness = 255;
    bool valid() const {
        return layer_height > 0 && bottom_exposure > 0 && normal_exposure > 0 && brightness > 0;
    }
    bool operator==(const ExposureFinderTableRow &other) const;
};
// Table preparation only: does not allocate rasters or modify a printer profile.
// Height rounds to 0.001 mm away from zero; exposure/steps to 0.01 s, ties to even.
// Preserve insertion order. Generated test duplicates are skipped; the base
// row is retained at each raw height step, as in the reference.
// Finite values within +/-1e6; at most 255 tests and 100000 planned rows.
// Negative/zero exposure rows remain visible and report valid()==false.
std::vector<ExposureFinderTableRow> generate_exposure_finder_table(
    const ExposureFinderTableParameters &, const LayerProgress &progress = {});
// Normalize without sorting or removing deliberate duplicate rows.
std::vector<ExposureFinderTableRow> normalize_exposure_finder_table(
    const std::vector<ExposureFinderTableRow> &, const LayerProgress &progress = {});
enum class ExposureFinderTableOrigin { Generated, Manual };

// Manual rows keep their validity flag semantics, including invalid/zero rows.
// Normalize, remove duplicates and sort by height/bottom/normal/brightness.
std::vector<ExposureFinderTableRow> sanitize_exposure_finder_table(
    const std::vector<ExposureFinderTableRow> &, const LayerProgress &progress = {});
// Ordered-table base selectors match the reference's first/middle/last entry,
// not a statistical min/median/max of an arbitrarily ordered manual table.
enum class ExposureFinderBaseMode { Iterative, First, Middle, Last, Custom };
enum class ExposureFinderBrightnessExclusion { None, Bottom, BottomAndBase };
struct ExposureFinderScheduleParameters {
    double base_height = .1, features_height = .1, custom_base_exposure = 2.;
    unsigned bottom_layers = 2;
    bool multiple_exposures = true, multiple_brightness = false;
    std::vector<uint8_t> brightness{255};
    ExposureFinderBaseMode base_mode = ExposureFinderBaseMode::Iterative;
    ExposureFinderBrightnessExclusion brightness_exclusion = ExposureFinderBrightnessExclusion::BottomAndBase;
    bool repeated_settings = false, repeated_lift_enabled = true, repeated_wait_enabled = true;
    double repeated_lift_height = 0., repeated_wait_before = 0.;
};
struct ExposureFinderRecipe {
    ExposureFinderTableParameters table_parameters;
    ExposureFinderTableOrigin table_origin = ExposureFinderTableOrigin::Generated;
    std::vector<ExposureFinderTableRow> rows;
    std::optional<ExposureFinderScheduleParameters> schedule;
    std::optional<ExposureFinderGenerationSettings> generation;
    void validate(const LayerProgress &progress = {}) const;
};
struct ExposureFinderPart {
    std::optional<size_t> source_row;
    double layer_height, bottom_exposure, normal_exposure;
    uint8_t requested_brightness;
    double label_bottom_exposure, label_normal_exposure;
};
struct ExposureFinderExposure {
    // Tested part spacing is distinct from the physical increment between Z groups.
    double position_z, layer_height, exposure;
    bool bottom, base;
    size_t physical_layer; // Zero-based physical Z group, independent of exposure index.
    std::vector<size_t> parts;
    // One generated exposure can contain several spatial parts with different
    // mask brightness. Motion override applies only after the first record at Z.
    std::vector<uint8_t> mask_brightness;
    std::optional<double> lift_height_override, wait_before_override;
};
struct ExposureFinderSchedule {
    std::vector<ExposureFinderPart> parts;
    std::vector<ExposureFinderExposure> exposures;
    size_t bottom_records = 0, physical_layers = 0;
};
// Plans all requested parts before raster allocation/layout. No spatial item
// may be silently dropped: the later layout must fit the complete parts list.
// Budgets: 100000 heights/records/parts, 1000000 candidate placements, 256 input
// brightness entries. Inputs are captured before callbacks; progress is phased.
// Base/feature heights and cure times use the reference's 0.01 rounding grid.
ExposureFinderSchedule plan_exposure_finder_schedule(const ExposureFinderRecipe &,
    const ExposureFinderScheduleParameters &, const LayerProgress &progress = {});
} // namespace Slic3r::sla
