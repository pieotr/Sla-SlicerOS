// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "CrealityCXDLPv4JobParameters.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Layout.hpp"

namespace Slic3r::sla {
inline CXDLPv4NativeJobSnapshot cxdlpv4_native_globals(const CrealityCXDLPv4Layout &layout)
{
    using F = LayerParameter;
    const size_t base = layout.base;
    const size_t parameters = layout.integer(base + 52);
    const size_t slicer = layout.integer(base + 72);
    if (!parameters || !slicer)
        throw RuntimeError("CXDLPv4 native globals require print parameters and slicer information");
    layout.require(parameters, 68);
    layout.require(slicer, 76);
    CXDLPv4NativeJobSnapshot result;
    result.layer_height = layout.number(base + 20);
    if (!std::isfinite(result.layer_height) || result.layer_height <= 0)
        throw RuntimeError("Invalid CXDLPv4 native nominal layer height");
    // Keep native uint32 metadata. Reference ushort getters and executable
    // cardinality rules belong to a contextual export plan, not this reader.
    result.bottom_layers = layout.integer(base + 24);
    result.parameter_bottom_layers = layout.integer(parameters + 40);
    result.transition_layers = layout.integer(slicer + 64);
    result.raw_per_layer_mode = layout.integer(slicer + 28);
    result.mirrors = {layout.number(slicer + 52), layout.number(slicer + 56),
                      layout.number(slicer + 60)};
    auto pwm = [&](size_t offset) {
        layout.require(offset, 2);
        return double(uint16_t(layout.bytes[offset]) | (uint16_t(layout.bytes[offset + 1]) << 8));
    };
    const double normal_total = layout.number(parameters + 8);
    const double bottom_total = layout.number(parameters);
    result.normal = {
        {F::Exposure, layout.number(parameters + 44)},
        {F::LightOff, layout.number(parameters + 36)},
        {F::LiftDistance, normal_total - layout.number(slicer + 8)},
        {F::LiftDistance2, layout.number(slicer + 8)},
        {F::LiftSpeed, layout.number(parameters + 12)},
        {F::LiftSpeed2, layout.number(slicer + 12)},
        {F::RetractSpeed, layout.number(parameters + 16)},
        {F::RetractDistance2, layout.number(slicer + 16)},
        {F::RetractSpeed2, layout.number(slicer + 20)},
        {F::WaitBefore, layout.number(slicer + 44)},
        {F::WaitAfter, layout.number(slicer + 48)},
        {F::WaitAfterLift, layout.number(slicer + 24)},
        {F::LightPWM, pwm(base + 64)}};
    result.bottom = {
        {F::Exposure, layout.number(parameters + 48)},
        {F::LightOff, layout.number(parameters + 32)},
        {F::LiftDistance, bottom_total - layout.number(slicer)},
        {F::LiftDistance2, layout.number(slicer)},
        {F::LiftSpeed, layout.number(parameters + 4)},
        {F::LiftSpeed2, layout.number(slicer + 4)},
        {F::LightPWM, pwm(base + 66)}};
    for (const auto *bucket : {&result.normal, &result.bottom}) {
        for (const auto &[field, value] : *bucket)
            if (!std::isfinite(value) || value < 0)
                throw RuntimeError("Invalid CXDLPv4 native global parameter or phase geometry");
        if (bucket->at(F::LightPWM) > 255)
            throw RuntimeError("CXDLPv4 native global PWM exceeds 255");
    }
    if (result.normal.at(F::RetractDistance2) > normal_total)
        throw RuntimeError("CXDLPv4 global second retract distance exceeds total lift distance");
    // Do not manufacture the six virtual bottom fields from presets, row0
    // or the ignored S+60 mirror. A reference projection needs masks and the
    // first meaningful bottom row after all row precision/fallback rules.
    return result;
}
} // namespace Slic3r::sla
