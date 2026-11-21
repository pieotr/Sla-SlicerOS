// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer recipe (goals G6/R17, G12): the declarative JSON automation format. Covered here: precise parse error
// locations, dry run, a multi-step recipe with exact pixels and parameters, atomic outputs on failure and
// cancellation. The recipe runs on a synthetic fresh document, so no archive file is needed.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerRecipe.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/Zipper.hpp"

using namespace Slic3r;
namespace rc = Slic3r::sla::recipe;
namespace fs = boost::filesystem;
using F = sla::LayerParameter;
using Json = nlohmann::json;
using Catch::Approx;

namespace {
constexpr size_t kW = 8, kH = 4;
constexpr double kLayer = 0.05;

// Printer job for `count` layers of width x height: layer i exposes 10 + i s, Z = 0.05 * (i + 1) mm, one lift stage
// of 1 mm plus a second one of 2 mm, waits 0.25 / 0.5 / 0.75 s. The first `bottom` layers are bottom layers.
sla::FreshLayerJob make_job(size_t count, size_t width, size_t height, size_t bottom)
{
    sla::FreshLayerJob job;
    job.producer = sla::FreshLayerProducer::Raster;
    job.geometry = {width, height, 16., 8.};
    job.globals.layer_height = kLayer;
    job.globals.bottom_layers = bottom;
    job.globals.normal = {{F::Exposure, 3.}, {F::LiftDistance, 1.}, {F::LiftSpeed, 60.}, {F::LiftDistance2, 2.},
                          {F::LiftSpeed2, 120.}, {F::RetractDistance2, .5}, {F::RetractSpeed2, 120.},
                          {F::RetractSpeed, 150.}, {F::WaitBefore, .25}, {F::WaitAfter, .5}, {F::WaitAfterLift, .75},
                          {F::LightOff, 0.}, {F::LightPWM, 255.}};
    job.globals.bottom = job.globals.normal;
    for (size_t i = 0; i < count; ++i) {
        auto row = job.globals.normal;
        row[F::Exposure]  = 10. + double(i);
        row[F::PositionZ] = kLayer * double(i + 1);
        job.rows.push_back(row);
    }
    return job;
}

using Pixels = std::vector<uint8_t>;

sla::LayerDocument make_document_from(const std::vector<Pixels> &pixels, size_t width, size_t height, size_t bottom = 1)
{
    std::vector<sla::EncodedRaster> layers;
    for (const auto &px : pixels) layers.push_back(sla::PNGRasterEncoder{}(px.data(), width, height, 1));
    return sla::LayerDocument::from_fresh_rasters(std::move(layers), make_job(pixels.size(), width, height, bottom));
}

// The same layers without any print parameters or printer job (a plain raster document).
sla::LayerDocument make_plain_document(const std::vector<Pixels> &pixels, size_t width, size_t height)
{
    std::vector<sla::EncodedRaster> layers;
    for (const auto &px : pixels) layers.push_back(sla::PNGRasterEncoder{}(px.data(), width, height, 1));
    return sla::LayerDocument::from_rasters(std::move(layers));
}

// Four 8x4 layers: a marker at (0,0) = 255 and a layer specific pixel (1 + i % 3, 1) = 200.
std::vector<Pixels> sparse_layers()
{
    std::vector<Pixels> layers;
    for (size_t i = 0; i < 4; ++i) {
        Pixels px(kW * kH, 0);
        px[0] = 255;
        px[kW + 1 + i % 3] = 200;
        layers.push_back(px);
    }
    return layers;
}

sla::LayerDocument make_document() { return make_document_from(sparse_layers(), kW, kH); }

// A filled rectangle on a width x height canvas.
Pixels block(size_t width, size_t height, size_t x0, size_t y0, size_t w, size_t h, uint8_t value = 255)
{
    Pixels px(width * height, 0);
    for (size_t y = y0; y < y0 + h; ++y)
        for (size_t x = x0; x < x0 + w; ++x) px[y * width + x] = value;
    return px;
}

Pixels pixels_of(const sla::LayerDocument &doc, size_t layer) { return doc.image(layer).buf; }
double value_of(const sla::LayerDocument &doc, size_t layer, sla::LayerParameter field) { return doc.layer_parameters(layer).at(field); }
size_t lit(const Pixels &px) { return size_t(std::count_if(px.begin(), px.end(), [](uint8_t v) { return v != 0; })); }

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("recipe-test-%%%%-%%%%-%%%%"))
    {
        fs::create_directories(path);
    }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
    std::string str(const std::string &name) const { return (path / name).string(); }
    size_t entries() const { return size_t(std::distance(fs::directory_iterator(path), fs::directory_iterator())); }
};

std::string recipe_text(const std::string &steps) { return R"({"version": 1, "steps": [)" + steps + "]}"; }
std::string quoted(const std::string &s) { return Json(s).dump(); }

// Location of the RecipeError a recipe text produces ("" when it parses).
std::string error_location(const std::string &text)
{
    try {
        rc::parse_recipe(text);
    } catch (const rc::RecipeError &e) {
        return e.location;
    }
    return {};
}

// Parses and runs `steps` on `doc`.
rc::RunResult run_on(const std::string &steps, sla::LayerDocument doc, bool dry_run = false, const std::string &base_dir = {})
{
    rc::RunOptions options;
    options.document = std::move(doc);
    options.dry_run  = dry_run;
    options.base_dir = base_dir;
    return rc::run_recipe(rc::parse_recipe(recipe_text(steps)), options);
}

std::vector<uint8_t> expected_pixels(size_t layer)
{
    // After flip horizontal + threshold 127: marker mirrored to the right edge, the 200 pixel mirrored and raised to 255.
    std::vector<uint8_t> px(kW * kH, 0);
    px[kW - 1] = 255;
    px[kW + (kW - 1 - (1 + layer % 3))] = 255;
    return px;
}
} // namespace

TEST_CASE("Recipe parse errors name the exact location", "[sla_layers][sla_layer_recipe]")
{
    SECTION("JSON syntax errors give line and column")
    {
        CHECK(error_location("{\n  \"version\": 1,\n  \"steps\": [ }\n}").rfind("line 3", 0) == 0);
    }
    SECTION("top level")
    {
        CHECK(error_location(R"({"steps": [{"op": "flip"}]})") == "version");
        CHECK(error_location(R"({"version": 2, "steps": [{"op": "flip"}]})") == "version");
        CHECK(error_location(R"({"version": 1})") == "steps");
        CHECK(error_location(R"({"version": 1, "steps": []})") == "steps");
        CHECK(error_location(R"({"version": 1, "steps": [{"op": "flip"}], "extra": 1})") == "extra");
        CHECK_THROWS_AS(rc::parse_recipe("[1]"), rc::RecipeError); // not an object
    }
    SECTION("steps")
    {
        CHECK(error_location(recipe_text(R"({"op": "flip"}, {"op": "frobnicate"})")) == "steps[1].op");
        CHECK(error_location(recipe_text(R"({"direction": "horizontal"})")) == "steps[0].op");
        CHECK(error_location(recipe_text(R"({"op": "flip"}, {"op": "open", "path": "a.zip"})")) == "steps[1].op");
        CHECK(error_location(recipe_text(R"({"op": "flip", "directon": "horizontal"})")) == "steps[0].directon");
        CHECK(error_location(recipe_text(R"({"op": "flip", "direction": "diagonal"})")) == "steps[0].direction");
        CHECK(error_location(recipe_text(R"({"op": "flip", "make_copy": "yes"})")) == "steps[0].make_copy");
        CHECK(error_location(recipe_text(R"({"op": "threshold", "threshold": 300})")) == "steps[0].threshold");
        CHECK(error_location(recipe_text(R"({"op": "threshold", "threshold": 12.5})")) == "steps[0].threshold");
        CHECK(error_location(recipe_text(R"({"op": "flip", "first_layer": 5, "last_layer": 2})")) == "steps[0].first_layer");
        CHECK(error_location(recipe_text(R"({"op": "move", "roi": {"x": 1, "y": 1, "height": 4}})")) == "steps[0].roi.width");
        CHECK(error_location(recipe_text(R"({"op": "move", "roi": {"x": 1, "y": 1, "width": 2, "height": 4, "z": 1}})")) == "steps[0].roi.z");
        CHECK(error_location(recipe_text(R"({"op": "edit_parameters", "values": {"exposure_s": -1}})")) == "steps[0].values.exposure_s");
        CHECK(error_location(recipe_text(R"({"op": "convert", "format": "no_such_format", "path": "x"})")) == "steps[0].format");
        CHECK(error_location(recipe_text(R"({"op": "export_images"})")) == "steps[0].folder");
        CHECK(error_location(recipe_text(R"({"op": "detect_issues", "disable": ["wobble"]})")) == "steps[0].disable");
    }
    SECTION("a valid recipe parses, only the last save/convert step is the final output")
    {
        const auto recipe = rc::parse_recipe(recipe_text(R"({"op": "flip", "name": "mirror"}, {"op": "threshold", "threshold": 100})"));
        REQUIRE(recipe.steps.size() == 2);
        CHECK(recipe.steps[0].label == "mirror");
        CHECK(recipe.steps[1].op == "threshold");
        CHECK_FALSE(recipe.steps[1].final_output);
    }
    SECTION("the catalogue lists the operations")
    {
        std::vector<std::string> names;
        for (const auto &op : rc::operation_catalogue()) names.push_back(op.name);
        CHECK(std::find(names.begin(), names.end(), "flip") != names.end());
        CHECK(std::find(names.begin(), names.end(), "convert") != names.end());
    }
}

