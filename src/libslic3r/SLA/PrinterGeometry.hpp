#ifndef SLIC3R_SLA_PRINTER_GEOMETRY_HPP
#define SLIC3R_SLA_PRINTER_GEOMETRY_HPP

#include <cmath>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::sla {

// The SLA scene uses the unrotated display dimensions in millimetres. Portrait
// orientation and mirrors affect exported pixels, not the physical scene bed.
// Partial material/process configs and invalid dimensions must not change it.
// Returns true only when a valid printer configuration was actually changed.
inline bool synchronize_bed_shape(DynamicPrintConfig &config)
{
    const auto *technology = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
    const auto *width = config.option<ConfigOptionFloat>("display_width");
    const auto *height = config.option<ConfigOptionFloat>("display_height");
    if (!technology || technology->value != ptSLA || !width || !height ||
        !std::isfinite(width->value) || !std::isfinite(height->value) ||
        width->value <= 0.0 || height->value <= 0.0)
        return false;

    const std::vector<Vec2d> rectangle{
        {0.0, 0.0}, {width->value, 0.0},
        {width->value, height->value}, {0.0, height->value}};
    const auto *bed = config.option<ConfigOptionPoints>("bed_shape");
    if (bed && bed->values == rectangle)
        return false;

    config.set_key_value("bed_shape", new ConfigOptionPoints(rectangle));
    return true;
}

} // namespace Slic3r::sla

#endif
