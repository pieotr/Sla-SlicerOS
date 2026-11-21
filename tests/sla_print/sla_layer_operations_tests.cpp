// Tests for the native UVtools layer operations (libslic3r/SLA/LayerOperations.hpp).
// Masks are written as ASCII art: '.' = 0, '#' = 255, digits 1-9 = value * 28 (so 9 = 252).
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "libslic3r/SLA/LayerOperations.hpp"

using namespace Slic3r::sla;
using namespace Slic3r::sla::layerops;

namespace {

Mask art(const std::vector<std::string> &rows)
{
    Mask m(rows.front().size(), rows.size());
    for (size_t y = 0; y < rows.size(); ++y)
        for (size_t x = 0; x < rows[y].size(); ++x) {
            const char c = rows[y][x];
            m.at(x, y) = c == '.' ? 0 : c == '#' ? 255 : uint8_t((c - '0') * 28);
        }
    return m;
}

std::vector<std::string> to_art(const Mask &m)
{
    std::vector<std::string> rows(m.height);
    for (size_t y = 0; y < m.height; ++y)
        for (size_t x = 0; x < m.width; ++x) {
            const uint8_t v = m.at(x, y);
            rows[y] += v == 0 ? '.' : v == 255 ? '#' : char('0' + v / 28);
        }
    return rows;
}

using Rows = std::vector<std::string>;

Mask block(size_t w, size_t h, int x0, int y0, int bw, int bh, uint8_t v = 255)
{
    Mask m(w, h);
    for (int y = y0; y < y0 + bh; ++y)
        for (int x = x0; x < x0 + bw; ++x) m.at(size_t(x), size_t(y)) = v;
    return m;
}

} // namespace

TEST_CASE("layerops move: anchor, margins, copy and bounds", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers{block(8, 8, 0, 0, 2, 2)};
    MoveParams p;
    p.anchor = Anchor::BottomRight;
    move_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"........", "........", "........", "........", "........", "........", "......##", "......##"});

    // Copy keeps the source; margins push the destination (left margin adds, right margin subtracts).
    layers = {block(8, 8, 0, 0, 2, 2)};
    p = MoveParams{};
    p.anchor = Anchor::TopLeft;
    p.margin_left = 3;
    p.margin_top = 1;
    p.is_cut_move = false;
    move_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"##......", "##.##...", "...##...", "........", "........", "........", "........", "........"});

    // Overlapping move must not smear: shift a 4x1 bar right by 1.
    layers = {block(8, 2, 0, 0, 4, 1)};
    p = MoveParams{};
    p.selection.roi = {0, 0, 4, 1};
    p.anchor = Anchor::TopLeft;
    p.margin_left = 1;
    move_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{".####...", "........"});

    // Destination outside the plate is an error and leaves the layers untouched.
    layers = {block(8, 8, 0, 0, 2, 2)};
    p = MoveParams{};
    p.anchor = Anchor::TopLeft;
    p.margin_left = 7;
    CHECK_FALSE(validate_move(p, layers).empty());
    CHECK_THROWS_AS(move_layers(layers, p), std::invalid_argument);
    CHECK(layers[0] == block(8, 8, 0, 0, 2, 2));

    // Destination maths: centre of a 10x10 image for a 4x2 box.
    CHECK(compute_move_destination({0, 0, 4, 2}, 10, 10, Anchor::MiddleCenter, 0, 0, 0, 0) == Rect{3, 4, 4, 2});
    CHECK(compute_move_destination({0, 0, 4, 2}, 10, 10, Anchor::BottomRight, 1, 0, 0, 1) == Rect{7, 7, 4, 2});
}

TEST_CASE("layerops flip: directions and blend", "[sla_layers][sla_layerops]")
{
    const Mask src = art({"#...", "##..", "###.", "...."});
    FlipParams p;
    std::vector<Mask> layers{src};
    p.direction = FlipDirection::Horizontal;
    flip_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"...#", "..##", ".###", "...."});

    layers = {src};
    p.direction = FlipDirection::Vertical;
    flip_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"....", "###.", "##..", "#..."});

    layers = {src};
    p.direction = FlipDirection::Both;
    flip_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"....", ".###", "..##", "...#"});

    layers = {src};
    p.direction = FlipDirection::Horizontal;
    p.make_copy = true;
    flip_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"#..#", "####", "####", "...."});

    // ROI flips only the region.
    layers = {art({"#.#.", "....", "....", "...."})};
    p = FlipParams{};
    p.selection.roi = {0, 0, 2, 1};
    flip_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{".##.", "....", "....", "...."});
}

TEST_CASE("layerops rotate: quarter turns and no-ops", "[sla_layers][sla_layerops]")
{
    // Like OpenCV/UVtools the pivot is (w/2, h/2) in pixel-index space, so a 4x4 turn maps (x, y) -> (4 - y, x) for 90 deg CW.
    const Mask src = art({"....", ".##.", ".#..", "...."});
    RotateParams p;
    p.angle_degrees = 90; // clockwise
    std::vector<Mask> layers{src};
    rotate_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"....", "..##", "...#", "...."});

    p.angle_degrees = -90;
    layers = {src};
    rotate_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"....", "....", ".#..", ".##."});

    p.angle_degrees = 180;
    layers = {src};
    rotate_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"....", "....", "...#", "..##"});

    p.angle_degrees = 360; // full turn: untouched
    layers = {src};
    rotate_layers(layers, p);
    CHECK(layers[0] == src);

    CHECK_FALSE(validate_rotate(RotateParams{{}, 0}).empty());
    CHECK(validate_rotate(RotateParams{{}, 15}).empty());

    // Arbitrary angle keeps the pixel count roughly (centre pixel of a plus sign stays set).
    layers = {block(9, 9, 3, 3, 3, 3)};
    p.angle_degrees = 30;
    rotate_layers(layers, p);
    CHECK(layers[0].at(4, 4) == 255);
}

