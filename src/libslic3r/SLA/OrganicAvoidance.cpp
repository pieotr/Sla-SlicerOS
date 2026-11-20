#include "OrganicAvoidance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <tbb/parallel_for.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r { namespace sla {

namespace {

struct Canceled {};

constexpr double SIMPLIFY_TOL = 0.02; // mm, polygon simplification of the cached areas
constexpr double ARC_TOL      = 0.05; // mm, tolerance of round offsets

ExPolygons simplified(ExPolygons &&in)
{
    ExPolygons out;
    out.reserve(in.size());
    for (const ExPolygon &ep : in)
        for (ExPolygon &s : ep.simplify(scaled(SIMPLIFY_TOL)))
            out.emplace_back(std::move(s));
    return out;
}

ExPolygons grow(const ExPolygons &in, double delta_mm)
{
    if (in.empty())
        return {};
    // + SIMPLIFY_TOL so the simplification can only make the area bigger
    return simplified(offset_ex(in, float(scaled(delta_mm + SIMPLIFY_TOL)),
                                ClipperLib::jtRound, scaled(ARC_TOL)));
}

} // namespace

void OrganicAvoidance::Region::finalize()
{
    bbs.clear();
    bbs.reserve(polys.size());
    for (const ExPolygon &ep : polys)
        bbs.emplace_back(get_extents(ep.contour));
}

bool OrganicAvoidance::Region::contains(const Point &p) const
{
    for (size_t i = 0; i < polys.size(); ++i)
        if (bbs[i].contains(p) && polys[i].contains(p))
            return true;
    return false;
}

size_t OrganicAvoidance::radius_class(double r) const
{
    size_t n = m_collision.size() - 1; // regular classes
    if (r <= m_p.thin_radius + 1e-9)
        return 0;
    if (n <= 1 || m_p.r_max <= m_p.r_min)
        return 1;
    double dr = (m_p.r_max - m_p.r_min) / double(n - 1);
    double c  = std::ceil((r - m_p.r_min) / dr - 1e-6);
    return 1 + size_t(std::clamp(c, 0., double(n - 1)));
}

double OrganicAvoidance::class_radius(size_t rc) const
{
    size_t n = m_collision.size() - 1;
    if (rc == 0)
        return m_p.thin_radius;
    if (n <= 1)
        return m_p.r_min;
    return m_p.r_min + double(rc - 1) * (m_p.r_max - m_p.r_min) / double(n - 1);
}

OrganicAvoidance::OrganicAvoidance(const indexed_triangle_set   &model,
                                   const Params                 &params,
                                   const std::function<bool()>  &cancelled)
    : m_p(params)
{
    if (m_p.step <= 0.)
        return;

    m_layers = size_t(std::max(1., std::ceil((m_p.top_z - m_p.ground_z) / m_p.step))) + 2;

    const size_t nclass = std::max<size_t>(1, m_p.classes) + 1; // + the thin class
    const size_t N      = m_layers;

    auto check = [&] { if (cancelled && cancelled()) throw Canceled{}; };

    try {
        // 1. Model outline for each layer (+ one below and above)
        std::vector<float> zs(N + 2);
        for (size_t i = 0; i < zs.size(); ++i)
            zs[i] = float(layer_z(i) + 1e-3); // not exactly on a horizontal face
        std::vector<ExPolygons> slices =
            slice_mesh_ex(model, zs, 0.005f, [&check] { check(); });

        // 2. The volume a branch has to stay away from on layer L is the union
        //    of the outlines of the layers L-1..L+1
        std::vector<ExPolygons> slab(N);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, N), [&](const tbb::blocked_range<size_t> &rg) {
            for (size_t L = rg.begin(); L < rg.end(); ++L) {
                if (cancelled && cancelled())
                    return;
                Polygons u;
                for (size_t k = (L == 0 ? 0 : L - 1); k <= L + 1 && k < slices.size(); ++k)
                    append(u, to_polygons(slices[k]));
                slab[L] = simplified(union_ex(u));
            }
        });
        check();
        slices.clear();

        // 3. Collision areas: slab grown by the class radius + safety gap
        m_collision.assign(nclass, std::vector<Region>(N));
        m_avoid.assign(nclass, std::vector<Region>(N));

        tbb::parallel_for(tbb::blocked_range<size_t>(0, nclass * N), [&](const tbb::blocked_range<size_t> &rg) {
            for (size_t idx = rg.begin(); idx < rg.end(); ++idx) {
                if (cancelled && cancelled())
                    return;
                size_t rc = idx / N, L = idx % N;
                double r  = class_radius(rc), gap = rc == 0 ? m_p.thin_gap : m_p.safety;
                // the pillar base is a cone: widest on the bed, as thick as the
                // pillar at the base height
                double above = layer_z(L) - m_p.ground_z;
                if (above < m_p.base_height && m_p.base_radius > r) {
                    double k = 1. - std::max(0., above) / std::max(m_p.base_height, 1e-6);
                    r += (m_p.base_radius - r) * k;
                    gap = std::max(gap, gap + (m_p.base_safety - gap) * k);
                }
                Region &reg = m_collision[rc][L];
                reg.polys = grow(slab[L], r + gap);
                reg.finalize();
            }
        });
        check();

        // 4. Avoidance: places from which the bed cannot be reached
        tbb::parallel_for(tbb::blocked_range<size_t>(0, nclass), [&](const tbb::blocked_range<size_t> &rg) {
            for (size_t rc = rg.begin(); rc < rg.end(); ++rc) {
                ExPolygons prev;
                for (size_t L = 0; L < N; ++L) {
                    if (cancelled && cancelled())
                        return;
                    Region &reg = m_avoid[rc][L];
                    if (L == 0) {
                        reg.polys = m_collision[rc][0].polys;
                    } else {
                        Polygons u = to_polygons(m_collision[rc][L].polys);
                        if (!prev.empty())
                            append(u, to_polygons(offset_ex(prev, -float(scaled(m_p.max_move)),
                                                            ClipperLib::jtRound, scaled(ARC_TOL))));
                        reg.polys = simplified(union_ex(u));
                    }
                    reg.finalize();
                    prev = reg.polys;
                }
            }
        });
        check();

        m_valid = true;
    } catch (const Canceled &) {
        m_valid = false;
    }
}

