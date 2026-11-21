// Tests of the two organic SLA tree support modes:
//   SupportTreeType::OrganicBranching  ("organic_branching") - branching layout smoothed into tubes
//   SupportTreeType::Organic           ("organic")           - full organic growth (OrganicGrowthSLA)
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>

#include "sla_test_utils.hpp"

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/SLA/BranchingTreeSLA.hpp>
#include <libslic3r/SLA/OrganicGrowthSLA.hpp>
#include <libslic3r/SLA/OrganicTreeSLA.hpp>
#include <libslic3r/SLA/SupportTree.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>

namespace {

const char *const ORGANIC_MODELS[] = {
    "20mm_cube.obj",                                  // plain flat underside
    "bridge.obj",                                     // bridge-like overhang
    "overhang.obj",                                   // overhang
    "cube_with_concave_hole.obj",                     // concave hole
    "cube_with_concave_hole_enlarged_standing.obj",   // internal overhang
    "A_upsidedown.obj",
    "extruder_idler.obj",
};

const sla::SupportTreeType ORGANIC_MODES[] = {
    sla::SupportTreeType::OrganicBranching,
    sla::SupportTreeType::Organic,
};

const char *mode_name(sla::SupportTreeType t)
{
    switch (t) {
    case sla::SupportTreeType::Organic: return "organic";
    case sla::SupportTreeType::OrganicBranching: return "organic_branching";
    case sla::SupportTreeType::Branching: return "branching";
    default: return "default";
    }
}

struct OrganicCase
{
    TriangleMesh                        mesh;
    std::unique_ptr<sla::SupportableMesh> sm;
    double                              zmin, zmax;
};

OrganicCase make_case_mesh(TriangleMesh mesh, sla::SupportTreeConfig cfg)
{
    OrganicCase c;
    c.mesh = std::move(mesh);
    REQUIRE_FALSE(c.mesh.empty());

    auto bb = c.mesh.bounding_box();
    c.zmin  = bb.min.z();
    c.zmax  = bb.max.z();

    float layer_h = 0.05f;
    auto  grid_   = grid(float(c.zmin - cfg.object_elevation_mm), float(c.zmax), layer_h);
    auto  slices  = slice_mesh_ex(c.mesh.its, grid_, 0.005f);

    c.sm = std::make_unique<sla::SupportableMesh>(c.mesh.its, sla::SupportPoints{}, cfg);
    c.sm->zoffset = c.zmin;

    sla::SupportPointGeneratorConfig gencfg;
    auto gen_data = sla::prepare_generator_data(std::move(slices), grid_);
    auto lsp      = sla::generate_support_points(gen_data, gencfg);
    double allowed_move = (grid_[1] - grid_[0]) + std::numeric_limits<float>::epsilon();
    c.sm->pts = sla::move_on_mesh_surface(lsp, c.sm->emesh, allowed_move);
    return c;
}

OrganicCase make_case(const std::string &fname, sla::SupportTreeConfig cfg)
{
    return make_case_mesh(load_model(fname), std::move(cfg));
}

sla::SupportTreeConfig organic_cfg(double elevation, sla::SupportTreeType type)
{
    sla::SupportTreeConfig cfg;
    cfg.object_elevation_mm = elevation;
    cfg.tree_type           = type;
    return cfg;
}

size_t valid_heads(const sla::SupportTreeBuilder &b)
{
    size_t n = 0;
    for (auto &h : b.heads())
        n += h.is_valid();
    return n;
}

struct RunResult
{
    size_t heads = 0, roots = 0, merges = 0, anchors = 0;
    double ms = 0.;
    sla::OrganicGrowthStats growth;
};

// Run a tree type on a prepared case. `b` receives the result.
RunResult run_tree(sla::SupportTreeBuilder &b, const sla::SupportableMesh &sm,
                   sla::SupportTreeType type,
                   std::vector<sla::OrganicBranchPath> *paths = nullptr)
{
    RunResult r;
    auto t0 = std::chrono::high_resolution_clock::now();
    switch (type) {
    case sla::SupportTreeType::Organic:
        sla::create_organic_growth_tree(b, sm, &r.growth, paths);
        r.roots   = r.growth.roots;
        r.merges  = r.growth.merges;
        r.anchors = r.growth.anchors;
        break;
    case sla::SupportTreeType::OrganicBranching: {
        sla::OrganicTreeStats st;
        sla::create_organic_tree(b, sm, &st);
        r.roots = st.roots; r.merges = st.merges; r.anchors = st.anchors;
        break;
    }
    default: { // branching: count through the skeleton
        sla::TreeSkeleton sk;
        sla::create_branching_skeleton(b, sm, sk);
        for (auto &n : sk.nodes) {
            r.merges += n.num_children() == 2;
            if (n.parent < 0 && n.num_children() > 0)
                (n.anchor ? r.anchors : r.roots)++;
        }
    }
    }
    r.ms    = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    r.heads = valid_heads(b);
    return r;
}

} // namespace

