// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerParameters.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"

namespace Slic3r::sla {
// Raw native values; public UVtools rounding/fallback is a separate view.
inline LayerParameters cxdlpv4_layer_parameters(const CrealityCXDLPv4Layout &layout, size_t layer)
{
    if (layer >= layout.layers.size())
        throw RuntimeError("CXDLPv4 layer index is outside the parameter table");
    using F = LayerParameter;
    const size_t row = layout.table + 40 * layer;
    const size_t extended = layout.layers[layer].data;
    const double total = layout.number(extended);
    const double second = layout.number(extended + 8);
    LayerParameters values{
        {F::PositionZ, layout.number(row)},
        {F::Exposure, layout.number(row + 4)},
        {F::LightOff, layout.number(row + 8)},
        {F::LiftDistance, total - second},
        {F::LiftDistance2, second},
        {F::LiftSpeed, layout.number(extended + 4)},
        {F::LiftSpeed2, layout.number(extended + 12)},
        {F::RetractSpeed, layout.number(extended + 16)},
        {F::RetractDistance2, layout.number(extended + 20)},
        {F::RetractSpeed2, layout.number(extended + 24)},
        {F::WaitAfter, layout.number(extended + 28)},
        {F::WaitAfterLift, layout.number(extended + 32)},
        {F::WaitBefore, layout.number(extended + 36)},
        {F::LightPWM, layout.number(extended + 40)}};
    for (const auto &[field, value] : values)
        if (!std::isfinite(value) || value < 0)
            throw RuntimeError("Invalid CXDLPv4 native layer parameter or phase geometry");
    if (values.at(F::RetractDistance2) > total)
        throw RuntimeError("CXDLPv4 second retract distance exceeds total lift distance");
    const double pwm = values.at(F::LightPWM);
    if (pwm > 255 || pwm != std::floor(pwm))
        throw RuntimeError("CXDLPv4 native light PWM must be an integer from 0 to 255");
    // Keep zero/tiny raw speeds intact. Effective speed fallback, dummy-row
    // behavior and destination representability require the full job context.
    // This reader does not claim that a raw row is printable or editable.
    return values;
}
} // namespace Slic3r::sla
