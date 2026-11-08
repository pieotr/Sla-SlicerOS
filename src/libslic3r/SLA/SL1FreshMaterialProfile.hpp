// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "SL1FreshExportPlan.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <array>

namespace Slic3r::sla {
// SL1's reference reads resin properties as Single values. Preserve a finite
// native profile and its derived weight/cost before producing the ZIP.
inline std::array<float,4> sl1_fresh_material_profile(const SL1FreshExportPlan &plan,
    const SLAMaterialConfig &material)
{
    auto native=[](double value,const char *field,bool positive=false) {
        const float result=float(value);
        if (!std::isfinite(value) || !std::isfinite(result) || value<0 ||
            (positive && value<=0) || (value>0 && result<=0))
            throw RuntimeError(std::string("SL1 ")+field+" cannot fit a finite native material value");
        return result;
    };
    const std::array<float,4> profile{
        native(material.material_density.value,"material_density"),
        native(material.bottle_cost.value,"bottle_cost"),
        native(material.bottle_volume.value,"bottle_volume",true),
        native(material.bottle_weight.value,"bottle_weight")};
    const float ml=native(plan.material_ml,"usedMaterial");
    const float weight=ml*profile[0],cost=ml*profile[1]/profile[2];
    if (!std::isfinite(weight) || (ml>0 && profile[0]>0 && weight<=0))
        throw RuntimeError("SL1 material_weight exceeds native float precision");
    if (!std::isfinite(cost) || (ml>0 && profile[1]>0 && cost<=0))
        throw RuntimeError("SL1 material_cost exceeds native float precision");
    return profile;
}
} // namespace Slic3r::sla
