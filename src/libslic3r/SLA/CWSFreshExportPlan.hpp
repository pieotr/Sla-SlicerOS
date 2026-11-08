// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "CWSCommandCodec.hpp"
#include "LayerDocument.hpp"

namespace Slic3r::sla {
struct CWSFreshExportPlan {
    FreshLayerJob source;
    FreshLayerJob represented;
    size_t projected_time_fields=0;
    double maximum_time_change=0;
    std::string commands;
};

// Verify executable intent; native inferred globals are a separate projection.
inline void cws_require_command_intent(const FreshLayerJob &source,
                                       const std::vector<CWSCommandRow> &rows)
{
    using F=LayerParameter;using namespace cws_command_detail;
    source.validate(source.rows.size());
    if(rows.size()!=source.rows.size())throw RuntimeError("CWS command layer count differs from source");
    double previous=0,previous_grid=0;
    long double planned_position=0,rounding_bound=0;
    // One full neighbouring spacing conservatively bounds a rounded
    // subtraction/addition, including cancellation and binade boundaries.
    auto spacing=[](auto value) {
        using T=decltype(value);
        const auto inf=std::numeric_limits<T>::infinity();
        return std::max(std::abs(static_cast<long double>(std::nextafter(value,inf))-value),
                        std::abs(static_cast<long double>(std::nextafter(value,-inf))-value));
    };
    for(size_t i=0;i<rows.size();++i) {
        const auto &s=source.rows[i];const auto &actual=rows[i];
        auto p=[&](F f){return s.at(f);};
        auto mismatch=[&](){fail(i,"Commands","executed schedule differs from source");};
        auto mismatch_z=[&]() {
            std::ostringstream reason;reason.imbue(std::locale::classic());
            reason<<std::setprecision(std::numeric_limits<double>::max_digits10)
                <<"executed PositionZ "<<actual.position_z<<" differs from source "<<p(F::PositionZ);
            fail(i,"Commands",reason.str().c_str());
        };
        std::vector<CWSMove> lift,retract;
        auto move=[](std::vector<CWSMove> &moves,double distance,double speed){
            if(distance) moves.push_back({distance,speed});
        };
        const double delta=p(F::PositionZ)-previous,total=p(F::LiftDistance)+p(F::LiftDistance2);
        if(p(F::LiftDistance)>0) {
            move(lift,delta+p(F::LiftDistance),p(F::LiftSpeed));
            move(lift,p(F::LiftDistance2),p(F::LiftSpeed2));
        } else move(lift,delta+p(F::LiftDistance2),p(p(F::LiftDistance2)>0?F::LiftSpeed2:F::LiftSpeed));
        move(retract,-(total-p(F::RetractDistance2)),p(F::RetractSpeed));
        move(retract,-p(F::RetractDistance2),p(F::RetractSpeed2));
        auto same=[](const auto &a,const auto &b){
            if(a.size()!=b.size())return false;
            for(size_t k=0;k<a.size();++k)
                if(a[k].distance!=b[k].distance || a[k].speed!=b[k].speed)return false;
            return true;
        };
        if(!same(lift,actual.lift) || !same(retract,actual.retract) || actual.z_advance_commit!=(total==0))mismatch();
        // Reconstruct from the independently checked source move vectors,
        // in execution order. A supplied/modified Z must match that trajectory
        // exactly, rather than merely fitting a progressively wider tolerance.
        for(const auto *moves:{&lift,&retract})for(const auto &step:*moves) {
            planned_position+=step.distance;
            rounding_bound+=spacing(planned_position);
        }
        // delta, first advance, total lift and primary retract are the only
        // double operations used to construct the relative distances.
        rounding_bound+=spacing(delta)+spacing(lift.front().distance)+
                        spacing(total)+spacing(total-p(F::RetractDistance2));
        if(!std::isfinite(planned_position) || !std::isfinite(rounding_bound) ||
           actual.position_z!=double(planned_position) ||
           std::abs(planned_position-static_cast<long double>(p(F::PositionZ)))>rounding_bound)
            mismatch_z();
        // The reference's public height grid must still preserve each source
        // layer. Large-motion cancellation cannot justify a changed height.
        const double grid=std::round(double(float(actual.position_z))*1000.);
        const double source_grid=std::round(double(float(p(F::PositionZ)))*1000.);
        if(!std::isfinite(grid) || grid!=source_grid || grid<=previous_grid)mismatch_z();
        previous_grid=grid;
        const std::array<uint32_t,3> waits{milliseconds(p(F::WaitAfterLift),i,"WaitAfterLift"),
            milliseconds(p(F::WaitBefore),i,"WaitBefore"),milliseconds(p(F::WaitAfter),i,"WaitAfter")};
        const auto exposure=milliseconds(p(F::Exposure),i,"Exposure");
        if(actual.waits_ms!=waits || actual.exposure_ms!=exposure ||
            actual.pwm!=(exposure?unsigned(p(F::LightPWM)):0))mismatch();
        previous=p(F::PositionZ);
    }
}
inline CWSFreshExportPlan cws_fresh_export_plan(const LayerDocument &document)
{
    if(!document.fresh_job())throw RuntimeError("CWS preserving plan requires owned source intent");
    CWSFreshExportPlan result;result.source=*document.fresh_job();
    result.source.validate(document.layer_count());
    result.represented=result.source;
    // Native CWS delay words are uint32 milliseconds. Keep original doubles
    // separately; metadata, execution and readback use this explicit projection.
    auto project_times=[&](LayerParameters &values,size_t layer) {
        for(auto [field,name]:std::array<std::pair<LayerParameter,const char *>,4>{{
            {LayerParameter::Exposure,"Exposure"},{LayerParameter::WaitBefore,"WaitBefore"},
            {LayerParameter::WaitAfter,"WaitAfter"},{LayerParameter::WaitAfterLift,"WaitAfterLift"}}}) {
            const double original=values.at(field),milliseconds=original*1000.;
            const double rounded=std::round(milliseconds);
            if(!std::isfinite(milliseconds) || milliseconds<0 || milliseconds>UINT32_MAX ||
               (original>0 && rounded==0))
                cws_command_detail::fail(layer,name,"outside positive uint32 millisecond representation");
            const double native=rounded/1000.;
            const double change=std::abs(native-original);
            if(change>4*std::numeric_limits<double>::epsilon()*std::max(1.,original)) {
                ++result.projected_time_fields;
                result.maximum_time_change=std::max(result.maximum_time_change,change);
            }
            values[field]=native;
        }
    };
    project_times(result.represented.globals.normal,0);
    project_times(result.represented.globals.bottom,0);
    for(size_t i=0;i<result.represented.rows.size();++i)project_times(result.represented.rows[i],i);
    const auto &g=result.source.geometry;
    if(g.columns>UINT16_MAX || g.rows>UINT16_MAX || !std::isfinite(float(g.width_mm)) ||
        !std::isfinite(float(g.height_mm)) || float(g.width_mm)<=0 || float(g.height_mm)<=0)
        throw RuntimeError("CWS source geometry exceeds native resolution or physical size range");
    // Fields actually stored in the profile/header must fit their native type.
    for(const auto *defaults:{&result.represented.globals.normal,&result.represented.globals.bottom}) {
        for(const auto f:{LayerParameter::Exposure,LayerParameter::WaitBefore,LayerParameter::WaitAfterLift})
            cws_command_detail::milliseconds(defaults->at(f),0,"HeaderTime");
        for(const auto f:{LayerParameter::LiftDistance,LayerParameter::LiftSpeed,LayerParameter::RetractSpeed})
            if(!std::isfinite(float(defaults->at(f))))throw RuntimeError("CWS header motion exceeds native float range");
    }
    if(!std::isfinite(float(result.source.globals.layer_height)) || float(result.source.globals.layer_height)<=0)
        throw RuntimeError("CWS nominal height exceeds positive native float range");
    result.commands=cws_encode_command_rows(result.represented);
    cws_require_command_intent(result.represented,cws_decode_command_rows(result.commands));
    return result;
}
} // namespace Slic3r::sla
