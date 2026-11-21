// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerParameters.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r::sla {
inline double finite_fresh_statistic(double value, const char *name)
{
    if (!std::isfinite(value) || value < 0)
        throw RuntimeError(std::string("Invalid owned raster statistic: ") + name);
    return value;
}

// Source schedule in seconds, with distances in mm and speeds in mm/min.
// Call only after FreshLayerJob::validate. No native codec precision,
// preparation time, tilt policy or exposure normalization belongs here.
inline double fresh_layer_schedule_time(const LayerParameters &row)
{
    using P = LayerParameter;
    const double lift = row.at(P::LiftDistance), lift2 = row.at(P::LiftDistance2);
    const double retract2 = row.at(P::RetractDistance2);
    double motor = 0;
    for (const auto &[distance, speed] : std::array<std::pair<double, double>, 4>{
            std::pair<double, double>{lift, row.at(P::LiftSpeed)},
            {lift2, row.at(P::LiftSpeed2)},
            {retract2, row.at(P::RetractSpeed2)},
            {lift + lift2 - retract2, row.at(P::RetractSpeed)}}) {
        if (distance == 0) continue; // Inactive phases may have zero speed.
        if (!std::isfinite(speed) || speed <= 0)
            throw RuntimeError("An owned raster moving phase requires a positive finite speed");
        const double seconds = finite_fresh_statistic((distance / speed) * 60., "motion time");
        motor = finite_fresh_statistic(motor + seconds, "total motion time");
    }
    double seconds = std::max(motor, row.at(P::LightOff));
    for (const auto field : {P::WaitBefore, P::Exposure, P::WaitAfter, P::WaitAfterLift})
        seconds = finite_fresh_statistic(seconds + row.at(field), "layer schedule time");
    return seconds;
}
} // namespace Slic3r::sla
