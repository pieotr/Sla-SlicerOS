// SPDX-License-Identifier: AGPL-3.0-or-later
// Fala 41 (Wave A): the UVtools operations that were missing in the native engine -- Blur variants, Mask by image,
// raft relief linked lines, Hershey text, support / drain-hole columns, Stir resin, Layer import, Redraw model.
// Every operation is checked on small masks with exact pixels, over a layer range, through LayerDocument (ONE undo
// step that restores the bytes, cancel leaves the document untouched) and through the JSON recipe.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

#include "libslic3r/SLA/LayerBlur.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerOperations.hpp"
#include "libslic3r/SLA/LayerRecipe.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Zipper.hpp"

using namespace Slic3r;
namespace lo = Slic3r::sla::layerops;
namespace rc = Slic3r::sla::recipe;
namespace fs = boost::filesystem;
using F    = sla::LayerParameter;
using Json = nlohmann::json;

namespace {

constexpr double kLayerMm = 0.05;

lo::Mask solid(size_t w, size_t h, uint8_t v = 0) { return lo::Mask(w, h, v); }
lo::Mask block(size_t w, size_t h, int x0, int y0, int bw, int bh, uint8_t v = 255)
{
    lo::Mask m(w, h);
    for (int y = y0; y < y0 + bh; ++y)
        for (int x = x0; x < x0 + bw; ++x) m.at(size_t(x), size_t(y)) = v;
    return m;
}
lo::Mask impulse(size_t w, size_t h, size_t x, size_t y, uint8_t v = 255) { lo::Mask m(w, h); m.at(x, y) = v; return m; }

sla::FreshLayerJob make_job(size_t count, size_t width, size_t height, size_t bottom)
{
    sla::FreshLayerJob job;
    job.producer = sla::FreshLayerProducer::Raster;
    job.geometry = {width, height, 16., 8.};
    job.globals.layer_height = kLayerMm;
    job.globals.bottom_layers = bottom;
    job.globals.normal = {{F::Exposure, 3.}, {F::LiftDistance, 1.}, {F::LiftSpeed, 60.}, {F::LiftDistance2, 2.},
                          {F::LiftSpeed2, 120.}, {F::RetractDistance2, .5}, {F::RetractSpeed2, 120.},
                          {F::RetractSpeed, 150.}, {F::WaitBefore, .25}, {F::WaitAfter, .5}, {F::WaitAfterLift, .75},
                          {F::LightOff, 0.}, {F::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    for (size_t i = 0; i < count; ++i) {
        auto row = job.globals.normal;
        row[F::Exposure]  = 10. + double(i);
        row[F::PositionZ] = kLayerMm * double(i + 1);
        job.rows.push_back(row);
    }
    return job;
}

sla::LayerDocument make_doc(const std::vector<lo::Mask> &masks, size_t bottom = 1)
{
    std::vector<sla::EncodedRaster> layers;
    for (const auto &m : masks) layers.push_back(sla::PNGRasterEncoder{}(m.px.data(), m.width, m.height, 1));
    return sla::LayerDocument::from_fresh_rasters(std::move(layers), make_job(masks.size(), masks.front().width, masks.front().height, bottom));
}

lo::Mask layer_of(const sla::LayerDocument &doc, size_t i)
{
    auto image = doc.image(i);
    lo::Mask m(size_t(image.cols), size_t(image.rows));
    m.px = std::move(image.buf);
    return m;
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

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("f41-test-%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
    std::string str(const std::string &name) const { return (path / name).string(); }
};

void write_png(const std::string &file, const lo::Mask &m)
{
    REQUIRE(png::write_gray_to_file(file, m.width, m.height, m.px));
}

std::string recipe_text(const std::string &steps) { return R"({"version": 1, "steps": [)" + steps + "]}"; }
std::string quoted(const std::string &s) { return Json(s).dump(); }

rc::RunResult run_on(const std::string &steps, sla::LayerDocument doc, const std::string &base = {}, bool dry = false)
{
    rc::RunOptions options;
    options.document = std::move(doc);
    options.dry_run  = dry;
    options.base_dir = base;
    return rc::run_recipe(rc::parse_recipe(recipe_text(steps)), options);
}

std::string error_location(const std::string &steps)
{
    try { rc::parse_recipe(recipe_text(steps)); } catch (const rc::RecipeError &e) { return e.location; }
    return {};
}

// Runs a kernel factory through the document (streamed, one undo step) and checks undo / cancel.
void check_document_contract(const std::vector<lo::Mask> &masks, const sla::LayerDocument::MaskKernelFactory &factory, size_t first, size_t last)
{
    auto doc = make_doc(masks);
    const auto before = all_bytes(doc);
    // cancel: untouched, no history
    CHECK_FALSE(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, {}, factory, [](size_t, size_t) { return false; }, {}));
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());
    REQUIRE(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, {}, factory, {}, {}));
    const auto after = all_bytes(doc);
    for (size_t i = 0; i < masks.size(); ++i) {
        if (i < first || i > last) CHECK(after[i] == before[i]);
        else CHECK(after[i] != before[i]);
    }
    REQUIRE(doc.undo());
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo()); // exactly one step
    REQUIRE(doc.redo());
    CHECK(all_bytes(doc) == after);
}

} // namespace

// ======================================================================================================
// 1. Blur variants
// ======================================================================================================

TEST_CASE("Blur: Gaussian, Box, Median on an impulse", "[sla_layers][sla_layerops][f41]")
{
    lo::BlurParams p;
    const lo::Mask in = impulse(7, 7, 3, 3);

    p.algorithm = lo::BlurAlgorithm::GaussianBlur;
    p.size = 3;
    lo::Mask out = lo::blur_mask(in, p);
    CHECK(out.at(3, 3) == 64);  // 255 * 4/16
    CHECK(out.at(2, 3) == 32);  // 255 * 2/16
    CHECK(out.at(2, 2) == 16);  // 255 * 1/16
    CHECK(out.at(1, 1) == 0);
    p.size = 1;
    CHECK(lo::blur_mask(in, p) == in); // k = 1 is the identity

    p.algorithm = lo::BlurAlgorithm::BoxBlur;
    p.size = 3;
    out = lo::blur_mask(in, p);
    for (size_t y = 2; y <= 4; ++y) for (size_t x = 2; x <= 4; ++x) CHECK(out.at(x, y) == 28); // 255 / 9
    CHECK(out.at(1, 3) == 0);

    p.algorithm = lo::BlurAlgorithm::MedianBlur;
    CHECK(lo::blur_mask(in, p) == solid(7, 7)); // the impulse is removed
}

TEST_CASE("Blur: StackBlur is a normalised triangular blur", "[sla_layers][sla_layerops][f41]")
{
    lo::BlurParams p;
    p.algorithm = lo::BlurAlgorithm::StackBlur;
    p.size = 3;
    const lo::Mask out = lo::blur_mask(impulse(7, 7, 3, 3), p);
    CHECK(out.at(3, 3) == 63); // OpenCV stack blur truncates: 63.75 -> 63, 31.9 -> 31, 15.9 -> 15
    CHECK(out.at(2, 3) == 31);
    CHECK(out.at(2, 2) == 15);
    // a constant image stays constant, grey input is preserved (no thresholding)
    CHECK(lo::blur_mask(solid(9, 5, 77), p) == solid(9, 5, 77));
}

TEST_CASE("Blur: Pyramid keeps the size, constant images and is symmetric", "[sla_layers][sla_layerops][f41]")
{
    lo::BlurParams p;
    p.algorithm = lo::BlurAlgorithm::Pyramid;
    CHECK(lo::blur_mask(solid(7, 5, 100), p) == solid(7, 5, 100));
    const lo::Mask out = lo::blur_mask(block(8, 8, 2, 2, 4, 4), p);
    REQUIRE(out.width == 8); REQUIRE(out.height == 8);
    CHECK(out != block(8, 8, 2, 2, 4, 4));                 // the described down/up step really blurs
    CHECK(out.at(0, 0) < out.at(3, 3));                      // the centre stays the brightest part
    CHECK(out.nonzero_count() > 16);                         // the energy spreads
}

TEST_CASE("Blur: Filter2D uses the raw kernel (correlation, saturating)", "[sla_layers][sla_layerops][f41]")
{
    lo::BlurParams p;
    p.algorithm = lo::BlurAlgorithm::Filter2D;
    const lo::Mask in = impulse(7, 7, 3, 3, 100);
    p.kernel_text = "0 0 0\n0 1 0\n0 0 0";
    CHECK(lo::blur_mask(in, p) == in); // identity kernel
    // correlation: dst(x,y) = sum k(i,j) * src(x+i-1, y+j-1); k(0,0)=1 -> dst(x+1,y+1) takes src(x,y)
    p.kernel_text = "1 0 0\n0 0 0\n0 0 0";
    lo::Mask out = lo::blur_mask(in, p);
    CHECK(out.at(4, 4) == 100);
    CHECK(out.at(3, 3) == 0);
    // the default all-ones kernel is a SUM (UVtools does not normalise): 3x3 of 100 saturates at 255
    p.kernel_text = "1 1 1\n1 1 1\n1 1 1";
    out = lo::blur_mask(block(7, 7, 2, 2, 3, 3, 100), p);
    CHECK(out.at(3, 3) == 255);
    CHECK(out.at(2, 2) == 255); // 4 neighbours of 100 plus itself = 400 -> saturated
    // anchor
    p.kernel_text = "1 0\n0 0";
    p.anchor_x = 0; p.anchor_y = 0;
    out = lo::blur_mask(in, p);
    CHECK(out.at(3, 3) == 100);
}

