// CXDLP v1 / v2 / v3 editing contract: an unchanged save is byte identical, an edit re-encodes only the edited layer (every
// other record is carried over byte for byte), and the v2/v3 checksum stays valid. Hand-built fixtures so the test pins the
// container layout, not the exporter.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <cmath>
#include <fstream>

#include "libslic3r/Format/CrealityCXDLPv1Archive.hpp"
#include "libslic3r/Format/CrealityChecksum.hpp"
#include "libslic3r/Format/CrealityContourArea.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"

using namespace Slic3r;

namespace {

constexpr size_t cols = 40, rows = 30;
constexpr double width_mm = 40., height_mm = 30.;

struct TempFile {
    boost::filesystem::path path;
    explicit TempFile(const std::string &name)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("cxdlp-edit-%%%%-" + name)) {}
    ~TempFile() { boost::system::error_code error; boost::filesystem::remove(path, error); }
};

std::vector<uint8_t> read_file(const boost::filesystem::path &path)
{
    std::ifstream input(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void write_file(const boost::filesystem::path &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
}

void put16(std::vector<uint8_t> &out, uint32_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t> &out, uint32_t v) { put16(out, v >> 16); put16(out, v & 0xffff); }
void put_string16(std::vector<uint8_t> &out, const std::string &text)
{
    put32(out, uint32_t(text.size() * 2));
    for (const char c : text) put16(out, uint8_t(c));
}
void put_magic(std::vector<uint8_t> &out)
{
    put32(out, 9);
    for (const char c : std::string("CXSW3DV2\0", 9)) out.push_back(uint8_t(c));
}
void page(std::vector<uint8_t> &out) { out.push_back(13); out.push_back(10); }

std::vector<uint8_t> mask_of(unsigned layer)
{
    std::vector<uint8_t> pixels(cols * rows, 0);
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < cols; ++x) {
            if (((x + layer * 3) / 5 + y / 7) % 3 == 0) pixels[y * cols + x] = uint8_t(60 + 30 * ((x / 5 + layer) % 6));
            if (layer == 2 && x > 30 && y > 20) pixels[y * cols + x] = 255;
        }
    return pixels;
}

// First word of a record: the lit pixel count (v1) or the contour area the exporter stores (v2/v3).
uint32_t first_word(unsigned version, const std::vector<uint8_t> &pixels, const std::vector<uint8_t> &record)
{
    if (version == 1) return (uint32_t(record[0]) << 24) | (uint32_t(record[1]) << 16) | (uint32_t(record[2]) << 8) | record[3];
    const float pitch_x = float(std::round(float(width_mm) / cols * 10000.f) / 10000.f);
    const float pitch_y = float(std::round(float(height_mm) / rows * 10000.f) / 10000.f);
    return uint32_t(sla::creality_largest_contour_area(pixels.data(), cols, rows) * float(pitch_x * pitch_y) * 1000.);
}

struct Fixture {
    std::vector<uint8_t> bytes;
    size_t table_at = 0;
};

Fixture build(unsigned version, size_t layers, bool foreign_first_word)
{
    Fixture fixture;
    auto &out = fixture.bytes;
    put_magic(out);
    if (version == 1) put16(out, uint32_t(layers));
    else {
        put16(out, version);
        put32(out, 6);
        for (const char c : std::string("CL-60\0", 6)) out.push_back(uint8_t(c));
        put16(out, uint32_t(layers));
    }
    put16(out, cols); put16(out, rows);
    if (version >= 2) out.insert(out.end(), 64, uint8_t(0x11));
    for (const size_t size : {116, 290, 290}) {
        for (size_t i = 0; i < size * size; ++i) put16(out, uint32_t(1 + i % 65000));
        page(out);
    }
    put_string16(out, "40"); put_string16(out, "30"); put_string16(out, "0.05");
    for (unsigned i = 0; i < 22; ++i) out.push_back(uint8_t(0x20 + i));
    std::vector<std::vector<uint8_t>> records(layers);
    for (size_t i = 0; i < layers; ++i) {
        const auto pixels = mask_of(unsigned(i));
        const auto packet = CrealityCXDLPv1Archive::encode_mask(pixels.data(), cols, rows, 1);
        const auto *p = static_cast<const uint8_t *>(packet.data());
        records[i].assign(p, p + packet.size());
        uint32_t word = first_word(version, pixels, records[i]);
        // A record written by another tool may carry a different area word; it has to survive an edit of a neighbour.
        if (foreign_first_word && version >= 2 && i == 1) word += 7;
        for (unsigned n = 0; n < 4; ++n) records[i][n] = uint8_t(word >> (24 - 8 * n));
    }
    fixture.table_at = out.size();
    for (const auto &record : records) out.insert(out.end(), record.begin(), record.begin() + 4);
    page(out);
    if (version >= 3) {
        put32(out, 11); for (const char c : std::string("SLA-Slicer\0", 11)) out.push_back(uint8_t(c));
        put32(out, 1); out.push_back(0);
        for (unsigned i = 0; i < 20; ++i) out.push_back(uint8_t(0x40 + i));
        page(out);
    }
    for (const auto &record : records) { out.insert(out.end(), record.begin(), record.end()); page(out); }
    put_magic(out);
    if (version == 1) put32(out, 0x12345678);
    else {
        const uint32_t checksum = sla::creality_checksum(out.data(), out.size(), version);
        put32(out, checksum);
    }
    return fixture;
}

// Record boundaries [begin, end) including the closing CR LF, walked from the table.
std::vector<std::pair<size_t, size_t>> records_of(const std::vector<uint8_t> &bytes, unsigned version, size_t table_at, size_t layers)
{
    size_t at = table_at + 4 * layers + 2;
    if (version >= 3) {
        for (int i = 0; i < 2; ++i) {
            const size_t size = (size_t(bytes.at(at)) << 24) | (size_t(bytes.at(at + 1)) << 16) | (size_t(bytes.at(at + 2)) << 8) | bytes.at(at + 3);
            at += 4 + size;
        }
        at += 20 + 2;
    }
    std::vector<std::pair<size_t, size_t>> result;
    for (size_t i = 0; i < layers; ++i) {
        const size_t lines = (size_t(bytes.at(at + 4)) << 24) | (size_t(bytes.at(at + 5)) << 16) | (size_t(bytes.at(at + 6)) << 8) | bytes.at(at + 7);
        const size_t end = at + 8 + lines * 6 + 2;
        result.emplace_back(at, end);
        at = end;
    }
    return result;
}

std::vector<uint8_t> slice(const std::vector<uint8_t> &bytes, std::pair<size_t, size_t> range)
{
    return {bytes.begin() + range.first, bytes.begin() + range.second};
}

} // namespace

