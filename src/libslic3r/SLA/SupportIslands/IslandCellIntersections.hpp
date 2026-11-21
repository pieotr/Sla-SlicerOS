// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/info.h>
#include <cfenv>
#include <functional>
#include <vector>
#include <limits>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
namespace Slic3r::sla {
// Bound clipping scratch concurrency by available CPUs and a local cap of16.
// Explicit experiment controls retain reproducible eight/sixteen-worker runs.
// Read once per process; this is not a global memory limit.
inline int island_cell_clip_arena_concurrency()
{
    static const int concurrency=[] {
        const char *value=std::getenv("SLA_EXPERIMENT_ISLAND_CLIP_WORKERS");
        if(value != nullptr && std::strcmp(value,"8")==0)return 8;
        if(value != nullptr && std::strcmp(value,"16")==0)return 16;
        return std::max(1,std::min(16,tbb::info::default_concurrency()));
    }();
    return concurrency;
}

// Diagnostic-only: one slot per independent clip, reduced after the barrier.
struct IslandCellClipProfile {
    uint64_t subject_setup_ns = 0;
    uint64_t island_setup_ns = 0;
    uint64_t execute_ns = 0;
    uint64_t destruction_ns = 0;
    uint64_t conversion_ns = 0;
    size_t subject_vertices = 0;
    size_t island_vertices = 0;
    size_t contour_vertices = 0;
    size_t hole_vertices = 0;
    size_t disjoint_holes = 0;
    size_t disjoint_hole_vertices = 0;
    bool completed = false;
};

// Mirrors ClipperUtils::intersection(Polygon, ExPolygon), with its exact
// providers, fill/operation flags and destruction-before-conversion order.
// Used only when an explicit profile output is supplied, never the normal path.
inline Polygons profile_island_cell_clip(const Polygon &cell,
    const ExPolygon &island, IslandCellClipProfile &profile)
{
    using Clock = std::chrono::steady_clock;
    const auto elapsed=[](Clock::time_point begin, Clock::time_point end) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count());
    };
    auto begin=Clock::now();
    ClipperLib::Paths paths;
    {
        ClipperLib::Clipper clipper;
        clipper.AddPaths(ClipperUtils::SinglePathProvider(cell.points),ClipperLib::ptSubject,true);
        auto now=Clock::now();profile.subject_setup_ns=elapsed(begin,now);begin=now;
        clipper.AddPaths(ClipperUtils::ExPolygonProvider(island),ClipperLib::ptClip,true);
        now=Clock::now();profile.island_setup_ns=elapsed(begin,now);begin=now;
        clipper.Execute(ClipperLib::ctIntersection,paths,ClipperLib::pftNonZero,ClipperLib::pftNonZero);
        now=Clock::now();profile.execute_ns=elapsed(begin,now);begin=now;
    }
    auto now=Clock::now();profile.destruction_ns=elapsed(begin,now);begin=now;
    auto result=to_polygons(std::move(paths));
    profile.conversion_ns=elapsed(begin,Clock::now());
    profile.subject_vertices=cell.points.size();
    profile.contour_vertices=island.contour.points.size();
    profile.hole_vertices=0;
    profile.disjoint_holes=0;
    profile.disjoint_hole_vertices=0;
    // Diagnostic counts only, after timing the exact original operation.
    // Touching boxes remain relevant; undefined/degenerate bounds cannot
    // establish exclusion. No clipping input is changed here.
    const BoundingBox cell_bounds(cell.points);
    for(const auto &hole:island.holes) {
        profile.hole_vertices+=hole.points.size();
        const BoundingBox hole_bounds(hole.points);
        if(cell_bounds.defined && hole_bounds.defined && !cell_bounds.overlap(hole_bounds)) {
            ++profile.disjoint_holes;
            profile.disjoint_hole_vertices+=hole.points.size();
        }
    }
    profile.island_vertices=profile.contour_vertices+profile.hole_vertices;
    profile.completed=true;
    return result;
}