TEST_CASE("Dry run checks everything and changes nothing", "[sla_layers][sla_layer_recipe]")
{
    TempDir dir;
    const std::string folder = dir.str("images");
    const auto text = recipe_text(R"({"op": "flip"}, {"op": "threshold"}, {"op": "export_images", "folder": )" + quoted(folder) + "}");
    const auto recipe = rc::parse_recipe(text);

    rc::RunOptions options;
    options.dry_run  = true;
    options.document = make_document();
    const auto result = rc::run_recipe(recipe, options);
    REQUIRE(result.ok);
    CHECK(result.dry_run);
    REQUIRE(result.steps.size() == 3);
    for (const auto &s : result.steps) CHECK(s.status == rc::StepResult::Status::Planned);
    CHECK_FALSE(result.document.has_value());
    CHECK(result.outputs.empty());
    CHECK_FALSE(fs::exists(folder));
    CHECK(dir.entries() == 0); // not even a staging file

    SECTION("a step that cannot run is reported with its index")
    {
        const auto bad = rc::parse_recipe(recipe_text(R"({"op": "flip"}, {"op": "threshold", "first_layer": 99})"));
        const auto failed = rc::run_recipe(bad, options);
        CHECK_FALSE(failed.ok);
        CHECK_FALSE(failed.cancelled);
        CHECK(failed.failed_step == 1);
        CHECK(failed.steps[1].status == rc::StepResult::Status::Failed);
        CHECK(failed.error.find("steps[1] (threshold)") == 0);
        CHECK(failed.error.find("first_layer 99") != std::string::npos);
    }
    SECTION("an output folder that already holds files is refused before anything runs")
    {
        fs::create_directories(folder);
        boost::nowide::ofstream(folder + "/keep.txt") << "x";
        const auto failed = rc::run_recipe(recipe, options);
        CHECK_FALSE(failed.ok);
        CHECK(failed.failed_step == 2);
        CHECK(fs::exists(folder + "/keep.txt"));
    }
    SECTION("checks after a layer-list step are skipped, not guessed")
    {
        const auto clone = rc::parse_recipe(recipe_text(R"({"op": "clone", "first_layer": 1, "last_layer": 1}, {"op": "threshold", "first_layer": 4})"));
        const auto planned = rc::run_recipe(clone, options);
        REQUIRE(planned.ok);
        CHECK(planned.steps[1].message.find("not checked") == 0);
    }
}

TEST_CASE("A multi-step recipe produces the exact pixels, parameters and outputs", "[sla_layers][sla_layer_recipe]")
{
    TempDir dir;
    const std::string folder = dir.str("images"), report = dir.str("issues.json");
    const auto recipe = rc::parse_recipe(recipe_text(
        R"({"op": "flip", "direction": "horizontal", "name": "mirror"},)"
        R"({"op": "threshold", "threshold": 127, "maximum": 255, "type": "binary"},)"
        R"({"op": "edit_parameters", "values": {"exposure_s": 5}, "bottom": {"exposure_s": 8}},)"
        R"({"op": "detect_issues", "output": )" + quoted(report) + R"(},)"
        R"({"op": "export_images", "folder": )" + quoted(folder) + R"(, "filename_prefix": "L"})"));

    std::vector<size_t> seen;
    rc::RunOptions options;
    options.document = make_document();
    options.progress = [&](size_t done, size_t total) { REQUIRE(total == 600); seen.push_back(done); return true; };
    auto result = rc::run_recipe(recipe, options);
    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(result.steps.size() == 5);
    for (const auto &s : result.steps) CHECK(s.status == rc::StepResult::Status::Done);
    CHECK(result.steps[0].label == "mirror");
    CHECK(std::is_sorted(seen.begin(), seen.end()));

    REQUIRE(result.document.has_value());
    const auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 4);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(doc.image(i).buf == expected_pixels(i));
        CHECK(doc.layer_parameters(i).at(F::Exposure) == (i == 0 ? 8. : 5.)); // layer 0 is the bottom layer
        CHECK(doc.layer_parameters(i).at(F::LiftDistance) == 1.);             // untouched fields stay
    }
    CHECK(doc.can_undo());

    // Outputs were published (and only the declared ones): 4 PNGs, the issue report, no staging leftovers.
    REQUIRE(result.outputs.size() == 2);
    CHECK(fs::is_directory(folder));
    size_t pngs = 0;
    for (fs::directory_iterator it(folder); it != fs::directory_iterator(); ++it) pngs += it->path().extension() == ".png";
    CHECK(pngs == 4);
    boost::nowide::ifstream file(report);
    std::stringstream text;
    text << file.rdbuf();
    const Json issues = Json::parse(text.str());
    CHECK(issues["layer_count"] == 4);
    CHECK(issues["issues"].is_array());
    CHECK(dir.entries() == 2);

    const Json json = Json::parse(rc::run_result_to_json(result));
    CHECK(json["ok"] == true);
    CHECK(json["steps"].size() == 5);
    CHECK(json["steps"][0]["status"] == "done");
}

TEST_CASE("Layer list steps keep per-layer parameters attached", "[sla_layers][sla_layer_recipe]")
{
    const auto recipe = rc::parse_recipe(recipe_text(R"({"op": "clone", "first_layer": 1, "last_layer": 1, "clones": 1})"));
    rc::RunOptions options;
    options.document = make_document();
    const auto result = rc::run_recipe(recipe, options);
    REQUIRE(result.ok);
    const auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 5);
    const double expected[] = {10., 11., 11., 12., 13.};
    for (size_t i = 0; i < 5; ++i) CHECK(doc.layer_parameters(i).at(F::Exposure) == expected[i]);
}

TEST_CASE("A failing step leaves every output untouched", "[sla_layers][sla_layer_recipe]")
{
    TempDir dir;
    const std::string folder = dir.str("images");
    // Step 0 would export; step 1 fails at run time (layer 99 does not exist).
    const auto recipe = rc::parse_recipe(recipe_text(
        R"({"op": "export_images", "folder": )" + quoted(folder) + R"(}, {"op": "flip", "first_layer": 99})"));
    rc::RunOptions options;
    options.document = make_document();
    const auto result = rc::run_recipe(recipe, options);
    CHECK_FALSE(result.ok);
    CHECK(result.failed_step == 1);
    CHECK(result.steps[0].status == rc::StepResult::Status::Done); // it ran, but was never published
    CHECK(result.steps[1].status == rc::StepResult::Status::Failed);
    CHECK(result.outputs.empty());
    CHECK_FALSE(result.document.has_value());
    CHECK_FALSE(fs::exists(folder));
    CHECK(dir.entries() == 0);
}