TEST_CASE("layerops resize: exact 50 percent, validation, fade", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers{Mask(8, 8, 255)};
    ResizeParams p;
    p.x_percent = p.y_percent = 50;
    resize_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"........", "........", "..####..", "..####..", "..####..", "..####..", "........", "........"});

    p = ResizeParams{};
    CHECK_FALSE(validate_resize(p).empty()); // 100 % / 100 %
    p.x_percent = 0;
    CHECK_FALSE(validate_resize(p).empty());
    p.x_percent = 50;
    p.y_percent = 100;
    CHECK(validate_resize(p).empty());

    // Only X: width halves, height stays.
    layers = {Mask(8, 4, 255)};
    resize_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"..####..", "..####..", "..####..", "..####.."});

    // Constrain follows X.
    p = ResizeParams{};
    p.x_percent = 50;
    p.y_percent = 100;
    p.constrain_xy = true;
    layers = {Mask(8, 8, 255)};
    resize_layers(layers, p);
    CHECK(layers[0].at(2, 2) == 255);
    CHECK(layers[0].at(1, 1) == 0);

    // Fade: first layer uses the full 50 %, later layers drift back toward 100 % (more pixels).
    p = ResizeParams{};
    p.x_percent = p.y_percent = 50;
    p.is_fade = true;
    layers = {Mask(8, 8, 255), Mask(8, 8, 255)};
    resize_layers(layers, p);
    CHECK(layers[0].nonzero_count() == 16);
    CHECK(layers[1].nonzero_count() > 16);
}

TEST_CASE("layerops pattern: grid placement, limits, anchor", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers{block(10, 10, 0, 0, 2, 2)};
    PatternParams p;
    p.cols = p.rows = 2;
    p.col_spacing = p.row_spacing = 2;
    pattern_layers(layers, p);
    // Pattern volume is 6x6, centred => offset 2.
    CHECK(to_art(layers[0]) == Rows{"..........", "..........", "..##..##..", "..##..##..", "..........", "..........", "..##..##..", "..##..##..", "..........", ".........."});

    layers = {block(10, 10, 0, 0, 2, 2)};
    p.anchor = Anchor::TopLeft;
    pattern_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"##..##....", "##..##....", "..........", "..........", "##..##....", "##..##....", "..........", "..........", "..........", ".........."});

    const PatternLimits lim = compute_pattern_limits(10, 10, Rect{0, 0, 2, 2});
    CHECK(lim.max_cols == 5);
    CHECK(lim.max_rows == 5);
    CHECK(lim.max_col_spacing == 0); // (10 - 2*5) / 5

    layers = {block(10, 10, 0, 0, 2, 2)};
    p = PatternParams{};
    CHECK_FALSE(validate_pattern(p, layers).empty()); // 1 x 1
    p.cols = 6;
    p.col_spacing = 0;
    CHECK_FALSE(validate_pattern(p, layers).empty()); // 12 px > 10 px
    CHECK_THROWS_AS(pattern_layers(layers, p), std::invalid_argument);
}

TEST_CASE("layerops solidify: fills enclosed holes only", "[sla_layers][sla_layerops]")
{
    SolidifyParams p;
    std::vector<Mask> layers{art({"#######", "#.....#", "#.....#", "#.....#", "#######", ".......", "......."})};
    solidify_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"#######", "#######", "#######", "#######", "#######", ".......", "......."});

    // Open C-shape: the cavity touches the border, so it is not a hole.
    const Mask open = art({"#####", "#....", "#....", "#####"});
    layers = {open};
    solidify_layers(layers, p);
    CHECK(layers[0] == open);

    // A hole that contains an island is left alone (UVtools skips holes with a child contour).
    const Mask island = art({"#######", "#.....#", "#..#..#", "#.....#", "#######"});
    layers = {island};
    solidify_layers(layers, p);
    CHECK(layers[0] == island);

    // Area filter: More = fill holes >= area, Less = fill holes <= area.
    const Mask two = art({"#########", "#..#....#", "#..#....#", "#########"});
    layers = {two};
    p.minimum_area = 5;
    p.area_check = AreaCheck::More;
    solidify_layers(layers, p);
    CHECK(layers[0].at(1, 1) == 0);   // 2 px hole smaller than 5: kept
    CHECK(layers[0].at(5, 1) == 255); // 8 px hole: filled

    layers = {two};
    p.area_check = AreaCheck::Less;
    solidify_layers(layers, p);
    CHECK(layers[0].at(1, 1) == 255);
    CHECK(layers[0].at(5, 1) == 0);

    // Grey anti-aliasing below 128 counts as background.
    layers = {art({"#####", "#3#2#", "#####"})}; // 3*28=84, 2*28=56
    SolidifyParams q;
    solidify_layers(layers, q);
    CHECK(to_art(layers[0]) == Rows{"#####", "#####", "#####"});
}

