// Full organic tree supports for SLA.
//
// The growth scheme (per layer descent, avoidance areas that become "unreachable
// from the bed" with the maximal move speed, merging of nearby branches, radius
// growing with the supported weight) is adapted from the PrusaSlicer 2.9 / Cura
// organic tree supports (src/libslic3r/Support/TreeSupport.cpp,
// TreeModelVolumes.cpp, OrganicSupport.cpp, AGPLv3). Tips are SLA support points
// instead of overhang polygon samples and no FFF type is used.
#include "OrganicGrowthSLA.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include "libslic3r/Execution/ExecutionSeq.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/SLA/OrganicAvoidance.hpp"
#include "libslic3r/SLA/SupportTree.hpp"
#include "libslic3r/SLA/SupportTreeMesher.hpp"
#include "libslic3r/SLA/SupportTreeUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace sla {

namespace {

constexpr double LAYER_STEP        = 0.3;   // mm, nominal growth layer height
constexpr size_t MAX_LAYERS        = 600;
constexpr double WIDENING_SCALE    = 0.05;  // same as the branching tree
constexpr int    SMOOTH_ITERS      = 14;
constexpr double SMOOTH_LAMBDA     = 0.5;
constexpr double RING_SPACING      = 0.6;   // mm between rings of the emitted tubes
constexpr size_t RING_STEPS        = 16;
constexpr size_t SPHERE_STEPS      = 24;
constexpr double PULL_FRACTION     = 1.0;   // of the max move per layer
constexpr double OFFSET_LANDING    = 2.;    // layers
constexpr double MAX_MERGE_RANGE   = 10.;   // mm, search distance of merge partners

using Clock = std::chrono::high_resolution_clock;
double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

enum class End : char { None, Ground, Anchor, Dead };

struct GNode
{
    Vec3d  pos    = Vec3d::Zero();
    double r      = 0.;
    double weight = 0.;
    int    layer  = 0;
    int    parent = -1;
    int    child[2] = {-1, -1};
    int    head   = -1;         // tip: id of the pinhead
    bool   to_bed = true;
    double r_cap  = 1e9;        // radius growth is capped when the space is tight
    End    end    = End::None;
    std::optional<Anchor> anchor;
    bool   short_anchor = false;

