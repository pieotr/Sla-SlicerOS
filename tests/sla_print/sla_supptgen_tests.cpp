#include "clipper/clipper_z.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/SLA/SupportIslands/IslandCellIntersections.hpp"
#include "libslic3r/SLA/SupportIslands/SupportAlignmentTransaction.hpp"
#include "libslic3r/SLA/SupportLinkIntersections.hpp"
#include <stdexcept>
#include "libslic3r/Utils.hpp"
#include <cfenv>
#include <chrono>
#include <numeric>
#include "libslic3r/ClipperOffsetProfile.hpp"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <tbb/global_control.h>
#include <test_utils.hpp>

#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/SLA/PeninsulaMinimumOffsetReuse.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/SLA/SpatIndex.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>

#include <libslic3r/SLA/SupportIslands/SampleConfigFactory.hpp>
#include <libslic3r/SLA/SupportIslands/SampleConfig.hpp>
#include <libslic3r/SLA/SupportIslands/VoronoiGraphUtils.hpp>
#include <libslic3r/SLA/SupportIslands/UniformSupportIsland.hpp>
#include <libslic3r/SLA/SupportIslands/PolygonUtils.hpp>
#include "nanosvg/nanosvg.h"    // load SVG file
#include "sla_test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

TEST_CASE("Support alignment accepts separable contacts without changing order", "[SupGen][alignment_transaction]")
{
    SupportIslandPoints points;
    SECTION("empty and single contact") {
        { SupportAlignmentTransaction step(points); CHECK(step.commit()); }
        points.push_back(std::make_unique<SupportIslandPoint>(Point(12, 8)));
        SupportAlignmentTransaction step(points);
        points[0]->move(Point(14, 9));
        CHECK(step.commit());
        CHECK(points[0]->point == Point(14, 9));
    }
    SECTION("legacy midpoint separates a duplicate") {
        points.push_back(std::make_unique<SupportIslandPoint>(Point(0, 0)));
        points.push_back(std::make_unique<SupportIslandPoint>(Point(4, 0)));
        SupportAlignmentTransaction step(points);
        points[1]->move(Point(0, 0));
        REQUIRE(step.commit());
        REQUIRE(points.size() == 2);
        CHECK(points[0]->point == Point(0, 0));
        CHECK(points[1]->point == Point(2, 0));
    }
}

TEST_CASE("Rounded coincident contacts reject the whole alignment step", "[SupGen][alignment_transaction]")
{
    SupportIslandPoints points;
    for (const Point &p : {Point(0, 0), Point(1, 0), Point(8, 9)})
        points.push_back(std::make_unique<SupportIslandPoint>(p));
    SupportAlignmentTransaction step(points);
    points[1]->move(Point(0, 0)); // Integer halves cannot move this contact.
    points[2]->move(Point(9, 9)); // Successful unrelated moves roll back too.
    CHECK_FALSE(step.commit());
    REQUIRE(points.size() == 3);
    CHECK(points[0]->point == Point(0, 0));
    CHECK(points[1]->point == Point(1, 0));
    CHECK(points[2]->point == Point(8, 9));
    CHECK_THROWS_AS(step.commit(), std::logic_error);
}

TEST_CASE("Rejected support alignment restores concrete movement constraints", "[SupGen][alignment_transaction]")
{
    auto restriction = std::make_shared<SupportOutlineIslandPoint::RestrictionLineSequence>(
        Lines{Line(Point(0, 0), Point(100, 0))}, std::vector<double>{100.}, 100);
    auto inner = std::make_shared<ExPolygons>(ExPolygons{
        ExPolygon(Polygon{Point(100, 100), Point(200, 100), Point(200, 200), Point(100, 200)})});
    SupportIslandPoints points;
    points.push_back(std::make_unique<SupportOutlineIslandPoint>(SupportOutlineIslandPoint::Position(0, .25f), restriction));
    points.push_back(std::make_unique<SupportIslandNoMovePoint>(Point(0, 0), SupportIslandPoint::Type::permanent));
    points.push_back(std::make_unique<SupportIslandInnerPoint>(Point(150, 150), inner));
    SupportAlignmentTransaction step(points);
    points[0]->move(Point(0, 0));
    points[2]->move(Point(175, 175));
    CHECK_FALSE(step.commit());
    REQUIRE(points.size() == 3);
    auto *outline = dynamic_cast<SupportOutlineIslandPoint *>(points[0].get());
    REQUIRE(outline != nullptr);
    CHECK(outline->point == Point(25, 0));
    CHECK(outline->position.index == 0);
    CHECK(outline->position.ratio == .25f);
    CHECK(outline->restriction == restriction);
    CHECK(outline->move(Point(50, 0)) == 25);
    CHECK(outline->point == Point(50, 0));
    REQUIRE(dynamic_cast<SupportIslandNoMovePoint *>(points[1].get()) != nullptr);
    CHECK_FALSE(points[1]->can_move());
    CHECK(points[1]->type == SupportIslandPoint::Type::permanent);
    CHECK(points[1]->move(Point(77, 88)) == 0);
    CHECK(points[1]->point == Point(0, 0));
    REQUIRE(dynamic_cast<SupportIslandInnerPoint *>(points[2].get()) != nullptr);
    CHECK(points[2]->point == Point(150, 150));
    points[2]->move(Point(175, 175));
    CHECK(points[2]->point == Point(175, 175));
}

TEST_CASE("Support duplicate correction checks cancellation and rolls back", "[SupGen][alignment_transaction]")
{
    struct Cancelled {};
    SupportIslandPoints points;
    points.push_back(std::make_unique<SupportIslandPoint>(Point(0, 0)));
    points.push_back(std::make_unique<SupportIslandPoint>(Point(4, 0)));
    unsigned calls = 0;
    CHECK_THROWS_AS(([&] {
        SupportAlignmentTransaction step(points);
        points[1]->move(Point(0, 0));
        step.commit([&] { if (++calls == 2) throw Cancelled{}; });
    }()), Cancelled);
    CHECK(calls == 2);
    REQUIRE(points.size() == 2);
    CHECK(points[0]->point == Point(0, 0));
    CHECK(points[1]->point == Point(4, 0));
}

TEST_CASE("Cycling support constraints cannot loop forever during alignment", "[SupGen][alignment_transaction]")
{
    struct CyclingPoint : SupportIslandPoint {
        unsigned *moves;
        CyclingPoint(Point p, unsigned &count) : SupportIslandPoint(p), moves(&count) {}
        std::unique_ptr<SupportIslandPoint> clone() const override
        { return std::make_unique<CyclingPoint>(*this); }
        coord_t move(const Point &) override {
            ++*moves;
            // Three contacts in two possible locations cannot all separate.
            point = Point(point.x() == 0 ? 1 : 0, 0);
            return 1;
        }
    };
    unsigned moves = 0;
    SupportIslandPoints points;
    for (int x : {10, 20, 30}) points.push_back(std::make_unique<CyclingPoint>(Point(x, 0), moves));
    SupportAlignmentTransaction step(points);
    for (auto &point : points) point->point = Point(0, 0);
    CHECK_FALSE(step.commit());
    CHECK(moves > 3);
    CHECK(moves < 256);
    REQUIRE(points.size() == 3);
    CHECK(points[0]->point == Point(10, 0));
    CHECK(points[1]->point == Point(20, 0));
    CHECK(points[2]->point == Point(30, 0));
}

//#define STORE_SAMPLE_INTO_SVG_FILES "C:/data/temp/test_islands/sample_"
//#define STORE_ISLAND_ISSUES "C:/data/temp/issues/"

TEST_CASE("Overhanging point should be supported", "[SupGen]") {

    // Pyramid with 45 deg slope
    TriangleMesh mesh = make_pyramid(10.f, 10.f);
    mesh.rotate_y(float(PI));
    //mesh.WriteOBJFile("Pyramid.obj");

    sla::SupportPoints pts = calc_support_pts(mesh);

    // The overhang, which is the upside-down pyramid's edge
    Vec3f overh{0., 0., -10.};

    REQUIRE(!pts.empty());

    float dist = (overh - pts.front().pos).norm();

    for (const auto &pt : pts)
        dist = std::min(dist, (overh - pt.pos).norm());

    // Should require exactly one support point at the overhang
    REQUIRE(pts.size() > 0);
    REQUIRE(dist < 1.f);
}

double min_point_distance(const sla::SupportPoints &pts)
{
    sla::PointIndex index;

    for (size_t i = 0; i < pts.size(); ++i)
        index.insert(pts[i].pos.cast<double>(), i);

    auto d = std::numeric_limits<double>::max();
    index.foreach([&d, &index](const sla::PointIndexEl &el) {
        auto res = index.nearest(el.first, 2);
        for (const sla::PointIndexEl &r : res)
            if (r.second != el.second)
                d = std::min(d, (el.first - r.first).norm());
    });

    return d;
}

TEST_CASE("Overhanging horizontal surface should be supported", "[SupGen]") {
    double width = 10., depth = 10., height = 1.;

    TriangleMesh mesh = make_cube(width, depth, height); 
    mesh.translate(0., 0., 5.); // lift up
    // mesh.WriteOBJFile("Cuboid.obj");
    sla::SupportPoints pts = calc_support_pts(mesh);
    REQUIRE(!pts.empty());
}

template<class M> auto&& center_around_bb(M &&mesh)
{
    auto bb = mesh.bounding_box();
    mesh.translate(-bb.center().template cast<float>());

    return std::forward<M>(mesh);
}

TEST_CASE("Overhanging edge should be supported", "[SupGen]") {
    float width = 10.f, depth = 10.f, height = 5.f;

    TriangleMesh mesh = make_prism(width, depth, height);
    mesh.rotate_y(float(PI)); // rotate on its back
    mesh.translate(0., 0., height);
    mesh.WriteOBJFile("Prism.obj");

    sla::SupportPoints pts = calc_support_pts(mesh);

    Linef3 overh{ {0.f, -depth / 2.f, 0.f}, {0.f, depth / 2.f, 0.f}};

    // Get all the points closer that 1 mm to the overhanging edge:
    sla::SupportPoints overh_pts; overh_pts.reserve(pts.size());

    std::copy_if(pts.begin(), pts.end(), std::back_inserter(overh_pts),
                 [&overh](const sla::SupportPoint &pt){
                     return line_alg::distance_to(overh, Vec3d{pt.pos.cast<double>()}) < 1.;
                 });

    //double ddiff = min_point_distance(pts) - cfg.minimal_distance;
    //REQUIRE(ddiff > - 0.1 * cfg.minimal_distance);
}

TEST_CASE("Hollowed cube should be supported from the inside", "[SupGen][Hollowed]") {
    TriangleMesh mesh = make_cube(20., 20., 20.);

    hollow_mesh(mesh, HollowingConfig{});

    mesh.WriteOBJFile("cube_hollowed.obj");

    auto bb = mesh.bounding_box();
    auto h  = float(bb.max.z() - bb.min.z());
    Vec3f mv = bb.center().cast<float>() - Vec3f{0.f, 0.f, 0.5f * h};
    mesh.translate(-mv);

    sla::SupportPoints pts = calc_support_pts(mesh);
    //sla::remove_bottom_points(pts, mesh.bounding_box().min.z() + EPSILON);

    REQUIRE(!pts.empty());
}

TEST_CASE("Two parallel plates should be supported", "[SupGen][Hollowed]")
{
    double width = 20., depth = 20., height = 1.;

    TriangleMesh mesh = center_around_bb(make_cube(width + 5., depth + 5., height));
    TriangleMesh mesh_high = center_around_bb(make_cube(width, depth, height));
    mesh_high.translate(0., 0., 10.); // lift up
    mesh.merge(mesh_high);

    mesh.WriteOBJFile("parallel_plates.obj");

    sla::SupportPoints pts = calc_support_pts(mesh);
    //sla::remove_bottom_points(pts, mesh.bounding_box().min.z() + EPSILON);

    REQUIRE(!pts.empty());
}


Slic3r::Polygon create_cross_roads(double size, double width)
{
    auto r1 = PolygonUtils::create_rect(5.3 * size, width);
    r1.rotate(3.14/4);
    r1.translate(2 * size, width / 2);
    auto r2 = PolygonUtils::create_rect(6.1 * size, 3 / 4. * width);
    r2.rotate(-3.14 / 5);
    r2.translate(3 * size, width / 2);
    auto r3 = PolygonUtils::create_rect(7.9 * size, 4 / 5. * width);
    r3.translate(2*size, width/2);
    auto r4 = PolygonUtils::create_rect(5 / 6. * width, 5.7 * size);
    r4.translate(-size,3*size);
    Polygons rr = union_(Polygons({r1, r2, r3, r4}));
    return rr.front();
}

ExPolygon create_trinagle_with_hole(double size)
{
    auto hole = PolygonUtils::create_equilateral_triangle(size / 3);
    hole.reverse();
    hole.rotate(3.14 / 4);
    return ExPolygon(PolygonUtils::create_equilateral_triangle(size), hole);
}

ExPolygon create_square_with_hole(double size, double hole_size)
{
    assert(sqrt(hole_size *hole_size / 2) < size);
    auto hole = PolygonUtils::create_square(hole_size);
    hole.rotate(M_PI / 4.); // 45
    hole.reverse();
    return ExPolygon(PolygonUtils::create_square(size), hole);
}

