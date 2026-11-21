// SPDX-License-Identifier: AGPL-3.0-or-later
// Streaming whole-document operations (goal G10): every layerops kernel run through LayerDocument::apply_mask_kernel
// (decode a few layers, apply, encode, one undo step) must give the byte-identical result of the batch function on a
// normal document, under a default and under a tiny decoded-pixel budget (which forces several chunks and a sliding
// neighbour window). Also: the window engine, cancellation, the streaming exports and the budget refusal.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <atomic>
#include <cmath>
#include <functional>
#include <random>

#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerExports.hpp"
#include "libslic3r/SLA/LayerListStream.hpp"
#include "libslic3r/SLA/LayerMemoryBudget.hpp"
#include "libslic3r/SLA/LayerOperations.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
namespace lo = Slic3r::sla::layerops;
namespace le = Slic3r::sla::layerexport;

namespace {
constexpr size_t kW = 64, kH = 48, kLayers = 14;
constexpr double kPitch = 0.05;

// Deterministic multi-layer part: a plate (layers 0..2), then six pillars with holes (layers 3..) that change slowly,
// a few grey levels and some noise, so island / hole / wall / bridge logic all have something to bite on.
std::vector<uint8_t> make_layer(size_t layer)
{
    std::vector<uint8_t> px(kW * kH, 0);
    std::mt19937 rng(uint32_t(layer * 7919 + 13));
    auto fill = [&](int x0, int y0, int x1, int y1, uint8_t v) {
        for (int y = std::max(0, y0); y < std::min<int>(kH, y1); ++y)
            for (int x = std::max(0, x0); x < std::min<int>(kW, x1); ++x) px[size_t(y) * kW + size_t(x)] = v;
    };
    if (layer < 3) {
        fill(4, 4, 60, 44, 255);
    } else {
        for (int k = 0; k < 6; ++k) fill(6 + 9 * k, 8 + int(layer % 4), 12 + 9 * k, 30 + int(layer % 3) * 2, uint8_t(k % 2 ? 200 : 255));
        fill(10, 34, 40, 40 - int(layer % 5), 255); // a wide bar
        fill(8, 10, 10, 12, 0); // holes
        fill(24 + int(layer % 3), 12, 26 + int(layer % 3), 14, 0);
        if (layer > 9) fill(30, 20, 36, 26, 90); // grey patch near the top
    }
    for (int n = 0; n < 12; ++n) { const size_t y = 6 + size_t(rng()) % 36, x = 8 + size_t(rng()) % 48; px[y * kW + x] = uint8_t(rng() % 256); }
    return px;
}

sla::LayerDocument make_doc()
{
    std::vector<sla::EncodedRaster> layers;
    for (size_t i = 0; i < kLayers; ++i) {
        const auto px = make_layer(i);
        layers.push_back(sla::PNGRasterEncoder{}(px.data(), kW, kH, 1));
    }
    return sla::LayerDocument::from_rasters(std::move(layers));
}

std::vector<double> make_z()
{
    std::vector<double> z(kLayers);
    for (size_t i = 0; i < kLayers; ++i) z[i] = kPitch * double(i + 1);
    return z;
}

std::vector<std::vector<uint8_t>> bytes_of(const sla::LayerDocument &doc)
{
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        const auto view = doc.encoded_layer(i);
        { const auto *b = static_cast<const uint8_t *>(view.buf); out.emplace_back(b, b + view.sz); }
    }
    return out;
}

struct OpCase {
    std::string name;
    std::function<void(std::vector<lo::Mask> &, const std::vector<double> &, const sla::LayerProgress &)> batch;
    sla::LayerDocument::MaskKernelFactory kernel;
    bool changes = true; // the operation must modify at least one layer on this part
};

// Batch of a typed operation from its params, and its kernel: the pair every row below is made of.
template<class P, class B, class K> OpCase make_case(const std::string &name, const P &p, B batch, K kernel, bool changes = true)
{
    OpCase c;
    c.name = name;
    c.batch = [p, batch](std::vector<lo::Mask> &layers, const std::vector<double> &z, const sla::LayerProgress &pr) { batch(layers, z, p, pr); };
    c.kernel = [p, kernel](const lo::StreamLayout &l) { return kernel(p, l); };
    c.changes = changes;
    return c;
}

lo::LayerSelection sel(size_t first, size_t last, lo::Rect roi = {}) { return {first, last, roi}; }