    int nchildren() const { return (child[0] >= 0) + (child[1] >= 0); }
};

// ---------------------------------------------------------------- meshing
// (copy of the closed tube sweep from OrganicTreeSLA.cpp, kept private so the
// two organic modes stay independent)
Vec3d safe_normalized(const Vec3d &v, const Vec3d &fallback)
{
    double l = v.norm();
    return l < EPSILON ? fallback : Vec3d(v / l);
}

indexed_triangle_set sweep_tube(const std::vector<Vec3d> &pts, const std::vector<double> &radii)
{
    indexed_triangle_set out;
    const size_t N = pts.size();
    if (N < 2)
        return out;

    out.vertices.reserve(N * RING_STEPS + 2);
    out.indices.reserve((N - 1) * RING_STEPS * 2 + 2 * RING_STEPS);

    Vec3d first_t = safe_normalized(pts[1] - pts[0], DOWN);
    Vec3d nrm = std::abs(first_t.z()) < 0.9 ? Vec3d(first_t.cross(Vec3d::UnitZ()))
                                            : Vec3d(first_t.cross(Vec3d::UnitX()));
    nrm.normalize();

    std::vector<Vec3d> tans(N);
    for (size_t i = 0; i < N; ++i) {
        if (i == 0)
            tans[i] = first_t;
        else if (i + 1 == N)
            tans[i] = safe_normalized(pts[i] - pts[i - 1], tans[i - 1]);
        else
            tans[i] = safe_normalized(pts[i + 1] - pts[i - 1], tans[i - 1]);
    }

    for (size_t i = 0; i < N; ++i) {
        const Vec3d &t = tans[i];
        nrm = nrm - nrm.dot(t) * t;
        if (nrm.norm() < 1e-6)
            nrm = t.cross(Vec3d::UnitX()).norm() > 1e-6 ?
                      Vec3d(t.cross(Vec3d::UnitX())) : Vec3d(t.cross(Vec3d::UnitY()));
        nrm.normalize();
        Vec3d bin = t.cross(nrm);

        for (size_t k = 0; k < RING_STEPS; ++k) {
            double a = 2. * PI * double(k) / double(RING_STEPS);
            Vec3d  v = pts[i] + radii[i] * (std::cos(a) * nrm + std::sin(a) * bin);
            out.vertices.emplace_back(v.cast<float>());
        }
    }

    auto vid = [](size_t ring, size_t k) { return int(ring * RING_STEPS + (k % RING_STEPS)); };

    for (size_t i = 0; i + 1 < N; ++i)
        for (size_t k = 0; k < RING_STEPS; ++k) {
            out.indices.emplace_back(vid(i, k), vid(i, k + 1), vid(i + 1, k));
            out.indices.emplace_back(vid(i, k + 1), vid(i + 1, k + 1), vid(i + 1, k));
        }

    Vec3d c0 = pts.front() + 0.05 * radii.front() * tans.front();
    Vec3d c1 = pts.back() - 0.05 * radii.back() * tans.back();
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

// ---------------------------------------------------------------- growth
struct Grower
{
    const SupportableMesh &sm;
    SupportTreeBuilder    &builder;
    OrganicAvoidance      &av;
    OrganicGrowthStats    &st;

    std::vector<GNode> nodes;
    std::vector<std::vector<int>> by_layer;

    double step, max_move, r0, r_max, wk, merge_range, ground;
    int    base_layer;
    bool   ground_only;

    Grower(const SupportableMesh &s, SupportTreeBuilder &b, OrganicAvoidance &a, OrganicGrowthStats &stats)
        : sm(s), builder(b), av(a), st(stats)
    {
        step        = av.step();
        max_move    = av.params().max_move;
        r0          = sm.cfg.head_back_radius_mm;
        r_max       = av.params().r_max;
        wk          = WIDENING_SCALE * sm.cfg.pillar_widening_factor;
        // branches look for partners twice as far as a bridge may be long (they
        // close in at the slope limited speed, so the drop decides if they get
        // there); capped, longer pulls make the branches hug the model surface
        merge_range = std::max(2. * max_move, std::min(MAX_MERGE_RANGE, 2. * sm.cfg.max_bridge_length_mm));
        ground      = av.layer_z(0);
        base_layer  = std::max(1, int(std::ceil(sm.cfg.base_height_mm / step)));
        ground_only = sm.cfg.ground_facing_only;
        by_layer.resize(av.layer_count() + 1);
    }

    double grown_radius(double r, double len) const { return std::min(r_max, r + wk * len); }

    size_t cls(const GNode &n) const
    {
        return av.radius_class(std::min({r_max, n.r_cap, n.r + wk * 1.5 * step}));
    }

    int add_node(GNode &&n)
    {
        nodes.emplace_back(std::move(n));
        ++st.nodes;
        return int(nodes.size() - 1);
    }

    void kill(int idx)
    {
        std::vector<int> stack{idx};
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            nodes[size_t(i)].end = End::Dead;
            for (int c : nodes[size_t(i)].child)
                if (c >= 0)
                    stack.push_back(c);
        }
    }

    // The straight-down (or sliding) proposal for node `i` on layer L-1.
    // Returns false if the node cannot continue as it is.
    bool propose(GNode &n, size_t L, Vec2d &q, bool &slid)
    {
        const size_t rc_hi = cls(n);
        const size_t rc_lo = 0;
        const Vec2d  p     = n.pos.head<2>();
        slid = false;
        for (size_t rc = rc_hi + 1; rc-- > rc_lo;) {
            bool ok = false;
            if (!av.blocked(L - 1, rc, n.to_bed, p)) {
                q  = p;
                ok = true;
            } else if (av.nearest_free(L - 1, rc, n.to_bed, p, 1.1 * max_move, q)) {
                slid = true;
                ok   = true;
            }
            if (ok) {
                // too tight for the full growth: stop thickening the branch
                if (rc < rc_hi) {
                    n.r_cap = std::min(n.r_cap, av.class_radius(rc));
                    n.r     = std::min(n.r, n.r_cap); // squeeze through a narrow gap
                }
                return true;
            }
        }
        return false;
    }

    // Try to end the branch on the model body below the node.
    bool try_land(GNode &n)
    {
        if (ground_only || n.weight > sm.cfg.max_weight_on_model_support)
            return false;

        auto hit = sm.emesh.query_ray_hit(n.pos, DOWN);
        double d = hit.distance();
        if (!std::isfinite(d) || d > std::max(10., sm.cfg.head_fullwidth() + OFFSET_LANDING * step + n.r))
            return false;

        Vec3d target = n.pos + d * DOWN;

        // The anchor head is made with the head sized strut, the branch tapers
        // into it; in tight places an even thinner strut is used.
        for (double rr : {std::min(n.r, r0), av.params().thin_radius}) {
            Junction fromj{n.pos, rr};
            auto anchor = calculate_anchor_placement(ex_seq, sm, fromj, target);
            if (!anchor)
                continue;

            Vec3d jp = anchor->junction_point();
            if (jp.z() > n.pos.z() + 0.05)
                continue;

            Beam beam{Ball{n.pos, std::min(n.r, rr)}, Ball{jp, anchor->r_back_mm}};
            auto bh = beam_mesh_hit(ex_seq, sm.emesh, beam, 0.);
            if (bh.distance() <= (jp - n.pos).norm())
                continue;

            n.r      = std::min(n.r, std::max(rr, anchor->r_back_mm));
            n.end    = End::Anchor;
            n.anchor = anchor;
            return true;
        }

        // Last resort, the node is too close to the surface for the regular
        // anchor head: a shortened head attached right at the node.
        {
            double back  = av.params().thin_radius;
            double pin   = sm.cfg.head_front_radius_mm;
            double pen   = sm.cfg.head_penetration_mm;
            double w     = d - 2. * pin - back + pen;
            if (w >= 0. && w <= sm.cfg.head_width_mm) {
                Anchor a(back, pin, w, pen, -DOWN, target);
                // the back sphere must keep the safety gap from the model
                double gap = std::sqrt(sm.emesh.squared_distance(a.junction_point())) - back;
                if ((a.junction_point() - n.pos).norm() < 0.1 && gap >= sm.cfg.safety_distance(back)) {
                    n.r      = std::min(n.r, back);
                    n.end    = End::Anchor;
                    n.anchor = a;
                    n.short_anchor = true;
                    return true;
                }
            }
        }
        return false;
    }

    int make_child_of(int a, int b, const Vec3d &pos, size_t L)
    {
        GNode m;
        m.pos   = pos;
        m.layer = int(L) - 1;
        double r = 0., w = 0.;
        int    ids[2] = {a, b};
        int    k = 0;
        for (int id : ids) {
            if (id < 0)
                continue;
            const GNode &c = nodes[size_t(id)];
            double len = (c.pos - pos).norm();
            w = std::max(w, c.weight + len); // like the branching tree: longest path
            r = std::max(r, grown_radius(c.r, len));
            m.r_cap = std::min(m.r_cap, c.r_cap);
            m.child[k++] = id;
            m.to_bed = (k == 1) ? c.to_bed : (m.to_bed && c.to_bed);
        }
        if (k == 2)
            r = std::max(r, std::min(r_max, r0 + wk * w));
        m.weight = w;
        m.r      = std::min(r, std::max(m.r_cap, std::max(a >= 0 ? nodes[size_t(a)].r : 0., b >= 0 ? nodes[size_t(b)].r : 0.)));
        int idx = add_node(std::move(m));
        for (int id : ids)
            if (id >= 0)
                nodes[size_t(id)].parent = idx;
        return idx;
    }

    bool grow()
    {
        const size_t Lmax = by_layer.size() - 1;
        for (size_t L = Lmax; L >= 1; --L) {
            if (builder.ctl().stopcondition())
                return false;

            std::vector<int> act;
            act.swap(by_layer[L]);
            if (act.empty())
                continue;

            std::vector<int> mv;
            for (int i : act) {
                GNode &n = nodes[size_t(i)];
                // Near the bed the pillar base must fit as well: layer 0 holds
                // the (widest) base footprint of the pillar.
                if (int(L) <= base_layer && !av.blocked(0, cls(n), false, n.pos.head<2>())) {
                    // ... but a neighbour which can still be reached before the bed
                    // is worth a merge first (otherwise two pillars end up a
                    // fraction of a millimetre apart); the last layer ends it.
                    bool wait = false;
                    if (L > 1) {
                        const double reach = 2. * max_move * double(L - 1);
                        for (int j : act)
                            if (j != i && nodes[size_t(j)].to_bed == n.to_bed &&
                                (nodes[size_t(j)].pos.head<2>() - n.pos.head<2>()).norm() <= reach) {
                                wait = true;
                                break;
                            }
                    }
                    if (!wait) {
                        n.end = End::Ground;
                        continue;
                    }
                }
                mv.push_back(i);
            }

            struct Prop { int id; Vec2d q; bool slid; bool done; };
            std::vector<Prop> props;
            props.reserve(mv.size());

            for (int i : mv) {
                GNode &n = nodes[size_t(i)];
                Vec2d q;
                bool slid = false;
                bool ok = propose(n, L, q, slid);
                if (!ok && n.to_bed && !ground_only) {
                    n.to_bed = false; // the bed cannot be reached from here: rest on the model
                    ok = propose(n, L, q, slid);
                }
                // model mode: when straight down is blocked, land on the model
                if (!n.to_bed && av.blocked(L - 1, cls(n), false, n.pos.head<2>()) && try_land(n))
                    continue;
                if (!ok) {
                    kill(i);
                    continue;
                }
                if (slid)
                    ++st.avoid_moves;
                props.push_back({i, q, slid, false});
            }

            // order by x for the neighbour search
            std::vector<size_t> ord(props.size());
            for (size_t k = 0; k < ord.size(); ++k) ord[k] = k;
            std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                const Vec3d &pa = nodes[size_t(props[a].id)].pos, &pb = nodes[size_t(props[b].id)].pos;
                return pa.x() < pb.x() || (pa.x() == pb.x() && props[a].id < props[b].id);
            });

            const double z_new = av.layer_z(L - 1);
            std::vector<int> next;

            for (size_t oi = 0; oi < ord.size(); ++oi) {
                Prop &A = props[ord[oi]];
                if (A.done)
                    continue;
                GNode &a = nodes[size_t(A.id)];
                Vec2d pa = a.pos.head<2>();

                // nearest unprocessed partner
                int    best = -1;
                double bestd = merge_range;
                for (size_t oj = oi + 1; oj < ord.size(); ++oj) {
                    Prop &B = props[ord[oj]];
                    const GNode &b = nodes[size_t(B.id)];
                    if (b.pos.x() - a.pos.x() > bestd)
                        break;
                    if (B.done || b.to_bed != a.to_bed)
                        continue;
                    double d = (b.pos.head<2>() - pa).norm();
                    if (d < bestd) {
                        bestd = d;
                        best  = int(ord[oj]);
                    }
                }
                // partners before in the order which are not done yet
                for (size_t oj = oi; oj-- > 0;) {
                    Prop &B = props[ord[oj]];
                    const GNode &b = nodes[size_t(B.id)];
                    if (a.pos.x() - b.pos.x() > bestd)
                        break;
                    if (B.done || b.to_bed != a.to_bed)
                        continue;
                    double d = (b.pos.head<2>() - pa).norm();
                    if (d < bestd) {
                        bestd = d;
                        best  = int(ord[oj]);
                    }
                }

                bool handled = false;
                if (best >= 0) {
                    Prop &B = props[size_t(best)];
                    GNode &b = nodes[size_t(B.id)];
                    Vec2d pb = b.pos.head<2>();
                    Vec2d ab = pb - pa;
                    double d = ab.norm();
                    double w_total = a.weight + b.weight + 1e-6;

                    double merged_w = std::max(a.weight, b.weight) + 2. * step;
                    double merged_r = std::max(std::min(r_max, r0 + wk * merged_w), std::max(a.r, b.r));

                    if (d <= 2. * max_move) {
                        // meeting point, the heavier branch moves less
                        double ta = std::clamp(d * b.weight / w_total, std::max(0., d - max_move),
                                               std::min(d, max_move));
                        Vec2d  mp = d > 1e-9 ? Vec2d(pa + ab * (ta / d)) : pa;
                        GNode probe;
                        probe.r = merged_r;
                        if (!av.blocked(L - 1, cls(probe), a.to_bed, mp)) {
                            int m = make_child_of(A.id, B.id, Vec3d(mp.x(), mp.y(), z_new), L);
                            next.push_back(m);
                            A.done = B.done = true;
                            handled = true;
                        }
                    }
                    if (!handled && !A.slid && !B.slid && d > 1e-6) {
                        // pull both towards each other (they merge in a later layer)
                        double s = std::min(PULL_FRACTION * max_move, 0.5 * d);
                        Vec2d  qa = pa + ab / d * s, qb = pb - ab / d * s;
                        if (!av.blocked(L - 1, cls(a), a.to_bed, qa))
                            A.q = qa;
                        if (!av.blocked(L - 1, cls(b), b.to_bed, qb))
                            B.q = qb;
                        // both nodes of the pair move this layer
                        B.done = true;
                        next.push_back(make_child_of(B.id, -1, Vec3d(B.q.x(), B.q.y(), z_new), L));
                    }
                }

                if (!handled && !A.slid && best < 0) {
                    // The nearest partners were already used by other pairs this
                    // layer: do not drop straight down but keep closing in on the
                    // nearest neighbour (wherever it goes), so more branches end
                    // up in a common trunk instead of one pillar per head.
                    int    nb  = -1;
                    double nbd = merge_range;
                    for (size_t oj = 0; oj < ord.size(); ++oj) {
                        if (oj == oi)
                            continue;
                        const Prop &B = props[ord[oj]];
                        const GNode &b = nodes[size_t(B.id)];
                        if (b.pos.x() - a.pos.x() > nbd)
                            break;
                        if (a.pos.x() - b.pos.x() > nbd || b.to_bed != a.to_bed)
                            continue;
                        double d = (b.pos.head<2>() - pa).norm();
                        if (d < nbd) {
                            nbd = d;
                            nb  = int(ord[oj]);
                        }
                    }
                    if (nb >= 0 && nbd > 1e-6) {
                        const Prop &B = props[size_t(nb)];
                        Vec2d tgt = B.q;
                        Vec2d dir = tgt - pa;
                        double dl = dir.norm();
                        if (dl > 1e-6) {
                            Vec2d qa = pa + dir / dl * std::min(PULL_FRACTION * max_move, dl);
                            if (!av.blocked(L - 1, cls(a), a.to_bed, qa))
                                A.q = qa;
                        }
                    }
                }

                if (!handled) {
                    A.done = true;
                    int c = make_child_of(A.id, -1, Vec3d(A.q.x(), A.q.y(), z_new), L);
                    next.push_back(c);
                }
            }

            for (int c : next)
                by_layer[size_t(nodes[size_t(c)].layer)].push_back(c);
        }
        // nodes which slid down to the bed itself
        for (int i : by_layer[0])
            nodes[size_t(i)].end = End::Ground;
        return true;
    }
};

} // namespace

