// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include "ExposurePositionSchedule.hpp"
#include "libslic3r/Exception.hpp"
#include <cmath>

namespace Slic3r::sla {
struct UVJFreshExportPlan {
    FreshLayerJob source;
    LayerJobParameters globals;
    LayerRasterGeometry geometry;
    std::vector<LayerParameters> rows;
};
inline UVJFreshExportPlan uvj_fresh_export_plan(const LayerDocument &document)
{
    if(!document.fresh_job())throw RuntimeError("Owned UVJ requires source intentions");
    UVJFreshExportPlan plan;plan.source=*document.fresh_job();plan.source.validate(document.layer_count());
    auto native=[](double value) {
        const float result=float(value);
        if(!std::isfinite(result) || (value>0 && result<=0))
            throw RuntimeError("UVJ parameter exceeds positive native float range");
        return double(result);
    };
    auto row=[&](LayerParameters value) {
        const double original_primary=value.at(LayerParameter::LiftDistance)+value.at(LayerParameter::LiftDistance2)-
            value.at(LayerParameter::RetractDistance2);
        for(auto &[field,number]:value)number=native(number);
        const double total=value.at(LayerParameter::LiftDistance)+value.at(LayerParameter::LiftDistance2);
        const float primary=float(total-value.at(LayerParameter::RetractDistance2));
        if(!std::isfinite(float(total)) || !std::isfinite(primary) || (original_primary>0 && primary<=0))
            throw RuntimeError("UVJ native lift/retract overflows or collapses a moving phase");
        if(value.at(LayerParameter::RetractDistance2)>
           value.at(LayerParameter::LiftDistance)+value.at(LayerParameter::LiftDistance2))
            throw RuntimeError("UVJ native retract exceeds total lift");
        return value;
    };
    plan.geometry=plan.source.geometry;
    plan.geometry.width_mm=native(plan.geometry.width_mm);plan.geometry.height_mm=native(plan.geometry.height_mm);
    plan.globals=plan.source.globals;
    if(plan.globals.bottom_layers>UINT16_MAX)throw RuntimeError("UVJ bottom count exceeds uint16");
    plan.globals.layer_height=native(plan.globals.layer_height);
    plan.globals.normal=row(plan.globals.normal);plan.globals.bottom=row(plan.globals.bottom);
    ExposurePositionProjectionValidator positions(plan.source.position_policy, .001);
    for(const auto &original:plan.source.rows) {
        auto projected=row(original);const double z=projected.at(LayerParameter::PositionZ);
        positions.append(original.at(LayerParameter::PositionZ), z);
        plan.rows.push_back(std::move(projected));
    }
    return plan;
}
} // namespace Slic3r::sla