TEST_CASE("Blur: validation texts", "[sla_layers][sla_layerops][f41]")
{
    lo::BlurParams p;
    p.algorithm = lo::BlurAlgorithm::GaussianBlur;
    p.size = 4;
    CHECK(lo::validate_blur(p) == "Size must be a odd number.\n");
    p.algorithm = lo::BlurAlgorithm::StackBlur;
    CHECK_FALSE(lo::validate_blur(p).empty());
    p.algorithm = lo::BlurAlgorithm::MedianBlur;
    CHECK_FALSE(lo::validate_blur(p).empty());
    p.algorithm = lo::BlurAlgorithm::BoxBlur; // even sizes are allowed for the box filter
    CHECK(lo::validate_blur(p).empty());
    p.algorithm = lo::BlurAlgorithm::Filter2D;
    p.kernel_text = "1 1\n1";
    CHECK(lo::validate_blur(p).find("Row 2") != std::string::npos);
    p.kernel_text = "1 300";
    CHECK(lo::validate_blur(p).find("0 to 255") != std::string::npos);
    p.kernel_text = "1 1 1\n1 1 1";
    p.anchor_x = 3;
    CHECK(lo::validate_blur(p).find("Anchor position") != std::string::npos);
    p.kernel_text.clear(); p.anchor_x = -1;
    CHECK(lo::validate_blur(p).find("can't be empty") != std::string::npos);
    CHECK_THROWS_AS(lo::blur_mask(solid(4, 4), p), std::invalid_argument);
}

TEST_CASE("Blur: range, ROI, streaming == batch, undo and cancel", "[sla_layers][sla_layerops][f41]")
{
    std::vector<lo::Mask> masks;
    for (int i = 0; i < 4; ++i) masks.push_back(block(16, 12, 4 + i, 3, 6, 5));
    lo::BlurParams p;
    p.algorithm = lo::BlurAlgorithm::GaussianBlur;
    p.size = 3;
    p.selection.first_layer = 1; p.selection.last_layer = 2;

    auto batch = masks;
    lo::blur_layers(batch, p);
    CHECK(batch[0] == masks[0]);
    CHECK(batch[3] == masks[3]);
    CHECK(batch[1] == lo::blur_mask(masks[1], p));
    CHECK(batch[2] == lo::blur_mask(masks[2], p));

    SECTION("ROI: only the ROI changes, inside it equals the whole-image filter")
    {
        p.selection.roi = {4, 3, 8, 6};
        auto roi = masks;
        lo::blur_layers(roi, p);
        const lo::Mask full = lo::blur_mask(masks[1], p);
        for (size_t y = 0; y < 12; ++y)
            for (size_t x = 0; x < 16; ++x) {
                const bool inside = x >= 4 && x < 12 && y >= 3 && y < 9;
                CHECK(roi[1].at(x, y) == (inside ? full.at(x, y) : masks[1].at(x, y)));
            }
        p.selection.roi = {10, 0, 10, 4}; // sticks out of the layer
        CHECK_THROWS_AS(lo::blur_layers(roi, p), std::invalid_argument);
    }
    SECTION("document: one undo step, cancel leaves everything")
    {
        check_document_contract(masks, [p](const lo::StreamLayout &l) { return lo::blur_kernel(p, l); }, 1, 2);
    }
}

TEST_CASE("Blur: recipe round trip equals the API and errors point at the key", "[sla_layers][sla_layerops][f41]")
{
    std::vector<lo::Mask> masks(3, block(16, 12, 4, 3, 6, 5));
    auto doc = make_doc(masks);
    const auto result = run_on(R"({"op": "blur", "algorithm": "gaussian_blur", "size": 3, "first_layer": 1, "last_layer": 1})", std::move(doc));
    lo::BlurParams p;
    p.algorithm = lo::BlurAlgorithm::GaussianBlur; p.size = 3;
    REQUIRE(result.document.has_value());
    CHECK(layer_of(*result.document, 1) == lo::blur_mask(masks[1], p));
    CHECK(layer_of(*result.document, 0) == masks[0]);

    CHECK(error_location(R"({"op": "blur", "algorithm": "gaussian_blur", "size": 4})") == "steps[0].size");
    CHECK(error_location(R"({"op": "blur", "algorithm": "lanczos"})") == "steps[0].algorithm");
    CHECK(error_location(R"({"op": "blur", "algorithm": "filter_2d", "kernel": "1 2\n3"})") == "steps[0].kernel");
    CHECK(error_location(R"({"op": "blur", "algorithm": "box_blur", "size": 4})").empty());
}

// ======================================================================================================
// 2. Mask by image
// ======================================================================================================

TEST_CASE("Mask by image: out = round(in * mask / 255)", "[sla_layers][sla_layerops][f41]")
{
    lo::MaskImageParams p;
    p.image = lo::Mask(4, 2);
    const uint8_t mask_values[8] = {255, 128, 0, 255, 100, 255, 128, 0};
    const uint8_t in_values[8]   = {0, 64, 128, 255, 200, 255, 255, 77};
    std::copy(mask_values, mask_values + 8, p.image.px.begin());
    lo::Mask in(4, 2);
    std::copy(in_values, in_values + 8, in.px.begin());

    std::vector<lo::Mask> layers{in};
    lo::mask_image_layers(layers, p);
    const uint8_t expected[8] = {0, 32, 0, 255, 78 /* 200*100/255 = 78.4 */, 255, 128 /* 255*128/255 */, 0};
    for (size_t i = 0; i < 8; ++i) CHECK(layers[0].px[i] == expected[i]);
    // 128 * 128 / 255 = 64.25 -> 64
    layers = {solid(4, 2, 128)};
    p.image = solid(4, 2, 128);
    lo::mask_image_layers(layers, p);
    CHECK(layers[0] == solid(4, 2, 64));
    // exhaustive: every (in, mask) pair equals lround(in*mask/255)
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned m = 0; m < 256; m += 5) {
            std::vector<lo::Mask> one{solid(1, 1, uint8_t(a))};
            lo::MaskImageParams q;
            q.image = solid(1, 1, uint8_t(m));
            lo::mask_image_layers(one, q);
            REQUIRE(one[0].px[0] == uint8_t(std::lround(double(a) * double(m) / 255.0)));
        }
    // invert: 255 - mask
    p.image = solid(4, 2, 255);
    p.invert = true;
    layers = {solid(4, 2, 200)};
    lo::mask_image_layers(layers, p);
    CHECK(layers[0] == solid(4, 2, 0));
}

TEST_CASE("Mask by image: range, ROI, size mismatch is an error, document contract", "[sla_layers][sla_layerops][f41]")
{
    std::vector<lo::Mask> masks(4, solid(8, 6, 200));
    lo::MaskImageParams p;
    p.image = solid(8, 6, 128);
    p.selection.first_layer = 1; p.selection.last_layer = 2;
    auto layers = masks;
    lo::mask_image_layers(layers, p);
    CHECK(layers[0] == masks[0]);
    CHECK(layers[3] == masks[3]);
    CHECK(layers[1] == solid(8, 6, 100)); // 200 * 128 / 255 = 100.4

    // the mask has the size of the ROI, pixels outside are untouched
    p.image = solid(3, 2, 0);
    p.selection.roi = {2, 1, 3, 2};
    layers = masks;
    lo::mask_image_layers(layers, p);
    CHECK(layers[1].at(2, 1) == 0);
    CHECK(layers[1].at(4, 2) == 0);
    CHECK(layers[1].at(5, 1) == 200);
    CHECK(layers[1].at(1, 1) == 200);

    // UVtools silently skips a size mismatch; we refuse
    p.selection.roi = {};
    CHECK_THROWS_AS(lo::mask_image_layers(layers, p), std::invalid_argument);
    CHECK(lo::validate_mask_image(p, 8, 6).find("3x2 but the layers") != std::string::npos);
    p.image = {};
    CHECK(lo::validate_mask_image(p, 8, 6) == "The mask can not be empty.\n");

    p.image = solid(8, 6, 128);
    p.selection.roi = {};
    check_document_contract(masks, [p](const lo::StreamLayout &l) { return lo::mask_image_kernel(p, l); }, 1, 2);
}