ExPolygon create_square_with_4holes(double size, double hole_size) {
    auto hole = PolygonUtils::create_square(hole_size);
    hole.reverse();
    double size_4 = size / 4;
    auto h1 = hole;
    h1.translate(size_4, size_4);
    auto h2 = hole;
    h2.translate(-size_4, size_4);
    auto h3   = hole;
    h3.translate(size_4, -size_4);
    auto h4   = hole;
    h4.translate(-size_4, -size_4);
    ExPolygon result(PolygonUtils::create_square(size));
    result.holes = Polygons({h1, h2, h3, h4});
    return result;
}

// boudary of circle
ExPolygon create_disc(double radius, double width, size_t count_line_segments)
{
    double width_2 = width / 2;
    auto   hole    = PolygonUtils::create_circle(radius - width_2,
                                            count_line_segments);
    hole.reverse();
    return ExPolygon(PolygonUtils::create_circle(radius + width_2,
                                                 count_line_segments),
                     hole);
}

Slic3r::Polygon create_V_shape(double height, double line_width, double angle = M_PI/4) {
    double angle_2 = angle / 2;
    auto   left_side  = PolygonUtils::create_rect(line_width, height);
    auto   right_side = left_side;
    right_side.rotate(-angle_2);
    double small_move = cos(angle_2) * line_width / 2;
    double side_move  = sin(angle_2) * height / 2 + small_move;
    right_side.translate(side_move,0);
    left_side.rotate(angle_2);
    left_side.translate(-side_move, 0);
    auto bottom = PolygonUtils::create_rect(4 * small_move, line_width);
    bottom.translate(0., -cos(angle_2) * height / 2 + line_width/2);
    Polygons polygons = union_(Polygons({left_side, right_side, bottom}));
    return polygons.front();
}

ExPolygon create_tiny_wide_test_1(double wide, double tiny)
{
    double hole_size = wide;
    double width     = 2 * wide + hole_size;
    double height    = wide + hole_size + tiny;
    auto   outline   = PolygonUtils::create_rect(width, height);
    auto   hole      = PolygonUtils::create_rect(hole_size, hole_size);
    hole.reverse();
    int hole_move_y = height/2 - (hole_size/2 + tiny);
    hole.translate(0, hole_move_y);
    
    ExPolygon result(outline);
    result.holes = {hole};
    return result;
}

ExPolygon create_tiny_wide_test_2(double wide, double tiny)
{
    double hole_size = wide;
    double width     = (3 + 1) * wide + 3 * hole_size;
    double height    = 2*wide + 2*tiny + 3*hole_size;
    auto outline = PolygonUtils::create_rect( width, height);
    auto   hole      = PolygonUtils::create_rect(hole_size, hole_size);
    hole.reverse();
    auto  hole2 = hole;// copy
    auto  hole3       = hole; // copy
    auto  hole4       = hole; // copy

    int   hole_move_x = wide + hole_size;
    int   hole_move_y = wide + hole_size;
    hole.translate(hole_move_x, hole_move_y);
    hole2.translate(-hole_move_x, hole_move_y);
    hole3.translate(hole_move_x, -hole_move_y);
    hole4.translate(-hole_move_x, -hole_move_y);

    auto hole5 = PolygonUtils::create_circle(hole_size / 2, 16);
    hole5.reverse();
    auto hole6 = hole5; // copy
    hole5.translate(0, hole_move_y);
    hole6.translate(0, -hole_move_y);

    auto hole7 = PolygonUtils::create_equilateral_triangle(hole_size);
    hole7.reverse();
    auto hole8 = PolygonUtils::create_circle(hole_size/2, 7, Point(hole_move_x,0));
    hole8.reverse();
    auto hole9 = PolygonUtils::create_circle(hole_size/2, 5, Point(-hole_move_x,0));
    hole9.reverse();

    ExPolygon result(outline);
    result.holes = {hole, hole2, hole3, hole4, hole5, hole6, hole7, hole8, hole9};
    return result;
}

ExPolygon create_tiny_between_holes(double wide, double tiny)
{
    double hole_size = wide;
    double width     = 2 * wide + 2*hole_size + tiny;
    double height    = 2 * wide + hole_size;
    auto   outline   = PolygonUtils::create_rect(width, height);
    auto   holeL      = PolygonUtils::create_rect(hole_size, hole_size);
    holeL.reverse();
    auto holeR       = holeL;
    int hole_move_x = (hole_size + tiny)/2;
    holeL.translate(-hole_move_x, 0);
    holeR.translate(hole_move_x, 0);

    ExPolygon result(outline);
    result.holes = {holeL, holeR};
    return result;
}

// stress test for longest path
// needs reshape
ExPolygon create_mountains(double size) {
    return ExPolygon({{0., 0.},
                      {size, 0.},
                      {5 * size / 6, size},
                      {4 * size / 6, size / 6},
                      {3 * size / 7, 2 * size},
                      {2 * size / 7, size / 6},
                      {size / 7, size}});
}

/// Neighbor points create trouble for voronoi - test of neccessary offseting(closing) of contour
ExPolygon create_cylinder_bottom_slice() {
    indexed_triangle_set its_cylinder = its_make_cylinder(6.6551999999999998, 11.800000000000001);
    MeshSlicingParams param;
    Polygons polygons = slice_mesh(its_cylinder, 0.0125000002, param);
    return ExPolygon{polygons.front()};
}

ExPolygon load_frog(){
    TriangleMesh mesh = load_model("frog_legs.obj");
    std::vector<ExPolygons> slices = slice_mesh_ex(mesh.its, {0.1f});
    return slices.front()[1];
}

ExPolygon load_svg(const std::string& svg_filepath) {
    struct NSVGimage *image = nsvgParseFromFile(svg_filepath.c_str(), "px", 96);
    ScopeGuard sg_image([&image] { nsvgDelete(image); });

    auto to_polygon = [](NSVGpath *path) { 
        Polygon r;
        r.points.reserve(path->npts);
        for (int i = 0; i < path->npts; i++)
            r.points.push_back(Point(path->pts[2 * i], path->pts[2 * i + 1]));
        return r;
    };

    for (NSVGshape *shape_ptr = image->shapes; shape_ptr != NULL; shape_ptr = shape_ptr->next) {
        const NSVGshape &shape = *shape_ptr;
        if (!(shape.flags & NSVG_FLAGS_VISIBLE)) continue; // is visible
        if (shape.fill.type != NSVG_PAINT_NONE) continue; // is not used fill
        if (shape.stroke.type == NSVG_PAINT_NONE) continue; // exist stroke
        //if (shape.strokeWidth < 1e-5f) continue; // is visible stroke width
        //if (shape.stroke.color != 4278190261) continue; // is red
        ExPolygon result;
        for (NSVGpath *path = shape.paths; path != NULL; path = path->next) {
            // Path order is reverse to path in file
            if (path->next == NULL) // last path is contour
                result.contour = to_polygon(path);
            else
                result.holes.push_back(to_polygon(path));        
        }
        return result;
    }
    REQUIRE(false);
    return {};
}

ExPolygons createTestIslands(double size)
{
    std::string dir = std::string(TEST_DATA_DIR PATH_SEPARATOR) + "sla_islands/";
    bool useFrogLeg = false;    
    // need post reorganization of longest path
    ExPolygons result = {
        // one support point
        ExPolygon(PolygonUtils::create_equilateral_triangle(size)), 
        ExPolygon(PolygonUtils::create_square(size)),
        ExPolygon(PolygonUtils::create_rect(size / 2, size)),
        ExPolygon(PolygonUtils::create_isosceles_triangle(size / 2, 3 * size / 2)), // small sharp triangle
        ExPolygon(PolygonUtils::create_circle(size / 2, 10)),
        create_square_with_4holes(size, size / 4),
        create_disc(size/4, size / 4, 10),
        ExPolygon(create_V_shape(2*size/3, size / 4)),

        // two support points
        ExPolygon(PolygonUtils::create_isosceles_triangle(size / 2, 3 * size)), // small sharp triangle
        ExPolygon(PolygonUtils::create_rect(size / 2, 3 * size)),
        ExPolygon(create_V_shape(1.5*size, size/3)),

        // tiny line support points
        ExPolygon(PolygonUtils::create_rect(size / 2, 10 * size)), // long line
        ExPolygon(create_V_shape(size*4, size / 3)),
        ExPolygon(create_cross_roads(size, size / 3)),
        create_disc(3*size, size / 4, 30),
        create_disc(2*size, size, 12), // 3 points
        create_square_with_4holes(5 * size, 5 * size / 2 - size / 3),

        // Tiny and wide part together with holes
        ExPolygon(PolygonUtils::create_isosceles_triangle(5. * size, 40. * size)),
        create_tiny_wide_test_1(3 * size, 2 / 3. * size),
        create_tiny_wide_test_2(3 * size, 2 / 3. * size),
        create_tiny_between_holes(3 * size, 2 / 3. * size),

        ExPolygon(PolygonUtils::create_equilateral_triangle(scale_(18.6))),
        create_cylinder_bottom_slice(),
        load_svg(dir + "lm_issue.svg"), // change from thick to thin and vice versa on circle
        load_svg(dir + "SPE-2674.svg"), // center of longest path lay inside of the VD node
        load_svg(dir + "SPE-2674_2.svg"), // missing Voronoi vertex even after the rotation of input.

        // still problem
        // three support points
        ExPolygon(PolygonUtils::create_equilateral_triangle(3 * size)), 
        ExPolygon(PolygonUtils::create_circle(size, 20)),

        create_mountains(size),
        create_trinagle_with_hole(size),
        create_square_with_hole(size, size / 2),
        create_square_with_hole(size, size / 3)
    };    
    if (useFrogLeg)
        result.push_back(load_frog());
    return result;
}

Points createNet(const BoundingBox& bounding_box, double distance)
{ 
    Point  size       = bounding_box.size();
    double distance_2 = distance / 2;
    int    cols1 = static_cast<int>(floor(size.x() / distance))+1;
    int    cols2 = static_cast<int>(floor((size.x() - distance_2) / distance))+1;
    // equilateral triangle height with side distance
    double h      = sqrt(distance * distance - distance_2 * distance_2);
    int    rows   = static_cast<int>(floor(size.y() / h)) +1;
    int    rows_2 = rows / 2;
    size_t count_points = rows_2 * (cols1 + static_cast<size_t>(cols2));
    if (rows % 2 == 1) count_points += cols2;
    Points result;
    result.reserve(count_points);
    bool   isOdd = true;
    Point offset = bounding_box.min;
    double x_max = offset.x() + static_cast<double>(size.x());
    double y_max  = offset.y() + static_cast<double>(size.y());
    for (double y = offset.y(); y <= y_max; y += h) {
        double x_offset = offset.x();
        if (isOdd) x_offset += distance_2;
        isOdd = !isOdd;
        for (double x = x_offset; x <= x_max; x += distance) {
            result.emplace_back(x, y);
        }
    }
    assert(result.size() == count_points);
    return result; 
}

// create uniform triangle net and return points laying inside island
Points rasterize(const ExPolygon &island, double distance) {
    BoundingBox bb;
    for (const Point &pt : island.contour.points) bb.merge(pt);
    Points      fullNet = createNet(bb, distance);
    Points result;
    result.reserve(fullNet.size());
    std::copy_if(fullNet.begin(), fullNet.end(), std::back_inserter(result),
                 [&island](const Point &p) { return island.contains(p); });
    return result;
}

SupportIslandPoints test_island_sampling(const ExPolygon &   island,
                                        const SampleConfig &config)
{
    auto points = uniform_support_island(island, {}, config);

    Points chck_points = rasterize(island, config.head_radius); // TODO: Use resolution of printer
    bool is_island_supported = true; // Check rasterized island points that exist support point in max_distance
    double max_distance = config.thick_inner_max_distance;
    std::vector<double> point_distances(chck_points.size(), {max_distance + 1});
    for (size_t index = 0; index < chck_points.size(); ++index) { 
        const Point &chck_point  = chck_points[index];
        double &min_distance = point_distances[index];
        bool         exist_close_support_point = false;
        for (const auto &island_point : points) {
            const Point& p = island_point->point;
            Point abs_diff(fabs(p.x() - chck_point.x()),
                           fabs(p.y() - chck_point.y()));
            if (abs_diff.x() < min_distance && abs_diff.y() < min_distance) {
                double distance = sqrt((double) abs_diff.x() * abs_diff.x() +
                                       (double) abs_diff.y() * abs_diff.y());
                if (min_distance > distance) {
                    min_distance = distance;
                    exist_close_support_point = true;
                }
            }
        }
        if (!exist_close_support_point) is_island_supported = false;
    }

    bool is_all_points_inside_island = true;
    for (const auto &point : points)
        if (!island.contains(point->point))
            is_all_points_inside_island = false;
    
#ifdef STORE_ISLAND_ISSUES
    if (!is_island_supported || !is_all_points_inside_island) { // visualize
        static int  counter = 0;
        BoundingBox bb;
        for (const Point &pt : island.contour.points) bb.merge(pt);
        SVG svg(STORE_ISLAND_ISSUES + std::string("Error") + std::to_string(++counter) + ".svg", bb);
        svg.draw(island, "blue", 0.5f);
        for (auto& p : points)
            svg.draw(p->point, island.contains(p->point) ? "lightgreen" : "red", config.head_radius);
        for (size_t index = 0; index < chck_points.size(); ++index) {
            const Point &chck_point = chck_points[index];
            double       distance   = point_distances[index];
            bool         isOk       = distance < max_distance;
            std::string  color      = (isOk) ? "gray" : "red";
            svg.draw(chck_point, color, config.head_radius / 4);
        }
    }
#endif // STORE_ISLAND_ISSUES

    CHECK(!points.empty());
    CHECK(is_all_points_inside_island);
    // CHECK(is_island_supported); // TODO: skip special cases with one point and 2 points

    return points;
}

