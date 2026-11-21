// SPDX-License-Identifier: AGPL-3.0-or-later
// GOO/PRZ reopen, edit and save-back through LayerDocument. Files are built here
// from the documented GooFile.cs layout with the shared writer RLE encoder, so
// the reader is checked against bytes it did not produce.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#include "libslic3r/SLA/LayerDocument.hpp"

namespace Slic3r {
std::vector<uint8_t> encode_goo_rle(const uint8_t *src, size_t pixel_count);
}

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr size_t header_size = 195477, def_size = 70, footer_size = 11;
constexpr size_t layer_count_at = 195310, volume_at = 195450, address_at = 195470;
constexpr size_t columns = 64, rows = 48, layer_total = 5;

struct TempFile {
    boost::filesystem::path path;
    explicit TempFile(const char *pattern) : path(boost::filesystem::unique_path(pattern)) {}
    ~TempFile() { boost::system::error_code ec; boost::filesystem::remove(path, ec); }
    std::string str() const { return path.string(); }
};

void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) { for (unsigned i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i)); }
void put_float(std::vector<uint8_t> &b, size_t at, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(b, at, v); }
uint32_t get32(const std::vector<uint8_t> &b, size_t at) { return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3]; }
float get_float(const std::vector<uint8_t> &b, size_t at) { const uint32_t v = get32(b, at); float f; std::memcpy(&f, &v, 4); return f; }

std::vector<std::vector<uint8_t>> make_masks()
{
    std::vector<std::vector<uint8_t>> masks(layer_total, std::vector<uint8_t>(columns * rows, 0));
    std::mt19937 rng(7);
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < columns; ++x) {
            masks[0][y * columns + x] = (x > 10 && x < 40 && y > 8 && y < 30) ? 255 : 0;        // plain white block
            masks[1][y * columns + x] = 255;                                                    // fully lit
            masks[3][y * columns + x] = uint8_t(x * 4);                                         // gradient: difference chunks
            masks[4][y * columns + x] = uint8_t((rng() % 5) ? 0 : (rng() % 254) + 1);           // sparse noise
        }
    return masks; // masks[2] stays black
}

// Builds a complete file.
std::vector<uint8_t> make_goo(const std::vector<std::vector<uint8_t>> &masks)
{
    std::vector<uint8_t> b(header_size, 0);
    std::memcpy(b.data(), "V3.0", 4);
    const uint8_t magic[8] = {0x07, 0, 0, 0, 'D', 'L', 'P', 0};
    std::memcpy(b.data() + 4, magic, 8);
    std::memcpy(b.data() + 12, "Test Soft", 9);
    std::memcpy(b.data() + 68, "2026-01-02 03:04:05", 19);
    for (size_t i = 194; i < 194 + 100; ++i) b[i] = uint8_t(i); // previews must survive untouched
    put32(b, layer_count_at, uint32_t(masks.size()));
    b[layer_count_at + 4] = 0; b[layer_count_at + 5] = columns;
    b[layer_count_at + 6] = 0; b[layer_count_at + 7] = rows;
    put_float(b, volume_at, 10.f); put_float(b, volume_at + 4, 11.f); put_float(b, volume_at + 8, 12.f);
    put32(b, address_at, header_size);
    for (size_t i = 0; i < masks.size(); ++i) {
        std::vector<uint8_t> def(def_size, 0);
        put_float(def, 6, 0.05f * float(i + 1));            // PositionZ
        put_float(def, 10, 2.f + float(i));                 // ExposureTime: distinct per layer
        def[64] = 0x0D; def[65] = 0x0A;
        const auto rle = encode_goo_rle(masks[i].data(), masks[i].size());
        put32(def, 66, uint32_t(rle.size()));
        b.insert(b.end(), def.begin(), def.end());
        b.insert(b.end(), rle.begin(), rle.end());
        b.push_back(0x0D); b.push_back(0x0A);
    }
    const uint8_t footer[footer_size] = {0, 0, 0, 0x07, 0, 0, 0, 'D', 'L', 'P', 0};
    b.insert(b.end(), footer, footer + footer_size);
    return b;
}