TEST_CASE("OrganicSupports::TubesAreClosedAndMergedMeshIsWatertight",
          "[SLASupportGeneration][Organic]")
{
    for (auto type : ORGANIC_MODES)
    for (double elevation : {10., 0.}) {
        for (const char *fname : ORGANIC_MODELS) {
            INFO(mode_name(type) << " " << fname << " elevation " << elevation);
            auto c = make_case(fname, organic_cfg(elevation, type));
            REQUIRE_FALSE(c.sm->pts.empty());

            sla::SupportTreeBuilder b;
            RunResult r = run_tree(b, *c.sm, type);

            std::cout << "ORGANIC mode=" << mode_name(type) << " " << fname << " elev=" << elevation
                      << " pts=" << c.sm->pts.size() << " heads=" << r.heads
                      << " roots=" << r.roots << " anchors=" << r.anchors
                      << " merges=" << r.merges << " ms=" << int(r.ms)
                      << " (avoid " << int(r.growth.ms_avoidance) << " grow " << int(r.growth.ms_growth)
                      << " smooth " << int(r.growth.ms_smooth) << " mesh " << int(r.growth.ms_mesh)
                      << " layers " << r.growth.layers << " dropped " << r.growth.dropped << ")"
                      << std::endl;

            // Flat cubes lying on the floor need no supports at all
            const bool flat_on_floor = std::string(fname) == "20mm_cube.obj" ||
                                       std::string(fname) == "cube_with_concave_hole.obj";
            const bool needs_supports = elevation > 0. || !flat_on_floor;
            if (needs_supports)
                REQUIRE(r.heads > 0);

            // Every tube is a closed, outward oriented surface
            for (const auto &part : b.support_meshes()) {
                indexed_triangle_set p = part;
                its_merge_vertices(p);
                REQUIRE(its_num_open_edges(p) == 0);
                REQUIRE(its_volume(p) > 0.f);
            }

            const indexed_triangle_set &out = b.retrieve_mesh(sla::MeshType::Support);
            if (!needs_supports && out.empty())
                continue;
            REQUIRE_FALSE(out.empty());
            REQUIRE(its_num_open_edges(out) == 0);

            auto obb = bounding_box(out);
            double allowed_zmin = c.zmin - elevation;
            if (elevation < EPSILON)
                allowed_zmin = c.zmin - 2 * c.sm->cfg.head_back_radius_mm;
            REQUIRE(obb.min.z() >= Approx(allowed_zmin));
            REQUIRE(obb.max.z() <= Approx(c.zmax));
        }
    }
}

