// SPDX-License-Identifier: AGPL-3.0-or-later
// LayerDocument bridge for the native UVtools layer operations (goal G6): a mask operation is one
// undo step that restores the exact PNG bytes, and a layer-list operation keeps per-layer
// parameters attached to their layer, or is refused without touching the document.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <boost/filesystem.hpp>
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerOperations.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Zipper.hpp"

using namespace Slic3r;
namespace lo = Slic3r::sla::layerops;
using F = sla::LayerParameter;

namespace {
constexpr size_t kW = 8, kH = 4;
constexpr double kLayer = 0.05; // mm

// Four 8x4 layers; each has a distinct asymmetric pattern so a flip is visible.
std::vector<sla::EncodedRaster> make_masks()
{
    std::vector<sla::EncodedRaster> layers;
    for (size_t i = 0; i < 4; ++i) {
        std::vector<uint8_t> px(kW * kH, 0);
        px[0] = 255;                  // left column marker, moved by a horizontal flip
        px[kW + 1 + i % 3] = 200;     // layer specific pixel
        layers.push_back(sla::PNGRasterEncoder{}(px.data(), kW, kH, 1));
    }
    return layers;
}

sla::FreshLayerJob make_job(sla::FreshLayerProducer producer = sla::FreshLayerProducer::Raster)
{
    sla::FreshLayerJob job;
    job.producer = producer;
    job.geometry = {kW, kH, 16., 8.};
    job.globals.layer_height = kLayer;
    job.globals.bottom_layers = 1;
    job.globals.normal = {{F::Exposure, 3.}, {F::LiftDistance, 1.}, {F::LiftSpeed, 60.}, {F::LiftDistance2, 2.},
                          {F::LiftSpeed2, 120.}, {F::RetractDistance2, .5}, {F::RetractSpeed2, 120.},
                          {F::RetractSpeed, 150.}, {F::WaitBefore, .25}, {F::WaitAfter, .5}, {F::WaitAfterLift, .75},
                          {F::LightOff, 0.}, {F::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    for (size_t i = 0; i < 4; ++i) {
        auto row = job.globals.normal;
        row[F::Exposure]  = 10. + double(i);        // identifies the layer after any re-ordering
        row[F::PositionZ] = kLayer * double(i + 1);
        job.rows.push_back(row);
    }
    return job;
}

sla::LayerDocument fresh_document(sla::FreshLayerProducer producer = sla::FreshLayerProducer::Raster)
{
    return sla::LayerDocument::from_fresh_rasters(make_masks(), make_job(producer));
}

std::vector<std::vector<uint8_t>> all_bytes(const sla::LayerDocument &doc)
{
    std::vector<std::vector<uint8_t>> result;
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const auto b = doc.encoded_layer(i);
        const auto *p = static_cast<const uint8_t *>(b.buf);
        result.emplace_back(p, p + b.sz);
    }
    return result;
}

double exposure_of(const sla::LayerDocument &doc, size_t layer) { return doc.layer_parameters(layer).at(F::Exposure); }
double z_of(const sla::LayerDocument &doc, size_t layer) { return doc.layer_parameters(layer).at(F::PositionZ); }
} // namespace

TEST_CASE("A mask operation over a range is one undo step that restores the bytes",
          "[sla_layers][sla_layer_ops_document]")
{
    auto doc = fresh_document();
    const auto before = all_bytes(doc);
    lo::FlipParams flip;
    flip.selection.first_layer = 1;
    flip.selection.last_layer  = 2;
    flip.direction = lo::FlipDirection::Horizontal;

    REQUIRE(doc.apply_mask_batch(sla::LayerMaskPlane::Main,
        [&](std::vector<lo::Mask> &layers, const sla::LayerProgress &p) { lo::flip_layers(layers, flip, p); }));
    const auto after = all_bytes(doc);
    CHECK(after[0] == before[0]);   // outside the range: bytes untouched
    CHECK(after[3] == before[3]);
    CHECK(after[1] != before[1]);
    CHECK(after[2] != before[2]);
    CHECK(doc.image(1).buf[kW - 1] == 255); // marker mirrored to the right edge

    REQUIRE(doc.undo());
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());    // exactly one step was recorded
    REQUIRE(doc.redo());
    CHECK(all_bytes(doc) == after);
}

TEST_CASE("A cancelled mask operation leaves the document and history untouched",
          "[sla_layers][sla_layer_ops_document]")
{
    auto doc = fresh_document();
    const auto before = all_bytes(doc);
    lo::FlipParams flip;
    const bool done = doc.apply_mask_batch(sla::LayerMaskPlane::Main,
        [&](std::vector<lo::Mask> &layers, const sla::LayerProgress &p) { lo::flip_layers(layers, flip, p); },
        [](size_t, size_t) { return false; });
    CHECK_FALSE(done);
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());
    CHECK(doc.revision() == 0);
}

