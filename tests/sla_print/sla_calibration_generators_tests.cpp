// SPDX-License-Identifier: AGPL-3.0-or-later
// UVtools-derived pixel calibrations: elephant foot, blooming effect, stress tower.
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>

using namespace Slic3r;

namespace {
// 100 x 100 mm on 1000 x 1000 px: 0.1 mm pixels, so px sizes are easy to check.
DynamicPrintConfig generator_config(const char *format = "uvj") {
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{});
  config.set("display_width", 100.);
  config.set("display_height", 100.);
  config.set("display_pixels_x", 1000);
  config.set("display_pixels_y", 1000);
  config.option("display_orientation")->setInt(sladoLandscape);
  config.set("display_mirror_x", false);
  config.set("display_mirror_y", false);
  config.set("layer_height", .05);
  config.set("initial_layer_height", .05);
  config.set("max_print_height", 100.);
  config.set("exposure_time", 2.5);
  config.set("initial_exposure_time", 30.);
  config.set("sla_archive_format", std::string(format));
  return config;
}

struct MaskStats {
  size_t lit = 0;                // non-zero pixels
  int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
  std::array<bool, 256> seen{};
};
MaskStats stats(const sla::LayerDocument &document, size_t layer) {
  const auto image = document.image(layer);
  MaskStats result;
  for (size_t y = 0; y < image.rows; ++y)
    for (size_t x = 0; x < image.cols; ++x) {
      const uint8_t value = image.get(y, x);
      result.seen[value] = true;
      if (!value) continue;
      ++result.lit;
      result.x0 = std::min(result.x0, int(x)); result.x1 = std::max(result.x1, int(x));
      result.y0 = std::min(result.y0, int(y)); result.y1 = std::max(result.y1, int(y));
    }
  return result;
}

sla::ElephantFootParameters small_elephant() {
  sla::ElephantFootParameters p;
  p.part_scale = .5; p.bottom_layers = 4; p.normal_layers = 6; p.text_height_mm = .2;
  p.erode_start = 2; p.erode_end = 3; p.dimming_start = 140; p.dimming_end = 140;
  return p;
}
} // namespace

TEST_CASE("Elephant foot plan follows UVtools geometry and defaults", "[sla_calibration]") {
  const auto config = generator_config();
  const auto plan = sla::elephant_foot_plan(config, small_elephant());
  // 250 px * 0.5 wide tooth + 2 px border on both sides; 8 steps of 25 px plus the foot ellipse.
  CHECK(plan.shape_width_px == 129);
  CHECK(plan.shape_height_px == 229);
  CHECK(plan.requested_objects == 4);  // ORI + 2 erode + 1 dimming
  REQUIRE(plan.cells.size() == 4);
  CHECK(plan.cells[0].kind == sla::ElephantFootCell::Kind::Original);
  CHECK(plan.cells[1].value == 2); CHECK(plan.cells[2].value == 3);
  CHECK(plan.cells[3].kind == sla::ElephantFootCell::Kind::Dimming);
  CHECK(plan.total_layers == 4 + 6 + 4);  // 0.2 mm of text = 4 layers

  const auto defaults = sla::elephant_foot_plan(config, sla::ElephantFootParameters{});
  CHECK(defaults.bottom_layers == 20);  // 1.0 mm
  CHECK(defaults.normal_layers == 70);  // 3.5 mm
  CHECK(defaults.text_layers == 20);    // 1.0 mm
}