TEST_CASE("Two steps cannot write the same output", "[sla_layers][sla_layer_recipe]")
{
    TempDir dir;
    const std::string folder = dir.str("images");
    const auto recipe = rc::parse_recipe(recipe_text(
        R"({"op": "export_images", "folder": )" + quoted(folder) + R"(}, {"op": "export_images", "folder": )" + quoted(folder) + "}"));
    rc::RunOptions options;
    options.document = make_document();
    const auto result = rc::run_recipe(recipe, options);
    CHECK_FALSE(result.ok);
    CHECK(result.failed_step == 1);
    CHECK(result.error.find("same output") != std::string::npos);
    CHECK(dir.entries() == 0);
}

TEST_CASE("Cancelling a recipe leaves no output behind", "[sla_layers][sla_layer_recipe]")
{
    TempDir dir;
    const std::string folder = dir.str("images");
    const auto recipe = rc::parse_recipe(recipe_text(
        R"({"op": "export_images", "folder": )" + quoted(folder) + R"(}, {"op": "flip"}, {"op": "threshold"})"));

    SECTION("between steps")
    {
        rc::RunOptions options;
        options.document = make_document();
        options.progress = [](size_t done, size_t) { return done < 100; }; // step 0 finished -> cancel at the start of step 1
        const auto result = rc::run_recipe(recipe, options);
        CHECK(result.cancelled);
        CHECK_FALSE(result.ok);
        CHECK_FALSE(result.document.has_value());
        CHECK(result.outputs.empty());
        CHECK_FALSE(fs::exists(folder));
        CHECK(dir.entries() == 0);
    }
    SECTION("inside an operation")
    {
        const auto mask_only = rc::parse_recipe(recipe_text(R"({"op": "flip"})"));
        rc::RunOptions options;
        options.document = make_document();
        options.progress = [](size_t done, size_t) { return done == 0; }; // allow the step to start, cancel inside it
        const auto result = rc::run_recipe(mask_only, options);
        CHECK(result.cancelled);
        CHECK(result.steps[0].status == rc::StepResult::Status::Cancelled);
        CHECK_FALSE(result.document.has_value());
    }
    SECTION("just before publication")
    {
        rc::RunOptions options;
        options.document = make_document();
        // The final progress report (step count * 100) is the publication gate.
        options.progress = [](size_t done, size_t total) { return done < total - 100; };
        const auto result = rc::run_recipe(recipe, options);
        CHECK(result.cancelled);
        CHECK_FALSE(fs::exists(folder));
        CHECK(dir.entries() == 0);
    }
}

TEST_CASE("A recipe without a document or input reports a clear error", "[sla_layers][sla_layer_recipe]")
{
    const auto recipe = rc::parse_recipe(recipe_text(R"({"op": "flip"})"));
    const auto result = rc::run_recipe(recipe);
    CHECK_FALSE(result.ok);
    CHECK(result.failed_step == 0);
    CHECK(result.error.find("no document is open") != std::string::npos);
}

// ======================================================================================================
// The remaining UVtools operations: parameters, layer lists, pixel operations, lithophane, suggestions
// ======================================================================================================

TEST_CASE("The catalogue lists every operation of the layer operations module", "[sla_layers][sla_layer_recipe]")
{
    std::vector<std::string> names;
    for (const auto &op : rc::operation_catalogue()) names.push_back(op.name);
    for (const char *name : {"repair_layers", "reheight", "double_exposure", "phased_exposure", "dynamic_lifts", "fade_exposure", "timelapse",
                             "dynamic_layer_height", "raise_on_print_finish", "change_resolution", "infill", "raft_relief", "layer_arithmetic", "lithophane", "blur", "mask_image", "layer_import", "stir_resin", "redraw_model", "draw_text", "draw_support", "drill_drain_hole"})
        CHECK(std::find(names.begin(), names.end(), name) != names.end());
}

TEST_CASE("Recipe parse errors of the extended operations name the exact location", "[sla_layers][sla_layer_recipe]")
{
    auto location = [](const std::string &step) { return error_location(recipe_text(step)); };
    SECTION("misspelt or mistyped parameters")
    {
        CHECK(location(R"({"op": "repair_layers", "repair_island": true})") == "steps[0].repair_island");
        CHECK(location(R"({"op": "reheight", "layer_height_mm": "0.1"})") == "steps[0].layer_height_mm");
        CHECK(location(R"({"op": "double_exposure", "first_normal_erod": 1})") == "steps[0].first_normal_erod");
        CHECK(location(R"({"op": "dynamic_lifts", "method": "fastest"})") == "steps[0].method");
        CHECK(location(R"({"op": "fade_exposure", "from_exposure_s": -1})") == "steps[0].from_exposure_s");
        CHECK(location(R"({"op": "timelapse", "mode": "photo"})") == "steps[0].mode");
        CHECK(location(R"({"op": "dynamic_layer_height", "exposure_type": "quadratic"})") == "steps[0].exposure_type");
        CHECK(location(R"({"op": "raise_on_print_finish", "wait_time_s": "long"})") == "steps[0].wait_time_s");
        CHECK(location(R"({"op": "infill", "type": "spiral"})") == "steps[0].type");
        CHECK(location(R"({"op": "raft_relief", "dilate_iterations": 1.5})") == "steps[0].dilate_iterations");
        CHECK(location(R"({"op": "layer_arithmetic", "sentence": "1 = 2+3", "roi": {"x": 0, "y": 0, "width": 2}})") == "steps[0].roi.height");
        CHECK(location(R"({"op": "lithophane", "image": "a.png", "base_type": "round"})") == "steps[0].base_type");
        CHECK(location(R"({"op": "lithophane", "image": "a.png", "brightness_gain": 200})") == "steps[0].brightness_gain");
    }
    SECTION("missing or impossible values")
    {
        CHECK(location(R"({"op": "reheight"})") == "steps[0].layer_height_mm");
        CHECK(location(R"({"op": "reheight", "method": "offset_position_z"})").empty()); // the height is only needed by "reheight"
        CHECK(location(R"({"op": "fade_exposure", "layer_count": 0})") == "steps[0].layer_count");
        CHECK(location(R"({"op": "change_resolution", "new_width": 10})") == "steps[0].new_height");
        CHECK(location(R"({"op": "change_resolution", "new_width": 0, "new_height": 4})") == "steps[0].new_width");
        CHECK(location(R"({"op": "layer_arithmetic"})") == "steps[0].sentence");
        CHECK(location(R"({"op": "layer_arithmetic", "sentence": ""})") == "steps[0].sentence");
        CHECK(location(R"({"op": "lithophane"})") == "steps[0].image");
        CHECK(location(R"({"op": "infill", "infill_spacing": 0})") == "steps[0].infill_spacing");
        CHECK(location(R"({"op": "raft_relief", "type": "linked_lines"})").empty()); // ported in Fala 41
        CHECK(location(R"({"op": "raft_relief", "type": "spiral_lines"})") == "steps[0].type");
        CHECK(location(R"({"op": "repair_layers", "repair_islands": false, "repair_resin_traps": false})") == "steps[0].repair_islands");
        CHECK(location(R"({"op": "repair_layers", "first_layer": 4, "last_layer": 2})") == "steps[0].first_layer");
        CHECK(location(R"({"op": "dynamic_lifts", "smallest_lift_height": 5, "largest_lift_height": 2})") == "steps[0].largest_lift_height");
        CHECK(location(R"({"op": "dynamic_layer_height", "exposure_type": "manual"})") == "steps[0].manual_table");
        CHECK(location(R"({"op": "dynamic_layer_height", "exposure_type": "manual", "manual_table": [{"layer_height_mm": 0.1}]})") ==
              "steps[0].manual_table[0].bottom_exposure_s");
    }
    SECTION("phases")
    {
        CHECK(location(R"({"op": "phased_exposure", "phases": [{"bottom_exposure_s": 1, "exposure_s": 1}]})") == "steps[0].phases");
        CHECK(location(R"({"op": "phased_exposure", "phases": 2})") == "steps[0].phases");
        CHECK(location(R"({"op": "phased_exposure", "phases": [{"bottom_exposure_s": 1, "exposure_s": 1}, {"exposure_s": 2}]})") == "steps[0].phases[1].bottom_exposure_s");
        CHECK(location(R"({"op": "phased_exposure", "phases": [{"bottom_exposure_s": 1, "exposure_s": 1}, {"bottom_exposure_s": 1, "exposure_s": 2, "iteration": 3}]})") ==
              "steps[0].phases[1].iteration");
        CHECK(location(R"({"op": "phased_exposure", "phases": [{"bottom_exposure_s": 1, "exposure_s": 1}, {"bottom_exposure_s": 1, "exposure_s": 2}]})").empty());
    }
}

