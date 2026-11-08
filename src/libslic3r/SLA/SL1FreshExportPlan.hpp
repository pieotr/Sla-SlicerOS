// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include "LayerMaskValidation.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace Slic3r::sla {
struct SL1FreshExportPlan {
    LayerJobParameters globals;
    std::vector<LayerParameters> rows;
    LayerRasterGeometry geometry;
    double initial_layer_height = 0.;
    float expected_print_time = 0.f;
    double material_ml = 0.;
    size_t projected_transition_exposures = 0;
    double maximum_exposure_change = 0.;
};

// Transition confirmation describes metadata only. It is not a complete
// export plan: no pixels/material have been inspected and no export is authorized.
struct SL1ExposureTransitionPreview {
    size_t projected_transition_exposures = 0;
    double maximum_exposure_change = 0.;
};

namespace sl1_fresh_detail {
struct ProjectedSchedule {
    LayerJobParameters globals;
    std::vector<LayerParameters> rows;
    LayerRasterGeometry geometry;
    double initial_layer_height = 0.;
    float expected_print_time = 0.f;
    double pixel_area_mm2 = 0.;
    size_t projected_transition_exposures = 0;
    double maximum_exposure_change = 0.;
};
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
[[noreturn]] inline void fail(const std::string &scope, F field, const char *reason) {
    throw RuntimeError("SL1 cannot preserve " + scope + "." + field_id(field) +
        " (" + field_unit(field) + "): " + reason);
}
inline float round_even2(float value) {
    // MathF.Round leaves floats beyond its decimal-rounding limit unchanged.
    const float result = std::abs(value) >= 1e8f ? value : std::nearbyint(value * 100.f) / 100.f;
    if (!std::isfinite(result)) throw RuntimeError("Invalid SL1 public float projection");
    return result;
}
inline float round_away(float value, float scale) {
    const float result = std::abs(value) >= 1e8f ? value : std::round(value * scale) / scale;
    if (!std::isfinite(result)) throw RuntimeError("Invalid SL1 height/transition projection");
    return result;
}
inline LayerParameters native_values(const LayerParameters &source, const std::string &scope) {
    auto row = source;
    for (auto &[field, value] : row) {
        const double native = double(float(value));
        if (!std::isfinite(native) || (value > 0 && native <= 0))
            fail(scope, field, "outside native float precision");
        value = native;
    }
    return row;
}
inline LayerParameters globals(const LayerParameters &source, const std::string &scope) {
    auto values = native_values(source, scope);
    for (auto &[field, value] : values) {
        // INI exposure getters are raw floats. Note-based globals go through
        // their public setters before ResetParameters copies them into rows.
        if (field == F::Exposure || field == F::LightPWM) continue;
        const double represented = double(round_even2(float(value)));
        if (represented != value) fail(scope, field, "notes public precision changes the owned value");
        value = represented;
    }
    const float total = round_even2(float(values.at(F::LiftDistance)) + float(values.at(F::LiftDistance2)));
    if (values.at(F::RetractDistance2) > total)
        fail(scope, F::RetractDistance2, "public total would clamp the second retract phase");
    return values;
}
inline float row_seconds(const LayerParameters &row) {
    auto value = [&](F field) { return float(row.at(field)); };
    float motor = 0.f;
    auto phase = [&](float distance, F speed_field) {
        if (distance <= 0) return;
        const float speed = value(speed_field);
        if (!std::isfinite(speed) || speed <= 0)
            fail("row", speed_field, "moving phase requires a finite positive executable speed");
        motor += distance / (speed / 60.f);
    };
    phase(value(F::LiftDistance), F::LiftSpeed);
    phase(value(F::LiftDistance2), F::LiftSpeed2);
    phase(value(F::RetractDistance2), F::RetractSpeed2);
    // Reference calculator uses this raw float subtraction, not RetractHeight.
    phase(value(F::LiftDistance) + value(F::LiftDistance2) - value(F::RetractDistance2), F::RetractSpeed);
    const float rests = value(F::WaitBefore) + value(F::Exposure) + value(F::WaitAfter) + value(F::WaitAfterLift);
    if (!std::isfinite(motor) || !std::isfinite(rests))
        throw RuntimeError("SL1 expected_print_time (s) exceeds finite native float range");
    return rests + std::max(round_even2(motor), value(F::LightOff));
}
// Shared metadata projection. The complete planner and UI preview execute
// exactly the same native precision, schedule, timing and geometry checks.
inline ProjectedSchedule project_schedule(const FreshLayerJob &source, size_t count,
                                           const LayerProgress &progress) {
    source.validate(count, [&](size_t, size_t) {
        check_layer_progress(progress, 0, count); return true;
    });
    const size_t bottom = source.globals.bottom_layers;
    const size_t maximum_transition = bottom == 0 || count - bottom <= 1 ? 0 : count - bottom - 1;
    if (bottom > UINT16_MAX || source.globals.transition_layers > UINT16_MAX)
        throw RuntimeError("SL1 bottom_layers/transition_layers exceed uint16 metadata range");
    if (source.globals.transition_layers > maximum_transition)
        throw RuntimeError("SL1 transition_layers would be clamped by the executable reference count");
    ProjectedSchedule plan;
    plan.geometry = source.geometry;
    plan.globals = source.globals;
    const float height = float(source.globals.layer_height);
    if (!std::isfinite(height) || height < .001f || height != round_away(height, 1000.f) ||
        height != float(std::round(source.globals.layer_height * 1000.) / 1000.))
        throw RuntimeError("SL1 layer_height (mm) requires supported native 0.001 mm precision");
    plan.globals.layer_height = double(height);
    const float initial = float(source.rows.front().at(F::PositionZ));
    if (!std::isfinite(initial) || initial <= 0 || initial != round_away(initial, 1000.f))
        fail("layer[0]", F::PositionZ, "initial layer height requires native three-decimal precision");
    plan.initial_layer_height = double(initial);
    plan.globals.normal = globals(source.globals.normal, "normal");
    plan.globals.bottom = globals(source.globals.bottom, "bottom");
    const float bottom_exposure = float(plan.globals.bottom.at(F::Exposure));
    const float normal_exposure = float(plan.globals.normal.at(F::Exposure));
    const size_t transition = plan.globals.transition_layers;
    const float decrement = transition == 0 || bottom_exposure == normal_exposure ? 0.f :
        round_away((bottom_exposure - normal_exposure) / float(transition + 1), 100.f);
    const float offset = initial - height;
    float complete_time = 300.f, previous_base = 0.f;
    plan.rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        check_layer_progress(progress, 0, count);
        const std::string scope = "layer[" + std::to_string(i) + "]";
        auto row = i < bottom ? plan.globals.bottom : plan.globals.normal;
        const float base = round_away(float(i + 1) * height, 1000.f);
        // The reference constructs masks in parallel. Reject precision where
        // previous-layer and index-based initialization could disagree.
        if (i > 0 && round_away(previous_base + height, 1000.f) != base)
            fail(scope, F::PositionZ, "parallel reference initialization has ambiguous float positions");
        previous_base = base;
        row[F::PositionZ] = double(i == 0 ? initial : round_away(base + offset, 1000.f));
        if (i >= bottom && i - bottom < transition && decrement > 0)
            row[F::Exposure] = double(round_even2(std::clamp(
                bottom_exposure - decrement * float(i - bottom + 1), normal_exposure, bottom_exposure)));
        const auto desired = native_values(source.rows[i], scope);
        for (const auto &[field, value] : row)
            if (desired.at(field) != value) {
                // A global SL1 transition is executable only at the
                // reference's rounded step. Accept the canonical unrounded
                // producer curve; arbitrary per-row overrides still fail.
                if(field==F::Exposure && i>=bottom && i-bottom<transition && decrement>0) {
                    const double first=source.globals.bottom.at(F::Exposure);
                    const double last=source.globals.normal.at(F::Exposure);
                    const double ideal=first+(last-first)*double(i-bottom+1)/double(transition+1);
                    if(desired.at(field)==double(float(ideal))) {
                        ++plan.projected_transition_exposures;
                        plan.maximum_exposure_change=std::max(plan.maximum_exposure_change,
                            std::abs(value-source.rows[i].at(field)));
                        continue;
                    }
                }
                fail(scope, field, "owned row differs from the executable global/transition schedule");
            }
        complete_time += row_seconds(row);
        plan.rows.push_back(std::move(row));
    }
    auto represented = source;
    represented.globals = plan.globals; represented.rows = plan.rows;
    represented.validate(count, [&](size_t, size_t) {
        check_layer_progress(progress, 0, count); return true;
    });
    if (!std::isfinite(complete_time))
        throw RuntimeError("SL1 expected_print_time (s) exceeds finite native float range");
    plan.expected_print_time = round_even2(complete_time);
    const double pixel_area = double(float(plan.geometry.width_mm)) / plan.geometry.columns *
        double(float(plan.geometry.height_mm)) / plan.geometry.rows;
    if (!std::isfinite(pixel_area) || pixel_area <= 0)
        throw RuntimeError("SL1 material_ml raster area (mm2) cannot be represented");
    plan.pixel_area_mm2 = pixel_area;
    return plan;
}
inline ProjectedSchedule document_schedule(const LayerDocument &document,
                                           const LayerProgress &progress) {
    check_layer_progress(progress, 0, document.layer_count());
    if (!document.fresh_job()) throw RuntimeError("SL1 fresh export requires an explicit owned descriptor");
    return project_schedule(*document.fresh_job(), document.layer_count(), progress);
}

