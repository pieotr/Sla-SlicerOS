// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/LayerMemoryBudget.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "sla_print/sla_archive_test_config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstring>

using namespace Slic3r;
namespace {
struct ReadbackDirectory {
    boost::filesystem::path path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("ctb-readback-%%%%-%%%%");
    ReadbackDirectory() { boost::filesystem::create_directory(path); }
    ~ReadbackDirectory() { boost::system::error_code ignored; boost::filesystem::remove_all(path, ignored); }
};
std::vector<uint8_t> read_bytes(const boost::filesystem::path &path)
{
    std::ifstream stream(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write_bytes(const boost::filesystem::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream stream(path.string(), std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    REQUIRE(stream.good());
}
void put_u32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) bytes.at(at + i) = uint8_t(value >> (8 * i));
}
void put_float(std::vector<uint8_t> &bytes, size_t at, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(bytes, at, bits);
}
sla::LayerDocument make_source(const boost::filesystem::path &path,
    DynamicPrintConfig &config, unsigned version)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "ctb_v" + std::to_string(version));
    config.apply(full);
    std::vector<sla::EncodedRaster> rasters;
    for (size_t layer = 0; layer < 3; ++layer) {
        std::vector<uint8_t> pixels(200 * 120, 255);
        pixels[layer] = 0;
        rasters.push_back(sla::PNGRasterEncoder{}(pixels.data(), 200, 120, 1));
    }
    auto masks = sla::LayerDocument::from_rasters(std::move(rasters));
    auto job = SLAPrint::create_raster_job(config, masks, {.05, .10, .15});
    job->export_print(path.string());
    return sla::LayerDocument::load_archive(path.string());
}
// The outer publication file contains bootstrap Z=.001 until parameter
// transfer atomically replaces it. Observe that transition using the real
// caller checkpoint; never depend on an arbitrary callback count or filename.
template<class Action>
bool on_final_container(const boost::filesystem::path &directory, Action &&action)
{
    for (const auto &entry : boost::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find(".sla-export-") != 0) continue;
        auto bytes = read_bytes(entry.path());
        if (bytes.size() < 112) continue;
        const sla::ChituboxLayerLayout layout(bytes);
        if (layout.number(layout.record(0)) != float(.05)) continue;
        action(entry.path(), bytes, layout);
        return true;
    }
    return false;
}
}

TEST_CASE("CTB conversion rejects a valid but changed final mask", "[sla_ctb_conversion_readback]")
{
    const unsigned version = GENERATE(3u, 4u, 5u);
    CAPTURE(version);
    ReadbackDirectory directory;
    DynamicPrintConfig config;
    auto source = make_source(directory.path / "input.ctb", config, version);
    source.invert(0);
    const auto expected = source.image(0).buf;
    const auto revision = source.revision();
    const auto target = directory.path / "keep.ctb";
    const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
    write_bytes(target, sentinel);
    const sla::NativeArchiveConversionRequest request(source, config,
        "ctb_v" + std::to_string(version), target.string());
    bool changed = false;
    CHECK_THROWS_AS(request.write([&](size_t, size_t) {
        if (!changed) changed = on_final_container(directory.path,
            [&](const auto &path, auto &bytes, const auto &layout) {
                const auto before = layout.decode(0);
                // XOR encryption preserves this low grayscale bit; run flags,
                // lengths, table pointers and file size remain unchanged.
                bytes.at(layout.integer(layout.record(0) + 12)) ^= 1;
                const auto after = layout.decode(0);
                REQUIRE(before != after);
                REQUIRE(before.size() == after.size());
                write_bytes(path, bytes);
            });
        return true;
    }), RuntimeError);
    REQUIRE(changed);
    CHECK(read_bytes(target) == sentinel);
    CHECK(source.image(0).buf == expected);
    CHECK(source.revision() == revision);
    CHECK(source.dirty());
    CHECK(source.can_undo());
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),
        boost::filesystem::directory_iterator{}) == 2);

    const sla::NativeArchiveConversionRequest retry(source, config,
        "ctb_v" + std::to_string(version), target.string());
    REQUIRE_NOTHROW(retry.write());
    const auto saved = sla::LayerDocument::load_archive(target.string());
    for (size_t i = 0; i < source.layer_count(); ++i) {
        CHECK(saved.image(i).buf == source.image(i).buf);
        CHECK(saved.layer_parameters(i) == source.layer_parameters(i));
    }
    REQUIRE(source.job_parameters());
    REQUIRE_NOTHROW(saved.require_job_parameters(*source.job_parameters()));
}

