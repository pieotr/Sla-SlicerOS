// SPDX-License-Identifier: AGPL-3.0-or-later
// Native open/edit/save-back of the remaining binary printer formats through
// LayerDocument (LGS family, ...). Every family is exercised the same way:
// the real exporter writes a file from known masks, the editor reopens it and
// must show the exported pixels, an unchanged save must be byte-identical, an
// edited save must reopen with only the edit applied, and damaged or truncated
// input must fail with a message instead of crashing.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "sla_archive_test_config.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

namespace {

struct TempFile {
    boost::filesystem::path path;
    explicit TempFile(const std::string &name)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("more-readers2-%%%%-%%%%-" + name)) {}
    ~TempFile() { boost::system::error_code ec; boost::filesystem::remove(path, ec); }
    std::string str() const { return path.string(); }
};

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

void put32le(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
    for (unsigned n = 0; n < 4; ++n) bytes.at(at + n) = uint8_t(value >> (8 * n));
}
uint32_t get32le(const std::vector<uint8_t> &bytes, size_t at)
{
    return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at + 1)) << 8) | (uint32_t(bytes.at(at + 2)) << 16) | (uint32_t(bytes.at(at + 3)) << 24);
}

// Layers that stress a run-length codec: a gradient (every level, short runs),
// white and black pages (the longest possible run), and sparse random blocks.
std::vector<std::vector<uint8_t>> family_masks(size_t columns, size_t rows)
{
    std::vector<std::vector<uint8_t>> masks(4, std::vector<uint8_t>(columns * rows, 0));
    std::mt19937 rng(7);
    for (size_t i = 0; i < columns * rows; ++i) {
        masks[0][i] = uint8_t(i * 255 / (columns * rows - 1));
        masks[1][i] = 255;
        const size_t x = i % columns, y = i / columns;
        const bool block = x > columns / 5 && x < columns / 2 && y > rows / 6 && y < rows / 2;
        masks[3][i] = block ? 255 : (rng() % 5 == 0 ? uint8_t(rng()) : 0);
    }
    return masks; // masks[2] stays black
}

struct Family {
    std::string id;
    size_t columns, rows;
    std::function<void(SLAFullPrintConfig &)> tweak;
    std::function<uint8_t(uint8_t)> stored; // what a gray value becomes in the file
};

uint8_t identity(uint8_t v) { return v; }

const Family &family(const std::string &id)
{
    static const std::vector<Family> families = {
        {"lgs", 128, 256, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t((v >> 4) * 17); }},
        {"lgs30", 128, 256, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t((v >> 4) * 17); }},
        {"lgs120", 128, 256, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t((v >> 4) * 17); }},
        {"lgs4k", 128, 256, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t((v >> 4) * 17); }},
        {"jxs", 4920, 2880, [](SLAFullPrintConfig &) {}, identity},
        {"vdt", 200, 120, [](SLAFullPrintConfig &) {}, identity},
        {"mdlp", 200, 120, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t(v >= 128 ? 255 : 0); }},
        {"qdt", 800, 400, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t(v >= 128 ? 255 : 0); }},
        {"osf", 200, 120, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t((v & 0xfe) ? ((v & 0xfe) | 1) : 0); }},
        {"osla", 200, 120, [](SLAFullPrintConfig &) {}, identity},
        {"n4", 1440, 2560, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t(v > 127 ? 255 : 0); }},
        {"n7", 2560, 1600, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t(v > 127 ? 255 : 0); }},
        {"svgx", 200, 120, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t(v ? 255 : 0); }},
        {"gr1", 200, 120, [](SLAFullPrintConfig &) {}, [](uint8_t v) { return uint8_t(v >= 128 ? 255 : 0); }},
    };
    for (const auto &candidate : families)
        if (candidate.id == id) return candidate;
    FAIL("unknown family " + id);
    return families.front();
}