TEST_CASE("Mask by image: recipe (PNG, invert, fit) round trip", "[sla_layers][sla_layerops][f41]")
{
    TempDir dir;
    std::vector<lo::Mask> masks(2, solid(8, 6, 200));
    write_png(dir.str("m.png"), solid(8, 6, 128));
    write_png(dir.str("small.png"), solid(4, 3, 128));

    auto result = run_on(R"({"op": "mask_image", "image": "m.png", "first_layer": 1})", make_doc(masks), dir.path.string());
    REQUIRE(result.ok);
    CHECK(layer_of(*result.document, 0) == masks[0]);
    CHECK(layer_of(*result.document, 1) == solid(8, 6, 100));

    result = run_on(R"({"op": "mask_image", "image": "m.png", "invert": true, "last_layer": 0})", make_doc(masks), dir.path.string());
    REQUIRE(result.ok);
    CHECK(layer_of(*result.document, 0) == solid(8, 6, 100)); // 200 * 127 / 255 = 99.6 -> 100

    result = run_on(R"({"op": "mask_image", "image": "small.png"})", make_doc(masks), dir.path.string());
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("4x3") != std::string::npos);
    result = run_on(R"({"op": "mask_image", "image": "small.png", "resize_to_layer": true})", make_doc(masks), dir.path.string());
    REQUIRE(result.ok);
    CHECK(layer_of(*result.document, 0).at(3, 3) == 100);

    CHECK(error_location(R"({"op": "mask_image"})") == "steps[0].image");
    CHECK(error_location(R"({"op": "mask_image", "image": "a.png", "invert": 1})") == "steps[0].invert");
    result = run_on(R"({"op": "mask_image", "image": "missing.png"})", make_doc(masks), dir.path.string());
    CHECK_FALSE(result.ok);
}

// ======================================================================================================
// 3. Raft relief: linked lines
// ======================================================================================================

namespace {
// Layers 0..1 are a big plate, layer 2 has six separate 6x6 supports (two rows of three).
std::vector<lo::Mask> linked_lines_stack()
{
    std::vector<lo::Mask> layers(3, block(120, 120, 5, 5, 110, 110));
    layers[2] = solid(120, 120);
    const int xs[3] = {20, 55, 90}, ys[2] = {30, 80};
    for (int y : ys) for (int x : xs)
        for (int j = 0; j < 6; ++j) for (int i = 0; i < 6; ++i) layers[2].at(size_t(x + i), size_t(y + j)) = 255;
    return layers;
}
lo::RaftReliefParams linked_params()
{
    lo::RaftReliefParams p;
    p.type = lo::RaftReliefType::LinkedLines;
    p.mask_layer_index = 2;
    p.dilate_iterations = 0;
    p.linked_line_thickness = 4;
    p.linked_minimum_links = 2;
    p.linked_external_supports = false;
    return p;
}
} // namespace

TEST_CASE("Raft relief linked lines: supports joined by lines", "[sla_layers][sla_layerops][f41]")
{
    auto p = linked_params();
    const auto source = linked_lines_stack();
    auto layers = source;
    REQUIRE(lo::raft_relief_layers(layers, p));
    CHECK(layers[0] == layers[1]);       // every raft layer gets the same picture
    CHECK(layers[2] == source[2]);       // the supports layer itself is untouched
    for (size_t y = 0; y < 120; ++y)
        for (size_t x = 0; x < 120; ++x)
            if (source[2].at(x, y)) CHECK(layers[0].at(x, y) == 255); // every support pixel is lit
    // supports (22,32) and (57,32) are nearest neighbours (distance 35): the midpoint of the centroid segment is lit
    CHECK(layers[0].at(40, 32) == 255);
    CHECK(layers[0].at(40, 33) == 255);
    CHECK(layers[0].nonzero_count() > source[2].nonzero_count());
    CHECK(layers[0].at(1, 1) == 0);      // far corner stays empty
    CHECK(layers[0].at(110, 118) == 0);

    // deterministic
    auto again = source;
    REQUIRE(lo::raft_relief_layers(again, p));
    CHECK(again[0] == layers[0]);

    // fewer links, fewer lit pixels
    auto one = source;
    p.linked_minimum_links = 1;
    REQUIRE(lo::raft_relief_layers(one, p));
    CHECK(one[0].nonzero_count() < layers[0].nonzero_count());
    // external supports add the perimeter web
    p.linked_minimum_links = 2;
    p.linked_external_supports = true;
    auto ext = source;
    REQUIRE(lo::raft_relief_layers(ext, p));
    CHECK(ext[0].nonzero_count() > layers[0].nonzero_count());
}

TEST_CASE("Raft relief linked lines: link colour and thickness", "[sla_layers][sla_layerops][f41]")
{
    auto p = linked_params();
    p.high_brightness = 128;
    auto layers = linked_lines_stack();
    REQUIRE(lo::raft_relief_layers(layers, p));
    CHECK(layers[0].at(40, 32) == 128);  // links use the dim colour
    CHECK(layers[0].at(22, 32) == 255);  // supports keep their own value
    // thickness below 4 is refused by the engine (the recipe clamps like UVtools' setter)
    p.linked_line_thickness = 3;
    CHECK(lo::validate_raft_relief(p) == "The link thickness can't be less than 4\n");
    CHECK_THROWS_AS(lo::raft_relief_layers(layers, p), std::invalid_argument);
    // ignore_first_layers keeps layer 0
    p = linked_params();
    p.ignore_first_layers = 1;
    layers = linked_lines_stack();
    REQUIRE(lo::raft_relief_layers(layers, p));
    CHECK(layers[0] == linked_lines_stack()[0]);
    CHECK(layers[1] != linked_lines_stack()[1]);
}

TEST_CASE("Raft relief linked lines: document is one undo step and the recipe matches", "[sla_layers][sla_layerops][f41]")
{
    const auto source = linked_lines_stack();
    auto p = linked_params();
    check_document_contract(source, [p](const lo::StreamLayout &l) {
        auto k = lo::raft_relief_kernel(p, l);
        REQUIRE(k.has_value());
        return *k;
    }, 0, 1);

    auto expect = source;
    REQUIRE(lo::raft_relief_layers(expect, p));
    const auto result = run_on(R"({"op": "raft_relief", "type": "linked_lines", "mask_layer_index": 2, "dilate_iterations": 0,
                                   "linked_line_thickness": 4, "linked_minimum_links": 2, "linked_external_supports": false})", make_doc(source));
    REQUIRE(result.ok);
    for (size_t i = 0; i < 3; ++i) CHECK(layer_of(*result.document, i) == expect[i]);
    // a thickness below 4 is clamped, not rejected
    CHECK(error_location(R"({"op": "raft_relief", "type": "linked_lines", "linked_line_thickness": 1})").empty());
    CHECK(error_location(R"({"op": "raft_relief", "type": "linked_lines", "linked_external_supports": "yes"})") == "steps[0].linked_external_supports");
}

// ======================================================================================================
// 4. Hershey text engine (shared renderer + UVtools PutTextExtended / PutTextRotated rules)
// ======================================================================================================

#include "libslic3r/SLA/HersheyText.hpp"
namespace ht = Slic3r::sla::hershey_text;

namespace {
ht::TextSpec text_spec(const std::string &text, ht::LineType type = ht::LineType::EightConnected, double scale = 1, int thickness = 1)
{
    ht::TextSpec s;
    s.text = text; s.scale = scale; s.thickness = thickness; s.line_type = type;
    return s;
}
lo::Mask draw(size_t w, size_t h, int x, int y, const ht::TextSpec &spec, uint8_t value = 255, lo::Mask base = {})
{
    lo::Mask m = base.px.empty() ? lo::Mask(w, h) : base;
    ht::put_text(m.px.data(), int(h), int(w), x, y, spec, value);
    return m;
}
} // namespace

TEST_CASE("Hershey text: extent, bounds and determinism", "[sla_layers][sla_hershey][f41]")
{
    const auto spec = text_spec("A");
    const lo::Mask a = draw(40, 30, 5, 20, spec);
    REQUIRE(a.nonzero_count() > 10);
    CHECK(a == draw(40, 30, 5, 20, spec)); // deterministic
    const lo::Rect box = lo::bounding_rect(a);
    const auto ext = ht::text_extent("A", 0, 1, 1);
    CHECK(ext.width > 0); CHECK(ext.height > 0);
    CHECK(box.x >= 5); CHECK(box.right() <= 5 + ext.width);
    CHECK(box.bottom() <= 20 + ext.baseline + 1);           // glyph sits on the baseline (origin = baseline-left)
    CHECK(box.y >= 20 - ext.height);
    // anti-aliased text produces grey pixels, aliased text only the requested value
    const lo::Mask aa = draw(40, 30, 5, 20, text_spec("A", ht::LineType::AntiAlias));
    bool grey = false;
    for (uint8_t v : aa.px) grey |= v != 0 && v != 255;
    CHECK(grey);
    for (uint8_t v : a.px) CHECK((v == 0 || v == 255));
    // clipping: text partly outside the image does not throw
    CHECK_NOTHROW(draw(40, 30, 35, 5, text_spec("WWW", ht::LineType::AntiAlias, 2)));
    CHECK_NOTHROW(draw(40, 30, -10, -4, text_spec("WWW")));
}

