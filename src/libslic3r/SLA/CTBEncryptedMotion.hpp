// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerParameters.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <optional>
#include <string_view>

namespace Slic3r::sla {
inline bool ctb_encrypted_named_tilting_vat(const std::string &machine)
{
    return boost::algorithm::icontains(machine, "Saturn 4 Ultra") ||
           boost::algorithm::icontains(machine, "Mars 5 Ultra");
}

// CTBEncryptedFile.HaveTiltingVat uses native float FIRST-phase geometry.
inline bool ctb_encrypted_tilting_motion(const std::string &machine, float nominal,
                                         float first_lift, float speed)
{
    return ctb_encrypted_named_tilting_vat(machine) ||
           (nominal == first_lift && first_lift < .5f && speed < .5f);
}

// Protocol placeholders from CTBEncryptedFile.OnBeforeEncode. These are not
// motor travel instructions: the reference estimates a fixed 4-second cycle.
inline const LayerParameters &ctb_encrypted_fixed_motion()
{
    using F = LayerParameter;
    static const LayerParameters values{{F::LiftDistance, double(.05f)},
        {F::LiftSpeed, double(.05f)}, {F::RetractSpeed, double(.05f)},
        {F::LiftDistance2, 0}, {F::LiftSpeed2, 0},
        {F::RetractDistance2, 0}, {F::RetractSpeed2, 0}};
    return values;
}
inline std::optional<float> ctb_encrypted_fixed_parameter(std::string_view key)
{
    if (key == "lift_height" || key == "lift_speed" || key == "retract_speed") return .05f;
    if (key == "lift_height2" || key == "lift_speed2" ||
        key == "retract_height2" || key == "retract_speed2") return 0.f;
    return std::nullopt;
}
} // namespace Slic3r::sla
