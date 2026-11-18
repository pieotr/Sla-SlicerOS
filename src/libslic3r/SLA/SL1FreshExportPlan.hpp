// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerDocument.hpp"
#include "LayerMaskValidation.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace Slic3r::sla {
// ---------------------------------------------------------------------------
// Memory-aware worker sizing shared by every full-resolution mask loop
// (raster, statistics, planning, encoding, readback verification).
//
// WHY: a fixed "512 MiB / (pixels*4)" budget gave 3 workers at 8K and 2 at 12K
// on a 31 GB / 14-core machine, leaving most cores idle during PNG inflation.
// RAM budget policy: in-flight full-resolution buffers may use at most 25% of
// physical memory, additionally capped at 6 GiB so a huge-RAM host does not
// over-allocate for diminishing returns. 8 GB host -> 2 GiB, 16 GB -> 4 GiB.
inline uint64_t physical_memory_bytes() {
    static const uint64_t bytes = [] () -> uint64_t {
#if defined(_WIN32)
        MEMORYSTATUSEX status{}; status.dwLength = sizeof(status);
        return GlobalMemoryStatusEx(&status) ? uint64_t(status.ullTotalPhys) : 0;
#elif defined(__unix__) || defined(__APPLE__)
        const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
        return pages > 0 && page > 0 ? uint64_t(pages) * uint64_t(page) : 0;
#else
        return 0;
#endif
    }();
    return bytes;
}
inline uint64_t inflight_raster_budget(uint64_t physical = physical_memory_bytes()) {
    // Unknown RAM is treated as a small 4 GiB machine (1 GiB budget).
    if (physical == 0) physical = uint64_t(4) << 30;
    return std::min<uint64_t>(physical / 4, uint64_t(6) << 30);
}
// Pure sizing rule (unit-testable): as many workers as the budget fits, never
// more than the hardware threads or remaining items, never fewer than 1
// (except when nothing remains).
inline size_t memory_bounded_workers(uint64_t budget, uint64_t bytes_per_worker, size_t remaining, size_t hardware) {
    const uint64_t fit = std::max<uint64_t>(1, budget / std::max<uint64_t>(1, bytes_per_worker));
    return std::min<size_t>({size_t(std::min<uint64_t>(fit, SIZE_MAX)), std::max<size_t>(hardware, 1), remaining});
}
inline size_t memory_bounded_workers(uint64_t bytes_per_worker, size_t remaining) {
    return memory_bounded_workers(inflight_raster_budget(), bytes_per_worker, remaining,
        size_t(std::max(1, tbb::this_task_arena::max_concurrency())));
}
// Run body(i) on `workers` threads in batches of 4*workers. `before(first)` and
// `after(first,end)` run on the CALLING thread only (user callbacks must never
// run on workers), so cancellation latency is one batch. Dynamic scheduling
// inside a batch keeps the barrier tail to at most one task per batch.
// In-flight memory is bounded by `workers`, not by the batch size, as long as
// body() keeps no per-layer result larger than a few bytes.
template<class Body, class Before, class After>
void for_each_in_batches(size_t count, size_t workers, Body &&body, Before &&before, After &&after) {
    workers = std::max<size_t>(workers, 1);
    const size_t batch = workers * 4;
    tbb::task_arena arena{int(workers)};
    for (size_t first = 0; first < count; first += batch) {
        before(first);
        const size_t end = std::min(first + batch, count);
        if (workers == 1) for (size_t i = first; i < end; ++i) body(i);
        else arena.execute([&] { execution::for_each(ExecutionTBB{}, first, end, body); });
        after(first, end);
    }
}

