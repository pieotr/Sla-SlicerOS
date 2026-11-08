// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef slic3r_ClipperOffsetProfile_hpp_
#define slic3r_ClipperOffsetProfile_hpp_

#include "ClipperUtils.hpp"
#include <array>
#include <cstdint>

namespace Slic3r {
// Optional per-call diagnostics. Each parallel caller owns its own counters;
// intervals may overlap across callers and must not be reported as CPU time.
struct ClipperOffsetProfile {
    enum Stage { Outer, Holes, Combine, Union, Tree, Count };
    std::array<int64_t, Count> elapsed_ns{};
    std::array<uint64_t, Count> calls{};
    uint64_t hole_offsets = 0;
};

// Run the same ExPolygons kernel and accumulate its completed stages.
// The ordinary offset_ex instantiation contains no clocks or counters.
ExPolygons offset_ex_profiled(const ExPolygons &polygons, float delta,
    ClipperOffsetProfile &profile, ClipperLib::JoinType join_type = DefaultJoinType,
    double miter_limit = DefaultMiterLimit);
} // namespace Slic3r
#endif
