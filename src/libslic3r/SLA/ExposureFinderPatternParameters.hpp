// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <vector>
#include <string>
#include <cstddef>
namespace Slic3r::sla {
// Pixel feature lists are canonical, sorted and unique. Physical margins and
// bar lengths use the oriented printer LCD geometry, never a nominal bed size.
struct ExposureFinderPatternParameters {
    std::vector<unsigned> holes;
    bool square_holes = true;
    std::vector<unsigned> bars{4, 6, 8, 60};
    std::vector<std::array<unsigned, 2>> bullseyes{{26, 5}, {60, 10}, {116, 15}, {190, 20}};
    double feature_margin_mm = 2., bar_length_mm = 4., bar_spacing_mm = 1.5;
    int bar_splitter = 0, bar_fence = 10, bar_fence_offset = 4;
    int bullseye_fence = 10, bullseye_fence_offset = 0;
    bool invert_bullseye_quadrants = true, counter_triangles = true, triangle_fence = false;
    int triangle_tip_offset = 3;
    unsigned staircase_thickness = 40;
    bool antialias = false;
    std::string text = "ABHJQRWZ%&#";
    int font_face = 2, text_thickness = 2;
    double text_scale = 1.;
};
struct ExposureFinderLayoutParameters {
    size_t cell_columns = 0, cell_rows = 0;
    double margin_x_mm = 0., margin_y_mm = 0., gap_x_mm = 0., gap_y_mm = 0.;
    bool center = true, mirror_x = false, mirror_y = false;
};
// Value-owned generation provenance, independent of renderer/GUI lifetimes.
// Version 1 fixes the classical Hershey font and inclusive rectangle contract.
struct ExposureFinderGenerationSettings {
    ExposureFinderPatternParameters pattern;
    ExposureFinderLayoutParameters layout;
    unsigned bottom_erosion = 0, chamfer_layers = 0;
    void validate() const;
};
ExposureFinderPatternParameters normalize_exposure_finder_pattern_parameters(
    const ExposureFinderPatternParameters &);
} // namespace Slic3r::sla
