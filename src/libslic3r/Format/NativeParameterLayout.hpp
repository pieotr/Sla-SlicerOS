// SPDX-License-Identifier: AGPL-3.0-or-later
// Helpers shared by the per-layer parameter tables of the wave 7-8 format
// families (PHZ/FDG records, OSLA records, VDT and Anycubic ZIP manifests,
// Klipper G-code). Header only: it needs no CMake entry.
//
// Canonical units are the ones of sla::LayerParameters: mm, seconds, mm/min.
// A family's `parameter_rows()` returns the values exactly as the container
// stores them (after the container's own float/decimal rounding), so that
// "edit, save, reopen" returns the edited value and an unedited save can be
// recognised by comparing rows.
#ifndef SLIC3R_NATIVE_PARAMETER_LAYOUT_HPP
#define SLIC3R_NATIVE_PARAMETER_LAYOUT_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <locale>
#include <sstream>
#include <limits>
#include <optional>
#include <utility>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/SLA/LayerParameters.hpp"

namespace Slic3r::native_parameters {

using sla::LayerParameter;
using Row = sla::LayerParameters;
using Table = std::vector<Row>;

inline double value_of(const Row &row, LayerParameter field)
{
    const auto found = row.find(field);
    return found == row.end() ? 0. : found->second;
}

// The file stores a 32-bit float: reading it back gives this double.
inline double as_float(double value) { return double(float(value)); }
inline double round_decimals(double value, double scale) { return std::round(value * scale) / scale; }

inline sla::LayerParameterCapability capability(LayerParameter field, bool editable = true)
{
    using F = LayerParameter;
    switch (field) {
    case F::PositionZ:         return {field, "Z position", "mm", editable};
    case F::Exposure:          return {field, "Exposure", "s", editable};
    case F::LiftDistance:      return {field, "Lift distance", "mm", editable};
    case F::LiftSpeed:         return {field, "Lift speed", "mm/min", editable};
    case F::LightOff:          return {field, "Light-off delay", "s", editable};
    case F::WaitBefore:        return {field, "Wait before exposure", "s", editable};
    case F::WaitAfter:         return {field, "Wait after exposure", "s", editable};
    case F::WaitAfterLift:     return {field, "Wait after lift", "s", editable};
    case F::LiftDistance2:     return {field, "Lift distance 2", "mm", editable};
    case F::LiftSpeed2:        return {field, "Lift speed 2", "mm/min", editable};
    case F::RetractSpeed:      return {field, "Retract speed", "mm/min", editable};
    case F::RetractDistance2:  return {field, "Retract distance 2", "mm", editable};
    case F::RetractSpeed2:     return {field, "Retract speed 2", "mm/min", editable};
    case F::LightPWM:          return {field, "Light PWM", "0\xE2\x80\x93" "255", editable};
    }
    return {field, "", "", editable};
}

inline sla::LayerParameterCapabilities capabilities(std::initializer_list<LayerParameter> fields)
{
    sla::LayerParameterCapabilities result;
    for (const auto field : fields) result.push_back(capability(field));
    return result;
}

// Seconds one layer takes, with the same terms as sla::lcd_layer_time that
// our exporters use for the stored print time. Absent fields count as zero.
inline double layer_seconds(const Row &row)
{
    using F = LayerParameter;
    auto move = [&](double distance, F speed) {
        const double rate = value_of(row, speed);
        return distance > 0 && rate > 0 ? distance * 60. / rate : 0.;
    };
    const double lift = value_of(row, F::LiftDistance), lift2 = value_of(row, F::LiftDistance2),
                 retract2 = value_of(row, F::RetractDistance2);
    return value_of(row, F::Exposure) + value_of(row, F::WaitBefore) + value_of(row, F::LightOff) +
           value_of(row, F::WaitAfter) + value_of(row, F::WaitAfterLift) +
           move(lift, F::LiftSpeed) + move(lift2, F::LiftSpeed2) +
           move(lift + lift2 - retract2, F::RetractSpeed) + move(retract2, F::RetractSpeed2);
}

// New stored print time. A file written by our exporter stores ceil(sum), so
// the same rounding is applied to the new sum. A file from another writer
// keeps its own overhead: only the difference of the model is added.
inline uint32_t carried_time(uint32_t stored, double old_total, double new_total)
{
    const bool ours = std::fabs(double(stored) - std::ceil(old_total)) < .5;
    const double next = ours ? std::ceil(new_total) : double(stored) + (new_total - old_total);
    if (!std::isfinite(next) || next < 0 || next > double(UINT32_MAX))
        throw RuntimeError("The edited print time does not fit its 32-bit field");
    return ours ? uint32_t(next) : uint32_t(std::llround(next));
}

// What the stored statistics need to know about one layer's mask: the number
// of lit pixels and their box (right/bottom exclusive, like the exporters).
struct MaskSummary {
    uint64_t lit = 0;
    size_t left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const MaskSummary &other) const
    {
        return lit == other.lit && left == other.left && top == other.top && right == other.right && bottom == other.bottom;
    }
    bool operator!=(const MaskSummary &other) const { return !(*this == other); }
};
inline MaskSummary summarize(const std::vector<uint8_t> &pixels, size_t columns, size_t rows)
{
    MaskSummary result;
    result.left = columns;
    result.top = rows;
    for (size_t y = 0; y < rows; ++y) {
        const uint8_t *line = pixels.data() + y * columns;
        for (size_t x = 0; x < columns; ++x)
            if (line[x]) {
                ++result.lit;
                result.left = std::min(result.left, x); result.top = std::min(result.top, y);
                result.right = std::max(result.right, x + 1); result.bottom = std::max(result.bottom, y + 1);
            }
    }
    if (!result.lit) result.left = result.top = 0;
    return result;
}
inline std::vector<double> lit_counts(const std::vector<MaskSummary> &masks)
{
    std::vector<double> result;
    result.reserve(masks.size());
    for (const auto &mask : masks) result.push_back(double(mask.lit));
    return result;
}