TEST_CASE("layerops threshold: all modes", "[sla_layers][sla_layerops]")
{
    const auto run = [](ThresholdType t) {
        Mask m(6, 1);
        const uint8_t v[6] = {0, 100, 127, 128, 200, 255};
        for (size_t i = 0; i < 6; ++i) m.px[i] = v[i];
        std::vector<Mask> layers{m};
        ThresholdParams p;
        p.threshold = 127;
        p.maximum = 200;
        p.type = t;
        threshold_layers(layers, p);
        return layers[0].px;
    };
    using V = std::vector<uint8_t>;
    CHECK(run(ThresholdType::Binary) == V{0, 0, 0, 200, 200, 200});
    CHECK(run(ThresholdType::BinaryInv) == V{200, 200, 200, 0, 0, 0});
    CHECK(run(ThresholdType::Trunc) == V{0, 100, 127, 127, 127, 127});
    CHECK(run(ThresholdType::ToZero) == V{0, 0, 0, 128, 200, 255});
    CHECK(run(ThresholdType::ToZeroInv) == V{0, 100, 127, 0, 0, 0});

    // Auto thresholds split a two-level image between its levels.
    Mask bimodal(4, 1);
    bimodal.px = {10, 10, 200, 200};
    for (ThresholdType t : {ThresholdType::Otsu, ThresholdType::Triangle}) {
        Mask m = bimodal;
        apply_threshold(m, 0, 255, t);
        CHECK(m.px == V{0, 0, 255, 255});
    }

    // ROI restricts the operation.
    std::vector<Mask> layers{Mask(4, 1, 200)};
    ThresholdParams p;
    p.threshold = 250;
    p.selection.roi = {1, 0, 2, 1};
    threshold_layers(layers, p);
    CHECK(layers[0].px == V{200, 0, 0, 200});
}

TEST_CASE("layerops pixel dimming: pattern text, walls and generators", "[sla_layers][sla_layerops]")
{
    std::string err;
    const PixelPattern bad = parse_pattern_text("1 2\n3", &err);
    CHECK(bad.empty());
    CHECK(err.find("Row 2") != std::string::npos);
    err.clear();
    CHECK(parse_pattern_text("1 300", &err).empty());
    CHECK_FALSE(err.empty());
    err.clear();
    const PixelPattern ok = parse_pattern_text("255 127\n127 255\n", &err);
    CHECK(err.empty());
    CHECK(ok.rows == 2);
    CHECK(ok.cols == 2);
    CHECK(ok.v == std::vector<uint8_t>{255, 127, 127, 255});

    PixelDimmingParams p;
    CHECK_FALSE(validate_pixel_dimming(p).empty()); // both patterns empty

    // Solid pattern 127 (subtracts 128), wall 1: the interior of a 6x6 block dims, the 1 px wall stays.
    p.pattern_text = "127";
    p.wall_thickness_start = p.wall_thickness_end = 1;
    std::vector<Mask> layers{block(10, 10, 2, 2, 6, 6)};
    pixel_dimming_layers(layers, p);
    CHECK(layers[0].at(2, 2) == 255);
    CHECK(layers[0].at(3, 3) == 127);
    CHECK(layers[0].at(6, 6) == 127);
    CHECK(layers[0].at(7, 7) == 255);
    CHECK(layers[0].at(1, 1) == 0);

    // Walls only: the wall dims, the inside stays.
    p.walls_only = true;
    layers = {block(10, 10, 2, 2, 6, 6)};
    pixel_dimming_layers(layers, p);
    CHECK(layers[0].at(2, 2) == 127);
    CHECK(layers[0].at(3, 3) == 255);
    CHECK(layers[0].at(1, 1) == 0);

    // Lightening adds instead: a 100-grey block lightens by 128 inside the wall.
    p = PixelDimmingParams{};
    p.pattern_text = "127";
    p.lightening_pixels = true;
    p.wall_thickness_start = p.wall_thickness_end = 1;
    layers = {block(10, 10, 2, 2, 6, 6, 100)};
    pixel_dimming_layers(layers, p);
    CHECK(layers[0].at(4, 4) == 228);
    CHECK(layers[0].at(2, 2) == 100);

    // Alternate pattern switches every layer: chessboard on a full wall-less layer.
    p = PixelDimmingParams{};
    const PatternTexts chess = generate_dimming_pattern("Chessboard", 127);
    p.pattern_text = chess.pattern;
    p.alternate_pattern_text = chess.alternate;
    p.wall_thickness_start = p.wall_thickness_end = 0;
    layers = {block(4, 4, 0, 0, 4, 4), block(4, 4, 0, 0, 4, 4)};
    // Erode by 0 keeps every pixel as "inside"; pattern value 127 darkens by 128.
    pixel_dimming_layers(layers, p);
    CHECK(layers[0].at(0, 0) == 255);
    CHECK(layers[0].at(1, 0) == 127);
    CHECK(layers[1].at(0, 0) == 127); // alternate layer is mirrored
    CHECK(layers[1].at(1, 0) == 255);

    CHECK(generate_dimming_pattern("Solid", 100).pattern == "100");
    CHECK(generate_infill_pattern("Rectilinear", 1, 2).pattern == "0\n0\n255\n255");
}