TEST_CASE("Hershey text: multi-line alignment follows UVtools", "[sla_layers][sla_hershey][f41]")
{
    // "WWWW\nW": the second line is moved by (w1 - w2) / 2 (Center) or (w1 - w2) (Right); the line step is h + h / 3.
    const int x = 6, y = 20;
    const auto one = ht::text_extent("W", 0, 1, 1), four = ht::text_extent("WWWW", 0, 1, 1);
    const int step = one.height + one.height / 3;
    for (auto [alignment, shift] : {std::pair{ht::Alignment::Left, 0}, {ht::Alignment::Center, (four.width - one.width) / 2}, {ht::Alignment::Right, four.width - one.width}}) {
        auto spec = text_spec("WWWW\nW");
        spec.alignment = alignment;
        lo::Mask expected(80, 60);
        ht::put_text(expected.px.data(), 60, 80, x, y, text_spec("WWWW"), 255);
        ht::put_text(expected.px.data(), 60, 80, x + shift, y + step, text_spec("W"), 255);
        CHECK(draw(80, 60, x, y, spec) == expected);
    }
    // trailing blank lines are ignored, blank lines in the middle only advance the line
    auto trailing = text_spec("WW\n\n");
    CHECK(draw(80, 60, x, y, trailing) == draw(80, 60, x, y, text_spec("WW")));
    // the block extent: height = lines * (gap + h) - gap
    auto two = text_spec("A\nA");
    CHECK(ht::block_extent(two).height == 2 * step - one.height / 3);
}

TEST_CASE("Hershey text: rotation by multiples of 90 degrees is exact, mirror flips vertically", "[sla_layers][sla_hershey][f41]")
{
    const int W = 121, H = 121, cx = 60, cy = 60;
    for (auto type : {ht::LineType::EightConnected, ht::LineType::AntiAlias}) {
        auto spec = text_spec("Ab1", type, 1, 2);
        const lo::Mask upright = draw(W, H, cx, cy, spec);
        REQUIRE(upright.nonzero_count() > 20);
        // positive angle = clockwise: (dx, dy) -> (-dy, dx) for 90, (-dx, -dy) for 180, (dy, -dx) for 270
        for (int angle : {90, 180, 270, -90}) {
            spec.angle = angle;
            const lo::Mask rotated = draw(W, H, cx, cy, spec);
            lo::Mask expected(W, H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const int dx = x - cx, dy = y - cy;
                    int tx = dx, ty = dy;
                    const int a = ((angle % 360) + 360) % 360;
                    if (a == 90) { tx = -dy; ty = dx; }
                    else if (a == 180) { tx = -dx; ty = -dy; }
                    else { tx = dy; ty = -dx; }
                    if (cx + tx >= 0 && cx + tx < W && cy + ty >= 0 && cy + ty < H) expected.at(size_t(cx + tx), size_t(cy + ty)) = upright.at(size_t(x), size_t(y));
                }
            CHECK(rotated == expected);
        }
    }
    // 45 degrees keeps the pixel count in the same ballpark and stays inside the picture
    auto spec = text_spec("Ab1", ht::LineType::AntiAlias, 1, 2);
    const size_t straight = draw(W, H, cx, cy, spec).nonzero_count();
    spec.angle = 45;
    const size_t turned = draw(W, H, cx, cy, spec).nonzero_count();
    CHECK(turned > straight / 2);
    CHECK(turned < straight * 3);
    // mirror (bottomLeftOrigin): reflected about the baseline row
    spec = text_spec("Ab1", ht::LineType::EightConnected, 1, 2);
    const lo::Rect up = lo::bounding_rect(draw(W, H, cx, cy, spec));
    spec.mirror = true;
    const lo::Rect down = lo::bounding_rect(draw(W, H, cx, cy, spec));
    CHECK(std::abs((down.y + down.bottom()) - (2 * cy * 2 - (up.y + up.bottom()))) <= 3);
    CHECK(down.h == up.h);
    CHECK(down.y + down.h / 2 > cy);  // below the baseline now
}

TEST_CASE("Hershey text: brightness, remove brightness, validation", "[sla_layers][sla_hershey][f41]")
{
    auto spec = text_spec("Hi", ht::LineType::EightConnected, 1, 2);
    lo::Mask m = draw(40, 30, 4, 22, spec, 200);
    for (uint8_t v : m.px) CHECK((v == 0 || v == 200));
    const lo::Mask erased = draw(40, 30, 4, 22, spec, 0, m);
    CHECK(erased.nonzero_count() == 0); // "remove" = same text with brightness 0
    CHECK(ht::validate_text(text_spec("")) == "The text can not be empty.\n");
    CHECK_FALSE(ht::validate_text(text_spec("x", ht::LineType::AntiAlias, 0.1)).empty());
    auto bad = text_spec("x"); bad.face = 9;
    CHECK_FALSE(ht::validate_text(bad).empty());
    CHECK(ht::validate_text(text_spec("ok", ht::LineType::AntiAlias, 0.2, 1)).empty());
    for (int face : {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23}) {
        auto f = text_spec("Ag"); f.face = face;
        CHECK(draw(60, 40, 4, 25, f).nonzero_count() > 5);
    }
}

TEST_CASE("Text tool: LayerDocument::paint_shape draws over a range as one undo step", "[sla_layers][sla_hershey][f41]")
{
    std::vector<lo::Mask> masks(4, solid(60, 40));
    auto doc = make_doc(masks);
    const auto before = all_bytes(doc);
    sla::LayerDocument::ShapeOp op;
    op.tool = sla::LayerDocument::ShapeTool::Text;
    op.x0 = 4; op.y0 = 25; op.value = 255;
    op.text = std::make_shared<const ht::TextSpec>(text_spec("Go", ht::LineType::EightConnected, 1, 2));
    REQUIRE(doc.paint_shape(1, 2, op));
    const auto after = all_bytes(doc);
    CHECK(after[0] == before[0]);
    CHECK(after[3] == before[3]);
    CHECK(after[1] != before[1]);
    CHECK(after[1] == after[2]);
    CHECK(layer_of(doc, 1) == draw(60, 40, 4, 25, text_spec("Go", ht::LineType::EightConnected, 1, 2)));
    REQUIRE(doc.undo());
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());
    // eraser: the "remove" brightness is the op value
    op.value = 0;
    REQUIRE(doc.paint_shape(1, 2, op)); // nothing to remove: no history
    CHECK_FALSE(doc.can_undo());
    // no text / empty text is refused and changes nothing
    op.text = nullptr;
    CHECK_THROWS(doc.paint_shape(0, 0, op));
    op.text = std::make_shared<const ht::TextSpec>(text_spec(""));
    CHECK_THROWS(doc.paint_shape(0, 0, op));
    CHECK_FALSE(doc.can_undo());
}

// ======================================================================================================
// 5. Supports / Drain holes: column digging
// ======================================================================================================

namespace {
size_t disc_pixels(const lo::Mask &m) { return m.nonzero_count(); }
png::ImageGreyscale grey_image(const lo::Mask &m) { png::ImageGreyscale i; i.rows = m.height; i.cols = m.width; i.buf = m.px; return i; }
} // namespace

TEST_CASE("Supports: per-layer step radius sequence and the touch rule", "[sla_layers][sla_columns][f41]")
{
    using D = sla::LayerDocument;
    D::SupportSpec spec;
    spec.anti_alias = false;
    // empty layer 12: first disc radius min(19 + 0, 32) / 2 = 9, then 10 (drawn = 1), capped at 16
    auto img = grey_image(solid(128, 128));
    CHECK(D::support_layer_step(img, 64, 64, 12, 0, spec) == D::ColumnStep::Drew);
    lo::Mask m; m.width = 128; m.height = 128; m.px = img.buf;
    const lo::Rect r0 = lo::bounding_rect(m);
    CHECK(r0.w == 19); CHECK(r0.h == 19);
    img = grey_image(solid(128, 128));
    D::support_layer_step(img, 64, 64, 11, 1, spec);
    m.px = img.buf; CHECK(lo::bounding_rect(m).w == 21);
    img = grey_image(solid(128, 128));
    D::support_layer_step(img, 64, 64, 11, 40, spec);
    m.px = img.buf; CHECK(lo::bounding_rect(m).w == 33);   // capped at the pillar diameter 32 -> radius 16
    // layers 0..10 use the base diameter 60 -> radius 30
    img = grey_image(solid(128, 128));
    D::support_layer_step(img, 64, 64, 10, 3, spec);
    m.px = img.buf; CHECK(lo::bounding_rect(m).w == 61);
    CHECK(disc_pixels(m) > 2800);
    // border: clipped, no exception
    img = grey_image(solid(128, 128));
    CHECK(D::support_layer_step(img, 2, 2, 12, 0, spec) == D::ColumnStep::Drew);
    CHECK(img.buf[2 * 128 + 2] == 255);

    // material under the tip: >= tip^2 / 3 lit pixels in the tip circle
    img = grey_image(block(128, 128, 50, 50, 30, 30));
    const auto before = img.buf;
    CHECK(D::support_layer_step(img, 64, 64, 12, 0, spec) == D::ColumnStep::Skipped); // nothing drawn yet: keep digging
    CHECK(D::support_layer_step(img, 64, 64, 12, 2, spec) == D::ColumnStep::Finished); // supported: stop
    CHECK(img.buf == before);
    // a sliver below one third does not count
    img = grey_image(block(128, 128, 64, 50, 4, 30));
    CHECK(D::support_layer_step(img, 64, 64, 12, 2, spec) == D::ColumnStep::Drew);
}