std::vector<OpCase> all_cases()
{
    std::vector<OpCase> cases;
    const auto plain = [](auto fn) { return [fn](std::vector<lo::Mask> &l, const std::vector<double> &, const auto &p, const sla::LayerProgress &pr) { fn(l, p, pr); }; };
    {
        lo::MoveParams p; p.selection = sel(0, lo::kLastLayer); p.anchor = lo::Anchor::TopLeft; p.margin_left = 3; p.margin_top = 2;
        cases.push_back(make_case("move model bounds", p, plain(lo::move_layers), lo::move_kernel));
        p.selection = sel(2, 9, {0, 0, 40, 30}); p.anchor = lo::Anchor::BottomRight; p.is_cut_move = false; p.margin_left = p.margin_top = 0;
        cases.push_back(make_case("move roi copy", p, plain(lo::move_layers), lo::move_kernel));
    }
    {
        lo::ResizeParams p; p.selection = sel(0, lo::kLastLayer); p.x_percent = 80; p.y_percent = 120; p.is_fade = true;
        cases.push_back(make_case("resize fade", p, plain(lo::resize_layers), lo::resize_kernel));
        p.selection = sel(3, 10, {4, 4, 50, 36}); p.constrain_xy = true; p.is_fade = false; p.x_percent = 130;
        cases.push_back(make_case("resize roi range", p, plain(lo::resize_layers), lo::resize_kernel));
    }
    {
        lo::RotateParams p; p.selection = sel(1, 11); p.angle_degrees = 23;
        cases.push_back(make_case("rotate", p, plain(lo::rotate_layers), lo::rotate_kernel));
    }
    {
        lo::FlipParams p; p.selection = sel(0, lo::kLastLayer); p.direction = lo::FlipDirection::Both; p.make_copy = true;
        cases.push_back(make_case("flip copy", p, plain(lo::flip_layers), lo::flip_kernel));
    }
    {
        lo::PatternParams p; p.selection = sel(0, lo::kLastLayer); p.cols = 2; p.rows = 1; p.col_spacing = 2; p.anchor = lo::Anchor::None;
        p.selection.roi = {2, 2, 28, 40};
        cases.push_back(make_case("pattern", p, plain(lo::pattern_layers), lo::pattern_kernel));
        p.selection = sel(2, 12); p.anchor = lo::Anchor::MiddleCenter; p.selection.roi = {2, 2, 28, 40};
        cases.push_back(make_case("pattern anchored", p, plain(lo::pattern_layers), lo::pattern_kernel));
    }
    {
        lo::SolidifyParams p; p.selection = sel(0, lo::kLastLayer); p.minimum_area = 2;
        cases.push_back(make_case("solidify", p, plain(lo::solidify_layers), lo::solidify_kernel));
    }
    {
        lo::ThresholdParams p; p.selection = sel(0, lo::kLastLayer, {1, 1, 60, 46}); p.type = lo::ThresholdType::Otsu;
        cases.push_back(make_case("threshold otsu roi", p, plain(lo::threshold_layers), lo::threshold_kernel));
    }
    {
        lo::PixelDimmingParams p; p.selection = sel(2, 12); p.wall_thickness_start = 1; p.wall_thickness_end = 4; p.chamfer = true;
        p.pattern_text = "127 255\n255 127"; p.alternate_pattern_text = "255 127\n127 255"; p.alternate_pattern_per_layers = 2;
        cases.push_back(make_case("pixel dimming chamfer", p, plain(lo::pixel_dimming_layers), lo::pixel_dimming_kernel));
        p.walls_only = true; p.lightening_pixels = true; p.selection = sel(0, lo::kLastLayer, {2, 2, 58, 44});
        cases.push_back(make_case("pixel dimming walls roi", p, plain(lo::pixel_dimming_layers), lo::pixel_dimming_kernel));
    }
    {
        lo::PixelArithmeticParams p; p.selection = sel(1, 12); p.op = lo::ArithmeticOperator::Subtract; p.apply = lo::ArithmeticApply::ModelSurfaceAndInset;
        p.value = 80; p.value_step = 3; p.wall_thickness_start = 1; p.wall_thickness_end = 3; p.wall_chamfer = true;
        cases.push_back(make_case("arithmetic surface+inset", p, plain(lo::pixel_arithmetic_layers), lo::pixel_arithmetic_kernel));
        p.apply = lo::ArithmeticApply::ModelSurface; p.selection = sel(0, lo::kLastLayer, {3, 3, 55, 40}); p.op = lo::ArithmeticOperator::Add; p.value = 40; p.value_step = 0;
        cases.push_back(make_case("arithmetic surface roi", p, plain(lo::pixel_arithmetic_layers), lo::pixel_arithmetic_kernel));
        p.apply = lo::ArithmeticApply::ModelWalls; p.selection = sel(0, lo::kLastLayer); p.op = lo::ArithmeticOperator::Set; p.value = 120;
        p.wall_thickness_start = 2; p.wall_thickness_end = 2; p.wall_chamfer = false;
        cases.push_back(make_case("arithmetic walls", p, plain(lo::pixel_arithmetic_layers), lo::pixel_arithmetic_kernel));
        p.op = lo::ArithmeticOperator::KeepRegion; p.apply = lo::ArithmeticApply::All; p.selection = sel(0, lo::kLastLayer, {8, 8, 30, 30});
        cases.push_back(make_case("arithmetic keep region", p, plain(lo::pixel_arithmetic_layers), lo::pixel_arithmetic_kernel));
        p.op = lo::ArithmeticOperator::Multiply; p.apply = lo::ArithmeticApply::ModelInner; p.use_pattern = true; p.pattern_text = "255 128\n64 255";
        p.selection = sel(0, lo::kLastLayer); p.wall_thickness_start = p.wall_thickness_end = 1;
        cases.push_back(make_case("arithmetic pattern inner", p, plain(lo::pixel_arithmetic_layers), lo::pixel_arithmetic_kernel));
    }
    {
        lo::LightBleedParams p; p.selection = sel(1, 12); p.subject = lo::LightBleedSubject::Both; p.lookup = lo::LightBleedLookup::Both; p.dim_by = "25,15,10";
        cases.push_back(make_case("light bleed both", p, plain(lo::light_bleed_compensation_layers), lo::light_bleed_kernel));
        p.subject = lo::LightBleedSubject::Similarities; p.lookup = lo::LightBleedLookup::Previous; p.selection = sel(0, lo::kLastLayer, {2, 2, 56, 42});
        cases.push_back(make_case("light bleed previous roi", p, plain(lo::light_bleed_compensation_layers), lo::light_bleed_kernel));
    }
    {
        lo::InfillParams p; p.selection = sel(0, lo::kLastLayer); p.floor_ceil_thickness_mm = 0.12; p.wall_thickness = 1; p.infill_thickness = 1; p.infill_spacing = 5;
        const auto batch = [](std::vector<lo::Mask> &l, const std::vector<double> &z, const lo::InfillParams &q, const sla::LayerProgress &pr) { lo::infill_layers(l, z, q, pr); };
        for (auto type : {lo::InfillAlgorithm::Honeycomb, lo::InfillAlgorithm::CubicCrossAlternating, lo::InfillAlgorithm::Gyroid, lo::InfillAlgorithm::Waves,
                          lo::InfillAlgorithm::Concentric, lo::InfillAlgorithm::Pillars, lo::InfillAlgorithm::CubicStar}) {
            p.type = type;
            cases.push_back(make_case("infill " + std::to_string(int(type)), p, batch, lo::infill_kernel, false));
        }
        p.type = lo::InfillAlgorithm::Honeycomb; p.selection = sel(2, 11, {2, 2, 58, 44}); p.floor_ceil_thickness_mm = 0.2;
        cases.push_back(make_case("infill roi range", p, batch, lo::infill_kernel, false));
    }
    {
        lo::RaftReliefParams p; p.type = lo::RaftReliefType::Relief; p.dilate_iterations = 1; p.wall_margin = 2; p.hole_diameter = 6; p.hole_spacing = 4;
        const auto batch = [](std::vector<lo::Mask> &l, const std::vector<double> &, const lo::RaftReliefParams &q, const sla::LayerProgress &pr) {
            REQUIRE(lo::raft_relief_layers(l, q, pr));
        };
        const auto kernel = [](const lo::RaftReliefParams &q, const lo::StreamLayout &l) {
            auto k = lo::raft_relief_kernel(q, l);
            REQUIRE(k);
            return *k;
        };
        cases.push_back(make_case("raft relief holes", p, batch, kernel));
        p.type = lo::RaftReliefType::Dimming; p.low_brightness = 100; p.roi = {2, 2, 58, 42}; p.ignore_first_layers = 1;
        cases.push_back(make_case("raft relief dimming roi", p, batch, kernel));
    }
    {
        lo::LayerArithmeticParams p; p.sentence = "5,6 = 5+7-3; 8:10 = 1|2";
        const auto batch = [](std::vector<lo::Mask> &l, const std::vector<double> &, const lo::LayerArithmeticParams &q, const sla::LayerProgress &pr) { lo::layer_arithmetic(l, q, pr); };
        cases.push_back(make_case("layer arithmetic", p, batch, lo::layer_arithmetic_kernel));
        p.sentence = "4 = 5*6; 9 = 4"; p.roi = {4, 4, 30, 30}; // layer 4 is read by a later group but written by an earlier one
        OpCase dependent = make_case("layer arithmetic roi", p, batch, lo::layer_arithmetic_kernel);
        dependent.name = "layer arithmetic dependent groups (batch fallback)";
        cases.push_back(dependent);
    }
    return cases;
}

size_t stream_runs = 0;
} // namespace

TEST_CASE("Every streamed layer operation is byte-identical to its batch result", "[sla_layers][sla_layers_stream]")
{
    const auto z = make_z();
    const auto cases = all_cases();
    const auto original = bytes_of(make_doc());
    // Default budget, and a tiny one (24 layers of this size) that forces several chunks per operation.
    const std::vector<std::optional<uint64_t>> budgets{std::nullopt, kW * kH * 24};
    for (const auto &c : cases) {
        INFO("operation: " << c.name);
        auto batch_doc = make_doc();
        REQUIRE(batch_doc.apply_mask_batch(sla::LayerMaskPlane::Main, [&](std::vector<lo::Mask> &layers, const sla::LayerProgress &pr) { c.batch(layers, z, pr); }));
        const auto expected = bytes_of(batch_doc);
        if (c.changes) REQUIRE(expected != original);
        for (const auto &budget : budgets) {
            INFO("budget: " << (budget ? *budget : 0));
            auto doc = make_doc();
            sla::LayerBudgetOverride limit;
            limit.decoded_bytes = budget;
            sla::ScopedLayerBudgets scoped(limit);
            // A dependent layer arithmetic sentence is not streamable: the call falls back to the batch function.
            sla::LayerDocument::MaskBatchOperation fallback = [&](std::vector<lo::Mask> &layers, const sla::LayerProgress &pr) { c.batch(layers, z, pr); };
            REQUIRE(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, z, c.kernel, {}, fallback));
            REQUIRE(bytes_of(doc) == expected);
            // One undo step restores the original bytes, redo the result.
            REQUIRE(doc.undo());
            REQUIRE_FALSE(doc.can_undo());
            REQUIRE(bytes_of(doc) == original);
            REQUIRE(doc.redo());
            REQUIRE(bytes_of(doc) == expected);
            ++stream_runs;
        }
    }
    CHECK(stream_runs >= cases.size() * 2);
}

TEST_CASE("A dependent layer arithmetic sentence reports NotStreamable and is refused without a fallback", "[sla_layers][sla_layers_stream]")
{
    auto doc = make_doc();
    lo::LayerArithmeticParams p; p.sentence = "4 = 5*6; 9 = 4";
    const auto kernel = [p](const lo::StreamLayout &l) { return lo::layer_arithmetic_kernel(p, l); };
    CHECK_THROWS_AS(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, {}, kernel), lo::NotStreamable);
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("The window engine hands out the original neighbours and one undo step", "[sla_layers][sla_layers_stream]")
{
    // Result of layer i = max(layer i-1, layer i, layer i+1) of the ORIGINALS, restricted to layers 2..11.
    auto reference = make_doc();
    std::vector<lo::Mask> originals;
    for (size_t i = 0; i < kLayers; ++i) { auto im = reference.image(i); lo::Mask m(im.cols, im.rows); m.px = im.buf; originals.push_back(std::move(m)); }
    std::vector<lo::Mask> expected = originals;
    for (size_t i = 2; i <= 11; ++i)
        for (size_t k = 0; k < expected[i].px.size(); ++k)
            expected[i].px[k] = std::max({originals[i - 1].px[k], originals[i].px[k], originals[i + 1].px[k]});
    for (const std::optional<uint64_t> budget : {std::optional<uint64_t>{}, std::optional<uint64_t>{kW * kH * 20}}) {
        auto doc = make_doc();
        sla::LayerBudgetOverride limit; limit.decoded_bytes = budget;
        sla::ScopedLayerBudgets scoped(limit);
        sla::LayerDocument::MaskWindowSpec spec; spec.first = 2; spec.last = 11; spec.before = 1; spec.after = 1;
        std::atomic<size_t> calls{0};
        std::atomic<bool> wrong_count{false};
        REQUIRE(doc.apply_mask_window(sla::LayerMaskPlane::Main, spec, [&](lo::Mask &mask, size_t layer, const sla::LayerDocument::MaskStreamContext &ctx) {
            ++calls;
            if (ctx.layer_count != kLayers) wrong_count = true;
            const auto prev = ctx.neighbour(layer - 1), next = ctx.neighbour(layer + 1);
            for (size_t k = 0; k < mask.px.size(); ++k) mask.px[k] = std::max({prev->px[k], mask.px[k], next->px[k]});
        }));
        CHECK(calls == 10);
        CHECK_FALSE(wrong_count);
        for (size_t i = 0; i < kLayers; ++i) {
            const auto im = doc.image(i);
            CHECK(im.buf == expected[i].px);
        }
        REQUIRE(doc.undo());
        REQUIRE_FALSE(doc.can_undo());
    }
}