TEST_CASE("CXDLP v1/v2/v3 edits keep unchanged records verbatim and the checksum valid", "[sla_cxdlp_edit]")
{
    const unsigned version = GENERATE(1u, 2u, 3u);
    const bool foreign = GENERATE(false, true);
    CAPTURE(version, foreign);
    constexpr size_t layers = 5;
    const auto fixture = build(version, layers, foreign);
    const char *format = version == 1 ? "cxdlpv1" : version == 2 ? "cxdlpv2" : "cxdlp";
    const std::string suffix = version == 1 ? ".v1.cxdlp" : version == 2 ? ".v2.cxdlp" : ".cxdlp";
    TempFile source("source" + suffix), unchanged("unchanged" + suffix), edited("edited" + suffix);
    write_file(source.path, fixture.bytes);

    auto document = sla::LayerDocument::load_archive(source.path.string());
    REQUIRE(document.archive_format() == format);
    REQUIRE(document.layer_count() == layers);
    for (size_t i = 0; i < layers; ++i) REQUIRE((document.image(i).buf == mask_of(unsigned(i))));

    // Untouched document: byte-identical file.
    document.save_archive(unchanged.path.string());
    REQUIRE((read_file(unchanged.path) == fixture.bytes));

    // Edit one layer: new lit region plus a changed gray run.
    auto image = document.image(3);
    for (size_t y = 2; y < 9; ++y)
        for (size_t x = 1; x < 12; ++x) image.buf[y * cols + x] = uint8_t(100 + x);
    document.replace_image(3, image);
    document.save_archive(edited.path.string());
    const auto saved = read_file(edited.path);

    const auto before = records_of(fixture.bytes, version, fixture.table_at, layers);
    const auto after = records_of(saved, version, fixture.table_at, layers);
    for (size_t i = 0; i < layers; ++i) {
        if (i == 3) REQUIRE((slice(saved, after[i]) != slice(fixture.bytes, before[i])));
        else REQUIRE((slice(saved, after[i]) == slice(fixture.bytes, before[i])));
    }
    // Prefix, table (but the edited entry) and footer stay as they were.
    REQUIRE(std::equal(fixture.bytes.begin(), fixture.bytes.begin() + fixture.table_at, saved.begin()));
    for (size_t i = 0; i < layers; ++i)
        if (i != 3) REQUIRE(std::equal(fixture.bytes.begin() + fixture.table_at + 4 * i, fixture.bytes.begin() + fixture.table_at + 4 * i + 4,
                                       saved.begin() + fixture.table_at + 4 * i));
    constexpr size_t footer = 13 + 4;
    REQUIRE(std::equal(fixture.bytes.end() - footer, fixture.bytes.end() - (version == 1 ? 0 : 4), saved.end() - footer));
    if (version >= 2) {
        const uint32_t checksum = sla::creality_checksum(saved.data(), saved.size() - 4, version);
        const uint32_t stored = (uint32_t(saved[saved.size() - 4]) << 24) | (uint32_t(saved[saved.size() - 3]) << 16) |
                                (uint32_t(saved[saved.size() - 2]) << 8) | saved[saved.size() - 1];
        REQUIRE(stored == checksum);
    }

    const auto reopened = sla::LayerDocument::load_archive(edited.path.string());
    REQUIRE(reopened.layer_count() == layers);
    for (size_t i = 0; i < layers; ++i)
        REQUIRE((reopened.image(i).buf == (i == 3 ? image.buf : mask_of(unsigned(i)))));

    // A saved document opens, saves unchanged and is byte-identical again.
    TempFile again("again" + suffix);
    auto second = sla::LayerDocument::load_archive(edited.path.string());
    second.save_archive(again.path.string());
    REQUIRE((read_file(again.path) == saved));
}