TEST_CASE("Supports: dig_column through the document", "[sla_layers][sla_columns][f41]")
{
    std::vector<lo::Mask> masks(14, solid(128, 128));
    for (size_t i = 0; i < 2; ++i) masks[i] = block(128, 128, 20, 20, 90, 90); // a plate below layers 0..1
    auto doc = make_doc(masks);
    const auto before = all_bytes(doc);
    sla::LayerDocument::SupportSpec spec;
    spec.anti_alias = false;
    REQUIRE(doc.dig_column(13, 64, 64, spec));
    const auto after = all_bytes(doc);
    CHECK(after[13] == before[13]);              // the clicked layer is not modified
    CHECK(after[0] == before[0]);                // the plate was reached: the support ends above it
    CHECK(after[1] == before[1]);
    for (size_t l = 2; l <= 12; ++l) {
        CHECK(after[l] != before[l]);
        const lo::Mask m = layer_of(doc, l);
        CHECK(m.at(64, 64) == 255);
        const int drawn = int(12 - l);
        const int width = (l > 10 ? std::min(19 + drawn, 32) : 60) / 2 * 2 + 1;
        CHECK(lo::bounding_rect(m).w == width);
    }
    REQUIRE(doc.undo());
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());                 // ONE undo step for all layers

    // layer 0 is ignored, cancel leaves everything, a point outside is an error
    CHECK(doc.dig_column(0, 64, 64, spec));
    CHECK_FALSE(doc.can_undo());
    CHECK_FALSE(doc.dig_column(13, 64, 64, spec, sla::LayerMaskPlane::Main, [](size_t, size_t) { return false; }));
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());
    CHECK_THROWS(doc.dig_column(13, 500, 64, spec));
    CHECK_THROWS(doc.dig_column(14, 64, 64, spec));
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("Drain holes: drill through a slab until the hollow space", "[sla_layers][sla_columns][f41]")
{
    using D = sla::LayerDocument;
    // layers 0..2 are hollow (empty), 3..9 a full slab
    std::vector<lo::Mask> masks(10, solid(128, 128, 255));
    for (size_t i = 0; i < 3; ++i) masks[i] = solid(128, 128);
    auto doc = make_doc(masks);
    const auto before = all_bytes(doc);
    D::DrainSpec spec;
    spec.anti_alias = false;
    REQUIRE(doc.dig_column(9, 64, 64, spec));
    const auto after = all_bytes(doc);
    CHECK(after[9] == before[9]);
    for (size_t l = 3; l <= 8; ++l) {
        const lo::Mask m = layer_of(doc, l);
        CHECK(m.at(64, 64) == 0);
        CHECK(m.at(64 + 25, 64) == 0);
        CHECK(m.at(64 + 27, 64) == 255);
        CHECK(m.at(10, 10) == 255);
        CHECK(lo::bounding_rect(m).w == 128);
    }
    for (size_t l = 0; l < 3; ++l) CHECK(after[l] == before[l]); // the hollow layers stay empty
    REQUIRE(doc.undo());
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());

    // pure step: enough dark pixels under the circle -> Skipped (nothing drilled yet) / Finished (after drilling)
    auto img = grey_image(solid(128, 128));
    CHECK(D::drain_layer_step(img, 64, 64, 0, spec) == D::ColumnStep::Skipped);
    CHECK(D::drain_layer_step(img, 64, 64, 2, spec) == D::ColumnStep::Finished);
    img = grey_image(solid(128, 128, 255));
    CHECK(D::drain_layer_step(img, 64, 64, 0, spec) == D::ColumnStep::Drew);
    CHECK(img.buf[64 * 128 + 64] == 0);
    // grey pixels at or below 100 count as empty
    img = grey_image(solid(128, 128, 100));
    CHECK(D::drain_layer_step(img, 64, 64, 1, spec) == D::ColumnStep::Finished);
    img = grey_image(solid(128, 128, 101));
    CHECK(D::drain_layer_step(img, 64, 64, 1, spec) == D::ColumnStep::Drew);
    // border: clipped
    img = grey_image(solid(128, 128, 255));
    CHECK(D::drain_layer_step(img, 3, 3, 0, spec) == D::ColumnStep::Drew);
    CHECK(img.buf[3 * 128 + 3] == 0);
}

// ======================================================================================================
// 6. Stir resin
// ======================================================================================================

#include "libslic3r/SLA/LayerListStream.hpp"

namespace {
double param(const sla::LayerDocument &doc, size_t layer, sla::LayerParameter field) { return doc.layer_parameters(layer).at(field); }
std::vector<lo::Mask> three_layers(size_t w = 16, size_t h = 8)
{
    std::vector<lo::Mask> masks;
    for (size_t i = 0; i < 3; ++i) masks.push_back(block(w, h, 3 + int(i), 2, 6, 4));
    return masks;
}
} // namespace

TEST_CASE("Stir resin: stir and print puts the stirs in front, idempotently", "[sla_layers][sla_stir][f41]")
{
    const auto masks = three_layers();
    auto result = run_on(R"({"op": "stir_resin", "method": "stir_and_print", "stirs": 3, "lift_speed": 120, "wait_time_before_stir_s": 1.5, "wait_time_after_lift_s": 0.5})", make_doc(masks));
    REQUIRE(result.ok);
    const auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 6);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(param(doc, i, F::Exposure) == 0.01);
        CHECK(param(doc, i, F::LiftDistance) == 45);
        CHECK(param(doc, i, F::LiftDistance2) == 0);
        CHECK(param(doc, i, F::LiftSpeed) == 120);
        CHECK(param(doc, i, F::RetractSpeed) == 120); // retract speed 0 -> lift speed
        CHECK(param(doc, i, F::WaitBefore) == 1.5);
        CHECK(param(doc, i, F::WaitAfter) == 0);
        CHECK(param(doc, i, F::WaitAfterLift) == 0.5);
        CHECK(param(doc, i, F::LightPWM) == 1);
        CHECK(param(doc, i, F::PositionZ) == 0.001);
        CHECK(layer_of(doc, i).nonzero_count() == 1); // dummy pixel
    }
    for (size_t i = 0; i < 3; ++i) {                  // the print is intact
        CHECK(layer_of(doc, 3 + i) == masks[i]);
        CHECK(param(doc, 3 + i, F::Exposure) == 10.0 + double(i));
        CHECK(param(doc, 3 + i, F::PositionZ) == Catch::Approx(0.05 * double(i + 1)));
        CHECK(param(doc, 3 + i, F::LiftDistance) == 1);
    }
    // the second run removes the previous stir block first: still 6 layers
    auto again = run_on(R"({"op": "stir_resin", "method": "stir_and_print", "stirs": 3}, {"op": "stir_resin", "method": "stir_and_print", "stirs": 3})", make_doc(masks));
    REQUIRE(again.ok);
    CHECK(again.document->layer_count() == 6);
    // more stirs the second time replace the block, they do not pile up
    again = run_on(R"({"op": "stir_resin", "method": "stir_and_print", "stirs": 3}, {"op": "stir_resin", "method": "stir_and_print", "stirs": 5})", make_doc(masks));
    REQUIRE(again.ok);
    CHECK(again.document->layer_count() == 8);
}

TEST_CASE("Stir resin: stir only replaces the print, no dummy pixel gives blank layers", "[sla_layers][sla_stir][f41]")
{
    const auto masks = three_layers();
    auto result = run_on(R"({"op": "stir_resin", "method": "stir_only", "stirs": 4, "output_dummy_pixel": false, "exposure_s": 0.2, "lift_height_mm": 10, "retract_speed": 300})", make_doc(masks));
    REQUIRE(result.ok);
    const auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 4);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(layer_of(doc, i).nonzero_count() == 0);
        CHECK(param(doc, i, F::Exposure) == 0.2);
        CHECK(param(doc, i, F::LiftDistance) == 10);
        CHECK(param(doc, i, F::RetractSpeed) == 300);
    }
    // validation, error locations, the dry run changes nothing
    CHECK(error_location(R"({"op": "stir_resin", "stirs": 0})") == "steps[0].stirs");
    CHECK(error_location(R"({"op": "stir_resin", "method": "spin"})") == "steps[0].method");
    CHECK(error_location(R"({"op": "stir_resin", "lift_height_mm": -1})") == "steps[0].lift_height_mm");
    const auto dry = run_on(R"({"op": "stir_resin", "stirs": 2})", make_doc(masks), {}, true);
    CHECK(dry.ok);
    CHECK_FALSE(dry.document.has_value());
}

