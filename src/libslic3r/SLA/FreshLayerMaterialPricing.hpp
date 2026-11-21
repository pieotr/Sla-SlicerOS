// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/Exception.hpp"
#include <array>
#include <cmath>

namespace Slic3r::sla {
// A generated job has an authoritative resin profile. Imported archives only
// have rounded native totals, so their existing proportional rescaling remains
// a separate policy. Never infer fresh pricing from those bootstrap totals.
struct FreshLayerMaterialPricing {
    double density_g_ml;
    double bottle_cost;
    double bottle_volume_ml;

    void validate() const {
        if (!std::isfinite(density_g_ml) || density_g_ml < 0 ||
            !std::isfinite(bottle_cost) || bottle_cost < 0 ||
            !std::isfinite(bottle_volume_ml) || bottle_volume_ml <= 0)
            throw RuntimeError("Fresh material pricing requires finite nonnegative density/cost and positive bottle volume");
        // CTB resin records also store density as float32, even for empty
        // masks whose mass cannot reveal overflow or positive underflow.
        const float density=float(density_g_ml);
        if (!std::isfinite(density) || (density_g_ml > 0 && density == 0))
            throw RuntimeError("Fresh material density exceeds the positive native float range");
    }
    // Integrate native geometry/Z in double first, then round each independent
    // native total once to float32. Decimal display rounding is not file data.
    std::array<float, 3> native_totals(double ml) const {
        validate();
        if (!std::isfinite(ml) || ml < 0)
            throw RuntimeError("Fresh material volume must be finite and nonnegative");
        const std::array<double, 3> physical{ml, ml * density_g_ml, ml * bottle_cost / bottle_volume_ml};
        const std::array<float, 3> values{float(physical[0]), float(physical[1]), float(physical[2])};
        for (size_t i=0;i<values.size();++i)
            if (!std::isfinite(values[i]) || values[i] < 0 || (physical[i] > 0 && values[i] == 0))
                throw RuntimeError("Fresh material totals exceed the native float range");
        return values;
    }
};
} // namespace Slic3r::sla