void create_organic_growth_tree(SupportTreeBuilder            &builder,
                                const SupportableMesh         &sm,
                                OrganicGrowthStats            *stats,
                                std::vector<OrganicBranchPath> *paths)
{
    OrganicGrowthStats st;
    auto stopped = [&builder] { return builder.ctl().stopcondition(); };

    // ---- heads, exactly as in the other tree types
    auto coordfn = [&sm](size_t id, size_t dim) { return sm.pts[id].pos(dim); };
    KDTreeIndirect<3, float, decltype(coordfn)> tree{coordfn, sm.pts.size()};

    auto nondup_idx = non_duplicate_suppt_indices(tree, sm.pts, 0.1);
    st.support_points = nondup_idx.size();
    std::vector<std::optional<Head>> heads(nondup_idx.size());

    execution::for_each(
        ex_tbb, size_t(0), nondup_idx.size(),
        [&](size_t i) {
            if (!stopped())
                heads[i] = calculate_pinhead_placement(ex_seq, sm, nondup_idx[i]);
        },
        execution::max_concurrency(ex_tbb));

    if (stopped())
        return;

    std::vector<Vec3d>  tip_pos;
    std::vector<double> tip_r;
    for (auto &h : heads)
        if (h && h->is_valid()) {
            h->id = long(tip_pos.size());
            tip_pos.emplace_back(h->junction_point());
            tip_r.emplace_back(h->r_back_mm);
            builder.add_head(h->id, *h);
        }
    st.heads = tip_pos.size();

    if (tip_pos.empty()) {
        if (stats) *stats = st;
        return;
    }

    // ---- avoidance areas
    auto t0 = Clock::now();
    const double ground = ground_level(sm);
    double top = ground;
    for (const Vec3d &p : tip_pos)
        top = std::max(top, p.z());
    top += 2. * LAYER_STEP;

    OrganicAvoidance::Params ap;
    ap.ground_z   = ground;
    ap.top_z      = top;
    ap.step       = std::max(LAYER_STEP, (top - ground) / double(MAX_LAYERS));
    ap.r_min      = sm.cfg.head_back_radius_mm;
    ap.r_max      = std::max(1.5 * ap.r_min, 0.9 * sm.cfg.base_radius_mm);
    ap.classes    = 6;
    ap.thin_radius = std::min(sm.cfg.head_fallback_radius_mm, ap.r_min);
    ap.thin_gap   = 0.5 * sm.cfg.safety_distance_mm;
    ap.safety     = sm.cfg.safety_distance_mm;
    ap.base_radius = sm.cfg.base_radius_mm;
    ap.base_safety = std::max(sm.cfg.pillar_base_safety_distance_mm, sm.cfg.safety_distance_mm);
    ap.base_height = sm.cfg.base_height_mm;
    double slope  = std::clamp(sm.cfg.bridge_slope, 20. * PI / 180., 80. * PI / 180.);
    ap.max_move   = std::clamp(ap.step / std::tan(slope), 0.2 * ap.step, 1.5 * ap.step);

    auto av_ptr = std::make_unique<OrganicAvoidance>(*sm.emesh.get_triangle_mesh(), ap, stopped);
    OrganicAvoidance &av = *av_ptr;
    st.ms_avoidance = ms_since(t0);
    if (!av.valid() || stopped())
        return;

    st.layers = av.layer_count();
    st.step   = av.step();

    // ---- growth
    t0 = Clock::now();
    Grower g(sm, builder, av, st);

    std::vector<int> tip_node(tip_pos.size(), -1);
    for (size_t k = 0; k < tip_pos.size(); ++k) {
        GNode n;
        n.pos    = tip_pos[k];
        n.r      = tip_r[k];
        n.head   = int(k);
        int Lt   = int(std::floor((tip_pos[k].z() - ground) / av.step()));
        n.layer  = std::clamp(Lt + 1, 1, int(av.layer_count()));
        // is the bed reachable from here?
        n.to_bed = !av.blocked(size_t(std::max(0, Lt)), av.radius_class(n.r), true, tip_pos[k].head<2>());
        tip_node[k] = g.add_node(std::move(n));
        g.by_layer[size_t(g.nodes[size_t(tip_node[k])].layer)].push_back(tip_node[k]);
    }
    st.nodes = g.nodes.size();

    if (!g.grow())
        return;
    st.ms_growth = ms_since(t0);

    // Heads whose branch died: never lose a support point. Fall back to a
    // branching style ground route, then to an anchor on the model.
    for (size_t k = 0; k < tip_node.size(); ++k) {
        if (g.nodes[size_t(tip_node[k])].end != End::Dead)
            continue;
        if (stopped())
            return;
        ++st.dropped;
        Junction j{tip_pos[k], tip_r[k]};
        auto conn = deepsearch_ground_connection(ex_seq, sm, j, sm.cfg.head_back_radius_mm, DOWN);
        if (conn) {
            build_ground_connection(builder, sm, conn);
            ++st.fallback_ground;
            continue;
        }
        GNode tmp;
        tmp.pos = tip_pos[k];
        tmp.r   = tip_r[k];
        if (g.try_land(tmp) && tmp.anchor) {
            builder.add_diffbridge(tip_pos[k], tmp.anchor->junction_point(), tip_r[k], tmp.anchor->r_back_mm);
            builder.add_anchor(*tmp.anchor);
            ++st.fallback_anchor;
            continue;
        }
        builder.head(unsigned(k)).invalidate();
        ++st.unroutable;
    }

    // the per layer areas are the big memory consumer and are not needed any more
    av_ptr.reset();

    // a node without an end which is not dead can only be a node that was never
    // processed (should not happen); treat it as dead.
    auto &nodes = g.nodes;
    auto alive = [&](size_t i) { return nodes[i].end != End::Dead; };

    // ---- smoothing of the lateral positions
    t0 = Clock::now();
    const size_t NN = nodes.size();
    std::vector<char> movable(NN, 0);
    for (size_t i = 0; i < NN; ++i) {
        const GNode &n = nodes[i];
        movable[i] = alive(i) && n.head < 0 && n.parent >= 0 && n.nchildren() >= 1 && n.end == End::None;
    }
    const double sd = sm.cfg.safety_distance_mm;
    std::vector<double> clearance(NN, 1e9);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, NN), [&](const tbb::blocked_range<size_t> &rg) {
        for (size_t i = rg.begin(); i < rg.end(); ++i)
            if (movable[i])
                clearance[i] = std::sqrt(sm.emesh.squared_distance(nodes[i].pos));
    });

    std::vector<Vec3d> cur(NN), nxt(NN);
    for (size_t i = 0; i < NN; ++i)
        cur[i] = nodes[i].pos;
    nxt = cur;

    for (int it = 0; it < SMOOTH_ITERS && !stopped(); ++it) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, NN), [&](const tbb::blocked_range<size_t> &rg) {
            for (size_t i = rg.begin(); i < rg.end(); ++i) {
                nxt[i] = cur[i];
                if (!movable[i])
                    continue;
                const GNode &n = nodes[i];
                Vec2d sum = cur[size_t(n.parent)].head<2>();
                int   cnt = 1;
                for (int c : n.child)
                    if (c >= 0) {
                        sum += cur[size_t(c)].head<2>();
                        ++cnt;
                    }
                Vec2d target = sum / double(cnt);
                Vec2d xy     = cur[i].head<2>() + SMOOTH_LAMBDA * (target - cur[i].head<2>());
                Vec3d cand(xy.x(), xy.y(), cur[i].z());

                // keep the slope limit towards the neighbours
                auto slope_ok = [&](const Vec3d &o) {
                    double dz = std::max(std::abs(cand.z() - o.z()), ap.step);
                    double dh = (cand.head<2>() - o.head<2>()).norm();
                    return dh <= 1.15 * ap.max_move / ap.step * dz + 1e-6;
                };
                bool ok = slope_ok(cur[size_t(n.parent)]);
                for (int c : n.child)
                    if (ok && c >= 0)
                        ok = slope_ok(cur[size_t(c)]);
                if (!ok)
                    continue;

                double d    = std::sqrt(sm.emesh.squared_distance(cand));
                double need = n.r + 0.9 * sd;
                if (d >= std::min(clearance[i], need)) {
                    nxt[i]       = cand;
                    clearance[i] = d;
                }
            }
        });
        cur.swap(nxt);
    }
    for (size_t i = 0; i < NN; ++i)
        nodes[i].pos = cur[i];
    st.ms_smooth = ms_since(t0);

    if (stopped())
        return;

    // ---- emission
    t0 = Clock::now();

    std::vector<std::vector<int>> chains;
    for (size_t i = 0; i < NN; ++i) {
        if (!alive(i))
            continue;
        const GNode &n = nodes[i];
        int nc = n.nchildren();
        if (nc == 2)
            ++st.merges;
        if (n.end == End::Ground)
            ++st.roots;
        if (n.end == End::Anchor)
            ++st.anchors;
        if (nc == 1)
            continue; // chain interior
        std::vector<int> ch{int(i)};
        for (int c = n.parent; c >= 0;) {
            ch.push_back(c);
            const GNode &cn = nodes[size_t(c)];
            if (cn.nchildren() == 2 || cn.parent < 0)
                break;
            c = cn.parent;
        }
        chains.emplace_back(std::move(ch));
    }
    st.branches = chains.size();

    // roots: pillar with a base on the bed / anchor on the model
    for (size_t i = 0; i < NN; ++i) {
        if (!alive(i))
            continue;
        const GNode &n = nodes[i];
        if (n.end == End::Ground) {
            Vec3d gp(n.pos.x(), n.pos.y(), ground);
            double h = n.pos.z() - ground;
            if (h > EPSILON) {
                long pid = builder.add_pillar(gp, h, n.r, n.r);
                builder.add_pillar_base(pid, sm.cfg.base_height_mm, sm.cfg.base_radius_mm);
            }
        } else if (n.end == End::Anchor && n.anchor) {
            builder.add_anchor(*n.anchor);
            st.max_anchor_weight = std::max(st.max_anchor_weight, n.weight);
            st.anchor_pts.emplace_back(n.anchor->junction_point());
            st.anchor_r.emplace_back(n.anchor->r_back_mm);
            st.anchor_shortened.emplace_back(n.short_anchor);
        }
    }

    std::vector<OrganicBranchPath> bp(chains.size());
    for (size_t c = 0; c < chains.size(); ++c) {
        for (int id : chains[c]) {
            bp[c].pts.emplace_back(nodes[size_t(id)].pos);
            bp[c].radii.emplace_back(nodes[size_t(id)].r);
        }
        const GNode &last = nodes[size_t(chains[c].back())];
        if (last.end == End::Anchor && last.anchor) {
            bp[c].pts.emplace_back(last.anchor->junction_point());
            bp[c].radii.emplace_back(last.anchor->r_back_mm);
        }
    }

    std::vector<indexed_triangle_set> tubes(chains.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, chains.size()), [&](const tbb::blocked_range<size_t> &rg) {
        for (size_t c = rg.begin(); c < rg.end(); ++c) {
            if (stopped())
                return;
            const auto &P = bp[c].pts;
            const auto &R = bp[c].radii;
            std::vector<Vec3d>  pp;
            std::vector<double> rr;
            double acc = 0.;
            for (size_t i = 0; i < P.size(); ++i) {
                if (i > 0)
                    acc += (P[i] - P[i - 1]).norm();
                bool last = i + 1 == P.size();
                if (i == 0 || last || acc >= RING_SPACING) {
                    if (!pp.empty() && (P[i] - pp.back()).norm() < 1e-3) {
                        if (last) { pp.back() = P[i]; rr.back() = R[i]; }
                        continue;
                    }
                    // do not leave a tiny last segment
                    if (last && pp.size() >= 2 && (P[i] - pp.back()).norm() < 0.3 * RING_SPACING) {
                        pp.back() = P[i];
                        rr.back() = R[i];
                        continue;
                    }
                    pp.emplace_back(P[i]);
                    rr.emplace_back(R[i]);
                    acc = 0.;
                }
            }
            if (pp.size() >= 2) {
                tubes[c] = sweep_tube(pp, rr);
                // slanted end rings of a root must not reach below the bed
                for (Vec3f &v : tubes[c].vertices)
                    v.z() = std::max(v.z(), float(ground));
            }
        }
    });

    if (stopped())
        return;

    indexed_triangle_set merged;
    for (auto &t : tubes) {
        its_merge(merged, t);
        t = {};
    }
    for (size_t i = 0; i < NN; ++i) {
        if (!alive(i))
            continue;
        const GNode &n = nodes[i];
        if (n.nchildren() == 2)
        {
            // the sphere must not reach below the bed
            double sr = std::min(n.r, n.pos.z() - ground - 0.01);
            if (sr > 0.3 * n.r)
                its_merge(merged, get_mesh(Junction{n.pos, sr}, SPHERE_STEPS));
        }
    }

    if (!merged.empty())
        builder.add_support_mesh(std::move(merged));

    st.ms_mesh = ms_since(t0);
    BOOST_LOG_TRIVIAL(info)
        << "Organic growth: heads=" << st.heads << " support_points=" << st.support_points
        << " grown=" << (st.heads - st.dropped) << " dropped=" << st.dropped
        << " merges=" << st.merges << " roots(ground)=" << st.roots << " anchors(model)=" << st.anchors
        << " fallback_ground=" << st.fallback_ground << " fallback_anchor=" << st.fallback_anchor
        << " unroutable=" << st.unroutable << " branches=" << st.branches
        << " layers=" << st.layers << " step=" << st.step << " mm nodes=" << st.nodes
        << " avoid_moves=" << st.avoid_moves
        << " ms(avoidance/growth/smooth/mesh)=" << int(st.ms_avoidance) << "/" << int(st.ms_growth)
        << "/" << int(st.ms_smooth) << "/" << int(st.ms_mesh);
    if (paths)
        *paths = std::move(bp);
    if (stats)
        *stats = st;
}

}} // namespace Slic3r::sla
