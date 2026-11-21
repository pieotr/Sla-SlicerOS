#ifndef SLIC3R_SLA_PRINTER_GEOMETRY_HPP
#define SLIC3R_SLA_PRINTER_GEOMETRY_HPP

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::sla {

// Engine-level geometry validation is deliberately independent of GUI schema
// minima: small rasters are useful for native fixtures and generated documents.
// The custom-printer wizard additionally validates its published UI limits.
// Return a field-specific error instead of throwing during preset normalization.
inline std::string printer_geometry_error(const DynamicPrintConfig &config)
{
    const auto *technology = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
    if (!technology || technology->value != ptSLA)
        return "An SLA printer configuration is required";
    const double coordinate_limit = double(std::numeric_limits<coord_t>::max()) * SCALING_FACTOR;
    for (const char *key : {"display_width", "display_height", "max_print_height"}) {
        const auto *value = config.option<ConfigOptionFloat>(key);
        if (!value || !std::isfinite(value->value) || value->value <= 0. || value->value >= coordinate_limit)
            return std::string(key) + " must be finite, positive and representable in SLA coordinates";
    }
    const auto *columns = config.option<ConfigOptionInt>("display_pixels_x");
    const auto *rows = config.option<ConfigOptionInt>("display_pixels_y");
    for (const auto &entry : {std::make_pair("display_pixels_x", columns),
                              std::make_pair("display_pixels_y", rows)})
        if (!entry.second || entry.second->value <= 0 || entry.second->value > 32768)
            return std::string(entry.first) + " must be an integer between 1 and 32768";
    if (uint64_t(columns->value) * uint64_t(rows->value) > 128ULL * 1024 * 1024)
        return "Display resolution exceeds the 128 Mi-pixel raster limit";
    const auto *orientation = config.option<ConfigOptionEnum<SLADisplayOrientation>>("display_orientation");
    if (!orientation || (orientation->value != sladoLandscape && orientation->value != sladoPortrait))
        return "Display orientation must be Landscape or Portrait";
    return {};
}

// The SLA scene uses unrotated physical LCD dimensions. Portrait/mirrors affect
// exported pixels, not the bed. Reject invalid/partial geometry without silently
// repairing user input or mutating the last valid bed; the GUI reports the error.
inline bool synchronize_bed_shape(DynamicPrintConfig &config)
{
    if (!printer_geometry_error(config).empty())
        return false;
    const double width = config.option<ConfigOptionFloat>("display_width")->value;
    const double height = config.option<ConfigOptionFloat>("display_height")->value;
    const std::vector<Vec2d> rectangle{
        {0.0, 0.0}, {width, 0.0}, {width, height}, {0.0, height}};
    const auto *bed = config.option<ConfigOptionPoints>("bed_shape");
    if (bed && bed->values == rectangle)
        return false;
    config.set_key_value("bed_shape", new ConfigOptionPoints(rectangle));
    return true;
}

} // namespace Slic3r::sla

#endif
