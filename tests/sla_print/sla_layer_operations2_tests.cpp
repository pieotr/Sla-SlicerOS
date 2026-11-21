// Tests for the batch 2 UVtools ports: per-layer parameter operations (LayerOperations.hpp), the calculator and
// the layer exports (LayerExports.hpp). All data is synthetic with hand computed expectations.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLA/LayerExports.hpp"
#include "libslic3r/SLA/LayerOperations.hpp"

using namespace Slic3r::sla;
using namespace Slic3r::sla::layerops;
namespace lx = Slic3r::sla::layerexport;
using Catch::Approx;

namespace {

Mask block(size_t w, size_t h, int x0, int y0, int bw, int bh, uint8_t v = 255)
{
    Mask m(w, h);
    for (int y = y0; y < y0 + bh; ++y)
        for (int x = x0; x < x0 + bw; ++x) m.at(size_t(x), size_t(y)) = v;
    return m;
}

Mask row(const std::vector<int> &v)
{
    Mask m(v.size(), 1);
    for (size_t i = 0; i < v.size(); ++i) m.px[i] = uint8_t(v[i]);
    return m;
}

std::vector<int> as_ints(const Mask &m) { return std::vector<int>(m.px.begin(), m.px.end()); }

std::vector<double> z_steps(size_t n, double h)
{
    std::vector<double> z;
    for (size_t i = 0; i < n; ++i) z.push_back(std::round(h * double(i + 1) * 1000.0) / 1000.0);
    return z;
}

std::vector<LayerParams> normal_table(size_t n, double exposure = 2.0)
{
    std::vector<LayerParams> t(n);
    for (LayerParams &p : t) { p.exposure_s = exposure; p.lift_height_mm = 5; p.lift_speed = 60; p.retract_speed = 120; }
    return t;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Per-layer parameter operations
// ---------------------------------------------------------------------------------------------------------

TEST_CASE("dynamic lifts: traditional and full range follow the previous layer area", "[sla_layers][sla_layerops2]")
{
    // Areas 100, 50, 25 px. UVtools drives layer i by the area of layer i-1 (layer 0 by itself).
    const std::vector<Mask> layers{block(20, 20, 0, 0, 10, 10), block(20, 20, 0, 0, 10, 5), block(20, 20, 0, 0, 5, 5)};
    DynamicLiftsParams p;
    p.smallest_lift_height = 2; p.largest_lift_height = 6;
    p.slowest_lift_speed = 60;  p.fastest_lift_speed = 180;
    p.smallest_bottom_lift_height = p.largest_bottom_lift_height = 3;
    p.slowest_bottom_lift_speed = p.fastest_bottom_lift_speed = 100;

    std::vector<LayerParams> table = normal_table(3);
    dynamic_lifts(layers, table, p);
    CHECK(table[0].lift_height_total() == Approx(6.0));  CHECK(table[0].lift_speed == Approx(60.0));
    CHECK(table[1].lift_height_total() == Approx(6.0));  CHECK(table[1].lift_speed == Approx(60.0));
    CHECK(table[2].lift_height_total() == Approx(3.0));  CHECK(table[2].lift_speed == Approx(90.0)); // 6*50/100, 180-180*0.5

    p.method = DynamicLiftsMethod::FullRange; // ratio of layer 1 = (50-25)/(100-25)
    table = normal_table(3);
    dynamic_lifts(layers, table, p);
    CHECK(table[0].lift_height_total() == Approx(6.0));
    CHECK(table[2].lift_height_total() == Approx(3.3));  // 2 + 4/3 rounded to 1 decimal
    CHECK(table[2].lift_speed == Approx(140.0));         // 180 - 120/3

    // Bottom layers use the bottom limits (Traditional: FullRange with a single bottom layer is 0/0 = NaN and leaves the layer alone).
    p.method = DynamicLiftsMethod::Traditional;
    table = normal_table(3);
    table[0].is_bottom = true;
    dynamic_lifts(layers, table, p);
    CHECK(table[0].lift_height_total() == Approx(3.0));
    CHECK(table[0].lift_speed == Approx(100.0));

    // Validation + cancel keeps the table.
    DynamicLiftsParams bad = p;
    bad.smallest_lift_height = 9;
    CHECK_FALSE(validate_dynamic_lifts(bad).empty());
    table = normal_table(3);
    const auto before = table;
    CHECK_THROWS_AS(dynamic_lifts(layers, table, p, [](size_t done, size_t) { return done < 1; }), LayerOperationCancelled);
    CHECK(table == before);

    // Defaults come from the first bottom / normal layer of the table.
    table = normal_table(3);
    const DynamicLiftsParams d = dynamic_lifts_defaults(table);
    CHECK(d.smallest_lift_height == Approx(5.0));
    CHECK(d.fastest_lift_speed == Approx(60.0));
    CHECK_FALSE(validate_dynamic_lifts(d).empty()); // all equal -> "will not produce a change"
}

TEST_CASE("fade exposure time: increments and transition layer count", "[sla_layers][sla_layerops2]")
{
    CHECK(transition_step_time(3.0, 1.0, 4) == Approx(0.4));
    CHECK(transition_step_time(3.0, 3.0, 4) == 0.0);
    CHECK(transition_step_time(3.0, 1.0, 0) == 0.0);

    std::vector<LayerParams> table = normal_table(6);
    FadeExposureParams p;
    p.first_layer = 1; p.layer_count = 4; p.from_exposure_s = 1.0; p.to_exposure_s = 3.0; p.bottom_layer_count = 1;
    const FadeExposureResult r = fade_exposure_time(table, p);
    CHECK(r.increment_s == Approx(0.4));
    CHECK(table[0].exposure_s == Approx(2.0)); // untouched
    CHECK(table[1].exposure_s == Approx(1.4));
    CHECK(table[2].exposure_s == Approx(1.8));
    CHECK(table[3].exposure_s == Approx(2.2));
    CHECK(table[4].exposure_s == Approx(2.6));
    CHECK(table[5].exposure_s == Approx(2.0));
    CHECK(r.transition_layer_count_changed);
    CHECK(r.transition_layer_count == 4);

    p.transition_type_firmware = true; // firmware transition layers are switched off
    const FadeExposureResult r2 = fade_exposure_time(table, p);
    CHECK(r2.transition_layer_count_changed);
    CHECK(r2.transition_layer_count == 0);

    p.from_exposure_s = 3.0;
    CHECK_FALSE(validate_fade_exposure(p, 6).empty());
    p.from_exposure_s = 1.0; p.layer_count = 10;
    CHECK_THROWS_AS(fade_exposure_time(table, p), std::invalid_argument); // range beyond the layers
}

TEST_CASE("light bleed compensation: lookup and subject modes", "[sla_layers][sla_layerops2]")
{
    const std::vector<Mask> original{row({255, 255, 255, 255, 255, 255}), row({255, 255, 255, 0, 0, 0}), row({255, 0, 0, 0, 0, 0})};
    LightBleedParams p;
    p.dim_by = "100,50";

    auto layers = original;
    light_bleed_compensation_layers(layers, p); // Similarities, Next
    CHECK(as_ints(layers[0]) == std::vector<int>{105, 155, 155, 255, 255, 255});
    CHECK(as_ints(layers[1]) == std::vector<int>{155, 255, 255, 0, 0, 0});
    CHECK(as_ints(layers[2]) == as_ints(original[2]));

    layers = original;
    p.subject = LightBleedSubject::Bridges; p.dim_by = "100";
    light_bleed_compensation_layers(layers, p);
    CHECK(as_ints(layers[0]) == std::vector<int>{255, 255, 255, 155, 155, 155});
    CHECK(as_ints(layers[1]) == std::vector<int>{255, 155, 155, 0, 0, 0});

    layers = original;
    p.subject = LightBleedSubject::Both;
    light_bleed_compensation_layers(layers, p); // every pixel gets dimmed once per level
    CHECK(as_ints(layers[0]) == std::vector<int>{155, 155, 155, 155, 155, 155});

    layers = original;
    p.subject = LightBleedSubject::Similarities; p.lookup = LightBleedLookup::Previous;
    light_bleed_compensation_layers(layers, p);
    CHECK(as_ints(layers[0]) == as_ints(original[0])); // nothing before layer 0
    CHECK(as_ints(layers[1]) == std::vector<int>{155, 155, 155, 0, 0, 0});
    CHECK(as_ints(layers[2]) == std::vector<int>{155, 0, 0, 0, 0, 0});

    CHECK(parse_dim_by("25, 15,x,0,255,300,5") == std::vector<uint8_t>{25, 15, 5});
    LightBleedParams bad; bad.dim_by = "200,100";
    CHECK_FALSE(validate_light_bleed(bad).empty());
    bad.dim_by = "abc";
    CHECK_FALSE(validate_light_bleed(bad).empty());

    // ROI: only the region changes.
    layers = original; p.lookup = LightBleedLookup::Next; p.selection.roi = Rect{0, 0, 2, 1};
    light_bleed_compensation_layers(layers, p);
    CHECK(as_ints(layers[0]) == std::vector<int>{155, 155, 255, 255, 255, 255}); // x2 lies outside the ROI
}

TEST_CASE("phased exposure: eroded first pass, difference only second pass", "[sla_layers][sla_layerops2]")
{
    std::vector<Mask> layers{block(20, 20, 6, 6, 8, 8)};
    std::vector<double> z{0.05};
    std::vector<LayerParams> table = normal_table(1);
    PhasedExposureParams p;
    p.phases = {{10.0, 3, 3.0, 2}, {2.0, 0, 2.0, 0}};
    p.difference_overlap_iterations = 1;
    p.sequential_lift_mm = 0.1;
    p.sequential_wait_before_cure_s = 0;

    auto L = layers; auto Z = z; auto T = table;
    const PhasedExposureResult r = phased_exposure_layers(L, Z, T, p, PrinterContext{});
    REQUIRE(L.size() == 2);
    CHECK(L[0].nonzero_count() == 16);       // 8x8 eroded by 2
    CHECK(L[1].nonzero_count() == 60);       // 8x8 minus the 2x2 core that eroding by (1 overlap + 2) leaves
    CHECK(Z == std::vector<double>{0.05, 0.05});
    CHECK(T[0].exposure_s == Approx(3.0));   CHECK(T[1].exposure_s == Approx(2.0));
    CHECK(T[0].lift_height_total() == Approx(5.0));
    CHECK(T[1].lift_height_total() == Approx(0.1)); // sequential lift
    CHECK(r.list.source_index == std::vector<size_t>{0, 0});
    CHECK(r.bottom_layer_count == 0);

    // A bottom layer uses the bottom iterations / exposure; its difference pass is empty so UVtools keeps the
    // clone's original mask (full layer).
    L = layers; Z = z; T = table; T[0].is_bottom = true;
    const PhasedExposureResult rb = phased_exposure_layers(L, Z, T, p, PrinterContext{});
    CHECK(L[0].nonzero_count() == 4);        // eroded by 3
    CHECK(L[1].nonzero_count() == 64);
    CHECK(T[0].exposure_s == Approx(10.0));
    CHECK(rb.bottom_layer_count == 2);       // clones inherit the bottom flag

    // Layers outside the range are kept in order and trailing layers follow.
    L = {layers[0], layers[0], layers[0]}; Z = {0.05, 0.1, 0.15}; T = normal_table(3);
    p.first_layer = 1; p.last_layer = 1;
    phased_exposure_layers(L, Z, T, p, PrinterContext{});
    CHECK(L.size() == 4);
    CHECK(Z == std::vector<double>{0.05, 0.1, 0.1, 0.15});

    // Validation.
    PhasedExposureParams one; one.phases = {{1, 0, 1, 0}};
    CHECK_FALSE(validate_phased_exposure(one, z).empty());
    PhasedExposureParams whole = p; whole.first_layer = 0; whole.last_layer = kLastLayer;
    CHECK_FALSE(validate_phased_exposure(whole, {0.05, 0.05, 0.1}).empty()); // same Z inside the range
}

TEST_CASE("raise on print finish appends a dummy layer", "[sla_layers][sla_layerops2]")
{
    std::vector<Mask> layers{block(10, 10, 3, 2, 4, 4), block(10, 10, 3, 2, 4, 4), block(10, 10, 3, 2, 4, 4)};
    std::vector<double> z{0.05, 0.1, 0.15};
    std::vector<LayerParams> table = normal_table(3);
    PrinterContext ctx; ctx.machine_z_mm = 10;
    RaiseOnPrintFinishParams p;
    REQUIRE(validate_raise_on_print_finish(p, z, ctx).empty());
    raise_on_print_finish(layers, z, table, p, ctx);
    REQUIRE(layers.size() == 4);
    CHECK(z.back() == Approx(10.0));
    CHECK(table.back().exposure_s == Approx(0.05));
    CHECK(table.back().light_pwm == 0);
    CHECK(table.back().wait_before_cure_s == Approx(1200.0));
    CHECK(table.back().wait_after_lift_s == 0.0);
    CHECK(layers.back().nonzero_count() == 1);
    CHECK(layers.back().at(3, 2) == 128); // first pixel of the last model layer

    CHECK_FALSE(validate_raise_on_print_finish(p, z, ctx).empty()); // now the last two layers are 9.85 mm apart
    RaiseOnPrintFinishParams low; low.position_z_mm = 0.1;
    CHECK_FALSE(validate_raise_on_print_finish(low, {0.05, 0.1, 0.15}, ctx).empty());
    low.position_z_mm = 0.15;
    CHECK_FALSE(validate_raise_on_print_finish(low, {0.05, 0.1, 0.15}, ctx).empty());

    PrinterContext gcode = ctx; gcode.is_gcode = true;
    layers = {block(10, 10, 3, 2, 4, 4)}; z = {0.05}; table = normal_table(1);
    p.output_dummy_pixel = false;
    raise_on_print_finish(layers, z, table, p, gcode);
    CHECK(layers.back().nonzero_count() == 0);
    CHECK(table.back().exposure_s == 0.0);
}

TEST_CASE("edit parameters: per layer pattern and global bottom / normal", "[sla_layers][sla_layerops2]")
{
    std::vector<double> z = z_steps(6, 0.05);
    std::vector<LayerParams> table = normal_table(6);
    table[0].is_bottom = true;

    EditParametersParams p;
    p.per_layer_override = true; p.skip_layers = 1; p.set_layers = 1;
    p.edit.exposure_s = 9.0; p.edit.light_pwm = 100;
    edit_parameters(z, table, p);
    CHECK(table[0].exposure_s == Approx(9.0)); CHECK(table[1].exposure_s == Approx(2.0));
    CHECK(table[2].exposure_s == Approx(9.0)); CHECK(table[3].exposure_s == Approx(2.0));
    CHECK(table[4].exposure_s == Approx(9.0)); CHECK(table[5].exposure_s == Approx(2.0));
    CHECK(table[4].light_pwm == 100);

    // Two on, one off over layers 1..5.
    table = normal_table(6);
    p.first_layer = 1; p.last_layer = 5; p.set_layers = 2; p.skip_layers = 1;
    p.edit = {}; p.edit.wait_after_cure_s = 1.234;
    edit_parameters(z, table, p);
    std::vector<int> changed;
    for (size_t i = 0; i < 6; ++i) if (table[i].wait_after_cure_s > 0) changed.push_back(int(i));
    CHECK(changed == std::vector<int>{1, 2, 4, 5});
    CHECK(table[1].wait_after_cure_s == Approx(1.23)); // rounded to 2 decimals like the UVtools setters

    // Global edit: bottom layers get `bottom`, the others `edit`.
    table = normal_table(6); table[0].is_bottom = true;
    EditParametersParams g;
    g.edit.exposure_s = 2.5; g.bottom.exposure_s = 20.0;
    edit_parameters(z, table, g);
    CHECK(table[0].exposure_s == Approx(20.0)); CHECK(table[3].exposure_s == Approx(2.5));

    // Z edit + validation.
    EditParametersParams zed;
    zed.per_layer_override = true; zed.first_layer = 2; zed.last_layer = 2; zed.edit.position_z_mm = 1.5;
    edit_parameters(z, table, zed);
    CHECK(z[2] == Approx(1.5));
    EditParametersParams nothing;
    CHECK_FALSE(validate_edit_parameters(nothing).empty());
    EditParametersParams alt; alt.per_layer_override = true; alt.skip_layers = 1; alt.edit.position_z_mm = 1;
    alt.first_layer = 0; alt.last_layer = 9;
    CHECK_FALSE(validate_edit_parameters(alt).empty()); // Z can't alternate
}

TEST_CASE("timelapse: lift height mode and virtual layers", "[sla_layers][sla_layerops2]")
{
    std::vector<Mask> layers(10, block(8, 8, 2, 2, 3, 3));
    const std::vector<double> z0 = z_steps(10, 0.1);
    PrinterContext ctx; ctx.layer_height_mm = 0.1; ctx.machine_z_mm = 10; ctx.can_use_lift_height2 = false;
    TimelapseParams p;
    p.raise_each_nth_height_mm = 0.3;
    CHECK(timelapse_lift_count(p, z0, ctx) == 3);

    {
        auto L = layers; auto Z = z0; auto T = normal_table(10);
        const TimelapseResult r = timelapse_layers(L, Z, T, p, ctx);
        REQUIRE(L.size() == 10);
        for (size_t i = 0; i < 10; ++i) {
            const bool lifted = i == 3 || i == 6 || i == 9;
            INFO("layer " << i);
            if (!lifted) CHECK(T[i].lift_height_total() == Approx(5.0));
            else CHECK(T[i].wait_after_lift_s == Approx(1.0));
        }
        CHECK(T[3].lift_height_total() == Approx(9.6));
        CHECK(T[6].lift_height_total() == Approx(9.3));
        CHECK(T[9].lift_height_total() == Approx(9.0));
        CHECK(r.inserted == std::vector<bool>(10, false));
    }
    {
        p.mode = TimelapseMode::VirtualLayer;
        auto L = layers; auto Z = z0; auto T = normal_table(10);
        const TimelapseResult r = timelapse_layers(L, Z, T, p, ctx);
        REQUIRE(L.size() == 16);                    // 3 photo layers + 3 slow lift layers
        CHECK(std::count(r.inserted.begin(), r.inserted.end(), true) == 6);
        CHECK(Z[3] == Approx(5.3)); CHECK(Z[4] == Approx(10.0)); CHECK(Z[5] == Approx(0.4)); // slow, photo, real layer
        CHECK(Z[8] == Approx(5.6)); CHECK(Z[9] == Approx(10.0)); CHECK(Z[10] == Approx(0.7));
        CHECK(Z[14] == Approx(6.0)); CHECK(Z[15] == Approx(10.0));
        CHECK(T[4].exposure_s == Approx(0.05));
        CHECK(T[3].light_pwm == 0);
        CHECK(T[5].lift_speed == Approx(200.0));    // real layer after a photo lifts "down" with the fast speed
        CHECK(r.list.source_index.size() == 16);
    }
    TimelapseParams none; none.raise_each_nth_height_mm = 100; none.ensure_last_layer = false; none.mode = TimelapseMode::LiftHeight;
    CHECK(timelapse_lift_count(none, z0, ctx) == 0);
    CHECK_FALSE(validate_timelapse(none, z0, ctx).empty());
}

TEST_CASE("dynamic layer height: identical layers stack, different layers stay", "[sla_layers][sla_layerops2]")
{
    PrinterContext ctx; ctx.layer_height_mm = 0.05;
    DynamicLayerHeightParams p;
    p.minimum_layer_height_mm = 0.05; p.maximum_layer_height_mm = 0.15;
    p.bottom_exposure_s = 10; p.exposure_s = 2; p.exposure_step_s = 0.2;

    const std::vector<ExposureItem> tbl = dynamic_layer_height_exposure_table(p, 0.05);
    REQUIRE(tbl.size() == 3);
    CHECK(tbl[2].layer_height_mm == Approx(0.15));
    CHECK(tbl[2].exposure_s == Approx(2.4));
    CHECK(tbl[2].bottom_exposure_s == Approx(10.0));

    {
        std::vector<Mask> layers(6, block(16, 16, 4, 4, 8, 8));
        std::vector<double> z = z_steps(6, 0.05);
        std::vector<LayerParams> table = normal_table(6);
        const DynamicLayerHeightResult r = dynamic_layer_height_layers(layers, z, table, p, ctx);
        REQUIRE(layers.size() == 2);
        CHECK(z == std::vector<double>{0.15, 0.3});
        CHECK(r.list.source_index == std::vector<size_t>{0, 3});
        CHECK(table[0].exposure_s == Approx(2.4));
        CHECK(r.report.old_layer_count == 6);  CHECK(r.report.new_layer_count == 2);
        CHECK(r.report.stacked_layers == 6);
        CHECK(r.report.maximum_layer_height_mm == Approx(0.15));
        CHECK(r.report.compression_ratio_percent() == Approx(300.0));
    }
    {
        // A, B, B, B with different A: A stays alone, the three B layers stack to 0.15 mm.
        std::vector<Mask> layers{block(16, 16, 0, 0, 8, 8), block(16, 16, 8, 8, 8, 8), block(16, 16, 8, 8, 8, 8), block(16, 16, 8, 8, 8, 8)};
        std::vector<double> z = z_steps(4, 0.05);
        std::vector<LayerParams> table = normal_table(4);
        const DynamicLayerHeightResult r = dynamic_layer_height_layers(layers, z, table, p, ctx);
        REQUIRE(layers.size() == 2);
        CHECK(z == std::vector<double>{0.05, 0.2});
        CHECK(table[0].exposure_s == Approx(2.0));
        CHECK(table[1].exposure_s == Approx(2.4));
        CHECK(r.list.source_index == std::vector<size_t>{0, 1});
    }
    {
        // Cancel leaves the inputs alone; modified Z positions are rejected.
        std::vector<Mask> layers(6, block(16, 16, 4, 4, 8, 8));
        std::vector<double> z = z_steps(6, 0.05);
        std::vector<LayerParams> table = normal_table(6);
        const auto l0 = layers; const auto z0 = z;
        CHECK_THROWS_AS(dynamic_layer_height_layers(layers, z, table, p, ctx, [](size_t done, size_t) { return done < 2; }), LayerOperationCancelled);
        CHECK(layers == l0); CHECK(z == z0);
        z[3] += 0.01;
        CHECK_FALSE(validate_dynamic_layer_height(p, z, ctx).empty());
    }
}

TEST_CASE("lithophane: threshold stack, packing and base", "[sla_layers][sla_layerops2]")
{
    // 4x2 picture: a black column on the left is cropped away, then 3x2 pixels with levels 64 / 128 / 255.
    Mask image(4, 2);
    for (size_t y = 0; y < 2; ++y) { image.at(1, y) = 64; image.at(2, y) = 128; image.at(3, y) = 255; }
    LithophaneParams p;
    p.layer_height_mm = 0.05; p.bottom_layer_count = 2; p.bottom_exposure_s = 30; p.normal_exposure_s = 2;
    p.start_threshold = 60; p.end_threshold = 130;
    p.one_layer_per_threshold = true; p.anti_aliasing = false; p.base_type = LithophaneBase::None;
    PrinterContext ctx;

    REQUIRE(validate_lithophane(p, image, 6, 4).empty());
    const Mask target = lithophane_target_image(image, p, ctx);
    CHECK(target.width == 3); CHECK(target.height == 2);

    std::vector<Mask> layers; std::vector<double> z; std::vector<LayerParams> table;
    lithophane_layers(image, 6, 4, p, ctx, layers, z, table);
    REQUIRE(layers.size() == 70);                    // thresholds 60 .. 129
    CHECK(layers[0].nonzero_count() == 6);           // all three levels are brighter than 60
    CHECK(layers[4].nonzero_count() == 4);           // threshold 64: the 64 column drops out
    CHECK(layers[68].nonzero_count() == 2);          // threshold 128: only the 255 column
    CHECK(layers[0].at(1, 1) == 255);                // centred on the 6x4 canvas
    CHECK(layers[0].at(0, 0) == 0);
    CHECK(z.back() == Approx(3.5));
    CHECK(table[0].is_bottom); CHECK(table[0].exposure_s == Approx(30.0)); CHECK(table[2].exposure_s == Approx(2.0));

    // Target height 0.5 mm: 7 grey levels are merged into one layer (maximum).
    p.one_layer_per_threshold = false; p.lithophane_height_mm = 0.5;
    lithophane_layers(image, 6, 4, p, ctx, layers, z, table);
    CHECK(layers.size() == 10);
    CHECK(layers[0].nonzero_count() == 6);

    // Square base: 2 layers, rectangle = picture + 2 px margin, clipped to the canvas.
    p.one_layer_per_threshold = true; p.base_type = LithophaneBase::Square; p.base_thickness_mm = 0.1; p.base_margin_px = 2;
    lithophane_layers(image, 6, 4, p, ctx, layers, z, table);
    REQUIRE(layers.size() == 72);
    CHECK(layers[0].nonzero_count() == 20);          // 5 x 4
    CHECK(layers[2].nonzero_count() == 6);

    // Flat (no grey separation): lithophane_height / layer_height copies.
    p.separate_grayscale_pixels = false; p.base_type = LithophaneBase::None; p.lithophane_height_mm = 0.25;
    lithophane_layers(image, 6, 4, p, ctx, layers, z, table);
    CHECK(layers.size() == 5);
    CHECK(layers[0].at(1, 1) == 64); CHECK(layers[0].at(3, 1) == 255);

    // Errors.
    p.separate_grayscale_pixels = true; p.start_threshold = 200; p.end_threshold = 100;
    CHECK_FALSE(validate_lithophane(p, image, 6, 4).empty());
    p.start_threshold = 60; p.end_threshold = 130;
    CHECK_FALSE(validate_lithophane(p, image, 2, 2).empty());      // does not fit
    CHECK_FALSE(validate_lithophane(p, Mask(4, 2), 6, 4).empty()); // blank picture
    p.start_threshold = 250; p.end_threshold = 254;                  // nothing brighter than those levels except 255 -> 4 layers
    lithophane_layers(image, 6, 4, p, ctx, layers, z, table);
    CHECK(layers.size() > 0);
}

TEST_CASE("repair layers: islands, resin traps, gap closing and empty layers", "[sla_layers][sla_layerops2]")
{
    RepairLayersParams p;
    p.gap_closing_iterations = 0;
    p.attach_islands_below_layers = 0;
    p.repair_resin_traps = false;
    p.remove_empty_layers = false;
    p.detection.island.enhanced_detection = false;

    // A 2 px island floats at layer 3 above the block; it is removed and the block stays.
    {
        std::vector<Mask> layers(5, block(14, 14, 2, 2, 8, 8));
        layers[3].at(12, 12) = 255; layers[3].at(12, 11) = 255;
        std::vector<double> z = z_steps(5, 0.05);
        const RepairLayersResult r = repair_layers(layers, z, p);
        CHECK(r.islands_removed == 1);
        CHECK(layers[3].nonzero_count() == 64);
        CHECK(layers[3].at(12, 12) == 0);
        for (size_t i : {0u, 1u, 2u, 4u}) CHECK(layers[i].nonzero_count() == 64);
    }
    // Islands bigger than the limit stay; cancel leaves the input untouched.
    {
        std::vector<Mask> layers(5, block(14, 14, 2, 2, 8, 8));
        for (int x = 11; x < 14; ++x) for (int y = 11; y < 14; ++y) layers[3].at(size_t(x), size_t(y)) = 255; // 9 px
        std::vector<double> z = z_steps(5, 0.05);
        const auto copy = layers;
        const RepairLayersResult r = repair_layers(layers, z, p);
        CHECK(r.islands_removed == 0);
        CHECK(layers == copy);
        p.remove_islands_below_equal_pixels = 20;
        CHECK_THROWS_AS(repair_layers(layers, z, p, [](size_t, size_t) { return false; }), LayerOperationCancelled);
        CHECK(layers == copy);
        repair_layers(layers, z, p);
        CHECK(layers[3].nonzero_count() == 64);
        p.remove_islands_below_equal_pixels = 5;
    }
    // Empty layers inside the range are removed and the Z of the following layers is pulled down.
    {
        std::vector<Mask> layers{block(14, 14, 2, 2, 8, 8), block(14, 14, 2, 2, 8, 8), Mask(14, 14), block(14, 14, 2, 2, 8, 8)};
        std::vector<double> z = z_steps(4, 0.05);
        p.remove_empty_layers = true;
        const RepairLayersResult r = repair_layers(layers, z, p);
        CHECK(layers.size() == 3);
        CHECK(r.empty_layers_removed == 1);
        CHECK(z == std::vector<double>{0.05, 0.1, 0.15});
        CHECK(r.list.source_index == std::vector<size_t>{0, 1, 3});
        p.remove_empty_layers = false;
    }
    // Resin trap: the enclosed hole of layers 1-2 is filled.
    {
        std::vector<Mask> layers;
        layers.push_back(block(16, 16, 2, 2, 12, 12));
        for (int i = 0; i < 2; ++i) { Mask m = block(16, 16, 2, 2, 12, 12); for (int y = 6; y < 10; ++y) for (int x = 6; x < 10; ++x) m.at(size_t(x), size_t(y)) = 0; layers.push_back(m); }
        layers.push_back(block(16, 16, 2, 2, 12, 12));
        std::vector<double> z = z_steps(4, 0.05);
        RepairLayersParams t; t.repair_islands = false; t.repair_resin_traps = true; t.remove_empty_layers = false; t.resin_traps_overlap_by = 0;
        t.detection.resin_trap.detect_suction_cups = false;
        const RepairLayersResult r = repair_layers(layers, z, t);
        CHECK(r.resin_traps_filled == 2);
        CHECK(layers[1].nonzero_count() == 144);
        CHECK(layers[2].nonzero_count() == 144);
    }
    // Gap closing runs together with island repair when something was detected.
    {
        std::vector<Mask> layers(4, block(14, 14, 2, 2, 8, 8));
        layers[1].at(5, 5) = 0;                              // one dead pixel in the middle
        layers[3].at(12, 12) = 255;                          // island so that the repair runs at all
        std::vector<double> z = z_steps(4, 0.05);
        RepairLayersParams c = p; c.gap_closing_iterations = 1;
        repair_layers(layers, z, c);
        CHECK(layers[1].at(5, 5) == 255);
        CHECK(layers[3].at(12, 12) == 0);
    }
    CHECK_FALSE(validate_repair_layers([] { RepairLayersParams x; x.repair_islands = x.repair_resin_traps = x.repair_suction_cups = x.remove_empty_layers = false; return x; }()).empty());
}

TEST_CASE("calculator: pixels, light-off delay, tilt and print time", "[sla_layers][sla_layerops2]")
{
    const PixelsPerMillimeter ppm = pixels_per_millimeter(1920, 1080, 120.0, 67.5);
    CHECK(ppm.x == Approx(16.0)); CHECK(ppm.y == Approx(16.0));
    const PixelsResult px = mm_to_pixels(1920, 1080, 120.0, 67.5, 2.0);
    CHECK(px.x == Approx(32.0)); CHECK(px.y == Approx(32.0));
    CHECK(mm_to_pixels(1920, 1080, 0, 0, 2.0).x == 0.0);

    CHECK(light_off_delay_seconds(5.0, 60.0, 120.0, 2.5) == Approx(10.0)); // 5 s up + 2.5 s down + 2.5 s wait
    CHECK(light_off_delay_seconds(5.0, 0.0, 120.0, 0.0) == Approx(2.5));
    // Two stage lift: 1 mm @30 + 4 mm @120 up, 3 mm @60 + the remaining 2 mm @120 down.
    CHECK(light_off_delay_seconds(1.0, 30.0, 120.0, 0.0, 4.0, 120.0, 3.0, 60.0) == Approx(2.0 + 2.0 + 3.0 + 1.0));
    LayerParams l; l.lift_height_mm = 5; l.lift_speed = 60; l.retract_speed = 120;
    CHECK(light_off_delay_seconds(l, 1.0) == Approx(8.5));
    CHECK(lift_only_seconds(l, 0.0) == Approx(5.0));

    CHECK(xy_resolution_mm(1920, 1080, 120.0, 67.5) == Approx(0.0625));
    CHECK(optimal_model_tilt_degrees(1920, 1080, 120.0, 67.5, 0.05) == Approx(38.66).margin(0.0006));
    CHECK(optimal_model_tilt_degrees(1920, 1080, 0, 0, 0.05) == 0.0);

    l.exposure_s = 2; l.wait_before_cure_s = 1; l.wait_after_cure_s = 0.5; l.wait_after_lift_s = 0.25;
    CHECK(layer_print_time_s(l) == Approx(1 + 2 + 0.5 + 0.25 + 7.5));
    CHECK(estimate_print_time_s({l, l}) == Approx(2 * 11.25));
}

// ---------------------------------------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------------------------------------

namespace {

struct DecodedGif {
    uint16_t width = 0, height = 0, loops = 0xFFFF;
    std::vector<uint16_t> delays;
    std::vector<std::vector<uint8_t>> frames; // palette indices
    std::vector<uint8_t> palette;              // rgb
    bool ok = false;
};

// Minimal GIF decoder: global palette, extensions, LZW. Enough to check the encoder bit-exactly.
DecodedGif decode_gif(const std::vector<uint8_t> &d)
{
    DecodedGif g;
    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= d.size(); };
    auto u16 = [&](size_t at) { return uint16_t(d[at] | (d[at + 1] << 8)); };
    if (d.size() < 13 || std::string(d.begin(), d.begin() + 6) != "GIF89a") return g;
    g.width = u16(6); g.height = u16(8);
    const uint8_t flags = d[10];
    pos = 13;
    if (flags & 0x80) {
        const size_t n = size_t(3) << ((flags & 7) + 1);
        if (!need(n)) return g;
        g.palette.assign(d.begin() + long(pos), d.begin() + long(pos + n));
        pos += n;
    }
    uint16_t pending_delay = 0;
    while (need(1)) {
        const uint8_t b = d[pos++];
        if (b == 0x3B) { g.ok = true; return g; }
        if (b == 0x21) {
            if (!need(1)) return g;
            const uint8_t label = d[pos++];
            std::vector<uint8_t> data;
            while (need(1)) {
                const uint8_t n = d[pos++];
                if (n == 0) break;
                if (!need(n)) return g;
                data.insert(data.end(), d.begin() + long(pos), d.begin() + long(pos + n));
                pos += n;
            }
            if (label == 0xF9 && data.size() >= 4) pending_delay = uint16_t(data[1] | (data[2] << 8));
            if (label == 0xFF && data.size() >= 14 && std::string(data.begin(), data.begin() + 11) == "NETSCAPE2.0") g.loops = uint16_t(data[12] | (data[13] << 8));
            continue;
        }
        if (b != 0x2C || !need(9)) return g;
        const size_t w = u16(pos + 4), h = u16(pos + 6);
        pos += 9;
        if (!need(1)) return g;
        const int min_code = d[pos++];
        std::vector<uint8_t> lzw;
        while (need(1)) {
            const uint8_t n = d[pos++];
            if (n == 0) break;
            if (!need(n)) return g;
            lzw.insert(lzw.end(), d.begin() + long(pos), d.begin() + long(pos + n));
            pos += n;
        }
        // LZW decode.
        std::vector<uint8_t> out;
        const unsigned clear = 1u << min_code, eoi = clear + 1;
        std::vector<std::vector<uint8_t>> dict;
        auto reset = [&] { dict.assign(eoi + 1, {}); for (unsigned i = 0; i < clear; ++i) dict[i] = {uint8_t(i)}; };
        reset();
        int size = min_code + 1;
        uint64_t bits = 0; int nbits = 0; size_t bi = 0;
        std::vector<uint8_t> prev; bool have_prev = false;
        while (true) {
            while (nbits < size && bi < lzw.size()) { bits |= uint64_t(lzw[bi++]) << nbits; nbits += 8; }
            if (nbits < size) return g; // truncated
            const unsigned code = unsigned(bits & ((1u << size) - 1));
            bits >>= size; nbits -= size;
            if (code == clear) { reset(); size = min_code + 1; have_prev = false; continue; }
            if (code == eoi) break;
            std::vector<uint8_t> entry;
            if (code < dict.size()) entry = dict[code];
            else if (code == dict.size() && have_prev) { entry = prev; entry.push_back(prev[0]); }
            else return g; // corrupt
            out.insert(out.end(), entry.begin(), entry.end());
            if (have_prev && dict.size() < 4096) {
                std::vector<uint8_t> add = prev; add.push_back(entry[0]);
                dict.push_back(std::move(add));
                if (dict.size() == (1u << size) && size < 12) ++size;
            }
            prev = entry; have_prev = true;
        }
        if (out.size() != w * h) return g;
        g.frames.push_back(std::move(out));
        g.delays.push_back(pending_delay);
    }
    return g;
}

} // namespace

TEST_CASE("export image: PNG round trip, names, ROI and orientation", "[sla_layers][sla_layerexport]")
{
    Mask m(5, 3);
    for (size_t i = 0; i < m.px.size(); ++i) m.px[i] = uint8_t(i * 17);
    const std::vector<uint8_t> png = lx::encode_png(m);
    REQUIRE(!png.empty());
    ::Slic3r::png::ImageGreyscale decoded;
    REQUIRE(::Slic3r::png::decode_png(::Slic3r::png::ReadBuf{png.data(), png.size()}, decoded));
    REQUIRE(decoded.cols == 5); REQUIRE(decoded.rows == 3);
    CHECK(decoded.buf == m.px);

    lx::ExportImageParams p;
    CHECK(lx::layer_image_filename(p, 7, 12) == "layer07.png");
    CHECK(lx::layer_image_filename(p, 7, 9) == "layer7.png");
    p.pad_layer_index = false;
    CHECK(lx::layer_image_filename(p, 7, 1000) == "layer7.png");
    p.pad_layer_index = true; p.type = lx::ImageType::Pgm; p.filename_prefix = "x_";
    CHECK(lx::layer_image_filename(p, 3, 100) == "x_003.pgm");

    // Range 1..2, ROI 2x2 at (1,0), flip horizontal then rotate 90 clockwise.
    std::vector<Mask> layers(4, Mask(4, 3));
    for (size_t l = 0; l < 4; ++l) { layers[l].at(1, 0) = uint8_t(10 * (l + 1)); layers[l].at(2, 0) = uint8_t(20 * (l + 1)); layers[l].at(1, 1) = 1; }
    lx::ExportImageParams q;
    q.selection.first_layer = 1; q.selection.last_layer = 2; q.selection.roi = Rect{1, 0, 2, 2};
    q.orientation.flip = FlipDirection::Horizontal; q.orientation.rotate = RotateDirection::Rotate90Clockwise;
    const std::vector<lx::ExportedFile> files = lx::render_layer_images(layers, q);
    REQUIRE(files.size() == 2);
    CHECK(files[0].name == "layer1.png");
    ::Slic3r::png::ImageGreyscale img;
    REQUIRE(::Slic3r::png::decode_png(::Slic3r::png::ReadBuf{files[0].data.data(), files[0].data.size()}, img));
    // ROI of layer 1 = [[20, 40], [1, 0]]; flip H = [[40, 20], [0, 1]]; rotate 90 cw = [[0, 40], [1, 20]].
    REQUIRE(img.cols == 2); REQUIRE(img.rows == 2);
    CHECK(img.buf == std::vector<uint8_t>{0, 40, 1, 20});

    // Cropping can be switched off.
    q.crop_by_roi = false;
    CHECK(lx::render_layer_images(layers, q)[0].data != files[0].data);
}

TEST_CASE("export image: files are staged and a cancel leaves the folder empty", "[sla_layers][sla_layerexport]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sla_layerexport_test_images";
    fs::remove_all(dir);
    std::vector<Mask> layers(70, block(4, 4, 1, 1, 2, 2));
    lx::ExportImageParams p;

    CHECK_THROWS_AS(lx::export_layer_images(layers, p, dir.string(), [](size_t done, size_t) { return done < 66; }), LayerOperationCancelled);
    size_t leftovers = 0;
    if (fs::exists(dir)) for (const auto &e : fs::directory_iterator(dir)) { (void) e; ++leftovers; }
    CHECK(leftovers == 0); // the first batch of 64 was written as .part files and rolled back

    CHECK(lx::export_layer_images(layers, p, dir.string()) == 70);
    size_t files = 0, parts = 0;
    for (const auto &e : fs::directory_iterator(dir)) { ++files; parts += e.path().extension() == ".part"; }
    CHECK(files == 70); CHECK(parts == 0);
    CHECK(fs::exists(dir / "layer00.png")); CHECK(fs::exists(dir / "layer69.png"));
    fs::remove_all(dir);
}

TEST_CASE("export GIF: encoder output decodes bit exactly", "[sla_layers][sla_layerexport]")
{
    // Small frames with a known pattern.
    std::vector<Mask> frames{Mask(5, 4, 0), Mask(5, 4, 255), Mask(5, 4, 7)};
    for (size_t i = 0; i < frames[0].px.size(); ++i) frames[0].px[i] = uint8_t(i * 13);
    std::vector<uint8_t> gif = lx::encode_gif(frames, 3, 0);
    DecodedGif g = decode_gif(gif);
    REQUIRE(g.ok);
    CHECK(g.width == 5); CHECK(g.height == 4);
    CHECK(g.loops == 0);
    CHECK(g.delays == std::vector<uint16_t>{3, 3, 3});
    REQUIRE(g.frames.size() == 3);
    for (size_t f = 0; f < 3; ++f) CHECK(g.frames[f] == frames[f].px);
    REQUIRE(g.palette.size() == 768);
    CHECK(g.palette[3 * 200] == 200); CHECK(g.palette[3 * 200 + 2] == 200); // grey ramp

    // Noisy frames overflow the 4096 code table many times (clear codes, width changes).
    std::vector<Mask> noisy(2, Mask(210, 190));
    uint32_t s = 12345;
    for (Mask &m : noisy) for (uint8_t &v : m.px) { s = s * 1664525u + 1013904223u; v = uint8_t(s >> 24); }
    noisy[1].px.assign(noisy[1].px.size(), 9);                      // and a flat frame: long runs
    for (size_t i = 0; i < noisy[1].px.size(); i += 97) noisy[1].px[i] = 200;
    g = decode_gif(lx::encode_gif(noisy, 10, 5));
    REQUIRE(g.ok);
    CHECK(g.loops == 5);
    REQUIRE(g.frames.size() == 2);
    CHECK(g.frames[0] == noisy[0].px);
    CHECK(g.frames[1] == noisy[1].px);

    // Single pixel and a mismatching frame size.
    g = decode_gif(lx::encode_gif({Mask(1, 1, 77)}, 1, 0));
    REQUIRE(g.ok); CHECK(g.frames[0] == std::vector<uint8_t>{77});
    CHECK_THROWS_AS(lx::encode_gif({Mask(2, 2), Mask(3, 2)}, 1, 0), std::invalid_argument);
    CHECK_THROWS_AS(lx::encode_gif({}, 1, 0), std::invalid_argument);
}

TEST_CASE("export GIF: frame picking, scale, clip and fps", "[sla_layers][sla_layerexport]")
{
    std::vector<Mask> layers;
    for (int i = 0; i < 10; ++i) layers.push_back(block(20, 20, 4, 4, 8, 8, uint8_t(20 + i)));
    lx::ExportGifParams p;
    p.scale_percent = 50; p.fps = 25; p.skip = 2;
    CHECK(lx::gif_frame_count(p, layers.size()) == 3);              // floor(10 / 3)
    DecodedGif g = decode_gif(lx::render_layer_gif(layers, p));
    REQUIRE(g.ok);
    CHECK(g.width == 10); CHECK(g.height == 10);                    // whole layer at 50 %
    REQUIRE(g.frames.size() == 3);
    CHECK(g.delays == std::vector<uint16_t>{4, 4, 4});             // 1000/25 = 40 ms -> 4 cs
    CHECK(g.frames[1][5 * 10 + 5] == 23);                           // layer 3 (0, 3, 6)
    CHECK(g.frames[2][5 * 10 + 5] == 26);

    p.clip_by_volume_bounds = true; p.scale_percent = 100; p.skip = 0;
    g = decode_gif(lx::render_layer_gif(layers, p));
    CHECK(g.width == 8); CHECK(g.height == 8);
    CHECK(g.frames.size() == 10);

    p.selection.first_layer = 20;
    CHECK_FALSE(lx::validate_export_gif(p, layers.size()).empty());
    CHECK_THROWS_AS(lx::render_layer_gif(layers, p), std::invalid_argument);
    p.selection.first_layer = 0; p.fps = 0;
    CHECK_FALSE(lx::validate_export_gif(p, layers.size()).empty());

    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "sla_layerexport_test.gif";
    fs::remove(file);
    lx::ExportGifParams ok; ok.scale_percent = 100;
    lx::export_layer_gif(layers, ok, file.string());
    CHECK(fs::exists(file));
    CHECK(!fs::exists(file.string() + ".part"));
    fs::remove(file);
    CHECK_THROWS_AS(lx::export_layer_gif(layers, ok, file.string(), [](size_t, size_t) { return false; }), LayerOperationCancelled);
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("export heat map: exact sums, merged Z and ROI", "[sla_layers][sla_layerexport]")
{
    const std::vector<Mask> layers{row({255, 0, 100}), row({255, 255, 100}), row({0, 255, 100}), row({0, 0, 0})};
    lx::ExportHeatMapParams p;
    p.merge_same_positioned_layers = false;
    const Mask heat = lx::render_heat_map(layers, {}, p);
    CHECK(as_ints(heat) == std::vector<int>{128, 128, 75}); // 510/4 = 127.5 rounds half to even like OpenCV, 300/4 = 75

    p.merge_same_positioned_layers = true;
    const Mask merged = lx::render_heat_map(layers, {0.05, 0.05, 0.1, 0.15}, p); // layers 0+1 share Z: max, 3 samples
    CHECK(as_ints(merged) == std::vector<int>{85, 170, 67});
    CHECK_THROWS_AS(lx::render_heat_map(layers, {}, p), std::invalid_argument);

    p.merge_same_positioned_layers = false; p.selection.roi = Rect{1, 0, 2, 1};
    CHECK(as_ints(lx::render_heat_map(layers, {}, p)) == std::vector<int>{128, 75});
    p.crop_by_roi = false;
    CHECK(as_ints(lx::render_heat_map(layers, {}, p)) == std::vector<int>{0, 128, 75});
    p.crop_by_roi = true; p.selection.roi = {};

    p.selection.first_layer = 1; p.selection.last_layer = 1;
    CHECK_FALSE(lx::validate_export_heat_map(p, layers.size()).empty()); // needs two layers
    p.selection = {};
    p.orientation.flip = FlipDirection::Horizontal;
    CHECK(as_ints(lx::render_heat_map(layers, {}, p)) == std::vector<int>{75, 128, 128});
}

TEST_CASE("export skeleton: a thick bar thins to its centre line", "[sla_layers][sla_layerexport]")
{
    const Mask bar = block(9, 5, 1, 1, 7, 3);
    const Mask sk = lx::skeletonize(bar);
    CHECK(sk.nonzero_count() == 5);
    for (int x = 2; x <= 6; ++x) CHECK(sk.at(size_t(x), 2) == 255);

    lx::ExportSkeletonParams p;
    const Mask sum = lx::render_skeleton({bar, bar}, p); // two layers: saturating sum 255 + 255
    CHECK(sum.nonzero_count() == 5);
    CHECK(sum.at(3, 2) == 255);
    p.selection.roi = Rect{0, 0, 9, 5};
    const Mask roi = lx::render_skeleton({bar}, p);
    CHECK(roi.width == 9); CHECK(roi.height == 5);
    CHECK(roi.nonzero_count() == 5);
    CHECK(lx::skeletonize(Mask(4, 4)).nonzero_count() == 0);
    CHECK(lx::skeletonize(Mask(4, 4, 255)).nonzero_count() == 0); // a full frame must terminate (it never erodes)
}

TEST_CASE("export HTML: self contained and escaped", "[sla_layers][sla_layerexport]")
{
    std::vector<Mask> layers{block(8, 8, 2, 2, 3, 3), block(8, 8, 2, 2, 4, 4), block(8, 8, 1, 1, 5, 5)};
    std::vector<double> z = z_steps(3, 0.05);
    std::vector<LayerParams> table = normal_table(3);
    table[0].is_bottom = true;
    lx::ExportHtmlParams p;
    p.title = "<script>alert(1)</script> & co";
    p.properties = {{"Name", "a<b"}, {"Printer", "\"quoted\""}};
    p.gcode = "G1 Z1 ; <cmd>";
    p.preview_scale_percent = 100;
    const std::string html = lx::render_html(layers, z, table, p);
    CHECK(html.find("<script>alert(1)") == std::string::npos);
    CHECK(html.find("&lt;script&gt;alert(1)&lt;/script&gt; &amp; co") != std::string::npos);
    CHECK(html.find("a&lt;b") != std::string::npos);
    CHECK(html.find("&quot;quoted&quot;") != std::string::npos);
    CHECK(html.find("G1 Z1 ; &lt;cmd&gt;") != std::string::npos);
    CHECK(html.find("data:image/png;base64,") != std::string::npos);
    CHECK(html.find("http://") == std::string::npos);
    CHECK(html.find("https://") == std::string::npos);
    CHECK(html.find("src=\"//") == std::string::npos);
    CHECK(html.find("<link") == std::string::npos);
    CHECK(html.find("0 (bottom)") != std::string::npos);
    // Three layer rows in the table + header rows of the properties table.
    size_t rows = 0;
    for (size_t pos = 0; (pos = html.find("<tr><td>", pos)) != std::string::npos; ++pos) ++rows;
    CHECK(rows == 3 + 2);
    p.export_layer_preview = false;
    CHECK(lx::render_html(layers, z, table, p).find("data:image/png") == std::string::npos);

    CHECK(lx::base64_encode({}) == "");
    CHECK(lx::base64_encode({'f'}) == "Zg==");
    CHECK(lx::base64_encode({'f', 'o'}) == "Zm8=");
    CHECK(lx::base64_encode({'f', 'o', 'o'}) == "Zm9v");
    CHECK(lx::html_escape("<&>\"'") == "&lt;&amp;&gt;&quot;&#39;");
}

namespace {

using V3 = std::array<long, 3>;

V3 quant(const float *v) { return {std::lround(double(v[0]) * 1e5), std::lround(double(v[1]) * 1e5), std::lround(double(v[2]) * 1e5)}; }

// Every directed edge must have exactly one opposite twin (closed, consistently oriented surface).
bool is_closed(const lx::VoxelMesh &m)
{
    std::map<std::pair<V3, V3>, int> edges;
    for (const lx::Triangle &t : m.triangles)
        for (int i = 0; i < 3; ++i) ++edges[{quant(t.v[i]), quant(t.v[(i + 1) % 3])}];
    for (const auto &[e, n] : edges) {
        if (n != 1) return false;
        const auto twin = edges.find({e.second, e.first});
        if (twin == edges.end() || twin->second != 1) return false;
    }
    return !edges.empty();
}

double signed_volume(const lx::VoxelMesh &m)
{
    double v = 0;
    for (const lx::Triangle &t : m.triangles) {
        const double ax = t.v[0][0], ay = t.v[0][1], az = t.v[0][2], bx = t.v[1][0], by = t.v[1][1], bz = t.v[1][2], cx = t.v[2][0], cy = t.v[2][1], cz = t.v[2][2];
        v += (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)) / 6.0;
    }
    return v;
}

} // namespace

TEST_CASE("export mesh: 2x2x2 voxel cube is a closed outward oriented surface", "[sla_layers][sla_layerexport]")
{
    const std::vector<Mask> layers{block(6, 6, 2, 2, 2, 2), block(6, 6, 2, 2, 2, 2)};
    const std::vector<double> z{0.1, 0.2};
    lx::ExportMeshParams p;
    p.pixel_width_mm = p.pixel_height_mm = 0.5;

    const lx::VoxelMesh merged = lx::build_voxel_mesh(layers, z, p);
    CHECK(merged.voxels == 8);
    CHECK(merged.triangles.size() == 12);            // 6 quads
    CHECK(is_closed(merged));
    CHECK(signed_volume(merged) == Approx(1.0 * 1.0 * 0.2).epsilon(1e-6)); // positive = normals point outwards

    p.merge_faces = false;
    const lx::VoxelMesh unmerged = lx::build_voxel_mesh(layers, z, p);
    CHECK(unmerged.triangles.size() == 48);          // 6 sides * 4 unit quads * 2
    CHECK(is_closed(unmerged));
    CHECK(signed_volume(unmerged) == Approx(0.2).epsilon(1e-6));

    // Bounds start at the origin: x/y 0..1, z 0..0.2.
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const lx::Triangle &t : merged.triangles)
        for (int v = 0; v < 3; ++v) for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], t.v[v][i]); hi[i] = std::max(hi[i], t.v[v][i]); }
    CHECK(lo[0] == Approx(0.0)); CHECK(hi[0] == Approx(1.0)); CHECK(hi[1] == Approx(1.0)); CHECK(hi[2] == Approx(0.2)); CHECK(lo[2] == Approx(0.0));

    // Binary STL: 80 byte header, count, 50 bytes per triangle, little endian count.
    const std::vector<uint8_t> stl = lx::encode_binary_stl(merged);
    REQUIRE(stl.size() == 84 + 12 * 50);
    CHECK(stl[80] == 12); CHECK(stl[81] == 0);
    // First triangle's normal is axis aligned.
    float n[3]; std::memcpy(n, &stl[84], 12);
    CHECK(std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]) == Approx(1.0));
}