SampleConfig create_sample_config(double size) {
    float head_diameter = .4f;
    return SampleConfigFactory::create(head_diameter);

    //coord_t max_distance = 3 * size + 0.1;
    //SampleConfig cfg;
    //cfg.head_radius = size / 4;
    //cfg.minimal_distance_from_outline = cfg.head_radius;
    //cfg.maximal_distance_from_outline = max_distance/4;
    //cfg.max_length_for_one_support_point = 2*size;
    //cfg.max_length_for_two_support_points = 4*size;
    //cfg.thin_max_width = size;
    //cfg.thick_min_width = cfg.thin_max_width;
    //cfg.thick_outline_max_distance       = max_distance;

    //cfg.minimal_move       = static_cast<coord_t>(size/30);
    //cfg.count_iteration = 100; 
    //cfg.max_align_distance = 0;
    //return cfg;
} 

#ifdef STORE_SAMPLE_INTO_SVG_FILES
namespace {
void store_sample(const SupportIslandPoints &samples, const ExPolygon &island) { 
    static int counter = 0;
    BoundingBox bb(island.contour.points);
    SVG svg((STORE_SAMPLE_INTO_SVG_FILES + std::to_string(counter++) + ".svg").c_str(), bb); 

    double mm = scale_(1);
    svg.draw(island, "lightgray");
    for (const auto &s : samples) 
        svg.draw(s->point, "blue", 0.2*mm);
    

    // draw resolution
    Point p(bb.min.x() + 1e6, bb.max.y() - 2e6);
    svg.draw_text(p, (std::to_string(samples.size()) + " samples").c_str(), "black");
    svg.draw_text(p - Point(0., 1.8e6), "Scale 1 cm ", "black");
    Point  start = p - Point(0., 2.3e6);
    svg.draw(Line(start + Point(0., 5e5), start + Point(10*mm, 5e5)), "black", 2e5);
    svg.draw(Line(start + Point(0., -5e5), start + Point(10*mm, -5e5)), "black", 2e5);
    svg.draw(Line(start + Point(10*mm, 5e5), start + Point(10*mm, -5e5)), "black", 2e5);
    for (int i=0; i<10;i+=2)
        svg.draw(Line(start + Point(i*mm, 0.), start + Point((i+1)*mm, 0.)), "black", 1e6);
}
} // namespace
#endif // STORE_SAMPLE_INTO_SVG_FILES

/// <summary>
/// Check for correct sampling of island
/// </summary>
TEST_CASE("Uniform sample test islands", "[SupGen], [VoronoiSkeleton]")
{
    //set_logging_level(5);
    float head_diameter = .4f;
    SampleConfig cfg = SampleConfigFactory::create(head_diameter);
    //cfg.path = "C:/data/temp/islands/<<order>>.svg";
    ExPolygons islands = createTestIslands(7 * scale_(head_diameter));
    for (ExPolygon &island : islands) {
        // information for debug which island cause problem
        [[maybe_unused]] size_t debug_index = &island - &islands.front(); 

        SupportIslandPoints points = test_island_sampling(island, cfg);
#ifdef STORE_SAMPLE_INTO_SVG_FILES
        store_sample(points, island);
#endif // STORE_SAMPLE_INTO_SVG_FILES
        
        double angle  = 3.14 / 3; // cca 60 degree

        island.rotate(angle);
        SupportIslandPoints pointsR = test_island_sampling(island, cfg);
#ifdef STORE_SAMPLE_INTO_SVG_FILES
        store_sample(pointsR, island);
#endif // STORE_SAMPLE_INTO_SVG_FILES

        // points count should be the same
        //CHECK(points.size() == pointsR.size())
    }
}

TEST_CASE("Sample island with config", "[SupportIsland]") {
    // set_logging_level(5);
    SampleConfig cfg{
        /*thin_max_distance*/ 5832568,
        /*thick_inner_max_distance*/ 7290710,
        /*thick_outline_max_distance*/ 5468032,
        /*head_radius*/ 250000,
        /*minimal_distance_from_outline*/ 250000,
        /*maximal_distance_from_outline*/ 1944189,
        /*max_length_for_one_support_point*/ 1869413,
        /*max_length_for_two_support_points*/ 7290710,
        /*max_length_ratio_for_two_support_points*/ 0.250000000f,
        /*thin_max_width*/ 4673532,
        /*thick_min_width*/ 4019237,
        /*min_part_length*/ 5832568,
        /*minimal_move*/ 100000,
        /*count_iteration*/ 30,
        /*max_align_distance*/ 3645355,
        /*simplification_tolerance*/ 50000.000000000007
        //*path*/, "C:/data/temp/islands/<<order>>.svg" // need define OPTION_TO_STORE_ISLAND in SampleConfig.hpp
    };
    std::string dir = std::string(TEST_DATA_DIR PATH_SEPARATOR) + "sla_islands/";
    ExPolygon island = load_svg(dir + "SPE-2709.svg"); // Bad field creation
    SupportIslandPoints points = test_island_sampling(island, cfg);
    // in time of write poins.size() == 39
    CHECK(points.size() > 22); // not only thin parts
}

TEST_CASE("Disable visualization", "[hide]") 
{
    CHECK(true);
#ifdef STORE_SAMPLE_INTO_SVG_FILES
    CHECK(false);
#endif // STORE_SAMPLE_INTO_SVG_FILES
#ifdef STORE_ISLAND_ISSUES
    CHECK(false);
#endif // STORE_ISLAND_ISSUES
#ifdef USE_ISLAND_GUI_FOR_SETTINGS
    CHECK(false);
#endif // USE_ISLAND_GUI_FOR_SETTINGS
    CHECK(is_uniform_support_island_visualization_disabled());
}

TEST_CASE("SPE-2714 3DBenchy - Sample island with config", "[SupportIsland]") {
    // set_logging_level(5);
    SampleConfig cfg{
        /*thin_max_distance*/ 5832568,
        /*thick_inner_max_distance*/ 7290710,
        /*thick_outline_max_distance*/ 5468032,
        /*head_radius*/ 250000,
        /*minimal_distance_from_outline*/ 250000,
        /*maximal_distance_from_outline*/ 1944189,
        /*max_length_for_one_support_point*/ 1869413,
        /*max_length_for_two_support_points*/ 7290710,
        /*max_length_ratio_for_two_support_points*/ 0.250000000f,
        /*thin_max_width*/ 4673532,
        /*thick_min_width*/ 4019237,
        /*min_part_length*/ 5832568,
        /*minimal_move*/ 100000,
        /*count_iteration*/ 30,
        /*max_align_distance*/ 3645355,
        /*simplification_tolerance*/ 50000.000000000007
        //*path*/, "C:/data/temp/islands/spe_2714_<<order>>.svg" // define OPTION_TO_STORE_ISLAND in SampleConfig.hpp
    };
    std::string dir = std::string(TEST_DATA_DIR PATH_SEPARATOR) + "sla_islands/";
    ExPolygon island = load_svg(dir + "SPE-2714.svg"); // Bad field creation
    SupportIslandPoints points = test_island_sampling(island, cfg);
    CHECK(points.size() > 22); // Before fix it not finished
}

TEST_CASE("Island cell batch clipping matches ordered clipping including holes", "[sla_island_cells]") {
    auto rectangle=[](double x0,double y0,double x1,double y1) {
        return Polygon({Point(scale_(x0),scale_(y0)),Point(scale_(x1),scale_(y0)),
            Point(scale_(x1),scale_(y1)),Point(scale_(x0),scale_(y1))});
    };
    ExPolygon island(rectangle(0,0,30,20));
    auto hole=rectangle(8,4,12,16);hole.make_clockwise();island.holes.push_back(hole);
    Polygons cells;
    for(int y=-5;y<=25;y+=5)for(int x=-5;x<=35;x+=5)cells.push_back(rectangle(x,y,x+6,y+6));
    cells.push_back(Polygon({Point(0,0),Point(1,1)}));
    std::vector<uint8_t> enabled(cells.size(),1);
    for(size_t i=0;i<cells.size();i+=7)enabled[i]=0;
    cells.push_back(Polygon({Point(1,1),Point(30000001,7000003),Point(23000003,19000001)}));
    enabled.push_back(1);
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment(){std::fegetenv(&state);}
        ~RestoreEnvironment(){std::fesetenv(&state);}
    } restore;
    const auto rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD);
    REQUIRE(std::fesetround(rounding)==0);
    const auto original_cells=cells;const auto original_island=island;
    const auto result=intersect_island_cells(cells,island,enabled);
    CHECK(std::fegetround()==rounding);
    REQUIRE(result.size()==cells.size());
    for(size_t i=0;i<cells.size();++i) {
        const auto expected=enabled[i] && cells[i].size()>=3 ? intersection(cells[i],island) : Polygons{};
        CHECK(result[i]==expected);
    }
    CHECK(cells==original_cells);CHECK(island==original_island);
    CHECK_THROWS(intersect_island_cells(cells,island,{}));
    CHECK_THROWS_AS(intersect_island_cells(cells,island,enabled,[]{throw std::runtime_error("cancel fixture");}),std::runtime_error);
    std::atomic<unsigned> checks{0};
    CHECK_THROWS_AS(intersect_island_cells(cells,island,enabled,[&] {
        if(checks.fetch_add(1)>=4)throw std::runtime_error("mid-batch cancellation");
    }),std::runtime_error);
    CHECK(checks.load()>=5);
    CHECK(std::fegetround()==rounding);
    CHECK(cells==original_cells);CHECK(island==original_island);
}

TEST_CASE("Support preparation checks cancellation on short layer stacks", "[sla_preparation_cancel]") {
    const ExPolygon shape(Polygon({Point(0,0),Point(scaled<coord_t>(30.),0),
        Point(scaled<coord_t>(30.),scaled<coord_t>(20.)),Point(0,scaled<coord_t>(20.))}));
    const std::vector<ExPolygons> input{{shape},{shape}};
    const auto original=input;
    const unsigned cancel_at=GENERATE(0u,1u,3u,8u,12u,18u);
    CAPTURE(cancel_at);
    std::atomic<unsigned> checks{0};
    struct PreparationCancelled {};
    PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    CHECK_THROWS_AS(prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config,[&] {
        if(checks.fetch_add(1)>=cancel_at)throw PreparationCancelled{};
    }),PreparationCancelled);
    CHECK(checks.load()>=cancel_at+1);
    CHECK(input==original);
}

TEST_CASE("Parallel support preparation preserves links and derived geometry", "[sla_preparation_parallel]") {
    const auto rectangle=[](double x0,double y0,double x1,double y1) {
        return ExPolygon(Polygon({Point(scale_(x0),scale_(y0)),Point(scale_(x1),scale_(y0)),
            Point(scale_(x1),scale_(y1)),Point(scale_(x0),scale_(y1))}));
    };
    std::vector<ExPolygons> input;
    std::vector<float> heights;
    for(size_t i=0;i<19;++i) {
        auto outer=rectangle(0,0,30.+double(i%3)*3.,20.);
        auto hole=rectangle(8,8,12,12).contour;hole.make_clockwise();
        if(i%4!=0)outer.holes.push_back(hole);
        input.push_back({std::move(outer),rectangle(45,0,50,5)});
        heights.push_back(float(i+1)*.05f);
    }
    const auto original=input;
    PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    tbb::task_arena serial(1);
    auto reference=serial.execute([&] {
        return prepare_generator_data(std::vector<ExPolygons>(input),heights,config);
    });
    const int workers=GENERATE(2,4,16);
    CAPTURE(workers);
    tbb::task_arena parallel(workers);
    auto actual=parallel.execute([&] {
        return prepare_generator_data(std::vector<ExPolygons>(input),heights,config);
    });
    REQUIRE(actual.layers.size()==reference.layers.size());
    for(size_t layer=0;layer<actual.layers.size();++layer) {
        const auto &parts=actual.layers[layer].parts;
        const auto &expected=reference.layers[layer].parts;
        REQUIRE(parts.size()==expected.size());
        CHECK(actual.layers[layer].print_z==reference.layers[layer].print_z);
        for(size_t part=0;part<parts.size();++part) {
            const auto &a=parts[part];const auto &b=expected[part];
            CHECK(*a.shape==*b.shape);CHECK(a.samples==b.samples);
            CHECK(a.extend_shape==b.extend_shape);
            // Compare link destinations, not iterators into different owners.
            const auto check_links=[](const PartLinks &left,const PartLinks &right) {
                REQUIRE(left.size()==right.size());
                for(size_t i=0;i<left.size();++i)CHECK(*left[i]->shape==*right[i]->shape);
            };
            check_links(a.prev_parts,b.prev_parts);check_links(a.next_parts,b.next_parts);
            REQUIRE(a.peninsulas.size()==b.peninsulas.size());
            for(size_t i=0;i<a.peninsulas.size();++i) {
                CHECK(a.peninsulas[i].unsuported_area==b.peninsulas[i].unsuported_area);
                CHECK(a.peninsulas[i].is_outline==b.peninsulas[i].is_outline);
            }
            if(layer==0)CHECK(a.extend_shape.empty()); // the first layer has no inherited supports
            else CHECK(a.extend_shape==offset_ex(*a.shape,config.removing_delta,ClipperLib::jtSquare));
        }
    }
    CHECK(input==original);CHECK(actual.slices==original);
}