TEST_CASE("CTB conversion rejects damaged final geometry metadata and addressing", "[sla_ctb_conversion_readback]")
{
    const unsigned version = GENERATE(3u, 4u, 5u);
    const auto mutation = GENERATE("seed", "shape", "physical", "count", "pointer", "length",
        "truncated", "budget", "row", "duplicate-row", "global", "duplicate-global", "mode", "version");
    CAPTURE(version, mutation);
    // The mutation "budget" pads the file past 1 GiB; the real budget follows free RAM, so pin it.
    sla::ScopedLayerBudgets pinned({.editor_bytes = uint64_t(1) << 30});
    ReadbackDirectory directory;
    DynamicPrintConfig config;
    auto source = make_source(directory.path / "input.ctb", config, version);
    source.invert(0);
    const auto pixels = source.image(0).buf;
    const auto revision = source.revision();
    const auto target = directory.path / "keep.ctb";
    const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
    write_bytes(target, sentinel);
    const sla::NativeArchiveConversionRequest request(source, config,
        "ctb_v" + std::to_string(version), target.string());
    bool changed = false;
    CHECK_THROWS_AS(request.write([&](size_t, size_t) {
        if (!changed) changed = on_final_container(directory.path,
            [&](const auto &path, auto &bytes, const auto &layout) {
                const std::string kind(mutation);
                const auto row = layout.record(0), extended = layout.extended_record(0);
                const auto slicer = layout.integer(104), params = layout.parameters;
                if (kind == "seed") put_u32(bytes, 100, layout.seed ^ 1);
                if (kind == "shape") { put_u32(bytes, 52, layout.rows); put_u32(bytes, 56, layout.columns); }
                if (kind == "physical") put_float(bytes, 8, layout.number(8) + 1);
                if (kind == "count") put_u32(bytes, 68, layout.count + 1);
                if (kind == "pointer") put_u32(bytes, row + 12, uint32_t(bytes.size()) + 1);
                if (kind == "length") put_u32(bytes, row + 16, layout.integer(row + 16) + 1);
                if (kind == "truncated") bytes.resize(80);
                if (kind == "row") {
                    put_float(bytes, row + 4, 123);
                    put_float(bytes, extended + 4, 123);
                }
                if (kind == "duplicate-row") put_float(bytes, extended + 4, 123);
                if (kind == "global") put_float(bytes, 36, 123);
                if (kind == "duplicate-global") put_u32(bytes, params + 40, layout.integer(48) + 1);
                if (kind == "mode") bytes.at(slicer + 39) = 0xff;
                if (kind == "version") put_u32(bytes, 4, version == 5 ? 4 : version + 1);
                write_bytes(path, bytes);
                if (kind == "budget") boost::filesystem::resize_file(path, (uint64_t(1) << 30) + 1);
            });
        return true;
    }), RuntimeError);
    REQUIRE(changed);
    CHECK((read_bytes(target) == sentinel));
    CHECK((source.image(0).buf == pixels));
    CHECK(source.revision() == revision);
    CHECK(source.dirty());
    CHECK(source.can_undo());
    CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),
        boost::filesystem::directory_iterator{}) == 2);
}