TEST_CASE("fade_exposure writes the faded exposure of every layer of its range", "[sla_layers][sla_layer_recipe]")
{
    auto result = run_on(R"({"op": "fade_exposure", "first_layer": 0, "layer_count": 4, "from_exposure_s": 10, "to_exposure_s": 14})", make_document());
    REQUIRE(result.ok);
    auto &doc = *result.document;
    // step = (14 - 10) / (4 + 1) = 0.8, added before every layer
    const double expected[] = {10.8, 11.6, 12.4, 13.2};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(value_of(doc, i, F::Exposure) == Approx(expected[i]));
        CHECK(value_of(doc, i, F::LiftDistance) == 1.); // everything else stays
        CHECK(value_of(doc, i, F::WaitAfter) == .5);
    }
    REQUIRE(doc.can_undo());
    doc.undo(); // one undo step
    for (size_t i = 0; i < 4; ++i) CHECK(value_of(doc, i, F::Exposure) == 10. + double(i));

    SECTION("omitted exposures fade from the first bottom to the first normal layer")
    {
        result = run_on(R"({"op": "fade_exposure", "first_layer": 1, "layer_count": 2})", make_document());
        REQUIRE(result.ok);
        // from 10 (bottom layer) to 11 (first normal layer): step 1 / 3 = 0.33
        CHECK(value_of(*result.document, 0, F::Exposure) == 10.);
        CHECK(value_of(*result.document, 1, F::Exposure) == Approx(10.33));
        CHECK(value_of(*result.document, 2, F::Exposure) == Approx(10.66));
        CHECK(value_of(*result.document, 3, F::Exposure) == 13.);
    }
    SECTION("refusals are step errors")
    {
        result = run_on(R"({"op": "fade_exposure", "first_layer": 2, "layer_count": 4, "from_exposure_s": 1, "to_exposure_s": 2})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("steps[0] (fade_exposure)") == 0);
        CHECK(result.error.find("exceeds the number of layers") != std::string::npos);
        result = run_on(R"({"op": "fade_exposure", "from_exposure_s": 5, "to_exposure_s": 5})", make_document());
        CHECK(result.error.find("can't be the same") != std::string::npos);
        result = run_on(R"({"op": "fade_exposure", "first_layer": 9, "layer_count": 1, "from_exposure_s": 1, "to_exposure_s": 2})", make_document());
        CHECK(result.error.find("first_layer 9 is outside") != std::string::npos);
    }
}

TEST_CASE("dynamic_lifts sets lift height and speed from the lit area", "[sla_layers][sla_layer_recipe]")
{
    // Lit pixels per layer: 2, 2, 6, 11 (layer 0 is the only bottom layer).
    auto layers = sparse_layers();
    for (size_t x = 0; x < 4; ++x) layers[2][3 * kW + x] = 255;
    for (size_t x = 0; x < kW; ++x) layers[3][3 * kW + x] = 255;
    layers[3][2 * kW + 1] = 255;
    REQUIRE(lit(layers[2]) == 6);
    REQUIRE(lit(layers[3]) == 11);

    const std::string step = R"({"op": "dynamic_lifts", "smallest_lift_height": 1, "largest_lift_height": 5, "slowest_lift_speed": 50, "fastest_lift_speed": 200,
        "smallest_bottom_lift_height": 1, "largest_bottom_lift_height": 4, "slowest_bottom_lift_speed": 40, "fastest_bottom_lift_speed": 100})";
    auto result = run_on(step, make_document_from(layers, kW, kH));
    REQUIRE(result.ok);
    auto &doc = *result.document;
    // The lift after layer i-1 depends on the area of layer i-1; layer 0 uses itself. Traditional method:
    // height = largest * area / max area of the normal layers (11), speed = fastest - fastest * area / max, both clamped.
    const double height[] = {4, 1, 1, 2.7}, speed[] = {40, 163.6, 163.6, 90.9};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(value_of(doc, i, F::LiftDistance) == Approx(height[i]));
        CHECK(value_of(doc, i, F::LiftSpeed) == Approx(speed[i]));
        CHECK(value_of(doc, i, F::LiftDistance2) == 0.); // the total lift is written to the first stage
        CHECK(value_of(doc, i, F::RetractDistance2) == 0.);
        CHECK(value_of(doc, i, F::Exposure) == 10. + double(i));
    }
    REQUIRE(doc.can_undo());
    doc.undo();
    CHECK(value_of(doc, 3, F::LiftDistance) == 1.);
    CHECK(value_of(doc, 3, F::LiftDistance2) == 2.);

    SECTION("limits left out take the values of the first bottom / normal layer, which here produce no change")
    {
        result = run_on(R"({"op": "dynamic_lifts"})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("will not produce a change") != std::string::npos);
    }
    SECTION("a range outside the document is refused")
    {
        result = run_on(R"({"op": "dynamic_lifts", "first_layer": 7, "smallest_lift_height": 1, "largest_lift_height": 2})", make_document());
        CHECK(result.error.find("first_layer 7 is outside") != std::string::npos);
    }
}

TEST_CASE("phased_exposure doubles every layer with its own exposure and lift", "[sla_layers][sla_layer_recipe]")
{
    const std::string phases = R"("phases": [{"bottom_exposure_s": 20, "exposure_s": 5}, {"bottom_exposure_s": 6, "exposure_s": 7}],
        "difference_only": false)";
    auto result = run_on(R"({"op": "phased_exposure", )" + phases + R"(, "sequential_lift_mm": 1.2, "sequential_wait_before_cure_s": 1.5})", make_document());
    REQUIRE(result.ok);
    auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 8);
    const auto original = sparse_layers();
    const double exposure[] = {20, 6, 5, 7, 5, 7, 5, 7};
    for (size_t i = 0; i < 8; ++i) {
        const bool second = i % 2 == 1;
        CHECK(value_of(doc, i, F::PositionZ) == Approx(kLayer * double(i / 2 + 1))); // both phases print at the same Z
        CHECK(value_of(doc, i, F::Exposure) == Approx(exposure[i]));
        CHECK(value_of(doc, i, F::LiftDistance) == Approx(second ? 1.2 : 1.));
        CHECK(value_of(doc, i, F::LiftDistance2) == (second ? 0. : 2.));
        CHECK(value_of(doc, i, F::WaitBefore) == Approx(second ? 1.5 : .25));
        CHECK(pixels_of(doc, i) == original[i / 2]); // 0 iterations: the phases print the unchanged layer
    }
    CHECK(doc.fresh_job()->globals.bottom_layers == 2); // the bottom layer was split as well
    REQUIRE(doc.can_undo());
    doc.undo();
    CHECK(doc.layer_count() == 4);

    SECTION("a phase may erode the layer")
    {
        auto big = make_document_from({block(16, 16, 2, 2, 8, 8), block(16, 16, 2, 2, 8, 8)}, 16, 16, 1);
        result = run_on(R"({"op": "phased_exposure", "phases": [{"bottom_exposure_s": 20, "bottom_iterations": 1, "exposure_s": 5, "iterations": 2},
            {"bottom_exposure_s": 6, "exposure_s": 7}], "difference_only": false, "sequential_lift_mm": 1.2})", big);
        REQUIRE(result.ok);
        REQUIRE(result.document->layer_count() == 4);
        CHECK(lit(pixels_of(*result.document, 0)) == 36); // bottom layer, phase 1: eroded by 1 px
        CHECK(lit(pixels_of(*result.document, 1)) == 64); // phase 2: the full layer
        CHECK(lit(pixels_of(*result.document, 2)) == 16); // normal layer, phase 1: eroded by 2 px
        CHECK(lit(pixels_of(*result.document, 3)) == 64);
    }
    SECTION("the document refuses a result it cannot store: the retract height may not exceed the lift")
    {
        result = run_on(R"({"op": "phased_exposure", )" + phases + R"(, "sequential_lift_mm": 0.4})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("cannot be stored in this document") != std::string::npos);
    }
    SECTION("identical phases are refused")
    {
        result = run_on(R"({"op": "phased_exposure", "phases": [{"bottom_exposure_s": 7, "exposure_s": 7}, {"bottom_exposure_s": 7, "exposure_s": 7}]})", make_document());
        CHECK(result.error.find("Duplicated exposure sequence") != std::string::npos);
    }
}