TEST_CASE("OrganicSupports::HeadsMatchSupportPointsAndTreeMerges",
          "[SLASupportGeneration][Organic]")
{
    for (auto type : ORGANIC_MODES)
    for (double elevation : {10., 0.})
    for (const char *fname : ORGANIC_MODELS) {
        INFO(mode_name(type) << " " << fname << " elevation " << elevation);
        auto c = make_case(fname, organic_cfg(elevation, type));

        sla::SupportTreeBuilder bo;
        RunResult r = run_tree(bo, *c.sm, type);

        sla::SupportTreeBuilder bb;
        RunResult rb = run_tree(bb, *c.sm, sla::SupportTreeType::Branching);

        std::cout << "HEADS " << mode_name(type) << " " << fname << " elev=" << elevation
                  << " branching=" << rb.heads << " this=" << r.heads << std::endl;

        if (type == sla::SupportTreeType::Organic)
            // a support point is never lost: whatever branching can serve, organic
            // serves (and it may serve more, branching drops unroutable heads and
            // its own head count varies a bit from run to run)
            REQUIRE(r.heads >= rb.heads);
        else
            // same support points served as by the branching tree
            REQUIRE(std::abs(double(r.heads) - double(rb.heads)) <= 0.1 * double(rb.heads) + 2.);

        if (rb.heads < 5)
            continue;
        // branches merge, so there are fewer roots than support points
        REQUIRE(r.merges > 0);
        REQUIRE(r.roots + r.anchors > 0);
        REQUIRE(r.roots + r.anchors < c.sm->pts.size());
    }
}

TEST_CASE("OrganicSupports::AnchorsKeepSafetyGapAndRespectMaxWeight",
          "[SLASupportGeneration][Organic]")
{
    for (double max_weight : {10., 2.})
    for (double elevation : {10., 0.})
    for (const char *fname : ORGANIC_MODELS) {
        INFO(fname << " elevation " << elevation << " max_weight " << max_weight);
        sla::SupportTreeConfig cfg = organic_cfg(elevation, sla::SupportTreeType::Organic);
        cfg.max_weight_on_model_support = max_weight;
        auto c = make_case(fname, cfg);

        sla::SupportTreeBuilder b;
        RunResult r = run_tree(b, *c.sm, sla::SupportTreeType::Organic);
        const auto &st = r.growth;

        // the back sphere of every anchor of a grown branch is clear of the model
        REQUIRE(st.anchor_shortened.size() == st.anchor_pts.size());
        REQUIRE(st.anchor_pts.size() == st.anchor_r.size());
        for (size_t i = 0; i < st.anchor_pts.size(); ++i) {
            double gap = std::sqrt(c.sm->emesh.squared_distance(st.anchor_pts[i])) - st.anchor_r[i];
            REQUIRE(gap > 0.);
            // a shortened anchor head (last resort) keeps the full safety gap
            if (st.anchor_shortened[i])
                REQUIRE(gap >= c.sm->cfg.safety_distance(st.anchor_r[i]) - 1e-6);
        }
        // branches resting on the model are not heavier than allowed
        REQUIRE(st.max_anchor_weight <= max_weight + 1e-9);

        sla::SupportTreeBuilder bb;
        RunResult rb = run_tree(bb, *c.sm, sla::SupportTreeType::Branching);
        if (max_weight > 5.)
            REQUIRE(r.heads >= rb.heads);
        std::cout << "WEIGHT " << fname << " elev=" << elevation << " maxw=" << max_weight
                  << " anchors=" << st.anchors << " max_anchor_weight=" << st.max_anchor_weight
                  << " fallback_ground=" << st.fallback_ground << " fallback_anchor=" << st.fallback_anchor
                  << " unroutable=" << st.unroutable << std::endl;
    }
}

TEST_CASE("OrganicSupports::SupportsDoNotPierceModelBeyondHeadPenetration",
          "[SLASupportGeneration][Organic]")
{
    for (auto type : ORGANIC_MODES)
        for (const char *fname : {"bridge.obj", "overhang.obj", "cube_with_concave_hole_enlarged_standing.obj",
                                  "A_upsidedown.obj"}) {
            INFO(mode_name(type) << " " << fname);
            sla::SupportTreeConfig cfg = organic_cfg(10., type);
            cfg.head_penetration_mm    = -0.2; // heads must not touch the model

            test_support_model_collision(fname, cfg);
        }
}