// Keep decoded images and decoder working memory bounded independently of the
// document size. Tiny masks stay sequential; additional workers would cost more
// than decoding them. The per-image bound matches LayerDocument::image.
inline size_t mask_batch_width(size_t columns, size_t rows, size_t remaining) {
    constexpr size_t maximum_mask = size_t(128) << 20;
    if (!columns || !rows || columns > 32768 || rows > 32768 || columns > maximum_mask / rows)
        throw RuntimeError("Layer dimensions exceed the editor memory budget");
    const size_t pixels = columns * rows;
    constexpr size_t decoder_budget = size_t(512) << 20;
    const size_t workers = pixels < (size_t(1) << 20) ? 1 :
        std::clamp(decoder_budget / (pixels * 4), size_t(1), size_t(4));
    return std::min(workers, remaining);
}

// Check the actual header before allocating a batch. A forged larger PNG must
// not bypass the budget by claiming smaller dimensions in the owned metadata.
// This preflight never authorizes pixels: image() still performs complete PNG
// decoding and validates every chunk CRC, including ancillary chunks and IEND.
inline void require_mask_dimensions(png::ReadBuf bytes, size_t columns, size_t rows) {
    if (bytes.sz < 33 || !png::is_png(bytes)) throw RuntimeError("Invalid layer PNG");
    const auto *data = static_cast<const uint8_t *>(bytes.buf);
    const auto word = [&](size_t offset) {
        return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset+1]) << 16) |
            (uint32_t(data[offset+2]) << 8) | uint32_t(data[offset+3]);
    };
    if (word(8) != 13 || std::memcmp(data+12, "IHDR", 4) != 0 || data[24] != 8 || data[25] != 0 ||
        word(16) != columns || word(20) != rows)
        throw RuntimeError("SL1 pixels do not match owned raster geometry");
}
} // namespace sl1_fresh_detail