TEST_CASE("layerops infill: pillars keep a hollow model with pillars", "[sla_layers][sla_layerops]")
{
    // 24x24 solid block inside a 30x30 image, 3 layers.
    std::vector<Mask> layers(3, block(30, 30, 3, 3, 24, 24));
    const std::vector<double> z{0.05, 0.10, 0.15};
    InfillParams p;
    p.type = InfillAlgorithm::Pillars;
    p.floor_ceil_thickness_mm = 0; // no solid floor/ceiling, every layer is infilled
    p.wall_thickness = 2;
    p.infill_spacing = 10;
    p.infill_thickness = 4;
    infill_layers(layers, z, p);
    const Mask &m = layers[0];
    // Target = model bounds (24x24 at (3,3)). The 2 px wall ring stays solid (176 px); the inside keeps only the
    // 2x2 corner squares of every 10 px tile, which merge into 4x4 pillars centred on tile corners (4 x 16 = 64 px).
    CHECK(m.nonzero_count() == 176 + 64);
    CHECK(m.at(3, 3) == 255);   // wall
    CHECK(m.at(8, 8) == 0);     // inside, between pillars
    CHECK(m.at(11, 11) == 255); // pillar at tile corner (10,10) -> pixels 8..11
    CHECK(m.at(15, 15) == 0);
    CHECK(m.at(0, 0) == 0);     // outside the model
    CHECK(validate_infill(p, layers, {0.05}).find("Z") != std::string::npos);
    InfillParams bad = p;
    bad.infill_spacing = 0;
    CHECK_FALSE(validate_infill(bad, layers, z).empty());
}

TEST_CASE("layerops infill: floor and ceiling stay solid", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers(5, block(40, 40, 5, 5, 30, 30));
    const std::vector<double> z{0.05, 0.10, 0.15, 0.20, 0.25};
    InfillParams p;
    p.type = InfillAlgorithm::Pillars;
    p.floor_ceil_thickness_mm = 0.1; // two layers of solid skin
    p.wall_thickness = 2;
    p.infill_spacing = 20;
    p.infill_thickness = 4;
    const Mask solid = layers[0];
    infill_layers(layers, z, p);
    // Layers with too few solid layers below (0, 1) or above (3, 4) are skipped; only the middle layer is hollowed.
    CHECK(layers[0] == solid);
    CHECK(layers[1] == solid);
    CHECK(layers[2] != solid);
    CHECK(layers[3] == solid);
    CHECK(layers[4] == solid);
    // The hollow layer still has its outer wall.
    CHECK(layers[2].at(5, 5) == 255);
    CHECK(layers[2].at(6, 20) == 255);
    CHECK(layers[2].at(20, 20) == 0);
}

TEST_CASE("layerops infill: other algorithms produce stripes without touching walls", "[sla_layers][sla_layerops]")
{
    for (InfillAlgorithm a : {InfillAlgorithm::Honeycomb, InfillAlgorithm::Concentric, InfillAlgorithm::Waves, InfillAlgorithm::Gyroid,
                              InfillAlgorithm::CubicCross, InfillAlgorithm::CubicStar, InfillAlgorithm::CubicCrossAlternating}) {
        std::vector<Mask> layers(2, block(120, 120, 10, 10, 100, 100));
        const std::vector<double> z{0.05, 0.10};
        InfillParams p;
        p.type = a;
        p.floor_ceil_thickness_mm = 0;
        p.wall_thickness = 5;
        p.infill_spacing = 30;
        p.infill_thickness = 6;
        infill_layers(layers, z, p);
        const size_t n = layers[0].nonzero_count();
        INFO("algorithm " << int(a));
        CHECK(n > 0);
        CHECK(n < 100 * 100);
        CHECK(layers[0].at(10, 10) == 255); // corner pixel belongs to the untouched wall ring
        CHECK(layers[0].at(5, 5) == 0);     // outside the model stays empty
    }
}