TEST_CASE("raise_on_print_finish appends a layer that parks the platform", "[sla_layers][sla_layer_recipe]")
{
    auto result = run_on(R"({"op": "raise_on_print_finish", "position_z_mm": 5, "wait_time_s": 100})", make_document());
    REQUIRE(result.ok);
    auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 5);
    CHECK(value_of(doc, 4, F::PositionZ) == Approx(5.));
    CHECK(value_of(doc, 4, F::Exposure) == Approx(0.05));
    CHECK(value_of(doc, 4, F::WaitBefore) == Approx(100.));
    CHECK(value_of(doc, 4, F::LightPWM) == 0.);
    const auto dummy = pixels_of(doc, 4);
    CHECK(lit(dummy) == 1);
    CHECK(*std::max_element(dummy.begin(), dummy.end()) == 128);
    CHECK(pixels_of(doc, 3) == sparse_layers()[3]); // the model is untouched
    doc.undo();
    CHECK(doc.layer_count() == 4);

    SECTION("without the dummy pixel the layer is black")
    {
        result = run_on(R"({"op": "raise_on_print_finish", "position_z_mm": 5, "output_dummy_pixel": false})", make_document());
        REQUIRE(result.ok);
        CHECK(lit(pixels_of(*result.document, 4)) == 0);
    }
    SECTION("applying it twice is refused")
    {
        result = run_on(R"({"op": "raise_on_print_finish", "position_z_mm": 5}, {"op": "raise_on_print_finish", "position_z_mm": 6})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.failed_step == 1);
        CHECK(result.error.find("already been applied") != std::string::npos);
    }
    SECTION("a position below the model is refused")
    {
        result = run_on(R"({"op": "raise_on_print_finish", "position_z_mm": 0.1})", make_document());
        CHECK(result.error.find("below the maximum print height") != std::string::npos);
    }
    SECTION("a dry run reports the same refusal without running")
    {
        result = run_on(R"({"op": "raise_on_print_finish", "position_z_mm": 0.1})", make_document(), true);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("below the maximum print height") != std::string::npos);
        result = run_on(R"({"op": "raise_on_print_finish", "position_z_mm": 5})", make_document(), true);
        CHECK(result.ok);
        CHECK(result.steps[0].status == rc::StepResult::Status::Planned);
    }
}

TEST_CASE("timelapse raises the platform in lift height mode or with a virtual layer", "[sla_layers][sla_layer_recipe]")
{
    SECTION("lift height mode changes only the lift of the layers that are due")
    {
        // Z 0.05 .. 0.2, a raise every 0.1 mm of height counted from the first layer: layer 2 (0.15) is due.
        auto result = run_on(R"({"op": "timelapse", "raise_position_z_mm": 5, "raise_each_nth_height_mm": 0.1, "wait_after_lift_s": 2})", make_document());
        REQUIRE(result.ok);
        auto &doc = *result.document;
        REQUIRE(doc.layer_count() == 4);
        for (size_t i = 0; i < 4; ++i) {
            CHECK(value_of(doc, i, F::LiftDistance2) == (i == 2 ? Approx(3.85) : Approx(2.))); // 5 - 0.15 - 1 mm of the first stage
            CHECK(value_of(doc, i, F::WaitAfterLift) == (i == 2 ? Approx(2.) : Approx(.75)));
            CHECK(value_of(doc, i, F::PositionZ) == Approx(kLayer * double(i + 1)));
        }
        doc.undo();
        CHECK(value_of(doc, 2, F::LiftDistance2) == 2.);
    }
    SECTION("virtual layer mode with only the last layer due appends the photo layer")
    {
        auto result = run_on(R"({"op": "timelapse", "mode": "virtual_layer", "raise_position_z_mm": 5, "raise_each_nth_height_mm": 10, "exposure_s": 0.5})",
                             make_document());
        REQUIRE(result.ok);
        auto &doc = *result.document;
        REQUIRE(doc.layer_count() == 5);
        CHECK(value_of(doc, 4, F::PositionZ) == Approx(5.));
        CHECK(value_of(doc, 4, F::Exposure) == Approx(0.5));
        CHECK(value_of(doc, 4, F::LightPWM) == 1.);
        CHECK(lit(pixels_of(doc, 4)) == 1);
        for (size_t i = 0; i < 4; ++i) CHECK(pixels_of(doc, i) == sparse_layers()[i]);
        doc.undo();
        CHECK(doc.layer_count() == 4);
    }
    SECTION("photo layers in the middle would break the Z order: the document refuses")
    {
        auto result = run_on(R"({"op": "timelapse", "mode": "virtual_layer", "raise_position_z_mm": 5, "raise_each_nth_height_mm": 0.1})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("cannot be stored in this document") != std::string::npos);
    }
    SECTION("without a raise position the document cannot know the machine height")
    {
        auto result = run_on(R"({"op": "timelapse"})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("give raise_position_z_mm") != std::string::npos);
    }
    SECTION("settings that generate no lift are refused")
    {
        auto result = run_on(R"({"op": "timelapse", "raise_position_z_mm": 5, "raise_each_nth_height_mm": 0.1, "first_layer": 3, "last_layer": 3})", make_document());
        CHECK(result.error.find("does not generate any lift") != std::string::npos);
    }
}

TEST_CASE("dynamic_layer_height stacks identical layers", "[sla_layers][sla_layer_recipe]")
{
    const std::vector<Pixels> same(4, block(16, 16, 2, 2, 10, 10));
    auto result = run_on(R"({"op": "dynamic_layer_height", "maximum_layer_height_mm": 0.1})", make_document_from(same, 16, 16));
    REQUIRE(result.ok);
    auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 2);
    CHECK(value_of(doc, 0, F::PositionZ) == Approx(0.1));
    CHECK(value_of(doc, 1, F::PositionZ) == Approx(0.2));
    CHECK(value_of(doc, 0, F::Exposure) == Approx(10.));  // bottom exposure of the first bottom layer
    CHECK(value_of(doc, 1, F::Exposure) == Approx(11.2)); // first normal exposure 11 + 0.2 s for the doubled height
    CHECK(pixels_of(doc, 0) == same[0]);
    doc.undo();
    CHECK(doc.layer_count() == 4);

    SECTION("a maximum height that is not above the file layer height is refused")
    {
        result = run_on(R"({"op": "dynamic_layer_height", "maximum_layer_height_mm": 0.05})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("Maximum layer height") != std::string::npos);
    }
    SECTION("a manual exposure table is used")
    {
        result = run_on(R"({"op": "dynamic_layer_height", "maximum_layer_height_mm": 0.1, "exposure_type": "manual",
            "manual_table": [{"layer_height_mm": 0.05, "bottom_exposure_s": 10, "exposure_s": 11}, {"layer_height_mm": 0.1, "bottom_exposure_s": 30, "exposure_s": 40}]})",
                        make_document_from(same, 16, 16));
        REQUIRE(result.ok);
        CHECK(value_of(*result.document, 0, F::Exposure) == Approx(30.));
        CHECK(value_of(*result.document, 1, F::Exposure) == Approx(40.));
    }
}