TEST_CASE("Island and peninsula cancellation leaves input intact", "[sla_support_cancel]") {
    ExPolygon island(Polygon({Point(0,0),Point(scaled<coord_t>(30.),0),Point(scaled<coord_t>(30.),scaled<coord_t>(20.)),Point(0,scaled<coord_t>(20.))}));
    const auto original=island;const Points permanent{Point(scaled<coord_t>(5.),scaled<coord_t>(5.))};
    const auto config=SampleConfigFactory::create(.4f);
    struct CancelFixture {};
    CHECK_THROWS_AS(uniform_support_island(island,permanent,config,[]{throw CancelFixture{};}),CancelFixture);
    Peninsula peninsula{island,{true,true,true,true}};
    CHECK_THROWS_AS(uniform_support_peninsula(peninsula,permanent,config,[]{throw CancelFixture{};}),CancelFixture);
    CHECK(island==original);CHECK(peninsula.unsuported_area==original);
    CHECK(permanent==Points{Point(scaled<coord_t>(5.),scaled<coord_t>(5.))});
    std::atomic<unsigned> checkpoints{0};
    CHECK_THROWS_AS(uniform_support_island(island,{},config,[&] {
        if(checkpoints.fetch_add(1)>=12)throw CancelFixture{};
    }),CancelFixture);
    CHECK(checkpoints.load()>=13);
    CHECK(island==original);
}

TEST_CASE("Support generator forwards cancellation into the first island", "[sla_support_cancel]") {
    ExPolygon shape(Polygon({Point(0,0),Point(scaled<coord_t>(30.),0),
        Point(scaled<coord_t>(30.),scaled<coord_t>(20.)),Point(0,scaled<coord_t>(20.))}));
    auto data=prepare_generator_data({{shape},{shape}},{.05f,.1f});
    const auto slices=data.slices;const auto first_parts=data.layers.front().parts.size();
    SupportPointGeneratorConfig config;config.island_configuration=SampleConfigFactory::create(.4f);
    std::atomic<unsigned> checks{0};struct CancelFixture {};
    CHECK_THROWS_AS(generate_support_points(data,config,[&] {
        if(checks.fetch_add(1)>=12)throw CancelFixture{};
    }),CancelFixture);
    CHECK(checks.load()>=13);CHECK(data.slices==slices);
    CHECK(data.layers.front().parts.size()==first_parts);CHECK(data.permanent_supports.empty());
    const auto plain=uniform_support_island(shape,{},config.island_configuration);
    std::atomic<unsigned> observed{0};
    const auto checked=uniform_support_island(shape,{},config.island_configuration,[&]{observed.fetch_add(1);});
    REQUIRE(plain.size()==checked.size());CHECK(observed.load()>0);
    for(size_t i=0;i<plain.size();++i) {
        CHECK(plain[i]->point==checked[i]->point);CHECK(plain[i]->type==checked[i]->type);
    }
}


TEST_CASE("Repeated support offsets preserve full geometry and independent outputs", "[sla_preparation_offsets]") {
    const auto rectangle=[](double x0,double y0,double x1,double y1) {
        return ExPolygon(Polygon({Point(scale_(x0),scale_(y0)),Point(scale_(x1),scale_(y0)),
            Point(scale_(x1),scale_(y1)),Point(scale_(x0),scale_(y1))}));
    };
    auto ring=rectangle(0,0,30,20);
    auto hole=rectangle(8,8,12,12).contour;hole.make_clockwise();ring.holes.push_back(hole);
    auto changed_hole=ring;
    changed_hole.holes.front().translate(Point(coord_t(scale_(1.)),coord_t(0)));
    const auto second=rectangle(45,0,50,5);
    // A long run exercises representative chains. Hole/contour changes and
    // reordered parts must not accidentally reuse a merely similar outline.
    std::vector<ExPolygons> input(8,ExPolygons{ring,second});
    input.push_back({changed_hole,second});
    input.push_back({changed_hole,second});
    input.push_back({second,ring});
    input.push_back({rectangle(0,0,31,20),second});
    const auto original=input;
    std::vector<float> heights;for(size_t i=0;i<input.size();++i)heights.push_back(float(i+1)*.05f);
    PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    const int workers=GENERATE(1,2,4,16);CAPTURE(workers);
    tbb::task_arena arena(workers);
    auto data=arena.execute([&]{return prepare_generator_data(std::vector<ExPolygons>(input),heights,config);});
    REQUIRE(data.layers.size()==input.size());
    for(size_t layer=0;layer<data.layers.size();++layer) {
        REQUIRE(data.layers[layer].parts.size()==input[layer].size());
        for(const auto &part:data.layers[layer].parts) {
            if(layer==0)CHECK(part.extend_shape.empty());
            else CHECK(part.extend_shape==offset_ex(*part.shape,config.removing_delta,ClipperLib::jtSquare));
        }
    }
    const auto earlier=data.layers[1].parts[0].extend_shape;
    REQUIRE_FALSE(earlier.empty());
    data.layers[2].parts[0].extend_shape.clear();
    CHECK(data.layers[1].parts[0].extend_shape==earlier);
    CHECK(data.layers[3].parts[0].extend_shape==earlier);
    CHECK(data.slices==original);CHECK(input==original);
}


TEST_CASE("Many linked parts on one layer preserve serial peninsula geometry", "[sla_preparation_part_jobs]") {
    const auto rectangle=[](double x0,double y0,double x1,double y1) {
        return ExPolygon(Polygon({Point(scale_(x0),scale_(y0)),Point(scale_(x1),scale_(y0)),
            Point(scale_(x1),scale_(y1)),Point(scale_(x0),scale_(y1))}));
    };
    ExPolygons below,above;
    for(size_t i=0;i<48;++i) {
        const double x=double(i%12)*20.,y=double(i/12)*20.;
        auto base=rectangle(x,y,x+8.,y+8.);
        auto top=rectangle(x,y,x+12.+double(i%3),y+10.);
        if(i%2==0) {
            auto hole=rectangle(x+2.,y+2.,x+4.,y+4.).contour;hole.make_clockwise();
            base.holes.push_back(hole);top.holes.push_back(hole);
        }
        below.push_back(std::move(base));above.push_back(std::move(top));
    }
    const std::vector<ExPolygons> input{below,above};
    PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    tbb::task_arena serial(1);
    auto reference=serial.execute([&]{return prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config);});
    const int workers=GENERATE(2,4,16);CAPTURE(workers);
    tbb::task_arena parallel(workers);
    auto actual=parallel.execute([&]{return prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config);});
    REQUIRE(actual.layers.size()==2);REQUIRE(actual.layers[1].parts.size()==48);
    for(size_t layer=0;layer<2;++layer) {
        REQUIRE(actual.layers[layer].parts.size()==reference.layers[layer].parts.size());
        for(size_t i=0;i<actual.layers[layer].parts.size();++i) {
            const auto &a=actual.layers[layer].parts[i];const auto &r=reference.layers[layer].parts[i];
            CHECK(*a.shape==*r.shape);CHECK(a.samples==r.samples);CHECK(a.extend_shape==r.extend_shape);
            const auto check_links=[](const PartLinks &left,const PartLinks &right) {
                REQUIRE(left.size()==right.size());
                for(size_t j=0;j<left.size();++j)CHECK(*left[j]->shape==*right[j]->shape);
            };
            check_links(a.prev_parts,r.prev_parts);check_links(a.next_parts,r.next_parts);
            REQUIRE(a.peninsulas.size()==r.peninsulas.size());
            for(size_t j=0;j<a.peninsulas.size();++j) {
                CHECK(a.peninsulas[j].unsuported_area==r.peninsulas[j].unsuported_area);
                CHECK(a.peninsulas[j].is_outline==r.peninsulas[j].is_outline);
            }
            if(layer==1) {CHECK_FALSE(a.prev_parts.empty());CHECK_FALSE(a.peninsulas.empty());}
        }
    }
    CHECK(actual.slices==input);CHECK(reference.slices==input);
}


TEST_CASE("Island clipping preserves exact geometry at scheduler boundaries", "[sla_island_cells_boundaries]") {
    const auto count=GENERATE(size_t(15),size_t(16),size_t(17),size_t(31),size_t(32),size_t(128),size_t(129));
    const auto workers=GENERATE(size_t(1),size_t(2),size_t(4),size_t(16));
    const auto rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD);
    CAPTURE(count,workers,rounding);
    const auto rectangle=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)});
    };
    ExPolygon island(rectangle(0,0,40000000,30000000));
    auto hole=rectangle(7000000,4000000,13000000,23000000);
    hole.make_clockwise();island.holes.push_back(hole);
    Polygons cells;
    std::vector<uint8_t> enabled(count,1);
    for(size_t i=0;i<count;++i) {
        const coord_t x=coord_t(i%7)*5000000-2000000;
        const coord_t y=coord_t(i%5)*6000000-1000000;
        if(i%3==0)cells.push_back(Polygon({Point(x,y),Point(x+9000001,y+1000003),
            Point(x+6500003,y+4000001),Point(x+9000001,y+8000003),
            Point(x,y+7000001),Point(x+2000001,y+3500003)}));
        else cells.push_back(rectangle(x,y,x+10000001,y+9000003));
        if(i%7==0)enabled[i]=0;
    }
    cells.back()=Polygon({Point(0,0),Point(1,1)});
    const auto original_cells=cells;const auto original_island=island;
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment(){std::fegetenv(&state);}
        ~RestoreEnvironment(){std::fesetenv(&state);}
    } restore;
    REQUIRE(std::fesetround(rounding)==0);
    std::vector<Polygons> expected(count);
    for(size_t i=0;i<count;++i)
        if(enabled[i] && cells[i].size()>=3)expected[i]=intersection(cells[i],island);
    tbb::global_control worker_limit(tbb::global_control::max_allowed_parallelism,workers);
    tbb::task_arena arena(static_cast<int>(workers));
    const auto actual=arena.execute([&]{return intersect_island_cells(cells,island,enabled);});
    CHECK(actual==expected);
    CHECK(std::fegetround()==rounding);
    CHECK(cells==original_cells);CHECK(island==original_island);
    std::atomic<unsigned> checkpoints{0};
    struct BoundaryCancelled {};
    CHECK_THROWS_AS(arena.execute([&]{return intersect_island_cells(cells,island,enabled,[&]{
        if(checkpoints.fetch_add(1)>=3)throw BoundaryCancelled{};
    });}),BoundaryCancelled);
    CHECK(checkpoints.load()>=4);
    CHECK(std::fegetround()==rounding);
    CHECK(cells==original_cells);CHECK(island==original_island);
}


TEST_CASE("Diagnostic clipping matches the exact reference and cancellation contract", "[.][sla_island_clip_profile]") {
    const auto count=GENERATE(size_t(15),size_t(16),size_t(17),size_t(31),size_t(32),size_t(128),size_t(129));
    const auto workers=GENERATE(size_t(1),size_t(2),size_t(4),size_t(16));
    const auto rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD);
    CAPTURE(count,workers,rounding);
    const auto rectangle=[](coord_t x,coord_t y,coord_t w,coord_t h) {
        return Polygon({Point(x,y),Point(x+w,y),Point(x+w,y+h),Point(x,y+h)});
    };
    ExPolygon island(rectangle(0,0,40000000,30000000));
    auto hole=rectangle(7000000,4000000,6000000,19000000);hole.make_clockwise();island.holes.push_back(hole);
    Polygons cells;
    std::vector<uint8_t> active(count,1);
    for(size_t i=0;i<count;++i) {
        cells.push_back(rectangle(coord_t(i%7)*5000000-2000000,coord_t(i%5)*6000000-1000000,10000001,9000003));
        if(i%7==0)active[i]=0;
    }
    cells.back()=Polygon({Point(0,0),Point(1,1)});
    const auto original_cells=cells;const auto original_island=island;
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment(){std::fegetenv(&state);}
        ~RestoreEnvironment(){std::fesetenv(&state);}
    } restore;
    REQUIRE(std::fesetround(rounding)==0);
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism,workers);
    tbb::task_arena arena(static_cast<int>(workers));
    const auto expected=arena.execute([&]{return intersect_island_cells(cells,island,active);});
    std::vector<IslandCellClipProfile> profile;
    const auto actual=arena.execute([&]{return intersect_island_cells(cells,island,active,{},&profile);});
    CHECK(actual==expected);REQUIRE(profile.size()==cells.size());
    for(size_t i=0;i<count;++i) {
        CHECK(profile[i].completed==bool(active[i] && cells[i].size()>=3));
        if(profile[i].completed) {
            CHECK(profile[i].subject_vertices==cells[i].points.size());
            CHECK(profile[i].island_vertices==8);
        }
    }
    CHECK(std::fegetround()==rounding);CHECK(cells==original_cells);CHECK(island==original_island);
    struct ClipProfileCancelled {};
    CHECK_THROWS_AS(intersect_island_cells(cells,island,active,[]{throw ClipProfileCancelled{};},&profile),ClipProfileCancelled);
    std::atomic<unsigned> checks{0};
    CHECK_THROWS_AS(arena.execute([&]{return intersect_island_cells(cells,island,active,[&]{
        if(checks.fetch_add(1)>=3)throw ClipProfileCancelled{};
    },&profile);}),ClipProfileCancelled);
    CHECK(checks.load()>=4);CHECK(std::fegetround()==rounding);
    CHECK(cells==original_cells);CHECK(island==original_island);
    CHECK_THROWS(intersect_island_cells(cells,island,{}, {},&profile));
}