TEST_CASE("layerops pixel arithmetic: operators and apply methods", "[sla_layers][sla_layerops]")
{
    const auto run = [](ArithmeticOperator op, uint8_t value, ArithmeticApply apply = ArithmeticApply::All) {
        Mask m(4, 1);
        m.px = {0, 100, 200, 255};
        std::vector<Mask> layers{m};
        PixelArithmeticParams p;
        p.op = op;
        p.value = value;
        p.apply = apply;
        pixel_arithmetic_layers(layers, p);
        return layers[0].px;
    };
    using V = std::vector<uint8_t>;
    CHECK(run(ArithmeticOperator::Set, 50) == V{50, 50, 50, 50});
    CHECK(run(ArithmeticOperator::Set, 50, ArithmeticApply::Model) == V{0, 50, 50, 50});
    CHECK(run(ArithmeticOperator::Add, 100) == V{100, 200, 255, 255});
    CHECK(run(ArithmeticOperator::Subtract, 100) == V{0, 0, 100, 155});
    CHECK(run(ArithmeticOperator::Multiply, 128) == V{0, 50, 100, 128});
    CHECK(run(ArithmeticOperator::Divide, 2) == V{0, 50, 100, 128});
    CHECK(run(ArithmeticOperator::Minimum, 150) == V{0, 100, 150, 150});
    CHECK(run(ArithmeticOperator::Maximum, 150) == V{150, 150, 200, 255});
    CHECK(run(ArithmeticOperator::BitwiseNot, 0) == V{255, 155, 55, 0});
    CHECK(run(ArithmeticOperator::BitwiseAnd, 0x0F) == V{0, 4, 8, 15});
    CHECK(run(ArithmeticOperator::BitwiseOr, 0x01) == V{1, 101, 201, 255});
    CHECK(run(ArithmeticOperator::BitwiseXor, 0xFF) == V{255, 155, 55, 0});
    CHECK(run(ArithmeticOperator::AbsDiff, 100) == V{100, 0, 100, 155});
    CHECK(run(ArithmeticOperator::Threshold, 127) == V{0, 0, 255, 255});

    PixelArithmeticParams p;
    p.op = ArithmeticOperator::Divide;
    p.value = 0;
    CHECK_FALSE(validate_pixel_arithmetic(p).empty());
    p = PixelArithmeticParams{};
    p.op = ArithmeticOperator::KeepRegion;
    CHECK_FALSE(validate_pixel_arithmetic(p).empty()); // needs a ROI
    p = PixelArithmeticParams{};
    p.op = ArithmeticOperator::Add;
    p.value = 0;
    CHECK_FALSE(validate_pixel_arithmetic(p).empty()); // no effect

    // Model walls: only the outer 1 px ring of a 6x6 block is set (elephant foot compensation preset).
    std::vector<Mask> layers{block(10, 10, 2, 2, 6, 6)};
    p = PixelArithmeticParams{};
    p.op = ArithmeticOperator::Set;
    p.apply = ArithmeticApply::ModelWalls;
    p.value = 190;
    p.wall_thickness_start = p.wall_thickness_end = 1;
    pixel_arithmetic_layers(layers, p);
    CHECK(layers[0].at(2, 2) == 190);
    CHECK(layers[0].at(7, 4) == 190);
    CHECK(layers[0].at(4, 4) == 255);
    CHECK(layers[0].at(1, 1) == 0);

    // Model inner: the complement.
    layers = {block(10, 10, 2, 2, 6, 6)};
    p.apply = ArithmeticApply::ModelInner;
    pixel_arithmetic_layers(layers, p);
    CHECK(layers[0].at(2, 2) == 255);
    CHECK(layers[0].at(4, 4) == 190);

    // Value step: +10 per layer.
    layers = {Mask(2, 1, 0), Mask(2, 1, 0), Mask(2, 1, 0)};
    p = PixelArithmeticParams{};
    p.op = ArithmeticOperator::Set;
    p.apply = ArithmeticApply::All;
    p.value = 100;
    p.value_step = 10;
    pixel_arithmetic_layers(layers, p);
    CHECK(layers[0].px[0] == 100);
    CHECK(layers[1].px[0] == 110);
    CHECK(layers[2].px[0] == 120);

    // Keep / discard region.
    layers = {Mask(4, 2, 255)};
    p = PixelArithmeticParams{};
    p.op = ArithmeticOperator::KeepRegion;
    p.selection.roi = {1, 0, 2, 1};
    pixel_arithmetic_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{".##.", "...."});
    layers = {Mask(4, 2, 255)};
    p.op = ArithmeticOperator::DiscardRegion;
    pixel_arithmetic_layers(layers, p);
    CHECK(to_art(layers[0]) == Rows{"#..#", "####"});

    // Surface method: the top skin (not covered by the next layer) plus a 1 px wall is selected.
    layers = {block(8, 8, 1, 1, 6, 6), block(8, 8, 1, 1, 3, 6)};
    p = PixelArithmeticParams{};
    p.op = ArithmeticOperator::Set;
    p.apply = ArithmeticApply::ModelSurface;
    p.value = 77;
    p.selection.first_layer = 0;
    p.selection.last_layer = 0;
    pixel_arithmetic_layers(layers, p);
    CHECK(layers[0].at(6, 4) == 77);  // exposed: layer 1 has nothing there
    CHECK(layers[0].at(2, 3) == 255); // covered and away from the wall
    CHECK(layers[0].at(1, 3) == 77);  // wall column
    CHECK(layers[0].at(3, 1) == 77);  // wall row
}