TEST_CASE("Elephant foot masks: bottom layers are modified, normal layers original, text raised", "[sla_calibration]") {
  const auto config = generator_config();
  const auto job = sla::create_elephant_foot_calibration(config, small_elephant());
  REQUIRE(job);
  const auto &document = *job->layer_document();
  REQUIRE(document.layer_count() == 14);
  const auto bottom = stats(document, 0), normal = stats(document, 4), text = stats(document, 13);
  CHECK(bottom.lit < normal.lit);          // erosion/dimming only act on the bottom layers
  CHECK(bottom.seen[140]);                  // dimmed wall keeps exactly the requested brightness
  CHECK(text.lit > 0); CHECK(text.lit < normal.lit);  // raised labels only
  for (size_t layer : {1u, 3u}) CHECK(stats(document, layer).lit == bottom.lit);
  for (size_t layer : {5u, 9u}) CHECK(stats(document, layer).lit == normal.lit);
  // The bottom footprint is centered on the 1000 x 1000 px screen.
  CHECK(std::abs((bottom.x0 + bottom.x1) / 2 - 500) <= 1);
  CHECK(std::abs((bottom.y0 + bottom.y1) / 2 - 500) <= 1);
  // Exposure comes from the presets: bottom for the first 4 layers.
  const auto &fresh = *document.fresh_job();
  CHECK(fresh.rows[0].at(sla::LayerParameter::Exposure) == Catch::Approx(30.));
  CHECK(fresh.rows[4].at(sla::LayerParameter::Exposure) == Catch::Approx(2.5));
}

TEST_CASE("Elephant foot engraved text stays inside the part and adds no layers", "[sla_calibration]") {
  auto p = small_elephant();
  p.extrude_text = false;
  const auto job = sla::create_elephant_foot_calibration(generator_config(), p);
  CHECK(job->layer_document()->layer_count() == 10);
}

TEST_CASE("Elephant foot rejects invalid parameters", "[sla_calibration]") {
  const auto config = generator_config();
  auto p = small_elephant();
  p.erode_start = 7; p.erode_end = 3;
  CHECK_THROWS_AS(sla::elephant_foot_plan(config, p), RuntimeError);
  p = small_elephant(); p.dimming_start = 200; p.dimming_end = 100;
  CHECK_THROWS_AS(sla::elephant_foot_plan(config, p), RuntimeError);
  p = small_elephant(); p.erode_step = 0;
  CHECK_THROWS_AS(sla::elephant_foot_plan(config, p), RuntimeError);
  p = small_elephant(); p.erode = false; p.dimming = false; p.output_original = false;
  CHECK_THROWS_AS(sla::elephant_foot_plan(config, p), RuntimeError);
  p = small_elephant(); p.part_scale = 20.;  // 5000 px part on a 1000 px screen
  CHECK_THROWS_AS(sla::elephant_foot_plan(config, p), RuntimeError);
  p = small_elephant(); p.part_scale = 0.;
  CHECK_THROWS_AS(sla::elephant_foot_plan(config, p), RuntimeError);
}

TEST_CASE("Blooming plan places squares and counts layers", "[sla_calibration]") {
  sla::BloomingParameters p;
  p.object_count = 3; p.object_diameter_px = 100; p.object_height_mm = .1;
  p.wait_start_s = 0.; p.wait_increment_s = .5;
  const auto plan = sla::blooming_plan(generator_config(), p);
  REQUIRE(plan.objects.size() == 4);
  CHECK(plan.objects[0].x_px == 200); CHECK(plan.objects[1].x_px == 320);
  CHECK(plan.objects[3].wait_s == Catch::Approx(1.5));
  CHECK(plan.object_layers == 3);   // first layer + 2 for 0.1 mm
  CHECK(plan.text_layers == 20);    // 1 mm
  CHECK(plan.total_layers == 4 * 3 + 20);

  // Squares wrap to the next row when the screen width is exhausted.
  p.object_count = 15;
  const auto wrapped = sla::blooming_plan(generator_config(), p);
  CHECK(wrapped.objects.size() > 4);
  CHECK(wrapped.objects.back().y_px > wrapped.objects.front().y_px);
}

