// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cmath>
#include "libslic3r/Exception.hpp"

namespace Slic3r::sla {
// Physical extents and resolution in the axes of the stored raster, after
// orientation. Source geometry must never be inferred from the chosen preset.
struct LayerRasterGeometry {
    size_t columns = 0, rows = 0;
    double width_mm = 0, height_mm = 0;
    void validate() const {
        if (!columns || !rows || !std::isfinite(width_mm) || !std::isfinite(height_mm) ||
            !std::isfinite(float(width_mm)) || !std::isfinite(float(height_mm)) ||
            float(width_mm) <= 0 || float(height_mm) <= 0)
            throw RuntimeError("Source raster geometry must have finite positive physical dimensions and resolution");
    }
    void require_same_scale(const LayerRasterGeometry &target) const {
        validate(); target.validate();
        if (columns != target.columns || rows != target.rows)
            throw RuntimeError("Selected printer resolution differs from the source layer raster");
        // These native families store float32 dimensions. Equal native words
        // permit only representational rounding, not an arbitrary mm tolerance.
        if (float(width_mm) != float(target.width_mm))
            throw RuntimeError("Selected printer LCD width differs from the source physical scale; rescale explicitly before conversion");
        if (float(height_mm) != float(target.height_mm))
            throw RuntimeError("Selected printer LCD height differs from the source physical scale; rescale explicitly before conversion");
    }
};
} // namespace Slic3r::sla