TEST_CASE("CTB final verification cancellation preserves output and permits retry", "[sla_ctb_conversion_readback]")
{
    const unsigned version = GENERATE(3u, 4u, 5u);
    CAPTURE(version);
    ReadbackDirectory directory;
    DynamicPrintConfig config;
    auto source = make_source(directory.path / "input.ctb", config, version);
    source.invert(0);
    const auto pixels = source.image(0).buf;
    const auto revision = source.revision();
    const auto target = directory.path / "keep.ctb";
    const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
    // Count actual final-stage checkpoints on a successful control run. This
    // covers the readback beginning, its middle and the publication boundary,
    // without coupling the contract to a particular decoder implementation.
    size_t checkpoints = 0;
    const sla::NativeArchiveConversionRequest control(source, config,
        "ctb_v" + std::to_string(version), target.string());
    control.write([&](size_t, size_t) {
        on_final_container(directory.path, [&](const auto &, auto &, const auto &) { ++checkpoints; });
        return true;
    });
    REQUIRE(checkpoints >= 3);
    for (const auto stop : {size_t(1), checkpoints / 2, checkpoints}) {
        CAPTURE(stop, checkpoints);
        write_bytes(target, sentinel);
        size_t reached = 0;
        const sla::NativeArchiveConversionRequest request(source, config,
            "ctb_v" + std::to_string(version), target.string());
        CHECK_THROWS_AS(request.write([&](size_t, size_t) {
            on_final_container(directory.path, [&](const auto &, auto &, const auto &) {
                if (++reached == stop) request.cancel();
            });
            return true;
        }), CanceledException);
        CHECK(reached == stop);
        CHECK((read_bytes(target) == sentinel));
        CHECK(source.revision() == revision);
        CHECK((source.image(0).buf == pixels));
        CHECK(source.dirty());
        CHECK(source.can_undo());
        CHECK(std::distance(boost::filesystem::directory_iterator(directory.path),
            boost::filesystem::directory_iterator{}) == 2);
        const sla::NativeArchiveConversionRequest retry(source, config,
            "ctb_v" + std::to_string(version), target.string());
        REQUIRE_NOTHROW(retry.write());
        CHECK((sla::LayerDocument::load_archive(target.string()).image(0).buf == pixels));
    }
}

TEST_CASE("CTB native readback preserves imported and fresh job intent", "[sla_ctb_conversion_readback]")
{
    const unsigned version = GENERATE(3u, 4u, 5u);
    const auto origin = GENERATE("imported", "zero-seed", "fresh");
    CAPTURE(version, origin);
    ReadbackDirectory directory;
    DynamicPrintConfig config;
    const auto input = directory.path / "input.ctb";
    auto source = make_source(input, config, version);
    if (std::string(origin) == "zero-seed") {
        auto bytes = read_bytes(input);
        const sla::ChituboxLayerLayout layout(bytes);
        REQUIRE(layout.seed != 0);
        for (size_t i = 0; i < layout.count; ++i) {
            const auto record = layout.record(i), data = size_t(layout.integer(record + 12));
            const auto size = layout.integer(record + 16);
            std::vector<uint8_t> payload(bytes.begin() + data, bytes.begin() + data + size);
            ctb_encrypted::xor_layer(payload, uint32_t(i), layout.seed);
            std::copy(payload.begin(), payload.end(), bytes.begin() + data);
        }
        put_u32(bytes, 100, 0);
        write_bytes(input, bytes);
        source = sla::LayerDocument::load_archive(input.string());
    } else if (std::string(origin) == "fresh") {
        sla::FreshLayerJob intent;
        intent.geometry = {200, 120, 100., 60.};
        REQUIRE(source.job_parameters());
        intent.globals = *source.job_parameters();
        std::vector<sla::EncodedRaster> masks;
        for (size_t i = 0; i < source.layer_count(); ++i) {
            auto row = i < intent.globals.bottom_layers ? intent.globals.bottom : intent.globals.normal;
            for (const auto &[field, value] : source.layer_parameters(i)) row[field] = value;
            intent.rows.push_back(std::move(row));
            const auto image = source.image(i);
            masks.push_back(sla::PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1));
        }
        source = sla::LayerDocument::from_fresh_rasters(std::move(masks), intent);
    }
    source.invert(0);
    const auto revision = source.revision();
    const auto target = directory.path / "output.ctb";
    const sla::NativeArchiveConversionRequest request(source, config,
        "ctb_v" + std::to_string(version), target.string());
    REQUIRE_NOTHROW(request.write());
    const auto saved = sla::LayerDocument::load_archive(target.string());
    REQUIRE(saved.layer_count() == 3);
    CHECK(saved.archive_format() == "ctb_v" + std::to_string(version));
    for (size_t i = 0; i < 3; ++i) {
        const auto mask = saved.image(i);
        std::vector<uint8_t> expected(200 * 120, i == 0 ? 0 : 255);
        expected[i] = i == 0 ? 255 : 0;
        CHECK(mask.cols == 200);
        CHECK(mask.rows == 120);
        CHECK((mask.buf == expected));
        for (const auto &[field, value] : saved.layer_parameters(i))
            CHECK(source.layer_parameters(i).at(field) == value);
    }
    const auto globals = source.fresh_job() ?
        std::optional<sla::LayerJobParameters>(source.fresh_job()->globals) : source.job_parameters();
    REQUIRE(globals);
    REQUIRE_NOTHROW(saved.require_job_parameters(*globals));
    CHECK(source.revision() == revision);
    CHECK(source.dirty());
    CHECK(source.can_undo());
}