TEST_CASE("Clipping uses the selected arena contract and exact geometry", "[sla_island_arena_experiment]") {
    const auto count=GENERATE(size_t(128),size_t(129));
    const unsigned target=static_cast<unsigned>(island_cell_clip_arena_concurrency());
    CAPTURE(count,target);
    const char *requested=std::getenv("SLA_EXPERIMENT_ISLAND_CLIP_WORKERS");
    if(requested != nullptr && std::strcmp(requested,"8")==0)CHECK(target==8);
    else if(requested != nullptr && std::strcmp(requested,"16")==0)CHECK(target==16);
    else {
        CHECK(target>=1);CHECK(target<=16);
        CHECK(target<=static_cast<unsigned>(std::max(1,tbb::info::default_concurrency())));
    }
    // Permit a larger global limit so the local arena's configuration is
    // checked directly, without asserting how many workers CI schedules.
    tbb::global_control limit(tbb::global_control::max_allowed_parallelism,32);
    const auto rectangle=[](coord_t x,coord_t y,coord_t w,coord_t h) {
        return Polygon({Point(x,y),Point(x+w,y),Point(x+w,y+h),Point(x,y+h)});
    };
    ExPolygon island(rectangle(0,0,40000000,30000000));
    auto hole=rectangle(7000000,4000000,6000000,19000000);hole.make_clockwise();island.holes.push_back(hole);
    Polygons cells;
    for(size_t i=0;i<count;++i)
        cells.push_back(rectangle(coord_t(i%7)*5000000-2000000,coord_t(i%5)*6000000-1000000,10000001,9000003));
    const auto original_cells=cells;const auto original_island=island;
    const std::vector<uint8_t> active(count,1);
    std::atomic<unsigned> callbacks{0},inside_callbacks{0};
    std::atomic<bool> wrong_arena{false};
    const auto probe=[&] {
        // The helper has one cancellation check before entering its arena.
        if(callbacks.fetch_add(1)==0)return;
        inside_callbacks.fetch_add(1);
        if(tbb::this_task_arena::max_concurrency()!=static_cast<int>(target))
            wrong_arena.store(true);
    };
    const auto actual=intersect_island_cells(cells,island,active,probe);
    CHECK_FALSE(wrong_arena.load());CHECK(inside_callbacks.load()==count);
    REQUIRE(actual.size()==cells.size());
    for(size_t i=0;i<count;++i)CHECK(actual[i]==intersection(cells[i],island));
    CHECK(cells==original_cells);CHECK(island==original_island);
    struct ArenaExperimentCancelled {};
    CHECK_THROWS_AS(intersect_island_cells(cells,island,active,[]{throw ArenaExperimentCancelled{};}),ArenaExperimentCancelled);
    std::atomic<unsigned> checks{0};
    CHECK_THROWS_AS(intersect_island_cells(cells,island,active,[&]{
        if(checks.fetch_add(1)>=5)throw ArenaExperimentCancelled{};
    }),ArenaExperimentCancelled);
    CHECK(checks.load()>=6);CHECK(cells==original_cells);CHECK(island==original_island);
}

TEST_CASE("Repeated ordered peninsula offsets compute once after successful reads", "[sla_peninsula_minimum_reuse]") {
    ExPolygon a(Polygon({Point(0,0),Point(10000,0),Point(10000,10000),Point(0,10000)}));
    ExPolygon b=a; b.translate(Point(20000,0));
    using namespace Slic3r::sla::detail;
    const std::vector<PeninsulaPreviousShapes> keys{{&a,&b},{&a,&b},{&b,&a},{&a,&a},{&a},{&a,&b}};
    PeninsulaMinimumOffsetReuse plan(keys,[]{});
    REQUIRE(plan.slot(0)!=nullptr);REQUIRE(plan.slot(1)!=nullptr);REQUIRE(plan.slot(5)!=nullptr);
    CHECK(plan.slot(0)==plan.slot(1));CHECK(plan.slot(0)==plan.slot(5));
    CHECK(plan.slot(2)==nullptr);CHECK(plan.slot(3)==nullptr);CHECK(plan.slot(4)==nullptr);
    std::atomic<size_t> calls{0};
    const int workers=GENERATE(1,2,4,16);CAPTURE(workers);
    tbb::task_arena arena(workers);
    const std::vector<size_t> requests{0,1,5};
    std::vector<const ExPolygons*> results(3,nullptr);
    arena.execute([&]{tbb::parallel_for(size_t(0),requests.size(),[&](size_t i){
        results[i]=&plan.slot(requests[i])->get([&]{calls.fetch_add(1);return ExPolygons{a,b};},[]{});
    });});
    CHECK(calls.load()==1);CHECK(plan.computed_after_join()==1);CHECK(plan.reused_after_join()==2);
    for(const auto *result:results){REQUIRE(result!=nullptr);CHECK(*result==ExPolygons{a,b});}
    CHECK(results[0]==results[1]);CHECK(results[0]==results[2]);
}

TEST_CASE("Lazy peninsula offset failure publishes no partial geometry and remains retryable", "[sla_peninsula_minimum_reuse]") {
    using namespace Slic3r::sla::detail;
    ExPolygon a(Polygon({Point(0,0),Point(10000,0),Point(10000,10000),Point(0,10000)}));
    const auto original=a;
    PeninsulaMinimumOffsetReuse plan({{&a},{&a}},[]{});
    struct Cancelled{};size_t calls=0;
    CHECK_THROWS_AS(plan.slot(0)->get([&]()->ExPolygons{++calls;throw Cancelled{};},[]{}),Cancelled);
    CHECK(plan.computed_after_join()==0);CHECK(plan.reused_after_join()==0);
    const auto &completed=plan.slot(1)->get([&]{++calls;return ExPolygons{a};},[]{});
    CHECK(completed==ExPolygons{a});CHECK(calls==2);CHECK(plan.computed_after_join()==1);
    CHECK_THROWS_AS(plan.slot(0)->get([&]{++calls;return ExPolygons{};},[]{throw Cancelled{};}),Cancelled);
    CHECK(calls==2);CHECK(a==original);
}

TEST_CASE("Actual preparation reuse preserves ordered supports and cancellation", "[sla_peninsula_minimum_reuse_api]") {
    struct RestoreEnv {
        bool had;std::string old;
        RestoreEnv():had(std::getenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE")!=nullptr),
            old(had?std::getenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE"):""){}
        static void set(const char *value){
#ifdef _WIN32
            _putenv_s("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE",value?value:"");
#else
            if(value)setenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE",value,1);
            else unsetenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE");
#endif
        }
        ~RestoreEnv(){set(had?old.c_str():nullptr);}
    } restore;
    const auto rectangle=[](double x0,double y0,double x1,double y1){
        return ExPolygon(Polygon({Point(scale_(x0),scale_(y0)),Point(scale_(x1),scale_(y0)),
            Point(scale_(x1),scale_(y1)),Point(scale_(x0),scale_(y1))}));
    };
    ExPolygons above;
    for(size_t i=0;i<6;++i)above.push_back(rectangle(3.*i,6.,3.*i+1.,8.5));
    // All six share the same previous native shape, but current masks differ.
    // A seventh covered job must never force an unnecessary minimum offset.
    above.push_back(rectangle(19.,1.,20.,2.));
    const std::vector<ExPolygons> input{{rectangle(0,0,21,8)},above};
    const auto original=input;PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    RestoreEnv::set("0");
    auto reference=prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config);
    SupportPointGeneratorConfig generator;
    const auto reference_supports=generate_support_points(reference,generator);
    const int workers=GENERATE(1,2,4,16);CAPTURE(workers);
    RestoreEnv::set("1");tbb::task_arena arena(workers);
    auto actual=arena.execute([&]{return prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config);});
    REQUIRE(actual.layers.size()==reference.layers.size());
    for(size_t layer=0;layer<actual.layers.size();++layer){
        REQUIRE(actual.layers[layer].parts.size()==reference.layers[layer].parts.size());
        for(size_t i=0;i<actual.layers[layer].parts.size();++i){
            const auto &a=actual.layers[layer].parts[i];const auto &b=reference.layers[layer].parts[i];
            CHECK(*a.shape==*b.shape);CHECK(a.samples==b.samples);CHECK(a.extend_shape==b.extend_shape);
            REQUIRE(a.prev_parts.size()==b.prev_parts.size());REQUIRE(a.next_parts.size()==b.next_parts.size());
            for(size_t j=0;j<a.prev_parts.size();++j)CHECK(*a.prev_parts[j]->shape==*b.prev_parts[j]->shape);
            for(size_t j=0;j<a.next_parts.size();++j)CHECK(*a.next_parts[j]->shape==*b.next_parts[j]->shape);
            REQUIRE(a.peninsulas.size()==b.peninsulas.size());
            for(size_t j=0;j<a.peninsulas.size();++j){CHECK(a.peninsulas[j].unsuported_area==b.peninsulas[j].unsuported_area);CHECK(a.peninsulas[j].is_outline==b.peninsulas[j].is_outline);}
        }
    }
    const auto supports=generate_support_points(actual,generator);
    REQUIRE(supports.size()==reference_supports.size());REQUIRE_FALSE(supports.empty());
    for(size_t i=0;i<supports.size();++i){
        CHECK(std::memcmp(supports[i].pos.data(),reference_supports[i].pos.data(),3*sizeof(float))==0);
        CHECK(std::memcmp(&supports[i].head_front_radius,&reference_supports[i].head_front_radius,sizeof(float))==0);
        CHECK(supports[i].type==reference_supports[i].type);
    }
    struct Cancelled{};std::atomic<size_t> checks{0};
    CHECK_THROWS_AS(prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config,[&]{if(checks.fetch_add(1)>=40)throw Cancelled{};}),Cancelled);
    CHECK(checks.load()>=41);CHECK(input==original);CHECK(actual.slices==original);
}

TEST_CASE("Cancellation after offset generation leaves the lazy slot retryable", "[sla_peninsula_minimum_reuse]") {
    using namespace Slic3r::sla::detail;
    ExPolygon a(Polygon({Point(0,0),Point(10000,0),Point(10000,10000),Point(0,10000)}));
    const auto original=a;PeninsulaMinimumOffsetReuse plan({{&a},{&a}},[]{});
    struct Cancelled{};size_t generated=0,checks=0;
    // First callback is before call_once, second is after a full temporary
    // offset was built but before that geometry may become shared.
    CHECK_THROWS_AS(plan.slot(0)->get([&]{++generated;return ExPolygons{a};},[&]{
        if(++checks==2)throw Cancelled{};
    }),Cancelled);
    CHECK(generated==1);CHECK(plan.computed_after_join()==0);CHECK(plan.reused_after_join()==0);
    const auto &retried=plan.slot(1)->get([&]{++generated;return ExPolygons{a};},[]{});
    CHECK(generated==2);CHECK(retried==ExPolygons{original});CHECK(plan.computed_after_join()==1);
    const auto &shared=plan.slot(0)->get([&]{++generated;return ExPolygons{};},[]{});
    CHECK(&shared==&retried);CHECK(generated==2);CHECK(plan.reused_after_join()==1);CHECK(a==original);
}