TEST_CASE("A cancelled streamed operation leaves the document and its history alone", "[sla_layers][sla_layers_stream]")
{
    auto doc = make_doc();
    const auto original = bytes_of(doc);
    lo::ThresholdParams p; p.selection = sel(0, lo::kLastLayer); p.threshold = 100;
    const auto kernel = [p](const lo::StreamLayout &l) { return lo::threshold_kernel(p, l); };
    size_t polls = 0;
    CHECK_FALSE(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, {}, kernel, [&](size_t, size_t) { return ++polls < 5; }));
    CHECK(bytes_of(doc) == original);
    CHECK_FALSE(doc.can_undo());
    CHECK(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, {}, kernel)); // still usable
    CHECK(doc.can_undo());
}

TEST_CASE("A neighbour window that cannot fit the decoded budget is refused with a clear message", "[sla_layers][sla_layers_stream]")
{
    auto doc = make_doc();
    const auto original = bytes_of(doc);
    lo::LightBleedParams p; p.selection = sel(0, lo::kLastLayer); p.lookup = lo::LightBleedLookup::Both; p.dim_by = "5,5,5,5,5,5,5,5";
    sla::LayerBudgetOverride limit; limit.decoded_bytes = kW * kH * 10; // the window needs 8 + 8 + 5 layers
    sla::ScopedLayerBudgets scoped(limit);
    const auto kernel = [p](const lo::StreamLayout &l) { return lo::light_bleed_kernel(p, l); };
    REQUIRE_THROWS_WITH(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, {}, kernel), ContainsSubstring("decoded-pixel budget"));
    CHECK(bytes_of(doc) == original);
}

TEST_CASE("Streaming exports equal the batch exports", "[sla_layers][sla_layers_stream]")
{
    auto doc = make_doc();
    std::vector<lo::Mask> layers;
    for (size_t i = 0; i < kLayers; ++i) { auto im = doc.image(i); lo::Mask m(im.cols, im.rows); m.px = im.buf; layers.push_back(std::move(m)); }
    const auto z = make_z();
    // The layout a GUI export gets: one decode per fetch, a parallel scan.
    lo::StreamLayout layout;
    layout.layer_count = kLayers; layout.width = kW; layout.height = kH; layout.z_mm = z;
    layout.fetch = [&](size_t i) { auto im = doc.image(i); auto m = std::make_shared<lo::Mask>(im.cols, im.rows); m->px = im.buf; return std::shared_ptr<const lo::Mask>(m); };
    layout.scan = [&](const std::function<void(size_t, const lo::Mask &)> &visit) {
        REQUIRE(doc.scan_masks(sla::LayerMaskPlane::Main, [&](size_t i, const lo::Mask &m) { visit(i, m); }));
    };
    CHECK(lo::scan_model_bounds(layout) == lo::bounding_rect(layers));

    le::ExportImageParams ip; ip.selection = sel(1, 12);
    const auto a = le::render_layer_images(layers, ip), b = le::render_layer_images(layout, ip);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) { CHECK(a[i].name == b[i].name); CHECK(a[i].data == b[i].data); }

    le::ExportGifParams gp; gp.selection = sel(0, lo::kLastLayer); gp.clip_by_volume_bounds = true; gp.skip = 1; gp.scale_percent = 50;
    CHECK(le::render_layer_gif(layers, gp) == le::render_layer_gif(layout, gp));

    le::ExportHeatMapParams hp; hp.selection = sel(0, lo::kLastLayer, {2, 2, 50, 40}); hp.crop_by_roi = false;
    CHECK(le::render_heat_map(layers, z, hp) == le::render_heat_map(layout, z, hp));

    le::ExportSkeletonParams sp; sp.selection = sel(2, 9);
    CHECK(le::render_skeleton(layers, sp) == le::render_skeleton(layout, sp));

    le::ExportHtmlParams htp; htp.preview_step = 3; htp.export_layer_settings = false;
    CHECK(le::render_html(layers, z, {}, htp) == le::render_html(layout, z, {}, htp));

    le::ExportMeshParams mp; mp.selection = sel(1, 11); mp.quality = 2;
    const auto m1 = le::build_voxel_mesh(layers, z, mp), m2 = le::build_voxel_mesh(layout, z, mp);
    CHECK(m1.voxels == m2.voxels);
    CHECK(le::encode_binary_stl(m1) == le::encode_binary_stl(m2));
}