// Exports `masks` (already in printer raster coordinates) with the real writer.
void export_family(const Family &f, const std::vector<std::vector<uint8_t>> &masks, const std::string &path)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, f.id);
    full.set("sla_archive_format", std::string("SL1"));
    if (f.id.rfind("lgs", 0) == 0) {
        full.set("display_pixels_x", int(f.columns)); full.set("display_pixels_y", int(f.rows));
        full.set("display_width", 64.); full.set("display_height", 128.);
        full.set("sla_bottom_layers", 1);
    }
    f.tweak(full);
    DynamicPrintConfig cfg;
    cfg.apply(full);
    std::vector<sla::EncodedRaster> encoded;
    std::vector<double> heights;
    for (size_t i = 0; i < masks.size(); ++i) {
        encoded.push_back(sla::PNGRasterEncoder{}(masks[i].data(), f.columns, f.rows, 1));
        heights.push_back(.05 * double(i + 1));
    }
    auto source = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source, heights);
    ThumbnailData thumbnail;
    thumbnail.set(120, 150);
    for (size_t i = 0; i < thumbnail.pixels.size(); i += 4) { thumbnail.pixels[i] = 200; thumbnail.pixels[i + 3] = 255; }
    job->export_layer_document(source, f.id, path, {thumbnail}, f.id);
}

std::vector<uint8_t> stored_copy(const Family &f, const std::vector<uint8_t> &mask)
{
    std::vector<uint8_t> result = mask;
    for (auto &value : result) value = f.stored(value);
    return result;
}

// Every family, and the ones whose edits can be saved back (SVGX is view-only).
const auto all_ids = [] { return GENERATE(std::string("lgs"), std::string("lgs30"), std::string("lgs120"), std::string("lgs4k"), std::string("jxs"), std::string("vdt"),
                                       std::string("mdlp"), std::string("gr1"), std::string("qdt"), std::string("osf"), std::string("osla"),
                                       std::string("n4"), std::string("n7"), std::string("svgx")); };
const auto editable_ids = [] { return GENERATE(std::string("lgs"), std::string("lgs30"), std::string("lgs120"), std::string("lgs4k"), std::string("jxs"), std::string("vdt"),
                                       std::string("mdlp"), std::string("gr1"), std::string("qdt"), std::string("osf"), std::string("osla"),
                                       std::string("n4"), std::string("n7")); };

} // namespace

TEST_CASE("Exported files reopen with the exported pixels and save byte-identically when unchanged", "[sla_layers][sla_more_readers2]")
{
    const std::string id = all_ids();
    CAPTURE(id);
    const auto &f = family(id);
    const auto masks = family_masks(f.columns, f.rows);
    TempFile exported("exported." + id), same("same." + id);
    REQUIRE_NOTHROW(export_family(f, masks, exported.str()));

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == id);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        const auto image = document.image(i);
        REQUIRE(image.cols == f.columns);
        REQUIRE(image.rows == f.rows);
        CHECK(image.buf == stored_copy(f, masks[i]));
    }
    CHECK_FALSE(document.dirty());
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));

    // The extension names the family: another family's extension is refused.
    TempFile wrong("out.sl1");
    CHECK_THROWS_WITH(document.save_archive(wrong.str()), ContainsSubstring("extension"));
    CHECK_FALSE(boost::filesystem::exists(wrong.path));
}

TEST_CASE("Edited layers are rewritten in place and every other layer survives", "[sla_layers][sla_more_readers2]")
{
    const std::string id = editable_ids();
    CAPTURE(id);
    const auto &f = family(id);
    const auto masks = family_masks(f.columns, f.rows);
    TempFile exported("exported." + id), edited("edited." + id), twice("twice." + id);
    export_family(f, masks, exported.str());

    auto document = sla::LayerDocument::load_archive(exported.str());
    document.invert(1);
    // The editor shows what will be saved: values the file cannot hold are snapped.
    document.paint(3, 10, 10, 6, 77);
    const auto shown1 = document.image(1).buf, shown3 = document.image(3).buf;
    CHECK(shown3[10 * f.columns + 10] == f.stored(77));
    CHECK(document.dirty());
    document.save_archive(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.layer_count() == masks.size());
    CHECK(reopened.image(1).buf == shown1);
    CHECK(reopened.image(3).buf == shown3);
    CHECK(reopened.image(0).buf == stored_copy(f, masks[0]));
    CHECK(reopened.image(2).buf == stored_copy(f, masks[2]));
    // Saving the reopened edit again changes nothing.
    reopened.save_archive(twice.str());
    CHECK(read_file(twice.str()) == read_file(edited.str()));
}