TEST_CASE("Blooming job exposes each square with its own wait time", "[sla_calibration]") {
  sla::BloomingParameters p;
  p.object_count = 3; p.object_diameter_px = 100; p.object_height_mm = .1;
  p.wait_increment_s = .5;
  const auto job = sla::create_blooming_calibration(generator_config(), p);
  REQUIRE(job);
  const auto &document = *job->layer_document();
  REQUIRE(document.layer_count() == 4 * 3 + 20);
  using F = sla::LayerParameter;
  const auto wait_field = document.layer_parameters(0).count(F::WaitBefore) ? F::WaitBefore : F::LightOff;
  for (size_t i = 0; i < 4; ++i) {
    CHECK(document.layer_parameters(i).at(F::PositionZ) == Catch::Approx(.05));  // same height
    CHECK(document.layer_parameters(i).at(wait_field) == Catch::Approx(.5 * i));
    CHECK(document.layer_parameters(i).at(F::Exposure) == Catch::Approx(30.));  // bottom layer
    CHECK(document.layer_parameters(i + 12).at(F::Exposure) == Catch::Approx(2.5));  // text layer, normal
  }
  // One square of (diameter + 1) px (Emgu inclusive corner) per record.
  CHECK(stats(document, 0).lit == 101 * 101);
  CHECK(document.layer_parameters(12).at(F::PositionZ) == Catch::Approx(.2));
  CHECK(stats(document, 12).lit > 0);  // text layer
}

TEST_CASE("Blooming rejects invalid parameters and unsupported formats", "[sla_calibration]") {
  const auto config = generator_config();
  sla::BloomingParameters p;
  p.object_diameter_px = 10;
  CHECK_THROWS_AS(sla::blooming_plan(config, p), RuntimeError);
  p = {}; p.left_right_margin_px = 600;
  CHECK_THROWS_AS(sla::blooming_plan(config, p), RuntimeError);
  p = {}; p.object_diameter_px = 700;
  CHECK_THROWS_AS(sla::blooming_plan(config, p), RuntimeError);
  p = {}; p.wait_increment_s = .01;
  CHECK_THROWS_AS(sla::blooming_plan(config, p), RuntimeError);
  p = {}; p.object_count = 0;
  CHECK_THROWS_AS(sla::blooming_plan(config, p), RuntimeError);
  CHECK_THROWS_AS(sla::create_blooming_calibration(generator_config("SL1"), {}), RuntimeError);
}

TEST_CASE("Stress tower geometry in pixels and layers", "[sla_calibration]") {
  sla::StressTowerParameters p;
  p.base_diameter_mm = 20.; p.base_height_mm = .5; p.body_height_mm = 1.; p.ceil_height_mm = .5;
  p.spiral_diameter_mm = 2.; p.chamfer_layers = 4; p.antialiasing = false;
  const auto config = generator_config();
  const auto plan = sla::stress_tower_plan(config, p);
  CHECK(plan.base_radius_px == 100);   // 20 mm at 0.1 mm/px
  CHECK(plan.spiral_radius_px == 10);
  CHECK(plan.base_layers == 10); CHECK(plan.body_layers == 20); CHECK(plan.ceil_layers == 10);
  CHECK(plan.total_layers == 40);

  const auto job = sla::create_stress_tower_calibration(config, p);
  const auto &document = *job->layer_document();
  REQUIRE(document.layer_count() == 40);
  const auto first = stats(document, 0), flat = stats(document, 9), ceil = stats(document, 39), body = stats(document, 15);
  CHECK(first.lit < flat.lit);          // chamfer shrinks the first layers
  CHECK(flat.x1 - flat.x0 + 1 >= 199);  // full 200 px base diameter
  CHECK(ceil.lit == flat.lit);
  CHECK(body.lit > 0); CHECK(body.lit < flat.lit);  // only the spirals
  // The spirals wind around: the lit pixels of body layers move with the layer index.
  const auto a = document.image(15), b = document.image(16);
  CHECK(a.buf != b.buf);
}

TEST_CASE("Stress tower rejects invalid parameters", "[sla_calibration]") {
  const auto config = generator_config();
  sla::StressTowerParameters p;
  p.base_diameter_mm = 0.;
  CHECK_THROWS_AS(sla::stress_tower_plan(config, p), RuntimeError);
  p = {}; p.spirals = 0;
  CHECK_THROWS_AS(sla::stress_tower_plan(config, p), RuntimeError);
  p = {}; p.base_diameter_mm = 150.;  // wider than the 100 mm screen
  CHECK_THROWS_AS(sla::stress_tower_plan(config, p), RuntimeError);
  p = {}; p.body_height_mm = 500.;    // taller than the printer
  CHECK_THROWS_AS(sla::stress_tower_plan(config, p), RuntimeError);
}