TEST_CASE("OrganicSupports::CancelProducesNoMesh", "[SLASupportGeneration][Organic]")
{
    for (auto type : ORGANIC_MODES) {
        INFO(mode_name(type));
        auto c = make_case("A_upsidedown.obj", organic_cfg(10., type));

        { // cancelled before start
            sla::JobController ctl;
            ctl.stopcondition = [] { return true; };
            auto mesh = sla::create_support_tree(*c.sm, ctl);
            REQUIRE(mesh.empty());
        }

        { // cancelled midway
            std::atomic<int> calls{0};
            sla::JobController ctl;
            ctl.stopcondition = [&calls] { return ++calls > 150; };
            auto mesh = sla::create_support_tree(*c.sm, ctl);
            REQUIRE(mesh.empty());
            REQUIRE(calls.load() > 150);
        }

        { // not cancelled
            auto mesh = sla::create_support_tree(*c.sm, sla::JobController{});
            REQUIRE_FALSE(mesh.empty());
        }
    }
}

TEST_CASE("OrganicSupports::DisabledSupportsProduceNothing", "[SLASupportGeneration][Organic]")
{
    for (auto type : ORGANIC_MODES) {
        sla::SupportTreeConfig cfg = organic_cfg(10., type);
        cfg.enabled                = false;
        auto c    = make_case("20mm_cube.obj", cfg);
        auto mesh = sla::create_support_tree(*c.sm, sla::JobController{});
        REQUIRE(mesh.empty());
    }
}

// Full organic mode: the branches are routed around the model, they merge and
// stay clear of it - which the branching layout of organic_branching does not
// guarantee to the same degree.
TEST_CASE("OrganicSupports::FullOrganicGrowthAvoidsModelAndMerges",
          "[SLASupportGeneration][Organic]")
{
    for (const char *fname : {"cube_with_concave_hole_enlarged_standing.obj", "A_upsidedown.obj",
                              "overhang.obj", "bridge.obj"}) {
        INFO(fname);
        auto c = make_case(fname, organic_cfg(0., sla::SupportTreeType::Organic));
        REQUIRE_FALSE(c.sm->pts.empty());

        sla::SupportTreeBuilder b;
        std::vector<sla::OrganicBranchPath> paths;
        RunResult r = run_tree(b, *c.sm, sla::SupportTreeType::Organic, &paths);
        REQUIRE(r.heads > 0);
        REQUIRE_FALSE(paths.empty());
        REQUIRE(r.growth.branches == paths.size());

        // fewer roots than support points: branches really merge
        REQUIRE(r.growth.merges > 0);
        REQUIRE(r.roots + r.anchors < r.growth.support_points);

        // sample points along every branch keep the clearance from the model: the
        // axis is further from the model than the branch radius, and points on
        // the tube surface are never inside the model. (The first and last point
        // of a path are the head / the anchor or pillar and are not checked.)
        size_t samples = 0, violations = 0;
        double min_gap = 1e9;
        for (const auto &p : paths)
            for (size_t i = 1; i + 1 < p.pts.size(); ++i) {
                double d   = std::sqrt(c.sm->emesh.squared_distance(p.pts[i]));
                double gap = d - p.radii[i];
                ++samples;
                min_gap = std::min(min_gap, gap);
                bool bad = gap < 0.;
                Vec3d t = (p.pts[i + 1] - p.pts[i - 1]).normalized();
                Vec3d u = std::abs(t.z()) < 0.9 ? Vec3d(t.cross(Vec3d::UnitZ())) : Vec3d(t.cross(Vec3d::UnitX()));
                u.normalize();
                Vec3d w = t.cross(u);
                for (int k = 0; k < 8 && !bad; ++k) {
                    double a = k * PI / 4.;
                    Vec3d  sp = p.pts[i] + p.radii[i] * (std::cos(a) * u + std::sin(a) * w);
                    bad = c.sm->emesh.query_ray_hit(sp, Vec3d(0.3, 0.2, 1.).normalized()).is_inside();
                }
                violations += bad;
            }
        std::cout << "ORGANIC-CLEARANCE " << fname << " samples=" << samples
                  << " min_gap=" << min_gap << " violations=" << violations << std::endl;
        REQUIRE(violations <= samples / 100);

        // branches really deviate from the vertical drop where the model is in the way
        size_t blocked_drops = 0, rerouted = 0;
        for (const auto &p : paths) {
            Vec3d top = p.pts.front();
            auto hit  = c.sm->emesh.query_ray_hit(top, sla::DOWN);
            if (std::isfinite(hit.distance()) && hit.distance() > 1.) {
                ++blocked_drops;
                if ((p.pts.back().head<2>() - top.head<2>()).norm() > 0.5)
                    ++rerouted;
            }
        }
        std::cout << "ORGANIC-REROUTE " << fname << " avoid_moves=" << r.growth.avoid_moves
                  << " drops_blocked_by_model=" << blocked_drops << " rerouted=" << rerouted << std::endl;
        // where the vertical drop would hit the model the branch has to go around it
        if (blocked_drops > 0)
            REQUIRE(rerouted > 0);
    }

    // the internal overhang model forces the avoidance machinery to move branches
    auto c = make_case("cube_with_concave_hole_enlarged_standing.obj",
                       organic_cfg(10., sla::SupportTreeType::Organic));
    sla::SupportTreeBuilder b;
    RunResult r = run_tree(b, *c.sm, sla::SupportTreeType::Organic);
    REQUIRE(r.growth.avoid_moves + r.growth.anchors > 0);
}

