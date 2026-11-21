// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include "CBDDLPFreshExportPlan.hpp"

namespace Slic3r::sla {
struct AnycubicUnavailableParameter {
    LayerParameter field;
    const char *name;
    const char *config_suffix;
};
// Shared by early settings validation and the full export plan. LightOff is
// neutral metadata; generated jobs use explicit waits and set it to zero.
inline constexpr std::array<AnycubicUnavailableParameter,4> anycubic_unavailable_parameters{{
    {LayerParameter::RetractDistance2,"RetractDistance2","retract_height2"},
    {LayerParameter::WaitAfter,"WaitAfter","wait_after_cure"},
    {LayerParameter::WaitAfterLift,"WaitAfterLift","wait_after_lift"},
    {LayerParameter::LightOff,"LightOff",nullptr}
}};
struct AnycubicFreshRow { float height=0, exposure=0, lift=0, speed_mms=0; };
struct AnycubicFreshExportPlan {
    FreshLayerJob source;
    LayerJobParameters globals;
    LayerRasterGeometry geometry;
    std::vector<AnycubicFreshRow> rows;
    float nominal_height=0, wait_before=0;
    uint32_t bottom_layers=0, transition_layers=0, estimated_print_time=0;
};
inline AnycubicFreshExportPlan anycubic_fresh_export_plan(const LayerDocument &document)
{
    using F=LayerParameter;
    if(!document.fresh_job())throw RuntimeError("Modern Anycubic preserving export requires owned source intent");
    AnycubicFreshExportPlan plan;plan.source=*document.fresh_job();plan.source.validate(document.layer_count());
    plan.globals=plan.source.globals;
    auto fail=[](const std::string &scope,const char *field,const char *why) {
        throw RuntimeError("Modern Anycubic cannot preserve "+scope+"."+field+": "+why);
    };
    auto native=[&](double value,const std::string &scope,const char *field) {
        const float out=float(value);
        if(!std::isfinite(out) || (value>0 && out<=0))fail(scope,field,"outside positive native float range");
        return out;
    };
    auto supported=[&](const LayerParameters &row,const std::string &scope) {
        for(const auto &parameter:anycubic_unavailable_parameters)
            if(row.at(parameter.field)!=0)fail(scope,parameter.name,"no independent field in the modern schema");
        if(row.at(F::LightPWM)!=255)fail(scope,"LightPWM","no custom power field");
    };
    plan.geometry=plan.source.geometry;
    plan.geometry.width_mm=native(plan.geometry.width_mm,"geometry","Width");
    plan.geometry.height_mm=native(plan.geometry.height_mm,"geometry","Height");
    if(plan.geometry.columns>UINT16_MAX || plan.geometry.rows>UINT16_MAX)
        throw RuntimeError("Modern Anycubic machine resolution exceeds uint16");
    plan.nominal_height=native(plan.source.globals.layer_height,"globals","LayerHeight");
    plan.bottom_layers=plan.source.globals.bottom_layers;plan.transition_layers=plan.source.globals.transition_layers;
    for(auto *group:{&plan.globals.normal,&plan.globals.bottom}) {
        supported(*group,"defaults");
        for(F field:{F::Exposure,F::LiftDistance,F::LiftDistance2,F::WaitBefore})
            (*group)[field]=native(group->at(field),"defaults","Parameter");
        for(F field:{F::LiftSpeed,F::LiftSpeed2,F::RetractSpeed,F::RetractSpeed2})
            (*group)[field]=double(native(group->at(field)/60.,"defaults","Speed"))*60.;
    }
    const auto &normal=plan.globals.normal;const auto &bottom=plan.globals.bottom;
    plan.wait_before=float(normal.at(F::WaitBefore));
    if(bottom.at(F::WaitBefore)!=normal.at(F::WaitBefore))
        fail("defaults","WaitBefore","normal and bottom share one wait field");
    double previous=0,seconds=300,native_z=0,previous_public_z=0;
    for(size_t i=0;i<plan.source.rows.size();++i) {
        const auto &row=plan.source.rows[i];const std::string scope="layer "+std::to_string(i+1);supported(row,scope);
        const auto &group=i<plan.bottom_layers?bottom:normal;
        for(F field:{F::LiftDistance2,F::WaitBefore})
            if(double(native(row.at(field),scope,"Global phase"))!=group.at(field))
                fail(scope,"GlobalPhase","per-row override has no field");
        for(F field:{F::LiftSpeed2,F::RetractSpeed,F::RetractSpeed2})
            if(double(native(row.at(field)/60.,scope,"Global speed"))*60.!=group.at(field))
                fail(scope,"GlobalSpeed","per-row override has no field");
        const float height=native(row.at(F::PositionZ)-previous,scope,"LayerHeight");
        native_z+=height;
        const float position=native(native_z,scope,"PositionZ");
        // The reference decoder accumulates relative heights and exposes Z
        // on a 0.001 mm grid. A positive source delta must not disappear.
        const double public_z=std::round(double(position)*1000.)/1000.;
        if(public_z<=previous_public_z)
            fail(scope,"PositionZ","native height accumulation collapses positions on the 0.001 mm grid");
        previous_public_z=public_z;
        // LayerDef.SetFrom/CopyTo stores FIRST lift, while HEADER is total.
        // Keep this separate from imported legacy total-lift interpretation.
        plan.rows.push_back({height,native(row.at(F::Exposure),scope,"Exposure"),
            native(row.at(F::LiftDistance),scope,"LiftDistance"),native(row.at(F::LiftSpeed)/60.,scope,"LiftSpeed")});
        previous=row.at(F::PositionZ);
        seconds+=row.at(F::Exposure)+row.at(F::WaitBefore);
        for(auto [d,s]:std::array<std::pair<double,double>,3>{{{row.at(F::LiftDistance),row.at(F::LiftSpeed)},
                {row.at(F::LiftDistance2),row.at(F::LiftSpeed2)},
                {row.at(F::LiftDistance)+row.at(F::LiftDistance2),row.at(F::RetractSpeed)}}})
            if(d>0)seconds+=d*60./s;
    }
    plan.estimated_print_time=std::isfinite(seconds) && seconds<=UINT32_MAX ? uint32_t(seconds) :
        throw RuntimeError("Modern Anycubic estimated time exceeds uint32");
    return plan;
}
} // namespace Slic3r::sla