TEST_CASE("Damaged and truncated files fail with a message and never crash", "[sla_layers][sla_more_readers2]")
{
    const std::string id = all_ids();
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + id), damaged("damaged." + id);
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    const auto good = read_file(exported.str());
    auto load = [&](const std::vector<uint8_t> &data) { write_file(damaged.str(), data); return sla::LayerDocument::load_archive(damaged.str()); };

    SECTION("shorter than the header") {
        auto bytes = good; bytes.resize(100);
        CHECK_THROWS(load(bytes));
    }
    SECTION("every truncation point is an error, not a crash") {
        // Cutting any number of bytes off the end must be reported. Probe a
        // spread of cut sizes instead of every byte to keep the test fast.
        for (size_t cut : {size_t(1), size_t(2), size_t(7), size_t(100), good.size() / 3, good.size() / 2, good.size() - 181}) {
            CAPTURE(cut);
            if ((id == "qdt" || id == "svgx") && cut == 1) continue; // a text file may legitimately lack its final newline
            auto bytes = good; bytes.resize(good.size() - cut);
            CHECK_THROWS_AS(load(bytes), Slic3r::Exception);
        }
    }
    SECTION("random single-byte damage never crashes") {
        std::mt19937 rng(3);
        for (int round = 0; round < (f.columns * f.rows > 1000000 ? 6 : 40); ++round) {
            auto bytes = good;
            // Half of the hits go to the start of the file (headers), half anywhere.
            bytes[rng() % ((round % 2) ? bytes.size() : std::min<size_t>(bytes.size(), 600))] ^= uint8_t(1 + rng() % 255);
            try { load(bytes); } catch (const Slic3r::Exception &) {} catch (const sla::LayerOperationCancelled &) { FAIL("cancelled"); }
        }
    }
}

TEST_CASE("Loading and saving honour cancellation", "[sla_layers][sla_more_readers2]")
{
    const std::string id = editable_ids();
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + id), out("out." + id);
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(exported.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    auto document = sla::LayerDocument::load_archive(exported.str());
    document.invert(0);
    REQUIRE_THROWS_AS(document.save_archive(out.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_FALSE(boost::filesystem::exists(out.path));
    CHECK(document.dirty());
}

TEST_CASE("LGS files reject specific damage with specific messages", "[sla_layers][sla_more_readers2]")
{
    const std::string id = GENERATE(std::string("lgs"), std::string("lgs120"), std::string("lgs4k"));
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + id), damaged("damaged." + id);
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    const auto good = read_file(exported.str());
    auto load = [&](const std::vector<uint8_t> &data) { write_file(damaged.str(), data); return sla::LayerDocument::load_archive(damaged.str()); };

    SECTION("unknown printer model") {
        auto bytes = good; put32le(bytes, 16, 77);
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("model 77"));
    }
    SECTION("zero layers") {
        auto bytes = good; put32le(bytes, 164, 0);
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("layer count"));
    }
    SECTION("more layers than the file holds") {
        auto bytes = good; put32le(bytes, 164, 90000);
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("truncated"));
    }
    SECTION("layer record larger than the file") {
        auto bytes = good;
        size_t at = 180 + 120 * 150 * 2;
        if (id == "lgs120") at += 4 + get32le(good, at) + 2;
        put32le(bytes, at, 0x7fffffff);
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("truncated"));
    }
    SECTION("RLE that stops before the raster is full") {
        // The last record is the all-black page: shorten it by one digit.
        auto bytes = good;
        size_t at = 180 + 120 * 150 * 2;
        if (id == "lgs120") at += 4 + get32le(good, at) + 2;
        size_t last = 0;
        for (size_t i = 0; i < f.columns * 0 + 4; ++i) { last = at; at += 4 + get32le(good, at); }
        const uint32_t size = get32le(good, last);
        REQUIRE(size > 1);
        put32le(bytes, last, size - 1);
        bytes.erase(bytes.begin() + last + 4 + size - 1);
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("layer 4"));
    }
    SECTION("a run past the raster") {
        auto bytes = good;
        size_t at = 180 + 120 * 150 * 2;
        if (id == "lgs120") at += 4 + get32le(good, at) + 2;
        size_t last = 0;
        for (size_t i = 0; i < 4; ++i) { last = at; at += 4 + get32le(good, at); }
        put32le(bytes, last, get32le(good, last) + 2);
        // Two more full-white digits can never fit.
        bytes.insert(bytes.begin() + at, {0xf1, 0xf1});
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("layer 4"));
    }
}

