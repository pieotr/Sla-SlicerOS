// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExposureFinderTable.hpp"
#include "RasterRounding.hpp"
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <tuple>
#include <map>

namespace Slic3r::sla {
namespace {
using Decimal = boost::multiprecision::cpp_dec_float_50;
constexpr size_t maximum_rows = 100000;
constexpr double maximum_magnitude = 1000000.;
using Key = std::tuple<double, double, double, uint8_t>;
Key key(const ExposureFinderTableRow &row) {
    return {row.layer_height, row.bottom_exposure, row.normal_exposure, row.brightness};
}
Decimal decimal(double value) {
    if (!std::isfinite(value) || std::abs(value) > maximum_magnitude)
        throw RuntimeError("ExposureFinder table values must be finite and within +/-1000000");
    // The decimal controls/reference operate on decimal numbers, not the exact
    // binary fraction hidden in a double (2.675 must round to 2.68 seconds).
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) throw RuntimeError("Cannot encode ExposureFinder decimal input");
    return Decimal(std::string(buffer, result.ptr));
}
Decimal rounded(const Decimal &value, unsigned scale, bool ties_even) {
    if (abs(value) > maximum_magnitude)
        throw RuntimeError("ExposureFinder generated value exceeds +/-1000000");
    const Decimal magnitude = abs(value) * scale;
    Decimal integral = floor(magnitude);
    const Decimal fraction = magnitude - integral;
    if (fraction > Decimal("0.5") ||
        (fraction == Decimal("0.5") && (!ties_even || fmod(integral, Decimal(2)) != 0)))
        ++integral;
    return (value < 0 ? -integral : integral) / scale;
}
ExposureFinderTableRow row(const Decimal &height, const Decimal &bottom,
                          const Decimal &normal, uint8_t brightness = 255) {
    return {rounded(height, 1000, false).convert_to<double>(),
            rounded(bottom, 100, true).convert_to<double>(),
            rounded(normal, 100, true).convert_to<double>(), brightness};
}
} // namespace
bool ExposureFinderTableRow::operator==(const ExposureFinderTableRow &other) const {
    return key(*this) == key(other);
}
std::vector<ExposureFinderTableRow> generate_exposure_finder_table(
    const ExposureFinderTableParameters &parameters, const LayerProgress &progress) {
    const auto plan = parameters;
    ScopedNearestRounding rounding;
    check_layer_progress(progress);
    if (plan.series != ExposureFinderSeries::Linear && plan.series != ExposureFinderSeries::Multiplier)
        throw RuntimeError("Unknown ExposureFinder exposure series");
    if (plan.tests > 255) throw RuntimeError("ExposureFinder test count exceeds 255");
    const Decimal first_height = rounded(decimal(plan.layer_height), 1000, false);
    const Decimal maximum = decimal(plan.maximum_layer_height);
    const Decimal step = decimal(plan.layer_height_step);
    const Decimal bottom = rounded(decimal(plan.bottom_exposure), 100, true);
    const Decimal normal = rounded(decimal(plan.normal_exposure), 100, true);
    const Decimal bottom_step = rounded(decimal(plan.bottom_step), 100, true);
    const Decimal normal_step = rounded(decimal(plan.normal_step), 100, true);
    if (first_height <= 0 || step <= 0)
        throw RuntimeError("ExposureFinder requires positive layer height and height step");
    const Decimal last_height = plan.multiple_layer_heights ? maximum : first_height;
    if (last_height < first_height) {
        check_layer_progress(progress); return {};
    }
    const Decimal count = floor((last_height - first_height) / step) + 1;
    const size_t tests_per_height = plan.tests + size_t(plan.include_base);
    if (count > maximum_rows || count * tests_per_height > maximum_rows)
        throw RuntimeError("ExposureFinder table exceeds the 100000-row planning budget");
    const size_t heights = count.convert_to<size_t>(), total = heights * tests_per_height;
    std::vector<ExposureFinderTableRow> rows;
    rows.reserve(total);
    std::set<Key> seen;
    size_t done = 0;
    for (size_t index = 0; index < heights; ++index) {
        check_layer_progress(progress, done, total);
        const Decimal height = first_height + step * index;
        const auto append = [&](const Decimal &bottom_value, const Decimal &normal_value, bool keep_base = false) {
            check_layer_progress(progress, done, total);
            auto entry = row(height, bottom_value, normal_value);
            const bool unique = seen.insert(key(entry)).second;
            if (keep_base || unique) rows.push_back(entry);
            ++done;
        };
        // Reference adds the base unconditionally for every raw height step.
        // Sub-micron steps can normalize to the same height; preserve these
        // base rows, while generated test duplicates are still skipped.
        if (plan.include_base) append(bottom, normal, true);
        for (unsigned test = 1; test <= plan.tests; ++test) {
            // This is the reference multiplier, including layer height. It is
            // not a percentage multiplier and does not compound between tests.
            Decimal next_bottom = bottom + bottom_step * test;
            Decimal next_normal = normal + normal_step * test;
            if (plan.series == ExposureFinderSeries::Multiplier) {
                next_bottom = bottom + bottom * height * bottom_step * test;
                next_normal = normal + normal * height * normal_step * test;
            }
            append(next_bottom, next_normal);
        }
    }
    check_layer_progress(progress, total, total);
    return rows;
}
std::vector<ExposureFinderTableRow> normalize_exposure_finder_table(
    const std::vector<ExposureFinderTableRow> &source, const LayerProgress &progress) {
    ScopedNearestRounding rounding;
    if (source.size() > maximum_rows) throw RuntimeError("ExposureFinder table exceeds 100000 rows");
    const auto input = source;
    std::vector<ExposureFinderTableRow> rows;
    rows.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        check_layer_progress(progress, i, input.size());
        const auto &entry = input[i];
        rows.push_back(row(decimal(entry.layer_height), decimal(entry.bottom_exposure),
                           decimal(entry.normal_exposure), entry.brightness));
    }
    check_layer_progress(progress, input.size(), input.size());
    return rows;
}
namespace {
void validate_finder_table(const ExposureFinderRecipe &recipe, const LayerProgress &progress) {
    check_layer_progress(progress);
    if (recipe.table_origin != ExposureFinderTableOrigin::Generated && recipe.table_origin != ExposureFinderTableOrigin::Manual)
        throw RuntimeError("Unknown ExposureFinder table origin");
    if (recipe.rows.empty() || recipe.rows.size() > maximum_rows)
        throw RuntimeError("ExposureFinder recipe requires 1-100000 table rows");
    if (normalize_exposure_finder_table(recipe.rows, progress) != recipe.rows)
        throw RuntimeError("ExposureFinder recipe table is not normalized");
    if (!std::any_of(recipe.rows.begin(), recipe.rows.end(), [](const auto &entry) { return entry.valid(); }))
        throw RuntimeError("ExposureFinder recipe has no valid test row");
    const auto generated = generate_exposure_finder_table(recipe.table_parameters, progress);
    if (recipe.table_origin == ExposureFinderTableOrigin::Generated && generated != recipe.rows)
        throw RuntimeError("ExposureFinder generated table differs from its parameters");
}
} // namespace
void ExposureFinderRecipe::validate(const LayerProgress &progress) const {
    if(rows.size()>maximum_rows)throw RuntimeError("ExposureFinder recipe row budget exceeded");
    if(generation)generation->validate();
    const auto frozen=*this;
    if (frozen.schedule) (void)plan_exposure_finder_schedule(frozen, *frozen.schedule, progress);
    else validate_finder_table(frozen, progress);
    if(frozen.generation) {
        if(!frozen.schedule)throw RuntimeError("Full ExposureFinder recipe requires its schedule");
        frozen.generation->validate();
    }

}
std::vector<ExposureFinderTableRow> sanitize_exposure_finder_table(
    const std::vector<ExposureFinderTableRow> &source, const LayerProgress &progress) {
    ScopedNearestRounding rounding;
    if (source.size() > maximum_rows)
        throw RuntimeError("ExposureFinder manual table exceeds 100000 rows");
    const auto input = source; // Progress callbacks may change the caller draft.
    std::vector<ExposureFinderTableRow> rows;
    rows.reserve(input.size());
    std::set<Key> seen;
    for (size_t i = 0; i < input.size(); ++i) {
        check_layer_progress(progress, i, input.size());
        const auto &entry = input[i];
        auto normalized = row(decimal(entry.layer_height), decimal(entry.bottom_exposure),
                              decimal(entry.normal_exposure), entry.brightness);
        if (seen.insert(key(normalized)).second) rows.push_back(normalized);
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return key(a) < key(b); });
    check_layer_progress(progress, input.size(), input.size());
    return rows;
}
ExposureFinderSchedule plan_exposure_finder_schedule(const ExposureFinderRecipe &source,
    const ExposureFinderScheduleParameters &parameters, const LayerProgress &progress) {
    if (source.rows.size() > maximum_rows || parameters.brightness.size() > 256)
        throw RuntimeError("ExposureFinder schedule input exceeds its row/brightness budget");
    const auto recipe = source;
    const auto settings = parameters;
    ScopedNearestRounding rounding;
    const auto checkpoint = [&] { check_layer_progress(progress); };
    checkpoint();
    validate_finder_table(recipe, [&](size_t, size_t) { checkpoint(); return true; });
    if (int(settings.base_mode) < 0 || int(settings.base_mode) > 4 ||
        int(settings.brightness_exclusion) < 0 || int(settings.brightness_exclusion) > 2)
        throw RuntimeError("Unknown ExposureFinder base or brightness mode");
    if (settings.bottom_layers > UINT16_MAX)
        throw RuntimeError("ExposureFinder bottom layer count exceeds 65535");
    const Decimal base = rounded(decimal(settings.base_height), 100, true);
    const Decimal features = rounded(decimal(settings.features_height), 100, true);
    const Decimal custom = rounded(decimal(settings.custom_base_exposure), 100, true);
    const Decimal lift = rounded(decimal(settings.repeated_lift_height), 100, true);
    const Decimal wait = rounded(decimal(settings.repeated_wait_before), 100, true);
    if (base < 0 || features < 0 || base + features <= 0 || lift < 0 || wait < 0)
        throw RuntimeError("ExposureFinder requires nonnegative heights/motion and positive total height");
    const int64_t base_um = (base * 1000).convert_to<int64_t>();
    const int64_t total_um = ((base + features) * 1000).convert_to<int64_t>();
    const auto &table = recipe.table_parameters;
    const Decimal first = rounded(decimal(table.layer_height), 1000, false);
    const Decimal step = decimal(table.layer_height_step);
    const Decimal last = table.multiple_layer_heights ? decimal(table.maximum_layer_height) : first;
    if (last < first) throw RuntimeError("ExposureFinder requires a nonempty height range");
    const size_t height_count = (floor((last - first) / step) + 1).convert_to<size_t>();
    if (height_count > 1 && step * 1000 != floor(step * 1000))
        throw RuntimeError("ExposureFinder physical height steps require a 0.001 mm grid");
    if (!height_count || height_count > maximum_rows)
        throw RuntimeError("ExposureFinder height count exceeds the schedule budget");
    std::set<uint8_t, std::greater<uint8_t>> levels;
    if (settings.multiple_brightness) {
        for (uint8_t value : settings.brightness) if (value) levels.insert(value);
        if (levels.empty()) throw RuntimeError("ExposureFinder brightness requires a value in 1-255");
    } else levels.insert(255);
    struct Candidate { std::optional<size_t> source_row; ExposureFinderTableRow values; };
    std::map<int64_t, std::vector<Candidate>> available, groups;
    if (settings.multiple_exposures)
        for (size_t i = 0; i < recipe.rows.size(); ++i) {
            checkpoint();
            const auto &entry = recipe.rows[i];
            if (entry.valid()) available[std::llround(entry.layer_height * 1000)].push_back({i, entry});
        }
    uint64_t work = 0;
    constexpr size_t maximum_work = 1000000;
    // Use sparse physical events, not one iteration per micron of empty space.
    std::map<int64_t, std::vector<int64_t>> events;
    for (size_t i = 0; i < height_count; ++i) {
        checkpoint();
        const int64_t height = ((first + step * i) * 1000).convert_to<int64_t>();
        auto &candidates = groups[height];
        if (settings.multiple_exposures) {
            const auto found = available.find(height);
            if (found == available.end()) throw RuntimeError("ExposureFinder has no valid test for a selected height");
            candidates = found->second;
        } else candidates.push_back({{}, row(Decimal(height) / 1000,
            rounded(decimal(table.bottom_exposure), 100, true), rounded(decimal(table.normal_exposure), 100, true))});
        const uint64_t count = uint64_t(total_um / height);
        work += count * candidates.size() * levels.size();
        if (work > maximum_work) throw RuntimeError("ExposureFinder schedule exceeds 1000000 part placements");
        for (int64_t z = height; z <= total_um; z += height) {
            checkpoint();
            events[z].push_back(height);
            if (events.size() > maximum_rows) throw RuntimeError("ExposureFinder exceeds 100000 physical heights");
        }
    }
    double base_exposure = custom.convert_to<double>();
    if (settings.base_mode == ExposureFinderBaseMode::First) base_exposure = recipe.rows.front().normal_exposure;
    if (settings.base_mode == ExposureFinderBaseMode::Middle) base_exposure = recipe.rows[recipe.rows.size() / 2].normal_exposure;
    if (settings.base_mode == ExposureFinderBaseMode::Last) base_exposure = recipe.rows.back().normal_exposure;
    ExposureFinderSchedule result;
    std::map<Key, size_t> part_ids;
    std::set<size_t> membership;
    size_t done = 0;
    for (const auto &[z_um, heights] : events) {
        checkpoint();
        const double z = double(z_um) / 1000.;
        for (const int64_t height : heights) {
            const bool bottom = z_um / height <= settings.bottom_layers;
            const bool is_base = z_um <= base_um;
            const bool excluded = settings.brightness_exclusion == ExposureFinderBrightnessExclusion::Bottom ? bottom :
                settings.brightness_exclusion == ExposureFinderBrightnessExclusion::BottomAndBase && (bottom || is_base);
            for (const auto &candidate : groups.at(height)) {
                const auto &entry = candidate.values;
                double exposure = bottom ? entry.bottom_exposure : entry.normal_exposure;
                if (!bottom && is_base && settings.multiple_exposures && settings.base_mode != ExposureFinderBaseMode::Iterative)
                    exposure = base_exposure;
                if (exposure <= 0) throw RuntimeError("ExposureFinder selected exposure must be positive");
                // Merge only adjacent compatible records, preserving manual order.
                // Apply the selected base policy also when only one test exists.
                const bool reuse = !result.exposures.empty() && result.exposures.back().position_z == z &&
                    result.exposures.back().layer_height == entry.layer_height && result.exposures.back().exposure == exposure;
                if (!reuse) {
                    if (result.exposures.size() >= maximum_rows)
                        throw RuntimeError("ExposureFinder exceeds 100000 exposure records");
                    result.exposures.push_back({z, entry.layer_height, exposure, bottom, is_base,
                        result.physical_layers, {}, {}, {}, {}});
                    membership.clear();
                    if (bottom) ++result.bottom_records;
                }
                for (uint8_t brightness : levels) {
                    check_layer_progress(progress, done, size_t(work));
                    const Key identity{entry.layer_height, entry.bottom_exposure, entry.normal_exposure, brightness};
                    auto found = part_ids.find(identity);
                    if (found == part_ids.end()) {
                        if (result.parts.size() >= maximum_rows) throw RuntimeError("ExposureFinder exceeds 100000 spatial parts");
                        const auto dose = [&](double seconds) {
                            return rounded(decimal(seconds) * brightness / 255, 100, true).convert_to<double>();
                        };
                        const size_t id = result.parts.size();
                        result.parts.push_back({candidate.source_row, entry.layer_height, entry.bottom_exposure,
                            entry.normal_exposure, brightness,
                            settings.multiple_brightness && settings.brightness_exclusion == ExposureFinderBrightnessExclusion::None ? dose(entry.bottom_exposure) : entry.bottom_exposure,
                            settings.multiple_brightness ? dose(entry.normal_exposure) : entry.normal_exposure});
                        found = part_ids.emplace(identity, id).first;
                    }
                    if (membership.insert(found->second).second) {
                        result.exposures.back().parts.push_back(found->second);
                        result.exposures.back().mask_brightness.push_back(excluded ? 255 : brightness);
                    }
                    ++done;
                }
            }
        }
        ++result.physical_layers;
    }
    if (result.exposures.empty()) throw RuntimeError("ExposureFinder produces no exposure within the requested height");
    for (size_t i = 1; i < result.exposures.size(); ++i) {
        checkpoint();
        if (settings.repeated_settings && result.exposures[i].position_z == result.exposures[i - 1].position_z) {
            if (settings.repeated_lift_enabled) result.exposures[i].lift_height_override = lift.convert_to<double>();
            if (settings.repeated_wait_enabled) result.exposures[i].wait_before_override = wait.convert_to<double>();
        }
    }
    check_layer_progress(progress, size_t(work), size_t(work));
    return result;
}
} // namespace Slic3r::sla
