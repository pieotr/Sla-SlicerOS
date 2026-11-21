// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "CrealityCXDLPv4JobReader.hpp"
#include "CrealityCXDLPv4LayerParameters.hpp"
#include "LayerProgress.hpp"
#include <algorithm>

namespace Slic3r::sla {

inline float cxdlpv4_public_round2(float value)
{
    // .NET MathF.Round(value, 2) leaves large finite floats unchanged.
    const float rounded = std::abs(value) >= 1e8f ? value : std::nearbyint(value * 100.f) / 100.f;
    if (!std::isfinite(rounded))
        throw RuntimeError("Invalid CXDLPv4 public parameter projection");
    return rounded;
}

// Six virtual global fields are assigned only after all reference row CopyTo
// calls. In particular bottom retract fallback still uses initial 100/80,
// regardless of the meaningful row chosen below. This is a diagnostic view,
// not an export plan or a replacement for the seven native bottom globals.
template<class ImageAt>
CXDLPv4ReferenceBottomView cxdlpv4_reference_bottom_view(
    const CrealityCXDLPv4Layout &layout, ImageAt image_at,
    const LayerProgress &progress = {})
{
    using F = LayerParameter;
    CXDLPv4ReferenceBottomView result;
    result.native = cxdlpv4_native_globals(layout);
    result.decoded_bottom_layers = uint16_t(result.native.bottom_layers);
    for (F field : {F::RetractSpeed, F::RetractDistance2, F::RetractSpeed2,
                    F::WaitBefore, F::WaitAfter, F::WaitAfterLift})
        result.virtual_bottom[field] = 0.;

    const size_t count = std::min(size_t(result.decoded_bottom_layers), layout.layers.size());
    check_layer_progress(progress, 0, count);
    for (size_t i = 0; i < count; ++i) {
        const auto native_row = cxdlpv4_layer_parameters(layout, i);
        const float exposure = cxdlpv4_public_round2(float(native_row.at(F::Exposure)));
        if (exposure > .01f) {
            // Current canonical masks, including grayscale and editor changes,
            // determine dummy status. Stop counting after the second lit pixel.
            const auto mask = image_at(i);
            size_t lit = 0;
            for (uint8_t pixel : mask.buf)
                if (pixel != 0 && ++lit == 2) break;
            if (lit > 1) {
                const size_t ex = layout.layers[i].data;
                const float total = cxdlpv4_public_round2(layout.number(ex));
                float first = cxdlpv4_public_round2(total - layout.number(ex + 8));
                const float second = cxdlpv4_public_round2(layout.number(ex + 8));
                const size_t parameters = layout.integer(layout.base + 52);
                const size_t slicer = layout.integer(layout.base + 72);
                const float global_second = layout.number(slicer);
                const float global_first = cxdlpv4_public_round2(
                    std::max(0.f, layout.number(parameters) - global_second));
                if (first < 0) first = global_first; // Reference Layer.LiftHeight fallback.
                const float row_retract2 = std::clamp(
                    cxdlpv4_public_round2(float(native_row.at(F::RetractDistance2))), 0.f,
                    cxdlpv4_public_round2(first + second));

                // The final bottom-global setter caps against global lift
                // getters, which need not equal this row's lift distances.
                const float global_total = cxdlpv4_public_round2(global_first + global_second);
                auto retract_speed = [&](F field, float fallback) {
                    const float rounded = cxdlpv4_public_round2(float(native_row.at(field)));
                    return double(cxdlpv4_public_round2(rounded <= 0 ? fallback : rounded));
                };
                result.virtual_bottom = {
                    {F::RetractSpeed, retract_speed(F::RetractSpeed, 100.f)},
                    {F::RetractSpeed2, retract_speed(F::RetractSpeed2, 80.f)},
                    {F::RetractDistance2, std::clamp(cxdlpv4_public_round2(row_retract2), 0.f, global_total)},
                    {F::WaitBefore, cxdlpv4_public_round2(float(native_row.at(F::WaitBefore)))},
                    {F::WaitAfter, cxdlpv4_public_round2(float(native_row.at(F::WaitAfter)))},
                    {F::WaitAfterLift, cxdlpv4_public_round2(float(native_row.at(F::WaitAfterLift)))}};
                result.meaningful_bottom_layer = i;
                check_layer_progress(progress, i + 1, count);
                return result;
            }
        }
        check_layer_progress(progress, i + 1, count);
    }
    return result;
}

} // namespace Slic3r::sla
