// SPDX-License-Identifier: AGPL-3.0-or-later
// GOO writer regression guards. The decoder below is written independently from
// UVtools GooFile.cs LayerDef.DecodeImage so it does not share code with the encoder.
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <utility>
#include <vector>

#include "sla_archive_test_config.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Thumbnails/ThumbnailData.hpp"

// Defined in UVToolsCompatArchive.cpp without a public header.
namespace Slic3r {
std::vector<uint8_t> encode_goo_rle(const uint8_t *src, size_t pixel_count);
void goo_append_run(std::vector<uint8_t> &rle, uint8_t current, uint8_t previous, uint64_t run);
}

using namespace Slic3r;

namespace {

using Run = std::pair<uint8_t, uint64_t>; // colour, pixel count

// Decodes chunk bytes (without the magic and checksum) into runs.
std::vector<Run> decode_goo_runs(const uint8_t *data, size_t size)
{
    std::vector<Run> runs;
    uint8_t color = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b0 = data[i];
        const unsigned type = b0 >> 6;
        uint64_t stride = 0;
        size_t next = i + 1;
        auto need = [&](size_t count) { REQUIRE(next + count <= size); };
        if (type == 2) {
            const unsigned kind = (b0 >> 4) & 3;
            const uint8_t diff = b0 & 0x0F;
            color = uint8_t(kind < 2 ? color + diff : color - diff);
            if (kind & 1) { need(1); stride = data[next++]; }
            else stride = 1;
        } else {
            if (type == 0) color = 0;
            else if (type == 3) color = 255;
            else { need(1); color = data[next++]; }
            const unsigned extra = (b0 >> 4) & 3;
            need(extra);
            stride = b0 & 0x0F;
            // Extra bytes are the high bits, most significant first.
            uint64_t high = 0;
            for (unsigned k = 0; k < extra; ++k) high = (high << 8) | data[next++];
            stride += high << 4;
        }
        runs.emplace_back(color, stride);
        i = next - 1;
    }
    return runs;
}

std::vector<uint8_t> decode_goo_image(const std::vector<uint8_t> &rle, size_t pixel_count)
{
    REQUIRE(rle.size() >= 3);
    REQUIRE(rle.front() == 0x55);
    uint8_t sum = 0;
    for (size_t i = 1; i + 1 < rle.size(); ++i) sum += rle[i];
    REQUIRE(rle.back() == uint8_t(~sum));
    std::vector<uint8_t> image;
    image.reserve(pixel_count);
    for (const auto &[color, count] : decode_goo_runs(rle.data() + 1, rle.size() - 2)) {
        REQUIRE(image.size() + count <= pixel_count);
        image.insert(image.end(), size_t(count), color);
    }
    return image;
}

std::vector<uint8_t> circle_mask(size_t w, size_t h, double radius, bool antialiased)
{
    std::vector<uint8_t> pixels(w * h, 0);
    const double cx = w / 2., cy = h / 2.;
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            const double d = std::hypot(x + .5 - cx, y + .5 - cy);
            const double coverage = antialiased ? std::clamp(radius - d + .5, 0., 1.) : (d <= radius ? 1. : 0.);
            pixels[y * w + x] = uint8_t(std::lround(coverage * 255.));
        }
    return pixels;
}

} // namespace

TEST_CASE("GOO RLE of a mostly empty full-resolution circle round-trips and stays tiny", "[sla_goo][sla_reference]")
{
    // The reported TheCircle.goo (3840x2400, 141 layers) grew to 2.6 GB, i.e. about one chunk per pixel.
    const size_t w = 3840, h = 2400;
    for (const bool antialiased : {false, true}) {
        CAPTURE(antialiased);
        const auto pixels = circle_mask(w, h, 1000., antialiased);
        const auto rle = encode_goo_rle(pixels.data(), pixels.size());
        CHECK(rle.size() < w * h / 100);
        CHECK(decode_goo_image(rle, pixels.size()) == pixels);
    }
}

TEST_CASE("GOO RLE round-trips noise and every run length tier", "[sla_goo][sla_reference]")
{
    std::mt19937 rng(12345);
    {
        std::vector<uint8_t> noise(640 * 480);
        for (auto &v : noise) v = uint8_t(rng());
        CHECK(decode_goo_image(encode_goo_rle(noise.data(), noise.size()), noise.size()) == noise);
    }
    {
        // Slowly varying noise exercises the small-difference chunks.
        std::vector<uint8_t> walk(100000);
        int value = 128;
        for (auto &v : walk) { value = std::clamp(value + int(rng() % 31) - 15, 0, 255); v = uint8_t(value); }
        CHECK(decode_goo_image(encode_goo_rle(walk.data(), walk.size()), walk.size()) == walk);
    }
    {
        // Run lengths around the 4, 12 and 20 bit boundaries, with all kinds of colours.
        std::vector<uint8_t> pixels;
        const std::vector<size_t> lengths{1, 2, 15, 16, 17, 255, 256, 4095, 4096, 4097, 65535, 65536, 1048575, 1048576, 1048577};
        const std::vector<uint8_t> colors{0, 255, 7, 8, 128, 130, 254, 1, 0, 255, 0, 40, 41, 255, 0};
        for (size_t i = 0; i < lengths.size(); ++i) pixels.insert(pixels.end(), lengths[i], colors[i]);
        CHECK(decode_goo_image(encode_goo_rle(pixels.data(), pixels.size()), pixels.size()) == pixels);
    }
    {
        // An all-black and an all-white raster each need a single chunk.
        for (const uint8_t color : {uint8_t(0), uint8_t(255)}) {
            std::vector<uint8_t> pixels(3840 * 2400, color);
            const auto rle = encode_goo_rle(pixels.data(), pixels.size());
            CHECK(rle.size() < 16);
            CHECK(decode_goo_image(rle, pixels.size()) == pixels);
        }
    }
}