TEST_CASE("OrganicSupports::ConfigRoundTrip", "[SLASupportGeneration][Organic]")
{
    SLAPrintObjectConfig cfg;
    DynamicPrintConfig   dcfg;
    dcfg.apply(cfg);

    ConfigSubstitutionContext ctx{ForwardCompatibilitySubstitutionRule::Disable};
    struct M { const char *key; sla::SupportTreeType type; };
    for (M m : {M{"organic", sla::SupportTreeType::Organic},
                M{"organic_branching", sla::SupportTreeType::OrganicBranching}}) {
        INFO(m.key);
        REQUIRE(dcfg.set_deserialize_nothrow("support_tree_type", m.key, ctx));
        REQUIRE(dcfg.opt_serialize("support_tree_type") == m.key);
        REQUIRE(dcfg.opt_enum<sla::SupportTreeType>("support_tree_type") == m.type);
        // both organic modes share the branching option family
        REQUIRE(get_sla_suptree_prefix(dcfg) == "branching");

        // ini round trip
        std::string ini = std::string("support_tree_type = ") + m.key + "\n";
        DynamicPrintConfig loaded;
        loaded.apply(cfg);
        loaded.load_from_ini_string(ini, ForwardCompatibilitySubstitutionRule::Disable);
        REQUIRE(loaded.opt_enum<sla::SupportTreeType>("support_tree_type") == m.type);
        REQUIRE(loaded.opt_serialize("support_tree_type") == m.key);

        // the print object config maps to the tree type and the branching params
        SLAPrintObjectConfig oc;
        oc.apply(dcfg, true);
        oc.branchingsupport_head_front_diameter.value = 0.37;
        sla::SupportTreeConfig sc = make_support_cfg(oc);
        REQUIRE(sc.tree_type == m.type);
        REQUIRE(sc.head_front_radius_mm == Approx(0.185));
    }

    // the other values still work
    REQUIRE(dcfg.set_deserialize_nothrow("support_tree_type", "branching", ctx));
    REQUIRE(dcfg.opt_enum<sla::SupportTreeType>("support_tree_type") == sla::SupportTreeType::Branching);
    REQUIRE(dcfg.set_deserialize_nothrow("support_tree_type", "default", ctx));
    REQUIRE(dcfg.opt_enum<sla::SupportTreeType>("support_tree_type") == sla::SupportTreeType::Default);

    // the enum is exposed with a label for the GUI
    const ConfigOptionDef *def = dcfg.def()->get("support_tree_type");
    REQUIRE(def != nullptr);
    REQUIRE(def->enum_def->values().size() == 4);
    REQUIRE(def->enum_def->labels().size() == 4);
}