// Barrier-free bounded parallel loop. body(i) runs on at most `workers`
// threads; items are handed out dynamically, so there is no per-batch tail
// where every thread but one idles (for_each_in_batches wastes about half an
// item per worker per batch, roughly 10% of an inflate-bound loop).
//
// on_progress(completed) runs ONLY on the calling thread: once before any work
// (completed == 0) and once after each finished item (1..count), so the number
// of checkpoints is deterministic and cancellation latency is one item. If it
// throws, remaining items are not started, in-flight items finish, and the
// exception is rethrown after all workers have stopped (never leaves a worker
// touching the caller's frame). A body exception is handled the same way and
// the first one is rethrown with its original type.
// In-flight memory is bounded by `workers`, exactly like for_each_in_batches.
template<class Body, class OnProgress>
void for_each_streaming(size_t count, size_t workers, Body &&body, OnProgress &&on_progress) {
    on_progress(size_t(0));
    workers = std::min(std::max<size_t>(workers, 1), count);
    if (workers <= 1) {
        for (size_t i = 0; i < count; ++i) { body(i); on_progress(i + 1); }
        return;
    }
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable wake;
    size_t completed = 0, running = workers;
    std::exception_ptr body_error;
    // No slot is reserved for the calling thread: it only waits and polls.
    tbb::task_arena arena{int(workers), 0};
    for (size_t w = 0; w < workers; ++w)
        arena.enqueue([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= count) break;
                try { body(i); }
                catch (...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!body_error) body_error = std::current_exception();
                    stop = true;
                    break;
                }
                std::lock_guard<std::mutex> lock(mutex);
                ++completed;
                wake.notify_one();
            }
            // Notify under the lock: the caller may destroy this frame the
            // moment it observes running == 0, so nothing is touched after.
            std::lock_guard<std::mutex> lock(mutex);
            --running;
            wake.notify_all();
        });
    std::exception_ptr callback_error;
    size_t reported = 0;
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        wake.wait(lock, [&] { return completed > reported || running == 0 || body_error; });
        while (reported < completed && !callback_error && !body_error) {
            ++reported;
            lock.unlock();
            try { on_progress(reported); }
            catch (...) { callback_error = std::current_exception(); stop = true; }
            lock.lock();
        }
        if (callback_error || body_error) {
            stop = true;
            wake.wait(lock, [&] { return running == 0; });
            break;
        }
        if (running == 0 && reported == completed) break;
    }
    lock.unlock();
    if (callback_error) std::rethrow_exception(callback_error);
    if (body_error) std::rethrow_exception(body_error);
}

// ---------------------------------------------------------------------------
// Lit-pixel counts taken while the rasterizer still holds the pixel buffer.
//
// WHY: statistics and the SL1 plan each inflated every full-resolution PNG
// only to count non-zero bytes (about 5.5 s wall / 76 CPU-s for 1100 8K layers),
// although the rasterizer had those pixels in memory a moment earlier.
// The ledger certifies "these exact PNG bytes were produced by our own encoder
// from a buffer with N lit pixels". It stores the byte length and a 64-bit
// hash of every encoded layer, so a layer that was edited, restored from a
// project or replaced afterwards no longer matches and takes the normal
// decode-and-count path. A PNG that did not come from the rasterizer (loaded
// archive, tampered bytes) has no entry and is always decoded and CRC-checked.
class MaskLitLedger {
public:
    MaskLitLedger(const LayerDocument &document, size_t columns, size_t rows,
                  std::vector<size_t> lit_counts)
        : m_columns(columns), m_rows(rows), m_entries(lit_counts.size())
    {
        if (lit_counts.size() != document.layer_count())
            throw RuntimeError("Lit-pixel ledger does not match the layer count");
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (lit_counts[i] == SIZE_MAX) continue; // not counted: always decoded later
            const auto png = document.encoded_layer(i);
            m_entries[i] = {png.sz, fingerprint(png), lit_counts[i]};
        }
    }
    size_t columns() const { return m_columns; }
    size_t rows() const { return m_rows; }
    // Count for `layer` only if `png` is byte-for-byte what was recorded.
    std::optional<size_t> lit_pixels(size_t layer, png::ReadBuf png) const {
        if (layer >= m_entries.size()) return std::nullopt;
        const auto &entry = m_entries[layer];
        if (!entry.bytes || entry.bytes != png.sz || entry.hash != fingerprint(png)) return std::nullopt;
        return entry.lit;
    }