TEST_CASE("layerops raft relief: dimming, decimate, relief, tabs", "[sla_layers][sla_layerops]")
{
    // Layers 0..1 are a solid raft, layer 2 has five small supports.
    auto make = [] {
        std::vector<Mask> layers(3, block(60, 60, 5, 5, 50, 50));
        layers[2] = Mask(60, 60);
        for (int i = 0; i < 5; ++i)
            for (int y = 10; y < 16; ++y)
                for (int x = 8 + i * 10; x < 14 + i * 10; ++x) layers[2].at(size_t(x), size_t(y)) = 255;
        return layers;
    };

    RaftReliefParams p;
    p.type = RaftReliefType::Dimming;
    p.low_brightness = 100;
    p.dilate_iterations = 2;
    p.wall_margin = 3;
    auto layers = make();
    REQUIRE(raft_relief_layers(layers, p)); // support layer auto-detected at index 2
    CHECK(layers[0].at(5, 5) == 255);       // wall margin keeps the edge
    CHECK(layers[0].at(30, 30) == 100);     // interior dimmed to the low brightness
    CHECK(layers[0].at(10, 11) == 255);     // under a (dilated) support: untouched
    CHECK(layers[2] == make()[2]);          // supports layer itself untouched

    p.type = RaftReliefType::Decimate;
    layers = make();
    REQUIRE(raft_relief_layers(layers, p));
    CHECK(layers[0].at(30, 30) == 0);
    CHECK(layers[0].at(9, 11) == 255); // support footprint kept

    p.type = RaftReliefType::Relief;
    p.hole_diameter = 10;
    p.hole_spacing = 6;
    p.low_brightness = 0;
    layers = make();
    REQUIRE(raft_relief_layers(layers, p));
    CHECK(layers[0].at(5, 5) == 255);
    bool has_hole = false;
    for (size_t y = 10; y < 55; ++y) for (size_t x = 10; x < 55; ++x) has_hole |= layers[0].at(x, y) == 0;
    CHECK(has_hole);

    // Layer 0 ignored => untouched.
    p.ignore_first_layers = 1;
    layers = make();
    REQUIRE(raft_relief_layers(layers, p));
    CHECK(layers[0] == make()[0]);

    // No supports anywhere: reports false and changes nothing.
    layers.assign(3, block(60, 60, 5, 5, 50, 50));
    p.ignore_first_layers = 0;
    CHECK_FALSE(raft_relief_layers(layers, p));
    CHECK(layers[1] == block(60, 60, 5, 5, 50, 50));

    // Tabs: a big raft gets triangles on its four sides.
    std::vector<Mask> raft(2, block(400, 400, 100, 100, 200, 200));
    raft[1] = Mask(400, 400); // mask layer index 1 (not used by tabs)
    RaftReliefParams t;
    t.type = RaftReliefType::Tabs;
    t.mask_layer_index = 1;
    t.tab_triangle_base = 40;
    t.tab_triangle_height = 40;
    t.high_brightness = 255;
    REQUIRE(raft_relief_layers(raft, t));
    CHECK(raft[0].at(200, 90) == 255);  // tab above the raft
    CHECK(raft[0].at(200, 310) == 255); // below
    CHECK(raft[0].at(90, 200) == 255);  // left
    CHECK(raft[0].at(310, 200) == 255); // right

    RaftReliefParams bad;
    bad.type = RaftReliefType::Tabs;
    bad.tab_triangle_base = 0;
    CHECK_FALSE(validate_raft_relief(bad).empty());
}

TEST_CASE("layerops clone: extra layers and z shifting", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers{Mask(2, 1, 10), Mask(2, 1, 20), Mask(2, 1, 30)};
    std::vector<double> z{0.05, 0.10, 0.15};
    CloneParams p;
    p.first_layer = 1;
    p.last_layer = 1;
    p.clones = 2;
    const LayerListResult r = clone_layers(layers, z, p);
    REQUIRE(layers.size() == 5);
    CHECK(layers[0].px[0] == 10);
    CHECK(layers[1].px[0] == 20);
    CHECK(layers[2].px[0] == 20);
    CHECK(layers[3].px[0] == 20);
    CHECK(layers[4].px[0] == 30);
    CHECK(z == std::vector<double>{0.05, 0.10, 0.15, 0.20, 0.25});
    CHECK(r.source_index == std::vector<size_t>{0, 1, 1, 1, 2});

    // Keep same z: clones share the position and nothing shifts.
    layers = {Mask(2, 1, 10), Mask(2, 1, 20)};
    z = {0.05, 0.10};
    p = CloneParams{};
    p.keep_same_z = true;
    p.last_layer = 0;
    clone_layers(layers, z, p);
    CHECK(z == std::vector<double>{0.05, 0.05, 0.10});

    p.clones = 0;
    CHECK_FALSE(validate_clone(p).empty());
}

TEST_CASE("layerops remove: ranges, threshold and z shifting", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers{Mask(2, 1, 10), Mask(2, 1, 0), Mask(2, 1, 30), Mask(2, 1, 40)};
    std::vector<double> z{0.05, 0.10, 0.15, 0.20};
    RemoveParams p;
    p.first_layer = 1;
    p.last_layer = 2;
    const LayerListResult r = remove_layers(layers, z, p);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].px[0] == 10);
    CHECK(layers[1].px[0] == 40);
    CHECK(z == std::vector<double>{0.05, 0.10});
    CHECK(r.source_index == std::vector<size_t>{0, 3});

    // Threshold: only blank layers (<= 0 px) go.
    layers = {Mask(2, 1, 10), Mask(2, 1, 0), Mask(2, 1, 30)};
    z = {0.05, 0.10, 0.15};
    p = RemoveParams{};
    p.use_threshold = true;
    p.pixel_threshold = 0;
    remove_layers(layers, z, p);
    CHECK(layers.size() == 2);
    CHECK(z == std::vector<double>{0.05, 0.10});

    layers = {Mask(2, 1, 10)};
    z = {0.05};
    p = RemoveParams{};
    CHECK_FALSE(validate_remove(p, layers).empty()); // would remove everything
    p.use_threshold = true;
    CHECK_FALSE(validate_remove(p, layers).empty()); // removes nothing
}