TEST_CASE("reheight merges, splits and offsets layers", "[sla_layers][sla_layer_recipe]")
{
    const auto original = sparse_layers();
    SECTION("merge x2")
    {
        auto result = run_on(R"({"op": "reheight", "layer_height_mm": 0.1})", make_document());
        REQUIRE(result.ok);
        auto &doc = *result.document;
        REQUIRE(doc.layer_count() == 2);
        CHECK(value_of(doc, 0, F::PositionZ) == Approx(0.1));
        CHECK(value_of(doc, 1, F::PositionZ) == Approx(0.2));
        CHECK(value_of(doc, 0, F::Exposure) == 10.); // each merged layer keeps the values of its first source layer
        CHECK(value_of(doc, 1, F::Exposure) == 12.);
        Pixels first(kW * kH, 0), second(kW * kH, 0);
        first[0] = second[0] = 255;
        first[kW + 1] = first[kW + 2] = 200;  // layers 0 and 1
        second[kW + 3] = second[kW + 1] = 200; // layers 2 and 3
        CHECK(pixels_of(doc, 0) == first);
        CHECK(pixels_of(doc, 1) == second);
        doc.undo();
        CHECK(doc.layer_count() == 4);
    }
    SECTION("split x2")
    {
        auto result = run_on(R"({"op": "reheight", "method": "reheight", "layer_height_mm": 0.025})", make_document());
        REQUIRE(result.ok);
        auto &doc = *result.document;
        REQUIRE(doc.layer_count() == 8);
        for (size_t i = 0; i < 8; ++i) {
            CHECK(value_of(doc, i, F::PositionZ) == Approx(0.025 * double(i + 1)));
            CHECK(value_of(doc, i, F::Exposure) == 10. + double(i / 2));
            CHECK(pixels_of(doc, i) == original[i / 2]);
        }
    }
    SECTION("offset_position_z moves a range")
    {
        auto result = run_on(R"({"op": "reheight", "method": "offset_position_z", "position_z_offset_mm": 0.5, "first_layer": 2})", make_document());
        REQUIRE(result.ok);
        const double z[] = {0.05, 0.1, 0.65, 0.7};
        for (size_t i = 0; i < 4; ++i) CHECK(value_of(*result.document, i, F::PositionZ) == Approx(z[i]));
    }
    SECTION("an unreachable height lists what is possible")
    {
        auto result = run_on(R"({"op": "reheight", "layer_height_mm": 0.07})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("0.07 mm cannot be reached exactly") != std::string::npos);
        CHECK(result.error.find("0.025 (split x2)") != std::string::npos);
        CHECK(result.error.find("0.1 (merge x2)") != std::string::npos);
        result = run_on(R"({"op": "reheight", "layer_height_mm": 0.07})", make_document(), true); // the dry run knows as well
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("cannot be reached exactly") != std::string::npos);
    }
    SECTION("an offset that moves a layer below zero is refused")
    {
        auto result = run_on(R"({"op": "reheight", "method": "offset_position_z", "position_z_offset_mm": -1})", make_document());
        CHECK(result.error.find("under 0mm") != std::string::npos);
    }
}

TEST_CASE("double_exposure prints every layer twice with its own exposure, lift and wait", "[sla_layers][sla_layer_recipe]")
{
    const std::string step = R"({"op": "double_exposure", "first_normal_erode": 0, "first_bottom_erode": 0, "first_bottom_exposure_s": 20,
        "second_bottom_exposure_s": 21, "first_normal_exposure_s": 5, "second_normal_exposure_s": 6, "second_layer_lift_mm": 0.3, "second_layer_wait_s": 2})";
    auto result = run_on(step, make_document());
    REQUIRE(result.ok);
    auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 8);
    const auto original = sparse_layers();
    const double exposure[] = {20, 21, 5, 6, 5, 6, 5, 6};
    for (size_t i = 0; i < 8; ++i) {
        const bool second = i % 2 == 1;
        CHECK(value_of(doc, i, F::PositionZ) == Approx(kLayer * double(i / 2 + 1)));
        CHECK(value_of(doc, i, F::Exposure) == Approx(exposure[i]));
        CHECK(value_of(doc, i, F::LiftDistance) == Approx(second ? 0.3 : 1.));
        CHECK(value_of(doc, i, F::LiftDistance2) == (second ? 0. : 2.));
        CHECK(value_of(doc, i, F::WaitBefore) == Approx(second ? 2. : .25));
        CHECK(pixels_of(doc, i) == original[i / 2]);
    }
    CHECK(doc.fresh_job()->globals.bottom_layers == 2);
    doc.undo();
    CHECK(doc.layer_count() == 4);

    SECTION("omitted exposures use those of the document")
    {
        result = run_on(R"({"op": "double_exposure", "first_normal_erode": 0, "first_bottom_erode": 0})", make_document());
        REQUIRE(result.ok);
        // first pass = second pass = the layer's first bottom / normal layer exposure (10 / 11)
        const double same[] = {10, 10, 11, 11, 11, 11, 11, 11};
        for (size_t i = 0; i < 8; ++i) CHECK(value_of(*result.document, i, F::Exposure) == Approx(same[i]));
    }
    SECTION("only the selected range doubles")
    {
        result = run_on(R"({"op": "double_exposure", "first_layer": 2, "last_layer": 2, "first_normal_erode": 0, "first_normal_exposure_s": 5, "second_normal_exposure_s": 6})",
                        make_document());
        REQUIRE(result.ok);
        REQUIRE(result.document->layer_count() == 5);
        const double exposure5[] = {10, 11, 5, 6, 13};
        for (size_t i = 0; i < 5; ++i) CHECK(value_of(*result.document, i, F::Exposure) == Approx(exposure5[i]));
    }
    SECTION("a document without exposure values needs them in the recipe")
    {
        result = run_on(R"({"op": "double_exposure"})", make_plain_document(sparse_layers(), kW, kH));
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("first_bottom_exposure_s") != std::string::npos);
    }
    SECTION("layers that already share a Z are refused")
    {
        result = run_on(step + ", " + step, make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.failed_step == 1);
        CHECK(result.error.find("same z position") != std::string::npos);
    }
}

TEST_CASE("change_resolution crops or pads around the model on documents that store their resolution", "[sla_layers][sla_layer_recipe]")
{
    auto result = run_on(R"({"op": "change_resolution", "new_width": 10, "new_height": 6})", make_plain_document(sparse_layers(), kW, kH));
    REQUIRE(result.ok);
    auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 4);
    // The model bounds over all layers are x 0..3, y 0..1 (4 x 2 px); they are centred on the new 10 x 6 canvas: offset (3, 2).
    for (size_t i = 0; i < 4; ++i) {
        Pixels expected(10 * 6, 0);
        expected[2 * 10 + 3] = 255;                  // marker (0,0)
        expected[3 * 10 + 3 + 1 + i % 3] = 200;      // (1 + i % 3, 1)
        CHECK(pixels_of(doc, i) == expected);
        CHECK(doc.image(i).cols == 10);
        CHECK(doc.image(i).rows == 6);
    }
    doc.undo();
    CHECK(doc.image(0).cols == kW);

    SECTION("a canvas that clips the model is refused")
    {
        result = run_on(R"({"op": "change_resolution", "new_width": 3, "new_height": 6})", make_plain_document(sparse_layers(), kW, kH));
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("not large enough") != std::string::npos);
    }
    SECTION("the same resolution is refused")
    {
        result = run_on(R"({"op": "change_resolution", "new_width": 8, "new_height": 4})", make_plain_document(sparse_layers(), kW, kH));
        CHECK(result.error.find("must be different") != std::string::npos);
    }
    SECTION("sliced and generated documents tie their resolution to the display and refuse")
    {
        result = run_on(R"({"op": "change_resolution", "new_width": 10, "new_height": 6})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("steps[0] (change_resolution)") == 0);
        CHECK(result.error.find("Changing the resolution is not available") != std::string::npos);
        result = run_on(R"({"op": "change_resolution", "new_width": 10, "new_height": 6})", make_document(), true);
        CHECK_FALSE(result.ok); // refused by the dry run as well
    }
}