// G-code number used by the ZCode, ZCodex and JXS exporters: three decimals,
// trailing zeros (and a bare point) removed.
inline std::string trimmed_number(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(3) << (value == 0 ? 0. : value);
    std::string text = out.str();
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text;
}

// Sum of lit pixels times layer thickness. The stored volume of a document
// scales with it, so a mask or Z edit scales the volume by new / old.
inline double thickness_weighted_area(const std::vector<MaskSummary> &masks, const std::vector<double> &z)
{
    double total = 0, previous = 0;
    for (size_t i = 0; i < masks.size(); ++i) {
        total += double(masks[i].lit) * (z[i] - previous);
        previous = z[i];
    }
    return total;
}
inline std::vector<double> z_column(const Table &table)
{
    std::vector<double> z;
    z.reserve(table.size());
    for (const auto &row : table) z.push_back(value_of(row, LayerParameter::PositionZ));
    return z;
}
inline double total_seconds(const Table &table)
{
    double total = 0;
    for (const auto &row : table) total += layer_seconds(row);
    return total;
}

// Positive-speed rule shared by every writer that emits a move: an active
// move needs a speed or the printer would never finish it.
inline void require_moves_have_speed(const Row &row, const char *family)
{
    using F = LayerParameter;
    const double lift = value_of(row, F::LiftDistance), lift2 = value_of(row, F::LiftDistance2),
                 retract2 = value_of(row, F::RetractDistance2);
    if (retract2 > lift + lift2)
        throw RuntimeError(std::string(family) + " retract distance 2 exceeds the total lift distance");
    const std::pair<double, F> moves[] = {{lift, F::LiftSpeed}, {lift2, F::LiftSpeed2},
        {lift + lift2 - retract2, F::RetractSpeed}, {retract2, F::RetractSpeed2}};
    for (const auto &[distance, speed] : moves)
        if (distance > 0 && value_of(row, speed) <= 0)
            throw RuntimeError(std::string(family) + ": a moving lift or retract phase needs a positive speed");
}

inline void require_pwm(const Row &row, const char *family)
{
    const auto found = row.find(LayerParameter::LightPWM);
    if (found == row.end()) return;
    if (found->second > 255 || found->second != std::floor(found->second))
        throw RuntimeError(std::string(family) + " light PWM must be a whole number from 0 to 255");
}

// How a JSON text of another writer is laid out: the indent (-1 = compact) and whether a line break follows
// the closing brace. A document is rewritten only when it dumps back to its own bytes under one of the
// common layouts (keys kept in file order), so that changing one value can never reformat anything else.
struct JsonStyle { int indent = 2; bool newline = false; };
inline std::optional<JsonStyle> json_style(const nlohmann::ordered_json &json, const std::string &text)
{
    for (const int indent : {2, 4, 1, 3, 8, -1}) {
        const auto dumped = json.dump(indent);
        if (dumped == text) return JsonStyle{indent, false};
        if (dumped + "\n" == text) return JsonStyle{indent, true};
    }
    return std::nullopt;
}
inline std::string json_dump(const nlohmann::ordered_json &json, const JsonStyle &style)
{
    return json.dump(style.indent) + (style.newline ? "\n" : "");
}

} // namespace Slic3r::native_parameters

#endif