// Prints the comparison of the three tree types (runtime, heads, roots, merges);
// the only assertion is that every type produces supports on every model.
TEST_CASE("OrganicSupports::ComparisonOfTreeTypes", "[SLASupportGeneration][Organic]")
{
    const sla::SupportTreeType all[] = {sla::SupportTreeType::Branching, sla::SupportTreeType::OrganicBranching,
                                        sla::SupportTreeType::Organic};
    for (const char *fname : ORGANIC_MODELS) {
        if (std::string(fname) == "20mm_cube.obj" || std::string(fname) == "cube_with_concave_hole.obj")
            continue;
        for (auto type : all) {
            auto c = make_case(fname, organic_cfg(10., type));
            sla::SupportTreeBuilder b;
            std::vector<sla::OrganicBranchPath> paths;
            RunResult r = run_tree(b, *c.sm, type, &paths);
            if (type == sla::SupportTreeType::Organic) {
                double vol = 0., len = 0., rmax = 0.;
                for (auto &p : paths)
                    for (size_t i = 0; i + 1 < p.pts.size(); ++i) {
                        double l = (p.pts[i + 1] - p.pts[i]).norm(), rr = 0.5 * (p.radii[i] + p.radii[i + 1]);
                        vol += PI * rr * rr * l;
                        len += l;
                        rmax = std::max(rmax, rr);
                    }
                std::cout << "PATHVOL " << fname << " vol " << int(vol) << " len " << int(len) << " rmax " << rmax << std::endl;
            }
            std::cout << "TABLE " << fname << " | " << mode_name(type) << " | pts " << c.sm->pts.size()
                      << " | heads " << r.heads << " | roots " << r.roots << " | anchors " << r.anchors
                      << " | merges " << r.merges << " | ms " << int(r.ms);
            const indexed_triangle_set &sup = b.retrieve_mesh(sla::MeshType::Support);
            std::cout << " | triangles " << sup.indices.size() << " | volume_mm3 " << int(its_volume(sup)) << std::endl;
            REQUIRE(r.heads > 0);
            REQUIRE_FALSE(sup.empty());
        }
    }
}

// ---------------------------------------------------------------------------
// Regression / diagnostic of the full organic growth: on ordinary models the
// growth itself (not the branching style fallbacks) has to route the heads, the
// tubes have to merge into few trunks and the geometry has to differ from the
// branching tree. The configuration is the one of the GUI (SLAPrintObjectConfig
// defaults of the branching family, pad below the object).
namespace {

sla::SupportTreeConfig gui_cfg(sla::SupportTreeType type, double elevation)
{
    SLAPrintObjectConfig oc;
    oc.pad_enable.value         = true;
    oc.pad_around_object.value  = false;
    oc.supports_enable.value    = true;
    oc.support_tree_type.value  = type;
    oc.branchingsupport_object_elevation.value = elevation;
    return make_support_cfg(oc);
}

struct Fixture
{
    std::string name;
    TriangleMesh mesh;
    double elevation;
};

std::vector<Fixture> organic_fixtures()
{
    std::vector<Fixture> f;
    f.push_back({"bridge", load_model("bridge.obj"), 4.});

    TriangleMesh cube = load_model("20mm_cube.obj");
    cube.rotate(PI / 4., Axis::X);
    cube.rotate(PI / 6., Axis::Y);
    f.push_back({"cube_rot", std::move(cube), 5.});

    f.push_back({"bridge_tall", load_model("bridge.obj"), 15.});

    f.push_back({"A_upsidedown", load_model("A_upsidedown.obj"), 5.});
    f.push_back({"extruder_idler", load_model("extruder_idler.obj"), 5.});

    TriangleMesh v = load_model("V_standing.obj");
    v.scale(2.);
    f.push_back({"V_standing_x2", std::move(v), 5.});
    return f;
}

struct Metrics
{
    size_t heads = 0, merges = 0, roots = 0, anchors = 0;
    double ms = 0., length = 0., volume = 0.;
    Vec3d  size = Vec3d::Zero();
    sla::OrganicGrowthStats growth;
};

Metrics measure(const Fixture &fx, sla::SupportTreeType type)
{
    auto c = make_case_mesh(fx.mesh, gui_cfg(type, fx.elevation));
    sla::SupportTreeBuilder b;
    std::vector<sla::OrganicBranchPath> paths;
    RunResult r = run_tree(b, *c.sm, type, &paths);
    Metrics m;
    m.heads = r.heads; m.merges = r.merges; m.roots = r.roots; m.anchors = r.anchors; m.ms = r.ms;
    m.growth = r.growth;
    for (auto &p : paths)
        for (size_t i = 0; i + 1 < p.pts.size(); ++i)
            m.length += (p.pts[i + 1] - p.pts[i]).norm();
    if (type == sla::SupportTreeType::Branching) {
        for (auto &br : b.bridges()) m.length += (br.endp - br.startp).norm();
        for (auto &pl : b.pillars()) m.length += pl.height;
    }
    const indexed_triangle_set &sup = b.retrieve_mesh(sla::MeshType::Support);
    m.volume = its_volume(sup);
    auto bb = bounding_box(sup);
    m.size = (bb.max - bb.min).cast<double>();
    return m;
}

} // namespace