TEST_CASE("Two previous masks with holes preserve exact union-derived preparation and supports", "[sla_peninsula_minimum_reuse_union]") {
    struct RestoreReuseEnv {
        bool had;std::string old;
        RestoreReuseEnv():had(std::getenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE")!=nullptr),
            old(had?std::getenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE"):""){}
        static void set(const char *value){
#ifdef _WIN32
            _putenv_s("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE",value?value:"");
#else
            if(value)setenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE",value,1);
            else unsetenv("SLA_EXPERIMENT_PENINSULA_OFFSET_REUSE");
#endif
        }
        ~RestoreReuseEnv(){set(had?old.c_str():nullptr);}
    } restore;
    const auto rectangle=[](double x0,double y0,double x1,double y1){
        return ExPolygon(Polygon({Point(scale_(x0),scale_(y0)),Point(scale_(x1),scale_(y0)),
            Point(scale_(x1),scale_(y1)),Point(scale_(x0),scale_(y1))}));
    };
    auto left=rectangle(0,0,5,8),right=rectangle(5.5,0,11,8);
    auto left_hole=rectangle(1,2.5,2,4.5).contour;left_hole.make_clockwise();
    auto right_hole=rectangle(8,4.5,9,6.5).contour;right_hole.make_clockwise();
    left.holes.push_back(left_hole);right.holes.push_back(right_hole);
    ExPolygons above;
    for(size_t i=0;i<3;++i){
        const double y=1.+2.*i;
        auto bar=rectangle(0,y,11,y+1.);
        auto hole=rectangle(3,y+.2,3.5,y+.8).contour;hole.make_clockwise();bar.holes.push_back(hole);
        above.push_back(std::move(bar));
    }
    const std::vector<ExPolygons> input{{left,right},above};const auto original=input;
    PrepareSupportConfig config;config.minimal_bounding_sphere_radius=0;
    RestoreReuseEnv::set("0");
    auto expected=prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config);
    REQUIRE(expected.layers.at(1).parts.size()==3);
    for(const auto &part:expected.layers.at(1).parts){
        // This must really exercise a repeated union of TWO linked native
        // shapes and nonempty holes, not separate single-mask trivial cases.
        REQUIRE(part.prev_parts.size()==2);CHECK_FALSE(part.shape->holes.empty());
        CHECK_FALSE(part.prev_parts[0]->shape->holes.empty());CHECK_FALSE(part.prev_parts[1]->shape->holes.empty());
    }
    SupportPointGeneratorConfig generator;const auto expected_supports=generate_support_points(expected,generator);
    const int workers=GENERATE(1,2,4,16);CAPTURE(workers);
    RestoreReuseEnv::set("1");tbb::task_arena arena(workers);
    auto actual=arena.execute([&]{return prepare_generator_data(std::vector<ExPolygons>(input),{.05f,.1f},config);});
    REQUIRE(actual.layers.size()==expected.layers.size());
    for(size_t layer=0;layer<actual.layers.size();++layer){
        REQUIRE(actual.layers[layer].parts.size()==expected.layers[layer].parts.size());
        for(size_t i=0;i<actual.layers[layer].parts.size();++i){
            const auto &a=actual.layers[layer].parts[i];const auto &b=expected.layers[layer].parts[i];
            CHECK(*a.shape==*b.shape);CHECK(a.samples==b.samples);CHECK(a.extend_shape==b.extend_shape);
            REQUIRE(a.prev_parts.size()==b.prev_parts.size());REQUIRE(a.next_parts.size()==b.next_parts.size());
            for(size_t j=0;j<a.prev_parts.size();++j)CHECK(*a.prev_parts[j]->shape==*b.prev_parts[j]->shape);
            for(size_t j=0;j<a.next_parts.size();++j)CHECK(*a.next_parts[j]->shape==*b.next_parts[j]->shape);
            REQUIRE(a.peninsulas.size()==b.peninsulas.size());
            for(size_t j=0;j<a.peninsulas.size();++j){CHECK(a.peninsulas[j].unsuported_area==b.peninsulas[j].unsuported_area);CHECK(a.peninsulas[j].is_outline==b.peninsulas[j].is_outline);}
        }
    }
    const auto actual_supports=generate_support_points(actual,generator);
    REQUIRE(actual_supports.size()==expected_supports.size());REQUIRE_FALSE(actual_supports.empty());
    for(size_t i=0;i<actual_supports.size();++i){
        CHECK(std::memcmp(actual_supports[i].pos.data(),expected_supports[i].pos.data(),3*sizeof(float))==0);
        CHECK(std::memcmp(&actual_supports[i].head_front_radius,&expected_supports[i].head_front_radius,sizeof(float))==0);
        CHECK(actual_supports[i].type==expected_supports[i].type);
    }
    CHECK(input==original);CHECK(actual.slices==original);
}

TEST_CASE("Spatial clipping diagnostics distinguish excluded and touching holes without changing geometry", "[sla_island_clip_spatial_profile]") {
    const auto rectangle=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)});
    };
    const bool reverse_holes=GENERATE(false,true);
    const auto rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO);
    CAPTURE(reverse_holes,rounding);
    ExPolygon island(rectangle(0,0,20000,20000));
    for(auto hole:{rectangle(2000,2000,4000,4000),rectangle(12000,12000,14000,14000)}) {
        if(!reverse_holes)hole.make_clockwise();
        island.holes.push_back(std::move(hole));
    }
    struct Example { Polygon cell; size_t excluded; const char *reason; };
    const std::vector<Example> examples{
        {rectangle(-4000,-4000,-2000,-2000),2,"outside both holes"},
        {rectangle(2500,2500,3500,3500),1,"inside first hole"},
        {rectangle(4000,2000,6000,4000),1,"touching first hole edge"},
        {rectangle(4000,4000,6000,6000),1,"touching first hole vertex"},
        {rectangle(1000,1000,15000,15000),0,"encloses both holes"},
        {rectangle(6000,6000,8000,8000),2,"interior away from holes"},
    };
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment(){std::fegetenv(&state);}
        ~RestoreEnvironment(){std::fesetenv(&state);}
    } restore;
    REQUIRE(std::fesetround(rounding)==0);
    const auto original=island;
    IslandCellClipProfile profile;
    for(const auto &example:examples) {
        CAPTURE(example.reason);
        const auto expected=intersection(example.cell,island);
        // Reuse the same diagnostic structure to detect accumulated counters.
        const auto actual=profile_island_cell_clip(example.cell,island,profile);
        CHECK(actual==expected);
        CHECK(profile.completed);
        CHECK(profile.subject_vertices==4);
        CHECK(profile.contour_vertices==4);
        CHECK(profile.hole_vertices==8);
        CHECK(profile.island_vertices==12);
        CHECK(profile.disjoint_holes==example.excluded);
        CHECK(profile.disjoint_hole_vertices==4*example.excluded);
        CHECK(std::fegetround()==rounding);
        CHECK(island==original);
    }
    // Undefined bounds must not be counted as a proven exclusion.
    ExPolygon degenerate(rectangle(0,0,20000,20000));
    degenerate.holes.push_back(Polygon({Point(30000,30000),Point(31000,30000)}));
    const Polygon cell=rectangle(6000,6000,8000,8000);
    CHECK(profile_island_cell_clip(cell,degenerate,profile)==intersection(cell,degenerate));
    CHECK(profile.hole_vertices==2);
    CHECK(profile.disjoint_holes==0);
    CHECK(profile.disjoint_hole_vertices==0);
}

TEST_CASE("Existing bbox clipping kernel preserves exact island-cell polygon vectors", "[sla_island_bbox_clip]") {
    const bool comb=GENERATE(false,true);
    const bool reverse_holes=GENERATE(false,true);
    const bool reverse_outer=GENERATE(false,true);
    const auto rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO);
    CAPTURE(comb,reverse_holes,reverse_outer,rounding);
    const auto rectangle=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)});
    };
    ExPolygon island(rectangle(0,0,40000,40000));
    if(comb) {
        Points outer;
        for(int i=0;i<=80;++i)outer.emplace_back(i*500,(i%2)*1000);
        outer.emplace_back(40000,40000);
        for(int i=79;i>=0;--i)outer.emplace_back(i*500,40000-(i%2)*1000);
        island.contour=Polygon(std::move(outer));
    }
    if(reverse_outer)island.contour.reverse();
    for(auto hole:{rectangle(2000,3000,4000,5000),rectangle(20000,20000,22000,22000),
                   Polygon({Point(10000,10000),Point(14000,10000),Point(14000,12000),
                            Point(12000,12000),Point(12000,14000),Point(10000,14000)})}) {
        if(!reverse_holes)hole.make_clockwise();
        island.holes.push_back(std::move(hole));
    }
    const Polygons cells{
        rectangle(-4000,-4000,-2000,-2000), // outside the island
        rectangle(2500,3500,3500,4500),     // enclosed by a hole
        rectangle(4000,3000,6000,5000),     // edge touch
        rectangle(4000,5000,6000,6000),     // vertex touch
        rectangle(1000,1000,23000,23000),   // encloses multiple holes
        rectangle(16000,16000,18000,18000), // many distant outer vertices
        rectangle(-1000,-1000,41000,1100),  // crosses the outer comb
        rectangle(10500,10500,14500,14500), // crosses a concave hole
        Polygon({Point(1000,1000),Point(16000,1000),Point(16000,5000),
                 Point(10000,5000),Point(10000,16000),Point(1000,16000)}),
        Polygon({Point(0,0),Point(1,1)}),   // degenerate fallback
    };
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment(){std::fegetenv(&state);}
        ~RestoreEnvironment(){std::fesetenv(&state);}
    } restore;
    REQUIRE(std::fesetround(rounding)==0);
    for(int order=0;order<2;++order) {
        CAPTURE(order);
        const auto original=island;
        for(const auto &cell:cells)CHECK(intersect_island_cell_bbox(cell,island)==intersection(cell,island));
        CHECK(std::fegetround()==rounding);CHECK(island==original);
        std::reverse(island.holes.begin(),island.holes.end());
        std::rotate(island.contour.points.begin(),island.contour.points.begin()+1,island.contour.points.end());
    }
}

TEST_CASE("BBox island clipping keeps cancellation local and handles extreme bounds", "[sla_island_bbox_clip]") {
    const auto rectangle=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)});
    };
    ExPolygon island(rectangle(0,0,40000,40000));
    auto hole=rectangle(2000,2000,4000,4000);hole.make_clockwise();island.holes.push_back(hole);
    const auto original=island;
    const Polygon cell=rectangle(10000,10000,12000,12000);
    struct BboxCancelled {};
    const Polygons sentinel{rectangle(1,1,2,2)};
    for(size_t stop:{size_t(0),size_t(1),size_t(2)}) {
        CAPTURE(stop);size_t checks=0;auto output=sentinel;
        CHECK_THROWS_AS(output=intersect_island_cell_bbox(cell,island,[&]{if(checks++==stop)throw BboxCancelled{};}),BboxCancelled);
        CHECK(checks==stop+1);CHECK(output==sentinel);CHECK(island==original);
        CHECK(intersect_island_cell_bbox(cell,island)==intersection(cell,island));
    }
    const auto lo=std::numeric_limits<coord_t>::min();
    const auto hi=std::numeric_limits<coord_t>::max();
    for(const auto &subject:{rectangle(lo,lo,lo+50,lo+50),rectangle(hi-50,hi-50,hi,hi)}) {
        // Inflation would overflow: the helper must take the full operation.
        ExPolygon local(subject);
        CHECK(intersect_island_cell_bbox(subject,local)==intersection(subject,local));
    }
    ExPolygon degenerate(rectangle(0,0,40000,40000));
    degenerate.holes.push_back(Polygon{});
    degenerate.holes.push_back(Polygon({Point(30000,30000),Point(31000,30000)}));
    CHECK(intersect_island_cell_bbox(cell,degenerate)==intersection(cell,degenerate));
}


TEST_CASE("Detailed clipping requests cannot silently use a different kernel", "[sla_island_clip_profile_policy]") {
    // Explicit controls are checked independently of the selector. Unset uses
    // the real production default, allowing the same contract before/after a
    // default change without manufacturing an expected failure.
    const char *requested=std::getenv("SLA_EXPERIMENT_ISLAND_BBOX_CLIP");
    const bool bbox=island_cell_bbox_clip_enabled();
    if(requested && std::strcmp(requested,"0")==0)REQUIRE_FALSE(bbox);
    if(requested && std::strcmp(requested,"1")==0)REQUIRE(bbox);
    const auto rectangle=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)});
    };
    ExPolygon island(rectangle(0,0,20000,20000));
    auto hole=rectangle(2000,2000,4000,4000);hole.make_clockwise();
    island.holes.push_back(hole);
    const Polygons cells{rectangle(1000,1000,6000,6000)};
    const auto original_island=island;
    const auto original_cells=cells;
    std::vector<IslandCellClipProfile> profile(2);
    profile[0].completed=true;profile[0].subject_vertices=91;
    profile[1].completed=true;profile[1].execute_ns=123;
    const std::vector<Polygons> sentinel{Polygons{rectangle(-10,-10,-5,-5)}};
    auto output=sentinel;
    size_t checkpoints=0;
    const auto request=[&] {
        output=intersect_island_cells(cells,island,{1},[&]{++checkpoints;},&profile);
    };
    if(bbox) {
        bool rejected=false;
        try {request();}
        catch(const RuntimeError &error) {
            rejected=true;
            CHECK(std::string(error.what())==
                "Island bbox clipping cannot be combined with detailed clip profiling");
        }
        REQUIRE(rejected);
        CHECK(checkpoints==1); // Coordinator check; no cell task was started.
        CHECK(output==sentinel);
        REQUIRE(profile.size()==2);
        CHECK(profile[0].completed);CHECK(profile[0].subject_vertices==91);
        CHECK(profile[1].completed);CHECK(profile[1].execute_ns==123);
    } else {
        request();
        CHECK(checkpoints==2); // Coordinator and the unchanged one-cell batch.
        CHECK(output==std::vector<Polygons>{intersection(cells.front(),island)});
        REQUIRE(profile.size()==1);CHECK(profile[0].completed);
        CHECK(profile[0].subject_vertices==cells.front().points.size());
        CHECK(profile[0].island_vertices==8);
    }
    CHECK(island==original_island);CHECK(cells==original_cells);
}

namespace {
std::vector<ExPolygons> support_link_literal_slices()
{
    const auto rect=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return ExPolygon(Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)}));
    };
    auto donut=rect(0,0,10000,10000);
    auto hole=rect(3000,3000,7000,7000).contour;hole.make_clockwise();
    donut.holes.push_back(hole);
    // Equal shapes remain separate parts; matching only their geometry would
    // hide an incorrect link to the other owner/index.
    const auto repeated=rect(20000,0,26000,6000);
    return {{donut,repeated,repeated},
            {rect(4000,4000,6000,6000),rect(1000,1000,2000,2000),
             rect(10000,0,12000,2000),rect(21000,1000,25000,5000),
             rect(40000,40000,42000,42000)}};
}