// Reuse the application's existing outside-bbox polygon reduction and its
// epsilon. Explicit 0 retains full clipping for diagnostics and comparison.
inline bool island_cell_bbox_clip_enabled()
{
    static const bool enabled=[] {
        const char *value=std::getenv("SLA_EXPERIMENT_ISLAND_BBOX_CLIP");
        return value == nullptr || std::strcmp(value,"0")!=0;
    }();
    return enabled;
}

inline Polygons intersect_island_cell_bbox(const Polygon &cell,const ExPolygon &island,
                                         const std::function<void()> &check={})
{
    if(check)check();
    const BoundingBox box=get_extents(cell);
    const coord_t margin=coord_t(SCALED_EPSILON);
    bool safe=box.defined;
    for(int axis=0;axis<2;++axis)
        if(box.min[axis]<std::numeric_limits<coord_t>::min()+margin ||
           box.max[axis]>std::numeric_limits<coord_t>::max()-margin)safe=false;
    Polygons result;
    if(safe) {
        // Same preparation as intersection_clipped(), using its existing
        // ExPolygon overload to avoid copying the entire original island.
        const auto reduced=ClipperUtils::clip_clipper_polygons_with_subject_bbox(
            island,box.inflated(SCALED_EPSILON));
        if(check)check();
        result=intersection(Polygons{cell},reduced,ApplySafetyOffset::No);
    } else {
        // Undefined bounds or an unrepresentable inflation keep the original
        // operation; no signed-coordinate wrap or approximation is allowed.
        result=intersection(cell,island,ApplySafetyOffset::No);
    }
    if(check)check();
    return result;
}

// Independent clipping phase of one alignment iteration. Point movement and
// collision resolution remain ordered in the caller. Inputs are immutable.
inline std::vector<Polygons> intersect_island_cells(const Polygons &cells,
    const ExPolygon &island, const std::vector<uint8_t> &enabled,
    const std::function<void()> &throw_on_cancel = {},
    std::vector<IslandCellClipProfile> *profile = nullptr)
{
    if (enabled.size() != cells.size()) throw RuntimeError("Cell activity mask size mismatch");
    if (throw_on_cancel) throw_on_cancel();
    if(profile && island_cell_bbox_clip_enabled())
        throw RuntimeError("Island bbox clipping cannot be combined with detailed clip profiling");
    std::vector<Polygons> result(cells.size());
    if(profile)profile->assign(cells.size(),IslandCellClipProfile{});
    std::fenv_t caller_environment;
    if (std::fegetenv(&caller_environment) != 0)
        throw RuntimeError("Cannot capture clipping floating-point environment");
    const auto clip = [&](size_t i) {
        if (throw_on_cancel) throw_on_cancel();
        if (!enabled[i] || cells[i].size() < 3) return;
        // CGAL and other geometry stages may change rounding modes. Each
        // independent clip must use the caller's environment, then restore
        // the worker's state, including when clipping throws.
        struct EnvironmentGuard {
            std::fenv_t previous;
            explicit EnvironmentGuard(const std::fenv_t &environment) {
                if (std::fegetenv(&previous) != 0 || std::fesetenv(&environment) != 0)
                    throw RuntimeError("Cannot set clipping floating-point environment");
            }
            ~EnvironmentGuard() { std::fesetenv(&previous); }
        } guard(caller_environment);
        if(profile)result[i]=profile_island_cell_clip(cells[i],island,(*profile)[i]);
        else if(island_cell_bbox_clip_enabled())result[i]=intersect_island_cell_bbox(cells[i],island);
        else result[i] = intersection(cells[i], island);
    };
    if (cells.size() < 16) {
        for (size_t i = 0; i < cells.size(); ++i) clip(i);
    } else {
        // Bound simultaneous Clipper allocations; parallelize only clipping,
        // not point movement or the subsequent ordered collision handling.
        tbb::task_arena arena(island_cell_clip_arena_concurrency());
        arena.execute([&] { execution::for_each(ex_tbb, size_t(0), cells.size(), clip, 8); });
    }
    return result;
}
}