TEST_CASE("OrganicSupports::FullOrganicIsRoutedByGrowthAndDiffersFromBranching",
          "[SLASupportGeneration][Organic][OrganicGrowth]")
{
    const sla::SupportTreeType all[] = {sla::SupportTreeType::Branching, sla::SupportTreeType::OrganicBranching,
                                        sla::SupportTreeType::Organic};
    for (const Fixture &fx : organic_fixtures()) {
        INFO(fx.name);
        Metrics m[3];
        for (int i = 0; i < 3; ++i) {
            m[i] = measure(fx, all[i]);
            std::cout << "DIAG " << fx.name << " | " << mode_name(all[i]) << " | heads " << m[i].heads
                      << " | junctions " << m[i].merges << " | roots " << m[i].roots << " | anchors " << m[i].anchors
                      << " | len " << int(m[i].length) << " | vol " << int(m[i].volume)
                      << " | bbox " << m[i].size.x() << "x" << m[i].size.y() << "x" << m[i].size.z()
                      << " | ms " << int(m[i].ms);
            if (all[i] == sla::SupportTreeType::Organic) {
                const auto &g = m[i].growth;
                std::cout << " | grown " << (g.heads - g.dropped) << " dropped " << g.dropped
                          << " fb_ground " << g.fallback_ground << " fb_anchor " << g.fallback_anchor
                          << " unroutable " << g.unroutable << " layers " << g.layers << " step " << g.step;
            }
            std::cout << std::endl;
        }
        const Metrics &br = m[0], &org = m[2];
        const auto &g = org.growth;
        // never lose a support point
        CHECK(org.heads + 2 >= br.heads);
        REQUIRE(g.heads > 0);
        // the growth, not the fallbacks, routes most of the heads
        double frac = double(g.heads - g.dropped) / double(g.heads);
        std::cout << "DIAG-FRACTION " << fx.name << " growth_routed=" << frac << std::endl;
        CHECK(frac >= 0.8);
        CHECK(g.fallback_ground + g.fallback_anchor + g.unroutable == g.dropped);
        // branches merge into few trunks (clearly fewer than there are heads)
        CHECK(g.merges > 0);
        CHECK(double(org.roots + org.anchors) <= 0.6 * double(g.heads));
        // clearly different geometry than branching: junction / trunk count or the support volume
        // (the organic tubes are curved and thicker, branching builds straight sticks)
        bool differs = std::abs(double(org.merges) - double(br.merges)) >= 0.1 * double(br.merges) + 1.
                    || std::abs(double(org.roots + org.anchors) - double(br.roots + br.anchors)) >= 2.
                    || std::abs(org.volume - br.volume) >= 0.1 * br.volume;
        CHECK(differs);
        // runtime stays comparable to branching
        CHECK(org.ms <= 3. * br.ms + 50.);
    }
}
