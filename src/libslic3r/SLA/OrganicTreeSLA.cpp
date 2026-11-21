#include "OrganicTreeSLA.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "libslic3r/Execution/ExecutionSeq.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include "libslic3r/SLA/SupportTree.hpp"
#include "libslic3r/SLA/SupportTreeMesher.hpp"
#include "libslic3r/SLA/SupportTreeUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace sla {

namespace {

constexpr size_t RING_STEPS   = 20;  // vertices of one tube cross-section
constexpr size_t SPHERE_STEPS = 24;
constexpr int    RELAX_ITERS  = 4;
constexpr double RELAX_LAMBDA = 0.5;
constexpr double SEGMENT_LEN  = 1.0; // target length of a tube segment [mm]

using Skel = TreeSkeleton;

// Does a tapered tube between two balls stay clear of the model?
bool tube_clear(const SupportableMesh &sm,
                const Vec3d &a, double ra,
                const Vec3d &b, double rb)
{
    double len = (b - a).norm();
    if (len < EPSILON)
        return true;

    Beam beam{Ball{a, ra}, Ball{b, rb}};
    auto hit = beam_mesh_hit(ex_seq, sm.emesh, beam, sm.cfg.safety_distance_mm);
    return hit.distance() > len;
}

// Slope of an edge (c above p) as dz / length, negative if it goes upwards.
double edge_ratio(const Vec3d &c, const Vec3d &p)
{
    double len = (c - p).norm();
    return len < EPSILON ? 1. : (c.z() - p.z()) / len;
}

bool node_edges_ok(const Skel &sk, const SupportableMesh &sm, int idx,
                   const Vec3d &newpos, const std::vector<double> &old_ratio)
{
    const Skel::Node &n = sk.nodes[size_t(idx)];

    // Edge to the parent (this node is the upper end)
    if (n.parent >= 0) {
        const Skel::Node &p = sk.nodes[size_t(n.parent)];
        double ratio = edge_ratio(newpos, p.pos);
        if (ratio < 0.8 * old_ratio[size_t(idx)] || ratio <= 0.05)
            return false;
        if (!tube_clear(sm, newpos, n.r, p.pos, p.r))
            return false;
    }

    // Edges to the children (this node is the lower end)
    for (int ci : n.child) {
        if (ci < 0)
            continue;
        const Skel::Node &c = sk.nodes[size_t(ci)];
        double ratio = edge_ratio(c.pos, newpos);
        if (ratio < 0.8 * old_ratio[size_t(ci)] || ratio <= 0.05)
            return false;
        if (!tube_clear(sm, c.pos, c.r, newpos, n.r))
            return false;
    }

    return true;
}

// Laplacian relaxation of the interior skeleton nodes. Leaves (heads), roots
// (pillars) and anchors stay where the routing put them since the head and the
// ground connection were built around these positions.
size_t relax_skeleton(Skel &sk, const SupportableMesh &sm, const JobController &ctl)
{
    std::vector<double> old_ratio(sk.nodes.size(), 1.);
    for (size_t i = 0; i < sk.nodes.size(); ++i)
        if (sk.nodes[i].parent >= 0)
            old_ratio[i] = edge_ratio(sk.nodes[i].pos,
                                      sk.nodes[size_t(sk.nodes[i].parent)].pos);

    std::vector<bool> moved(sk.nodes.size(), false);

    for (int it = 0; it < RELAX_ITERS; ++it) {
        for (size_t i = 0; i < sk.nodes.size(); ++i) {
            if (ctl.stopcondition())
                return 0;

            Skel::Node &n = sk.nodes[i];
            // fixed: leaves, roots, anchors
            if (n.parent < 0 || n.num_children() == 0 || n.anchor)
                continue;

            Vec3d sum = sk.nodes[size_t(n.parent)].pos;
            int   cnt = 1;
            for (int ci : n.child)
                if (ci >= 0) {
                    sum += sk.nodes[size_t(ci)].pos;
                    ++cnt;
                }

            Vec3d target = sum / double(cnt);
            Vec3d newpos = n.pos + RELAX_LAMBDA * (target - n.pos);

            if ((newpos - n.pos).norm() < 1e-4)
                continue;

            if (node_edges_ok(sk, sm, int(i), newpos, old_ratio)) {
                n.pos    = newpos;
                moved[i] = true;
            }
        }
    }

    return size_t(std::count(moved.begin(), moved.end(), true));
}

Vec3d safe_normalized(const Vec3d &v, const Vec3d &fallback)
{
    double l = v.norm();
    return l < EPSILON ? fallback : Vec3d(v / l);
}

// Tangent of the branch at the node `idx`, oriented downwards (child -> parent).
// `from_child` tells which neighbour the curve is coming from, -1 if unknown.
Vec3d node_tangent(const Skel &sk, int idx, int via_child)
{
    const Skel::Node &n = sk.nodes[size_t(idx)];
    if (n.parent < 0) {
        // root: continue in the direction the branch arrived
        if (via_child >= 0)
            return safe_normalized(n.pos - sk.nodes[size_t(via_child)].pos, DOWN);
        return DOWN;
    }

    const Vec3d &pp = sk.nodes[size_t(n.parent)].pos;
    if (n.num_children() == 1 || via_child < 0) {
        // chain node (or leaf): central difference
        int ci = via_child >= 0 ? via_child : n.child[0];
        if (ci >= 0)
            return safe_normalized(pp - sk.nodes[size_t(ci)].pos, DOWN);
        return safe_normalized(pp - n.pos, DOWN);
    }

    // merge node: both child branches flow into the trunk direction
    return safe_normalized(pp - n.pos, DOWN);
}

struct Curve
{
    std::vector<Vec3d>  pts;
    std::vector<double> radii;
};

Curve make_curve(const Vec3d &p0, double r0, const Vec3d &t0,
                 const Vec3d &p1, double r1, const Vec3d &t1,
                 double tscale)
{
    double L = (p1 - p0).norm();
    size_t n = size_t(std::max(2., std::ceil(L / SEGMENT_LEN)));
    if (tscale <= 0.)
        n = 1;

    Curve c;
    c.pts.reserve(n + 1);
    c.radii.reserve(n + 1);

    Vec3d m0 = t0 * L * tscale, m1 = t1 * L * tscale;

    // arc length parametrisation of the radius would be more accurate but
    // the segments are almost equal in length, a linear parameter is enough.
    for (size_t i = 0; i <= n; ++i) {
        double t = double(i) / double(n);
        double t2 = t * t, t3 = t2 * t;
        double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t,
               h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
        c.pts.emplace_back(h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1);
        c.radii.emplace_back(r0 + (r1 - r0) * t);
    }

    c.pts.front() = p0;
    c.pts.back()  = p1;

    return c;
}

bool curve_clear(const SupportableMesh &sm, const Curve &c, double ground)
{
    for (size_t i = 0; i + 1 < c.pts.size(); ++i) {
        if (c.pts[i].z() < ground - EPSILON || c.pts[i + 1].z() < ground - EPSILON)
            return false;
        if (!tube_clear(sm, c.pts[i], c.radii[i], c.pts[i + 1], c.radii[i + 1]))
            return false;
    }
    return true;
}

// Sweep a circle along the curve (rotation minimising frames) and close both
// ends with flat caps. The result is a closed, outward oriented surface.
indexed_triangle_set sweep_tube(const Curve &c)
{
    indexed_triangle_set out;
    const size_t N = c.pts.size();
    if (N < 2)
        return out;

    out.vertices.reserve(N * RING_STEPS + 2);
    out.indices.reserve((N - 1) * RING_STEPS * 2 + 2 * RING_STEPS);

    Vec3d prev_t = safe_normalized(c.pts[1] - c.pts[0], DOWN);
    Vec3d nrm    = std::abs(prev_t.z()) < 0.9 ? Vec3d(prev_t.cross(Vec3d::UnitZ()))
                                              : Vec3d(prev_t.cross(Vec3d::UnitX()));
    nrm.normalize();

    std::vector<Vec3d> tans(N);
    for (size_t i = 0; i < N; ++i) {
        if (i == 0)
            tans[i] = safe_normalized(c.pts[1] - c.pts[0], prev_t);
        else if (i + 1 == N)
            tans[i] = safe_normalized(c.pts[i] - c.pts[i - 1], prev_t);
        else
            tans[i] = safe_normalized(c.pts[i + 1] - c.pts[i - 1], prev_t);
    }

    for (size_t i = 0; i < N; ++i) {
        const Vec3d &t = tans[i];
        nrm = nrm - nrm.dot(t) * t; // project onto the new cross-section plane
        if (nrm.norm() < 1e-6)
            nrm = t.cross(Vec3d::UnitX()).norm() > 1e-6 ?
                      Vec3d(t.cross(Vec3d::UnitX())) : Vec3d(t.cross(Vec3d::UnitY()));
        nrm.normalize();
        Vec3d bin = t.cross(nrm);

        for (size_t k = 0; k < RING_STEPS; ++k) {
            double a = 2. * PI * double(k) / double(RING_STEPS);
            Vec3d  v = c.pts[i] + c.radii[i] * (std::cos(a) * nrm + std::sin(a) * bin);
            out.vertices.emplace_back(v.cast<float>());
        }
    }

    auto vid = [](size_t ring, size_t k) {
        return int(ring * RING_STEPS + (k % RING_STEPS));
    };

    for (size_t i = 0; i + 1 < N; ++i)
        for (size_t k = 0; k < RING_STEPS; ++k) {
            out.indices.emplace_back(vid(i, k), vid(i, k + 1), vid(i + 1, k));
            out.indices.emplace_back(vid(i, k + 1), vid(i + 1, k + 1), vid(i + 1, k));
        }

    // Caps. The centers are pulled slightly inside the tube so that caps of
    // different tubes ending in the same node never share a vertex.
    Vec3d c0 = c.pts.front() + 0.05 * c.radii.front() * tans.front();
    Vec3d c1 = c.pts.back() - 0.05 * c.radii.back() * tans.back();
    int   i0 = int(out.vertices.size());
    out.vertices.emplace_back(c0.cast<float>());
    int   i1 = int(out.vertices.size());
    out.vertices.emplace_back(c1.cast<float>());

    for (size_t k = 0; k < RING_STEPS; ++k) {
        out.indices.emplace_back(i0, vid(0, k + 1), vid(0, k));
        out.indices.emplace_back(i1, vid(N - 1, k), vid(N - 1, k + 1));
    }

    return out;
}

} // namespace