// ======================================================================================================
// Streamed layer-list operations (Clone, Remove, ReHeight, Double / Phased exposure, Change resolution, Timelapse,
// Raise on finish, Lithophane): LayerDocument::apply_layer_list_stream / apply_parameter_list_stream must give the
// byte-identical document of the batch apply_*_list_operation, with the same per-layer parameters, one undo step.
// ======================================================================================================
namespace {
namespace ls = Slic3r::sla::liststream;
using F = sla::LayerParameter;
using Doc = sla::LayerDocument;

// 14 layers with a parameter table (a sliced / generated document); `plain` = no table (a raster document).
Doc make_fresh_doc()
{
    std::vector<sla::EncodedRaster> layers;
    for (size_t i = 0; i < kLayers; ++i) { const auto px = make_layer(i); layers.push_back(sla::PNGRasterEncoder{}(px.data(), kW, kH, 1)); }
    sla::FreshLayerJob job;
    job.producer = sla::FreshLayerProducer::Raster;
    job.geometry = {kW, kH, 16., 12.};
    job.globals.layer_height = kPitch;
    job.globals.bottom_layers = 3;
    job.globals.normal = {{F::Exposure, 3.}, {F::LiftDistance, 1.}, {F::LiftSpeed, 60.}, {F::LiftDistance2, 2.},
                          {F::LiftSpeed2, 120.}, {F::RetractDistance2, .5}, {F::RetractSpeed2, 120.},
                          {F::RetractSpeed, 150.}, {F::WaitBefore, .25}, {F::WaitAfter, .5}, {F::WaitAfterLift, .75},
                          {F::LightOff, 0.}, {F::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    for (size_t i = 0; i < kLayers; ++i) {
        auto row = job.globals.normal;
        row[F::Exposure]  = 10. + double(i);
        row[F::PositionZ] = kPitch * double(i + 1);
        job.rows.push_back(row);
    }
    return Doc::from_fresh_rasters(std::move(layers), std::move(job));
}

std::vector<sla::LayerParameters> rows_of(const Doc &doc)
{
    std::vector<sla::LayerParameters> rows;
    if (doc.fresh_job()) for (size_t i = 0; i < doc.layer_count(); ++i) rows.push_back(doc.layer_parameters(i));
    return rows;
}

// Everything a list operation may change.
void check_same(const Doc &a, const Doc &b)
{
    REQUIRE(a.layer_count() == b.layer_count());
    CHECK(bytes_of(a) == bytes_of(b));
    CHECK(rows_of(a) == rows_of(b));
    REQUIRE(a.fresh_job().has_value() == b.fresh_job().has_value());
    if (a.fresh_job()) {
        CHECK(a.fresh_job()->globals.bottom_layers == b.fresh_job()->globals.bottom_layers);
        CHECK(a.fresh_job()->globals.layer_height == b.fresh_job()->globals.layer_height);
        CHECK(a.fresh_job()->globals.transition_layers == b.fresh_job()->globals.transition_layers);
        CHECK(int(a.fresh_job()->position_policy) == int(b.fresh_job()->position_policy));
    }
}

struct ListCase {
    std::string name;
    bool fresh = true;                                    // needs the parameter table
    std::function<bool(Doc &)> batch, stream;
    bool resolution = false;
};

using Change = Doc::LayerListChange;

ListCase layer_case(const std::string &name, bool fresh, bool resolution,
                    std::function<Change(std::vector<lo::Mask> &, std::vector<double> &, const sla::LayerProgress &)> batch,
                    ls::Operation stream)
{
    ListCase c; c.name = name; c.fresh = fresh; c.resolution = resolution;
    c.batch = [batch, resolution](Doc &d) { return d.apply_layer_list_operation(batch, resolution, kPitch); };
    c.stream = [stream, resolution](Doc &d) { return d.apply_layer_list_stream(stream, resolution, kPitch); };
    return c;
}

ListCase param_case(const std::string &name,
                    std::function<Change(std::vector<lo::Mask> &, std::vector<double> &, std::vector<lo::LayerParams> &, const sla::LayerProgress &, const lo::PrinterContext &)> batch,
                    std::function<ls::Operation(const lo::PrinterContext &, const std::vector<lo::LayerParams> &)> stream)
{
    ListCase c; c.name = name; c.fresh = true;
    c.batch = [batch](Doc &d) {
        const auto context = d.parameter_printer_context(kPitch);
        return d.apply_parameter_list_operation([&](auto &m, auto &z, auto &t, const auto &pr) { return batch(m, z, t, pr, context); }, kPitch);
    };
    c.stream = [stream](Doc &d) { return d.apply_parameter_list_stream(stream(d.parameter_printer_context(kPitch), d.parameter_table()), kPitch); };
    return c;
}

std::vector<ListCase> list_cases()
{
    std::vector<ListCase> cases;
    {
        lo::CloneParams p; p.first_layer = 2; p.last_layer = 7; p.clones = 2;
        cases.push_back(layer_case("clone", false, false, [p](auto &l, auto &z, const auto &pr) { Change c; c.list = lo::clone_layers(l, z, p, pr); return c; }, ls::clone(p)));
        p.keep_same_z = true; p.first_layer = 0; p.last_layer = lo::kLastLayer; p.clones = 1;
        cases.push_back(layer_case("clone same z", false, false, [p](auto &l, auto &z, const auto &pr) { Change c; c.list = lo::clone_layers(l, z, p, pr); return c; }, ls::clone(p)));
    }
    {
        lo::RemoveParams p; p.first_layer = 1; p.last_layer = 5;
        cases.push_back(layer_case("remove range", false, false, [p](auto &l, auto &z, const auto &pr) { Change c; c.list = lo::remove_layers(l, z, p, pr); return c; }, ls::remove(p)));
        // A pixel threshold between the counts of the plate layers and the pillar layers.
        std::vector<size_t> counts;
        { auto doc = make_doc(); for (size_t i = 0; i < kLayers; ++i) { auto im = doc.image(i); size_t n = 0; for (auto v : im.buf) n += v != 0; counts.push_back(n); } }
        auto sorted = counts; std::sort(sorted.begin(), sorted.end());
        p.first_layer = 0; p.last_layer = lo::kLastLayer; p.use_threshold = true; p.pixel_threshold = unsigned(sorted[kLayers / 3]);
        cases.push_back(layer_case("remove threshold", false, false, [p](auto &l, auto &z, const auto &pr) { Change c; c.list = lo::remove_layers(l, z, p, pr); return c; }, ls::remove(p)));
    }
    {
        const auto reheight = [&](lo::ReHeightParams p, const std::string &name) {
            cases.push_back(layer_case(name, false, false, [p](auto &l, auto &z, const auto &pr) { Change c; c.list = lo::reheight_layers(l, z, p, pr); return c; }, ls::reheight(p)));
        };
        lo::ReHeightParams p;
        p.preset = {false, 2, kPitch / 2, kLayers * 2}; reheight(p, "reheight split");
        for (auto aa : {lo::ReHeightAntiAliasing::None, lo::ReHeightAntiAliasing::Average, lo::ReHeightAntiAliasing::Difference}) {
            p.preset = {true, 2, kPitch * 2, kLayers / 2}; p.anti_aliasing = aa;
            reheight(p, "reheight merge " + std::to_string(int(aa)));
        }
        p.preset = {true, 7, kPitch * 7, kLayers / 7}; p.anti_aliasing = lo::ReHeightAntiAliasing::Average; reheight(p, "reheight merge x7");
        p.method = lo::ReHeightMethod::OffsetPositionZ; p.first_layer = 3; p.last_layer = 9; p.position_z_offset_mm = 0.01;
        reheight(p, "reheight offset z");
    }
    {
        lo::DoubleExposureParams p; p.first_layer = 1; p.last_layer = 11; p.bottom_layer_count = 3;
        p.first_bottom_exposure_s = 20; p.first_normal_exposure_s = 2; p.second_bottom_exposure_s = 8; p.second_normal_exposure_s = 1;
        p.first_bottom_erode = 2; p.second_bottom_erode = 0; p.first_normal_erode = 1; p.second_normal_erode = 0;
        p.second_layer_difference = true; p.second_layer_difference_overlap_erode = 1;
        p.second_layer_lift_enabled = true; p.second_layer_lift_mm = 0.5; p.second_layer_wait_enabled = true; p.second_layer_wait_s = 2;
        cases.push_back(layer_case("double exposure", true, false, [p](auto &l, auto &z, const auto &pr) {
            auto r = lo::double_exposure_layers(l, z, p, pr);
            Change c; c.list = std::move(r.list); c.exposure_s = std::move(r.exposure_s); c.lift_mm = std::move(r.lift_mm);
            c.wait_before_cure_s = std::move(r.wait_before_cure_s); c.bottom_layer_count = r.bottom_layer_count; return c; }, ls::double_exposure(p)));
        p.second_layer_difference = false; p.first_normal_erode = p.second_normal_erode = 2; p.first_bottom_erode = p.second_bottom_erode = 2;
        cases.push_back(layer_case("double exposure equal erode", true, false, [p](auto &l, auto &z, const auto &pr) {
            auto r = lo::double_exposure_layers(l, z, p, pr);
            Change c; c.list = std::move(r.list); c.exposure_s = std::move(r.exposure_s); c.lift_mm = std::move(r.lift_mm);
            c.wait_before_cure_s = std::move(r.wait_before_cure_s); c.bottom_layer_count = r.bottom_layer_count; return c; }, ls::double_exposure(p)));
    }
    {
        lo::ChangeResolutionParams p; p.new_width = kW + 16; p.new_height = kH + 8;
        cases.push_back(layer_case("change resolution", false, true, [p](auto &l, auto &, const auto &pr) {
            Change c; c.list.source_index.resize(l.size()); for (size_t i = 0; i < l.size(); ++i) c.list.source_index[i] = i;
            lo::change_resolution_layers(l, p, pr); return c; }, ls::change_resolution(p)));
        p.fix_ratio = true; p.ratio_x = 0.9; p.ratio_y = 0.95;
        cases.push_back(layer_case("change resolution ratio", false, true, [p](auto &l, auto &, const auto &pr) {
            Change c; c.list.source_index.resize(l.size()); for (size_t i = 0; i < l.size(); ++i) c.list.source_index[i] = i;
            lo::change_resolution_layers(l, p, pr); return c; }, ls::change_resolution(p)));
    }
    {
        lo::PhasedExposureParams p = lo::phased_exposure_defaults({}, {}, false);
        p.first_layer = 1; p.last_layer = 12;
        p.phases = {{9, 1, 3, 1}, {5, 0, 2, 0}};
        p.difference_only = true; p.difference_overlap_iterations = 1; p.different_settings_for_sequential_layers = true;
        p.sequential_lift_enabled = true; p.sequential_lift_mm = 0.5;
        cases.push_back(param_case("phased exposure", [p](auto &l, auto &z, auto &t, const auto &pr, const auto &ctx) {
            auto r = lo::phased_exposure_layers(l, z, t, p, ctx, pr); Change c; c.list = std::move(r.list); c.bottom_layer_count = r.bottom_layer_count; return c; },
            [p](const auto &ctx, const auto &) { return ls::phased_exposure(p, ctx); }));
    }
    for (bool dummy : {true, false}) {
        lo::TimelapseParams p; p.mode = lo::TimelapseMode::VirtualLayer; p.raise_position_z_mm = 1.0; p.raise_each_nth_height_mm = 10; // only the last layer is due (photo layers in the middle break the Z order)
        p.output_dummy_pixel = dummy; p.ensure_last_layer = true; p.exposure_s = 0.1; p.use_custom_lift = !dummy;
        p.lift_speed = 60; p.lift_speed2 = 120; p.retract_speed = 150; p.retract_speed2 = 120; p.slow_lift_height_mm = 0.3; p.slow_retract_height_mm = 0.3;
        cases.push_back(param_case(std::string("timelapse virtual ") + (dummy ? "dummy" : "blank"), [p](auto &l, auto &z, auto &t, const auto &pr, const auto &ctx) {
            auto r = lo::timelapse_layers(l, z, t, p, ctx, pr); Change c; c.list = std::move(r.list); return c; },
            [p](const auto &ctx, const auto &) { return ls::timelapse(p, ctx); }));
    }
    for (bool dummy : {true, false}) {
        lo::RaiseOnPrintFinishParams p; p.position_z_mm = 2.0; p.wait_time_s = 30; p.output_dummy_pixel = dummy;
        cases.push_back(param_case(std::string("raise on finish ") + (dummy ? "dummy" : "blank"), [p](auto &l, auto &z, auto &t, const auto &, const auto &ctx) {
            const size_t count = l.size(); lo::raise_on_print_finish(l, z, t, p, ctx);
            Change c; c.list.source_index.resize(count + 1); for (size_t i = 0; i < count; ++i) c.list.source_index[i] = i; c.list.source_index[count] = count - 1; return c; },
            [p](const auto &ctx, const auto &) { return ls::raise_on_print_finish(p, ctx); }));
    }
    {
        lo::Mask image(30, 20);
        for (size_t y = 0; y < image.height; ++y) for (size_t x = 0; x < image.width; ++x) image.at(x, y) = uint8_t(20 + (x * 7 + y * 5) % 230);
        for (bool separate : {true, false}) {
            lo::LithophaneParams p; p.layer_height_mm = kPitch; p.bottom_layer_count = 2; p.separate_grayscale_pixels = separate;
            p.start_threshold = 20; p.end_threshold = 120; p.lithophane_height_mm = 1.0; p.base_thickness_mm = 0.2; p.base_margin_px = 2; p.anti_aliasing = true;
            cases.push_back(param_case(std::string("lithophane ") + (separate ? "grey levels" : "single"), [p, image](auto &l, auto &z, auto &t, const auto &pr, const auto &ctx) {
                const size_t old_count = l.size();
                const auto bottom = *std::find_if(t.begin(), t.end(), [](const auto &r) { return r.is_bottom; });
                const auto normal = *std::find_if(t.begin(), t.end(), [](const auto &r) { return !r.is_bottom; });
                std::vector<lo::Mask> layers; std::vector<double> heights; std::vector<lo::LayerParams> params;
                lo::lithophane_layers(image, kW, kH, p, ctx, layers, heights, params, pr);
                for (auto &row : params) { auto merged = row.is_bottom ? bottom : normal; merged.is_bottom = row.is_bottom; merged.exposure_s = row.exposure_s; row = merged; }
                l = std::move(layers); z = std::move(heights); t = std::move(params);
                Change c; for (size_t i = 0; i < l.size(); ++i) c.list.source_index.push_back(std::min(i, old_count - 1)); return c; },
                [p, image](const auto &ctx, const auto &table) {
                    // the document's first bottom / normal layer rows, as the recipe passes them
                    const auto bottom = std::find_if(table.begin(), table.end(), [](const auto &r) { return r.is_bottom; });
                    const auto normal = std::find_if(table.begin(), table.end(), [](const auto &r) { return !r.is_bottom; });
                    return ls::lithophane(image, kW, kH, p, ctx, *bottom, *normal); }));
        }
    }
    return cases;
}
} // namespace

TEST_CASE("Every streamed layer-list operation equals its batch result, with undo and redo", "[sla_layers][sla_layers_stream_list]")
{
    const std::vector<std::optional<uint64_t>> budgets{std::nullopt, kW * kH * 12};
    for (const auto &c : list_cases()) {
        for (const bool fresh : {true, false}) {
            if (c.fresh && !fresh) continue;
            for (const auto &budget : budgets) {
                INFO(c.name << (fresh ? " (generated)" : " (raster)") << " budget " << (budget ? *budget : 0));
                Doc a = fresh ? make_fresh_doc() : make_doc(), b = fresh ? make_fresh_doc() : make_doc();
                const auto original = bytes_of(a);
                const auto original_rows = rows_of(a);
                if (c.resolution && fresh) continue;
                REQUIRE(c.batch(a)); // the batch path decodes everything: default budget
                sla::LayerBudgetOverride limit; limit.decoded_bytes = budget;
                sla::ScopedLayerBudgets scoped(limit);
                const bool streamed = c.stream(b);
                REQUIRE(streamed);
                check_same(a, b);
                REQUIRE(b.can_undo());
                const auto done = bytes_of(b);
                const auto done_rows = rows_of(b);
                REQUIRE(b.undo());
                CHECK_FALSE(b.can_undo());
                CHECK(bytes_of(b) == original);
                CHECK(rows_of(b) == original_rows);
                REQUIRE(b.redo());
                CHECK(bytes_of(b) == done);
                CHECK(rows_of(b) == done_rows);
                CHECK_FALSE(b.can_redo());
            }
        }
    }
}

TEST_CASE("Streamed list operations that only re-index layers decode nothing", "[sla_layers][sla_layers_stream_list]")
{
    // A decoded-pixel budget of one byte: any decode would be refused, yet Clone, Remove (no threshold), ReHeight split
    // and offset only reference the existing payloads.
    sla::LayerBudgetOverride limit; limit.decoded_bytes = 1;
    sla::ScopedLayerBudgets scoped(limit);
    Doc doc = make_fresh_doc();
    const auto original = bytes_of(doc);
    lo::CloneParams clone; clone.first_layer = 3; clone.last_layer = 5; clone.clones = 3;
    REQUIRE(doc.apply_layer_list_stream(ls::clone(clone), false, kPitch));
    CHECK(doc.layer_count() == kLayers + 9);
    lo::RemoveParams remove; remove.first_layer = 0; remove.last_layer = 1;
    REQUIRE(doc.apply_layer_list_stream(ls::remove(remove), false, kPitch));
    lo::ReHeightParams split; split.preset = {false, 2, kPitch / 2, doc.layer_count() * 2};
    REQUIRE(doc.apply_layer_list_stream(ls::reheight(split), false, kPitch));
    CHECK(doc.layer_count() == (kLayers + 9 - 2) * 2);
    for (int i = 0; i < 3; ++i) REQUIRE(doc.undo());
    CHECK(bytes_of(doc) == original);
    // A merge needs layers: refused clearly under that budget, document untouched.
    lo::ReHeightParams merge; merge.preset = {true, 2, kPitch * 2, kLayers / 2};
    CHECK_THROWS_WITH(doc.apply_layer_list_stream(ls::reheight(merge), false, kPitch), ContainsSubstring("decoded"));
    CHECK(bytes_of(doc) == original);
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("A cancelled or refused streamed list operation leaves the document alone", "[sla_layers][sla_layers_stream_list]")
{
    Doc doc = make_fresh_doc();
    const auto original = bytes_of(doc);
    const auto rows = rows_of(doc);
    const auto revision = doc.revision();
    lo::DoubleExposureParams p; p.first_layer = 0; p.last_layer = lo::kLastLayer; p.bottom_layer_count = 3;
    p.first_bottom_exposure_s = 10; p.first_normal_exposure_s = 2; p.second_bottom_exposure_s = 5; p.second_normal_exposure_s = 1;
    std::atomic<int> calls{0};
    CHECK_FALSE(doc.apply_layer_list_stream(ls::double_exposure(p), false, kPitch, [&](size_t, size_t) { return ++calls < 4; }));
    CHECK(bytes_of(doc) == original);
    CHECK(rows_of(doc) == rows);
    CHECK(doc.revision() == revision);
    CHECK_FALSE(doc.can_undo());
    // Invalid parameters: a readable refusal, nothing changed.
    lo::RemoveParams all; all.first_layer = 0; all.last_layer = lo::kLastLayer;
    CHECK_THROWS_WITH(doc.apply_layer_list_stream(ls::remove(all), false, kPitch), ContainsSubstring("Keep at least one"));
    CHECK_THROWS(doc.apply_layer_list_stream(ls::change_resolution({}), true, kPitch)); // a generated document refuses a resolution change
    CHECK(bytes_of(doc) == original);
    CHECK_FALSE(doc.can_undo());
    // The structure is shared between the old and the new layout only through payload references: still readable.
    lo::CloneParams clone; clone.clones = 1;
    REQUIRE(doc.apply_layer_list_stream(ls::clone(clone), false, kPitch));
    REQUIRE(doc.undo());
    CHECK(bytes_of(doc) == original);
}

TEST_CASE("The infill window keeps only the model rectangle of its neighbours", "[sla_layers][sla_layers_stream]")
{
    // 20 layers of a 16x16 block on a 128x96 canvas; the floor / ceiling thickness spans 10 layers on each side. A
    // window of whole layers would need ~28 decoded layers; the cropped one fits a budget of 12.
    constexpr size_t W = 128, H = 96, N = 20;
    auto make = [&] {
        std::vector<sla::EncodedRaster> layers;
        for (size_t i = 0; i < N; ++i) {
            std::vector<uint8_t> px(W * H, 0);
            for (size_t y = 40; y < 56; ++y) for (size_t x = 56; x < 72; ++x) px[y * W + x] = 255;
            layers.push_back(sla::PNGRasterEncoder{}(px.data(), W, H, 1));
        }
        return sla::LayerDocument::from_rasters(std::move(layers));
    };
    std::vector<double> z(N);
    for (size_t i = 0; i < N; ++i) z[i] = kPitch * double(i + 1);
    lo::InfillParams p; p.selection = sel(0, lo::kLastLayer); p.floor_ceil_thickness_mm = kPitch * 9.5; p.wall_thickness = 1; p.infill_thickness = 1; p.infill_spacing = 4;
    p.type = lo::InfillAlgorithm::Pillars;
    auto reference = make();
    {
        std::vector<lo::Mask> masks;
        for (size_t i = 0; i < N; ++i) { auto im = reference.image(i); lo::Mask m(im.cols, im.rows); m.px = im.buf; masks.push_back(std::move(m)); }
        lo::infill_layers(masks, z, p);
        REQUIRE(reference.apply_mask_batch(sla::LayerMaskPlane::Main, [&](std::vector<lo::Mask> &l, const sla::LayerProgress &) { l = masks; }));
    }
    sla::LayerBudgetOverride limit; limit.decoded_bytes = W * H * 12;
    sla::ScopedLayerBudgets scoped(limit);
    auto doc = make();
    const auto kernel = [&](const lo::StreamLayout &l) { return lo::infill_kernel(p, l); };
    REQUIRE(doc.apply_mask_kernel(sla::LayerMaskPlane::Main, z, kernel));
    CHECK(bytes_of(doc) == bytes_of(reference));
    // The same window whole would not fit: the engine without a crop refuses it.
    sla::LayerDocument::MaskWindowSpec spec; spec.before = 10; spec.after = 10;
    auto other = make();
    CHECK_THROWS_WITH(other.apply_mask_window(sla::LayerMaskPlane::Main, spec, [](lo::Mask &, size_t, const sla::LayerDocument::MaskStreamContext &) {}),
                      ContainsSubstring("decoded-pixel budget"));
    spec.neighbour_crop = {56, 40, 16, 16};
    CHECK(other.apply_mask_window(sla::LayerMaskPlane::Main, spec, [](lo::Mask &, size_t, const sla::LayerDocument::MaskStreamContext &) {}));
}

// ======================================================================================================
// Streamed Dynamic lifts, Dynamic layer height and Repair layers (the last whole-document operations): the same
// results as the batch functions byte for byte (pixels, parameter rows, globals), with a default and a tiny decoded
// budget, undo / redo, cancellation, and a budget too small for the document.
// ======================================================================================================
namespace {
using Pixels = std::vector<uint8_t>;
using BatchFn = std::function<bool(Doc &)>;
using StreamFn = std::function<bool(Doc &, const sla::LayerProgress &)>;

Doc raster_from(const std::vector<Pixels> &pixels, size_t w, size_t h)
{
    std::vector<sla::EncodedRaster> layers;
    for (const auto &px : pixels) layers.push_back(sla::PNGRasterEncoder{}(px.data(), w, h, 1));
    return Doc::from_rasters(std::move(layers));
}

Doc fresh_from(const std::vector<Pixels> &pixels, size_t w, size_t h, size_t bottom = 3)
{
    std::vector<sla::EncodedRaster> layers;
    for (const auto &px : pixels) layers.push_back(sla::PNGRasterEncoder{}(px.data(), w, h, 1));
    sla::FreshLayerJob job;
    job.producer = sla::FreshLayerProducer::Raster;
    job.geometry = {w, h, 16., 12.};
    job.globals.layer_height = kPitch;
    job.globals.bottom_layers = bottom;
    job.globals.normal = {{F::Exposure, 3.}, {F::LiftDistance, 1.}, {F::LiftSpeed, 60.}, {F::LiftDistance2, 2.},
                          {F::LiftSpeed2, 120.}, {F::RetractDistance2, .5}, {F::RetractSpeed2, 120.},
                          {F::RetractSpeed, 150.}, {F::WaitBefore, .25}, {F::WaitAfter, .5}, {F::WaitAfterLift, .75},
                          {F::LightOff, 0.}, {F::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    for (size_t i = 0; i < pixels.size(); ++i) {
        auto row = job.globals.normal;
        row[F::Exposure]  = 10. + double(i);
        row[F::PositionZ] = kPitch * double(i + 1);
        job.rows.push_back(row);
    }
    return Doc::from_fresh_rasters(std::move(layers), std::move(job));
}

void fill_px(Pixels &px, size_t w, size_t h, int x0, int y0, int x1, int y1, uint8_t v)
{
    for (int y = std::max(0, y0); y < std::min<int>(int(h), y1); ++y)
        for (int x = std::max(0, x0); x < std::min<int>(int(w), x1); ++x) px[size_t(y) * w + size_t(x)] = v;
}

// 18 layers of 40 x 30 that stack in groups: identical layers, layers that differ by one anti-aliased pixel (the stack
// merges), by a 4 x 4 patch (the stack closes) and by a whole new shape.
constexpr size_t kSW = 40, kSH = 30;
std::vector<Pixels> stack_layers()
{
    std::vector<Pixels> layers;
    for (size_t i = 0; i < 18; ++i) {
        Pixels px(kSW * kSH, 0);
        if (i < 4) fill_px(px, kSW, kSH, 4, 4, 36, 26, 255);
        else if (i < 7) { fill_px(px, kSW, kSH, 6, 6, 30, 22, 255); if (i == 5) px[10 * kSW + 10] = 100; }
        else if (i == 7) fill_px(px, kSW, kSH, 10, 4, 34, 28, 200);
        else if (i < 12) { fill_px(px, kSW, kSH, 8, 8, 28, 20, 255); if (i == 10) fill_px(px, kSW, kSH, 14, 10, 18, 14, 0); }
        else if (i < 14) fill_px(px, kSW, kSH, 5, 5, 20, 25, 255);
        else { fill_px(px, kSW, kSH, 9, 9, 31, 21, 255); for (int x = 9; x < 31; ++x) px[size_t(9) * kSW + size_t(x)] = uint8_t(100 + 8 * (i - 14) + x); }
        layers.push_back(std::move(px));
    }
    return layers;
}

bool changed_anything(const Doc &before, const Doc &after)
{
    return before.layer_count() != after.layer_count() || bytes_of(before) != bytes_of(after) || rows_of(before) != rows_of(after);
}

// Batch and stream give the same document under the default budget and under a budget of `tiny_layers` decoded layers,
// stay one undo step, and a cancelled stream changes nothing.
void check_streams(const std::string &name, const std::function<Doc()> &make, size_t width, size_t height, size_t tiny_layers,
                   const BatchFn &batch, const StreamFn &stream)
{
    Doc reference = make();
    const auto original = bytes_of(reference);
    const auto original_rows = rows_of(reference);
    REQUIRE(batch(reference)); // the batch path decodes everything: default budget
    REQUIRE(changed_anything(make(), reference));
    for (const std::optional<uint64_t> &budget : std::vector<std::optional<uint64_t>>{std::nullopt, uint64_t(width) * height * tiny_layers}) {
        INFO(name << " budget " << (budget ? *budget : 0));
        Doc doc = make();
        sla::LayerBudgetOverride limit; limit.decoded_bytes = budget;
        sla::ScopedLayerBudgets scoped(limit);
        REQUIRE(stream(doc, {}));
        check_same(reference, doc);
        REQUIRE(doc.can_undo());
        const auto done = bytes_of(doc);
        const auto done_rows = rows_of(doc);
        REQUIRE(doc.undo());
        CHECK_FALSE(doc.can_undo());
        CHECK(bytes_of(doc) == original);
        CHECK(rows_of(doc) == original_rows);
        REQUIRE(doc.redo());
        CHECK(bytes_of(doc) == done);
        CHECK(rows_of(doc) == done_rows);
        CHECK_FALSE(doc.can_redo());
    }
    // Cancelled after n progress calls: nothing changes, no undo step, no new revision.
    size_t cancelled = 0;
    for (const size_t n : {1u, 2u, 4u, 9u, 30u}) {
        Doc doc = make();
        const auto revision = doc.revision();
        std::atomic<size_t> calls{0};
        if (stream(doc, [&](size_t, size_t) { return ++calls < n; })) continue;
        ++cancelled;
        CHECK(bytes_of(doc) == original);
        CHECK(rows_of(doc) == original_rows);
        CHECK(doc.revision() == revision);
        CHECK_FALSE(doc.can_undo());
    }
    CHECK(cancelled > 0);
}

lo::PrinterContext context_of(Doc &doc) { return doc.parameter_printer_context(kPitch); }
} // namespace

TEST_CASE("Streamed dynamic lifts give the batch parameter rows", "[sla_layers][sla_layers_stream_params]")
{
    struct Row { std::string name; lo::DynamicLiftsParams p; };
    std::vector<Row> rows;
    {
        lo::DynamicLiftsParams p; p.largest_lift_height = 8; p.smallest_lift_height = 2; p.largest_bottom_lift_height = 6; p.smallest_bottom_lift_height = 3;
        p.slowest_lift_speed = 40; p.fastest_lift_speed = 120; p.slowest_bottom_lift_speed = 30; p.fastest_bottom_lift_speed = 90;
        rows.push_back({"traditional", p});
        p.method = lo::DynamicLiftsMethod::FullRange; rows.push_back({"full range", p});
        p.first_layer = 2; p.last_layer = 10; rows.push_back({"full range, range", p});
        p.method = lo::DynamicLiftsMethod::Traditional; p.first_layer = 0; p.last_layer = lo::kLastLayer;
        p.smallest_lift_height = p.largest_lift_height = 0; p.smallest_bottom_lift_height = p.largest_bottom_lift_height = 0; // defaults from the first rows
        p.slowest_lift_speed = 20; p.fastest_lift_speed = 200; p.slowest_bottom_lift_speed = p.fastest_bottom_lift_speed = 0;
        rows.push_back({"defaults filled in", p});
    }
    for (const Row &row : rows) {
        const auto p = row.p;
        check_streams("dynamic lifts " + row.name, [] { return make_fresh_doc(); }, kW, kH, 2,
            [p](Doc &d) {
                return d.apply_parameter_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &, std::vector<lo::LayerParams> &table, const sla::LayerProgress &pr) {
                    lo::dynamic_lifts(layers, table, lo::dynamic_lifts_defaults(table, p), pr);
                    return Doc::LayerListChange{};
                }, true, kPitch);
            },
            [p](Doc &d, const sla::LayerProgress &pr) { return ls::apply_dynamic_lifts(d, p, kPitch, pr); });
    }
    // Decoded budget of one byte: nothing may be decoded whole; the lifts only count pixels layer by layer.
    {
        Doc a = make_fresh_doc(), b = make_fresh_doc();
        lo::DynamicLiftsParams p; p.largest_lift_height = 8; p.smallest_lift_height = 2;
        REQUIRE(a.apply_parameter_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &, std::vector<lo::LayerParams> &table, const sla::LayerProgress &pr) {
            lo::dynamic_lifts(layers, table, lo::dynamic_lifts_defaults(table, p), pr); return Doc::LayerListChange{}; }, true, kPitch));
        sla::LayerBudgetOverride limit; limit.decoded_bytes = 1;
        sla::ScopedLayerBudgets scoped(limit);
        REQUIRE(ls::apply_dynamic_lifts(b, p, kPitch));
        check_same(a, b);
        // The batch path refuses the same document under that budget; the document is untouched.
        Doc c = make_fresh_doc();
        CHECK_THROWS_WITH(c.apply_parameter_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &, std::vector<lo::LayerParams> &table, const sla::LayerProgress &pr) {
            lo::dynamic_lifts(layers, table, lo::dynamic_lifts_defaults(table, p), pr); return Doc::LayerListChange{}; }, true, kPitch), ContainsSubstring("decoded-pixel budget"));
    }
    // Refused parameters read like the batch ones and change nothing.
    {
        Doc d = make_fresh_doc();
        const auto original = bytes_of(d); const auto original_rows = rows_of(d);
        lo::DynamicLiftsParams bad; // every smallest / largest value equal: nothing would change
        CHECK_THROWS_WITH(ls::apply_dynamic_lifts(d, bad, kPitch), ContainsSubstring("will not produce a change"));
        CHECK(bytes_of(d) == original); CHECK(rows_of(d) == original_rows); CHECK_FALSE(d.can_undo());
    }
}