TEST_CASE("layer_arithmetic computes layers from other layers", "[sla_layers][sla_layer_recipe]")
{
    auto result = run_on(R"({"op": "layer_arithmetic", "sentence": "2 = 0+1"})", make_document());
    REQUIRE(result.ok);
    const auto original = sparse_layers();
    Pixels expected(kW * kH, 0);
    expected[0] = 255;
    expected[kW + 1] = 200; // layer 0
    expected[kW + 2] = 200; // layer 1
    CHECK(pixels_of(*result.document, 2) == expected);
    for (size_t i : {0, 1, 3}) CHECK(pixels_of(*result.document, i) == original[i]);

    SECTION("a region of interest limits the computation")
    {
        result = run_on(R"({"op": "layer_arithmetic", "sentence": "2 = 0+1", "roi": {"x": 2, "y": 0, "width": 2, "height": 2}})", make_document());
        REQUIRE(result.ok);
        // Columns 0..1 stay. Columns 2..3 become (layer 0 + layer 1) cropped to the region: only the 200 pixel of layer 1
        // at x = 2 is in it, so layer 2's own pixel at x = 3 is overwritten by 0.
        Pixels roi_expected = original[2];
        roi_expected[kW + 3] = 0;
        roi_expected[kW + 2] = 200;
        CHECK(pixels_of(*result.document, 2) == roi_expected);
    }
    SECTION("a sentence with a layer number beyond the document has no effect and is refused")
    {
        result = run_on(R"({"op": "layer_arithmetic", "sentence": "9 = 10+11"})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("no impact") != std::string::npos);
    }
}

TEST_CASE("raft_relief decimates the raft or reports that there is none", "[sla_layers][sla_layer_recipe]")
{
    const auto original = sparse_layers();
    auto result = run_on(R"({"op": "raft_relief", "type": "decimate", "mask_layer_index": 2, "dilate_iterations": 0})", make_document());
    REQUIRE(result.ok);
    // The layers below the first support layer (2) become a copy of it.
    CHECK(pixels_of(*result.document, 0) == original[2]);
    CHECK(pixels_of(*result.document, 1) == original[2]);
    CHECK(pixels_of(*result.document, 2) == original[2]);
    CHECK(pixels_of(*result.document, 3) == original[3]);

    SECTION("auto detection without supports changes nothing and is an error")
    {
        result = run_on(R"({"op": "raft_relief", "type": "decimate"})", make_document());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("no raft or support layers were found") != std::string::npos);
    }
    SECTION("tabs need a non-zero size")
    {
        CHECK(error_location(recipe_text(R"({"op": "raft_relief", "type": "tabs", "tab_triangle_base": 0})")) == "steps[0].type");
    }
    SECTION("a support layer outside the document is refused")
    {
        result = run_on(R"({"op": "raft_relief", "type": "decimate", "mask_layer_index": 9})", make_document());
        CHECK(result.error.find("outside of the layers") != std::string::npos);
    }
}

TEST_CASE("infill replaces the interior of the model by a pattern", "[sla_layers][sla_layer_recipe]")
{
    // A 10 x 10 block at (2,2) on 16 x 16, walls of 2 px, pillars of 2 px every 6 px, no floor / ceiling.
    const std::vector<Pixels> solid(4, block(16, 16, 2, 2, 10, 10));
    Pixels expected = solid[0];
    for (size_t y = 4; y <= 9; ++y)
        for (size_t x = 4; x <= 9; ++x) expected[y * 16 + x] = 0;
    for (size_t y : {7, 8})
        for (size_t x : {7, 8}) expected[y * 16 + x] = 255; // the pillar of the tile that reaches the interior
    REQUIRE(lit(expected) == 68);

    const std::string step = R"({"op": "infill", "type": "pillars", "wall_thickness": 2, "infill_thickness": 2, "infill_spacing": 6, "floor_ceil_thickness_mm": 0)";
    auto result = run_on(step + "}", make_document_from(solid, 16, 16));
    REQUIRE(result.ok);
    for (size_t i = 0; i < 4; ++i) CHECK(pixels_of(*result.document, i) == expected);
    result.document->undo();
    CHECK(pixels_of(*result.document, 0) == solid[0]); // one undo step for the whole document

    SECTION("a layer range limits the effect")
    {
        result = run_on(step + R"(, "first_layer": 1, "last_layer": 2})", make_document_from(solid, 16, 16));
        REQUIRE(result.ok);
        CHECK(pixels_of(*result.document, 0) == solid[0]);
        CHECK(pixels_of(*result.document, 1) == expected);
        CHECK(pixels_of(*result.document, 2) == expected);
        CHECK(pixels_of(*result.document, 3) == solid[3]);
    }
    SECTION("the infill brightness is written")
    {
        result = run_on(step + R"(, "infill_brightness": 100})", make_document_from(solid, 16, 16));
        REQUIRE(result.ok);
        CHECK(pixels_of(*result.document, 0)[7 * 16 + 7] == 100);
        CHECK(pixels_of(*result.document, 0)[2 * 16 + 2] == 255); // the wall keeps its value
    }
    SECTION("a range outside the document is refused")
    {
        result = run_on(step + R"(, "first_layer": 9})", make_document_from(solid, 16, 16));
        CHECK(result.error.find("first_layer 9 is outside") != std::string::npos);
    }
}

TEST_CASE("repair_layers removes islands and, on request, empty layers", "[sla_layers][sla_layer_recipe]")
{
    const Pixels body = block(16, 16, 2, 2, 6, 6);
    Pixels with_island = body;
    with_island[13 * 16 + 13] = 255; // one lit pixel with nothing below it
    const std::vector<Pixels> layers = {body, with_island, body};

    auto result = run_on(R"({"op": "repair_layers"})", make_document_from(layers, 16, 16));
    REQUIRE(result.ok);
    REQUIRE(result.document->layer_count() == 3);
    for (size_t i = 0; i < 3; ++i) CHECK(pixels_of(*result.document, i) == body);
    result.document->undo();
    CHECK(pixels_of(*result.document, 1) == with_island);

    SECTION("remove_empty_layers changes the layer list and keeps the parameters of the remaining layers")
    {
        std::vector<Pixels> with_empty = layers;
        with_empty.push_back(Pixels(16 * 16, 0));
        result = run_on(R"({"op": "repair_layers", "remove_empty_layers": true})", make_document_from(with_empty, 16, 16));
        REQUIRE(result.ok);
        REQUIRE(result.document->layer_count() == 3);
        for (size_t i = 0; i < 3; ++i) {
            CHECK(pixels_of(*result.document, i) == body);
            CHECK(value_of(*result.document, i, F::Exposure) == 10. + double(i));
        }
        result.document->undo();
        CHECK(result.document->layer_count() == 4);
    }
    SECTION("without remove_empty_layers the layer count is never touched")
    {
        std::vector<Pixels> with_empty = layers;
        with_empty.push_back(Pixels(16 * 16, 0));
        result = run_on(R"({"op": "repair_layers"})", make_document_from(with_empty, 16, 16));
        REQUIRE(result.ok);
        CHECK(result.document->layer_count() == 4);
    }
    SECTION("a range outside the document is refused")
    {
        result = run_on(R"({"op": "repair_layers", "first_layer": 7})", make_document_from(layers, 16, 16));
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("first_layer 7 is outside") != std::string::npos);
    }
}

TEST_CASE("lithophane replaces the layers by the stack built from a picture", "[sla_layers][sla_layer_recipe]")
{
    TempDir dir;
    const Pixels picture = {0, 85, 170, 255, 255, 170, 85, 0}; // 4 x 2
    const auto encoded = sla::PNGRasterEncoder{}(picture.data(), 4, 2, 1);
    {
        boost::nowide::ofstream file(dir.str("picture.png"), std::ios::binary);
        file.write(static_cast<const char *>(encoded.data()), std::streamsize(encoded.size()));
    }
    // Relative to the recipe folder (RunOptions::base_dir stands in for it here).
    const std::string step = R"({"op": "lithophane", "image": "picture.png", "bottom_layer_count": 2, "layer_height_mm": 0.05, "base_type": "none",
        "base_thickness_mm": 0, "lithophane_height_mm": 0.2, "separate_grayscale_pixels": false, "bottom_exposure_s": 30, "normal_exposure_s": 2})";
    auto result = run_on(step, make_document(), false, dir.path.string());
    REQUIRE(result.ok);
    auto &doc = *result.document;
    REQUIRE(doc.layer_count() == 4);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(value_of(doc, i, F::PositionZ) == Approx(kLayer * double(i + 1)));
        CHECK(value_of(doc, i, F::Exposure) == Approx(i < 2 ? 30. : 2.));
        CHECK(value_of(doc, i, F::LiftDistance) == 1.); // lift and wait values stay those of the document
        CHECK(value_of(doc, i, F::WaitAfter) == .5);
        CHECK(lit(pixels_of(doc, i)) > 0);
        CHECK(doc.image(i).cols == kW);
        CHECK(doc.image(i).rows == kH);
    }
    CHECK(doc.fresh_job()->globals.bottom_layers == 2);
    CHECK(pixels_of(doc, 0) != sparse_layers()[0]); // the old content is gone
    doc.undo();
    CHECK(pixels_of(doc, 0) == sparse_layers()[0]);

    SECTION("a missing picture is a step error, also in a dry run")
    {
        result = run_on(R"({"op": "lithophane", "image": "missing.png"})", make_document(), true, dir.path.string());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("does not exist") != std::string::npos);
    }
    SECTION("a file that is not a PNG is refused")
    {
        { boost::nowide::ofstream(dir.str("fake.png")) << "not a picture"; }
        result = run_on(R"({"op": "lithophane", "image": "fake.png"})", make_document(), false, dir.path.string());
        CHECK(result.error.find("not a PNG") != std::string::npos);
    }
    SECTION("the dry run accepts a good picture and writes nothing")
    {
        result = run_on(step, make_document(), true, dir.path.string());
        CHECK(result.ok);
        CHECK(result.steps[0].status == rc::StepResult::Status::Planned);
        CHECK_FALSE(result.document.has_value());
    }
    SECTION("a plain raster document has no printer geometry for it")
    {
        result = run_on(step, make_plain_document(sparse_layers(), kW, kH), false, dir.path.string());
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("sliced or generated document") != std::string::npos);
    }
}