void write_file(const std::string &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Walks the layer table; returns {def offset, data offset, data length} per layer.
struct Span { size_t def, data, length; };
std::vector<Span> layer_spans(const std::vector<uint8_t> &b)
{
    std::vector<Span> spans;
    size_t at = get32(b, address_at);
    for (uint32_t i = 0, n = get32(b, layer_count_at); i < n; ++i) {
        const size_t length = get32(b, at + 66);
        spans.push_back({at, at + def_size, length});
        at += def_size + length + 2;
    }
    return spans;
}

// True when no chunk after the magic is a colour-difference chunk (type 0b10).
bool has_no_difference_chunks(const uint8_t *rle, size_t length)
{
    for (size_t i = 1; i + 1 < length; ++i) {
        const unsigned type = rle[i] >> 6;
        if (type == 2) return false;
        const unsigned extra = (rle[i] >> 4) & 3;
        i += (type == 1 ? 1 : 0) + extra;
    }
    return true;
}

} // namespace

TEST_CASE("GOO reopens with every pixel and saves byte-identically when unchanged", "[sla_layers][sla_goo_reader]")
{
    const auto masks = make_masks();
    const auto bytes = make_goo(masks);
    TempFile source("goo-reader-%%%%.goo"), saved("goo-reader-%%%%.goo");
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    REQUIRE(document.archive_format() == "goo");
    REQUIRE(document.layer_count() == layer_total);
    for (size_t i = 0; i < layer_total; ++i) {
        const auto image = document.image(i);
        CAPTURE(i);
        REQUIRE(image.cols == columns);
        REQUIRE(image.rows == rows);
        CHECK(image.buf == masks[i]);
    }
    CHECK_FALSE(document.dirty());
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == bytes);

    // Saving under another container's extension is rejected before writing.
    TempFile wrong("goo-reader-%%%%.prz");
    CHECK_THROWS_WITH(document.save_archive(wrong.str()), ContainsSubstring("extension"));
}

TEST_CASE("GOO edit re-encodes only the changed layer and preserves header and layer parameters", "[sla_layers][sla_goo_reader]")
{
    const auto masks = make_masks();
    const auto bytes = make_goo(masks);
    TempFile source("goo-reader-%%%%.goo"), edited("goo-reader-%%%%.goo");
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    document.invert(3);
    REQUIRE(document.dirty());
    document.save_archive(edited.str());
    const auto out = read_file(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.layer_count() == layer_total);
    for (size_t i = 0; i < layer_total; ++i) {
        CAPTURE(i);
        auto expected = masks[i];
        if (i == 3) for (auto &p : expected) p = uint8_t(255 - p);
        CHECK(reopened.image(i).buf == expected);
    }

    // Header: identical except volume, mass and cost (scaled with the lit area).
    REQUIRE(out.size() >= header_size);
    CHECK(std::equal(bytes.begin(), bytes.begin() + volume_at, out.begin()));
    CHECK(std::equal(bytes.begin() + volume_at + 12, bytes.begin() + header_size, out.begin() + volume_at + 12));
    const auto before = layer_spans(bytes), after = layer_spans(out);
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CAPTURE(i);
        // Per-layer parameters (everything before DataLength) are untouched.
        CHECK(std::equal(bytes.begin() + before[i].def, bytes.begin() + before[i].def + 66, out.begin() + after[i].def));
        const bool same_rle = before[i].length == after[i].length &&
            std::equal(bytes.begin() + before[i].data, bytes.begin() + before[i].data + before[i].length, out.begin() + after[i].data);
        CHECK(same_rle == (i != 3));
    }
    CHECK(std::equal(bytes.end() - footer_size, bytes.end(), out.end() - footer_size));

    // Volume follows the thickness-weighted lit pixel count.
    auto lit = [&](const std::vector<std::vector<uint8_t>> &m) {
        double total = 0;
        for (size_t i = 0; i < m.size(); ++i)
            total += double(std::count_if(m[i].begin(), m[i].end(), [](uint8_t p) { return p != 0; })) * 0.05;
        return total;
    };
    auto changed = masks;
    for (auto &p : changed[3]) p = uint8_t(255 - p);
    const double ratio = lit(changed) / lit(masks);
    CHECK(get_float(out, volume_at) == Catch::Approx(10.f * ratio).epsilon(1e-4));
    CHECK(get_float(out, volume_at + 4) == Catch::Approx(11.f * ratio).epsilon(1e-4));
    CHECK(get_float(out, volume_at + 8) == Catch::Approx(12.f * ratio).epsilon(1e-4));

    // Undo returns the document to pixels equal to the source; saving then
    // reuses every original layer payload.
    REQUIRE(document.undo());
    TempFile restored("goo-reader-%%%%.goo");
    document.save_archive(restored.str());
    CHECK(read_file(restored.str()) == bytes);
}