TEST_CASE("export mesh: L shape, merged Z slabs, quality and range", "[sla_layers][sla_layerexport]")
{
    // L shaped footprint in two layers; with merging off the surface must be manifold.
    Mask l(8, 8);
    for (int y = 1; y < 4; ++y) for (int x = 1; x < 3; ++x) l.at(size_t(x), size_t(y)) = 255;
    for (int y = 3; y < 4; ++y) for (int x = 3; x < 6; ++x) l.at(size_t(x), size_t(y)) = 255;
    lx::ExportMeshParams p;
    p.pixel_width_mm = p.pixel_height_mm = 1.0; p.merge_faces = false;
    const lx::VoxelMesh m = lx::build_voxel_mesh({l, l}, {0.5, 1.0}, p);
    CHECK(is_closed(m));
    CHECK(m.voxels == 2 * (6 + 3));
    CHECK(signed_volume(m) == Approx(9.0 * 1.0).epsilon(1e-6)); // 9 px * 1 mm^2 * 1 mm

    // Layers with the same Z form one slab (not two voxel layers).
    p.merge_faces = true;
    const lx::VoxelMesh slab = lx::build_voxel_mesh({l, l}, {0.5, 0.5}, p);
    CHECK(slab.voxels == 9);
    CHECK(signed_volume(slab) == Approx(9.0 * 0.5).epsilon(1e-6));

    // Range starting above layer 0: the first slab begins at the Z of the layer below.
    p.selection.first_layer = 1; p.selection.last_layer = 1;
    const lx::VoxelMesh top = lx::build_voxel_mesh({l, l}, {0.5, 1.0}, p);
    CHECK(signed_volume(top) == Approx(9.0 * 0.5).epsilon(1e-6));

    // Quality 2: 2x2 pixel blocks become one voxel (majority), 4x4 block -> 2x2 voxels of 2 mm.
    lx::ExportMeshParams q;
    q.pixel_width_mm = q.pixel_height_mm = 1.0; q.quality = 2;
    const lx::VoxelMesh coarse = lx::build_voxel_mesh({block(8, 8, 2, 2, 4, 4)}, {0.5}, q);
    CHECK(coarse.voxels == 4);
    CHECK(is_closed(coarse));
    CHECK(signed_volume(coarse) == Approx(4.0 * 4.0 * 0.5).epsilon(1e-6));

    // Orientation moves the footprint; an empty model gives an empty mesh; bad input is rejected.
    CHECK(lx::build_voxel_mesh({Mask(4, 4)}, {0.1}, lx::ExportMeshParams{}).triangles.empty());
    CHECK_THROWS_AS(lx::build_voxel_mesh({l}, {0.1, 0.2}, lx::ExportMeshParams{}), std::invalid_argument);
    CHECK_FALSE(lx::validate_export_mesh(lx::ExportMeshParams{}, 0).empty());
    CHECK_THROWS_AS(lx::build_voxel_mesh({l, l}, {0.5, 1.0}, p, [](size_t, size_t) { return false; }), LayerOperationCancelled);

    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "sla_layerexport_test.stl";
    fs::remove(file);
    lx::ExportMeshParams cube; cube.pixel_width_mm = cube.pixel_height_mm = 0.5;
    lx::export_mesh({block(6, 6, 2, 2, 2, 2), block(6, 6, 2, 2, 2, 2)}, {0.1, 0.2}, cube, file.string());
    CHECK(fs::file_size(file) == 84 + 12 * 50);
    fs::remove(file);
}