TEST_CASE("Fresh CTB v3 rejects unsupported motion and waits before staging", "[sla_ctb_conversion_readback]")
{
    using F = sla::LayerParameter;
    const auto field = GENERATE(F::LiftDistance2, F::LiftSpeed2, F::RetractDistance2,
        F::RetractSpeed2, F::WaitBefore, F::WaitAfter, F::WaitAfterLift);
    const bool global = GENERATE(false, true);
    CAPTURE(int(field), global);
    ReadbackDirectory directory;
    DynamicPrintConfig config;
    const auto imported = make_source(directory.path / "input.ctb", config, 3);
    sla::FreshLayerJob intent;
    intent.geometry = {200, 120, 100., 60.};
    REQUIRE(imported.job_parameters());
    intent.globals = *imported.job_parameters();
    std::vector<sla::EncodedRaster> masks;
    for (size_t i = 0; i < 3; ++i) {
        auto row = intent.globals.bottom;
        row[F::PositionZ] = imported.layer_parameters(i).at(F::PositionZ);
        intent.rows.push_back(std::move(row));
        const auto image = imported.image(i);
        masks.push_back(sla::PNGRasterEncoder{}(image.buf.data(), image.cols, image.rows, 1));
    }
    const auto change = [&](sla::LayerParameters &row) {
        row[field] = .5;
        if (field == F::LiftDistance2) row[F::LiftSpeed2] = 60;
        if (field == F::RetractDistance2) row[F::RetractSpeed2] = 60;
    };
    if (global) { change(intent.globals.normal); change(intent.globals.bottom); }
    else change(intent.rows.at(1));
    auto source = sla::LayerDocument::from_fresh_rasters(std::move(masks), intent);
    source.invert(0);
    const auto revision = source.revision();
    const auto target = directory.path / "keep.ctb";
    const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
    write_bytes(target, sentinel);
    const sla::NativeArchiveConversionRequest request(source, config, "ctb_v3", target.string());
    bool staged = false;
    REQUIRE_THROWS_AS(request.write([&](size_t, size_t) {
        for (const auto &entry : boost::filesystem::directory_iterator(directory.path))
            if (entry.path().filename().string().find(".sla-export-") == 0) staged = true;
        return true;
    }), RuntimeError);
    CHECK_FALSE(staged);
    CHECK((read_bytes(target) == sentinel));
    CHECK(source.revision() == revision);
    CHECK(source.dirty());
    CHECK(source.can_undo());
    CHECK(source.fresh_job()->rows == intent.rows);
}
