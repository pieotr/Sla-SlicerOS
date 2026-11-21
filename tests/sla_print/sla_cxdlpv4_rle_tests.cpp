// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Format/CrealityCXDLPv4.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "sla_archive_test_config.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>

using namespace Slic3r;
namespace {
// Independent scalar serialization of maximal seven-bit runs. The boundary
// vectors below also pin each prefix width to literal wire bytes.
std::vector<uint8_t> reference_rle(const uint8_t *pixels, size_t size)
{
    std::vector<uint8_t> result;
    for (size_t begin = 0; begin < size;) {
        const uint8_t color = pixels[begin] / 2;
        size_t end = begin + 1;
        while (end < size && pixels[end] / 2 == color) ++end;
        const uint32_t count = uint32_t(end - begin);
        result.push_back(color | (count > 1 ? 128 : 0));
        if (count > 1) {
            const unsigned extra = count <= 127 ? 0 : count <= 16383 ? 1 : count <= 2097151 ? 2 : 3;
            const uint8_t prefix[] = {0, 128, 192, 224};
            result.push_back(prefix[extra] | uint8_t(count >> (extra * 8)));
            for (unsigned remaining = extra; remaining > 0; --remaining)
                result.push_back(uint8_t(count >> ((remaining - 1) * 8)));
        }
        begin = end;
    }
    return result;
}

void check_rle(const uint8_t *pixels, size_t width, size_t height)
{
    const size_t size = width * height;
    const auto encoded = CrealityCXDLPv4Archive::encode_mask(pixels, width, height, 1);
    const auto *bytes = static_cast<const uint8_t *>(encoded.data());
    CHECK(std::vector<uint8_t>(bytes, bytes + encoded.size()) == reference_rle(pixels, size));
    auto expected = std::vector<uint8_t>(pixels, pixels + size);
    for (auto &value : expected) value = value < 2 ? 0 : uint8_t(value | 1);
    CHECK(CrealityCXDLPv4Archive::decode_mask(bytes, encoded.size(), width, height) == expected);
}
}

TEST_CASE("CXDLPv4 run scanning preserves all gray values and unaligned tails", "[sla_cxdlpv4_rle]")
{
    for (unsigned color = 0; color < 256; ++color) {
        for (size_t offset = 0; offset < 8; ++offset) {
            for (size_t length : {1u, 7u, 8u, 9u, 15u, 16u, 17u, 23u, 24u, 31u, 32u, 33u}) {
                CAPTURE(color, offset, length);
                std::vector<uint8_t> pixels(offset + length, uint8_t(color));
                // Adjacent low-bit variants must remain one encoded run.
                for (size_t i = offset + 1; i < pixels.size(); i += 2) pixels[i] ^= 1;
                const auto before = pixels;
                check_rle(pixels.data() + offset, length, 1);
                CHECK(pixels == before);
            }
        }
    }
}

TEST_CASE("CXDLPv4 run prefixes and transitions have exact wire bytes", "[sla_cxdlpv4_rle]")
{
    const std::vector<std::pair<size_t, std::vector<uint8_t>>> cases = {
        {1, {0x7f}}, {2, {0xff, 2}}, {127, {0xff, 0x7f}},
        {128, {0xff, 0x80, 0x80}}, {16383, {0xff, 0xbf, 0xff}},
        {16384, {0xff, 0xc0, 0x40, 0}}, {2097151, {0xff, 0xdf, 0xff, 0xff}},
        {2097152, {0xff, 0xe0, 0x20, 0, 0}}
    };
    // Prefix boundaries above 32768 pixels span rows; RLE does not reset at
    // a row boundary. Pad with black and verify the complete byte sequence.
    for (const auto &[length, prefix] : cases) {
        const size_t width = std::min(size_t(32768), length);
        const size_t height = (length + width - 1) / width;
        std::vector<uint8_t> pixels(width * height, 0);
        std::fill_n(pixels.begin(), length, 255);
        const auto encoded = CrealityCXDLPv4Archive::encode_mask(pixels.data(), width, height, 1);
        const auto *bytes = static_cast<const uint8_t *>(encoded.data());
        REQUIRE(encoded.size() >= prefix.size());
        CHECK(std::equal(prefix.begin(), prefix.end(), bytes));
        check_rle(pixels.data(), width, height);
    }
    std::mt19937 random(1729);
    for (size_t count : {1u, 15u, 16u, 17u, 129u, 8192u}) {
        std::vector<uint8_t> pixels(count);
        for (auto &pixel : pixels) pixel = uint8_t(random());
        check_rle(pixels.data(), count, 1);
        for (size_t i = 0; i < count; ++i) pixels[i] = uint8_t((i % 2) * 255);
        check_rle(pixels.data(), count, 1);
    }
}