TEST_CASE("Streamed dynamic layer height equals the batch stack", "[sla_layers][sla_layers_stream_params]")
{
    struct Row { std::string name; lo::DynamicLayerHeightParams p; };
    std::vector<Row> rows;
    {
        lo::DynamicLayerHeightParams p; p.minimum_layer_height_mm = 0.05; p.maximum_layer_height_mm = 0.2; p.bottom_exposure_s = 30; p.exposure_s = 3;
        rows.push_back({"linear", p});
        p.strip_anti_aliasing = true; p.reconstruct_anti_aliasing = true; rows.push_back({"strip + reconstruct AA", p});
        p.reconstruct_anti_aliasing = false; p.exposure_type = lo::ExposureSetType::Multiplier; p.iterate_bottom_exposure = true; p.maximum_layer_height_mm = 0.15;
        p.first_layer = 2; p.last_layer = 15; rows.push_back({"strip, multiplier, range", p});
        p.strip_anti_aliasing = false; p.maximum_erodes = 0; p.minimum_layer_height_mm = 0.1; p.first_layer = 0; p.last_layer = lo::kLastLayer;
        rows.push_back({"no erodes allowed", p});
        p.maximum_erodes = 10; p.maximum_layer_height_mm = 0.2; p.exposure_type = lo::ExposureSetType::Manual;
        p.manual_table = {{0.05, 30, 3}, {0.10, 32, 4}, {0.15, 34, 5}, {0.20, 36, 6}};
        rows.push_back({"manual table", p});
    }
    const auto make = [] { return fresh_from(stack_layers(), kSW, kSH); };
    for (const Row &row : rows) {
        const auto p = row.p;
        check_streams("dynamic layer height " + row.name, make, kSW, kSH, 12,
            [p](Doc &d) {
                const auto context = context_of(d);
                return d.apply_parameter_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &z, std::vector<lo::LayerParams> &table, const sla::LayerProgress &pr) {
                    Doc::LayerListChange change;
                    change.list = lo::dynamic_layer_height_layers(layers, z, table, p, context, pr).list;
                    return change;
                }, kPitch);
            },
            [p](Doc &d, const sla::LayerProgress &pr) { return d.apply_parameter_list_stream(ls::dynamic_layer_height(p, context_of(d)), kPitch, pr); });
    }
    // The stacks were really built (fewer layers), and unchanged layers kept the very PNG they had.
    {
        Doc d = make();
        const auto original = bytes_of(d);
        lo::DynamicLayerHeightParams p = rows[0].p;
        REQUIRE(d.apply_parameter_list_stream(ls::dynamic_layer_height(p, context_of(d)), kPitch));
        CHECK(d.layer_count() < original.size());
        CHECK(bytes_of(d).front() == original.front());
    }
    // Refusals: a document with moved layers, and a budget of fewer than the ~10 layers the walk holds. Nothing changes.
    {
        Doc d = make();
        const auto original = bytes_of(d);
        lo::DynamicLayerHeightParams p = rows[0].p;
        sla::LayerBudgetOverride limit; limit.decoded_bytes = kSW * kSH * 3;
        sla::ScopedLayerBudgets scoped(limit);
        CHECK_THROWS_WITH(d.apply_parameter_list_stream(ls::dynamic_layer_height(p, context_of(d)), kPitch), ContainsSubstring("decoded-pixel budget"));
        CHECK(bytes_of(d) == original); CHECK_FALSE(d.can_undo());
        Doc plain = raster_from(stack_layers(), kSW, kSH);
        CHECK_THROWS(plain.apply_parameter_list_stream(ls::dynamic_layer_height(p, lo::PrinterContext{}), kPitch));
    }
}

