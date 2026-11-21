// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <map>
#include <vector>
#include <string>

namespace Slic3r::sla {
// Canonical units are mm, seconds and mm/min. Missing fields remain absent;
// they must never be replaced by invented defaults during conversion.
enum class LayerParameter {
    PositionZ, Exposure, LiftDistance, LiftSpeed, LightOff,
    WaitBefore, WaitAfter, WaitAfterLift, LiftDistance2, LiftSpeed2,
    RetractSpeed, RetractDistance2, RetractSpeed2, LightPWM
};
using LayerParameters = std::map<LayerParameter, double>;
struct LayerParameterCapability {
    LayerParameter field;
    const char *label;
    const char *unit;
    bool editable;
};
using LayerParameterCapabilities = std::vector<LayerParameterCapability>;
// Structural fields accepted by the implemented conversion route. Actual
// printer motion constraints are validated against the generated native file.
LayerParameterCapabilities parameter_conversion_schema(const std::string &destination);
}