TEST_CASE("A mask operation that changes nothing records no history", "[sla_layers][sla_layer_ops_document]")
{
    auto doc = fresh_document();
    REQUIRE(doc.apply_mask_batch(sla::LayerMaskPlane::Main, [](std::vector<lo::Mask> &, const sla::LayerProgress &) {}));
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("Clone keeps per-layer parameters attached to their source layer", "[sla_layers][sla_layer_ops_document]")
{
    auto doc = fresh_document();
    const auto before = all_bytes(doc);
    lo::CloneParams clone;
    clone.first_layer = 1;
    clone.last_layer  = 1;
    clone.clones      = 1;
    REQUIRE(doc.apply_layer_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &) {
        sla::LayerDocument::LayerListChange change;
        change.list = lo::clone_layers(layers, z, clone);
        return change;
    }, false, kLayer));

    REQUIRE(doc.layer_count() == 5);
    // Source layer 1 (exposure 11) is printed twice; the rest keep their own value.
    CHECK(exposure_of(doc, 0) == 10.);
    CHECK(exposure_of(doc, 1) == 11.);
    CHECK(exposure_of(doc, 2) == 11.);
    CHECK(exposure_of(doc, 3) == 12.);
    CHECK(exposure_of(doc, 4) == 13.);
    for (size_t i = 1; i < 5; ++i) CHECK(z_of(doc, i) > z_of(doc, i - 1)); // shifted, strictly increasing
    CHECK(doc.image(1).buf == doc.image(2).buf);

    REQUIRE(doc.undo());
    CHECK(doc.layer_count() == 4);
    CHECK(all_bytes(doc) == before);
    CHECK(exposure_of(doc, 3) == 13.);
    CHECK(z_of(doc, 3) == Catch::Approx(kLayer * 4));
    CHECK_FALSE(doc.can_undo());
    REQUIRE(doc.redo());
    CHECK(doc.layer_count() == 5);
    CHECK(exposure_of(doc, 2) == 11.);
}

TEST_CASE("Remove drops the layers together with their parameters", "[sla_layers][sla_layer_ops_document]")
{
    auto doc = fresh_document();
    lo::RemoveParams remove;
    remove.first_layer = 1;
    remove.last_layer  = 2;
    REQUIRE(doc.apply_layer_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &) {
        sla::LayerDocument::LayerListChange change;
        change.list = lo::remove_layers(layers, z, remove);
        return change;
    }, false, kLayer));
    REQUIRE(doc.layer_count() == 2);
    CHECK(exposure_of(doc, 0) == 10.);
    CHECK(exposure_of(doc, 1) == 13.);
    REQUIRE(doc.undo());
    CHECK(doc.layer_count() == 4);
    CHECK(exposure_of(doc, 2) == 12.);
}

TEST_CASE("Double exposure applies the per-layer exposure vectors and is one undo step",
          "[sla_layers][sla_layer_ops_document]")
{
    auto doc = fresh_document();
    lo::DoubleExposureParams p;
    p.bottom_layer_count = 1;
    p.first_bottom_exposure_s = 4; p.first_normal_exposure_s = 2;
    p.second_bottom_exposure_s = 5; p.second_normal_exposure_s = 3;
    lo::DoubleExposureResult expected;
    {   // Reference run on independent copies of the same data.
        std::vector<lo::Mask> masks;
        std::vector<double> z;
        for (size_t i = 0; i < 4; ++i) {
            const auto img = doc.image(i);
            lo::Mask m(img.cols, img.rows);
            m.px = img.buf;
            masks.push_back(std::move(m));
            z.push_back(z_of(doc, i));
        }
        expected = lo::double_exposure_layers(masks, z, p);
    }
    REQUIRE(doc.apply_layer_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &) {
        auto result = lo::double_exposure_layers(layers, z, p);
        sla::LayerDocument::LayerListChange change;
        change.list = std::move(result.list);
        change.exposure_s = std::move(result.exposure_s);
        change.wait_before_cure_s = std::move(result.wait_before_cure_s);
        change.lift_mm = std::move(result.lift_mm);
        change.bottom_layer_count = result.bottom_layer_count;
        return change;
    }, false, kLayer));
    REQUIRE(doc.layer_count() == expected.list.source_index.size());
    CHECK(doc.layer_count() == 8);
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const double want = expected.exposure_s[i] >= 0 ? expected.exposure_s[i]
                                                        : 10. + double(expected.list.source_index[i]);
        CHECK(exposure_of(doc, i) == Catch::Approx(want));
    }
    CHECK(doc.fresh_job()->globals.bottom_layers == expected.bottom_layer_count);
    REQUIRE(doc.undo());
    CHECK(doc.layer_count() == 4);
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("Documents without parameter rows accept list operations and resolution changes",
          "[sla_layers][sla_layer_ops_document]")
{
    auto doc = sla::LayerDocument::from_rasters(make_masks());
    REQUIRE(doc.layer_list_refusal(true).empty());
    lo::ChangeResolutionParams res;
    res.new_width = 12; res.new_height = 6;
    REQUIRE(doc.apply_layer_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &, const sla::LayerProgress &) {
        sla::LayerDocument::LayerListChange change;
        change.list.source_index = {0, 1, 2, 3};
        lo::change_resolution_layers(layers, res);
        return change;
    }, true, kLayer));
    CHECK(doc.image(0).cols == 12);
    CHECK(doc.image(0).rows == 6);
    REQUIRE(doc.undo());
    CHECK(doc.image(0).cols == kW);
    CHECK(doc.image(0).rows == kH);
}

