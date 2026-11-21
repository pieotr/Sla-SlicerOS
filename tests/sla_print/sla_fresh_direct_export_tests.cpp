// SPDX-License-Identifier: AGPL-3.0-or-later
// A freshly sliced job (printer format SL1 owns its parameters) must export to EVERY writable
// destination the export dialog offers, through the same conversion route the GUI uses. Formats without
// a bespoke fresh plan run their ordinary direct writer with the job's globals applied.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <optional>
#include <iostream>
#include "sla_archive_test_config.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "libslic3r/SLA/ExportSnapshot.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/FreshLayerJob.hpp"
#include "libslic3r/PNGReadWrite.hpp"

using namespace Slic3r;

namespace {
struct TempFile {
    boost::filesystem::path path;
    explicit TempFile(const std::string &extension)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("fresh-direct-%%%%-%%%%." + extension)) {}
    ~TempFile() { boost::system::error_code error; boost::filesystem::remove(path, error); }
};

// The printer profile is SL1 (it owns fresh-job parameters); geometry and material come from the destination's
// fixture so that the destination is actually exportable.
DynamicPrintConfig sl1_profile_for(const std::string &destination)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, destination);
    full.set("sla_archive_format", std::string("SL1"));
    full.set("supports_enable", false);
    full.set("pad_enable", false);
    full.set("hollowing_enable", false);
    DynamicPrintConfig config;
    config.apply(full);
    return config;
}

std::unique_ptr<SLAPrint> slice_cube(const DynamicPrintConfig &config, Model &model)
{
    auto *object = model.add_object("cube", "", make_cube(6., 4., 1.));
    object->add_instance()->set_offset(Vec3d(50., 30., 0.));
    auto print = std::make_unique<SLAPrint>();
    print->set_status_callback([](const PrintBase::SlicingStatus &) {});
    print->apply(model, config);
    print->process();
    REQUIRE(print->finished());
    REQUIRE(print->layer_document() != nullptr);
    REQUIRE(print->layer_document()->fresh_job().has_value());
    return print;
}
} // namespace


namespace {
using F = sla::LayerParameter;
constexpr size_t layer_total = 6;

std::vector<uint8_t> binary_mask(size_t columns, size_t rows, size_t layer)
{
    std::vector<uint8_t> pixels(columns * rows, 0);
    const size_t left = columns / 8 + layer * 2, top = rows / 8 + layer;
    for (size_t y = top; y < top + rows / 4; ++y)
        for (size_t x = left; x < left + columns / 3; ++x) pixels[y * columns + x] = 255;
    return pixels;
}

// Fresh job exactly as slicing derives it from the profile, with synthetic binary masks.
sla::LayerDocument synthetic_fresh_document(const DynamicPrintConfig &config, std::vector<std::vector<uint8_t>> &masks)
{
    SLAPrinterConfig printer; printer.apply(config, true);
    SLAMaterialConfig material; material.apply(config, true);
    std::vector<double> positions;
    for (size_t i = 0; i < layer_total; ++i) positions.push_back(double(i + 1) * config.opt_float("layer_height"));
    auto job = sla::make_fresh_layer_job(printer, material, config.opt_float("layer_height"), positions,
                                         sla::FreshLayerProducer::ModelSlice);
    std::vector<sla::EncodedRaster> encoded;
    for (size_t i = 0; i < layer_total; ++i) {
        masks.push_back(binary_mask(job.geometry.columns, job.geometry.rows, i));
        encoded.push_back(sla::PNGRasterEncoder{}(masks.back().data(), job.geometry.columns, job.geometry.rows, 1));
    }
    return sla::LayerDocument::from_fresh_rasters(std::move(encoded), std::move(job));
}

size_t lit(const std::vector<uint8_t> &pixels)
{
    return size_t(std::count_if(pixels.begin(), pixels.end(), [](uint8_t v) { return v != 0; }));
}
}