TEST_CASE("Pixel calibrations honor cancellation", "[sla_calibration]") {
  const auto cancel = [] { throw RuntimeError("canceled"); };
  CHECK_THROWS_AS(sla::create_elephant_foot_calibration(generator_config(), small_elephant(), cancel), RuntimeError);
  CHECK_THROWS_AS(sla::create_stress_tower_calibration(generator_config(), {}, cancel), RuntimeError);
}

// ---------------------------------------------------------------------------
// XYZ accuracy, tolerance, lift height, external tests, apply-result helpers.
// ---------------------------------------------------------------------------
namespace {
sla::XYZAccuracyParameters small_xyz() {
  sla::XYZAccuracyParameters p;
  p.bottom_layers = 4;
  p.x_mm = 20.; p.y_mm = 20.; p.z_mm = 1.;
  p.wall_thickness_mm = 2.; p.drain_hole_mm = .5;
  p.center_hole_relief = false;
  p.objects = {true, false, false, false, true, false, false, false, false};  // top-left + centre
  return p;
}
sla::ToleranceParameters small_tolerance() {
  sla::ToleranceParameters p;
  p.bottom_layers = 3; p.z_mm = 1.;
  p.antialiasing = false;
  return p;
}
} // namespace

TEST_CASE("XYZ accuracy plan quantizes sizes to pixels and layers", "[sla_calibration]") {
  const auto plan = sla::xyz_accuracy_plan(generator_config(), small_xyz());
  CHECK(plan.block_x_px == 200); CHECK(plan.block_y_px == 200);
  CHECK(plan.wall_x_px == 20);
  CHECK(plan.total_layers == 20);       // 1 mm at 0.05 mm
  CHECK(plan.bottom_layers == 4);
  CHECK(plan.vent_layers == 11);        // 0.5 mm drain = 10 layers, inclusive like UVtools
  CHECK(plan.object_count == 2);
  CHECK(plan.real_x_mm == Catch::Approx(20.));
  CHECK(plan.real_z_mm == Catch::Approx(1.));

  // A size that is not a whole pixel is rounded down and reported.
  auto p = small_xyz();
  p.x_mm = 20.05;
  CHECK(sla::xyz_accuracy_plan(generator_config(), p).block_x_px == 200);
  CHECK(sla::xyz_accuracy_plan(generator_config(), p).real_x_mm == Catch::Approx(20.));
}

TEST_CASE("XYZ accuracy uses each axis pitch on non-square pixels", "[sla_calibration]") {
  auto config = generator_config();
  config.set("display_height", 50.);  // 1000 px over 50 mm: 0.05 mm tall pixels
  auto p = small_xyz();
  p.x_mm = 10.; p.y_mm = 10.;
  const auto plan = sla::xyz_accuracy_plan(config, p);
  CHECK(plan.block_x_px == 100);
  CHECK(plan.block_y_px == 200);
  CHECK(plan.real_x_mm == Catch::Approx(10.));
  CHECK(plan.real_y_mm == Catch::Approx(10.));
}

TEST_CASE("XYZ accuracy masks: solid bottom, hollow body, drain slot above the bottom", "[sla_calibration]") {
  const auto job = sla::create_xyz_accuracy_calibration(generator_config(), small_xyz());
  REQUIRE(job);
  const auto &document = *job->layer_document();
  REQUIRE(document.layer_count() == 20);
  const auto bottom = stats(document, 0), vent = stats(document, 5), hollow = stats(document, 15);
  CHECK(bottom.lit > 70000);               // two 200 x 200 px solid blocks minus labels
  CHECK(bottom.lit < 80000);
  CHECK(hollow.lit <= 28800);              // wall ring 200^2 - 160^2 per block
  CHECK(hollow.lit > 27000);
  CHECK(vent.lit < hollow.lit);            // the slot removes wall pixels
  CHECK(stats(document, 3).lit == bottom.lit);
  CHECK(stats(document, 14).lit == vent.lit);   // last vent layer: 4 + 11 - 1
  CHECK(stats(document, 15).lit == hollow.lit);
  // Blocks stay at the requested positions: top-left corner at the 100 px margin.
  CHECK(bottom.x0 == 100); CHECK(bottom.y0 == 100);
  const auto &fresh = *document.fresh_job();
  CHECK(fresh.rows[0].at(sla::LayerParameter::Exposure) == Catch::Approx(30.));
  CHECK(fresh.rows[4].at(sla::LayerParameter::Exposure) == Catch::Approx(2.5));
}

