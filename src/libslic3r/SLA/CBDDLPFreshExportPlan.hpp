// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include "FreshLayerMaterialPricing.hpp"
#include <array>
#include <cmath>
#include <string>

namespace Slic3r::sla {
// CBDDLP/Photon v2 stores global single-stage motion and one Z/exposure/
// light-off row per layer/AA plane. It has no v4 per-layer motion record.
struct CBDDLPFreshRow { float z=0, exposure=0, light_off=0; };
struct CBDDLPFreshExportPlan {
    FreshLayerJob source;
    LayerRasterGeometry geometry;
    std::vector<CBDDLPFreshRow> rows;
    std::array<float,2> lift_height{}, lift_speed{}, default_light_off{}; // normal,bottom
    float retract_speed=0, nominal_height=0;
    uint32_t bottom_layers=0;
    std::array<uint16_t,2> pwm{};
    std::array<float,2> default_exposure{};
    uint32_t estimated_print_time=0;
};
inline CBDDLPFreshExportPlan cbddlp_fresh_export_plan(const LayerDocument &document)
{
    using F=LayerParameter;
    if(!document.fresh_job())throw RuntimeError("CBDDLP preserving export requires owned source intent");
    CBDDLPFreshExportPlan result;result.source=*document.fresh_job();
    result.source.validate(document.layer_count());
    auto fail=[](const std::string &scope,const char *field,const char *reason) {
        throw RuntimeError("CBDDLP cannot preserve "+scope+"."+field+": "+reason);
    };
    auto native=[&](double value,const std::string &scope,const char *field) {
        const float projected=float(value);
        if(!std::isfinite(projected) || (value>0 && projected<=0))
            fail(scope,field,"outside positive native float range");
        return projected;
    };
    auto supported=[&](const LayerParameters &row,const std::string &scope) {
        for(auto [field,name]:std::array<std::pair<F,const char *>,4>{{
            {F::LiftDistance2,"LiftDistance2"},{F::RetractDistance2,"RetractDistance2"},
            {F::WaitAfter,"WaitAfter"},{F::WaitAfterLift,"WaitAfterLift"}}})
            if(row.at(field)!=0)fail(scope,name,"no field in the v2 single-stage schema");
    };
    result.geometry=result.source.geometry;
    result.geometry.width_mm=native(result.geometry.width_mm,"geometry","Width");
    result.geometry.height_mm=native(result.geometry.height_mm,"geometry","Height");
    result.nominal_height=native(result.source.globals.layer_height,"globals","LayerHeight");
    result.bottom_layers=result.source.globals.bottom_layers;
    result.retract_speed=native(result.source.globals.normal.at(F::RetractSpeed),"normal","RetractSpeed");
    // Choose the shared field from executed returns, not an unused normal
    // template in an all-bottom job. Unused source defaults stay in project.
    bool active_return=false;
    for(const auto &row:result.source.rows)if(row.at(F::LiftDistance)>0) {
        const float speed=native(row.at(F::RetractSpeed),"active rows","RetractSpeed");
        if(active_return && speed!=result.retract_speed)
            fail("active rows","RetractSpeed","the v2 header has only one return speed");
        result.retract_speed=speed;active_return=true;
    }
    for(size_t region=0;region<2;++region) {
        const auto &group=region?result.source.globals.bottom:result.source.globals.normal;
        const std::string scope=region?"bottom":"normal";supported(group,scope);
        result.lift_height[region]=native(group.at(F::LiftDistance),scope,"LiftDistance");
        result.lift_speed[region]=native(group.at(F::LiftSpeed),scope,"LiftSpeed");
        result.pwm[region]=uint16_t(group.at(F::LightPWM));
        result.default_exposure[region]=native(group.at(F::Exposure),scope,"Exposure");
        const bool used=region?result.bottom_layers>0:result.bottom_layers<document.layer_count();
        if(used && result.lift_height[region]>0 && native(group.at(F::RetractSpeed),scope,"RetractSpeed")!=result.retract_speed)
            fail(scope,"RetractSpeed","normal and bottom share one active return speed");
    }
    auto period=[&](const LayerParameters &row,size_t region,const std::string &scope) {
        const double height=result.lift_height[region];
        const double motor=height>0 ? height*60./result.lift_speed[region]+height*60./result.retract_speed : 0.;
        return native(std::max(motor,double(native(row.at(F::LightOff),scope,"LightOff")))+
            row.at(F::WaitBefore),scope,"LightOffWithWaitBefore");
    };
    for(size_t region=0;region<2;++region) {
        const auto &group=region?result.source.globals.bottom:result.source.globals.normal;
        result.default_light_off[region]=period(group,region,region?"bottom":"normal");
    }
    float previous=0;
    for(size_t i=0;i<result.source.rows.size();++i) {
        const auto &row=result.source.rows[i];const size_t region=i<result.bottom_layers?1:0;
        const std::string scope="layer "+std::to_string(i+1);supported(row,scope);
        if(native(row.at(F::LiftDistance),scope,"LiftDistance")!=result.lift_height[region])
            fail(scope,"LiftDistance","per-layer motion differs from fixed region header");
        if(result.lift_height[region]>0 && (native(row.at(F::LiftSpeed),scope,"LiftSpeed")!=result.lift_speed[region] ||
            native(row.at(F::RetractSpeed),scope,"RetractSpeed")!=result.retract_speed))
            fail(scope,"MotionSpeed","per-layer active speed differs from fixed header");
        if(row.at(F::LightPWM)!=result.pwm[region])fail(scope,"LightPWM","per-layer power differs from fixed region header");
        const float z=native(row.at(F::PositionZ),scope,"PositionZ");
        if(z<=previous)fail(scope,"PositionZ","native float conversion collapses layer positions");
        result.rows.push_back({z,native(row.at(F::Exposure),scope,"Exposure"),period(row,region,scope)});
        previous=z;
    }
    double seconds=300.; // UVtools' file preparation estimate, not a cure delay.
    for(const auto &row:result.rows)seconds+=double(row.exposure)+row.light_off;
    if(!std::isfinite(seconds) || seconds>UINT32_MAX)throw RuntimeError("CBDDLP represented print time exceeds uint32 range");
    result.estimated_print_time=uint32_t(seconds);
    return result;
}
// V2 stores totals, not a density record. Validate the authoritative source
// profile in double; only the independently stored totals require float32.
inline std::array<float,3> cbddlp_material_totals(double ml,double density,double cost,double bottle_ml)
{
    if(!std::isfinite(ml) || ml<0 || !std::isfinite(density) || density<0 ||
        !std::isfinite(cost) || cost<0 || !std::isfinite(bottle_ml) || bottle_ml<=0)
        throw RuntimeError("CBDDLP fresh material profile must be finite, nonnegative and have positive bottle volume");
    const std::array<double,3> exact{ml,ml*density,ml*cost/bottle_ml};
    std::array<float,3> values{};
    for(size_t i=0;i<3;++i) {
        values[i]=float(exact[i]);
        if(!std::isfinite(values[i]) || values[i]<0 || (exact[i]>0 && values[i]==0))
            throw RuntimeError("CBDDLP fresh material total exceeds native float range");
    }
    return values;
}
} // namespace Slic3r::sla