TEST_CASE("SVGX opens for viewing, copies unchanged and refuses to save pixel edits", "[sla_layers][sla_more_readers2]")
{
    const auto &f = family("svgx");
    TempFile exported("exported.svgx"), same("same.svgx"), edited("edited.svgx");
    const auto masks = family_masks(f.columns, f.rows);
    export_family(f, masks, exported.str());

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == "svgx");
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(document.image(i).buf == stored_copy(f, masks[i]));
    }
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));

    // Editing is possible in memory, but saving would need an exact re-trace of the contours.
    document.invert(1);
    CHECK_THROWS_WITH(document.save_archive(edited.str()), ContainsSubstring("vector contours"));
    CHECK_FALSE(boost::filesystem::exists(edited.path));
    CHECK(document.dirty());

    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(exported.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
}

TEST_CASE("SVGX files with a damaged document fail with specific messages", "[sla_layers][sla_more_readers2]")
{
    const auto &f = family("svgx");
    TempFile exported("exported.svgx"), damaged("damaged.svgx");
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    const auto good = read_file(exported.str());
    auto load = [&](const std::vector<uint8_t> &data) { write_file(damaged.str(), data); return sla::LayerDocument::load_archive(damaged.str()); };
    auto replace_text = [&](const std::string &from, const std::string &to) {
        std::string text(good.begin(), good.end());
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return std::vector<uint8_t>(text.begin(), text.end());
    };
    SECTION("document address outside the file") {
        auto bytes = good; put32le(bytes, 24, 0x7fffffff);
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("document address"));
    }
    SECTION("entities are refused") {
        CHECK_THROWS_WITH(load(replace_text("<svg ", "<!DOCTYPE svg [<!ENTITY a \"b\">]><svg ")), ContainsSubstring("DTD"));
    }
    SECTION("zero resolution") {
        CHECK_THROWS_WITH(load(replace_text("resolutionx=\"200\"", "resolutionx=\"0\"")), ContainsSubstring("resolution"));
    }
    SECTION("zero layers") {
        CHECK_THROWS_WITH(load(replace_text("layercount=\"4\"", "layercount=\"0\"")), ContainsSubstring("layer count"));
    }
    SECTION("a group beyond the declared layers") {
        CHECK_THROWS_WITH(load(replace_text("layercount=\"4\"", "layercount=\"2\"")), ContainsSubstring("beyond its declared"));
    }
    SECTION("missing print parameters") {
        CHECK_THROWS_WITH(load(replace_text("<printparams ", "<printparam ")), ContainsSubstring("printparams"));
    }
}

