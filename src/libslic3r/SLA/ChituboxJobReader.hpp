// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ChituboxJobOverlay.hpp"
#include "ChituboxLayerParameters.hpp"

namespace Slic3r::sla {
// One native interpretation for imported documents and final conversion
// readback. Duplicate fields and disabled per-layer firmware modes are checked
// before any values are accepted. This reader never constructs PNG images.
inline LayerJobParameters chitubox_job_parameters(const ChituboxLayerLayout &layout,
    const std::function<void()> &check = {})
{
    using F = LayerParameter;
    if (check) check();
    if (layout.binary || layout.magic == 0xff220810 || layout.version < 3 || layout.version > 5)
        throw RuntimeError("Native global readback requires ordinary CTB v3, v4 or v5");
    const auto &bytes = layout.bytes;
    if (layout.version == 3) {
        const auto params = layout.parameters, slicer = layout.integer(104);
        if (!params || layout.integer(108) < 64)
            throw RuntimeError("Missing CTB v3 global parameter sections");
        if (layout.integer(params + 40) != layout.integer(48) ||
            layout.number(params + 36) != layout.number(44))
            throw RuntimeError("Conflicting duplicate Chitubox global defaults");
        LayerJobParameters actual;
        actual.layer_height = layout.number(32);
        actual.bottom_layers = layout.integer(48);
        actual.transition_layers = layout.integer(slicer + 60);
        // V3 has a single motion phase. The second-phase/wait slots exist
        // physically but UVtools ignores them for v3; do not reinterpret them
        // or inherit their semantics from a newly selected printer preset.
        for (bool bottom : {false, true}) {
            auto &row = bottom ? actual.bottom : actual.normal;
            row = {{F::Exposure, layout.number(bottom ? 40 : 36)},
                {F::LiftDistance, layout.number(params + (bottom ? 0 : 8))},
                {F::LiftSpeed, layout.number(params + (bottom ? 4 : 12))},
                {F::RetractSpeed, layout.number(params + 16)},
                {F::LightOff, layout.number(params + (bottom ? 32 : 36))},
                {F::LightPWM, double(uint16_t(bytes.at(bottom ? 98 : 96)) |
                    (uint16_t(bytes.at(bottom ? 99 : 97)) << 8))},
                {F::LiftDistance2, 0}, {F::LiftSpeed2, 0},
                {F::RetractDistance2, 0}, {F::RetractSpeed2, 0},
                {F::WaitBefore, 0}, {F::WaitAfter, 0}, {F::WaitAfterLift, 0}};
        }
        validate_chitubox_job(actual, layout.count);
        const auto flag = bytes.at(slicer + 39);
        if (flag != 0 && flag != 0x30)
            throw RuntimeError("Unknown or mismatched source per-layer settings mode");
        if (flag == 0) {
            // Conservative firmware ambiguity check, independent of UVtools'
            // decoder, which exposes records even when this mode is disabled.
            for (size_t i = 0; i < layout.count; ++i) {
                if (check) check();
                const auto row = chitubox_layer_parameters(layout, i);
                const auto &defaults = i < actual.bottom_layers ? actual.bottom : actual.normal;
                for (const auto &[field, value] : row)
                    if (field != F::PositionZ && defaults.at(field) != value)
                        throw RuntimeError("Disabled source per-layer table conflicts with global defaults");
            }
        }
        return actual;
    }
    if (!layout.parameters)
        throw RuntimeError("Missing CTB global parameter section");
    LayerJobParameters actual;
    uint8_t flag = 0; unsigned enabled = 0;
    const auto slicer = layout.integer(104), v4 = layout.integer(slicer + 64), params = layout.parameters;
    flag = bytes.at(slicer + 39); enabled = layout.version * 0x10;
    if (layout.number(slicer + 52) != layout.number(v4 + 24) ||
        layout.number(slicer + 24) != layout.number(v4 + 28) || layout.number(slicer + 56) != layout.number(v4 + 28))
        throw RuntimeError("Conflicting duplicate Chitubox global waits");
    if (layout.integer(params + 40) != layout.integer(48) || layout.number(44) != layout.number(params + 36))
        throw RuntimeError("Conflicting duplicate Chitubox global defaults");
    actual.layer_height = layout.number(32);
    actual.bottom_layers = layout.integer(48); actual.transition_layers = layout.integer(slicer + 60);
    for (bool bottom : {false, true}) {
        auto &row = bottom ? actual.bottom : actual.normal;
        row = {{F::Exposure, layout.number(bottom ? 40 : 36)},
            {F::LiftDistance, double(layout.number(params + (bottom ? 0 : 8))) - layout.number(slicer + (bottom ? 0 : 8))},
            {F::LiftDistance2, layout.number(slicer + (bottom ? 0 : 8))},
            {F::LiftSpeed, layout.number(params + (bottom ? 4 : 12))}, {F::LiftSpeed2, layout.number(slicer + (bottom ? 4 : 12))},
            {F::RetractSpeed, layout.number(bottom ? v4 : params + 16)},
            {F::RetractSpeed2, layout.number(bottom ? v4 + 4 : slicer + 20)},
            {F::RetractDistance2, layout.number(bottom ? v4 + 36 : slicer + 16)},
            {F::WaitBefore, layout.number(v4 + 24)}, {F::WaitAfter, layout.number(v4 + 32)},
            {F::WaitAfterLift, layout.number(v4 + 28)}, {F::LightOff, layout.number(params + (bottom ? 32 : 36))},
            {F::LightPWM, double(uint16_t(bytes.at(bottom ? 98 : 96)) | (uint16_t(bytes.at(bottom ? 99 : 97)) << 8))}};
    }
    validate_chitubox_job(actual, layout.count);
    if (flag != 0 && flag != enabled)
        throw RuntimeError("Unknown or mismatched source per-layer settings mode");
    if (flag == 0) {
        // Inspect the original raw rows, not edited document overrides. A
        // disabled table with conflicting values has ambiguous firmware meaning.
        const auto check_rows = [&](const auto &layout, const auto &read_row) {
            for (size_t i = 0; i < layout.count; ++i) {
                if (check) check();
                const auto row = read_row(layout, i);
                const auto &defaults = i < actual.bottom_layers ? actual.bottom : actual.normal;
                for (const auto &[field, value] : defaults)
                    if (row.at(field) != value)
                        throw RuntimeError("Disabled source per-layer table conflicts with global defaults");
            }
        };
        check_rows(layout, chitubox_layer_parameters);
    }
    return actual;
}
} // namespace Slic3r::sla