TEST_CASE("XYZ accuracy rejects layouts that cannot be printed", "[sla_calibration]") {
  const auto config = generator_config();
  auto p = small_xyz();
  p.wall_thickness_mm = 11.;  // 110 px walls leave no cavity in a 200 px block
  CHECK_THROWS_AS(sla::xyz_accuracy_plan(config, p), RuntimeError);
  p = small_xyz(); p.objects.fill(true); p.x_mm = 40.; p.y_mm = 40.;  // 400 px blocks overlap in a 1000 px screen
  CHECK_THROWS_AS(sla::xyz_accuracy_plan(config, p), RuntimeError);
  p = small_xyz(); p.objects.fill(false);
  CHECK_THROWS_AS(sla::xyz_accuracy_plan(config, p), RuntimeError);
  p = small_xyz(); p.x_mm = 120.;  // wider than the screen
  CHECK_THROWS_AS(sla::xyz_accuracy_plan(config, p), RuntimeError);
  p = small_xyz(); p.z_mm = 500.;  // taller than the printer
  CHECK_THROWS_AS(sla::xyz_accuracy_plan(config, p), RuntimeError);
  p = small_xyz(); p.x_mm = 0.;
  CHECK_THROWS_AS(sla::xyz_accuracy_plan(config, p), RuntimeError);
}

TEST_CASE("Tolerance plan steps male parts around the hole size", "[sla_calibration]") {
  const auto plan = sla::tolerance_plan(generator_config(), small_tolerance());
  CHECK(plan.total_layers == 20);
  CHECK(plan.female_x_px == 160); CHECK(plan.hole_x_px == 100);
  CHECK(plan.requested_objects == 7);   // same size + 5 thinner + the female ring
  REQUIRE(plan.parts.size() == 6);
  const int expected_width[6] = {100, 99, 98, 97, 96, 95};
  for (size_t i = 0; i < 6; ++i) CHECK(plan.parts[i].width_px == expected_width[i]);
  CHECK(plan.parts[1].step_mm == Catch::Approx(-.1));
  CHECK(plan.parts[1].size_mm == Catch::Approx(9.9));
  // First male part sits right of the ring (100 + 160 + 50 px margin); the row wraps at the screen edge.
  CHECK(plan.parts[0].x_px == 310);
  CHECK(plan.parts[4].x_px == 100);
  CHECK(plan.parts[4].y_px == 326);
}

TEST_CASE("Tolerance rings follow the pitch of each axis", "[sla_calibration]") {
  auto config = generator_config();
  config.set("display_height", 50.);
  const auto plan = sla::tolerance_plan(config, small_tolerance());
  CHECK(plan.female_x_px == 160);
  CHECK(plan.female_y_px == 320);   // 16 mm / 0.05 mm
  CHECK(plan.hole_y_px == 200);
}

TEST_CASE("Tolerance masks erode the bottom, chamfer both ends and engrave labels late", "[sla_calibration]") {
  const auto job = sla::create_tolerance_calibration(generator_config(), small_tolerance());
  REQUIRE(job);
  const auto &document = *job->layer_document();
  REQUIRE(document.layer_count() == 20);
  const auto bottom = stats(document, 0), chamfered = stats(document, 3), plain = stats(document, 4),
             labelled = stats(document, 10), top = stats(document, 19);
  CHECK(bottom.lit < chamfered.lit);    // bottom erosion + strongest chamfer
  CHECK(chamfered.lit < plain.lit);
  CHECK(labelled.lit < plain.lit);      // labels are engraved in the last 15 layers only
  CHECK(top.lit < labelled.lit);        // top chamfer
  CHECK(stats(document, 10).lit == stats(document, 11).lit);
  // The footprint is centred on the 1000 x 1000 px screen.
  CHECK(std::abs((plain.x0 + plain.x1) / 2 - 500) <= 1);
  CHECK(std::abs((plain.y0 + plain.y1) / 2 - 500) <= 1);
  const auto &fresh = *document.fresh_job();
  CHECK(fresh.rows[0].at(sla::LayerParameter::Exposure) == Catch::Approx(30.));
  CHECK(fresh.rows[3].at(sla::LayerParameter::Exposure) == Catch::Approx(2.5));
}

