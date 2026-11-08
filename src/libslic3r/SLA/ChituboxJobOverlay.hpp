// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerJobParameters.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include <cstring>

namespace Slic3r::sla {
// Destination constraints must never narrow the format-neutral SLA model.
void validate_chitubox_job(const LayerJobParameters &job, size_t layer_count);
void validate_chitubox_v3_job(const LayerJobParameters &job, size_t layer_count);
// Temporary writer baseline only; the typed overlay restores native precision.
void apply_chitubox_container_baseline(const LayerJobParameters &job, size_t layer_count, DynamicPrintConfig &config);

// These writes change typed global defaults only. Raster pointers, statistics,
// opaque metadata, signatures and the per-layer table retain their ownership.
inline void job_u32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = uint8_t(value >> (8 * i));
}
inline void job_float(std::vector<uint8_t> &bytes, size_t at, double value)
{
    const float native = float(value);
    if (!std::isfinite(native)) throw RuntimeError("Native global float overflow");
    uint32_t bits; std::memcpy(&bits, &native, sizeof(bits)); job_u32(bytes, at, bits);
}
inline void job_pwm(std::vector<uint8_t> &bytes, size_t at, double value)
{
    const auto native = uint16_t(value);
    bytes.at(at) = uint8_t(native); bytes.at(at + 1) = uint8_t(native >> 8);
}
inline void overlay_encrypted_job(std::vector<uint8_t> &settings, const LayerJobParameters &job)
{
    using F = LayerParameter;
    job_float(settings, 36, job.layer_height);
    job_u32(settings, 52, uint32_t(job.bottom_layers));
    job_u32(settings, 188, uint32_t(job.transition_layers));
    for (bool bottom : {false, true}) {
        const auto &row = bottom ? job.bottom : job.normal;
        for (auto [field, offset] : std::map<F, size_t>{{F::Exposure,bottom ? 44u : 40u},
                {F::LiftDistance2,bottom ? 132u : 140u}, {F::LiftSpeed,bottom ? 88u : 96u},
                {F::LiftSpeed2,bottom ? 136u : 144u}, {F::RetractSpeed,bottom ? 192u : 100u},
                {F::RetractSpeed2,bottom ? 196u : 152u}, {F::RetractDistance2,bottom ? 228u : 148u},
                {F::LightOff,bottom ? 116u : 48u}})
            job_float(settings, offset, row.at(field));
        job_float(settings, bottom ? 84 : 92, row.at(F::LiftDistance) + row.at(F::LiftDistance2));
        job_pwm(settings, bottom ? 126 : 124, row.at(F::LightPWM));
    }
    for (auto [field, offsets] : std::map<F, std::vector<size_t>>{
            {F::WaitBefore,{180,216}}, {F::WaitAfter,{224}}, {F::WaitAfterLift,{156,184,220}}})
        for (size_t offset : offsets) job_float(settings, offset, job.normal.at(field));
    // AES v4 and v5 both use the 0x40 enabled-table mode in UVtools.
    settings.at(171) = 0x40;
}
inline void overlay_chitubox_job(std::vector<uint8_t> &bytes, const ChituboxLayerLayout &layout,
                                const LayerJobParameters &job)
{
    using F = LayerParameter;
    const size_t params = layout.parameters, slicer = layout.integer(104);
    if (layout.version == 3) {
        // V3 has no v4 section. Update its owned defaults while retaining
        // opaque phase/wait slots and all raster/preview addresses.
        job_float(bytes, 32, job.layer_height);
        job_u32(bytes, 48, uint32_t(job.bottom_layers));
        job_u32(bytes, params + 40, uint32_t(job.bottom_layers));
        job_u32(bytes, slicer + 60, uint32_t(job.transition_layers));
        for (bool bottom : {false, true}) {
            const auto &row = bottom ? job.bottom : job.normal;
            job_float(bytes, bottom ? 40 : 36, row.at(F::Exposure));
            job_float(bytes, params + (bottom ? 0 : 8), row.at(F::LiftDistance));
            job_float(bytes, params + (bottom ? 4 : 12), row.at(F::LiftSpeed));
            job_float(bytes, params + (bottom ? 32 : 36), row.at(F::LightOff));
            job_pwm(bytes, bottom ? 98 : 96, row.at(F::LightPWM));
        }
        job_float(bytes, params + 16, job.normal.at(F::RetractSpeed));
        job_float(bytes, 44, job.normal.at(F::LightOff));
        bytes.at(slicer + 39) = 0x30;
        return;
    }
    const size_t v4 = layout.integer(slicer + 64);
    job_float(bytes, 32, job.layer_height);
    job_u32(bytes, 48, uint32_t(job.bottom_layers));
    job_u32(bytes, params + 40, uint32_t(job.bottom_layers));
    job_u32(bytes, slicer + 60, uint32_t(job.transition_layers));
    for (bool bottom : {false, true}) {
        const auto &row = bottom ? job.bottom : job.normal;
        job_float(bytes, bottom ? 40 : 36, row.at(F::Exposure));
        job_float(bytes, params + (bottom ? 0 : 8), row.at(F::LiftDistance) + row.at(F::LiftDistance2));
        for (auto [field, offset] : std::map<F,size_t>{
                {F::LiftSpeed,params + (bottom ? 4u : 12u)},
                {F::LiftDistance2,slicer + (bottom ? 0u : 8u)},
                {F::LiftSpeed2,slicer + (bottom ? 4u : 12u)},
                {F::RetractSpeed,bottom ? v4 : params + 16},
                {F::RetractSpeed2,bottom ? v4 + 4 : slicer + 20},
                {F::RetractDistance2,bottom ? v4 + 36 : slicer + 16},
                {F::LightOff,params + (bottom ? 32u : 36u)}})
            job_float(bytes, offset, row.at(field));
        job_pwm(bytes, bottom ? 98 : 96, row.at(F::LightPWM));
    }
    job_float(bytes, 44, job.normal.at(F::LightOff));
    for (auto [field, offsets] : std::map<F,std::vector<size_t>>{
            {F::WaitBefore,{slicer+52,v4+24}}, {F::WaitAfter,{v4+32}},
            {F::WaitAfterLift,{slicer+24,slicer+56,v4+28}}})
        for (size_t offset : offsets) job_float(bytes, offset, job.normal.at(field));
    bytes.at(slicer + 39) = uint8_t(layout.version * 0x10);
}
} // namespace Slic3r::sla
