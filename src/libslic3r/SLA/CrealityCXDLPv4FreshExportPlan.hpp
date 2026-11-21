// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include "CrealityCXDLPv4ReferenceBottom.hpp"
#include "libslic3r/Format/CrealityCXDLPv4Counts.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <limits>

namespace Slic3r::sla {
struct CXDLPv4FreshExportPlan {
    ExposurePositionPolicy position_policy = ExposurePositionPolicy::IncreasingLayers;
    LayerJobParameters globals;
    std::vector<LayerParameters> rows;
    LayerRasterGeometry geometry;
    std::optional<size_t> meaningful_bottom_layer;
    uint32_t expected_print_time = 0;
    float material_ml = 0.f;
};

// Material properties belong to the selected resin/profile context. Validate
// them before encoding as well as at the format-owned writer boundary.
inline std::array<float, 2> cxdlpv4_fresh_material_metrics(float ml, double density,
    double bottle_cost, double bottle_volume) {
    auto input = [](double value, const char *field, bool positive = false) {
        if (!std::isfinite(value) || (positive ? value <= 0 : value < 0))
            throw RuntimeError(std::string("CXDLPv4 ") + field + " must be finite and " +
                (positive ? "positive" : "nonnegative"));
    };
    input(ml, "material_ml");
    input(density, "material_density");
    input(bottle_cost, "bottle_cost");
    input(bottle_volume, "bottle_volume", true);
    const std::array<float, 2> metrics{float(ml * density), float(ml * bottle_cost / bottle_volume)};
    for (size_t i = 0; i < metrics.size(); ++i)
        if (!std::isfinite(metrics[i]) || metrics[i] < 0)
            throw RuntimeError(std::string("CXDLPv4 ") + (i ? "material_cost" : "material_weight") +
                " exceeds finite nonnegative native float range");
    return metrics;
}

namespace cxdlpv4_fresh_detail {
using F = LayerParameter;
inline const char *field_id(F field) {
    switch (field) {
    case F::PositionZ: return "PositionZ";
    case F::Exposure: return "Exposure";
    case F::LiftDistance: return "LiftDistance";
    case F::LiftDistance2: return "LiftDistance2";
    case F::LiftSpeed: return "LiftSpeed";
    case F::LiftSpeed2: return "LiftSpeed2";
    case F::RetractSpeed: return "RetractSpeed";
    case F::RetractSpeed2: return "RetractSpeed2";
    case F::RetractDistance2: return "RetractDistance2";
    case F::WaitBefore: return "WaitBefore";
    case F::WaitAfter: return "WaitAfter";
    case F::WaitAfterLift: return "WaitAfterLift";
    case F::LightOff: return "LightOff";
    case F::LightPWM: return "LightPWM";
    }
    return "Unknown";
}
inline const char *field_unit(F field) {
    switch (field) {
    case F::PositionZ: case F::LiftDistance: case F::LiftDistance2:
    case F::RetractDistance2: return "mm";
    case F::LiftSpeed: case F::LiftSpeed2: case F::RetractSpeed:
    case F::RetractSpeed2: return "mm/min";
    case F::LightPWM: return "0-255";
    default: return "s";
    }
}
inline const char *field_label(F field) {
    switch (field) {
    case F::PositionZ: return "Z position";
    case F::Exposure: return "Exposure";
    case F::LiftDistance: return "Lift distance";
    case F::LiftDistance2: return "Lift distance 2";
    case F::LiftSpeed: return "Lift speed";
    case F::LiftSpeed2: return "Lift speed 2";
    case F::RetractSpeed: return "Retract speed";
    case F::RetractSpeed2: return "Retract speed 2";
    case F::RetractDistance2: return "Retract distance 2";
    case F::WaitBefore: return "Wait before cure";
    case F::WaitAfter: return "Wait after cure";
    case F::WaitAfterLift: return "Wait after lift";
    case F::LightOff: return "Light-off delay";
    case F::LightPWM: return "Light PWM";
    }
    return "Unknown parameter";
}
[[noreturn]] inline void fail(const std::string &scope, F field, const char *reason) {
    throw RuntimeError("CXDLPv4 cannot preserve " + scope + "." + field_id(field) +
        " — " + field_label(field) + " (" + field_unit(field) + "): " + reason);
}
inline LayerParameters native_row(const LayerParameters &source, const std::string &scope) {
    auto row = source;
    for (auto &[field, value] : row) {
        const double native = double(float(value));
        if (!std::isfinite(native) || (value > 0 && native <= 0))
            fail(scope, field, "outside native float precision");
        value = native;
    }
    const double total = double(float(source.at(F::LiftDistance) + source.at(F::LiftDistance2)));
    const double first = total - row.at(F::LiftDistance2);
    if (!std::isfinite(total) || first < 0 || (source.at(F::LiftDistance) > 0 && first <= 0))
        fail(scope, F::LiftDistance, "TOTAL/SECOND encoding loses the first phase");
    row[F::LiftDistance] = first;
    const double native_retract1 = total - row.at(F::RetractDistance2);
    const double source_retract1 = source.at(F::LiftDistance) + source.at(F::LiftDistance2) -
        source.at(F::RetractDistance2);
    if (native_retract1 < 0 || (source_retract1 > 0 && native_retract1 <= 0))
        fail(scope, F::RetractDistance2, "native precision loses the first retract phase");
    return row;
}

struct PublicMotion { float first, second, retract2, total; };
inline PublicMotion public_motion(const LayerParameters &row, const std::string &scope) {
    const float total = float(row.at(F::LiftDistance) + row.at(F::LiftDistance2));
    const float first = cxdlpv4_public_round2(cxdlpv4_public_round2(total) - float(row.at(F::LiftDistance2)));
    const float second = cxdlpv4_public_round2(float(row.at(F::LiftDistance2)));
    if (first < 0 || (row.at(F::LiftDistance) > 0 && first <= 0))
        fail(scope, F::LiftDistance, "public precision loses or replaces the first phase");
    if (row.at(F::LiftDistance2) > 0 && second <= 0)
        fail(scope, F::LiftDistance2, "public precision loses the second phase");
    const float public_total = cxdlpv4_public_round2(first + second);
    const float retract2 = std::clamp(cxdlpv4_public_round2(float(row.at(F::RetractDistance2))),
        0.f, public_total);
    const double native_retract1 = double(total) - row.at(F::RetractDistance2);
    const float retract1 = public_total - retract2;
    if ((native_retract1 > 0 && retract1 <= 0) || (row.at(F::RetractDistance2) > 0 && retract2 <= 0))
        fail(scope, F::RetractDistance2, "public precision loses a retract phase");
    for (const auto &[distance, speed] : std::array<std::pair<float,F>,4>{
            std::pair<float,F>{first,F::LiftSpeed}, {second,F::LiftSpeed2},
            {retract1,F::RetractSpeed}, {retract2,F::RetractSpeed2}}) {
        if (distance > 0 && cxdlpv4_public_round2(float(row.at(speed))) <= 0)
            fail(scope, speed, "rounded-zero speed would invoke a different global fallback");
    }
    return {first, second, retract2, public_total};
}

inline float public_row_seconds(const LayerParameters &row, const std::string &scope) {
    const auto motion = public_motion(row, scope);
    float motor = 0.f;
    auto phase = [&](float distance, F speed_field) {
        if (distance <= 0) return;
        const float speed = cxdlpv4_public_round2(float(row.at(speed_field)));
        if (!std::isfinite(speed) || speed <= 0)
            fail(scope, speed_field, "moving phase requires a finite positive public speed");
        motor += distance / (speed / 60.f);
    };
    phase(motion.first, F::LiftSpeed);
    phase(motion.second, F::LiftSpeed2);
    phase(motion.retract2, F::RetractSpeed2);
    phase(cxdlpv4_public_round2(motion.total - motion.retract2), F::RetractSpeed);
    auto value = [&](F field) { return cxdlpv4_public_round2(float(row.at(field))); };
    const float rests = value(F::WaitBefore) + value(F::Exposure) +
        value(F::WaitAfter) + value(F::WaitAfterLift);
    if (!std::isfinite(motor) || !std::isfinite(rests))
        throw RuntimeError("CXDLPv4 expected_print_time (s) exceeds finite native float range");
    return rests + std::max(cxdlpv4_public_round2(motor), value(F::LightOff));
}
} // namespace cxdlpv4_fresh_detail

// This plan represents only an explicit fresh descriptor. It does not enable
// native editing or infer missing defaults from the selected printer preset.
// Progress belongs to the calling thread. Pixels are examined in bounded
// batches; native float accounting remains in layer order on the caller.
CXDLPv4FreshExportPlan cxdlpv4_fresh_export_plan(const LayerDocument &document,
    const LayerProgress &progress = {});

} // namespace Slic3r::sla
