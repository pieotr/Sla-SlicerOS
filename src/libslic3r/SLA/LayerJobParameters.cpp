// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerJobParameters.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Exception.hpp"
#include <array>
#include <cmath>
#include <limits>

namespace Slic3r::sla {

void LayerJobParameters::validate(size_t layer_count) const
{
    using F = LayerParameter;
    if (!std::isfinite(layer_height) || layer_height <= 0)
        throw RuntimeError("Nominal SLA layer height must be finite and positive");
    if (layer_count == 0 || bottom_layers > layer_count || transition_layers > layer_count - bottom_layers)
        throw RuntimeError("Nominal bottom and transition counts exceed the SLA job");
    constexpr std::array<F, 13> fields{F::Exposure, F::LiftDistance, F::LiftDistance2,
        F::LiftSpeed, F::LiftSpeed2, F::RetractSpeed, F::RetractSpeed2,
        F::RetractDistance2, F::WaitBefore, F::WaitAfter, F::WaitAfterLift, F::LightOff, F::LightPWM};
    for (const auto *row : {&normal, &bottom}) {
        if (row->size() != fields.size()) throw RuntimeError("Incomplete nominal SLA parameter table");
        for (const auto field : fields) {
            const auto found = row->find(field);
            if (found == row->end() || !std::isfinite(found->second) || found->second < 0)
                throw RuntimeError("Nominal SLA parameters must be finite and nonnegative");
        }
        const double total = row->at(F::LiftDistance) + row->at(F::LiftDistance2);
        if (!std::isfinite(total) || row->at(F::RetractDistance2) > total)
            throw RuntimeError("Nominal SLA retract distance exceeds the finite lift distance");
        const double pwm = row->at(F::LightPWM);
        if (pwm > 255 || pwm != std::floor(pwm))
            throw RuntimeError("Nominal SLA light PWM must be an integer from 0 to 255");
        for (const auto &[distance, speed] : std::array<std::pair<double,F>,4>{
                std::pair<double,F>{row->at(F::LiftDistance),F::LiftSpeed},
                {row->at(F::LiftDistance2),F::LiftSpeed2},
                {total-row->at(F::RetractDistance2),F::RetractSpeed},
                {row->at(F::RetractDistance2),F::RetractSpeed2}})
            if (distance > 0 && row->at(speed) <= 0)
                throw RuntimeError("A nominal SLA moving phase requires a positive speed");
    }
}

void LayerJobParameters::apply_to(DynamicPrintConfig &config) const
{
    // This bridge does not know the actual layer count. Validate all values
    // and addressable count arithmetic here; the job owner validates against
    // its actual count before export. No malformed PWM may reach an int cast.
    validate(std::numeric_limits<size_t>::max());
    // Configuration counts use signed ints; the neutral model does not.
    if (bottom_layers > size_t(std::numeric_limits<int>::max()) ||
        transition_layers > size_t(std::numeric_limits<int>::max()))
        throw RuntimeError("Nominal SLA counts exceed the print configuration range");
    using F = LayerParameter;
    config.set("layer_height", layer_height);
    config.set("exposure_time", normal.at(F::Exposure));
    config.set("initial_exposure_time", bottom.at(F::Exposure));
    config.set("sla_bottom_layers", int(bottom_layers));
    config.set("sla_transition_layers", int(transition_layers));
    for (bool is_bottom : {false, true}) {
        const auto &values = is_bottom ? bottom : normal;
        const std::string prefix = is_bottom ? "sla_bottom_" : "sla_";
        for (const auto &[field, key] : std::map<F, std::string>{
                {F::LiftDistance,"lift_height"}, {F::LiftDistance2,"lift_height2"},
                {F::LiftSpeed,"lift_speed"}, {F::LiftSpeed2,"lift_speed2"},
                {F::RetractSpeed,"retract_speed"}, {F::RetractSpeed2,"retract_speed2"},
                {F::RetractDistance2,"retract_height2"}, {F::WaitBefore,"wait_before_cure"},
                {F::WaitAfter,"wait_after_cure"}, {F::WaitAfterLift,"wait_after_lift"}})
            config.set(prefix + key, values.at(field));
        config.set(prefix + "light_pwm", int(values.at(F::LightPWM)));
    }
}

} // namespace Slic3r::sla