TEST_CASE("Stir resin: engine rules, undo and Z validation", "[sla_layers][sla_stir][f41]")
{
    lo::StirResinParams p;
    p.stirs = 0;
    lo::PrinterContext ctx;
    CHECK(lo::validate_stir_resin(p, ctx) == "The number of stirs must be greater than 0.\n");
    p.stirs = 10; p.lift_height_mm = 5;
    ctx.same_layer_z_allowed = false;
    CHECK(lo::validate_stir_resin(p, ctx).empty());           // 10 * 0.001 < 5
    p.stirs = 6000;
    CHECK_FALSE(lo::validate_stir_resin(p, ctx).empty());     // 6 mm of Z steps do not fit below the 5 mm lift
    p.stirs = 3; p.method = lo::StirMethod::StirAndPrint;
    CHECK(lo::validate_stir_resin(p, ctx).find("not compatible") != std::string::npos);
    ctx.same_layer_z_allowed = true;
    CHECK(lo::validate_stir_resin(p, ctx).empty());
    CHECK(lo::is_dummy_layer(1, 5.0));
    CHECK(lo::is_dummy_layer(100, 0.01));
    CHECK_FALSE(lo::is_dummy_layer(2, 0.02));
    // stir seconds = light-off delay of one stir times the number of stirs
    p.stirs = 2; p.lift_height_mm = 10; p.lift_speed = 60; p.retract_speed = 120; p.exposure_s = 1;
    CHECK(lo::stir_resin_seconds(p, ctx) == Catch::Approx(2 * (10.0 + 5.0 + 1.0)));

    // one undo step on a document
    auto doc = make_doc(three_layers());
    const auto before = all_bytes(doc);
    lo::LayerParams base; base.exposure_s = 3;
    const auto context = doc.parameter_printer_context(kLayerMm);
    lo::StirResinParams q; q.stirs = 2; q.method = lo::StirMethod::StirAndPrint;
    REQUIRE(doc.apply_parameter_list_stream(sla::liststream::stir_resin(q, context, base), kLayerMm));
    CHECK(doc.layer_count() == 5);
    REQUIRE(doc.undo());
    CHECK(all_bytes(doc) == before);
    CHECK_FALSE(doc.can_undo());
    // cancel leaves the document alone
    CHECK_FALSE(doc.apply_parameter_list_stream(sla::liststream::stir_resin(q, context, base), kLayerMm, [](size_t, size_t) { return false; }));
    CHECK(doc.layer_count() == 3);
    CHECK_FALSE(doc.can_undo());
}

// ======================================================================================================
// 7. Layer import
// ======================================================================================================

namespace {
lo::LayerImportParams import_params(lo::LayerImportType type, size_t start, std::vector<lo::Mask> layers)
{
    lo::LayerImportParams p;
    p.type = type; p.start_layer = start;
    p.sources.push_back({"source", std::move(layers)});
    return p;
}
std::vector<double> z_for(size_t n) { std::vector<double> z; for (size_t i = 0; i < n; ++i) z.push_back(0.05 * double(i + 1)); return z; }
} // namespace

TEST_CASE("Layer import tier 1: per pixel types on a 2 layer source starting at layer 1", "[sla_layers][sla_import][f41]")
{
    struct Case { lo::LayerImportType type; uint8_t target, imported, expected; };
    const Case cases[] = {{lo::LayerImportType::Replace, 200, 100, 100}, {lo::LayerImportType::MergeSum, 200, 100, 255}, {lo::LayerImportType::MergeSum, 50, 100, 150},
                          {lo::LayerImportType::MergeMax, 50, 100, 100}, {lo::LayerImportType::Subtract, 100, 200, 0}, {lo::LayerImportType::Subtract, 200, 100, 100},
                          {lo::LayerImportType::AbsDiff, 100, 200, 100}, {lo::LayerImportType::BitwiseAnd, 0xF0, 0x3C, 0x30}, {lo::LayerImportType::BitwiseOr, 0xF0, 0x3C, 0xFC},
                          {lo::LayerImportType::BitwiseXor, 0xF0, 0x3C, 0xCC}};
    for (const Case &c : cases) {
        std::vector<lo::Mask> layers(4, solid(8, 4, c.target));
        std::vector<double> z = z_for(4);
        const auto result = lo::layer_import_layers(layers, z, import_params(c.type, 1, {solid(8, 4, c.imported), solid(8, 4, c.imported)}));
        INFO("type " << int(c.type) << " " << int(c.target) << " vs " << int(c.imported));
        CHECK(layers[0] == solid(8, 4, c.target));
        CHECK(layers[3] == solid(8, 4, c.target));
        CHECK(layers[1] == solid(8, 4, c.expected));
        CHECK(layers[2] == solid(8, 4, c.expected));
        CHECK(layers.size() == 4);
        CHECK(result.list.source_index == std::vector<size_t>{0, 1, 2, 3});
        CHECK(result.report.empty());
    }
}

TEST_CASE("Layer import: extend, no extend, discard, insert", "[sla_layers][sla_import][f41]")
{
    const lo::Mask src = solid(8, 4, 77);
    {   // surplus source layers are dropped without extend
        std::vector<lo::Mask> layers(3, solid(8, 4)); std::vector<double> z = z_for(3);
        auto p = import_params(lo::LayerImportType::Replace, 2, {src, src, src});
        p.extend_beyond_layer_count = false;
        CHECK_FALSE(lo::layer_import_changes_list(p, 3));
        lo::layer_import_layers(layers, z, p);
        CHECK(layers.size() == 3);
        CHECK(layers[2] == src);
        CHECK(layers[1] == solid(8, 4));
    }
    {   // extend appends blank layers (cloned parameters from the last layer, Z continues)
        std::vector<lo::Mask> layers(3, solid(8, 4)); std::vector<double> z = z_for(3);
        auto p = import_params(lo::LayerImportType::Replace, 2, {src, src, src});
        CHECK(lo::layer_import_changes_list(p, 3));
        const auto r = lo::layer_import_layers(layers, z, p);
        REQUIRE(layers.size() == 5);
        CHECK(layers[4] == src);
        CHECK(r.list.source_index == std::vector<size_t>{0, 1, 2, 2, 2});
        CHECK(z[3] == Catch::Approx(0.20));
        CHECK(z[4] == Catch::Approx(0.25));
    }
    {   // discard unmodified layers truncates the tail
        std::vector<lo::Mask> layers(5, solid(8, 4)); std::vector<double> z = z_for(5);
        auto p = import_params(lo::LayerImportType::Replace, 0, {src, src});
        p.discard_unmodified_layers = true;
        CHECK(lo::layer_import_changes_list(p, 5));
        lo::layer_import_layers(layers, z, p);
        CHECK(layers.size() == 2);
        CHECK(z.size() == 2);
    }
    {   // insert two layers at 1: 5 layers, Z increasing, later layers keep their data and move up
        std::vector<lo::Mask> layers{solid(8, 4, 10), solid(8, 4, 20), solid(8, 4, 30)}; std::vector<double> z = z_for(3);
        const auto p = import_params(lo::LayerImportType::Insert, 1, {solid(8, 4, 1), solid(8, 4, 2)});
        CHECK(lo::layer_import_changes_list(p, 3));
        const auto r = lo::layer_import_layers(layers, z, p);
        REQUIRE(layers.size() == 5);
        CHECK(layers[0] == solid(8, 4, 10)); CHECK(layers[1] == solid(8, 4, 1)); CHECK(layers[2] == solid(8, 4, 2));
        CHECK(layers[3] == solid(8, 4, 20)); CHECK(layers[4] == solid(8, 4, 30));
        CHECK(r.list.source_index == std::vector<size_t>{0, 0, 0, 1, 2});
        for (size_t i = 1; i < z.size(); ++i) CHECK(z[i] > z[i - 1]);
        CHECK(z[3] == Catch::Approx(0.20));
    }
}

