// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/Format/ChituboxLayerLayout.hpp"
#include "sla_print/sla_archive_test_config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace Slic3r;
namespace {
struct BootstrapFixture {
    boost::filesystem::path path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("ctb-bootstrap-%%%%-%%%%");
    DynamicPrintConfig config;
    std::string format;
    explicit BootstrapFixture(unsigned version) : format("ctb_v" + std::to_string(version))
    {
        boost::filesystem::create_directory(path);
        SLAFullPrintConfig full;
        Test::configure_archive_fixture(full, format);
        config.apply(full);
    }
    ~BootstrapFixture()
    {
        boost::system::error_code ignored;
        boost::filesystem::remove_all(path, ignored);
    }
    sla::LayerDocument source() const
    {
        std::vector<uint8_t> pixels(200 * 120, 255);
        pixels[0] = 0;
        const auto png = sla::PNGRasterEncoder{}(pixels.data(), 200, 120, 1);
        const auto masks = sla::LayerDocument::from_rasters({png, png, png});
        auto job = SLAPrint::create_raster_job(config, masks, {.05, .10, .15});
        job->export_print((path / "input.ctb").string());
        return sla::LayerDocument::load_archive((path / "input.ctb").string());
    }
};
std::vector<uint8_t> bootstrap_bytes(const boost::filesystem::path &path)
{
    std::ifstream stream(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write_bootstrap_bytes(const boost::filesystem::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream stream(path.string(), std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    REQUIRE(stream.good());
}
}

TEST_CASE("CTB bootstrap shared masks are compared against every source layer", "[sla_ctb_bootstrap_readback]")
{
    const unsigned version = GENERATE(3u, 4u, 5u);
    CAPTURE(version);
    BootstrapFixture fixture(version);
    auto source = fixture.source();
    const auto first = source.image(0).buf;
    auto second = source.image(1);
    std::swap(second.buf[0], second.buf[1]); // Different pixels, identical lit count.
    source.replace_image(1, second);
    const auto revision = source.revision();
    const auto target = fixture.path / "keep.ctb";
    const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
    write_bootstrap_bytes(target, sentinel);
    bool changed = false;
    const sla::NativeArchiveConversionRequest request(source, fixture.config, fixture.format, target.string());
    REQUIRE_THROWS_WITH(request.write([&](size_t, size_t) {
        if (changed) return true;
        for (const auto &entry : boost::filesystem::directory_iterator(fixture.path)) {
            if (entry.path().filename().string().find(".sla-export-") != 0) continue;
            auto bytes = bootstrap_bytes(entry.path());
            if (bytes.size() < 112) continue;
            const sla::ChituboxLayerLayout layout(bytes);
            if (layout.number(layout.record(0)) != float(.001)) continue;
            REQUIRE(layout.seed != 0);
            const size_t data = layout.integer(layout.record(0) + 12);
            const size_t length = layout.integer(layout.record(0) + 16);
            std::vector<uint8_t> payload(bytes.begin() + data, bytes.begin() + data + length);
            ctb_encrypted::xor_layer(payload, 0, layout.seed);
            std::copy(payload.begin(), payload.end(), bytes.begin() + data);
            std::fill(bytes.begin() + 100, bytes.begin() + 104, 0);
            // Share the complete native definition, including its duplicated
            // parameter prefix. This is valid repeated-Z input, not an overlap
            // or damaged-RLE test. The second SOURCE mask is deliberately different.
            const std::vector<uint8_t> row(bytes.begin() + layout.record(0), bytes.begin() + layout.record(0) + 36);
            for (size_t i = 1; i < layout.count; ++i)
                std::copy(row.begin(), row.end(), bytes.begin() + layout.record(i));
            write_bootstrap_bytes(entry.path(), bytes);
            const auto shared = sla::LayerDocument::load_archive(entry.path().string());
            REQUIRE(shared.layer_count() == 3);
            for (size_t i = 0; i < 3; ++i) CHECK(shared.image(i).buf == first);
            CHECK_FALSE(shared.can_undo());
            changed = true;
            break;
        }
        return true;
    }), Catch::Matchers::ContainsSubstring("Destination grayscale precision cannot preserve"));
    REQUIRE(changed);
    CHECK(bootstrap_bytes(target) == sentinel);
    CHECK(source.revision() == revision);
    CHECK(source.image(0).buf == first);
    CHECK(source.image(1).buf == second.buf);
    CHECK(source.can_undo());
    CHECK(source.dirty());
    CHECK(std::distance(boost::filesystem::directory_iterator(fixture.path), boost::filesystem::directory_iterator{}) == 2);
    const sla::NativeArchiveConversionRequest retry(source, fixture.config, fixture.format, target.string());
    REQUIRE_NOTHROW(retry.write());
    const auto reopened = sla::LayerDocument::load_archive(target.string());
    for (size_t i = 0; i < 3; ++i) {
        CHECK(reopened.image(i).buf == source.image(i).buf);
        CHECK(reopened.layer_parameters(i) == source.layer_parameters(i));
    }
}

TEST_CASE("CTB bootstrap conversion cancellation preserves every public checkpoint", "[sla_ctb_bootstrap_readback]")
{
    const unsigned version = GENERATE(3u, 4u, 5u);
    CAPTURE(version);
    BootstrapFixture fixture(version);
    auto source = fixture.source();
    source.invert(0); source.invert(1); source.undo();
    const auto revision = source.revision();
    const auto expected = source.image(0).buf;
    const auto input = fixture.path / "input.ctb", target = fixture.path / "keep.ctb";
    const auto original = bootstrap_bytes(input);
    size_t checkpoints = 0;
    const sla::NativeArchiveConversionRequest successful(source, fixture.config, fixture.format, target.string());
    REQUIRE_NOTHROW(successful.write([&](size_t, size_t) { ++checkpoints; return true; }));
    REQUIRE(checkpoints > 0);
    for (size_t stop = 1; stop <= checkpoints; ++stop) {
        CAPTURE(stop, checkpoints);
        const std::vector<uint8_t> sentinel{'o', 'l', 'd'};
        write_bootstrap_bytes(target, sentinel);
        size_t seen = 0;
        const sla::NativeArchiveConversionRequest request(source, fixture.config, fixture.format, target.string());
        REQUIRE_THROWS_AS(request.write([&](size_t, size_t) { return ++seen != stop; }), sla::LayerOperationCancelled);
        CHECK(seen == stop);
        CHECK(request.cancelled());
        CHECK(bootstrap_bytes(target) == sentinel);
        CHECK(bootstrap_bytes(input) == original);
        CHECK(source.revision() == revision);
        CHECK(source.image(0).buf == expected);
        CHECK(source.can_undo()); CHECK(source.can_redo()); CHECK(source.dirty());
        CHECK(std::distance(boost::filesystem::directory_iterator(fixture.path), boost::filesystem::directory_iterator{}) == 2);
    }
    const sla::NativeArchiveConversionRequest retry(source, fixture.config, fixture.format, target.string());
    REQUIRE_NOTHROW(retry.write());
    const auto reopened = sla::LayerDocument::load_archive(target.string());
    for (size_t i = 0; i < 3; ++i) {
        CHECK(reopened.image(i).buf == source.image(i).buf);
        CHECK(reopened.layer_parameters(i) == source.layer_parameters(i));
    }
    std::cout << "CTB bootstrap cancellation checkpoints: version=" << version << "; count=" << checkpoints << '\n';
}
