// Explicit physical/material context for imported layer exports.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cmath>
#include <limits>
#include "libslic3r/Exception.hpp"
namespace Slic3r::sla {
// Relative native float deltas and an independently rounded machine limit
// can differ at the ceiling. Bound only their representational uncertainty;
// this is not a configurable tolerance for losing or moving layers.
inline bool layer_z_within_native_limit(double z,double limit) {
    return std::isfinite(z) && std::isfinite(limit) && limit>0 &&
        (z<=limit || z-limit<=limit*std::numeric_limits<float>::epsilon());
}
struct LayerExportProfile {
    size_t columns = 0, rows = 0; // Output raster axes, after portrait rotation.
    double screen_width_mm = 0, screen_height_mm = 0;
    double density_g_ml = 0, cost_per_ml = 0;
    double max_print_height_mm = 0; // 0 means unknown, not an inferred printer limit.
    void validate(size_t expected_columns, size_t expected_rows) const {
        if (columns != expected_columns || rows != expected_rows)
            throw RuntimeError("Selected printer resolution differs from the imported layer raster");
        if (!std::isfinite(screen_width_mm) || !std::isfinite(screen_height_mm) ||
            screen_width_mm <= 0 || screen_height_mm <= 0 ||
            !std::isfinite(density_g_ml) || density_g_ml <= 0 ||
            !std::isfinite(cost_per_ml) || cost_per_ml < 0 ||
            !std::isfinite(max_print_height_mm) || max_print_height_mm < 0 ||
            (max_print_height_mm>0 && (!std::isfinite(float(max_print_height_mm)) || float(max_print_height_mm)<=0)))
            throw RuntimeError("Screen dimensions, resin density, price and printer height must be valid finite values");
        if (!std::isfinite(pixel_area()) || pixel_area() <= 0)
            throw RuntimeError("Selected printer pixel area is outside the numeric range");
    }
    void validate_height(double z) const {
        if (!std::isfinite(z) || z<=0 || !std::isfinite(max_print_height_mm) || max_print_height_mm<0 ||
            (max_print_height_mm>0 && !layer_z_within_native_limit(z,max_print_height_mm)))
            throw RuntimeError("Layer Z exceeds the selected printer height or is invalid");
    }
    double pixel_area() const { return screen_width_mm*screen_height_mm/(double(columns)*rows); }
};
}