TEST_CASE("Layer import: stack beside the model, skipped files, validation", "[sla_layers][sla_import][f41]")
{
    std::vector<lo::Mask> layers(2, block(60, 20, 10, 5, 4, 4)); std::vector<double> z = z_for(2);
    lo::LayerImportParams p = import_params(lo::LayerImportType::Stack, 0, {block(60, 20, 20, 3, 6, 6), block(60, 20, 20, 3, 6, 6)});
    p.stack_margin = 5;
    lo::layer_import_layers(layers, z, p);
    // model (4 wide) + 5 margin + import (6 wide) = 15 x 6, centred again
    const lo::Rect all = lo::bounding_rect(layers[0]);
    CHECK(all.w == 15); CHECK(all.h == 6);
    CHECK(std::abs(all.x - (60 - 15) / 2) <= 1); // centred (the move rounds like UVtools' Anchor.MiddleCenter)
    CHECK(layers[0] == layers[1]);
    // too large for the layer: skipped, so nothing was imported -> error
    std::vector<lo::Mask> small(2, solid(10, 10)); std::vector<double> zs = z_for(2);
    auto big = import_params(lo::LayerImportType::Replace, 0, {block(30, 30, 0, 0, 30, 30), block(30, 30, 0, 0, 30, 30)});
    CHECK_THROWS_AS(lo::layer_import_layers(small, zs, big), std::invalid_argument);
    CHECK(small[0] == solid(10, 10)); // untouched
    // one of two files imported -> report
    std::vector<lo::Mask> three(2, solid(10, 10)); zs = z_for(2);
    lo::LayerImportParams two = import_params(lo::LayerImportType::MergeMax, 0, {solid(10, 10, 9), solid(10, 10, 9)});
    two.sources.push_back({"other", {solid(5, 5, 1)}});
    const auto r = lo::layer_import_layers(three, zs, two);
    CHECK(r.imported == 1); CHECK(r.total == 2);
    CHECK(r.report.find("1 out of 2") != std::string::npos);
    // validation
    lo::LayerImportParams none;
    CHECK(lo::validate_layer_import(none, 3, 8, 4) == "No files to import.\n");
    CHECK_FALSE(lo::validate_layer_import(import_params(lo::LayerImportType::Replace, 9, {solid(8, 4)}), 3, 8, 4).empty());
    CHECK(lo::validate_layer_import(import_params(lo::LayerImportType::Replace, 3, {solid(8, 4)}), 3, 8, 4).empty()); // extends
    auto no_extend = import_params(lo::LayerImportType::Replace, 3, {solid(8, 4)});
    no_extend.extend_beyond_layer_count = false;
    CHECK_FALSE(lo::validate_layer_import(no_extend, 3, 8, 4).empty());
    // cancel: untouched
    std::vector<lo::Mask> keep(2, solid(8, 4, 5)); z = z_for(2);
    CHECK_THROWS_AS(lo::layer_import_layers(keep, z, import_params(lo::LayerImportType::Replace, 0, {solid(8, 4, 1)}), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK(keep[0] == solid(8, 4, 5));
}

TEST_CASE("Layer import: recipe with PNG pictures, one undo step, errors", "[sla_layers][sla_import][f41]")
{
    TempDir dir;
    write_png(dir.str("a.png"), solid(16, 8, 100));
    write_png(dir.str("b.png"), solid(16, 8, 50));
    const auto masks = three_layers();
    // tier 1: pixels only (merge_sum, no list change): the pictures are layers 1 and 2
    auto result = run_on(R"({"op": "layer_import", "files": ["a.png", "b.png"], "import_type": "merge_sum", "start_layer": 1, "extend_beyond_layer_count": false})", make_doc(masks), dir.path.string());
    REQUIRE(result.ok);
    auto &doc = *result.document;
    CHECK(doc.layer_count() == 3);
    CHECK(layer_of(doc, 0) == masks[0]);
    const lo::Mask expect1 = [&] { lo::Mask m = masks[1]; for (auto &v : m.px) v = uint8_t(std::min(255, int(v) + 100)); return m; }();
    CHECK(layer_of(doc, 1) == expect1);
    REQUIRE(doc.undo());
    CHECK(layer_of(doc, 1) == masks[1]);
    CHECK_FALSE(doc.can_undo()); // ONE undo step
    // tier 2: insert on a generated document
    result = run_on(R"({"op": "layer_import", "files": ["a.png", "b.png"], "import_type": "insert", "start_layer": 1})", make_doc(masks), dir.path.string());
    REQUIRE(result.ok);
    CHECK(result.document->layer_count() == 5);
    CHECK(layer_of(*result.document, 1) == solid(16, 8, 100));
    CHECK(layer_of(*result.document, 2) == solid(16, 8, 50));
    CHECK(layer_of(*result.document, 3) == masks[1]);
    CHECK(param(*result.document, 3, F::Exposure) == 11); // the layer kept its own parameters
    REQUIRE(result.document->undo());
    CHECK(result.document->layer_count() == 3);
    CHECK_FALSE(result.document->can_undo());
    // errors
    CHECK(error_location(R"({"op": "layer_import", "files": ["a.png"], "import_type": "blend"})") == "steps[0].import_type");
    CHECK(error_location(R"({"op": "layer_import"})") == "steps[0].files");
    CHECK(error_location(R"({"op": "layer_import", "files": []})") == "steps[0].files");
    CHECK(error_location(R"({"op": "layer_import", "files": ["a.png", 3]})") == "steps[0].files[1]");
    result = run_on(R"({"op": "layer_import", "files": ["missing.png"]})", make_doc(masks), dir.path.string());
    CHECK_FALSE(result.ok);
    result = run_on(R"({"op": "layer_import", "files": ["a.bmp"]})", make_doc(masks), dir.path.string());
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("PNG") != std::string::npos);
}

// ======================================================================================================
// 8. Redraw model
// ======================================================================================================

#include "libslic3r/SLA/LayerRedrawModel.hpp"

namespace {
// body = 4x4 square at (2,2); full = square + two support pixels at (0,0) and (7,7)
lo::Mask redraw_body() { return block(8, 8, 2, 2, 4, 4); }
lo::Mask redraw_full()
{
    lo::Mask m = redraw_body();
    m.at(0, 0) = 255; m.at(7, 7) = 255;
    return m;
}
} // namespace

TEST_CASE("Redraw model: supports and model with every operator (UVtools quirks kept)", "[sla_layers][sla_redraw][f41]")
{
    lo::RedrawModelParams p;
    p.contact_points_only = false;
    p.brightness = 220;
    struct Case { lo::RedrawOperator op; uint8_t dot, square, empty; };
    // pattern = 220 on the support pixels and 0 elsewhere
    const Case cases[] = {{lo::RedrawOperator::Set, 220, 0, 0}, {lo::RedrawOperator::Add, 255, 255, 0}, {lo::RedrawOperator::Subtract, 35, 255, 0},
                          {lo::RedrawOperator::Multiply, 220, 0, 0}, {lo::RedrawOperator::Divide, 1, 0, 0}, {lo::RedrawOperator::Minimum, 220, 0, 0},
                          {lo::RedrawOperator::Maximum, 255, 255, 0}, {lo::RedrawOperator::AbsDiff, 35, 255, 0}};
    for (const Case &c : cases) {
        p.op = c.op;
        lo::Mask full = redraw_full();
        REQUIRE(lo::redraw_model_layer(full, redraw_body(), nullptr, p));
        INFO("operator " << int(c.op));
        CHECK(full.at(0, 0) == c.dot);
        CHECK(full.at(7, 7) == c.dot);
        CHECK(full.at(3, 3) == c.square);
        CHECK(full.at(1, 6) == c.empty);
    }
    // Model: the pattern covers the square, Subtract(40) -> 255 - 40
    p.type = lo::RedrawType::Model; p.op = lo::RedrawOperator::Subtract; p.brightness = 40;
    lo::Mask full = redraw_full();
    lo::redraw_model_layer(full, redraw_body(), nullptr, p);
    CHECK(full.at(3, 3) == 215);
    CHECK(full.at(0, 0) == 255);
    // ROI: pixels outside are untouched
    p.type = lo::RedrawType::Supports; p.op = lo::RedrawOperator::Subtract; p.roi = {0, 0, 4, 4};
    full = redraw_full();
    lo::redraw_model_layer(full, redraw_body(), nullptr, p);
    CHECK(full.at(0, 0) == 215);
    CHECK(full.at(7, 7) == 255);
}

TEST_CASE("Redraw model: contact points only", "[sla_layers][sla_redraw][f41]")
{
    lo::RedrawModelParams p;
    p.brightness = 220;
    lo::Mask next = redraw_body();
    next.at(0, 0) = 255;                       // only the first support touches the next body layer
    lo::Mask full = redraw_full();
    REQUIRE(lo::redraw_model_layer(full, redraw_body(), &next, p));
    CHECK(full.at(0, 0) == 220);
    CHECK(full.at(7, 7) == 255);               // no contact: untouched
    CHECK(full.at(3, 3) == 255);               // the model is untouched
    // the last body layer has no next layer: nothing changes
    full = redraw_full();
    CHECK_FALSE(lo::redraw_model_layer(full, redraw_body(), nullptr, p));
    CHECK(full == redraw_full());
    // strict mode needs supports > 100 and a next layer > 150, then the whole contour is filled
    p.ignore_contactless_pixels = false;
    lo::Mask big_full = redraw_body();
    for (int y = 0; y < 2; ++y) for (int x = 6; x < 8; ++x) big_full.at(size_t(x), size_t(y)) = 255; // a 2x2 support
    lo::Mask big_next = redraw_body();
    big_next.at(6, 0) = 200;
    REQUIRE(lo::redraw_model_layer(big_full, redraw_body(), &big_next, p));
    { std::string v; for (int y = 0; y < 2; ++y) for (int x = 6; x < 8; ++x) v += std::to_string(int(big_full.at(size_t(x), size_t(y)))) + " "; INFO(v); CHECK(big_full.at(6, 0) == 220); }
    big_next.at(6, 0) = 100;                   // below 150: not a contact
    big_full = redraw_body();
    big_full.at(6, 0) = 255;
    CHECK_FALSE(lo::redraw_model_layer(big_full, redraw_body(), &big_next, p));
}

TEST_CASE("Redraw model: alignment with a raft, empty layers, validation and the kernel contract", "[sla_layers][sla_redraw][f41]")
{
    lo::RedrawModelParams p;
    p.contact_points_only = false;
    p.op = lo::RedrawOperator::Add;
    p.brightness = 10;
    // two raft layers in front: body layer j aligns with full layer j + 2
    std::vector<lo::Mask> full{block(8, 8, 0, 0, 8, 8), block(8, 8, 0, 0, 8, 8), redraw_full(), solid(8, 8), redraw_full()};
    const std::vector<lo::Mask> body{redraw_body(), redraw_body(), redraw_body()};
    auto before = full;
    lo::redraw_model_layers(full, body, p);
    CHECK(full[0] == before[0]); CHECK(full[1] == before[1]); // raft untouched
    CHECK(full[2].at(0, 0) == 255);                            // 255 + 10 saturates
    CHECK(full[3] == before[3]);                               // an empty layer is skipped
    CHECK((full[4] != before[4] || full[4].at(0, 0) == 255));
    // body longer than the document, resolution mismatch
    std::vector<lo::Mask> longer(6, redraw_body());
    CHECK_THROWS_AS(lo::redraw_model_layers(full, longer, p), std::invalid_argument);
    CHECK_THROWS_AS(lo::redraw_model_layers(full, {solid(4, 4)}, p), std::invalid_argument);
    CHECK(lo::validate_redraw_model_files(p, 5, 8, 8, 3, 8, 8).empty());
    CHECK_FALSE(lo::validate_redraw_model_files(p, 5, 8, 8, 3, 9, 8).empty());
    // validation texts
    lo::RedrawModelParams v; v.op = lo::RedrawOperator::Add; v.brightness = 0;
    CHECK(lo::validate_redraw_model(v).find("yield no result") != std::string::npos);
    v.op = lo::RedrawOperator::Divide;
    CHECK(lo::validate_redraw_model(v).find("not valid") != std::string::npos);
    v.contact_points_only = false; v.type = lo::RedrawType::Supports; v.brightness = 5; v.op = lo::RedrawOperator::Add;
    CHECK(lo::validate_redraw_model(v).empty());
    // streaming == batch, one undo step, cancel
    p.op = lo::RedrawOperator::Subtract;
    p.brightness = 100;
    std::vector<lo::Mask> doc_layers{block(8, 8, 0, 0, 2, 2), redraw_full(), redraw_full()};
    std::vector<lo::Mask> body2{redraw_body(), redraw_body()};
    auto expected = doc_layers;
    lo::redraw_model_layers(expected, body2, p);
    check_document_contract(doc_layers, [&](const lo::StreamLayout &l) {
        return lo::redraw_model_kernel(p, l, body2.size(), [&](size_t j) { return std::make_shared<const lo::Mask>(body2[j]); });
    }, 1, 2);
    auto doc = make_doc(doc_layers);
    REQUIRE(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, {}, [&](const lo::StreamLayout &l) {
        return lo::redraw_model_kernel(p, l, body2.size(), [&](size_t j) { return std::make_shared<const lo::Mask>(body2[j]); });
    }, {}, {}));
    for (size_t i = 0; i < 3; ++i) CHECK(layer_of(doc, i) == expected[i]);
}