namespace {
constexpr size_t kRW = 48, kRH = 40, kRN = 16;
// A part with everything the repair looks at: a tiny island chain (recursive removal), a mid-size island that can be
// attached to the layers below, a resin trap, a suction cup, dead / noise pixels for gap closing and noise removal,
// grey edges and an empty layer at the top.
std::vector<Pixels> repair_layers_input()
{
    std::vector<Pixels> layers;
    for (size_t i = 0; i < kRN; ++i) {
        Pixels px(kRW * kRH, 0);
        auto fill = [&](int x0, int y0, int x1, int y1, uint8_t v) { fill_px(px, kRW, kRH, x0, y0, x1, y1, v); };
        if (i < 3) fill(4, 4, 44, 36, 255);
        else if (i == 15) { /* empty layer on top */ }
        else {
            fill(8, 8, 40, 32, 255);
            if (i >= 6 && i <= 8) fill(18, 16, 26, 24, 0);          // enclosed hole: resin trap
            if (i >= 9) fill(28, 12, 38, 22, 0);                    // wide cavity closed from below: suction cup
            if (i >= 4 && i <= 7) fill(42, 34, 44, 36, 255);        // 4 px chain with nothing under it (recursive removal)
            if (i == 8 || i == 10) fill(10, 34, 16, 37, 255);       // 18 px island two layers above its support
            if (i == 5) fill(12, 12, 13, 13, 0);                    // dead pixel
            if (i == 12) fill(20, 28, 21, 29, 0);
            if (i == 13) fill(6, 6, 7, 7, 255);                     // noise pixel next to the body
            if (i % 2) fill(7, 10, 8, 30, 120);                     // grey edge
        }
        layers.push_back(std::move(px));
    }
    return layers;
}

struct RepairRow { std::string name; lo::RepairLayersParams p; bool list = false; };
std::vector<RepairRow> repair_rows()
{
    std::vector<RepairRow> rows;
    lo::RepairLayersParams p;
    p.remove_empty_layers = false;
    p.detection.resin_trap.required_area_to_consider_suction_cup_px = 20;
    p.detection.resin_trap.required_height_to_consider_suction_cup_mm = 0.1;
    rows.push_back({"defaults", p});
    {
        auto q = p; q.remove_islands_recursive_iterations = 0; q.gap_closing_iterations = 2; q.noise_removal_iterations = 1; q.attach_islands_below_layers = 0;
        rows.push_back({"recursive until done, closing + opening", q});
    }
    {
        auto q = p; q.remove_islands_recursive_iterations = 1; q.attach_islands_below_layers = 3; q.resin_traps_overlap_by = 0; q.gap_closing_iterations = 0;
        rows.push_back({"single pass, attach 3, no overlap", q});
    }
    {
        auto q = p; q.repair_suction_cups = true; q.suction_cups_vent_hole = 6;
        rows.push_back({"suction cups", q});
        q.suction_cups_vent_hole = 10; q.attach_islands_below_layers = 0; q.remove_islands_recursive_iterations = 2;
        rows.push_back({"suction cups, wide vent", q});
    }
    {
        auto q = p; q.repair_islands = false; q.resin_traps_overlap_by = 3;
        rows.push_back({"resin traps only", q});
        q = p; q.repair_resin_traps = false; q.attach_islands_below_layers = 4;
        rows.push_back({"islands only", q});
    }
    {
        auto q = p; q.first_layer = 3; q.last_layer = 12; q.repair_suction_cups = true; q.suction_cups_vent_hole = 8;
        rows.push_back({"layer range", q});
    }
    {
        auto q = p; q.remove_empty_layers = true;
        rows.push_back({"remove empty layers", q, true});
        q.repair_suction_cups = true; q.suction_cups_vent_hole = 6; q.first_layer = 1; q.last_layer = 14;
        rows.push_back({"empty layers + suction cups, range", q, true});
    }
    return rows;
}

std::vector<double> pitch_z(size_t n) { std::vector<double> z(n); for (size_t i = 0; i < n; ++i) z[i] = kPitch * double(i + 1); return z; }

BatchFn repair_batch(const lo::RepairLayersParams &p, bool list, lo::RepairLayersResult *out = nullptr)
{
    if (list)
        return [p, out](Doc &d) {
            return d.apply_layer_list_operation([&](std::vector<lo::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &pr) {
                Doc::LayerListChange change;
                const auto result = lo::repair_layers(layers, z, p, pr);
                change.list = result.list;
                if (out) *out = result;
                return change;
            }, false, kPitch);
        };
    return [p, out](Doc &d) {
        return d.apply_mask_batch(sla::LayerMaskPlane::Main, [&](std::vector<lo::Mask> &layers, const sla::LayerProgress &pr) {
            std::vector<double> z = pitch_z(layers.size());
            const auto result = lo::repair_layers(layers, z, p, pr);
            if (out) *out = result;
        });
    };
}
StreamFn repair_stream(const lo::RepairLayersParams &p, bool list)
{
    if (list) return [p](Doc &d, const sla::LayerProgress &pr) { return d.apply_layer_list_stream(ls::repair_layers(p), false, kPitch, pr); };
    return [p](Doc &d, const sla::LayerProgress &pr) { return d.apply_pixel_list_stream(ls::repair_layers(p), kPitch, pr); };
}
} // namespace