TEST_CASE("layerops reheight: presets, divide, multiply, offset", "[sla_layers][sla_layerops]")
{
    const auto presets = reheight_presets(10, 0.05);
    // Divide by 2..5 (0.025, 0.0166.. is skipped, 0.0125 has 4 decimals) and multiply 2, 4.
    bool has_div2 = false, has_mul2 = false, has_div3 = false;
    for (const auto &s : presets) {
        has_div2 |= !s.is_multiply && s.modifier == 2 && s.layer_count == 20 && std::abs(s.layer_height_mm - 0.025) < 1e-9;
        has_mul2 |= s.is_multiply && s.modifier == 2 && s.layer_count == 5 && std::abs(s.layer_height_mm - 0.1) < 1e-9;
        has_div3 |= !s.is_multiply && s.modifier == 3;
    }
    CHECK(has_div2);
    CHECK(has_mul2);
    CHECK_FALSE(has_div3);

    std::vector<Mask> layers{Mask(2, 1, 100), Mask(2, 1, 200)};
    std::vector<double> z{0.05, 0.10};
    ReHeightParams p;
    p.preset = {false, 2, 0.025, 4};
    LayerListResult r = reheight_layers(layers, z, p);
    REQUIRE(layers.size() == 4);
    CHECK(layers[0].px[0] == 100);
    CHECK(layers[1].px[0] == 100);
    CHECK(layers[2].px[0] == 200);
    CHECK(z == std::vector<double>{0.025, 0.05, 0.075, 0.1});
    CHECK(r.source_index == std::vector<size_t>{0, 0, 1, 1});

    layers = {Mask(2, 1, 100), Mask(2, 1, 100), Mask(2, 1, 10), Mask(2, 1, 20)};
    z = {0.05, 0.10, 0.15, 0.20};
    p.preset = {true, 2, 0.10, 2};
    reheight_layers(layers, z, p);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].px[0] == 200);
    CHECK(layers[1].px[0] == 30);
    CHECK(z == std::vector<double>{0.1, 0.2});

    layers = {Mask(2, 1, 100), Mask(2, 1, 50)};
    z = {0.05, 0.10};
    p.preset = {true, 2, 0.10, 1};
    p.anti_aliasing = ReHeightAntiAliasing::Average;
    reheight_layers(layers, z, p);
    CHECK(layers[0].px[0] == 75);

    // Offset method.
    layers = {Mask(1, 1), Mask(1, 1)};
    z = {0.05, 0.10};
    p = ReHeightParams{};
    p.method = ReHeightMethod::OffsetPositionZ;
    p.position_z_offset_mm = 0.01;
    reheight_layers(layers, z, p);
    CHECK(z == std::vector<double>{0.06, 0.11});
    p.position_z_offset_mm = -1;
    CHECK_FALSE(validate_reheight(p, layers, z).empty());
    p.position_z_offset_mm = 0;
    CHECK_FALSE(validate_reheight(p, layers, z).empty());
}

TEST_CASE("layerops layer arithmetic: parsing and sequential evaluation", "[sla_layers][sla_layerops]")
{
    std::vector<LayerArithmeticGroup> g;
    std::string err;
    REQUIRE(parse_layer_arithmetic("10,11,12 = 11+12-10*5; 3:5=3|4", 20, g, &err));
    REQUIRE(g.size() == 2);
    CHECK(g[0].set_layers == std::vector<size_t>{10, 11, 12});
    REQUIRE(g[0].operations.size() == 4);
    CHECK(g[0].operations[0].layer == 11);
    CHECK(g[0].operations[0].op == '+');
    CHECK(g[0].operations[3].op == '\0');
    CHECK(g[1].set_layers == std::vector<size_t>{3, 4, 5});
    CHECK_FALSE(parse_layer_arithmetic("   ", 20, g, &err));
    CHECK_FALSE(parse_layer_arithmetic("1=", 20, g, &err));

    // Implicit target: "0+1" sets layer 0.
    std::vector<Mask> layers{Mask(2, 1, 100), Mask(2, 1, 50), Mask(2, 1, 0)};
    LayerArithmeticParams p;
    p.sentence = "0+1";
    layer_arithmetic(layers, p);
    CHECK(layers[0].px[0] == 150);
    CHECK(layers[1].px[0] == 50);

    // Sequential: "2 = 0 - 1 + 1" ; then a second group uses the result of the first.
    layers = {Mask(2, 1, 100), Mask(2, 1, 40), Mask(2, 1, 0)};
    p.sentence = "2 = 0-1+1; 1 = 2$0";
    layer_arithmetic(layers, p);
    CHECK(layers[2].px[0] == 100);
    CHECK(layers[1].px[0] == 0); // |100 - 100|

    // Operators.
    layers = {Mask(1, 1, 200), Mask(1, 1, 100), Mask(1, 1, 0)};
    p.sentence = "2=0*1";
    layer_arithmetic(layers, p);
    CHECK(layers[2].px[0] == 78); // 200*100/255
    p.sentence = "2=0/1";
    layer_arithmetic(layers, p);
    CHECK(layers[2].px[0] == 2);
    p.sentence = "2=0&1";
    layer_arithmetic(layers, p);
    CHECK(layers[2].px[0] == (200 & 100));
    p.sentence = "2=0^1";
    layer_arithmetic(layers, p);
    CHECK(layers[2].px[0] == (200 ^ 100));

    // ROI only touches the region.
    layers = {Mask(4, 1, 100), Mask(4, 1, 50), Mask(4, 1, 7)};
    p.sentence = "2=0+1";
    p.roi = {1, 0, 2, 1};
    layer_arithmetic(layers, p);
    CHECK(layers[2].px == std::vector<uint8_t>{7, 150, 150, 7});

    p = LayerArithmeticParams{};
    p.sentence = "0=0";
    CHECK_FALSE(validate_layer_arithmetic(p, 3).empty()); // no impact
}

