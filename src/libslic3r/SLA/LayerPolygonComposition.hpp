// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "RasterRounding.hpp"

namespace Slic3r::sla {
struct LayerPolygonComposition {
    ExPolygons polygons;
    double model_area = 0.;
    double support_area = 0.;
};
namespace layer_composition_detail {
inline bool point_less(const Point &a, const Point &b)
{
    return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
}
inline bool ring_less(const Polygon &a, const Polygon &b)
{
    return std::lexicographical_compare(a.points.begin(), a.points.end(), b.points.begin(), b.points.end(), point_less);
}
inline void canonicalize_ring(Polygon &ring)
{
    const size_t count = ring.points.size();
    if (count < 2) return;
    // Minimal cyclic rotation (linear time), including repeated minimum
    // vertices. Preserve winding and every coordinate; this is not smoothing.
    size_t first = 0, second = 1, matched = 0;
    while (first < count && second < count && matched < count) {
        const auto &a = ring.points[(first + matched) % count];
        const auto &b = ring.points[(second + matched) % count];
        if (a == b) { ++matched; continue; }
        if (point_less(a, b)) {
            second += matched + 1;
            if (first == second) ++second;
        } else {
            first += matched + 1;
            if (first == second) ++first;
        }
        matched = 0;
    }
    const size_t start = std::min(first, second);
    std::rotate(ring.points.begin(), ring.points.begin() + start, ring.points.end());
}
inline void canonicalize(ExPolygons &polygons)
{
    for (auto &polygon : polygons) {
        canonicalize_ring(polygon.contour);
        for (auto &hole : polygon.holes) canonicalize_ring(hole);
        std::sort(polygon.holes.begin(), polygon.holes.end(), ring_less);
    }
    std::sort(polygons.begin(), polygons.end(), [](const ExPolygon &a, const ExPolygon &b) {
        if (ring_less(a.contour, b.contour)) return true;
        if (ring_less(b.contour, a.contour)) return false;
        return std::lexicographical_compare(a.holes.begin(), a.holes.end(), b.holes.begin(), b.holes.end(), ring_less);
    });
}
} // namespace layer_composition_detail
// Preserve the original composition stages and move consumed contours rather
// than duplicating every model/support polygon for the final union.
inline LayerPolygonComposition compose_layer_polygons(ExPolygons models, ExPolygons supports)
{
    const ScopedNearestRounding rounding;
    // Boolean intersections can retain different near-collinear vertices
    // when the same input rings arrive in another traversal order. Give the
    // kernel a stable representation before clipping, and return one too.
    layer_composition_detail::canonicalize(models);
    layer_composition_detail::canonicalize(supports);
    LayerPolygonComposition result;
    models = union_ex(models);
    layer_composition_detail::canonicalize(models);
    result.model_area = area(models);
    if (!supports.empty())
        supports = models.empty() ? union_ex(supports) : diff_ex(supports, models);
    layer_composition_detail::canonicalize(supports);
    result.support_area = area(supports);
    ExPolygons combined;
    combined.reserve(models.size() + supports.size());
    for (auto &polygon : models) combined.emplace_back(std::move(polygon));
    for (auto &polygon : supports) combined.emplace_back(std::move(polygon));
    result.polygons = union_ex(combined);
    layer_composition_detail::canonicalize(result.polygons);
    return result;
}
} // namespace Slic3r::sla