TEST_CASE("suggestions with apply also sets the bottom layer count of a generated document", "[sla_layers][sla_layer_recipe]")
{
    const std::vector<Pixels> layers(10, block(16, 16, 2, 2, 10, 10));
    auto result = run_on(R"({"op": "suggestions", "apply": true, "only": ["bottom_layer_count"]})", make_document_from(layers, 16, 16, 1));
    REQUIRE(result.ok);
    CHECK(result.steps[0].message.find("bottom layer count 1 -> 5") != std::string::npos);
    CHECK(result.steps[0].message.find("0 layer value(s) changed") != std::string::npos);
    CHECK(result.document->fresh_job()->globals.bottom_layers == 5);
    CHECK(result.document->layer_count() == 10);
    result.document->undo();
    CHECK(result.document->fresh_job()->globals.bottom_layers == 1); // one undo step

    SECTION("together with the per-layer values")
    {
        result = run_on(R"({"op": "suggestions", "apply": true, "only": ["bottom_layer_count", "wait_time_after_cure"]})", make_document_from(layers, 16, 16, 1));
        REQUIRE(result.ok);
        CHECK(result.document->fresh_job()->globals.bottom_layers == 5);
        CHECK(result.steps[0].message.find("10 layer value(s) changed") != std::string::npos);
        // UVtools default limits: 7 s after cure on the bottom layer, 1 s on normal layers (evaluated for the old count of 1).
        CHECK(value_of(*result.document, 0, F::WaitAfter) == Approx(7.));
        for (size_t i = 1; i < 10; ++i) CHECK(value_of(*result.document, i, F::WaitAfter) == Approx(1.));
        result.document->undo(); // a single undo step restores everything
        CHECK(result.document->fresh_job()->globals.bottom_layers == 1);
        CHECK(value_of(*result.document, 0, F::WaitAfter) == Approx(.5));
    }
    SECTION("the transition layer count is reported but never written")
    {
        result = run_on(R"({"op": "suggestions", "apply": true, "only": ["transition_layer_count"]})", make_document_from(layers, 16, 16, 1));
        REQUIRE(result.ok);
        CHECK(result.document->fresh_job()->globals.transition_layers == make_job(10, 16, 16, 1).globals.transition_layers);
    }
    SECTION("without apply nothing changes")
    {
        result = run_on(R"({"op": "suggestions", "only": ["bottom_layer_count"]})", make_document_from(layers, 16, 16, 1));
        REQUIRE(result.ok);
        CHECK(result.document->fresh_job()->globals.bottom_layers == 1);
        CHECK_FALSE(result.document->can_undo());
    }
}

namespace {
// A document opened from a container file: SL1 stores no per-layer table and keeps layer count and heights in its header.
sla::LayerDocument open_sl1_document(const TempDir &dir)
{
    const Pixels pixels{0, 1, 127, 255};
    const auto mask = sla::PNGRasterEncoder{}(pixels.data(), 2, 2, 1);
    const std::string path = dir.str("input.sl1");
    {
        Zipper zip(path);
        zip.add_entry("config.ini"); zip << "layerHeight = 0.05\n";
        zip.add_entry("prusaslicer.ini"); zip << "display_width = 2\n";
        zip.add_entry("job00000.png", mask.data(), mask.size());
        zip.finalize();
    }
    return sla::LayerDocument::load_sl1(path);
}
} // namespace

TEST_CASE("Documents opened from a file refuse the operations that need a layer list or a parameter table", "[sla_layers][sla_layer_recipe]")
{
    TempDir dir;
    const std::string list_reason = "opened from a 'SL1' file";
    const std::string table_reason = "stores no per-layer print parameters";
    struct Case { std::string step, reason; };
    const std::vector<Case> refused = {
        {R"({"op": "reheight", "layer_height_mm": 0.1})", list_reason},
        {R"({"op": "double_exposure", "first_bottom_exposure_s": 1, "first_normal_exposure_s": 1})", list_reason},
        {R"({"op": "change_resolution", "new_width": 4, "new_height": 4})", list_reason},
        {R"({"op": "repair_layers", "remove_empty_layers": true})", list_reason},
        {R"({"op": "phased_exposure"})", list_reason},
        {R"({"op": "timelapse", "mode": "virtual_layer", "raise_position_z_mm": 5})", list_reason},
        {R"({"op": "dynamic_layer_height"})", list_reason},
        {R"({"op": "raise_on_print_finish", "position_z_mm": 5})", list_reason},
        {R"({"op": "lithophane", "image": "x.png"})", list_reason},
        {R"({"op": "fade_exposure", "from_exposure_s": 1, "to_exposure_s": 2, "layer_count": 1})", table_reason},
        {R"({"op": "dynamic_lifts", "smallest_lift_height": 1, "largest_lift_height": 2})", table_reason},
        {R"({"op": "timelapse", "raise_position_z_mm": 5})", table_reason},
    };
    for (const auto &c : refused) {
        INFO(c.step);
        for (const bool dry : {true, false}) {
            const auto result = run_on(c.step, open_sl1_document(dir), dry);
            CHECK_FALSE(result.ok);
            CHECK(result.failed_step == 0);
            INFO(result.error);
            CHECK(result.error.find("steps[0] (") == 0);
            CHECK(result.error.find(c.reason) != std::string::npos);
        }
    }
    SECTION("pixel operations still work and are one undo step")
    {
        const auto result = run_on(R"({"op": "layer_arithmetic", "sentence": "0 = 0+0"})", open_sl1_document(dir));
        REQUIRE(result.ok);
        CHECK(pixels_of(*result.document, 0) == Pixels({0, 2, 254, 255}));
        CHECK(result.document->can_undo());
    }
    SECTION("a plain raster document refuses the parameter operations with the reason of the document")
    {
        const auto result = run_on(R"({"op": "fade_exposure", "from_exposure_s": 1, "to_exposure_s": 2, "layer_count": 1})", make_plain_document(sparse_layers(), kW, kH));
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("stores no per-layer print parameters") != std::string::npos);
    }
}

TEST_CASE("Dry runs of the extended operations plan them and change nothing", "[sla_layers][sla_layer_recipe]")
{
    const std::string steps = R"({"op": "fade_exposure", "layer_count": 2, "from_exposure_s": 1, "to_exposure_s": 2},
        {"op": "dynamic_lifts", "smallest_lift_height": 1, "largest_lift_height": 2},
        {"op": "layer_arithmetic", "sentence": "2 = 0+1"},
        {"op": "raft_relief", "type": "dimming", "mask_layer_index": 2},
        {"op": "infill", "type": "gyroid"},
        {"op": "repair_layers"},
        {"op": "reheight", "layer_height_mm": 0.1},
        {"op": "double_exposure", "first_normal_erode": 0})";
    const auto result = run_on(steps, make_document(), true);
    REQUIRE(result.ok);
    REQUIRE(result.steps.size() == 8);
    for (const auto &s : result.steps) CHECK(s.status == rc::StepResult::Status::Planned);
    CHECK(result.steps[6].message == "checked");
    CHECK(result.steps[7].message.find("not checked") == 0); // follows a layer-list step
    CHECK_FALSE(result.document.has_value());
}