TEST_CASE("Tolerance fused parts sit inside their own rings", "[sla_calibration]") {
  auto p = small_tolerance();
  p.fuse_parts = true; p.output_same_diameter_part = false;
  p.thinner_models = 2; p.part_margin_px = 20;
  const auto plan = sla::tolerance_plan(generator_config(), p);
  CHECK(plan.requested_objects == 2);
  REQUIRE(plan.parts.size() == 2);
  CHECK(plan.parts[0].width_px == 99);
  // Square shape draws the same layout with rectangles.
  p.shape = sla::ToleranceShape::Square;
  CHECK(sla::tolerance_plan(generator_config(), p).parts.size() == 2);
}

TEST_CASE("Tolerance rejects invalid parameters", "[sla_calibration]") {
  const auto config = generator_config();
  auto p = small_tolerance();
  p.female_hole_diameter_mm = 16.;
  CHECK_THROWS_AS(sla::tolerance_plan(config, p), RuntimeError);
  p = small_tolerance(); p.z_mm = 500.;
  CHECK_THROWS_AS(sla::tolerance_plan(config, p), RuntimeError);
  p = small_tolerance(); p.female_diameter_mm = 150.;  // ring wider than the screen
  p.female_hole_diameter_mm = 100.;
  CHECK_THROWS_AS(sla::tolerance_plan(config, p), RuntimeError);
  p = small_tolerance(); p.output_same_diameter_part = false; p.thinner_models = 0; p.thicker_models = 0;
  p.fuse_parts = true;
  CHECK_THROWS_AS(sla::tolerance_plan(config, p), RuntimeError);
}

TEST_CASE("Lift height plan counts the shrinking layers like UVtools", "[sla_calibration]") {
  sla::LiftHeightParameters p;
  p.bottom_layers = 2; p.normal_layers = 3;
  p.decrease_factor_pct = 25; p.minimum_factor_pct = 25;
  const auto plan = sla::lift_height_plan(generator_config(), p);
  CHECK(plan.block_width_px == 600); CHECK(plan.block_height_px == 600);
  REQUIRE(plan.factors_pct.size() == 3);   // 75, 50, 25
  CHECK(plan.factors_pct[0] == 75); CHECK(plan.factors_pct[2] == 25);
  CHECK(plan.decrease_layers == 3);
  CHECK(plan.total_layers == 8);
  CHECK(plan.total_height_mm == Catch::Approx(.4));
  p.decrease_image = false;
  CHECK(sla::lift_height_plan(generator_config(), p).total_layers == 5);
}

TEST_CASE("Lift height masks shrink at the end and carry the requested motion", "[sla_calibration]") {
  sla::LiftHeightParameters p;
  p.bottom_layers = 2; p.normal_layers = 3;
  p.decrease_factor_pct = 25; p.minimum_factor_pct = 25;
  p.bottom_exposure_s = 20.; p.normal_exposure_s = 1.5;
  p.bottom_lift_height_mm = 8.; p.lift_height_mm = 7.5;
  const auto job = sla::create_lift_height_calibration(generator_config(), p);
  REQUIRE(job);
  const auto &document = *job->layer_document();
  REQUIRE(document.layer_count() == 8);
  CHECK(stats(document, 0).lit == 600 * 600);
  CHECK(stats(document, 4).lit == 600 * 600);
  CHECK(stats(document, 5).lit == 450 * 450);
  CHECK(stats(document, 6).lit == 300 * 300);
  CHECK(stats(document, 7).lit == 150 * 150);
  CHECK(document.layer_parameters(7).at(sla::LayerParameter::PositionZ) == Catch::Approx(.4));
  const auto &fresh = *document.fresh_job();
  CHECK(fresh.rows[0].at(sla::LayerParameter::Exposure) == Catch::Approx(20.));
  CHECK(fresh.rows[2].at(sla::LayerParameter::Exposure) == Catch::Approx(1.5));
  if (fresh.rows[0].count(sla::LayerParameter::LiftDistance)) {
    CHECK(fresh.rows[0].at(sla::LayerParameter::LiftDistance) == Catch::Approx(8.));
    CHECK(fresh.rows[2].at(sla::LayerParameter::LiftDistance) == Catch::Approx(7.5));
  }
}