TEST_CASE("layerops double exposure: layer pairs with erode and difference", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers{block(12, 12, 1, 1, 10, 10), block(12, 12, 1, 1, 10, 10)};
    std::vector<double> z{0.05, 0.10};
    DoubleExposureParams p;
    p.first_layer = 0;
    p.last_layer = 0;
    p.bottom_layer_count = 1;
    p.first_bottom_exposure_s = 30;
    p.second_bottom_exposure_s = 5;
    p.first_normal_exposure_s = 2;
    p.first_bottom_erode = 1;
    p.second_bottom_erode = 0;
    p.second_layer_difference = true;
    p.second_layer_difference_overlap_erode = 0;
    const DoubleExposureResult r = double_exposure_layers(layers, z, p);
    REQUIRE(layers.size() == 3);
    CHECK(z == std::vector<double>{0.05, 0.05, 0.10});
    CHECK(r.list.source_index == std::vector<size_t>{0, 0, 1});
    CHECK(r.is_second == std::vector<bool>{false, true, false});
    CHECK(r.exposure_s[0] == 30);
    CHECK(r.exposure_s[1] == 5);
    CHECK(r.exposure_s[2] < 0);
    CHECK(r.bottom_layer_count == 2);
    // First pass is eroded by 1 px, second is |first - original| = the 1 px ring.
    CHECK(layers[0].at(1, 1) == 0);
    CHECK(layers[0].at(2, 2) == 255);
    CHECK(layers[1].at(1, 1) == 255);
    CHECK(layers[1].at(2, 2) == 0);

    // Equal erode on both passes with difference overlap: second = first minus its inner part.
    layers = {block(12, 12, 1, 1, 10, 10)};
    z = {0.05};
    p = DoubleExposureParams{};
    p.first_normal_erode = p.second_normal_erode = 1;
    p.second_layer_difference = true;
    p.second_layer_difference_overlap_erode = 2;
    double_exposure_layers(layers, z, p);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].at(2, 2) == 255);
    CHECK(layers[1].at(2, 2) == 255);  // outer ring of the eroded layer
    CHECK(layers[1].at(5, 5) == 0);    // inside removed

    // Same z in the range is rejected.
    z = {0.05, 0.05};
    CHECK_FALSE(validate_double_exposure(DoubleExposureParams{}, z).empty());
}

TEST_CASE("layerops change resolution: centre model and fix ratio", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers{block(10, 10, 0, 0, 4, 2)};
    ChangeResolutionParams p;
    p.new_width = 12;
    p.new_height = 6;
    change_resolution_layers(layers, p);
    REQUIRE(layers[0].width == 12);
    REQUIRE(layers[0].height == 6);
    CHECK(to_art(layers[0]) == Rows{"............", "............", "....####....", "....####....", "............", "............"});

    // Cropping to the model bounds is fine, clipping is not.
    layers = {block(10, 10, 0, 0, 4, 2)};
    p.new_width = 3;
    p.new_height = 6;
    CHECK_FALSE(validate_change_resolution(p, layers).empty());
    CHECK_THROWS_AS(change_resolution_layers(layers, p), std::invalid_argument);
    CHECK(layers[0].width == 10);

    // Same resolution is an error.
    p.new_width = 10;
    p.new_height = 10;
    CHECK_FALSE(validate_change_resolution(p, layers).empty());

    CHECK(pixel_pitch_ratio(100, 1000, 100, 2000) == 2.0);
    CHECK(pixel_pitch_ratio(0, 1000, 100, 2000) == 1.0);
}

TEST_CASE("layerops progress and cancellation leave masks untouched", "[sla_layers][sla_layerops]")
{
    std::vector<Mask> layers(6, block(8, 8, 1, 1, 3, 3));
    const std::vector<Mask> before = layers;
    FlipParams p;
    size_t calls = 0, last_total = 0;
    flip_layers(layers, p, [&](size_t, size_t total) { ++calls; last_total = total; return true; });
    CHECK(calls >= 6);
    CHECK(last_total == 6);

    layers = before;
    CHECK_THROWS_AS(flip_layers(layers, p, [](size_t done, size_t) { return done < 2; }), LayerOperationCancelled);
    CHECK(layers == before);

    // Cancel before the first layer.
    CHECK_THROWS_AS(flip_layers(layers, p, [](size_t, size_t) { return false; }), LayerOperationCancelled);
    CHECK(layers == before);

    // Layer list ops cancel cleanly too.
    std::vector<double> z{1, 2, 3, 4, 5, 6};
    CloneParams c;
    CHECK_THROWS_AS(clone_layers(layers, z, c, [](size_t, size_t) { return false; }), LayerOperationCancelled);
    CHECK(layers == before);
    CHECK(z.size() == 6);

    // Range errors.
    p.selection.first_layer = 10;
    CHECK_THROWS_AS(flip_layers(layers, p), std::invalid_argument);
    // Non-uniform layers are rejected.
    std::vector<Mask> mixed{Mask(2, 2), Mask(3, 3)};
    CHECK_THROWS_AS(flip_layers(mixed, FlipParams{}), std::invalid_argument);
}
