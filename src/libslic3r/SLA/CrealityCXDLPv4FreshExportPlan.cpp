// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CrealityCXDLPv4FreshExportPlan.hpp"
#include "ExposureMaskUnion.hpp"
#include "LayerMaskValidation.hpp"
#include "MachineSettings.hpp"
#include <charconv>
#include "libslic3r/Execution/ExecutionTBB.hpp"

namespace Slic3r::sla {
namespace {
// Format the actual finite values without locale separators or trailing float
// noise. The projected value is a native float; the request is the owned input.
template<class Number>
std::string diagnostic_number(Number value)
{
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (converted.ec != std::errc{})
        throw RuntimeError("Cannot format a CXDLPv4 parameter diagnostic");
    return std::string(buffer, converted.ptr);
}

[[noreturn]] void fail_bottom_projection(const FreshLayerJob &source,
    const CXDLPv4FreshExportPlan &plan, LayerParameter field, double represented)
{
    using F = LayerParameter;
    const char *key = nullptr;
    switch (field) {
    case F::RetractSpeed: key = "sla_bottom_retract_speed"; break;
    case F::RetractSpeed2: key = "sla_bottom_retract_speed2"; break;
    case F::RetractDistance2: key = "sla_bottom_retract_height2"; break;
    case F::WaitBefore: key = "sla_bottom_wait_before_cure"; break;
    case F::WaitAfter: key = "sla_bottom_wait_after_cure"; break;
    case F::WaitAfterLift: key = "sla_bottom_wait_after_lift"; break;
    default: break;
    }
    const char *label = cxdlpv4_fresh_detail::field_label(field);
    for (const auto &setting : machine_settings)
        if (key && std::string(setting.key) == key) { label = setting.label; break; }
    const std::string unit = cxdlpv4_fresh_detail::field_unit(field);
    std::string message = std::string("CXDLPv4 cannot preserve the bottom-layer setting \"") + label +
        "\" (bottom." + cxdlpv4_fresh_detail::field_id(field) + ").\nRequested: " +
        diagnostic_number(source.globals.bottom.at(field)) + " " + unit +
        ". Read back from CXDLPv4: " + diagnostic_number(float(represented)) + " " + unit + ".\n";
    if (plan.meaningful_bottom_layer) {
        message += "This value is derived from bottom layer " +
            std::to_string(*plan.meaningful_bottom_layer + 1) + ". ";
        if ((field == F::RetractSpeed || field == F::RetractSpeed2) &&
            cxdlpv4_public_round2(float(plan.rows[*plan.meaningful_bottom_layer].at(field))) <= 0)
            message += "A retract speed that rounds to zero is read back using a fallback speed, even when that phase has zero travel. ";
    } else {
        message += "No bottom layer has more than one lit pixel and exposure above 0.01 s after format rounding, so this setting is read back as 0. ";
    }
    // Plain text works in both the worker's error dialog and the layer editor.
    // Do not emit an HTML link: neither current caller installs a link handler.
    message += "\nFor model projects, review Materials > LCD exposure and motion > Bottom layers > ";
    message += label;
    message += ", then update slicing and export again. To keep the requested value, choose an output format that can preserve it.";
    throw RuntimeError(message);
}
} // namespace

CXDLPv4FreshExportPlan cxdlpv4_fresh_export_plan(const LayerDocument &document,
    const LayerProgress &progress) {
    using namespace cxdlpv4_fresh_detail;
    const size_t layer_count = document.layer_count();
    const auto checkpoint = [&](size_t done = 0) {
        check_layer_progress(progress, done, layer_count);
    };
    const auto validation_progress = [&](size_t, size_t) { checkpoint(); return true; };
    checkpoint();
    if (!document.fresh_job()) throw RuntimeError("CXDLPv4 fresh export requires an explicit owned descriptor");
    const auto &source = *document.fresh_job();
    source.validate(layer_count, validation_progress);
    if (source.globals.bottom_layers > UINT16_MAX || source.globals.transition_layers > UINT16_MAX)
        throw RuntimeError("CXDLPv4 bottom_layers/transition_layers exceed uint16 decoder range");
    CXDLPv4FreshExportPlan plan;
    plan.position_policy = source.position_policy;
    plan.geometry = source.geometry;
    plan.globals = source.globals;
    plan.globals.layer_height = double(float(source.globals.layer_height));
    if (!std::isfinite(plan.globals.layer_height) || plan.globals.layer_height < .001 ||
        float(source.globals.layer_height) != float(std::round(source.globals.layer_height * 1000.) / 1000.))
        throw RuntimeError("CXDLPv4 layer_height (mm) requires supported native 0.001 mm precision");
    plan.globals.normal = native_row(source.globals.normal, "normal");
    plan.globals.bottom = native_row(source.globals.bottom, "bottom");
    plan.rows.reserve(source.rows.size());
    ExposurePositionProjectionValidator positions(source.position_policy, .001);
    float complete_time = 300.f; // Reference preparation time, accumulated in native float.
    for (size_t i = 0; i < source.rows.size(); ++i) {
        checkpoint();
        const std::string scope = i < plan.globals.bottom_layers ? "bottom" : "layer[" + std::to_string(i) + "]";
        auto row = native_row(source.rows[i], scope);
        positions.append(source.rows[i].at(F::PositionZ), row.at(F::PositionZ));
        complete_time += public_row_seconds(row, scope);
        plan.rows.push_back(std::move(row));
    }
    auto represented = source;
    represented.globals = plan.globals;
    represented.rows = plan.rows;
    represented.validate(layer_count, validation_progress);
    if (!std::isfinite(complete_time))
        throw RuntimeError("CXDLPv4 expected_print_time (s) exceeds finite native float range");
    complete_time = cxdlpv4_public_round2(complete_time);
    if (complete_time < 0 || double(complete_time) > UINT32_MAX)
        throw RuntimeError("CXDLPv4 expected_print_time (s) exceeds its uint32 range");
    plan.expected_print_time = uint32_t(complete_time);
    CXDLPv4GeneratedCounts counts{};
    try {
        counts = cxdlpv4_generated_counts(document.layer_count(), int(plan.globals.bottom_layers),
            int(plan.globals.transition_layers), [&](size_t i) { return plan.rows[i].at(F::Exposure); });
    } catch (const ExportError &error) {
        throw RuntimeError(std::string("CXDLPv4 bottom_layers/transition_layers: ") + error.what());
    }
    if (counts.bottom != plan.globals.bottom_layers || counts.transition != plan.globals.transition_layers)
        throw RuntimeError("CXDLPv4 cannot preserve bottom_layers/transition_layers from owned exposure rows");

    LayerParameters virtual_bottom;
    for (F field : {F::RetractSpeed, F::RetractSpeed2, F::RetractDistance2,
                    F::WaitBefore, F::WaitAfter, F::WaitAfterLift}) virtual_bottom[field] = 0.;
    const double pixel_area = double(float(plan.geometry.width_mm)) / plan.geometry.columns *
        double(float(plan.geometry.height_mm)) / plan.geometry.rows;
    if (!std::isfinite(pixel_area) || pixel_area <= 0)
        throw RuntimeError("CXDLPv4 material_ml raster area (mm2) cannot be represented");
    // Reject a forged/mismatched PNG header before allocating worker images.
    // A private validated count belongs to these exact PNG bytes. Masks without
    // that evidence still undergo full PNG decoding and palette validation below.
    for (size_t i = 0; i < layer_count; ++i) {
        checkpoint();
        const auto bytes = document.encoded_layer(i);
        if (bytes.sz < 24 || !png::is_png(bytes))
            throw RuntimeError("Invalid layer PNG");
        const auto *data = static_cast<const uint8_t *>(bytes.buf);
        const auto dimension = [&](size_t offset) {
            return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
                (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
        };
        if (dimension(16) != plan.geometry.columns || dimension(20) != plan.geometry.rows)
            throw RuntimeError("CXDLPv4 pixels do not match owned raster geometry");
    }
    const uint64_t pixels = uint64_t(plan.geometry.columns) * plan.geometry.rows;
    // Allow one decoded mask plus decoder scratch per task. This budget is
    // for the scan's working images, not the entire application's memory.
    const size_t workers = std::clamp<uint64_t>((uint64_t(512) << 20) /
        std::max<uint64_t>(1, pixels * 2), 1, 8);
    tbb::task_arena arena{int(workers)};
    std::vector<size_t> lit_counts(layer_count);
    double volume_mm3 = 0., previous_z = 0.;
    for (size_t first = 0; first < layer_count; first += workers) {
        checkpoint(first);
        const size_t end = std::min(first + workers, layer_count);
        arena.execute([&] {
            execution::for_each(ExecutionTBB{}, first, end, [&](size_t i) {
                // Counts from another palette cannot prove NativeGray7 membership.
                // LayerDocument invalidates this evidence on every PNG replacement,
                // including Undo/Redo, and never restores it from project metadata.
                const auto cached = document.validated_mask_statistics(i);
                if (source.mask_precision == LayerMaskPrecision::NativeGray7 && cached &&
                    cached->columns == plan.geometry.columns && cached->rows == plan.geometry.rows &&
                    cached->nonzero_pixels <= pixels) {
                    lit_counts[i] = cached->nonzero_pixels;
                    return;
                }
                const auto mask = document.image(i);
                if (mask.cols != plan.geometry.columns || mask.rows != plan.geometry.rows)
                    throw RuntimeError("CXDLPv4 pixels do not match owned raster geometry");
                // Keep complete PNG decoding and share the proven palette /
                // nonzero scan used by document validation. Callback ownership
                // stays with the coordinator at the existing batch boundaries.
                const auto statistics = validate_and_count_mask_chunk(mask.buf.data(), mask.buf.size(),
                    LayerMaskPrecision::NativeGray7);
                if (!statistics.valid)
                    throw RuntimeError("CXDLPv4 cannot preserve grayscale pixels outside 0/odd 3..255");
                lit_counts[i] = statistics.nonzero_pixels;
            });
        });
        for (size_t i = first; i < end; ++i) {
            checkpoint(i);
            const size_t lit = lit_counts[i];
            const double z = plan.rows[i].at(F::PositionZ);
            volume_mm3 += lit * pixel_area * (z - previous_z);
            previous_z = z;
            if (!plan.meaningful_bottom_layer && i < plan.globals.bottom_layers && lit > 1 &&
                cxdlpv4_public_round2(float(plan.rows[i].at(F::Exposure))) > .01f) {
                const auto &row = plan.rows[i];
                const auto motion = public_motion(row, "bottom");
                const float global_second = float(plan.globals.bottom.at(F::LiftDistance2));
                const float global_total = float(plan.globals.bottom.at(F::LiftDistance) +
                    plan.globals.bottom.at(F::LiftDistance2));
                const float global_first = cxdlpv4_public_round2(std::max(0.f, global_total - global_second));
                const float global_cap = cxdlpv4_public_round2(global_first + global_second);
                auto retract_speed = [&](F field, float fallback) {
                    const float speed = cxdlpv4_public_round2(float(row.at(field)));
                    return double(cxdlpv4_public_round2(speed <= 0 ? fallback : speed));
                };
                virtual_bottom = {
                    {F::RetractSpeed, retract_speed(F::RetractSpeed,100.f)},
                    {F::RetractSpeed2, retract_speed(F::RetractSpeed2,80.f)},
                    {F::RetractDistance2, std::clamp(cxdlpv4_public_round2(motion.retract2),0.f,global_cap)},
                    {F::WaitBefore, cxdlpv4_public_round2(float(row.at(F::WaitBefore)))},
                    {F::WaitAfter, cxdlpv4_public_round2(float(row.at(F::WaitAfter)))},
                    {F::WaitAfterLift, cxdlpv4_public_round2(float(row.at(F::WaitAfterLift)))}};
                plan.meaningful_bottom_layer = i;
            }
        }
    }
    for (const auto &[field,value] : virtual_bottom)
        if (plan.globals.bottom.at(field) != value)
            fail_bottom_projection(source, plan, field, value);
    if (source.position_policy == ExposurePositionPolicy::RepeatedExposureHeights) {
        // A record is an exposure, not necessarily a new physical layer.
        // Union overlapping masks once per Z; never drop disjoint later masks.
        // Ordinary model exports retain their existing parallel count path.
        ExposureMaskUnion occupied{size_t(pixels)};
        volume_mm3 = 0.;
        double previous = 0.;
        for (size_t first = 0; first < layer_count;) {
            const double z = plan.rows[first].at(F::PositionZ);
            size_t end = first + 1;
            while (end < layer_count && plan.rows[end].at(F::PositionZ) == z) ++end;
            size_t count = lit_counts[first];
            if (end > first + 1) {
                occupied.reset();
                for (size_t i = first; i < end; ++i) {
                    checkpoint(i);
                    occupied.append(document.image(i).buf);
                }
                count = occupied.nonzero_pixels();
            }
            volume_mm3 += count * pixel_area * (z - previous);
            previous = z;
            first = end;
        }
    }
    plan.material_ml = float(volume_mm3 / 1000.);
    if (!std::isfinite(plan.material_ml) || plan.material_ml < 0 ||
        (volume_mm3 > 0 && plan.material_ml <= 0))
        throw RuntimeError("CXDLPv4 material_ml (ml) is outside native float precision");
    checkpoint(layer_count);
    return plan;
}
} // namespace Slic3r::sla
