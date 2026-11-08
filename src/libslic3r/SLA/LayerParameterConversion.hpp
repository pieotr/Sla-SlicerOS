// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ChituboxLayerParameters.hpp"
#include <cmath>
#include <limits>

namespace Slic3r::sla {

// The imported Anycubic table already expresses speeds in mm/min, but those
// doubles came from native float32 mm/s multiplied by 60. CTB stores float32
// mm/min directly. Conversion must round at the destination boundary, rather
// than reinterpret the source's units or relax native snapshot validation.
inline LayerParameters anycubic_parameters_for_ctb(const LayerParameters &source,
                                                  const LayerParameterCapabilities &schema)
{
    using F = LayerParameter;
    for (const auto &[field, value] : source)
        if (std::none_of(schema.begin(), schema.end(),
                        [field](const auto &capability) { return capability.field == field; }))
            throw RuntimeError("Destination cannot preserve a source per-layer field");
    auto represented = [](double value) {
        if (!std::isfinite(value) || value < 0 || value > std::numeric_limits<float>::max())
            throw RuntimeError("Converted layer parameter exceeds the destination float range");
        return double(float(value));
    };

    // Anycubic has no after-cure/after-lift waits, light-off floor, PWM override
    // or second retract distance in this table. Neutral values describe that
    // source behavior; the target printer preset must not invent extra phases
    // or waits. Unused phase speeds are retained when present in EXTRA.
    LayerParameters result;
    for (const auto &capability : schema) {
        const auto field = capability.field;
        const auto found = source.find(field);
        double value = 0;
        if (found != source.end())
            value = found->second;
        else if (field == F::LightPWM)
            value = 255;
        else if (field == F::LiftSpeed2)
            value = source.at(F::LiftSpeed);
        else if (field == F::RetractSpeed2)
            value = source.at(F::RetractSpeed);
        else if (field == F::PositionZ || field == F::Exposure ||
                 field == F::LiftDistance || field == F::LiftSpeed || field == F::RetractSpeed)
            throw RuntimeError("Source layer is missing a required motion or exposure parameter");
        result[field] = represented(value);
    }

    // Anycubic LiftDistance means TOTAL, while the CTB document exposes FIRST.
    // Both containers store total and second as floats. Subtract their native
    // representations without independently rounding the first phase again.
    const double total = represented(source.at(F::LiftDistance));
    const double second = result.count(F::LiftDistance2) ? result.at(F::LiftDistance2) : 0;
    if (total < second)
        throw RuntimeError("Source total lift is smaller than its second phase");
    result[F::LiftDistance] = total - second;
    validate_chitubox_parameters(result);
    return result;
}

} // namespace Slic3r::sla