namespace {
// A minimal SL1 archive (config.ini, prusaslicer.ini, one PNG per layer) that LayerDocument::load_archive reads.
void write_sl1(const std::string &path, const std::vector<lo::Mask> &layers)
{
    Zipper zip(path);
    zip.add_entry("config.ini"); zip << "layerHeight = 0.05\nnumFade = 0\n";
    zip.add_entry("prusaslicer.ini"); zip << "display_width = 16\ndisplay_height = 8\ndisplay_pixels_x = " + std::to_string(layers.front().width) +
                                             "\ndisplay_pixels_y = " + std::to_string(layers.front().height) + "\n";
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto png = sla::PNGRasterEncoder{}(layers[i].px.data(), layers[i].width, layers[i].height, 1);
        char name[32];
        std::snprintf(name, sizeof name, "job%05zu.png", i);
        zip.add_entry(name, static_cast<const char *>(png.data()), png.size());
    }
    zip.finalize();
}
} // namespace

TEST_CASE("Redraw model: recipe with a second model-only file", "[sla_layers][sla_redraw][f41]")
{
    TempDir dir;
    const std::vector<lo::Mask> body{redraw_body(), redraw_body()};
    write_sl1(dir.str("body.sl1"), body);
    std::vector<lo::Mask> full{block(8, 8, 0, 0, 8, 8), redraw_full(), redraw_full()};
    const std::string step = R"({"op": "redraw_model", "file": "body.sl1", "redraw_type": "supports", "operator": "subtract", "brightness": 100, "contact_points_only": false})";
    auto result = run_on(step, make_doc(full), dir.path.string());
    REQUIRE(result.ok);
    auto expected = full;
    lo::RedrawModelParams p;
    p.contact_points_only = false; p.op = lo::RedrawOperator::Subtract; p.brightness = 100;
    lo::redraw_model_layers(expected, body, p);
    for (size_t i = 0; i < 3; ++i) CHECK(layer_of(*result.document, i) == expected[i]);
    CHECK(layer_of(*result.document, 0) == full[0]);
    // a body with another resolution, a missing file, bad parameters
    write_sl1(dir.str("odd.sl1"), std::vector<lo::Mask>(2, solid(10, 8)));
    CHECK_FALSE(run_on(R"({"op": "redraw_model", "file": "odd.sl1"})", make_doc(full), dir.path.string()).ok);
    CHECK_FALSE(run_on(R"({"op": "redraw_model", "file": "missing.sl1"})", make_doc(full), dir.path.string()).ok);
    CHECK(error_location(R"({"op": "redraw_model"})") == "steps[0].file");
    CHECK(error_location(R"({"op": "redraw_model", "file": "x", "operator": "xor"})") == "steps[0].operator");
    CHECK(error_location(R"({"op": "redraw_model", "file": "x", "operator": "add", "contact_points_only": true, "brightness": 0})") == "steps[0].brightness");
}

TEST_CASE("Pixel editor recipe steps: draw_text, draw_support, drill_drain_hole", "[sla_layers][sla_hershey][f41]")
{
    std::vector<lo::Mask> masks(6, solid(80, 40));
    auto result = run_on(R"({"op": "draw_text", "text": "Hi", "x": 4, "y": 25, "font_scale": 1, "thickness": 2, "line_type": "eight_connected", "first_layer": 1, "last_layer": 2})", make_doc(masks));
    REQUIRE(result.ok);
    CHECK(layer_of(*result.document, 0).nonzero_count() == 0);
    CHECK(layer_of(*result.document, 1).nonzero_count() > 10);
    CHECK(layer_of(*result.document, 1) == layer_of(*result.document, 2));
    CHECK(layer_of(*result.document, 3).nonzero_count() == 0);
    result = run_on(R"({"op": "draw_support", "layer": 5, "x": 40, "y": 20, "tip_diameter": 5, "pillar_diameter": 9, "base_diameter": 15, "anti_alias": false})", make_doc(masks));
    REQUIRE(result.ok);
    CHECK(layer_of(*result.document, 5).nonzero_count() == 0);
    CHECK(layer_of(*result.document, 4).at(40, 20) == 255);
    CHECK(layer_of(*result.document, 0).at(40, 20) == 255);
    std::vector<lo::Mask> slab(4, solid(80, 40, 255));
    slab[0] = solid(80, 40);
    result = run_on(R"({"op": "drill_drain_hole", "layer": 3, "x": 40, "y": 20, "diameter": 9, "anti_alias": false})", make_doc(slab));
    REQUIRE(result.ok);
    CHECK(layer_of(*result.document, 2).at(40, 20) == 0);
    CHECK(layer_of(*result.document, 3).at(40, 20) == 255);
    CHECK(error_location(R"({"op": "draw_text", "x": 1, "y": 1})") == "steps[0].text");
    CHECK(error_location(R"({"op": "draw_text", "text": "a", "y": 1})") == "steps[0].x");
    CHECK(error_location(R"({"op": "draw_text", "text": "a", "x": 1, "y": 1, "font_scale": 0.1})") == "steps[0].font_scale");
    CHECK(error_location(R"({"op": "draw_text", "text": "  ", "x": 1, "y": 1})") == "steps[0].text");
    CHECK(error_location(R"({"op": "draw_support", "x": 1, "y": 1})") == "steps[0].layer");
    CHECK(error_location(R"({"op": "drill_drain_hole", "layer": 2, "x": 1, "y": 1, "diameter": 0})") == "steps[0].diameter");
}
