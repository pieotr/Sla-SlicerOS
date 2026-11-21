// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "sla_print/sla_archive_test_config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <fstream>

using namespace Slic3r;
namespace {
struct TransferDirectory {
    boost::filesystem::path path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("ctb-transfer-%%%%-%%%%");
    TransferDirectory() { boost::filesystem::create_directory(path); }
    ~TransferDirectory() { boost::system::error_code ignored; boost::filesystem::remove_all(path, ignored); }
};
std::vector<uint8_t> transfer_bytes(const boost::filesystem::path &path)
{
    std::ifstream stream(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write_transfer_bytes(const boost::filesystem::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream stream(path.string(), std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    REQUIRE(stream.good());
}
sla::LayerDocument transfer_source(const boost::filesystem::path &path, DynamicPrintConfig &config, unsigned version)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "ctb_v" + std::to_string(version));
    full.set("material_density", 1.5);
    full.set("bottle_cost", 100.);
    full.set("bottle_volume", 500.);
    config.apply(full);
    std::vector<uint8_t> pixels(200 * 120, 255);
    pixels[0] = 0;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), 200, 120, 1);
    const auto masks = sla::LayerDocument::from_rasters({png, png, png});
    auto job = SLAPrint::create_raster_job(config, masks, {.05, .10, .15});
    job->export_print(path.string());
    return sla::LayerDocument::load_archive(path.string());
}
template<class Action>
bool inspect_bootstrap(const boost::filesystem::path &directory, Action &&action)
{
    for (const auto &entry : boost::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find(".sla-export-") != 0) continue;
        auto bytes = transfer_bytes(entry.path());
        if (bytes.size() < 112) continue;
        const sla::ChituboxLayerLayout layout(bytes);
        if (layout.number(layout.record(0)) != float(.001)) continue;
        action(entry.path(), bytes, layout);
        return true;
    }
    return false;
}
double literal_volume(const std::array<size_t, 3> &lit, const std::vector<double> &positions)
{
    double volume = 0, previous = 0;
    for (size_t i = 0; i < lit.size(); ++i) {
        // Integrate known physical pixel area and actual float32 native Z.
        volume += lit[i] * 100. * 60. / (200. * 120.) * (positions[i] - previous) / 1000.;
        previous = positions[i];
    }
    return volume;
}
}

TEST_CASE("CTB conversion verifies bootstrap pixels even when lit counts match", "[sla_ctb_parameter_transfer]")
{
    const unsigned version = GENERATE(3u, 4u, 5u);
    CAPTURE(version);
    TransferDirectory directory;
    DynamicPrintConfig config;
    auto source = transfer_source(directory.path / "input.ctb", config, version);
    source.invert(0);
    const auto pixels = source.image(0).buf;
    const auto revision = source.revision();
    const auto target = directory.path / "keep.ctb";
    const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
    write_transfer_bytes(target, sentinel);
    const sla::NativeArchiveConversionRequest request(source, config,
        "ctb_v" + std::to_string(version), target.string());
    bool changed = false;
    REQUIRE_THROWS_AS(request.write([&](size_t, size_t) {
        if (!changed) changed = inspect_bootstrap(directory.path,
            [&](const auto &path, auto &bytes, const auto &layout) {
                const auto before = layout.decode(0);
                bytes.at(layout.integer(layout.record(0) + 12)) ^= 1;
                const auto after = layout.decode(0);
                REQUIRE(before != after);
                REQUIRE(before.size() == after.size());
                const auto lit = [](const auto &mask) {
                    return std::count_if(mask.begin(), mask.end(), [](uint8_t value) { return value != 0; });
                };
                REQUIRE(lit(before) == lit(after));
                write_transfer_bytes(path, bytes);
            });
        return true;
    }), RuntimeError);
    REQUIRE(changed);
    CHECK((transfer_bytes(target) == sentinel));
    CHECK((source.image(0).buf == pixels));
    CHECK(source.revision() == revision);
    CHECK(source.dirty());
    CHECK(source.can_undo());
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),
        boost::filesystem::directory_iterator{}) == 2);
    const sla::NativeArchiveConversionRequest retry(source, config,
        "ctb_v" + std::to_string(version), target.string());
    REQUIRE_NOTHROW(retry.write());
    CHECK((sla::LayerDocument::load_archive(target.string()).image(0).buf == pixels));
}