TEST_CASE("The repair test part exercises every repair step", "[sla_layers][sla_layers_stream_params]")
{
    lo::RepairLayersResult seen;
    for (const RepairRow &row : repair_rows()) {
        Doc d = fresh_from(repair_layers_input(), kRW, kRH);
        lo::RepairLayersResult result;
        REQUIRE(repair_batch(row.p, row.list, &result)(d));
        INFO(row.name << ": removed " << result.islands_removed << " attached " << result.islands_attached << " traps " << result.resin_traps_filled
             << " cups " << result.suction_cups_drilled << " empty " << result.empty_layers_removed);
        seen.islands_removed += result.islands_removed; seen.islands_attached += result.islands_attached;
        seen.resin_traps_filled += result.resin_traps_filled; seen.suction_cups_drilled += result.suction_cups_drilled;
        seen.empty_layers_removed += result.empty_layers_removed;
    }
    CHECK(seen.islands_removed > 0);
    CHECK(seen.islands_attached > 0);
    CHECK(seen.resin_traps_filled > 0);
    CHECK(seen.suction_cups_drilled > 0);
    CHECK(seen.empty_layers_removed > 0);
}

TEST_CASE("Streamed repair layers equals the batch repair, with undo and redo", "[sla_layers][sla_layers_stream_params]")
{
    for (const RepairRow &row : repair_rows()) {
        for (const bool fresh : {true, false}) {
            if (row.list && !fresh) continue; // removing layers needs a sliced / generated document
            const auto make = [fresh] { return fresh ? fresh_from(repair_layers_input(), kRW, kRH) : raster_from(repair_layers_input(), kRW, kRH); };
            check_streams("repair " + row.name + (fresh ? " (generated)" : " (raster)"), make, kRW, kRH, 12, repair_batch(row.p, row.list), repair_stream(row.p, row.list));
        }
    }
}