TEST_CASE("Unrepresentable layer-list edits are refused and leave the document unchanged",
          "[sla_layers][sla_layer_ops_document]")
{
    SECTION("resolution change on a sliced/generated document")
    {
        auto doc = fresh_document();
        const auto before = all_bytes(doc);
        CHECK_FALSE(doc.layer_list_refusal(true).empty());
        CHECK_THROWS_AS(doc.apply_layer_list_operation([](std::vector<lo::Mask> &, std::vector<double> &, const sla::LayerProgress &) {
            return sla::LayerDocument::LayerListChange{};
        }, true, kLayer), RuntimeError);
        CHECK(all_bytes(doc) == before);
        CHECK(doc.revision() == 0);
    }
    SECTION("repeated Z in a model-sliced document")
    {
        // Mesh layers must stay strictly ordered, so a duplicate-Z result cannot be stored.
        auto doc = fresh_document(sla::FreshLayerProducer::ModelSlice);
        const auto before = all_bytes(doc);
        lo::CloneParams clone;
        clone.keep_same_z = true;
        CHECK_THROWS_AS(doc.apply_layer_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &) {
            sla::LayerDocument::LayerListChange change;
            change.list = lo::clone_layers(layers, z, clone);
            return change;
        }, false, kLayer), RuntimeError);
        CHECK(doc.layer_count() == 4);
        CHECK(all_bytes(doc) == before);
        CHECK_FALSE(doc.can_undo());
    }
    SECTION("inconsistent operation result")
    {
        auto doc = fresh_document();
        CHECK_THROWS_AS(doc.apply_layer_list_operation([](std::vector<lo::Mask> &layers, std::vector<double> &, const sla::LayerProgress &) {
            sla::LayerDocument::LayerListChange change;
            change.list.source_index = {0}; // one index for four layers
            return change;
        }, false, kLayer), RuntimeError);
        CHECK(doc.layer_count() == 4);
    }
    SECTION("document opened from a container")
    {
        struct File {
            boost::filesystem::path path = boost::filesystem::unique_path("ops-bridge-%%%%.sl1");
            ~File() { boost::system::error_code e; boost::filesystem::remove(path, e); }
        } file;
        const std::vector<uint8_t> pixels{0, 1, 127, 255};
        const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 2, 2, 1);
        {
            Zipper zip(file.path.string());
            zip.add_entry("config.ini"); zip << "layerHeight = 0.05\n";
            zip.add_entry("prusaslicer.ini"); zip << "display_width = 2\n";
            zip.add_entry("job00000.png", mask.data(), mask.size());
            zip.finalize();
        }
        auto doc = sla::LayerDocument::load_sl1(file.path.string());
        CHECK_FALSE(doc.layer_list_refusal(false).empty());
        CHECK_THROWS_AS(doc.apply_layer_list_operation([](std::vector<lo::Mask> &, std::vector<double> &, const sla::LayerProgress &) {
            return sla::LayerDocument::LayerListChange{};
        }, false, kLayer), RuntimeError);
        // Pixel edits of the same file are still fine.
        CHECK(doc.apply_mask_batch(sla::LayerMaskPlane::Main,
            [](std::vector<lo::Mask> &layers, const sla::LayerProgress &) { layers[0].px[0] = 255; }));
        CHECK(doc.can_undo());
    }
}

TEST_CASE("An earlier pixel edit survives undo and redo of a later list operation",
          "[sla_layers][sla_layer_ops_document]")
{
    auto doc = fresh_document();
    doc.replace_image(0, [&] { auto i = doc.image(0); i.buf[5] = 77; return i; }());
    const auto edited = all_bytes(doc);
    lo::RemoveParams remove;
    remove.first_layer = 0; remove.last_layer = 0;
    REQUIRE(doc.apply_layer_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &) {
        sla::LayerDocument::LayerListChange change;
        change.list = lo::remove_layers(layers, z, remove);
        return change;
    }, false, kLayer));
    CHECK(doc.layer_count() == 3);
    REQUIRE(doc.undo());
    CHECK(all_bytes(doc) == edited);
    REQUIRE(doc.undo()); // the pixel edit itself
    CHECK(doc.image(0).buf[5] == 0);
    REQUIRE(doc.redo());
    REQUIRE(doc.redo());
    CHECK(doc.layer_count() == 3);
}
