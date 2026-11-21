// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cmath>
#include <optional>
#include <string_view>

namespace Slic3r::sla {
struct DerivedBottleSetting { const char *key; double value; };

// Volume is in ml, bottle weight in kg and density in g/ml. Editing density
// keeps the bottle's physical volume; editing weight can derive volume only
// with a positive known density. Never propagate a nonrepresentable result.
inline std::optional<DerivedBottleSetting> derive_bottle_setting(std::string_view edited,
    double volume, double weight, double density)
{
    if (!std::isfinite(density) || density < 0) return std::nullopt;
    long double derived;
    const char *key;
    if (edited == "bottle_volume" || edited == "material_density") {
        if (!std::isfinite(volume) || volume < 0) return std::nullopt;
        derived = static_cast<long double>(volume) * density / 1000;
        key = "bottle_weight";
    } else if (edited == "bottle_weight") {
        if (density == 0 || !std::isfinite(weight) || weight < 0) return std::nullopt;
        derived = static_cast<long double>(weight) * 1000 / density;
        key = "bottle_volume";
    } else return std::nullopt;
    const double result = double(derived);
    if (!std::isfinite(result) || (derived > 0 && result == 0)) return std::nullopt;
    return DerivedBottleSetting{key, result};
}
} // namespace Slic3r::sla