TEST_CASE("PRZ is read as GOO and edited layers carry no colour-difference chunks", "[sla_layers][sla_goo_reader]")
{
    const auto masks = make_masks();
    TempFile source("goo-reader-%%%%.prz"), edited("goo-reader-%%%%.prz");
    write_file(source.str(), make_goo(masks));

    auto document = sla::LayerDocument::load_archive(source.str());
    REQUIRE(document.archive_format() == "prz");
    document.invert(3);
    document.save_archive(edited.str());
    const auto out = read_file(edited.str());
    const auto spans = layer_spans(out);
    CHECK(has_no_difference_chunks(out.data() + spans[3].data, spans[3].length));

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    auto expected = masks[3];
    for (auto &p : expected) p = uint8_t(255 - p);
    CHECK(reopened.image(3).buf == expected);
    CHECK(reopened.image(4).buf == masks[4]);
}

TEST_CASE("Damaged GOO files fail with specific messages", "[sla_layers][sla_goo_reader]")
{
    const auto bytes = make_goo(make_masks());
    TempFile file("goo-reader-%%%%.goo");
    auto load = [&](const std::vector<uint8_t> &data) { write_file(file.str(), data); return sla::LayerDocument::load_archive(file.str()); };

    SECTION("shorter than the fixed header") {
        REQUIRE_THROWS_WITH(load(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 1000)), ContainsSubstring("truncated"));
    }
    SECTION("layer table cut short") {
        REQUIRE_THROWS_WITH(load(std::vector<uint8_t>(bytes.begin(), bytes.end() - 200)), ContainsSubstring("GOO"));
    }
    SECTION("layer count larger than the file can hold") {
        auto data = bytes; put32(data, layer_count_at, 90000);
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("layer table is truncated"));
    }
    SECTION("zero layers") {
        auto data = bytes; put32(data, layer_count_at, 0);
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("layer count"));
    }
    SECTION("zero resolution") {
        auto data = bytes; data[layer_count_at + 4] = data[layer_count_at + 5] = 0;
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("resolution"));
    }
    SECTION("layer table address outside the file") {
        auto data = bytes; put32(data, address_at, uint32_t(bytes.size()));
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("table address"));
    }
    SECTION("data length larger than the file") {
        auto data = bytes; put32(data, header_size + 66, 0x7FFFFFFF);
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("layer 1 data is truncated"));
    }
    SECTION("RLE checksum") {
        auto data = bytes; const auto spans = layer_spans(bytes);
        data[spans[1].data + spans[1].length - 1] ^= 0x5A;
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("layer 2 RLE checksum"));
    }
    SECTION("RLE magic") {
        auto data = bytes; const auto spans = layer_spans(bytes);
        data[spans[0].data] = 0x54;
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("layer 1 RLE magic"));
    }
    SECTION("footer magic") {
        auto data = bytes; data[data.size() - 2] = 'X';
        REQUIRE_THROWS_WITH(load(data), ContainsSubstring("footer"));
    }
}

TEST_CASE("GOO loading and saving honour cancellation", "[sla_layers][sla_goo_reader]")
{
    TempFile source("goo-reader-%%%%.goo"), out("goo-reader-%%%%.goo");
    write_file(source.str(), make_goo(make_masks()));
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(source.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);

    auto document = sla::LayerDocument::load_archive(source.str());
    document.invert(0);
    REQUIRE_THROWS_AS(document.save_archive(out.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_FALSE(boost::filesystem::exists(out.path));
    CHECK(document.dirty());
}