TEST_CASE("Streamed repair works under a decoded budget smaller than the document and refuses a hopeless one", "[sla_layers][sla_layers_stream_params]")
{
    const auto rows = repair_rows();
    // The document is 16 layers of 1920 bytes; the budget holds 10 layers (detection works in blocks, the per-layer step needs 8).
    Doc a = raster_from(repair_layers_input(), kRW, kRH), b = raster_from(repair_layers_input(), kRW, kRH);
    REQUIRE(repair_batch(rows[0].p, false)(a));
    {
        sla::LayerBudgetOverride limit; limit.decoded_bytes = uint64_t(kRW) * kRH * 10;
        sla::ScopedLayerBudgets scoped(limit);
        REQUIRE(repair_stream(rows[0].p, false)(b, {}));
        check_same(a, b);
        // The batch operation of the document would have to decode all of it.
        Doc whole = raster_from(repair_layers_input(), kRW, kRH);
        CHECK_THROWS_WITH(whole.apply_mask_batch(sla::LayerMaskPlane::Main, [](std::vector<lo::Mask> &, const sla::LayerProgress &) {}), ContainsSubstring("decoded-pixel budget"));
    }
    // One layer of budget: the per-layer step cannot hold its temporaries; refused with the budget in the message, nothing changed.
    Doc c = raster_from(repair_layers_input(), kRW, kRH);
    const auto original = bytes_of(c);
    sla::LayerBudgetOverride limit; limit.decoded_bytes = uint64_t(kRW) * kRH;
    sla::ScopedLayerBudgets scoped(limit);
    CHECK_THROWS_WITH(repair_stream(rows[0].p, false)(c, {}), ContainsSubstring("decoded-pixel budget"));
    CHECK(bytes_of(c) == original);
    CHECK_FALSE(c.can_undo());
}
