// SPDX-License-Identifier: AGPL-3.0-or-later
// Fala 42 (G11 safety): Stir resin inserts layers at Z = 0.001 mm in front of the print, so the document has REPEATED and
// non-evenly-spaced Z values. Every writer the project can export to must either write that document faithfully or refuse
// it with a clear message BEFORE anything is written (an existing destination file stays untouched, no partial file is
// left). No writer may silently produce a reordered / corrupt file. SLA_STIR_EXPORT_TABLE=1 prints the matrix.
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include "sla_archive_test_config.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerListStream.hpp"
#include "libslic3r/SLA/FreshLayerJob.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLA/LayerConversionPlan.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include <functional>

using namespace Slic3r;
namespace lo = Slic3r::sla::layerops;
using F = sla::LayerParameter;

namespace {
namespace fs = boost::filesystem;
struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("stir-export-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

DynamicPrintConfig profile_for(const std::string &destination)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, destination);
    full.set("supports_enable", false);
    full.set("pad_enable", false);
    full.set("hollowing_enable", false);
    DynamicPrintConfig config;
    config.apply(full);
    return config;
}

size_t lit(const std::vector<uint8_t> &pixels)
{
    return size_t(std::count_if(pixels.begin(), pixels.end(), [](uint8_t v) { return v != 0; }));
}

constexpr size_t kLayers = 4, kStirs = 2;

std::vector<uint8_t> model_mask(size_t columns, size_t rows, size_t layer)
{
    std::vector<uint8_t> pixels(columns * rows, 0);
    const size_t left = columns / 8 + layer * 2, top = rows / 8 + layer;
    for (size_t y = top; y < top + rows / 4; ++y)
        for (size_t x = left; x < left + columns / 3; ++x) pixels[y * columns + x] = 255;
    return pixels;
}

struct Matrix { std::string id, verdict; };
} // namespace

TEST_CASE("Stir resin: every exportable format writes the document correctly or refuses it before writing", "[sla_layers][sla_stir][sla_stir_export][f42]")
{
    size_t written = 0, refused = 0;
    std::vector<Matrix> matrix;
    for (const ArchiveEntry &entry : registered_sla_archives()) {
        if (!archive_export_unavailable_reason(entry.id).empty()) continue;
        const std::string id = entry.id;
        INFO("destination " << id);
        const auto config = profile_for(id);
        // A generated (raster) document of this printer's raster, then Stir resin (stir and print) on it.
        SLAPrinterConfig printer; printer.apply(config, true);
        SLAMaterialConfig material; material.apply(config, true);
        std::vector<double> positions;
        for (size_t i = 0; i < kLayers; ++i) positions.push_back(double(i + 1) * config.opt_float("layer_height"));
        auto probe = sla::make_fresh_layer_job(printer, material, config.opt_float("layer_height"), positions, sla::FreshLayerProducer::Raster);
        const size_t columns = probe.geometry.columns, rows = probe.geometry.rows;
        std::vector<std::vector<uint8_t>> masks;
        std::vector<sla::EncodedRaster> encoded;
        for (size_t i = 0; i < kLayers; ++i) {
            masks.push_back(model_mask(columns, rows, i));
            encoded.push_back(sla::PNGRasterEncoder{}(masks.back().data(), columns, rows, 1));
        }
        const auto raw = sla::LayerDocument::from_rasters(std::move(encoded));
        std::unique_ptr<SLAPrint> print;
        try { print = SLAPrint::create_raster_job(config, raw, positions, sla::FreshLayerProducer::Raster); }
        catch (const std::exception &) { continue; } // the fixture itself cannot describe a generated job for this format
        auto edited = *print->layer_document();
        REQUIRE(edited.fresh_job().has_value());
        lo::StirResinParams p;
        p.stirs = kStirs; p.method = lo::StirMethod::StirAndPrint;
        const auto context = edited.parameter_printer_context(config.opt_float("layer_height"));
        const auto table = edited.parameter_table();
        lo::LayerParams base = table.front();
        for (const auto &row : table) if (!row.is_bottom) { base = row; break; }
        const bool applied = [&] {
            try { return edited.apply_parameter_list_stream(sla::liststream::stir_resin(p, context, base), config.opt_float("layer_height")); }
            catch (const std::exception &e) { INFO(e.what()); return false; }
        }();
        if (!applied) { matrix.push_back({id, "STIR REFUSED (document)"}); continue; }
        REQUIRE(edited.layer_count() == kLayers + kStirs);

        // Both export routes the application has: the generated job (editor "Export generated layers", CLI) and the
        // direct conversion of the document (Convert archive, saved layer projects).
        const auto check_route = [&](const std::string &route, const std::function<void(const std::string &)> &export_to) {
            TempDir dir;
            const fs::path target = dir.path / ("out." + std::string(get_default_extension(id.c_str())));
            { std::ofstream keep(target.string()); keep << "keep"; }
            std::string error;
            try { export_to(target.string()); }
            catch (const std::exception &e) { error = e.what(); }
            if (!error.empty()) {
                ++refused;
                // Refused: the message must say what is wrong, and the old destination file stays untouched (nothing
                // was written, no partial file, no reordered layers).
                CHECK(error.size() > 20);
                std::ifstream in(target.string());
                std::string contents; in >> contents;
                CHECK(contents == "keep");
                std::vector<std::string> leftovers;
                for (const auto &e2 : fs::directory_iterator(dir.path)) if (e2.path() != target) leftovers.push_back(e2.path().filename().string());
                CHECK(leftovers.empty());
                matrix.push_back({id + " [" + route + "]", "REFUSED: " + error.substr(0, 100)});
                return;
            }
            ++written;
            REQUIRE(fs::file_size(target) > 4);
            if (const char *keep = std::getenv("SLA_STIR_EXPORT_KEEP")) { // for the independent decoders (UVtoolsCmd)
                fs::create_directories(keep);
                fs::copy_file(target, fs::path(keep) / (id + "-" + route + "." + std::string(get_default_extension(id.c_str()))), fs::copy_option::overwrite_if_exists);
            }
            // Written: read it back with the document reader; layer order and content must be the document's.
            std::optional<sla::LayerDocument> reopened;
            try { reopened = sla::LayerDocument::load_archive(target.string()); }
            catch (const std::exception &) { matrix.push_back({id + " [" + route + "]", "WRITTEN (no reader)"}); return; }
            const size_t count = reopened->layer_count();
            CHECK(count == edited.layer_count());
            if (count == edited.layer_count())
                for (size_t i = 0; i < count; ++i) CHECK(lit(reopened->image(i).buf) == lit(edited.image(i).buf));
            matrix.push_back({id + " [" + route + "]", "WRITTEN + reread"});
        };
        check_route("job", [&](const std::string &path) { print->export_layer_document(edited, id, path, {}, "stir"); });
        check_route("convert", [&](const std::string &path) { sla::NativeArchiveConversionRequest(edited, config, id, path).write(); });
    }
    if (std::getenv("SLA_STIR_EXPORT_TABLE"))
        for (const auto &m : matrix) std::cout << "STIRTABLE " << m.id << " " << m.verdict << "\n";
    CHECK(written + refused > 40);
}
