// SPDX-License-Identifier: AGPL-3.0-or-later
// LayerDocument bridge for the batch-2 UVtools operations (goal G6): per-layer parameter edits go
// through the document's parameter capabilities as ONE undo step, and anything the format cannot
// store (or that would change the layer list through the wrong entry point) is refused without
// touching the document.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerOperations.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>

using namespace Slic3r;
namespace lo = Slic3r::sla::layerops;
using Par = sla::LayerParameter;
using Catch::Approx;

namespace {
constexpr size_t kW = 8, kH = 4;
constexpr double kLayer = 0.05;

std::vector<sla::EncodedRaster> make_masks(size_t count = 4)
{
    std::vector<sla::EncodedRaster> layers;
    for (size_t i = 0; i < count; ++i) {
        std::vector<uint8_t> px(kW * kH, 0);
        px[0] = 255;
        px[kW + 1 + i % 3] = 200;
        layers.push_back(sla::PNGRasterEncoder{}(px.data(), kW, kH, 1));
    }
    return layers;
}

sla::LayerDocument fresh_document()
{
    sla::FreshLayerJob job;
    job.producer = sla::FreshLayerProducer::Raster;
    job.geometry = {kW, kH, 16., 8.};
    job.globals.layer_height = kLayer;
    job.globals.bottom_layers = 1;
    job.globals.normal = {{Par::Exposure, 3.}, {Par::LiftDistance, 1.}, {Par::LiftSpeed, 60.}, {Par::LiftDistance2, 2.},
                          {Par::LiftSpeed2, 120.}, {Par::RetractDistance2, .5}, {Par::RetractSpeed2, 120.},
                          {Par::RetractSpeed, 150.}, {Par::WaitBefore, .25}, {Par::WaitAfter, .5}, {Par::WaitAfterLift, .75},
                          {Par::LightOff, 0.}, {Par::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    for (size_t i = 0; i < 4; ++i) {
        auto row = job.globals.normal;
        row[Par::PositionZ] = kLayer * double(i + 1);
        job.rows.push_back(row);
    }
    return sla::LayerDocument::from_fresh_rasters(make_masks(), std::move(job));
}

double value(const sla::LayerDocument &doc, size_t layer, Par field) { return doc.layer_parameters(layer).at(field); }
} // namespace

TEST_CASE("Parameter table mirrors the stored rows and the bottom layer count",
          "[sla_layers][sla_layer_ops_document2]")
{
    auto doc = fresh_document();
    REQUIRE(doc.parameter_operation_refusal().empty());
    const auto table = doc.parameter_table();
    REQUIRE(table.size() == 4);
    CHECK(table[0].is_bottom);
    CHECK_FALSE(table[1].is_bottom);
    CHECK(table[2].exposure_s == Approx(3.));
    CHECK(table[2].lift_height2_mm == Approx(2.));
    CHECK(table[2].wait_after_lift_s == Approx(.75));
    const auto context = doc.parameter_printer_context(kLayer);
    CHECK(context.can_use_lift_height2);
    CHECK(context.layer_height_mm == Approx(kLayer));
}

TEST_CASE("A parameter edit over a range is one undo step", "[sla_layers][sla_layer_ops_document2]")
{
    auto doc = fresh_document();
    const auto revision = doc.revision();
    lo::EditParametersParams edit;
    edit.first_layer = 1;
    edit.last_layer = 2;
    edit.per_layer_override = true;
    edit.edit.exposure_s = 7.;
    edit.edit.lift_speed = 90.;

    REQUIRE(doc.apply_parameter_operation(
        [&](std::vector<lo::Mask> &, std::vector<double> &z, std::vector<lo::LayerParams> &table, const sla::LayerProgress &p) {
            lo::edit_parameters(z, table, edit, p);
            return sla::LayerDocument::LayerListChange{};
        }, false, kLayer));
    CHECK(value(doc, 0, Par::Exposure) == Approx(3.));
    CHECK(value(doc, 1, Par::Exposure) == Approx(7.));
    CHECK(value(doc, 2, Par::Exposure) == Approx(7.));
    CHECK(value(doc, 2, Par::LiftSpeed) == Approx(90.));
    CHECK(value(doc, 3, Par::Exposure) == Approx(3.));
    CHECK(doc.revision() != revision);

    REQUIRE(doc.undo());                         // ONE undo restores every touched layer
    for (size_t i = 0; i < 4; ++i) CHECK(value(doc, i, Par::Exposure) == Approx(3.));
    CHECK(value(doc, 2, Par::LiftSpeed) == Approx(60.));
    CHECK_FALSE(doc.can_undo());
    REQUIRE(doc.redo());
    CHECK(value(doc, 1, Par::Exposure) == Approx(7.));
}

TEST_CASE("A parameter edit that changes nothing leaves no undo step", "[sla_layers][sla_layer_ops_document2]")
{
    auto doc = fresh_document();
    REQUIRE(doc.apply_parameter_operation(
        [](std::vector<lo::Mask> &, std::vector<double> &, std::vector<lo::LayerParams> &, const sla::LayerProgress &) {
            return sla::LayerDocument::LayerListChange{};
        }, false, kLayer));
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("Read-only fields and layer-list changes are refused without touching the document",
          "[sla_layers][sla_layer_ops_document2]")
{
    auto doc = fresh_document();
    const auto revision = doc.revision();

    // Z of an owned (generated) document is read-only for a parameter edit.
    CHECK_THROWS_WITH(doc.apply_parameter_operation(
        [](std::vector<lo::Mask> &, std::vector<double> &z, std::vector<lo::LayerParams> &, const sla::LayerProgress &) {
            z[1] += 0.01;
            return sla::LayerDocument::LayerListChange{};
        }, false, kLayer), Catch::Matchers::ContainsSubstring("read-only"));

    // Adding a layer must go through the layer-list entry point.
    CHECK_THROWS_WITH(doc.apply_parameter_operation(
        [](std::vector<lo::Mask> &m, std::vector<double> &z, std::vector<lo::LayerParams> &t, const sla::LayerProgress &) {
            m.push_back(m.back()); z.push_back(z.back() + kLayer); t.push_back(t.back());
            return sla::LayerDocument::LayerListChange{};
        }, true, kLayer), Catch::Matchers::ContainsSubstring("layer-list"));

    // Pixel changes are not a parameter edit.
    CHECK_THROWS_WITH(doc.apply_parameter_operation(
        [](std::vector<lo::Mask> &m, std::vector<double> &, std::vector<lo::LayerParams> &, const sla::LayerProgress &) {
            m[0].px[5] = 99;
            return sla::LayerDocument::LayerListChange{};
        }, true, kLayer), Catch::Matchers::ContainsSubstring("pixel"));

    // A bottom layer count change cannot be stored by a parameter edit.
    CHECK_THROWS(doc.apply_parameter_operation(
        [](std::vector<lo::Mask> &, std::vector<double> &, std::vector<lo::LayerParams> &, const sla::LayerProgress &) {
            sla::LayerDocument::LayerListChange change;
            change.bottom_layer_count = 3;
            return change;
        }, false, kLayer));

    CHECK(doc.revision() == revision);
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("Cancelling a parameter operation leaves the document untouched", "[sla_layers][sla_layer_ops_document2]")
{
    auto doc = fresh_document();
    lo::EditParametersParams edit;
    edit.per_layer_override = true;
    edit.edit.exposure_s = 9.;
    const bool done = doc.apply_parameter_operation(
        [&](std::vector<lo::Mask> &, std::vector<double> &z, std::vector<lo::LayerParams> &table, const sla::LayerProgress &p) {
            lo::edit_parameters(z, table, edit, p);
            return sla::LayerDocument::LayerListChange{};
        }, false, kLayer, [](size_t, size_t) { return false; });
    CHECK_FALSE(done);
    CHECK_FALSE(doc.can_undo());
    CHECK(value(doc, 1, Par::Exposure) == Approx(3.));
}

TEST_CASE("A document without a parameter table refuses parameter operations", "[sla_layers][sla_layer_ops_document2]")
{
    auto doc = sla::LayerDocument::from_rasters(make_masks());
    CHECK_FALSE(doc.parameter_operation_refusal().empty());
    CHECK(doc.parameter_table().empty());
    CHECK_THROWS(doc.apply_parameter_operation(
        [](std::vector<lo::Mask> &, std::vector<double> &, std::vector<lo::LayerParams> &, const sla::LayerProgress &) {
            return sla::LayerDocument::LayerListChange{};
        }, false, kLayer));
}

TEST_CASE("A parameter operation that adds a layer is one undo step and carries its fields",
          "[sla_layers][sla_layer_ops_document2]")
{
    auto doc = fresh_document();
    REQUIRE(doc.apply_parameter_list_operation(
        [](std::vector<lo::Mask> &m, std::vector<double> &z, std::vector<lo::LayerParams> &t, const sla::LayerProgress &) {
            m.push_back(m.back());
            z.push_back(z.back() + kLayer);
            t.push_back(t.back());
            t.back().wait_after_lift_s = 5.;
            t.back().exposure_s = 1.5;
            sla::LayerDocument::LayerListChange change;
            change.list.source_index = {0, 1, 2, 3, 3};
            return change;
        }, kLayer));
    REQUIRE(doc.layer_count() == 5);
    CHECK(value(doc, 4, Par::WaitAfterLift) == Approx(5.));
    CHECK(value(doc, 4, Par::Exposure) == Approx(1.5));
    CHECK(value(doc, 3, Par::WaitAfterLift) == Approx(.75));
    CHECK(value(doc, 4, Par::PositionZ) == Approx(kLayer * 5));
    CHECK(doc.fresh_job()->globals.bottom_layers == 1);

    REQUIRE(doc.undo());
    CHECK(doc.layer_count() == 4);
    CHECK(value(doc, 3, Par::WaitAfterLift) == Approx(.75));
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("A generated document can be built from masks and a parameter table", "[sla_layers][sla_layer_ops_document2]")
{
    auto source = fresh_document();
    std::vector<lo::Mask> masks(3, lo::Mask(kW, kH, 0));
    for (auto &m : masks) m.px[2] = 255;
    std::vector<double> z{kLayer, 2 * kLayer, 3 * kLayer};
    auto table = source.parameter_table();
    table.resize(3);
    table[2].exposure_s = 4.5;
    table[2].is_bottom = false;

    const auto doc = sla::LayerDocument::from_parameter_masks(*source.fresh_job(), masks, z, table);
    REQUIRE(doc.layer_count() == 3);
    CHECK(value(doc, 2, Par::Exposure) == Approx(4.5));
    CHECK(value(doc, 2, Par::PositionZ) == Approx(3 * kLayer));
    CHECK(doc.fresh_job()->globals.bottom_layers == 1);

    masks[1] = lo::Mask(kW + 1, kH, 0); // wrong resolution is refused
    CHECK_THROWS(sla::LayerDocument::from_parameter_masks(*source.fresh_job(), masks, z, table));
}

// Regression: "Save layer project" of an imported native archive (CTB/CBDDLP/PHZ/...) used to fail with
// "Missing persisted native source or parameters" while SL1 worked.
TEST_CASE("Imported native archives save to a layer project and reopen with equal masks and parameters",
          "[sla_layers][sla_project][sla_native_project]")
{
    const std::string format = GENERATE("sl1", "cbddlp", "ctb_v3", "ctb_v4", "ctb_v5", "ctb_encrypted", "ctb_encrypted_v4",
                                        "phz", "pwc", "cws", "goo", "vdt", "chitubox_zip",
                                        "klipper", "photon", "fdg");
    CAPTURE(format);
    if (!get_archive_entry(format.c_str())) return;
    struct Dir {
        boost::filesystem::path path = boost::filesystem::unique_path("native-project-%%%%");
        Dir() { boost::filesystem::create_directories(path); }
        ~Dir() { boost::system::error_code e; boost::filesystem::remove_all(path, e); }
    } dir;
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA); cfg.set("sla_archive_format", format);
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.set("display_width", 16.); cfg.set("display_height", 4.);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32, 0); pixels[1] = 255;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 8, 4, 1);
    auto job = SLAPrint::create_raster_job(cfg, sla::LayerDocument::from_rasters({png, png, png}), {.05, .1, .15});
    const auto ext = get_default_extension(format.c_str());
    const auto source = dir.path / (std::string("source.") + ext), project = dir.path / "p.sla-project";
    job->export_print(source.string());
    auto document = sla::LayerDocument::load_archive(source.string());
    document.paint(1, 0, 0, 0, 255);
    sla::ProjectRasterState state{document, {}, "", std::nullopt, sla::ProjectLayerOrigin::ImportedArchive, std::nullopt};
    REQUIRE_NOTHROW(sla::save_layer_project(project.string(), state));
    auto restored = sla::read_project_layers(project.string());
    REQUIRE(restored.has_value());
    REQUIRE(restored->document.layer_count() == document.layer_count());
    for (size_t i = 0; i < document.layer_count(); ++i) {
        CHECK(restored->document.image(i).buf == document.image(i).buf);
        CHECK(restored->document.layer_parameters(i) == document.layer_parameters(i));
    }
}