void check_support_link_literal_graph(const SupportPointGeneratorData &data)
{
    REQUIRE(data.layers.size()==2);
    REQUIRE(data.layers[0].parts.size()==3);REQUIRE(data.layers[1].parts.size()==5);
    const auto indices=[](const PartLinks &links,const LayerParts &owners) {
        std::vector<size_t> result;
        for(const auto link:links)result.push_back(size_t(link-owners.cbegin()));
        return result;
    };
    const std::vector<std::vector<size_t>> expected_previous{{},{0},{},{1,2},{}};
    const std::vector<std::vector<size_t>> expected_next{{1},{3},{3}};
    for(size_t i=0;i<5;++i) {
        CHECK(indices(data.layers[1].parts[i].prev_parts,data.layers[0].parts)==expected_previous[i]);
        CHECK(data.layers[1].parts[i].next_parts.empty());
        CHECK(data.layers[1].parts[i].samples.empty());
        CHECK(data.layers[1].parts[i].peninsulas.empty());
    }
    for(size_t i=0;i<3;++i) {
        CHECK(indices(data.layers[0].parts[i].next_parts,data.layers[1].parts)==expected_next[i]);
        CHECK(data.layers[0].parts[i].prev_parts.empty());
    }
    CHECK(data.slices==support_link_literal_slices());
}

SupportPointGeneratorData run_support_link_literal_fixture(const ThrowOnCancel &cancel=[] {})
{
    PrepareSupportConfig config;
    config.minimal_bounding_sphere_radius=0;
    config.removing_delta=0;
    return prepare_generator_data(support_link_literal_slices(),{.05f,.1f},config,cancel);
}
}

TEST_CASE("Support links preserve literal owner indices, rounding and cancellation", "[sla_support_link_graph]") {
    const int workers=GENERATE(1,2,4,16);
    const int rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO);
    CAPTURE(workers,rounding);
    struct RestoreEnvironment {
        std::fenv_t saved;
        RestoreEnvironment(){std::fegetenv(&saved);}
        ~RestoreEnvironment(){std::fesetenv(&saved);}
    } restore;
    REQUIRE(std::fesetround(rounding)==0);
    tbb::task_arena arena(workers);
    const auto result=arena.execute([]{return run_support_link_literal_fixture();});
    check_support_link_literal_graph(result);
    CHECK(std::fegetround()==rounding);
    // Fixed two-layer serial fixture: prepare entry + two bounds jobs + bounds
    // phase checkpoint + link-layer entry = five. Checks6/7 bracket the first
    // real intersection; check8 precedes the first disjoint-bbox rejection.
    tbb::task_arena serial(1);
    struct LinkCancelled {};
    for(size_t stop:{size_t(1),size_t(6),size_t(7),size_t(8)}) {
        CAPTURE(stop);std::atomic<size_t> checks{0};
        CHECK_THROWS_AS(serial.execute([&]{return run_support_link_literal_fixture([&] {
            if(checks.fetch_add(1)+1==stop)throw LinkCancelled{};
        });}),LinkCancelled);
        CHECK(checks.load()==stop);
        CHECK(std::fegetround()==rounding);
    }
    check_support_link_literal_graph(serial.execute([]{return run_support_link_literal_fixture();}));
}


TEST_CASE("Support-link graph distinguishes hole boundaries and duplicate owners",
          "[sla_support_link_graph][sla_support_link_graph_boundaries]") {
    const int workers=GENERATE(1,2,4,16);
    const int rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO);
    const size_t ring_start=GENERATE(size_t(0),size_t(1));
    CAPTURE(workers,rounding,ring_start);
    const auto rect=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return ExPolygon(Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)}));
    };
    auto donut=rect(0,0,10000,10000);
    auto hole=rect(3000,3000,7000,7000).contour;hole.make_clockwise();
    donut.holes.push_back(hole);
    const auto repeated=rect(20000,0,26000,6000);
    std::vector<ExPolygons> slices{{donut,repeated,repeated}, {
        rect(3000,4000,5000,6000), // Hole interior touching its left edge: no area.
        rect(3000,3000,5000,5000), // Hole interior touching two edges and a vertex.
        rect(3000,3000,7000,7000), // Exact hole boundary: still no filled intersection.
        rect(2000,4000,3000,6000), // Filled side touching the hole: positive area.
        rect(2000,4000,4000,6000), // Crosses the hole boundary: positive area.
        rect(10000,10000,12000,12000), // Outer vertex-only contact: no area.
        rect(10000,2000,12000,4000), // Outer edge-only contact: no area.
        rect(11000,11000,12000,12000), // Strictly disjoint bounds.
        rect(21000,1000,25000,5000), // Links to both equal-shaped distinct owners.
        repeated, // Exact equal outer rings also link to both owners.
        donut // Exact equal outer AND hole rings link only to owner0.
    }};
    if(ring_start) for(auto &layer:slices) for(auto &shape:layer) {
        std::rotate(shape.contour.points.begin(),shape.contour.points.begin()+1,shape.contour.points.end());
        for(auto &ring:shape.holes)
            std::rotate(ring.points.begin(),ring.points.begin()+1,ring.points.end());
    }
    const auto original=slices;
    const std::vector<std::vector<size_t>> previous{{},{},{},{0},{0},{},{},{},{1,2},{1,2},{0}};
    const std::vector<std::vector<size_t>> next{{3,4,10},{8,9},{8,9}};
    struct RestoreEnvironment {
        std::fenv_t saved;
        RestoreEnvironment(){std::fegetenv(&saved);}
        ~RestoreEnvironment(){std::fesetenv(&saved);}
    } restore;
    REQUIRE(std::fesetround(rounding)==0);
    // Literal bool expectations are independent of the proposed helper. The
    // public preparation below must retain exact indices in both directions.
    for(size_t above=0;above<slices[1].size();++above)
        for(size_t below=0;below<slices[0].size();++below) {
            CAPTURE(above,below);
            const bool linked=std::find(previous[above].begin(),previous[above].end(),below)!=previous[above].end();
            CHECK(!intersection(slices[1][above],slices[0][below],ApplySafetyOffset::No).empty()==linked);
        }
    PrepareSupportConfig config;
    config.minimal_bounding_sphere_radius=0;config.removing_delta=0;
    tbb::task_arena arena(workers);
    const auto result=arena.execute([&] {
        return prepare_generator_data(std::vector<ExPolygons>(slices),{.05f,.1f},config);
    });
    REQUIRE(result.layers.size()==2);
    REQUIRE(result.layers[0].parts.size()==next.size());
    REQUIRE(result.layers[1].parts.size()==previous.size());
    const auto indices=[](const PartLinks &links,const LayerParts &owners) {
        std::vector<size_t> values;
        for(const auto link:links)values.push_back(size_t(link-owners.cbegin()));
        return values;
    };
    for(size_t i=0;i<previous.size();++i) {
        CAPTURE(i);
        CHECK(indices(result.layers[1].parts[i].prev_parts,result.layers[0].parts)==previous[i]);
        CHECK(result.layers[1].parts[i].next_parts.empty());
        CHECK(*result.layers[1].parts[i].shape==original[1][i]);
    }
    for(size_t i=0;i<next.size();++i) {
        CAPTURE(i);
        CHECK(indices(result.layers[0].parts[i].next_parts,result.layers[1].parts)==next[i]);
        CHECK(result.layers[0].parts[i].prev_parts.empty());
        CHECK(*result.layers[0].parts[i].shape==original[0][i]);
    }
    CHECK(slices==original);CHECK(result.slices==original);
    CHECK(std::fegetround()==rounding);
}

// The companion Python gate verifies the literal opt-in counter line. On the
// uninstrumented production this native fixture passes while that gate fails
// for the genuinely missing diagnostic, without inventing a geometry failure.
TEST_CASE("Emit the literal support-link reduction diagnostic fixture", "[.][sla_support_link_profile_fixture]") {
    set_logging_level(4);
    tbb::task_arena serial(1);
    check_support_link_literal_graph(serial.execute([]{return run_support_link_literal_fixture();}));
}

TEST_CASE("Support-link reduction preserves literal intersection geometry and fill semantics", "[sla_support_link_reduction]") {
    const bool reverse_outer=GENERATE(false,true);
    const bool reverse_holes=GENERATE(false,true);
    const int rounding=GENERATE(FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO);
    CAPTURE(reverse_outer,reverse_holes,rounding);
    const auto rect=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)});
    };
    Points outer;
    for(int i=0;i<=80;++i)outer.emplace_back(i*500,(i%2)*1000);
    outer.emplace_back(40000,40000);
    for(int i=79;i>=0;--i)outer.emplace_back(i*500,40000-(i%2)*1000);
    ExPolygon clip(Polygon(std::move(outer)));
    for(auto hole:{rect(2000,3000,4000,5000),rect(20000,20000,22000,22000),
                  Polygon({Point(10000,10000),Point(14000,10000),Point(14000,12000),
                           Point(12000,12000),Point(12000,14000),Point(10000,14000)})}) {
        if(!reverse_holes)hole.make_clockwise();
        clip.holes.push_back(hole);
    }
    if(reverse_outer)clip.contour.reverse();
    std::vector<ExPolygon> subjects;
    for(auto polygon:{rect(-4000,-4000,-2000,-2000),rect(2500,3500,3500,4500),
                     rect(4000,3000,6000,5000),rect(4000,5000,6000,6000),
                     rect(1000,1000,23000,23000),rect(16000,16000,18000,18000),
                     rect(-1000,-1000,41000,1100),rect(10500,10500,14500,14500)})
        subjects.emplace_back(polygon);
    ExPolygon subject_with_hole(rect(0,0,40000,40000));
    auto subject_hole=rect(1000,1000,39000,39000);subject_hole.make_clockwise();
    subject_with_hole.holes.push_back(subject_hole);
    subjects.push_back(subject_with_hole);
    // Nonzero fill also sees independently wound paths beyond the outer ring.
    // The pruning bound must not discard them via get_extents(ExPolygon).
    ExPolygon outside_hole(rect(1000,1000,2000,2000));
    auto far=rect(20000,20000,24000,24000);far.make_clockwise();
    outside_hole.holes.push_back(far);subjects.push_back(outside_hole);
    struct RestoreEnvironment {
        std::fenv_t saved;
        RestoreEnvironment(){std::fegetenv(&saved);}
        ~RestoreEnvironment(){std::fesetenv(&saved);}
    } restore;
    REQUIRE(std::fesetround(rounding)==0);
    for(int order=0;order<2;++order) {
        CAPTURE(order);
        const auto original_clip=clip;
        for(size_t i=0;i<subjects.size();++i) {
            CAPTURE(i);const auto original_subject=subjects[i];
            const auto expected=intersection(subjects[i],clip,ApplySafetyOffset::No);
            const auto actual=Slic3r::sla::detail::intersect_support_link_bbox(subjects[i],clip);
            CHECK(actual.empty()==expected.empty());CHECK(actual==expected);
            CHECK(subjects[i]==original_subject);CHECK(clip==original_clip);
            CHECK(std::fegetround()==rounding);
        }
        std::reverse(clip.holes.begin(),clip.holes.end());
        std::rotate(clip.contour.points.begin(),clip.contour.points.begin()+1,clip.contour.points.end());
    }
}

TEST_CASE("Support-link diagnostic counts and unsafe bounds keep the original contract", "[sla_support_link_reduction]") {
    const auto rect=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return ExPolygon(Polygon({Point(x0,y0),Point(x1,y0),Point(x1,y1),Point(x0,y1)}));
    };
    const auto slices=support_link_literal_slices();
    Slic3r::sla::detail::SupportLinkReductionProfile total;
    for(const auto &above:slices[1])for(const auto &below:slices[0]) {
        ++total.pairs;
        if(!get_extents(above).overlap(get_extents(below))) {++total.bbox_rejected;continue;}
        const auto expected=intersection(above,below);
        total.observe(above,below,!expected.empty());
        CHECK(Slic3r::sla::detail::intersect_support_link_bbox(above,below)==expected);
    }
    CHECK(total.pairs==15);CHECK(total.bbox_rejected==10);CHECK(total.full_calls==5);
    CHECK(total.nonempty==3);CHECK(total.subject_vertices==20);CHECK(total.clip_vertices==32);
    CHECK(total.retained_clip_vertices==24);CHECK(total.reduced_pairs==2);CHECK(total.fallback_calls==0);
    const coord_t lo=std::numeric_limits<coord_t>::min(),hi=std::numeric_limits<coord_t>::max();
    for(const auto &subject:{rect(lo,lo,lo+50,lo+50),rect(hi-50,hi-50,hi,hi),
                            ExPolygon(Polygon({Point(0,0),Point(10,0)})),ExPolygon{}}) {
        const auto prepared=Slic3r::sla::detail::prepare_support_link_reduction(subject,subject);
        CHECK(prepared.fallback);
        CHECK(Slic3r::sla::detail::intersect_support_link_bbox(subject,subject)==intersection(subject,subject));
    }
    // Two independent zero-area rings must not turn into a filled link.
    ExPolygon degenerate(rect(0,0,20000,20000));
    degenerate.holes.push_back(Polygon{});
    degenerate.holes.push_back(Polygon({Point(30000,30000),Point(31000,30000)}));
    const auto subject=rect(1000,1000,2000,2000);
    CHECK(Slic3r::sla::detail::intersect_support_link_bbox(subject,degenerate)==intersection(subject,degenerate));
}

