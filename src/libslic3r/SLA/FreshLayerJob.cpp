// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FreshLayerJob.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>

namespace Slic3r::sla {
FreshLayerJob make_fresh_layer_job(const SLAPrinterConfig &printer,
    const SLAMaterialConfig &material, double layer_height,
    const std::vector<double> &positions_z, FreshLayerProducer producer,
    ExposurePositionPolicy position_policy)
{
    using F = LayerParameter;
    if (positions_z.empty())
        throw RuntimeError("Fresh producer requires finalized nonempty SLA layers");
    if (printer.display_pixels_x.value <= 0 || printer.display_pixels_y.value <= 0)
        throw RuntimeError("Fresh producer requires positive LCD pixel dimensions");
    if (material.sla_bottom_layers.value < 0 || material.sla_transition_layers.value < 0)
        throw RuntimeError("Fresh producer bottom and transition counts must be nonnegative");
    FreshLayerJob result;
    result.producer = producer;
    result.position_policy = position_policy;
    const bool portrait = printer.display_orientation.value == sladoPortrait;
    result.geometry = {size_t(portrait ? printer.display_pixels_y.value : printer.display_pixels_x.value),
        size_t(portrait ? printer.display_pixels_x.value : printer.display_pixels_y.value),
        portrait ? printer.display_height.value : printer.display_width.value,
        portrait ? printer.display_width.value : printer.display_height.value};
    auto &globals = result.globals;
    globals.layer_height = layer_height;
    const size_t configured_bottom = size_t(material.sla_bottom_layers.value);
    const size_t configured_transition = size_t(material.sla_transition_layers.value);
    globals.bottom_layers = std::min(configured_bottom, positions_z.size());
    globals.transition_layers = std::min(configured_transition, positions_z.size() - globals.bottom_layers);
    // Read typed doubles directly. The legacy encoder helpers return floats
    // and must not round a neutral document before a destination is selected.
    const std::map<F, std::string> keys{
        {F::LiftDistance,"lift_height"}, {F::LiftDistance2,"lift_height2"},
        {F::LiftSpeed,"lift_speed"}, {F::LiftSpeed2,"lift_speed2"},
        {F::RetractSpeed,"retract_speed"}, {F::RetractSpeed2,"retract_speed2"},
        {F::RetractDistance2,"retract_height2"}, {F::WaitBefore,"wait_before_cure"},
        {F::WaitAfter,"wait_after_cure"}, {F::WaitAfterLift,"wait_after_lift"}};
    for (bool bottom : {false, true}) {
        auto &values = bottom ? globals.bottom : globals.normal;
        const std::string prefix = bottom ? "sla_bottom_" : "sla_";
        values[F::Exposure] = bottom ? material.initial_exposure_time.value : material.exposure_time.value;
        for (const auto &[field, key] : keys) {
            const auto *option = material.option(prefix + key);
            if (!option) throw RuntimeError("Missing fresh SLA parameter: " + prefix + key);
            values[field] = option->getFloat();
        }
        const auto *pwm = material.option(prefix + "light_pwm");
        if (!pwm) throw RuntimeError("Missing fresh SLA parameter: " + prefix + "light_pwm");
        values[F::LightPWM] = pwm->getInt();
        // Current operator intent uses explicit rests. Legacy LightOff/cycle
        // adaptation, native rounding and fixed-motion policies are codec-owned.
        values[F::LightOff] = 0.;
    }
    result.rows.reserve(positions_z.size());
    for (size_t i = 0; i < positions_z.size(); ++i) {
        auto row = i < configured_bottom ? globals.bottom : globals.normal;
        row[F::PositionZ] = positions_z[i];
        if (i >= configured_bottom && i - configured_bottom < configured_transition)
            // Clipping counts for a short job must not change its exposure
            // schedule: the denominator remains the configured transition.
            row[F::Exposure] = material.initial_exposure_time.value +
                (material.exposure_time.value - material.initial_exposure_time.value) *
                double(i - configured_bottom + 1) / double(configured_transition + 1);
        result.rows.push_back(std::move(row));
    }
    result.validate(positions_z.size());
    return result;
}
} // namespace Slic3r::sla