// Metadata-only confirmation for SL1 and SL1S. The full export planner below
// still decodes/checks every PNG and computes material before publication.
inline SL1ExposureTransitionPreview sl1_exposure_transition_preview(const LayerDocument &document,
                                                                   const LayerProgress &progress = {}) {
    const auto schedule = sl1_fresh_detail::document_schedule(document, progress);
    check_layer_progress(progress, document.layer_count(), document.layer_count());
    return {schedule.projected_transition_exposures, schedule.maximum_exposure_change};
}

// Fresh intent is explicit and immutable. No per-layer metadata is invented
// for SL1: every row must equal the schedule its reference reader executes.
inline SL1FreshExportPlan sl1_fresh_export_plan(const LayerDocument &document,
                                              const LayerProgress &progress = {}) {
    auto schedule = sl1_fresh_detail::document_schedule(document, progress);
    SL1FreshExportPlan plan;
    plan.globals = std::move(schedule.globals);
    plan.rows = std::move(schedule.rows);
    plan.geometry = schedule.geometry;
    plan.initial_layer_height = schedule.initial_layer_height;
    plan.expected_print_time = schedule.expected_print_time;
    plan.projected_transition_exposures = schedule.projected_transition_exposures;
    plan.maximum_exposure_change = schedule.maximum_exposure_change;
    const double pixel_area = schedule.pixel_area_mm2;
    const size_t count = document.layer_count();
    const char *batch_flag = std::getenv("SLA_EXPERIMENT_SL1_PLAN_MASKS");
    // Enable the measured path on SSE2 targets; other architectures retain
    // serial accounting until equivalent whole-export measurements are available.
#if defined(__SSE2__)
    constexpr bool default_batched = true;
#else
    constexpr bool default_batched = false;
#endif
    const bool batched = batch_flag ? std::strcmp(batch_flag, "1") == 0 : default_batched;
    const size_t workers = batched ?
        sl1_fresh_detail::mask_batch_width(plan.geometry.columns, plan.geometry.rows, count) : 1;
    if (std::getenv("SLA_TRACE_EXPORT_STAGES"))
        BOOST_LOG_TRIVIAL(info) << "SL1 mask accounting: batched=" << (batched ? 1 : 0)
            << "; width=" << workers << "; layers=" << count;
    double volume = 0., previous_z = 0.;
    const auto account = [&](size_t i, const png::ImageGreyscale &mask) {
        if (mask.cols != plan.geometry.columns || mask.rows != plan.geometry.rows)
            throw RuntimeError("SL1 pixels do not match owned raster geometry");
        // Ordered physical accumulation and every 1MiB checkpoint stay on the
        // coordinator. User callbacks never execute on decoder worker threads.
        size_t lit = 0;
        constexpr size_t chunk_size = size_t(1) << 20;
        for (size_t offset = 0; offset < mask.buf.size(); offset += chunk_size) {
            check_layer_progress(progress, i, count);
            const size_t size = std::min(mask.buf.size() - offset, chunk_size);
            if (batched)
                lit += validate_and_count_mask_chunk(mask.buf.data() + offset, size,
                    LayerMaskPrecision::Gray8).nonzero_pixels;
            else
                lit += size_t(std::count_if(mask.buf.begin() + offset, mask.buf.begin() + offset + size,
                    [](uint8_t p) { return p != 0; }));
        }
        const double z = plan.rows[i].at(LayerParameter::PositionZ);
        volume += lit * pixel_area * (z - previous_z); previous_z = z;
    };
    if (!batched) {
        for (size_t i = 0; i < count; ++i) {
            check_layer_progress(progress, i, count);
            const auto mask = document.image(i);
            account(i, mask);
        }
    } else {
        tbb::task_arena arena{int(workers)};
        for (size_t first = 0; first < count; first += workers) {
            check_layer_progress(progress, first, count);
            const size_t end = std::min(first + workers, count);
            for (size_t i = first; i < end; ++i)
                sl1_fresh_detail::require_mask_dimensions(document.encoded_layer(i),
                    plan.geometry.columns, plan.geometry.rows);
            std::vector<png::ImageGreyscale> masks(end - first);
            // No caller callback runs while workers borrow document bytes.
            // A decoding error unwinds the batch before any counts are consumed.
            if (workers == 1) masks[0] = document.image(first);
            else arena.execute([&] {
                execution::for_each(ExecutionTBB{}, first, end, [&](size_t i) {
                    masks[i - first] = document.image(i);
                });
            });
            for (size_t i = first; i < end; ++i) {
                check_layer_progress(progress, i, count);
                account(i, masks[i - first]);
            }
        }
    }
    check_layer_progress(progress, count, count);
    plan.material_ml = volume / 1000.;
    if (!std::isfinite(plan.material_ml) || plan.material_ml < 0)
        throw RuntimeError("SL1 material_ml (ml) exceeds finite physical accounting range");
    return plan;
}
} // namespace Slic3r::sla
