// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ChituboxLayerParameters.hpp"
#include "libslic3r/Format/CTBEncryptedLayerLayout.hpp"
#include "CTBEncryptedMotion.hpp"

namespace Slic3r::sla {
inline bool ctb_encrypted_tilting_vat(const CTBEncryptedLayerLayout &layout)
{
    const auto address=layout.setting(160),size=layout.setting(164);
    const std::string machine(size?reinterpret_cast<const char *>(layout.bytes.data()+address):"",size);
    // Reference uses the public FIRST lift phase, not stored total or bottom
    // speed. Float subtraction matches CTBEncryptedFile.LiftHeight semantics.
    const float first = std::max(layout.parameter(92) - layout.parameter(140), 0.f);
    return ctb_encrypted_tilting_motion(machine, layout.parameter(36), first, layout.parameter(96));
}
inline LayerParameters ctb_encrypted_layer_parameters(const CTBEncryptedLayerLayout &layout,size_t layer)
{
    using F=LayerParameter;const auto at=layout.record(layer);
    LayerParameters result;
    for(auto [field,offset]:std::map<F,size_t>{{F::PositionZ,4},{F::Exposure,8},{F::LightOff,12},
        {F::LiftDistance,40},{F::LiftSpeed,44},{F::LiftDistance2,48},{F::LiftSpeed2,52},
        {F::RetractSpeed,56},{F::RetractDistance2,60},{F::RetractSpeed2,64},
        {F::WaitAfter,68},{F::WaitAfterLift,72},{F::WaitBefore,76},{F::LightPWM,80}})
        result[field]=layout.number(at+offset);
    result[F::LiftDistance]-=result.at(F::LiftDistance2);
    if(result.at(F::LiftDistance)<0)throw RuntimeError("Invalid CTBEncrypted lift phase geometry");
    return result;
}
inline LayerParameterCapabilities ctb_encrypted_parameter_capabilities(const CTBEncryptedLayerLayout &layout)
{
    auto capabilities=chitubox_parameter_capabilities(4);
    if(ctb_encrypted_tilting_vat(layout)) for(auto &capability:capabilities) {
        using F=LayerParameter;
        switch(capability.field) {
            case F::LiftDistance:case F::LiftSpeed:case F::LiftDistance2:case F::LiftSpeed2:
            case F::RetractSpeed:case F::RetractDistance2:case F::RetractSpeed2:capability.editable=false;break;
            default:break;
        }
    }
    return capabilities;
}
inline double ctb_encrypted_layer_seconds(const LayerParameters &values,bool tilting)
{
    if(!tilting)return chitubox_layer_seconds(values);
    using F=LayerParameter;
    return values.at(F::Exposure)+values.at(F::WaitBefore)+values.at(F::WaitAfter)+
        values.at(F::WaitAfterLift)+std::max(4.,values.at(F::LightOff));
}
}