namespace {
struct Outcome { std::string error; std::optional<sla::LayerDocument> reopened; bool no_reader = false; };

std::vector<char> slurp(const boost::filesystem::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
Outcome reopen(const boost::filesystem::path &path)
{
    Outcome out;
    try { out.reopened = sla::LayerDocument::load_archive(path.string()); }
    catch (const std::exception &) { out.no_reader = true; }
    return out;
}
// The only refusals a fresh SL1 job may meet: a vector-only container, and CTB v3 whose fixture settings use
// second-phase motion/waits that the format has no fields for. Both must say what to change.
bool genuine_refusal(const std::string &id, const std::string &error)
{
    if (id == "SL1SVG") return error.find("vector outlines") != std::string::npos;
    if (id == "ctb_v3") return error.find("cannot store") != std::string::npos && error.find("Set them to 0") != std::string::npos;
    return false;
}
}

TEST_CASE("A fresh job converts to every writable destination", "[sla_fresh_direct]")
{
    size_t exported = 0;
    for (const ArchiveEntry &entry : registered_sla_archives()) {
        if (!archive_export_unavailable_reason(entry.id).empty()) continue;
        const std::string id = entry.id;
        INFO("destination " << id);
        const auto config = sl1_profile_for(id);
        std::vector<std::vector<uint8_t>> masks;
        const auto document = synthetic_fresh_document(config, masks);
        TempFile file(get_default_extension(id.c_str()));
        std::string error;
        try {
            sla::NativeArchiveConversionRequest(document, config, id, file.path.string()).write();
        } catch (const std::exception &e) { error = e.what(); }
        if (std::getenv("SLA_FRESH_DIRECT_TABLE"))
            std::cout << "CONVTABLE " << id << " " << (error.empty() ? "OK" : "REFUSED: " + error) << "\n";
        if (!error.empty()) {
            INFO(error);
            CHECK(genuine_refusal(id, error));
            continue;
        }
        ++exported;
        REQUIRE(boost::filesystem::file_size(file.path) > 0);
        auto result = reopen(file.path);
        if (result.no_reader) { CHECK(id == "photons"); continue; } // binary Photon S has no reader
        REQUIRE(result.reopened);
        REQUIRE(result.reopened->layer_count() == layer_total);
        for (size_t i = 0; i < layer_total; ++i) CHECK(lit(result.reopened->image(i).buf) == lit(masks[i]));
        if (id == "cxdlp" || id == "cxdlpv2" || id == "cxdlpv1") {
            // v1/v2/v3 keep one parameter set per job. The conversion must produce exactly the file the
            // destination's own direct writer (the CLI route) makes for the same masks and profile.
            std::vector<sla::EncodedRaster> plain;
            std::vector<double> positions;
            for (size_t i = 0; i < layer_total; ++i) {
                plain.push_back(sla::PNGRasterEncoder{}(masks[i].data(), document.image(0).cols, document.image(0).rows, 1));
                positions.push_back(double(i + 1) * config.opt_float("layer_height"));
            }
            auto direct_config = config;
            direct_config.set("sla_archive_format", id);
            const auto raw = sla::LayerDocument::from_rasters(std::move(plain));
            auto direct = SLAPrint::create_raster_job(direct_config, raw, positions, std::nullopt);
            TempFile reference(get_default_extension(id.c_str()));
            direct->export_layer_document(raw, id, reference.path.string(), {}, "reference");
            const auto converted = slurp(file.path), expected = slurp(reference.path);
            REQUIRE(converted.size() == expected.size());
            // Only the Z-independent volume/time accounting may differ between a fresh and a plain document.
            size_t different = 0;
            for (size_t i = 0; i < converted.size(); ++i) different += converted[i] != expected[i];
            if (std::getenv("SLA_FRESH_DIRECT_TABLE")) std::cout << "BYTEDIFF " << id << " " << different << "\n";
            CHECK(different == 0);
        }
    }
    CHECK(exported > 70);
}

TEST_CASE("A sliced SL1-profile job exports edited layers to every writable destination like the GUI", "[sla_fresh_direct]")
{
    for (const ArchiveEntry &entry : registered_sla_archives()) {
        if (!archive_export_unavailable_reason(entry.id).empty()) continue;
        const std::string id = entry.id;
        INFO("destination " << id);
        Model model;
        const auto config = sl1_profile_for(id);
        auto print = slice_cube(config, model);
        // The layer editor edit reaches the print as a replaced document.
        auto edited = *print->layer_document();
        const size_t original_lit = lit(edited.image(0).buf);
        edited.invert(0);
        const size_t inverted_lit = lit(edited.image(0).buf);
        REQUIRE(inverted_lit > 10 * original_lit);
        print->replace_layer_document(edited);
        const auto snapshot = std::make_shared<const sla::ExportSnapshot>(print->capture_export_snapshot());
        TempFile file(get_default_extension(id.c_str()));
        std::string error;
        try {
            sla::ExportRequest(snapshot, id, file.path.string(), "fresh", /*palette_approved*/ true).write();
        } catch (const std::exception &e) { error = e.what(); }
        if (std::getenv("SLA_FRESH_DIRECT_TABLE"))
            std::cout << "GUITABLE " << id << " " << (error.empty() ? "OK" : "REFUSED: " + error) << "\n";
        if (!error.empty()) {
            INFO(error);
            CHECK(genuine_refusal(id, error));
            continue;
        }
        auto result = reopen(file.path);
        if (result.no_reader) { CHECK(id == "photons"); continue; }
        REQUIRE(result.reopened);
        REQUIRE(result.reopened->layer_count() == edited.layer_count());
        // Edited layer 0 is in the file, not the re-rasterized model slice.
        CHECK(lit(result.reopened->image(0).buf) > 10 * original_lit);
    }
}

TEST_CASE("Direct fresh export keeps the printer geometry and per-job parameter checks", "[sla_fresh_direct]")
{
    const auto config = sl1_profile_for("cxdlp");
    std::vector<std::vector<uint8_t>> masks;
    auto document = synthetic_fresh_document(config, masks);
    TempFile file("cxdlp");
    SECTION("a printer whose raster differs from the job is refused") {
        auto other = config;
        other.set("display_pixels_x", config.opt_int("display_pixels_x") + 4);
        CHECK_THROWS(sla::NativeArchiveConversionRequest(document, other, "cxdlp", file.path.string()).write());
    }
    SECTION("a per-layer parameter the format cannot store is refused by name") {
        SLAPrinterConfig printer; printer.apply(config, true);
        SLAMaterialConfig material; material.apply(config, true);
        std::vector<double> positions;
        for (size_t i = 0; i < layer_total; ++i) positions.push_back(double(i + 1) * config.opt_float("layer_height"));
        auto job = sla::make_fresh_layer_job(printer, material, config.opt_float("layer_height"), positions,
                                             sla::FreshLayerProducer::ModelSlice);
        job.rows[3][F::Exposure] = 7.75;
        std::vector<sla::EncodedRaster> encoded;
        for (size_t i = 0; i < layer_total; ++i)
            encoded.push_back(sla::PNGRasterEncoder{}(masks[i].data(), job.geometry.columns, job.geometry.rows, 1));
        const auto edited = sla::LayerDocument::from_fresh_rasters(std::move(encoded), std::move(job));
        try {
            sla::NativeArchiveConversionRequest(edited, config, "cxdlp", file.path.string()).write();
            FAIL("an exposure edit that CXDLP cannot store must not be exported silently");
        } catch (const std::exception &e) {
            const std::string message = e.what();
            CHECK_THAT(message, Catch::Matchers::ContainsSubstring("layer 4"));
            CHECK_THAT(message, Catch::Matchers::ContainsSubstring("exposure"));
            CHECK_THAT(message, Catch::Matchers::ContainsSubstring("cxdlpv4"));
        }
        // The same job is stored faithfully by formats with a per-layer table.
        for (const std::string target : {"cxdlpv4", "ctb_v4", "uvj"}) {
            INFO("per-layer destination " << target);
            TempFile table(get_default_extension(target.c_str()));
            CHECK_NOTHROW(sla::NativeArchiveConversionRequest(edited, sl1_profile_for(target), target, table.path.string()).write());
        }
    }
    SECTION("CTB v3 exports once its unsupported second-phase fields are zero") {
        auto v3 = sl1_profile_for("ctb_v3");
        for (const char *key : {"sla_lift_height2", "sla_bottom_lift_height2", "sla_retract_height2", "sla_bottom_retract_height2",
                                "sla_wait_before_cure", "sla_bottom_wait_before_cure", "sla_wait_after_cure",
                                "sla_bottom_wait_after_cure", "sla_wait_after_lift", "sla_bottom_wait_after_lift",
                                "sla_lift_speed2", "sla_bottom_lift_speed2", "sla_retract_speed2", "sla_bottom_retract_speed2"})
            v3.set(key, 0.);
        std::vector<std::vector<uint8_t>> v3masks;
        const auto v3doc = synthetic_fresh_document(v3, v3masks);
        TempFile v3file("v3.ctb");
        CHECK_NOTHROW(sla::NativeArchiveConversionRequest(v3doc, v3, "ctb_v3", v3file.path.string()).write());
    }
}