TEST_CASE("Lift height rejects invalid parameters", "[sla_calibration]") {
  const auto config = generator_config();
  sla::LiftHeightParameters p;
  p.left_right_margin_px = 500;  // margins consume the whole screen
  CHECK_THROWS_AS(sla::lift_height_plan(config, p), RuntimeError);
  p = {}; p.decrease_factor_pct = 0;
  CHECK_THROWS_AS(sla::lift_height_plan(config, p), RuntimeError);
  p = {}; p.minimum_factor_pct = 100;
  CHECK_THROWS_AS(sla::lift_height_plan(config, p), RuntimeError);
  p = {}; p.normal_layers = 5000;  // 250 mm
  CHECK_THROWS_AS(sla::lift_height_plan(config, p), RuntimeError);
  p = {}; p.normal_exposure_s = 0.;
  CHECK_THROWS_AS(sla::create_lift_height_calibration(config, p), RuntimeError);
  p = {}; p.lift_height_mm = -1.;
  CHECK_THROWS_AS(sla::create_lift_height_calibration(config, p), RuntimeError);
}

TEST_CASE("New pixel calibrations honor cancellation", "[sla_calibration]") {
  const auto cancel = [] { throw RuntimeError("canceled"); };
  CHECK_THROWS_AS(sla::create_xyz_accuracy_calibration(generator_config(), small_xyz(), cancel), RuntimeError);
  CHECK_THROWS_AS(sla::create_tolerance_calibration(generator_config(), small_tolerance(), cancel), RuntimeError);
  CHECK_THROWS_AS(sla::create_lift_height_calibration(generator_config(), {}, cancel), RuntimeError);
}

TEST_CASE("External test list is offline reference text", "[sla_calibration]") {
  const auto &tests = sla::external_calibration_tests();
  REQUIRE(tests.size() >= 4);
  for (const auto &test : tests) {
    CHECK(!test.name.empty());
    CHECK(!test.purpose.empty());
    CHECK(test.address.rfind("https://", 0) == 0);
    CHECK(!test.native_alternative.empty());
  }
}

TEST_CASE("Apply-result conversions give the value written to the profile", "[sla_calibration]") {
  // Gray 128 of 255 cured at 2.5 s corresponds to a full-white dose of about 1.25 s.
  CHECK(sla::grayscale_equivalent_exposure_s(2.5, 128) == Catch::Approx(2.5 * 128. / 255.));
  CHECK(sla::grayscale_equivalent_exposure_s(2.5, 255) == Catch::Approx(2.5));
  CHECK_THROWS_AS(sla::grayscale_equivalent_exposure_s(2.5, 0), RuntimeError);
  CHECK_THROWS_AS(sla::grayscale_equivalent_exposure_s(0., 100), RuntimeError);
  // Three erode iterations on 50 um pixels remove 0.15 mm per side.
  CHECK(sla::elephant_foot_compensation_mm(3, .05, .05) == Catch::Approx(.15));
  CHECK(sla::elephant_foot_compensation_mm(2, .05, .10) == Catch::Approx(.15));  // mean pitch
  CHECK(sla::elephant_foot_compensation_mm(0, .05, .05) == Catch::Approx(0.));
  CHECK_THROWS_AS(sla::elephant_foot_compensation_mm(1, 0., .05), RuntimeError);
}