void create_organic_tree(SupportTreeBuilder    &builder,
                         const SupportableMesh &sm,
                         OrganicTreeStats      *stats)
{
    OrganicTreeStats st;

    Skel sk;
    create_branching_skeleton(builder, sm, sk);

    if (builder.ctl().stopcondition())
        return;

    for (const Head &h : builder.heads())
        if (h.is_valid())
            ++st.heads;

    st.smoothed_nodes = relax_skeleton(sk, sm, builder.ctl());

    if (builder.ctl().stopcondition())
        return;

    const double ground = ground_level(sm);

    // One tube for each skeleton edge (child -> parent)
    std::vector<int> edges; // child indices
    for (size_t i = 0; i < sk.nodes.size(); ++i) {
        const Skel::Node &n = sk.nodes[i];
        if (n.parent >= 0)
            edges.emplace_back(int(i));
        if (n.num_children() == 2)
            ++st.merges;
        if (n.parent < 0 && n.num_children() > 0)
            n.anchor ? ++st.anchors : ++st.roots;
    }

    std::vector<indexed_triangle_set> tubes(edges.size());
    std::vector<char> curved(edges.size(), 0);

    execution::for_each(
        ex_tbb, size_t(0), edges.size(),
        [&](size_t e) {
            if (builder.ctl().stopcondition())
                return;

            const Skel::Node &c = sk.nodes[size_t(edges[e])];
            const Skel::Node &p = sk.nodes[size_t(c.parent)];

            if ((p.pos - c.pos).norm() < EPSILON)
                return;

            Vec3d t0 = node_tangent(sk, edges[e], -1);
            Vec3d t1 = node_tangent(sk, c.parent, edges[e]);

            Curve curve;
            bool  ok = false;
            for (double ts : {1., 0.5}) {
                curve = make_curve(c.pos, c.r, t0, p.pos, p.r, t1, ts);
                if ((ok = curve_clear(sm, curve, ground)))
                    break;
            }

            if (!ok)
                curve = make_curve(c.pos, c.r, t0, p.pos, p.r, t1, 0.);

            curved[e] = ok;
            tubes[e]  = sweep_tube(curve);
        },
        execution::max_concurrency(ex_tbb));

    if (builder.ctl().stopcondition())
        return;

    indexed_triangle_set merged;
    for (size_t e = 0; e < edges.size(); ++e) {
        its_merge(merged, tubes[e]);
        ++st.branches;
        curved[e] ? ++st.curved : ++st.straight;
        tubes[e] = {};
    }

    // Spheres at the merge nodes and roots hide the seams between the tubes
    for (const Skel::Node &n : sk.nodes) {
        bool merge = n.num_children() == 2;
        bool root  = n.parent < 0 && !n.anchor && n.num_children() > 0;
        if (merge || root)
            its_merge(merged, get_mesh(Junction{n.pos, n.r}, SPHERE_STEPS));
    }

    if (!merged.empty())
        builder.add_support_mesh(std::move(merged));

    if (stats)
        *stats = st;
}

}} // namespace Slic3r::sla
