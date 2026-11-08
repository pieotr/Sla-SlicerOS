// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChituboxJobOverlay.hpp"
#include "ChituboxLayerParameters.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::sla {
void validate_chitubox_job(const LayerJobParameters &job, size_t layer_count)
{
    using F = LayerParameter;
    job.validate(layer_count);
    if (!std::isfinite(float(job.layer_height)) || job.layer_height < .001 ||
        float(job.layer_height) != float(std::round(job.layer_height * 1000.) / 1000.))
        throw RuntimeError("CTB nominal layer height must be a supported multiple of 0.001 mm");
    if (job.bottom_layers > UINT16_MAX || job.transition_layers > UINT16_MAX)
        throw RuntimeError("CTB bottom or transition count exceeds the destination decoder range");
    for (const auto *row : {&job.normal, &job.bottom}) {
        for (const auto &[field, value] : *row)
            if (!std::isfinite(float(value)))
                throw RuntimeError("CTB cannot represent a nominal SLA parameter as a native float");
        validate_chitubox_parameters(*row);
    }
    for (const auto field : {F::WaitBefore, F::WaitAfter, F::WaitAfterLift})
        if (job.normal.at(field) != job.bottom.at(field))
            throw RuntimeError("CTB has shared global waits and cannot preserve different bottom defaults");
}

void validate_chitubox_v3_job(const LayerJobParameters &job, size_t layer_count)
{
    validate_chitubox_job(job, layer_count);
    using F = LayerParameter;
    for (const auto *row : {&job.normal, &job.bottom})
        for (const auto field : {F::LiftDistance2, F::LiftSpeed2, F::RetractDistance2,
                F::RetractSpeed2, F::WaitBefore, F::WaitAfter, F::WaitAfterLift})
            if (row->at(field) != 0)
                throw RuntimeError("CTB v3 cannot preserve second-phase motion or explicit waits");
    if (job.normal.at(F::RetractSpeed) != job.bottom.at(F::RetractSpeed))
        throw RuntimeError("CTB v3 requires the same normal and bottom retract speed");
}

void apply_chitubox_container_baseline(const LayerJobParameters &job, size_t layer_count, DynamicPrintConfig &config)
{
    validate_chitubox_job(job, layer_count);
    auto baseline = job;
    baseline.layer_height = std::round(job.layer_height * 1000.) / 1000.;
    for (auto *row : {&baseline.normal, &baseline.bottom}) {
        for (auto &[field, value] : *row) {
            const bool speed = field == LayerParameter::LiftSpeed || field == LayerParameter::LiftSpeed2 ||
                field == LayerParameter::RetractSpeed || field == LayerParameter::RetractSpeed2;
            value = speed ? std::ceil(value * 100.) / 100. : std::round(value * 100.) / 100.;
        }
        (*row)[LayerParameter::RetractDistance2] = std::min(row->at(LayerParameter::RetractDistance2),
            row->at(LayerParameter::LiftDistance) + row->at(LayerParameter::LiftDistance2));
    }
    baseline.apply_to(config);
    config.set("initial_layer_height", baseline.layer_height);
}

} // namespace Slic3r::sla
