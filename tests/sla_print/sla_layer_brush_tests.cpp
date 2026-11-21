// Pure rasterisers of the pixel editor and the canvas geometry helpers.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/SLA/LayerDocument.hpp"
#include "../../src/slic3r/GUI/LayerCanvasGeometry.hpp"

using namespace Slic3r;
using Doc = sla::LayerDocument;
using Brush = Doc::BrushShape;

namespace {
png::ImageGreyscale blank(size_t cols, size_t rows, uint8_t v = 0)
{
    png::ImageGreyscale img;
    img.cols = cols; img.rows = rows; img.buf.assign(cols*rows, v);
    return img;
}
size_t count_of(const png::ImageGreyscale &img, uint8_t v)
{
    size_t n = 0;
    for (auto b : img.buf) n += b == v;
    return n;
}
uint8_t at(const png::ImageGreyscale &img, int x, int y) { return img.buf[size_t(y)*img.cols + size_t(x)]; }
}

TEST_CASE("Square stamp covers exactly (2r+1)^2 pixels", "[sla_layers][sla_brush]")
{
    auto img = blank(20, 20);
    CHECK(Doc::draw_stroke(img, 10, 10, 10, 10, 2, 200, Brush::Square) == 25);
    CHECK(count_of(img, 200) == 25);
    CHECK(at(img, 8, 8) == 200); CHECK(at(img, 12, 12) == 200);
    CHECK(at(img, 7, 10) == 0);  CHECK(at(img, 13, 10) == 0);
}

TEST_CASE("Square stroke is clipped at borders and may start outside", "[sla_layers][sla_brush]")
{
    auto img = blank(10, 10);
    CHECK(Doc::draw_stroke(img, 0, 0, 0, 0, 2, 255, Brush::Square) == 9); // 3x3 survives
    CHECK(count_of(img, 255) == 9);
    auto other = blank(10, 10);
    CHECK(Doc::draw_stroke(other, -20, -20, -15, -15, 2, 255, Brush::Square) == 0);
    auto edge = blank(10, 10);
    CHECK(Doc::draw_stroke(edge, 9, 9, 12, 9, 0, 1, Brush::Square) == 1);
}

TEST_CASE("Thick horizontal line with square brush is a solid bar", "[sla_layers][sla_brush]")
{
    auto img = blank(30, 12);
    // Segment 5..24 at y=6, side 3 -> columns 4..25 (22) x rows 5..7 (3)
    CHECK(Doc::draw_stroke(img, 5, 6, 24, 6, 1, 90, Brush::Square) == 66);
    CHECK(count_of(img, 90) == 66);
    CHECK(at(img, 4, 5) == 90); CHECK(at(img, 25, 7) == 90);
    CHECK(at(img, 3, 6) == 0);  CHECK(at(img, 26, 6) == 0);
}

TEST_CASE("Round stroke matches the existing disk rasteriser", "[sla_layers][sla_brush]")
{
    auto a = blank(40, 40), b = blank(40, 40);
    Doc::draw_line(a, 10, 12, 30, 25, 4, 255);
    Doc::draw_stroke(b, 10, 12, 30, 25, 4, 255, Brush::Round);
    CHECK(a.buf == b.buf);
}

TEST_CASE("Rectangle fill is inclusive, order independent and clipped", "[sla_layers][sla_brush]")
{
    auto img = blank(10, 10);
    CHECK(Doc::fill_rectangle(img, 6, 4, 2, 1, 77) == 20); // x 2..6, y 1..4
    CHECK(at(img, 2, 1) == 77); CHECK(at(img, 6, 4) == 77);
    CHECK(at(img, 1, 1) == 0);  CHECK(at(img, 7, 4) == 0); CHECK(at(img, 2, 5) == 0);
    auto clip = blank(10, 10);
    CHECK(Doc::fill_rectangle(clip, -5, 8, 3, 20, 5) == 4*2); // x 0..3, y 8..9
    CHECK(Doc::fill_rectangle(clip, 50, 50, 60, 60, 5) == 0);
}

TEST_CASE("Ellipse fill: circle, degenerate boxes, symmetry", "[sla_layers][sla_brush]")
{
    auto one = blank(5, 5);
    CHECK(Doc::fill_ellipse(one, 2, 2, 2, 2, 9) == 1);
    auto two = blank(5, 5);
    CHECK(Doc::fill_ellipse(two, 1, 1, 2, 2, 9) == 4);
    auto three = blank(5, 5);
    // 3x3 box: centres within radius 1.5 -> all 9 (corner distance 1.414)
    CHECK(Doc::fill_ellipse(three, 0, 0, 2, 2, 9) == 9);
    auto five = blank(5, 5);
    // 5x5 box, radius 2.5: corners at 2.83 are out, (2,1) at 2.24 is in -> 25-4
    CHECK(Doc::fill_ellipse(five, 0, 0, 4, 4, 9) == 21);
    CHECK(at(five, 0, 0) == 0); CHECK(at(five, 4, 4) == 0); CHECK(at(five, 2, 0) == 9);
    auto wide = blank(30, 10);
    Doc::fill_ellipse(wide, 0, 0, 29, 9, 255);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 30; ++x) {
            CHECK(at(wide, x, y) == at(wide, 29-x, y));
            CHECK(at(wide, x, y) == at(wide, x, 9-y));
        }
    auto clip = blank(4, 4);
    CHECK(Doc::fill_ellipse(clip, -10, -10, 20, 20, 1) > 0); // clipped, no crash
}