TEST_CASE("Support-link candidate rejects full-kernel-only profiling", "[.][sla_support_link_profile_policy]")
{
    const char *bbox = std::getenv("SLA_EXPERIMENT_SUPPORT_LINK_BBOX");
    const char *profile = std::getenv("SLA_PROFILE_SUPPORT_LINK_REDUCTION");
    REQUIRE(bbox != nullptr);
    REQUIRE(profile != nullptr);
    REQUIRE(std::strcmp(bbox, "1") == 0);
    REQUIRE(std::strcmp(profile, "1") == 0);

    tbb::task_arena serial(1);
    size_t checkpoints = 0;
    bool rejected = false;
    try {
        serial.execute([&] {
            return run_support_link_literal_fixture([&] { ++checkpoints; });
        });
    } catch (const RuntimeError &error) {
        rejected = true;
        CHECK(std::string(error.what()) ==
              "Support-link bbox experiment cannot be combined with full-kernel reduction profiling");
    }
    // Before the candidate is applied, ON plus profiling is not rejected.
    // Its RED must be this assertion, not a geometry mismatch or unrelated error.
    REQUIRE(rejected);
    // Entry, two bounds jobs and the bounds barrier; no clipping job begins.
    CHECK(checkpoints == 4);
}


TEST_CASE("Peninsula profiling fixture preserves literal links and completed geometry",
          "[.][sla_peninsula_profile_fixture]")
{
    struct RestoreLogging {
        unsigned saved = get_logging_level();
        ~RestoreLogging() { set_logging_level(saved); }
    } logging;
    set_logging_level(4);
    const auto rectangle = [](double x0, double y0, double x1, double y1) {
        return ExPolygon(Polygon({Point(scale_(x0), scale_(y0)),
            Point(scale_(x1), scale_(y0)), Point(scale_(x1), scale_(y1)),
            Point(scale_(x0), scale_(y1))}));
    };
    const std::vector<ExPolygons> input{
        {rectangle(0, 0, 10, 10), rectangle(30, 0, 40, 10),
         rectangle(60, 0, 70, 10), rectangle(90, 0, 110, 10)},
        {rectangle(0, 0, 10, 10), rectangle(30, 0, 41, 10),
         rectangle(60, 0, 71, 10), rectangle(90, -1, 99, 9),
         rectangle(101, 1, 110, 11)}};
    // The first job is already covered. The remaining four overhangs are
    // narrower than the 2 mm minimum offset. Last two jobs share the same
    // previous shape, preserving its owner and ordered reuse identity.
    PrepareSupportConfig config;
    config.minimal_bounding_sphere_radius = 0;
    config.peninsula_min_width = scale_(2.);
    config.peninsula_self_supported_width = scale_(1.5);
    config.removing_delta = scale_(.5);
    const auto original = input;
    const int original_rounding = std::fegetround();
    tbb::task_arena serial(1);
    const auto actual = serial.execute([&] {
        return prepare_generator_data(std::vector<ExPolygons>(input), {.05f, .1f}, config);
    });
    REQUIRE(actual.layers.size() == 2);
    REQUIRE(actual.layers[0].parts.size() == 4);
    REQUIRE(actual.layers[1].parts.size() == 5);
    const auto indices = [](const PartLinks &links, const LayerParts &owners) {
        std::vector<size_t> result;
        for (const auto link : links)
            result.push_back(size_t(link - owners.cbegin()));
        return result;
    };
    const std::vector<std::vector<size_t>> previous{{0}, {1}, {2}, {3}, {3}};
    const std::vector<std::vector<size_t>> next{{0}, {1}, {2}, {3, 4}};
    for (size_t i = 0; i < previous.size(); ++i) {
        const auto &part = actual.layers[1].parts[i];
        CHECK(indices(part.prev_parts, actual.layers[0].parts) == previous[i]);
        CHECK(part.next_parts.empty());
        CHECK(part.peninsulas.empty());
        CHECK(*part.shape == original[1][i]);
        CHECK(part.extend_shape == offset_ex(*part.shape, config.removing_delta,
                                             ClipperLib::jtSquare));
    }
    for (size_t i = 0; i < next.size(); ++i) {
        const auto &part = actual.layers[0].parts[i];
        CHECK(indices(part.next_parts, actual.layers[1].parts) == next[i]);
        CHECK(part.prev_parts.empty());
        CHECK(part.extend_shape.empty());
        CHECK(*part.shape == original[0][i]);
    }
    CHECK(actual.slices == original);
    CHECK(input == original);
    CHECK(std::fegetround() == original_rounding);
}


TEST_CASE("Profiled polygon offsets preserve literal geometry and caller-owned counters",
          "[sla_offset_profile]")
{
    const auto rectangle = [](double x0, double y0, double x1, double y1) {
        return ExPolygon(Polygon({Point(scale_(x0), scale_(y0)),
            Point(scale_(x1), scale_(y0)), Point(scale_(x1), scale_(y1)),
            Point(scale_(x0), scale_(y1))}));
    };
    ExPolygon perforated = rectangle(0, 0, 30, 30);
    for (const double x : {3., 12., 21.}) {
        Polygon hole = rectangle(x, 5, x + 4, 20).contour;
        hole.make_clockwise();
        perforated.holes.push_back(std::move(hole));
    }
    const std::vector<ExPolygons> inputs{
        {}, {rectangle(0, 0, 10, 10)}, {perforated},
        {perforated, rectangle(40, 0, 50, 10)}};
    const auto original = inputs;
    const int rounding = std::fegetround();
    const auto join = GENERATE(ClipperLib::jtSquare, ClipperLib::jtMiter, ClipperLib::jtRound);
    const float delta = float(scale_(GENERATE(-100., -1., 0., .5, 3.)));
    // Independent callers use the same ordinary geometry contract. No point
    // sorting, contour rotation or tolerance can hide an ordering difference.
    std::vector<ClipperOffsetProfile> counters(inputs.size());
    std::vector<ExPolygons> actual(inputs.size());
    std::vector<int64_t> whole_ns(inputs.size());
    tbb::parallel_for(size_t(0), inputs.size(), [&](size_t i) {
        const auto begin = std::chrono::steady_clock::now();
        actual[i] = offset_ex_profiled(inputs[i], delta, counters[i], join);
        whole_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count();
    });
    for (size_t i = 0; i < inputs.size(); ++i) {
        CAPTURE(i, delta, join);
        CHECK(actual[i] == offset_ex(inputs[i], delta, join));
        const auto &profile = counters[i];
        CHECK(profile.calls[ClipperOffsetProfile::Outer] == inputs[i].size());
        CHECK(profile.calls[ClipperOffsetProfile::Union] == 1);
        CHECK(profile.calls[ClipperOffsetProfile::Tree] == 1);
        CHECK(profile.calls[ClipperOffsetProfile::Combine] <= inputs[i].size());
        CHECK(profile.calls[ClipperOffsetProfile::Holes] <= (i >= 2 ? 1 : 0));
        CHECK(profile.hole_offsets == 3 * profile.calls[ClipperOffsetProfile::Holes]);
        CHECK(std::all_of(profile.elapsed_ns.begin(), profile.elapsed_ns.end(),
                          [](int64_t value) { return value >= 0; }));
        CHECK(std::accumulate(profile.elapsed_ns.begin(), profile.elapsed_ns.end(),
                              int64_t(0)) <= whole_ns[i]);
        if (delta >= 0) {
            CHECK(profile.calls[ClipperOffsetProfile::Combine] == inputs[i].size());
            CHECK(profile.calls[ClipperOffsetProfile::Holes] == (i >= 2 ? 1 : 0));
        } else if (delta == float(scale_(-100.))) {
            CHECK(profile.calls[ClipperOffsetProfile::Combine] == 0);
            CHECK(profile.hole_offsets == 0);
            CHECK(actual[i].empty());
        }
        // Reusing a caller's accumulator must sum stages without leaking data
        // from another parallel invocation or changing the repeated geometry.
        auto accumulated = profile;
        CHECK(offset_ex_profiled(inputs[i], delta, accumulated, join) == actual[i]);
        for (size_t stage = 0; stage < ClipperOffsetProfile::Count; ++stage) {
            CHECK(accumulated.calls[stage] == 2 * profile.calls[stage]);
            CHECK(accumulated.elapsed_ns[stage] >= profile.elapsed_ns[stage]);
        }
        CHECK(accumulated.hole_offsets == 2 * profile.hole_offsets);
    }
    CHECK(inputs == original);
    CHECK(std::fegetround() == rounding);
}

#include "clipper/ClipperIntegerRounding.hpp"
#include <limits>

namespace {
struct ClipperRoundingModeRestore {
    int mode = std::fegetround();
    ~ClipperRoundingModeRestore() { std::fesetround(mode); }
};
#if defined(__GNUC__)
__attribute__((noinline))
#endif
double clipper_legacy_round_oracle(double value)
{
    // Volatile forces the historical addition to occur in the requested mode.
    volatile double shifted = value + 0.5;
    return value == 0.49999999999999994 ? 0. : std::floor(shifted);
}
template<class Integer>
#if defined(__GNUC__)
__attribute__((noinline))
#endif
Integer clipper_new_round(double value) { return ClipperIntegerRounding::round_up<Integer>(value); }
}

TEST_CASE("Clipper integer rounding preserves signed ties boundaries and rounding modes", "[sla_clipper_integer_rounding]")
{
    ClipperRoundingModeRestore restore;
    const int mode = GENERATE(FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO);
    REQUIRE(std::fesetround(mode) == 0);
    const auto check = [](double value) {
        const double expected = clipper_legacy_round_oracle(value);
        CAPTURE(value, expected);
        if (expected >= double(std::numeric_limits<int32_t>::min()) &&
            expected <= double(std::numeric_limits<int32_t>::max()))
            CHECK(clipper_new_round<int32_t>(value) == static_cast<int32_t>(expected));
        if (expected >= -std::ldexp(1.,63) && expected < std::ldexp(1.,63))
            CHECK(clipper_new_round<int64_t>(value) == static_cast<int64_t>(expected));
    };
    for (int i=-2048;i<=2048;++i) {
        const double half = i + .5;
        for (double value : {double(i),half,std::nextafter(half,-INFINITY),std::nextafter(half,INFINITY)})
            check(value);
    }
    for (double base : {0.,std::ldexp(1.,31),-std::ldexp(1.,31),std::ldexp(1.,52),
                        -std::ldexp(1.,52),std::ldexp(1.,62),-std::ldexp(1.,62)}) {
        for (double value : {base-1.5,base-.5,base,base+.5,base+1.5,
                            std::nextafter(base,-INFINITY),std::nextafter(base,INFINITY)})
            check(value);
    }
    uint64_t random = 1729;
    for (size_t i=0;i<10000;++i) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        // Every finite exponent including subnormal and tiny negative values;
        // check() excludes only the original cast's nonrepresentable domain.
        uint64_t bits = random;
        if ((bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000))
            bits ^= UINT64_C(0x0010000000000000);
        double value; std::memcpy(&value,&bits,sizeof(value)); check(value);
    }
    CHECK(clipper_new_round<int32_t>(-.5) == 0);
    CHECK(clipper_new_round<int32_t>(.5) == 1);
    CHECK(clipper_new_round<int32_t>(-2.5) == -2);
    CHECK(clipper_new_round<int32_t>(2.5) == 3);
    CHECK(clipper_new_round<int32_t>(0.49999999999999994) == 0);
    CHECK(std::fegetround() == mode);
}


TEST_CASE("Clipper integer rounding retains intersection coordinates and Z callbacks", "[sla_clipper_integer_rounding_z]")
{
    using namespace ClipperLib_Z;
    const Path subject{IntPoint(0,0,11),IntPoint(10,0,12),IntPoint(10,10,13),IntPoint(0,10,14)};
    const Path clip{IntPoint(5,-5,21),IntPoint(15,-5,22),IntPoint(15,5,23),IntPoint(5,5,24)};
    Clipper engine; size_t callbacks=0;
    engine.ZFillFunction([&](const IntPoint&,const IntPoint&,const IntPoint&,const IntPoint&,IntPoint &point){
        ++callbacks; point.z()=900;
    });
    REQUIRE(engine.AddPath(subject,ptSubject,true)); REQUIRE(engine.AddPath(clip,ptClip,true));
    Paths output; REQUIRE(engine.Execute(ctIntersection,output,pftNonZero,pftNonZero));
    REQUIRE(output.size()==1); REQUIRE(output[0].size()==4);
    std::vector<std::array<int32_t,3>> coordinates;
    for (const auto &point:output[0]) coordinates.push_back({point.x(),point.y(),point.z()});
    std::sort(coordinates.begin(),coordinates.end());
    const std::vector<std::array<int32_t,3>> expected{{5,0,900},{5,5,24},{10,0,12},{10,5,900}};
    CHECK(coordinates==expected); CHECK(callbacks>=2);
}