private:
    struct Entry { size_t bytes = 0; uint64_t hash = 0; size_t lit = 0; };
    static uint64_t fingerprint(png::ReadBuf png) {
        return uint64_t(std::hash<std::string_view>{}(
            std::string_view(static_cast<const char *>(png.buf), png.sz)));
    }
    size_t m_columns, m_rows;
    std::vector<Entry> m_entries;
};

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
    // One decoded Gray8 image plus inflate/filter scratch is about 4 bytes/pixel.
    return pixels < (size_t(1) << 20) ? std::min<size_t>(1, remaining) :
        memory_bounded_workers(uint64_t(pixels) * 4, remaining);
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
                                              const LayerProgress &progress = {},
                                              const MaskLitLedger *rasterized = nullptr) {
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
    const auto account = [&](size_t i, size_t lit) {
        const double z = plan.rows[i].at(LayerParameter::PositionZ);
        volume += lit * pixel_area * (z - previous_z); previous_z = z;
    };
    if (!batched) {
        for (size_t i = 0; i < count; ++i) {
            check_layer_progress(progress, i, count);
            const auto mask = document.image(i);
            if (mask.cols != plan.geometry.columns || mask.rows != plan.geometry.rows)
                throw RuntimeError("SL1 pixels do not match owned raster geometry");
            size_t lit = 0;
            constexpr size_t chunk_size = size_t(1) << 20;
            for (size_t offset = 0; offset < mask.buf.size(); offset += chunk_size) {
                check_layer_progress(progress, i, count);
                const size_t size = std::min(mask.buf.size() - offset, chunk_size);
                lit += size_t(std::count_if(mask.buf.begin() + offset, mask.buf.begin() + offset + size,
                    [](uint8_t p) { return p != 0; }));
            }
            account(i, lit);
        }
    } else {
        // Each task decodes one PNG (full CRC/IEND validation inside image()),
        // validates its dimensions and stores only the lit-pixel count, so the
        // decoded image dies on the worker and memory is bounded by `workers`.
        // The ordered volume accumulation and progress callbacks stay on the
        // coordinator after each batch, preserving float-identical sums.
        std::vector<size_t> lit_counts(count);
        // Layers the rasterizer counted, and whose bytes still match the
        // ledger, need no inflate; their header geometry is still checked.
        std::vector<size_t> pending;
        std::vector<std::atomic<char>> ready(count);
        for (auto &flag : ready) flag.store(0, std::memory_order_relaxed);
        pending.reserve(count);
        const bool ledger_fits = rasterized && rasterized->columns() == plan.geometry.columns &&
                                 rasterized->rows() == plan.geometry.rows;
        for (size_t i = 0; i < count; ++i) {
            const auto png = document.encoded_layer(i);
            if (const auto lit = ledger_fits ? rasterized->lit_pixels(i, png) : std::nullopt) {
                sl1_fresh_detail::require_mask_dimensions(png, plan.geometry.columns, plan.geometry.rows);
                lit_counts[i] = *lit;
                ready[i].store(1, std::memory_order_relaxed);
            } else pending.push_back(i);
        }
        // Account strictly in layer order as soon as a layer's count exists, so
        // the sums stay float-identical to the serial path and progress is monotonic.
        size_t next = 0;
        const auto drain = [&] {
            for (; next < count && ready[next].load(std::memory_order_acquire); ++next) {
                check_layer_progress(progress, next, count);
                account(next, lit_counts[next]);
            }
        };
        drain();
        for_each_streaming(pending.size(), pending.empty() ? 1 : std::min(workers, pending.size()),
            [&](size_t index) {
                const size_t i = pending[index];
                sl1_fresh_detail::require_mask_dimensions(document.encoded_layer(i),
                    plan.geometry.columns, plan.geometry.rows);
                const auto mask = document.image(i);
                if (mask.cols != plan.geometry.columns || mask.rows != plan.geometry.rows)
                    throw RuntimeError("SL1 pixels do not match owned raster geometry");
                lit_counts[i] = validate_and_count_mask_chunk(mask.buf.data(), mask.buf.size(),
                    LayerMaskPrecision::Gray8).nonzero_pixels;
                ready[i].store(1, std::memory_order_release);
            },
            [&](size_t) {
                check_layer_progress(progress, next, count);
                drain();
            });
        drain();
    }
    check_layer_progress(progress, count, count);
    plan.material_ml = volume / 1000.;
    if (!std::isfinite(plan.material_ml) || plan.material_ml < 0)
        throw RuntimeError("SL1 material_ml (ml) exceeds finite physical accounting range");
    return plan;
}
} // namespace Slic3r::sla