TEST_CASE("Flood fill is 4-connected and stops at different grey values", "[sla_layers][sla_brush]")
{
    auto img = blank(6, 6);
    // Diagonal wall with a one-pixel gap only on a diagonal: 4-connectivity must not leak.
    for (int i = 0; i < 6; ++i) img.buf[size_t(i)*6 + size_t(5-i)] = 255;
    CHECK(Doc::flood_fill(img, 0, 0, 100) == 15); // upper-left triangle (x+y<5)
    CHECK(at(img, 0, 0) == 100); CHECK(at(img, 4, 0) == 100);
    CHECK(at(img, 5, 0) == 255); // wall untouched
    CHECK(at(img, 5, 5) == 0);   // other side untouched
    CHECK(Doc::flood_fill(img, 5, 5, 100) == 15);
}

TEST_CASE("Flood fill tolerance zero and trivial cases", "[sla_layers][sla_brush]")
{
    auto img = blank(4, 4, 10);
    img.buf[5] = 11; // near-equal value is a boundary (tolerance 0)
    CHECK(Doc::flood_fill(img, 0, 0, 50) == 15);
    CHECK(at(img, 1, 1) == 11);
    CHECK(Doc::flood_fill(img, 0, 0, 50) == 0);   // already that value
    CHECK(Doc::flood_fill(img, -1, 0, 1) == 0);   // seed outside
    CHECK(Doc::flood_fill(img, 4, 0, 1) == 0);
    auto big = blank(300, 300);
    CHECK(Doc::flood_fill(big, 150, 150, 255) == 90000); // no recursion overflow
}

TEST_CASE("apply_shape dispatches to the rasterisers", "[sla_layers][sla_brush]")
{
    auto img = blank(10, 10);
    Doc::ShapeOp op;
    op.value = 255; op.x0 = 1; op.y0 = 1; op.x1 = 3; op.y1 = 2;
    op.tool = Doc::ShapeTool::Rectangle;
    CHECK(Doc::apply_shape(img, op) == 6);
    op.tool = Doc::ShapeTool::Fill; op.x0 = 9; op.y0 = 9; op.value = 7;
    CHECK(Doc::apply_shape(img, op) == 94);
    op.tool = Doc::ShapeTool::Line; op.brush = Brush::Square; op.radius = 0;
    op.x0 = 0; op.y0 = 9; op.x1 = 3; op.y1 = 9; op.value = 1;
    CHECK(Doc::apply_shape(img, op) == 4);
}

TEST_CASE("Canvas geometry: zoom anchor keeps the pixel under the cursor", "[sla_layers][sla_brush]")
{
    using namespace GUI;
    CHECK(layer_zoom_clamp(100.) == layer_zoom_max);
    CHECK(layer_zoom_clamp(0.) == layer_zoom_min);
    CHECK(layer_zoom_after_steps(layer_zoom_after_steps(1., 3), -3) == Catch::Approx(1.));
    // Cursor at canvas x=400 (scale 1, scroll 100, border 4 -> viewport x=304); zoom to 2.
    const double scroll = layer_zoom_anchor_scroll(400., 304., 1., 2., 4.);
    CHECK(scroll == Catch::Approx(500.));
    // Image point under cursor before: 400/1; after: (304+500-4)/2 = 400.
    CHECK((304. + scroll - 4.) / 2. == Catch::Approx(400.));
    CHECK(layer_zoom_anchor_scroll(0., 300., 1., 2., 0.) == 0.); // never negative
    CHECK(layer_pan_scroll(50., 80.) == 0.);
    CHECK(layer_pan_scroll(50., -30.) == 80.);
}

TEST_CASE("Canvas geometry: pixel mapping and mm", "[sla_layers][sla_brush]")
{
    using namespace GUI;
    CHECK(layer_canvas_pixel(-5, 100, 200) == 0);
    CHECK(layer_canvas_pixel(50, 100, 200) == 100);
    CHECK(layer_canvas_pixel(500, 100, 200) == 199);
    CHECK(layer_pixel_to_canvas(100, 100, 200) == Catch::Approx(50.));
    CHECK(layer_pixel_to_mm(0, 100, 50.) == Catch::Approx(0.25));
    CHECK(layer_canvas_contains(0, 10)); CHECK(!layer_canvas_contains(10, 10));
    CHECK(!layer_canvas_contains(-1, 10));
}