TEST_CASE("GOO runs over 28 bits are split into valid chunks", "[sla_goo][sla_reference]")
{
    const uint64_t limit = 0xFFFFFFFull;
    for (const uint8_t color : {uint8_t(0), uint8_t(255), uint8_t(128), uint8_t(3)})
        for (const uint64_t run : {limit - 1, limit, limit + 1, 2 * limit + 5, uint64_t(1) << 32}) {
            CAPTURE(int(color), run);
            std::vector<uint8_t> chunks;
            goo_append_run(chunks, color, uint8_t(color == 3 ? 10 : 0), run);
            uint64_t total = 0;
            for (const auto &[decoded_color, count] : decode_goo_runs(chunks.data(), chunks.size())) {
                CHECK(decoded_color == color);
                CHECK(count >= 1);
                CHECK(count <= limit);
                total += count;
            }
            CHECK(total == run);
        }
}

namespace {

struct TempFile {
    boost::filesystem::path path = boost::filesystem::unique_path("goo-test-%%%%.goo");
    ~TempFile() { boost::system::error_code error; boost::filesystem::remove(path, error); }
};

uint32_t be32(const std::vector<uint8_t> &b, size_t at) { return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3]; }
float be_float(const std::vector<uint8_t> &b, size_t at) { const uint32_t v = be32(b, at); float f; std::memcpy(&f, &v, 4); return f; }

std::vector<uint8_t> export_goo(bool mirror_x, bool mirror_y, double machine_z, const ThumbnailsList &thumbnails)
{
    Model model;
    auto *object = model.add_object("goo", "", make_cube(4., 3., .15));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, "goo");
    full.set("supports_enable", false); full.set("pad_enable", false);
    full.set("support_object_elevation", 0.);
    full.set("display_mirror_x", mirror_x); full.set("display_mirror_y", mirror_y);
    full.set("max_print_height", machine_z);
    DynamicPrintConfig config; config.apply(full);
    SLAPrint print;
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, config);
    print.process();
    REQUIRE(print.finished());
    TempFile file;
    print.export_print(file.path.string(), thumbnails, "goo");
    std::ifstream input(file.path.string(), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool all_zero(const std::vector<uint8_t> &b, size_t begin, size_t size)
{
    for (size_t i = begin; i < begin + size; ++i) if (b[i]) return false;
    return true;
}

} // namespace

TEST_CASE("GOO header carries previews, mirroring, machine Z and a UTC timestamp", "[sla_goo][sla_reference]")
{
    // Fixed header offsets from GooFile.cs.
    constexpr size_t time_at = 68, small_at = 194, small_size = 116 * 116 * 2,
                     big_at = small_at + small_size + 2, big_size = 290 * 290 * 2,
                     layers_at = big_at + big_size + 2, mirror_x_at = layers_at + 8, machine_z_at = mirror_x_at + 2 + 8;

    ThumbnailData red;
    red.set(64, 32);
    for (size_t i = 0; i < red.pixels.size(); i += 4) { red.pixels[i] = 255; red.pixels[i + 1] = 0; red.pixels[i + 2] = 0; red.pixels[i + 3] = 255; }

    const auto with_preview = export_goo(true, false, 123., {red});
    REQUIRE(with_preview.size() > machine_z_at + 4);
    CHECK_FALSE(all_zero(with_preview, small_at, small_size));
    CHECK_FALSE(all_zero(with_preview, big_at, big_size));
    // Centre pixel is opaque red in big-endian RGB565.
    const size_t centre = big_at + 2 * (145 * 290 + 145);
    CHECK(with_preview[centre] == 0xF8);
    CHECK(with_preview[centre + 1] == 0x00);
    CHECK(with_preview[small_at + small_size] == 0x0D);
    CHECK(with_preview[big_at + big_size + 1] == 0x0A);

    CHECK(with_preview[mirror_x_at] == 1);
    CHECK(with_preview[mirror_x_at + 1] == 0);
    CHECK(be_float(with_preview, machine_z_at) == 123.f);

    // PausePositionZ is the second field of the first LayerDef. LayerDefAddress is the
    // header field that ends 7 bytes before that LayerDef (address, gray level, transition
    // layers), so find the u32 equal to its own offset + 7 after the machine Z field.
    bool found_pause_z = false;
    for (size_t at = machine_z_at; at + 7 < with_preview.size() && at < machine_z_at + 400; ++at) {
        const uint32_t address = be32(with_preview, at);
        if (address == at + 7 && address + 6 <= with_preview.size()) {
            found_pause_z = be_float(with_preview, address + 2) == 123.f;
            break;
        }
    }
    CHECK(found_pause_z);

    // Timestamp: "YYYY-MM-DD HH:MM:SS" in UTC, within a day of now.
    const std::string stamp(reinterpret_cast<const char *>(&with_preview[time_at]), 19);
    std::tm parsed{};
    REQUIRE(strptime(stamp.c_str(), "%Y-%m-%d %H:%M:%S", &parsed) != nullptr);
    const std::time_t written = timegm(&parsed);
    CHECK(std::abs(double(written) - double(std::time(nullptr))) < 24. * 3600.);

    const auto without_preview = export_goo(false, true, 90., {});
    CHECK(all_zero(without_preview, small_at, small_size));
    CHECK(all_zero(without_preview, big_at, big_size));
    CHECK(without_preview[mirror_x_at] == 0);
    CHECK(without_preview[mirror_x_at + 1] == 1);
    CHECK(be_float(without_preview, machine_z_at) == 90.f);
}
