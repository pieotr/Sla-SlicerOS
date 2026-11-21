// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "AnycubicFreshExportPlan.hpp"
#include "libslic3r/Config.hpp"

namespace Slic3r::sla {
// Checks generated defaults only. Document-owned rows and geometry still
// require the complete export plan. Reading settings must never normalize them.
inline std::string anycubic_generated_settings_error(const ConfigBase &config)
{
    auto name=[&](const std::string &key) {
        const auto *definition=config.def()?config.def()->get(key):nullptr;
        return definition && !definition->label.empty() ? definition->label+" ("+key+")" : key;
    };
    auto option=[&](const std::string &key) {
        const auto *value=config.option(key);
        if (!value) throw RuntimeError("Missing SLA setting: "+key);
        return value;
    };
    for (const char *group:{"sla_","sla_bottom_"}) {
        for (const auto &parameter:anycubic_unavailable_parameters) {
            if (!parameter.config_suffix) continue;
            const std::string key=std::string(group)+parameter.config_suffix;
            if (option(key)->getFloat()!=0.)
                return "Modern Anycubic cannot preserve "+name(key)+". Set it to zero or select another format.";
        }
        const std::string pwm=std::string(group)+"light_pwm";
        if (option(pwm)->getInt()!=255)
            return "Modern Anycubic requires "+name(pwm)+" to be 255.";
        const std::string wait=std::string(group)+"wait_before_cure";
        const double value=option(wait)->getFloat();
        if (value<0 || !std::isfinite(float(value)) || (value>0 && float(value)<=0))
            return "Modern Anycubic requires "+name(wait)+" to be nonnegative and fit the native float range.";
    }
    if (float(option("sla_wait_before_cure")->getFloat())!=float(option("sla_bottom_wait_before_cure")->getFloat()))
        return "Modern Anycubic uses one wait before cure. "+name("sla_wait_before_cure")+" and "+
            name("sla_bottom_wait_before_cure")+" must match.";
    return {};
}
} // namespace Slic3r::sla
