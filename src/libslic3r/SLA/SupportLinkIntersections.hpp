// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"

#include <cstdint>
#include <limits>

namespace Slic3r::sla::detail {

inline uint64_t support_link_vertices(const Polygons &polygons)
{
    uint64_t count = 0;
    for (const auto &polygon : polygons)
        count += polygon.points.size();
    return count;
}

struct SupportLinkReductionInput {
    Polygons subject;
    Polygons clip;
    uint64_t original_clip_vertices = 0;
    bool fallback = false;
};

// Shared preparation for eligibility diagnostics and the bbox link kernel.
// Include every subject path: get_extents(ExPolygon) only covers its contour,
// while nonzero fill also observes independently wound paths outside it.
// The existing reducer retains path/vertex order and winding; it does not
// offset the polygons. Undefined or unsafe inflated bounds keep the full input.
inline SupportLinkReductionInput prepare_support_link_reduction(
    const ExPolygon &subject, const ExPolygon &clip)
{
    SupportLinkReductionInput result;
    result.subject = to_polygons(subject);
    result.original_clip_vertices = clip.contour.points.size();
    for (const auto &hole : clip.holes)
        result.original_clip_vertices += hole.points.size();

    const BoundingBox bounds = get_extents(result.subject);
    const coord_t margin = coord_t(SCALED_EPSILON);
    bool safe = bounds.defined;
    for (int axis = 0; axis < 2; ++axis) {
        if (bounds.min[axis] < std::numeric_limits<coord_t>::min() + margin ||
            bounds.max[axis] > std::numeric_limits<coord_t>::max() - margin)
            safe = false;
    }
    result.fallback = !safe;
    result.clip = safe ? ClipperUtils::clip_clipper_polygons_with_subject_bbox(
                            clip, bounds.inflated(SCALED_EPSILON)) :
                         to_polygons(clip);
    return result;
}

struct SupportLinkReductionProfile {
    uint64_t pairs = 0;
    uint64_t bbox_rejected = 0;
    uint64_t full_calls = 0;
    uint64_t nonempty = 0;
    uint64_t subject_vertices = 0;
    uint64_t clip_vertices = 0;
    uint64_t retained_clip_vertices = 0;
    uint64_t reduced_pairs = 0;
    uint64_t fallback_calls = 0;
    uint64_t clip_task_ns_sum = 0;

    void observe(const ExPolygon &subject, const ExPolygon &clip, bool has_intersection)
    {
        // Extra copies/reduction are outside the original intersection timer,
        // but remain part of phase wall time. They do not choose graph links.
        const auto reduced = prepare_support_link_reduction(subject, clip);
        ++full_calls;
        nonempty += has_intersection;
        subject_vertices += support_link_vertices(reduced.subject);
        clip_vertices += reduced.original_clip_vertices;
        const auto retained = support_link_vertices(reduced.clip);
        retained_clip_vertices += retained;
        reduced_pairs += retained < reduced.original_clip_vertices;
        fallback_calls += reduced.fallback;
    }

    void append(const SupportLinkReductionProfile &other)
    {
        pairs += other.pairs;
        bbox_rejected += other.bbox_rejected;
        full_calls += other.full_calls;
        nonempty += other.nonempty;
        subject_vertices += other.subject_vertices;
        clip_vertices += other.clip_vertices;
        retained_clip_vertices += other.retained_clip_vertices;
        reduced_pairs += other.reduced_pairs;
        fallback_calls += other.fallback_calls;
        clip_task_ns_sum += other.clip_task_ns_sum;
    }
};

// Reuse the shared Clipper reducer without changing fill,
// safety-offset flags or output order. The caller owns the existing pre-pair
// and post-intersection cancellation checkpoints; no callbacks are added here.
// Scratch copies are local to one pair, not a global cap across TBB workers.
inline Polygons intersect_support_link_bbox(const ExPolygon &subject, const ExPolygon &clip)
{
    const auto reduced = prepare_support_link_reduction(subject, clip);
    if (reduced.fallback)
        return intersection(subject, clip, ApplySafetyOffset::No);
    return intersection(reduced.subject, reduced.clip, ApplySafetyOffset::No);
}

} // namespace Slic3r::sla::detail
