// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerParameters.hpp"
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
#include <cmath>

namespace Slic3r::sla {
// Anycubic's layer LiftHeight is the stored distance: native writer and the
// original global setters store total lift there. Original CopyTo exposes it
// unchanged, alongside EXTRA's second phase. Never add that phase again.
// EXTRA/waits/retract are GLOBAL context, not writable per-layer fields.
inline LayerParameters anycubic_layer_parameters(const AnycubicLayerLayout &layout,size_t layer,double z)
{
    using F=LayerParameter;const auto at=layout.record(layer);
    LayerParameters values{{F::PositionZ,z},{F::Exposure,layout.number(at+16)},
        {F::LiftDistance,layout.number(at+8)},{F::LiftSpeed,double(layout.number(at+12))*60.},
        {F::WaitBefore,layout.number(layout.header+28)},
        {F::RetractSpeed,double(layout.number(layout.header+48))*60.}};
    if(layout.version>=516 && layout.integer(40)) {
        const auto extra=layout.integer(40);layout.named(extra,"EXTRA",72);
        const bool advanced=layout.integer(layout.header+12)>=84 && layout.integer(layout.header+96)!=0;
        const bool bottom=double(layer)<layout.number(layout.header+36);
        if(advanced) {
            values[F::LiftDistance2]=layout.number(extra+(bottom?32:60));
            values[F::LiftSpeed2]=double(layout.number(extra+(bottom?36:64)))*60.;
            values[F::RetractSpeed]=double(layout.number(extra+(bottom?40:68)))*60.;
            values[F::RetractSpeed2]=double(layout.number(extra+(bottom?28:56)))*60.;
        } else {
            // Original decode resets EXTRA to HEADER in basic mode.
            values[F::LiftDistance2]=0;
            values[F::LiftSpeed2]=double(layout.number(layout.header+44))*60.;
            values[F::RetractSpeed2]=values.at(F::RetractSpeed);
        }
    }
    for(auto [field,value]:values)if(!std::isfinite(value) || value<0)throw RuntimeError("Invalid Anycubic motion parameter");
    return values;
}
inline LayerParameterCapabilities anycubic_parameter_capabilities(const LayerParameters &values)
{
    using F=LayerParameter;
    LayerParameterCapabilities capabilities{{F::PositionZ,"Z position","mm",true},
        {F::Exposure,"Exposure","s",true},{F::LiftDistance,"Stored lift distance","mm",true},
        {F::LiftSpeed,"Lift speed","mm/min",true}};
    for(auto capability:LayerParameterCapabilities{{F::WaitBefore,"Global wait before exposure","s",false},
        {F::RetractSpeed,"Global retract speed","mm/min",false},
        {F::LiftDistance2,"Global lift distance 2","mm",false},{F::LiftSpeed2,"Global lift speed 2","mm/min",false},
        {F::RetractSpeed2,"Global retract speed 2","mm/min",false}})
        if(values.count(capability.field))capabilities.push_back(capability);
    return capabilities;
}
inline void validate_anycubic_motion(const LayerParameters &values)
{
    using F=LayerParameter;const auto distance=values.at(F::LiftDistance);
    const auto second=values.count(F::LiftDistance2)?values.at(F::LiftDistance2):0.;
    if(distance<second)throw RuntimeError("Stored lift distance must include the global second phase");
    if(distance>second && values.at(F::LiftSpeed)<=0)throw RuntimeError("A moving lift phase requires a positive speed");
    if(second>0 && values.at(F::LiftSpeed2)<=0)throw RuntimeError("Global second lift phase requires a positive speed");
    if(distance>0 && values.at(F::RetractSpeed)<=0)throw RuntimeError("Global retract movement requires a positive speed");
}
inline double anycubic_layer_seconds(const LayerParameters &values)
{
    using F=LayerParameter;
    const auto distance=values.at(F::LiftDistance),second=values.count(F::LiftDistance2)?values.at(F::LiftDistance2):0.;
    double motor=0;
    auto phase=[&](double d,double speed){if(d>0 && speed>0)motor+=60*d/speed;};
    phase(distance-second,values.at(F::LiftSpeed));
    if(second>0)phase(second,values.at(F::LiftSpeed2));
    // The schema has retract speed2, but no retract-height2; retain its bytes
    // without inventing a second retract distance or a per-layer override.
    phase(distance,values.at(F::RetractSpeed));
    motor=std::nearbyint(motor*100)/100;
    return values.at(F::Exposure)+values.at(F::WaitBefore)+motor;
}
}