TEST_CASE("CTB transfer retains literal grayscale volume and ordinary editing after restart", "[sla_ctb_parameter_transfer]")
{
    using F = sla::LayerParameter;
    const unsigned version = GENERATE(3u, 4u, 5u);
    const bool fresh = GENERATE(false, true);
    CAPTURE(version, fresh);
    TransferDirectory directory;
    DynamicPrintConfig config;
    auto source = transfer_source(directory.path / "input.ctb", config, version);
    std::array<png::ImageGreyscale, 3> expected;
    for (size_t i = 0; i < 3; ++i) {
        expected[i] = {std::vector<uint8_t>(200 * 120, i == 1 ? 255 : 0), 120, 200};
        if (i == 2) for (size_t pixel = 1; pixel < expected[i].buf.size(); pixel += 2) expected[i].buf[pixel] = 127;
        source.replace_image(i, expected[i]);
    }
    source.undo(); source.redo();
    std::vector<sla::LayerParameters> rows;
    const std::vector<double> positions{double(float(.05)), double(float(.125)), double(float(.2))};
    for (size_t i = 0; i < 3; ++i) {
        auto row = source.layer_parameters(i);
        row[F::PositionZ] = positions[i];
        row[F::Exposure] = 7.25 + i;
        rows.push_back(std::move(row));
    }
    source.replace_layer_parameter_table(rows);
    REQUIRE(source.job_parameters());
    const auto globals = *source.job_parameters();
    if (fresh) {
        sla::FreshLayerJob intent;
        intent.geometry = {200, 120, 100., 60.};
        intent.globals = globals;
        std::vector<sla::EncodedRaster> masks;
        for (size_t i = 0; i < 3; ++i) {
            auto row = intent.globals.bottom;
            for (const auto &[field, value] : rows[i]) row[field] = value;
            intent.rows.push_back(std::move(row));
            masks.push_back(sla::PNGRasterEncoder{}(expected[i].buf.data(), 200, 120, 1));
        }
        source = sla::LayerDocument::from_fresh_rasters(std::move(masks), intent);
    }
    const auto project = directory.path / "source.sla-project";
    const sla::ProjectRasterState state{source, fresh ? positions : std::vector<double>{}, "", {},
        fresh ? sla::ProjectLayerOrigin::GeneratedRaster : sla::ProjectLayerOrigin::ImportedArchive};
    sla::save_layer_project(project.string(), state);
    auto restored = sla::read_project_layers(project.string());
    REQUIRE(restored);
    auto &document = restored->document;
    document.invert(0); document.undo(); // Leave a redo entry; export must retain it.
    REQUIRE(document.can_redo());
    const auto revision = document.revision();
    const auto target = directory.path / ("output.v" + std::to_string(version) + ".ctb");
    std::vector<uint8_t> bootstrap;
    const sla::NativeArchiveConversionRequest request(document, config,
        "ctb_v" + std::to_string(version), target.string());
    REQUIRE_NOTHROW(request.write([&](size_t, size_t) {
        if (bootstrap.empty()) inspect_bootstrap(directory.path,
            [&](const auto &, const auto &bytes, const auto &) { bootstrap = bytes; });
        return true;
    }));
    REQUIRE_FALSE(bootstrap.empty());
    const auto output = transfer_bytes(target);
    const sla::ChituboxLayerLayout before(bootstrap), after(output);
    const double volume = literal_volume({0, 24000, 12000}, positions);
    CHECK(after.number(after.parameters + 20) == float(volume));
    REQUIRE(before.integer(76) > 0);
    double time = before.integer(76);
    for (size_t i = 0; i < 3; ++i) time += rows[i].at(F::Exposure) - before.number(before.record(i) + 4);
    CHECK(after.integer(76) == uint32_t(std::ceil(time)));
    if (fresh) {
        CHECK(after.number(after.parameters + 24) == float(volume * 1.5));
        CHECK(after.number(after.parameters + 28) == float(volume * 100. / 500.));
    } else {
        REQUIRE(before.number(before.parameters + 20) > 0);
        for (size_t field : {24u, 28u})
            CHECK(after.number(after.parameters + field) ==
                float(double(before.number(before.parameters + field)) * volume / before.number(before.parameters + 20)));
    }
    auto saved = sla::LayerDocument::load_archive(target.string());
    REQUIRE_NOTHROW(saved.require_job_parameters(globals));
    for (size_t i = 0; i < 3; ++i) {
        CHECK((saved.image(i).buf == expected[i].buf));
        CHECK((document.image(i).buf == expected[i].buf));
        CHECK(saved.layer_parameters(i) == rows[i]);
    }
    CHECK(document.revision() == revision);
    CHECK(document.can_redo());
    // A later ordinary native save must never reuse the conversion-only proof.
    auto black = expected[0];
    saved.replace_image(1, black);
    for (bool erased : {true, false, true}) {
        CAPTURE(erased);
        saved.save_archive(target.string());
        const auto bytes = transfer_bytes(target);
        const sla::ChituboxLayerLayout layout(bytes);
        CHECK(layout.number(layout.parameters + 20) == float(literal_volume({0, erased ? 0u : 24000u, 12000}, positions)));
        CHECK((sla::LayerDocument::load_archive(target.string()).image(1).buf == (erased ? black.buf : expected[1].buf)));
        if (erased) saved.undo(); else saved.redo();
    }
}
