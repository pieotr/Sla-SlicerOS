// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerParameters.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::sla {
// The extended record stores TOTAL lift, not the first phase. V3's remaining
// phase/wait slots are opaque; expose only fields used by its decoder/firmware.
inline LayerParameterCapabilities chitubox_parameter_capabilities(unsigned version)
{
    using F=LayerParameter;
    LayerParameterCapabilities result{{F::PositionZ,"Z position","mm",true},
        {F::Exposure,"Exposure","s",true},{F::LightOff,"Light-off delay","s",true},
        {F::LiftDistance,"Lift distance","mm",true},{F::LiftSpeed,"Lift speed","mm/min",true},
        {F::RetractSpeed,"Retract speed","mm/min",true},{F::LightPWM,"Light PWM","0–255",true}};
    if(version>=4) for(auto capability:LayerParameterCapabilities{
        {F::LiftDistance2,"Lift distance 2","mm",true},{F::LiftSpeed2,"Lift speed 2","mm/min",true},
        {F::RetractDistance2,"Retract distance 2","mm",true},{F::RetractSpeed2,"Retract speed 2","mm/min",true},
        {F::WaitBefore,"Wait before exposure","s",true},{F::WaitAfter,"Wait after exposure","s",true},
        {F::WaitAfterLift,"Wait after lift","s",true}}) result.push_back(capability);
    return result;
}
inline LayerParameters chitubox_layer_parameters(const ChituboxLayerLayout &layout,size_t layer)
{
    using F=LayerParameter;const auto at=layout.record(layer),ex=layout.extended_record(layer);
    LayerParameters result{{F::PositionZ,layout.number(at)},{F::Exposure,layout.number(at+4)},
        {F::LightOff,layout.number(at+8)},{F::LiftDistance,layout.number(ex+40)},
        {F::LiftSpeed,layout.number(ex+44)},{F::RetractSpeed,layout.number(ex+56)},
        {F::LightPWM,layout.number(ex+80)}};
    if(layout.version>=4) {
        result[F::LiftDistance]-=layout.number(ex+48);
        for(auto [field,offset]:std::map<F,size_t>{{F::LiftDistance2,48},{F::LiftSpeed2,52},
            {F::RetractDistance2,60},{F::RetractSpeed2,64},{F::WaitAfter,68},
            {F::WaitAfterLift,72},{F::WaitBefore,76}}) result[field]=layout.number(ex+offset);
    }
    for(auto [field,value]:result) if(!std::isfinite(value) || value<0)
        throw RuntimeError("Invalid Chitubox layer parameter or phase geometry");
    return result;
}
inline void validate_chitubox_parameters(const LayerParameters &values)
{
    using F=LayerParameter;
    auto value=[&](F field){auto i=values.find(field);return i==values.end()?0.:i->second;};
    const auto pwm=value(F::LightPWM);
    if(pwm>255 || pwm!=std::floor(pwm))throw RuntimeError("Light PWM must be an integer from 0 to 255");
    const auto total=value(F::LiftDistance)+value(F::LiftDistance2);
    if(total>std::numeric_limits<float>::max())throw RuntimeError("Total lift distance exceeds the format range");
    if(value(F::RetractDistance2)>total)throw RuntimeError("Second retract distance exceeds total lift distance");
    for(auto [distance,speed]:std::vector<std::pair<double,F>>{
        {value(F::LiftDistance),F::LiftSpeed},{value(F::LiftDistance2),F::LiftSpeed2},
        {total-value(F::RetractDistance2),F::RetractSpeed},{value(F::RetractDistance2),F::RetractSpeed2}})
        if(distance>0 && value(speed)<=0)throw RuntimeError("A moving phase requires a positive speed");
}
// Follow the local UVtools Layer/LightOffDelayC estimate: motion is rounded to
// hundredths, and light-off is a minimum cycle delay, not an additional wait.
// Preserve the source's unknown fixed overhead by applying only the delta.
inline double chitubox_layer_seconds(const LayerParameters &values)
{
    using F=LayerParameter;
    auto value=[&](F field){auto i=values.find(field);return i==values.end()?0.:i->second;};
    double motor=0;
    auto phase=[&](double distance,F speed){if(distance>0 && value(speed)>0)motor+=60*distance/value(speed);};
    phase(value(F::LiftDistance),F::LiftSpeed);phase(value(F::LiftDistance2),F::LiftSpeed2);
    phase(value(F::RetractDistance2),F::RetractSpeed2);
    phase(value(F::LiftDistance)+value(F::LiftDistance2)-value(F::RetractDistance2),F::RetractSpeed);
    motor=std::nearbyint(motor*100)/100;
    return value(F::Exposure)+value(F::WaitBefore)+value(F::WaitAfter)+value(F::WaitAfterLift)+std::max(motor,value(F::LightOff));
}
}