const OrganicAvoidance::Region &OrganicAvoidance::region(size_t layer, size_t rc, bool to_bed) const
{
    layer = std::min(layer, m_layers - 1);
    rc    = std::min(rc, m_collision.size() - 1);
    return to_bed ? m_avoid[rc][layer] : m_collision[rc][layer];
}

bool OrganicAvoidance::blocked(size_t layer, size_t rc, bool to_bed, const Vec2d &p) const
{
    return region(layer, rc, to_bed).contains(scaled(p).cast<coord_t>());
}

bool OrganicAvoidance::nearest_free(size_t layer, size_t rc, bool to_bed,
                                    const Vec2d &p, double max_dist, Vec2d &out) const
{
    const Region &reg = region(layer, rc, to_bed);
    const Vec2d   ps  = p * double(1. / SCALING_FACTOR);
    const double  maxd = max_dist / SCALING_FACTOR;

    if (!reg.contains(Point(coord_t(ps.x()), coord_t(ps.y())))) {
        out = p;
        return true;
    }

    // closest boundary point of the blocked region
    double best = std::numeric_limits<double>::max();
    Vec2d  bestp = ps, bestn = Vec2d::Zero();

    auto scan = [&](const Polygon &poly) {
        const Points &pts = poly.points;
        for (size_t i = 0; i < pts.size(); ++i) {
            Vec2d a = pts[i].cast<double>(), b = pts[(i + 1) % pts.size()].cast<double>();
            Vec2d ab = b - a;
            double l2 = ab.squaredNorm();
            double t  = l2 > 0 ? std::clamp((ps - a).dot(ab) / l2, 0., 1.) : 0.;
            Vec2d c   = a + t * ab;
            double d2 = (c - ps).squaredNorm();
            if (d2 < best) {
                best  = d2;
                bestp = c;
                double l = std::sqrt(l2);
                // outer contours are CCW, holes CW: the right hand normal always
                // points out of the blocked region
                bestn = l > 0 ? Vec2d(ab.y() / l, -ab.x() / l) : Vec2d::Zero();
            }
        }
    };

    for (size_t i = 0; i < reg.polys.size(); ++i) {
        const BoundingBox &bb = reg.bbs[i];
        if (!bb.contains(Point(coord_t(ps.x()), coord_t(ps.y()))))
            continue;
        if (!reg.polys[i].contains(Point(coord_t(ps.x()), coord_t(ps.y()))))
            continue;
        scan(reg.polys[i].contour);
        for (const Polygon &h : reg.polys[i].holes)
            scan(h);
    }

    if (std::sqrt(best) > maxd)
        return false;

    for (double push : {scaled(0.005), scaled(0.02), scaled(0.08)}) {
        Vec2d c = bestp + bestn * push;
        if (!reg.contains(Point(coord_t(c.x()), coord_t(c.y())))) {
            if ((c - ps).norm() > maxd * 1.1)
                return false;
            out = c * SCALING_FACTOR;
            return true;
        }
    }
    return false;
}

}} // namespace Slic3r::sla