TEST_CASE("QDT edits re-vectorise exactly like the exporter", "[sla_layers][sla_more_readers2]")
{
    // The edited layer's text must be the bytes the exporter writes for that
    // mask, so a QDT edited here and one exported from the same masks are equal.
    const auto &f = family("qdt");
    TempFile exported("exported.qdt"), edited("edited.qdt"), reference("reference.qdt");
    auto masks = family_masks(f.columns, f.rows);
    export_family(f, masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    for (size_t layer : {size_t(1), size_t(2)}) { // an odd (mirrored) and an even layer
        auto image = document.image(layer);
        for (size_t i = 0; i < image.buf.size(); i += 5) image.buf[i] = image.buf[i] ? 0 : 255;
        document.replace_image(layer, image);
        masks[layer] = image.buf;
    }
    document.save_archive(edited.str());
    export_family(f, masks, reference.str());
    CHECK(read_file(edited.str()) == read_file(reference.str()));
}

TEST_CASE("OSF runs that look like a layer marker survive an edit", "[sla_layers][sla_more_readers2]")
{
    // A run of ten or eleven 0x0c pixels must be written in the long form: the
    // reader (like UVtools) finds the next layer by the byte pair 0D 0A / 0D 0B.
    const auto &f = family("osf");
    TempFile exported("exported.osf"), edited("edited.osf");
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    auto image = document.image(2);
    std::fill(image.buf.begin(), image.buf.end(), 0);
    std::fill_n(image.buf.begin() + 3, 10, 12);  // gray 12 -> 0x0c, run of 10
    std::fill_n(image.buf.begin() + 40, 11, 12); // run of 11
    image.buf[100] = 255;
    document.replace_image(2, image);
    const auto shown = document.image(2).buf;
    document.save_archive(edited.str());
    CHECK(sla::LayerDocument::load_archive(edited.str()).image(2).buf == shown);
}

TEST_CASE("Material statistics follow an edit where the format stores them", "[sla_layers][sla_more_readers2]")
{
    // OSLA keeps lit-pixel count and bounding box per layer; they must describe the edited mask.
    const auto &f = family("osla");
    TempFile exported("exported.osla"), edited("edited.osla");
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    auto image = document.image(2); // the black layer
    for (size_t y = 10; y < 20; ++y) for (size_t x = 30; x < 50; ++x) image.buf[y * f.columns + x] = 255;
    document.replace_image(2, image);
    document.save_archive(edited.str());
    const auto bytes = read_file(edited.str());
    const size_t table = get32le(bytes, 150 + 79), record = table + 2 * get32le(bytes, 150 + 75);
    CHECK(get32le(bytes, record + 53) == 200);       // lit pixels
    CHECK(get32le(bytes, record + 57) == 30);        // x
    CHECK(get32le(bytes, record + 61) == 10);        // y
    CHECK(get32le(bytes, record + 65) == 20);        // width
    CHECK(get32le(bytes, record + 69) == 10);        // height
}

TEST_CASE("MDLP and GR1 files reject specific damage with specific messages", "[sla_layers][sla_more_readers2]")
{
    const std::string id = GENERATE(std::string("mdlp"), std::string("gr1"));
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + id), damaged("damaged." + id);
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    const auto good = read_file(exported.str());
    auto load = [&](const std::vector<uint8_t> &data) { write_file(damaged.str(), data); return sla::LayerDocument::load_archive(damaged.str()); };
    auto be32 = [&](size_t at) { return (size_t(good[at]) << 24) | (size_t(good[at + 1]) << 16) | (size_t(good[at + 2]) << 8) | good[at + 3]; };
    // Walk the slicer info to the first layer record.
    size_t at = 4 + 7 + 290 * 290 * 2 + 116 * 116 * 2 + 4 + 6;
    for (int text = 0; text < 3; ++text) at += 4 + be32(at);
    at += id == "gr1" ? 8 + 14 : 8;

    SECTION("closing header missing") {
        auto bytes = good; bytes.resize(bytes.size() - 11);
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("closing header"));
    }
    SECTION("zero layers") {
        auto bytes = good; bytes[4 + 7 + 290 * 290 * 2 + 116 * 116 * 2 + 4] = 0; bytes[4 + 7 + 290 * 290 * 2 + 116 * 116 * 2 + 5] = 0;
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("layer count"));
    }
    SECTION("a line below the raster") {
        auto bytes = good;
        REQUIRE(be32(at) > 0);
        bytes[at + 4 + 2] = 0xff; bytes[at + 4 + 3] = 0xff; // end y of the first line
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("outside the raster"));
    }
    SECTION("a layer table longer than the file") {
        auto bytes = good;
        bytes[at] = 0x7f;
        CHECK_THROWS_WITH(load(bytes), ContainsSubstring("truncated"));
    }
    SECTION("an unrelated extension is told apart by structure") {
        // MDLP and GR1 share a magic and differ only in the settings tail, so with
        // no .mdlp/.gr1 extension the reader tries both and keeps the one that fits.
        TempFile other("other.bin");
        write_file(other.str(), good);
        auto reopened = sla::LayerDocument::load_archive(other.str());
        CHECK(reopened.archive_format() == id);
    }
}
