// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Utils.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

namespace Slic3r::sla {
inline bool statistics_time_is_dhms(double seconds)
{
    // The compact/shared GUI parsers use signed int days. This bound also
    // keeps whole seconds exact in double and makes every integer cast safe.
    return std::isfinite(seconds) && seconds >= 0 && seconds <= 1e12;
}
inline std::string statistics_time(double seconds, bool compact = false, bool localized = false)
{
    if (!std::isfinite(seconds) || seconds < 0) return "N/A";
    if (!statistics_time_is_dhms(seconds)) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::scientific << std::setprecision(3) << seconds << " s";
        return out.str(); // Never feed scientific notation to the dhms parsers.
    }
    // Preserve the historical sub-minute rounding and longer-time truncation,
    // without first projecting the source estimate to float32.
    const uint64_t whole = uint64_t(seconds < 60 ? std::round(seconds) : std::floor(seconds));
    const auto days = whole / 86400, hours = whole / 3600 % 24;
    const auto minutes = whole / 60 % 60, remainder = whole % 60;
    std::string result;
    if (days)
        result = std::to_string(days) + "d " + std::to_string(hours) + "h " +
            std::to_string(minutes) + "m " + std::to_string(remainder) + "s";
    else if (hours)
        result = std::to_string(hours) + "h " + std::to_string(minutes) + "m " + std::to_string(remainder) + "s";
    else if (minutes)
        result = std::to_string(minutes) + "m " + std::to_string(remainder) + "s";
    else
        result = std::to_string(remainder) + "s";
    return compact ? short_time(result, localized) : result;
}
inline std::string statistics_number(double value, unsigned decimals)
{
    if (!std::isfinite(value) || value < 0) return "N/A";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    decimals = std::min(decimals, 6u);
    // Tiny real jobs must not look empty/free; huge fields must not stretch
    // the sidebar by hundreds of digits. Units remain in the owning UI label.
    if ((value > 0 && value < std::pow(10., -int(decimals))) || value >= 1e9)
        out << std::scientific << std::setprecision(3) << value;
    else
        out << std::fixed << std::setprecision(decimals) << (value == 0 ? 0. : value);
    return out.str();
}
inline std::optional<double> statistics_cost(const SLAPrintStatistics &statistics, const SLAMaterialConfig &material)
{
    const double volume = material.bottle_volume.value, price = material.bottle_cost.value;
    if (!std::isfinite(volume) || volume <= 0 || !std::isfinite(price) || price < 0)
        return std::nullopt; // An unavailable unit price is not a free material.
    if (statistics.time_basis == SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule) {
        if (!std::isfinite(statistics.total_cost) || statistics.total_cost < 0) return std::nullopt;
        return statistics.total_cost;
    }
    // Legacy polygon statistics do not populate total_cost. Use the material
    // associated with this print, never an independently edited preset tab.
    if (!std::isfinite(statistics.objects_used_material) || statistics.objects_used_material < 0 ||
        !std::isfinite(statistics.support_used_material) || statistics.support_used_material < 0)
        return std::nullopt;
    const long double cost = (static_cast<long double>(statistics.objects_used_material) + statistics.support_used_material) /
        1000.L * price / volume;
    const double result = double(cost);
    return std::isfinite(result) && result >= 0 ? std::optional<double>(result) : std::nullopt;
}
inline std::optional<double> statistics_layer_fill(const SLAPrintStatistics &statistics, double fallback_area, size_t index)
{
    const double display_area = statistics.time_basis == SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule ?
        statistics.raster_area_mm2 : fallback_area;
    if (index >= statistics.layers_areas.size() || !std::isfinite(display_area) || display_area <= 0)
        return std::nullopt;
    const double area = statistics.layers_areas[index];
    if (!std::isfinite(area) || area < 0) return std::nullopt;
    const double percent = (area / display_area) * 100.;
    return std::isfinite(percent) ? std::optional<double>(percent) : std::nullopt;
}
inline std::optional<double> statistics_layer_time(const SLAPrintStatistics &statistics, size_t index)
{
    if (statistics.layers_areas.size() != statistics.layers_times_running_total.size() ||
        index >= statistics.layers_times_running_total.size()) return std::nullopt;
    const double end = statistics.layers_times_running_total[index];
    const double start = index ? statistics.layers_times_running_total[index - 1] : 0.;
    if (!std::isfinite(end) || !std::isfinite(start) || start < 0 || end < start) return std::nullopt;
    return end - start;
}
} // namespace Slic3r::sla