TEST_CASE("CXDLPv4 codec rejects invalid raster sizes before reading", "[sla_cxdlpv4_rle]")
{
    const uint8_t pixel = 0;
    CHECK_THROWS(CrealityCXDLPv4Archive::encode_mask(nullptr, 1, 1, 1));
    CHECK_THROWS(CrealityCXDLPv4Archive::encode_mask(&pixel, 0, 1, 1));
    CHECK_THROWS(CrealityCXDLPv4Archive::encode_mask(&pixel, 1, 0, 1));
    CHECK_THROWS(CrealityCXDLPv4Archive::encode_mask(&pixel, 1, 1, 3));
    CHECK_THROWS(CrealityCXDLPv4Archive::encode_mask(&pixel, 32769, 1, 1));
    CHECK_THROWS(CrealityCXDLPv4Archive::encode_mask(&pixel, 1, 32769, 1));
    CHECK_THROWS(CrealityCXDLPv4Archive::encode_mask(&pixel, 32768, 32768, 1));
}

// Explicit developer benchmark, excluded from normal CTest. It measures the
// whole public fresh export separately from source construction and readback.
// Inputs are authored rasters, not a copy of the user's model.
TEST_CASE("Measure CXDLPv4 fresh export of long and short raster runs", "[.][sla_cxdlpv4_rle_benchmark]")
{
    const char *destination = std::getenv("SLA_RLE_BENCH_OUTPUT");
    const char *scenario = std::getenv("SLA_RLE_BENCH_CASE");
    REQUIRE(destination); REQUIRE(scenario);
    const std::string mode(scenario);
    REQUIRE((mode == "large" || mode == "small" || mode == "noisy"));
    const size_t width = mode == "large" ? 4096 : 200;
    const size_t height = mode == "large" ? 2560 : 120;
    const size_t count = mode == "large" ? 24 : 3;
    const boost::filesystem::path folder(destination);
    boost::filesystem::create_directories(folder);
    REQUIRE_FALSE(boost::filesystem::exists(folder / "report.json"));
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "cxdlpv4");
    full.set("display_pixels_x", int(width)); full.set("display_pixels_y", int(height));
    DynamicPrintConfig config; config.apply(full);
    std::vector<sla::EncodedRaster> masks;
    std::vector<double> positions;
    std::vector<uint8_t> pixels(width * height);
    std::mt19937 random(1729);
    for (size_t layer = 0; layer < count; ++layer) {
        std::fill(pixels.begin(), pixels.end(), 0);
        for (size_t y = height / 5; y < height * 4 / 5; ++y)
            for (size_t x = width / 4 + layer; x < width * 3 / 4; ++x)
                pixels[y * width + x] = (y % 17 == 0) ? 127 : 255;
        if (mode == "noisy")
            for (auto &pixel : pixels) {
                const auto value = uint8_t(random() | 1);
                pixel = value == 1 ? 0 : value; // Exact native palette; 1 maps to 0.
            }
        masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), width, height, 1));
        positions.push_back((layer + 1) * .05);
    }
    auto document = sla::LayerDocument::from_rasters(std::move(masks));
    auto job = SLAPrint::create_raster_job(config, document, positions);
    const auto started = std::chrono::steady_clock::now();
    job->export_print((folder / "output.cxdlpv4").string());
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto saved = sla::LayerDocument::load_archive((folder / "output.cxdlpv4").string());
    REQUIRE(saved.layer_count() == count);
    for (size_t layer = 0; layer < count; ++layer) CHECK(saved.image(layer).buf == document.image(layer).buf);
    std::ofstream report((folder / "report.json").string());
    report << nlohmann::json{{"case", mode}, {"width", width}, {"height", height},
        {"layers", count}, {"export_seconds", elapsed}}.dump(2) << '\n';
    REQUIRE(report.good());
}
