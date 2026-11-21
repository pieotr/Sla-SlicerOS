// SPDX-License-Identifier: AGPL-3.0-or-later
// Native open/edit/save-back of more printer formats through LayerDocument
// (Anycubic ZIP family, ...). Source files are built here from the documented
// layouts with an encoder that shares no code with the reader, and a second
// group of tests reopens files produced by the real exporters.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "sla_archive_test_config.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::json;

namespace {

struct TempFile {
    boost::filesystem::path path;
    explicit TempFile(const std::string &name)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("more-readers-%%%%-%%%%-" + name)) {}
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

std::vector<uint8_t> text_bytes(const std::string &text) { return {text.begin(), text.end()}; }

// Every entry of a ZIP by exact (case preserved) name; read_zipper_archive
// lower-cases names, which would hide a wrong ".pw0Img" spelling.
std::map<std::string, std::vector<uint8_t>> zip_entries(const std::string &path)
{
    std::map<std::string, std::vector<uint8_t>> entries;
    MZ_Archive archive;
    REQUIRE(open_zip_reader(&archive.arch, path));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive.arch); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&archive.arch, i, &stat));
        if (mz_zip_reader_is_file_a_directory(&archive.arch, i)) continue;
        std::vector<uint8_t> data(size_t(stat.m_uncomp_size));
        if (!data.empty()) REQUIRE(mz_zip_reader_extract_to_mem(&archive.arch, i, data.data(), data.size(), 0));
        entries.emplace(stat.m_filename, std::move(data));
    }
    close_zip_reader(&archive.arch);
    return entries;
}

// Lines of a Klipper script may differ after a mask edit only where they carry
// statistics derived from the masks; everything else (G-code motion, Z, waits,
// exposure) must stay byte-identical.
void check_only_statistics_changed(const std::vector<uint8_t> &before, const std::vector<uint8_t> &after)
{
    auto split = [](const std::vector<uint8_t> &bytes) {
        std::vector<std::string> lines; std::string line;
        for (uint8_t c : bytes) { if (c == '\n') { lines.push_back(line); line.clear(); } else line.push_back(char(c)); }
        lines.push_back(line); return lines;
    };
    const auto a = split(before), b = split(after);
    REQUIRE(a.size() == b.size());
    static const char *statistics[] = { "VOLUME=", "MATERIALML=", "CONSUME_MATERIAL=", "MATERIAL_TOTAL=", "BOUNDS=",
        "AREA", "PIXELS", ";Volume:", ";MaterialMl:", ";MaterialGrams:", ";Price:", ";PrintTime:" };
    size_t changed = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) continue;
        ++changed;
        CAPTURE(i, a[i], b[i]);
        CHECK(std::any_of(std::begin(statistics), std::end(statistics),
                          [&](const char *key) { return a[i].find(key) != std::string::npos; }));
    }
    CHECK(changed > 0); // the inverted layer changes its lit area
}

// A gradient of n distinct values; every layer differs so a mix-up is visible.
std::vector<std::vector<uint8_t>> test_masks(size_t columns, size_t rows, size_t count, bool only_pw0_levels)
{
    std::vector<std::vector<uint8_t>> masks(count, std::vector<uint8_t>(columns * rows, 0));
    std::mt19937 rng(11);
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < columns; ++x) {
            const bool block = x > columns / 5 && x < columns / 2 && y > rows / 6 && y < rows / 2;
            masks[0][y * columns + x] = block ? 255 : 0;
            if (count > 1) masks[1][y * columns + x] = uint8_t((x * 16 / columns) * 17); // all sixteen PW0 levels
            if (count > 3) masks[3][y * columns + x] = 255;                              // one run longer than 4095 pixels
            if (count > 4) masks[4][y * columns + x] = (rng() % 3) ? 0 : uint8_t((rng() % 16) * 17);
        }
    // masks[2] stays black. Other formats keep full eight-bit values.
    if (!only_pw0_levels)
        for (auto &mask : masks)
            for (size_t i = 0; i < mask.size(); i += 7)
                mask[i] = uint8_t(mask[i] ? mask[i] - 1 : 3);
    return masks;
}

png::ImageGreyscale make_image(size_t columns, size_t rows, const std::vector<uint8_t> &pixels)
{
    png::ImageGreyscale image;
    image.cols = columns; image.rows = rows; image.buf = pixels;
    return image;
}

// ---------------------------------------------------------------------------
// Anycubic ZIP (pm4u, pm7, pm7m, pwsz)
// ---------------------------------------------------------------------------
constexpr size_t zip_columns = 100, zip_rows = 60, zip_layers = 5;

// Test-local PW0 encoder: deliberately written differently from the library
// (state machine over the previous colour) so the two cannot share a bug.
std::vector<uint8_t> pw0_encode(const std::vector<uint8_t> &pixels)
{
    std::vector<uint8_t> out;
    int color = -1;
    size_t run = 0;
    auto flush = [&]() {
        while (run) {
            const bool wide = color == 0 || color == 15;
            const size_t part = std::min<size_t>(run, wide ? 4095 : 15);
            if (wide) { out.push_back(uint8_t((color << 4) | (part >> 8))); out.push_back(uint8_t(part & 0xff)); }
            else out.push_back(uint8_t((color << 4) | part));
            run -= part;
        }
    };
    for (uint8_t value : pixels) {
        if (int(value >> 4) != color) { flush(); color = value >> 4; }
        ++run;
    }
    flush();
    return out;
}

struct ZipFlaws {
    int missing_layer = -1;     // layer image absent
    int vector_layer = -1;      // pwszImg instead of pw0Img
    int short_layer = -1;       // RLE stops before the last pixel
    int overrun_layer = -1;     // last run goes past the raster
    bool bad_settings = false;  // manifest is not JSON
    bool count_mismatch = false;
    bool zero_resolution = false;
    bool no_layer_table = false;
};

std::vector<uint8_t> make_anycubic_zip(const std::string &extension, const std::vector<std::vector<uint8_t>> &masks,
                                       ZipFlaws flaws = {})
{
    TempFile file("build.zip");
    {
        Zipper zip(file.str());
        auto add_text = [&](const std::string &name, const std::string &text) { zip.add_entry(name, text.data(), text.size()); };
        if (flaws.bad_settings) add_text("anycubic_photon_resins.pwsp", "{not json");
        else {
            Json settings = {{"version", "3"}, {"machine_type", {{"name", "Test"}, {"key_suffix", extension},
                {"res_x", flaws.zero_resolution ? 0 : zip_columns}, {"res_y", zip_rows}, {"print_xsize", 50.}, {"print_ysize", 30.}}}};
            add_text("anycubic_photon_resins.pwsp", settings.dump(2));
        }
        add_text("software_info.conf", "{\"mark\":\"Test\"}");
        const std::string preview = "\x89PNG-fake-preview";
        zip.add_entry("preview_images/preview_0.png", preview.data(), preview.size());
        add_text("print_info.json", "{\"volume\":1.5,\"print_time\":99}");
        for (size_t i = 0; i < masks.size(); ++i) {
            if (int(i) == flaws.missing_layer) continue;
            auto pixels = masks[i];
            std::string name = "layer_images/layer_" + std::to_string(i) + ".pw0Img";
            if (int(i) == flaws.short_layer) pixels.resize(pixels.size() / 2);
            if (int(i) == flaws.overrun_layer) { pixels.back() = 255; pixels.insert(pixels.end(), 100, 255); }
            if (int(i) == flaws.vector_layer) name = "layer_images/layer_" + std::to_string(i) + ".pwszImg";
            const auto rle = pw0_encode(pixels);
            zip.add_entry(name, rle.data(), rle.size());
        }
        Json paras = Json::array();
        for (size_t i = 0; i < masks.size(); ++i)
            paras.push_back({{"layer_index", i}, {"exposure_time", 2.0 + double(i)}, {"layer_thickness", 0.05}});
        if (!flaws.no_layer_table)
            add_text("layers_controller.conf", Json{{"count", masks.size() + (flaws.count_mismatch ? 1 : 0)}, {"paras", paras}}.dump());
        else
            add_text("layers_controller.conf", "{\"count\":5}");
        add_text("scene.slice", "SCENE-STATS-NOT-RECOMPUTED");
        add_text("vendor/notes.txt", "keep me");
        zip.finalize();
    }
    return read_file(file.str());
}

} // namespace

TEST_CASE("Anycubic ZIP reopens every pixel and saves byte-identically when unchanged", "[sla_layers][sla_more_readers]")
{
    const std::string extension = GENERATE("pm4u", "pm7", "pm7m", "pwsz");
    CAPTURE(extension);
    const auto masks = test_masks(zip_columns, zip_rows, zip_layers, true);
    const auto bytes = make_anycubic_zip(extension, masks);
    TempFile source("src." + extension), saved("out." + extension);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    REQUIRE(document.archive_format() == extension);
    REQUIRE(document.layer_count() == zip_layers);
    for (size_t i = 0; i < zip_layers; ++i) {
        CAPTURE(i);
        const auto image = document.image(i);
        REQUIRE(image.cols == zip_columns);
        REQUIRE(image.rows == zip_rows);
        CHECK(image.buf == masks[i]);
    }
    CHECK_FALSE(document.dirty());
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == bytes);

    // A document keeps its family: another extension is refused before any write.
    TempFile wrong("out.pwmx");
    CHECK_THROWS_WITH(document.save_archive(wrong.str()), ContainsSubstring("extension"));
    CHECK_FALSE(boost::filesystem::exists(wrong.path));
}

TEST_CASE("Anycubic ZIP edit rewrites only the changed layer image and keeps everything else", "[sla_layers][sla_more_readers]")
{
    const auto masks = test_masks(zip_columns, zip_rows, zip_layers, true);
    const auto bytes = make_anycubic_zip("pm7", masks);
    TempFile source("src.pm7"), edited("out.pm7");
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    // 100 and 255 are not both PW0 levels: the editor must show what the file can hold.
    auto painted = masks[2];
    for (size_t i = 0; i < 400; ++i) painted[i] = (i % 2) ? 100 : 255;
    document.replace_image(2, make_image(zip_columns, zip_rows, painted));
    auto expected = painted;
    for (auto &value : expected) value = uint8_t((value >> 4) * 17);
    CHECK(document.image(2).buf == expected);
    REQUIRE(document.dirty());
    document.save_archive(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.layer_count() == zip_layers);
    for (size_t i = 0; i < zip_layers; ++i) {
        CAPTURE(i);
        CHECK(reopened.image(i).buf == (i == 2 ? expected : masks[i]));
    }

    // Entries other than the edited layer image are carried over verbatim, in order.
    const auto before = zip_entries(source.str()), after = zip_entries(edited.str());
    REQUIRE(after.size() == before.size());
    for (const auto &[name, data] : before) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK((after.at(name) == data) == (name != "layer_images/layer_2.pw0Img"));
    }
    CHECK(after.at("layer_images/layer_2.pw0Img") == pw0_encode(expected));

    // Undo back to the file's own pixels saves the same layer data again.
    REQUIRE(document.undo());
    TempFile restored("restored.pm7");
    document.save_archive(restored.str());
    CHECK(zip_entries(restored.str()) == before);
}

TEST_CASE("Anycubic ZIP files from the real exporter reopen with the exported masks", "[sla_layers][sla_more_readers]")
{
    const std::string format = GENERATE("pm4u", "pm7", "pm7m", "pwsz");
    CAPTURE(format);
    constexpr size_t columns = 64, rows = 48;
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", format);
    cfg.set("display_width", 64.); cfg.set("display_height", 48.);
    cfg.set("display_pixels_x", int(columns)); cfg.set("display_pixels_y", int(rows));
    cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("display_mirror_x", false); cfg.set("display_mirror_y", false);
    cfg.set("layer_height", .05); cfg.set("initial_layer_height", .1);
    cfg.set("max_print_height", 100.);
    cfg.set("exposure_time", 3.12); cfg.set("initial_exposure_time", 9.12);
    cfg.set("sla_bottom_layers", 1); cfg.set("sla_transition_layers", 0);
    // The ZIP schema stores one pre-cure wait and no PWM, after-cure wait or second retract.
    for (const char *prefix : {"sla_", "sla_bottom_"}) {
        cfg.set(std::string(prefix) + "lift_height", 5.); cfg.set(std::string(prefix) + "lift_height2", 0.);
        cfg.set(std::string(prefix) + "lift_speed", 90.); cfg.set(std::string(prefix) + "retract_speed", 150.);
        cfg.set(std::string(prefix) + "retract_height2", 0.); cfg.set(std::string(prefix) + "wait_before_cure", 1.);
        cfg.set(std::string(prefix) + "wait_after_cure", 0.); cfg.set(std::string(prefix) + "wait_after_lift", 0.);
        cfg.set(std::string(prefix) + "light_pwm", 255);
    }
    const auto masks = test_masks(columns, rows, 3, true);
    std::vector<sla::EncodedRaster> encoded;
    for (const auto &mask : masks) encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), columns, rows, 1));
    const auto source_document = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source_document, {.1, .15, .2});
    TempFile exported("exported." + format), edited("edited." + format);
    REQUIRE_NOTHROW(job->export_print(exported.str()));

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == format);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(document.image(i).buf == masks[i]);
    }
    // Unchanged save is the exporter's own file; an edit keeps its metadata entries.
    TempFile same("same." + format);
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));
    document.invert(1);
    document.save_archive(edited.str());
    const auto before = zip_entries(exported.str()), after = zip_entries(edited.str());
    for (const char *name : {"anycubic_photon_resins.pwsp", "layers_controller.conf", "print_info.json", "scene.slice"}) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK(after.at(name) == before.at(name));
    }
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    auto inverted = masks[1];
    for (auto &value : inverted) value = uint8_t(255 - value);
    CHECK(reopened.image(1).buf == inverted);
    CHECK(reopened.image(0).buf == masks[0]);
    CHECK(reopened.image(2).buf == masks[2]);
}

TEST_CASE("Damaged Anycubic ZIP files fail with specific messages", "[sla_layers][sla_more_readers]")
{
    const auto masks = test_masks(zip_columns, zip_rows, zip_layers, true);
    TempFile file("damaged.pm7");
    auto load = [&](const std::vector<uint8_t> &data, const std::string &name = "") {
        if (name.empty()) write_file(file.str(), data);
        else write_file(name, data);
        return sla::LayerDocument::load_archive(name.empty() ? file.str() : name);
    };
    SECTION("layer image missing") {
        ZipFlaws flaws; flaws.missing_layer = 2;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("layer image 2 is missing"));
    }
    SECTION("vector contour layers cannot be edited") {
        ZipFlaws flaws; flaws.vector_layer = 1;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("vector contours"));
    }
    SECTION("RLE ends before the raster is full") {
        ZipFlaws flaws; flaws.short_layer = 3;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("layer 3 ends short"));
    }
    SECTION("RLE run goes past the raster") {
        ZipFlaws flaws; flaws.overrun_layer = 4;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("layer 4 runs past"));
    }
    SECTION("settings are not JSON") {
        ZipFlaws flaws; flaws.bad_settings = true;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("not valid JSON"));
    }
    SECTION("layer count disagrees with the table") {
        ZipFlaws flaws; flaws.count_mismatch = true;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("does not match its layer table"));
    }
    SECTION("layer table without paras") {
        ZipFlaws flaws; flaws.no_layer_table = true;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("paras"));
    }
    SECTION("zero resolution") {
        ZipFlaws flaws; flaws.zero_resolution = true;
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("pm7", masks, flaws)), ContainsSubstring("res_x"));
    }
    SECTION("extension that names no Anycubic ZIP family") {
        TempFile other("other.zip");
        // Neither the extension nor the manifest's key_suffix names a family.
        REQUIRE_THROWS_WITH(load(make_anycubic_zip("xyz", masks), other.str()), ContainsSubstring(".pm7"));
    }
    SECTION("truncated archive") {
        auto bytes = make_anycubic_zip("pm7", masks);
        bytes.resize(bytes.size() / 2);
        REQUIRE_THROWS(load(bytes));
    }
    SECTION("tiny file") {
        REQUIRE_THROWS(load({'P', 'K', 3, 4}));
    }
}

TEST_CASE("Anycubic ZIP loading and saving honour cancellation", "[sla_layers][sla_more_readers]")
{
    const auto masks = test_masks(zip_columns, zip_rows, zip_layers, true);
    TempFile source("src.pm7"), out("out.pm7");
    write_file(source.str(), make_anycubic_zip("pm7", masks));
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(source.str(), [](size_t, size_t) { return false; }),
                      sla::LayerOperationCancelled);

    auto document = sla::LayerDocument::load_archive(source.str());
    document.invert(0);
    REQUIRE_THROWS_AS(document.save_archive(out.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_FALSE(boost::filesystem::exists(out.path));
    CHECK(document.dirty());
}

// ---------------------------------------------------------------------------
// Klipper MSLA ZIP (klipper = gray PNG, klipper_rgb = PNG24)
// ---------------------------------------------------------------------------
namespace {

constexpr size_t klipper_columns = 64, klipper_rows = 48;

std::vector<uint8_t> png_bytes(const std::vector<uint8_t> &pixels, size_t columns, size_t rows, bool rgb)
{
    sla::EncodedRaster encoded;
    if (!rgb) encoded = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
    else {
        std::vector<uint8_t> color(pixels.size() * 3);
        for (size_t i = 0; i < pixels.size(); ++i) color[3 * i] = color[3 * i + 1] = color[3 * i + 2] = pixels[i];
        encoded = sla::PNGRasterEncoder{}(color.data(), columns, rows, 3);
    }
    const auto *data = static_cast<const uint8_t *>(encoded.data());
    return {data, data + encoded.size()};
}

struct KlipperFlaws {
    int missing_layer = -1;       // 1-based image absent
    bool second_script = false;
    bool no_marker = false;
    int header_columns = -1;      // ResolutionX in the script
    int header_count = -1;        // LayerCount in the script
    int text_layer = -1;          // 0-based layer that is not a PNG
    int resized_layer = -1;       // 0-based layer with other dimensions
    bool extra_layer = false;     // image beyond the declared count
};

std::vector<uint8_t> make_klipper_zip(const std::vector<std::vector<uint8_t>> &masks, bool rgb, KlipperFlaws flaws = {})
{
    TempFile file("build.zip");
    {
        Zipper zip(file.str(), Zipper::NO_COMPRESSION);
        if (!flaws.no_marker) zip.add_entry(".klipper", "", 0);
        std::string script = ";ResolutionX:" + std::to_string(flaws.header_columns >= 0 ? flaws.header_columns : int(klipper_columns)) +
            "\n;ResolutionY:" + std::to_string(klipper_rows) + "\n;LayerCount:" +
            std::to_string(flaws.header_count >= 0 ? flaws.header_count : int(masks.size())) +
            "\nMSLA_DISPLAY_VALIDATE RESOLUTION=64,48 PIXEL=1,1\nPRINT_START LAYERS=" + std::to_string(masks.size()) + "\n";
        for (size_t i = 0; i < masks.size(); ++i) script += "M1451 F\"" + std::to_string(i + 1) + ".png\"\n";
        zip.add_entry("job.gcode", script.data(), script.size());
        if (flaws.second_script) zip.add_entry("other.gcode", script.data(), script.size());
        const std::string preview = "preview-bytes";
        zip.add_entry("preview.png", preview.data(), preview.size());
        for (size_t i = 0; i < masks.size(); ++i) {
            if (int(i) + 1 == flaws.missing_layer) continue;
            auto bytes = png_bytes(masks[i], klipper_columns, klipper_rows, rgb);
            if (int(i) == flaws.text_layer) bytes = text_bytes("this is not a png file at all, just some long enough text");
            if (int(i) == flaws.resized_layer) bytes = png_bytes(std::vector<uint8_t>(32 * 48, 9), 32, 48, rgb);
            zip.add_entry(std::to_string(i + 1) + ".png", bytes.data(), bytes.size());
        }
        if (flaws.extra_layer) {
            const auto bytes = png_bytes(masks[0], klipper_columns, klipper_rows, rgb);
            zip.add_entry(std::to_string(masks.size() + 3) + ".png", bytes.data(), bytes.size());
        }
        zip.finalize();
    }
    return read_file(file.str());
}

bool entries_uncompressed(const std::string &path)
{
    MZ_Archive archive;
    REQUIRE(open_zip_reader(&archive.arch, path));
    bool stored = true;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive.arch); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&archive.arch, i, &stat));
        stored = stored && stat.m_method == 0;
    }
    close_zip_reader(&archive.arch);
    return stored;
}

} // namespace

TEST_CASE("Klipper ZIP reopens every pixel and saves byte-identically when unchanged", "[sla_layers][sla_more_readers]")
{
    const bool rgb = GENERATE(false, true);
    CAPTURE(rgb);
    const auto masks = test_masks(klipper_columns, klipper_rows, 5, false);
    const auto bytes = make_klipper_zip(masks, rgb);
    const std::string suffix = rgb ? "rgb.zip" : "zip";
    TempFile source("src." + suffix), saved("out." + suffix);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    REQUIRE(document.archive_format() == (rgb ? "klipper_rgb" : "klipper"));
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        const auto image = document.image(i);
        REQUIRE(image.cols == klipper_columns);
        REQUIRE(image.rows == klipper_rows);
        CHECK(image.buf == masks[i]); // full eight-bit values, unlike PW0
    }
    CHECK_FALSE(document.dirty());
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == bytes);

    // Mono and RGB are different families: the other family's extension is refused.
    TempFile wrong(rgb ? "out.zip" : "out.rgb.zip");
    CHECK_THROWS_WITH(document.save_archive(wrong.str()), ContainsSubstring("extension"));
    CHECK_FALSE(boost::filesystem::exists(wrong.path));
}

TEST_CASE("Klipper ZIP edit rewrites only the changed layer image and keeps the script and marker", "[sla_layers][sla_more_readers]")
{
    const bool rgb = GENERATE(false, true);
    CAPTURE(rgb);
    const auto masks = test_masks(klipper_columns, klipper_rows, 5, false);
    const auto bytes = make_klipper_zip(masks, rgb);
    const std::string suffix = rgb ? "rgb.zip" : "zip";
    TempFile source("src." + suffix), edited("out." + suffix);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    auto painted = masks[2];
    for (size_t i = 0; i < 300; ++i) painted[i] = uint8_t(i % 251);
    document.replace_image(2, make_image(klipper_columns, klipper_rows, painted));
    CHECK(document.image(2).buf == painted); // no quantisation for 8-bit gray PNG
    document.save_archive(edited.str());
    CHECK(entries_uncompressed(edited.str())); // the writer's storage method survives an edit

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.archive_format() == (rgb ? "klipper_rgb" : "klipper"));
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(reopened.image(i).buf == (i == 2 ? painted : masks[i]));
    }
    const auto before = zip_entries(source.str()), after = zip_entries(edited.str());
    REQUIRE(after.size() == before.size());
    for (const auto &[name, data] : before) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK((after.at(name) == data) == (name != "3.png"));
    }
    CHECK(after.at("3.png") == png_bytes(painted, klipper_columns, klipper_rows, rgb));
    CHECK(after.at(".klipper").empty());
}

TEST_CASE("Klipper ZIP files from the real exporter reopen with the exported masks", "[sla_layers][sla_more_readers]")
{
    const bool rgb = GENERATE(false, true);
    CAPTURE(rgb);
    const std::string format = rgb ? "klipper_rgb" : "klipper", suffix = rgb ? "rgb.zip" : "zip";
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", format);
    cfg.set("printer_model", std::string("Klipper Test"));
    cfg.set("display_width", 64.); cfg.set("display_height", 48.);
    cfg.set("display_pixels_x", int(klipper_columns)); cfg.set("display_pixels_y", int(klipper_rows));
    cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("display_mirror_x", false); cfg.set("display_mirror_y", false);
    cfg.set("max_print_height", 200.);
    cfg.set("layer_height", .05); cfg.set("initial_layer_height", .1);
    cfg.set("sla_bottom_layers", 1); cfg.set("sla_transition_layers", 1);
    cfg.set("initial_exposure_time", 30.); cfg.set("exposure_time", 3.);
    cfg.set("sla_bottom_lift_height", 5.); cfg.set("sla_lift_height", 4.);
    cfg.set("sla_bottom_lift_height2", 0.); cfg.set("sla_lift_height2", 0.);
    cfg.set("sla_bottom_lift_speed", 60.); cfg.set("sla_lift_speed", 90.);
    cfg.set("sla_bottom_retract_speed", 120.); cfg.set("sla_retract_speed", 150.);
    cfg.set("sla_bottom_retract_height2", 0.); cfg.set("sla_retract_height2", 0.);
    cfg.set("sla_bottom_wait_before_cure", 2.5); cfg.set("sla_wait_before_cure", 1.25);
    cfg.set("sla_bottom_wait_after_cure", .3); cfg.set("sla_wait_after_cure", .2);
    cfg.set("sla_bottom_wait_after_lift", .75); cfg.set("sla_wait_after_lift", .5);
    cfg.set("sla_bottom_light_pwm", 200); cfg.set("sla_light_pwm", 180);
    cfg.set("material_density", 1.1); cfg.set("bottle_volume", 500.); cfg.set("bottle_cost", 50.);
    const auto masks = test_masks(klipper_columns, klipper_rows, 3, false);
    std::vector<sla::EncodedRaster> encoded;
    for (const auto &mask : masks) encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), klipper_columns, klipper_rows, 1));
    const auto source_document = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source_document, {.1, .15, .2});
    TempFile exported("exported." + suffix), same("same." + suffix), edited("edited." + suffix);
    REQUIRE_NOTHROW(job->export_print(exported.str()));

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == format);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(document.image(i).buf == masks[i]);
    }
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));
    document.invert(0);
    document.save_archive(edited.str());
    const auto before = zip_entries(exported.str()), after = zip_entries(edited.str());
    REQUIRE(after.count("job.gcode") == 1);
    // A mask edit refreshes the mask-derived statistics in the script (per-layer
    // volume/material/bounds and the totals), but must not touch motion, Z or exposure.
    check_only_statistics_changed(before.at("job.gcode"), after.at("job.gcode"));
    CHECK(after.count(".klipper") == 1);
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    auto inverted = masks[0];
    for (auto &value : inverted) value = uint8_t(255 - value);
    CHECK(reopened.image(0).buf == inverted);
    CHECK(reopened.image(1).buf == masks[1]);
    CHECK(reopened.image(2).buf == masks[2]);
}

TEST_CASE("Damaged Klipper ZIP files fail with specific messages", "[sla_layers][sla_more_readers]")
{
    const auto masks = test_masks(klipper_columns, klipper_rows, 4, false);
    TempFile file("damaged.zip");
    auto load = [&](const std::vector<uint8_t> &data) { write_file(file.str(), data); return sla::LayerDocument::load_archive(file.str()); };
    SECTION("layer image missing") {
        KlipperFlaws flaws; flaws.missing_layer = 3;
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, false, flaws)), ContainsSubstring("layer image 3 is missing"));
    }
    SECTION("two G-code files") {
        KlipperFlaws flaws; flaws.second_script = true;
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, false, flaws)), ContainsSubstring("several G-code files"));
    }
    SECTION("script resolution disagrees with the images") {
        KlipperFlaws flaws; flaws.header_columns = 128;
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, false, flaws)), ContainsSubstring("declares 128x48"));
    }
    SECTION("script declares fewer layers than there are images") {
        KlipperFlaws flaws; flaws.header_count = 3;
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, false, flaws)), ContainsSubstring("beyond its declared 3 layers"));
    }
    SECTION("script declares more layers than there are images") {
        KlipperFlaws flaws; flaws.header_count = 6;
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, false, flaws)), ContainsSubstring("layer image 5 is missing"));
    }
    SECTION("a layer is not a PNG") {
        KlipperFlaws flaws; flaws.text_layer = 2;
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, false, flaws)), ContainsSubstring("layer 3 is not a PNG"));
    }
    SECTION("a layer has other dimensions") {
        KlipperFlaws flaws; flaws.resized_layer = 1;
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, false, flaws)), ContainsSubstring("layer 2 differs in size"));
    }
    SECTION("packed RGB anti-aliasing") {
        KlipperFlaws flaws; flaws.header_columns = int(klipper_columns * 3);
        REQUIRE_THROWS_WITH(load(make_klipper_zip(masks, true, flaws)), ContainsSubstring("packed RGB"));
    }
    SECTION("no layer images at all") {
        REQUIRE_THROWS_WITH(load(make_klipper_zip({}, false)), ContainsSubstring("no layer images"));
    }
    SECTION("without the marker it is not a Klipper file") {
        KlipperFlaws flaws; flaws.no_marker = true;
        REQUIRE_THROWS(load(make_klipper_zip(masks, false, flaws)));
    }
    SECTION("truncated archive") {
        auto bytes = make_klipper_zip(masks, false);
        bytes.resize(bytes.size() / 2);
        REQUIRE_THROWS(load(bytes));
    }
}

TEST_CASE("Klipper ZIP loading and saving honour cancellation", "[sla_layers][sla_more_readers]")
{
    const auto masks = test_masks(klipper_columns, klipper_rows, 5, false);
    TempFile source("src.zip"), out("out.zip");
    write_file(source.str(), make_klipper_zip(masks, false));
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(source.str(), [](size_t, size_t) { return false; }),
                      sla::LayerOperationCancelled);
    auto document = sla::LayerDocument::load_archive(source.str());
    document.invert(0);
    REQUIRE_THROWS_AS(document.save_archive(out.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_FALSE(boost::filesystem::exists(out.path));
    CHECK(document.dirty());
}

// ---------------------------------------------------------------------------
// NanoDLP plate ZIP (nanodlp, nanodlp_v1, nanodlp_rgb, nanodlp_rgb_v1)
// ---------------------------------------------------------------------------
namespace {

constexpr size_t nano_rows = 24;

enum class NanoShape { Gray, Packed, FullRgb };

// Test-local layer PNG encoder; the packed form reshapes bytes (no colour conversion).
std::vector<uint8_t> nano_png(const std::vector<uint8_t> &pixels, size_t columns, size_t rows, NanoShape shape)
{
    sla::EncodedRaster encoded;
    if (shape == NanoShape::Gray) encoded = sla::PNGRasterEncoder{}(pixels.data(), columns, rows, 1);
    else if (shape == NanoShape::Packed) encoded = sla::PNGRasterEncoder{}(pixels.data(), columns / 3, rows, 3);
    else {
        std::vector<uint8_t> color(pixels.size() * 3);
        for (size_t i = 0; i < pixels.size(); ++i) color[3 * i] = color[3 * i + 1] = color[3 * i + 2] = pixels[i];
        encoded = sla::PNGRasterEncoder{}(color.data(), columns, rows, 3);
    }
    const auto *data = static_cast<const uint8_t *>(encoded.data());
    return {data, data + encoded.size()};
}

struct NanoFlaws {
    int format_version = 2;
    int missing_layer = -1;      // 1-based
    int layers_count = -1;       // plate.json LayersCount override
    int slicer_width = -1;       // PWidth override
    bool no_slicer = false;
    bool bad_meta = false;
    int text_layer = -1;         // 0-based
    int resized_layer = -1;      // 0-based
};

std::vector<uint8_t> make_nanodlp_zip(const std::vector<std::vector<uint8_t>> &masks, size_t columns, NanoShape shape,
                                      NanoFlaws flaws = {})
{
    TempFile file("build.zip");
    {
        Zipper zip(file.str());
        auto add_text = [&](const std::string &name, const std::string &text) { zip.add_entry(name, text.data(), text.size()); };
        add_text("meta.json", flaws.bad_meta ? "{broken" : Json{{"FormatVersion", flaws.format_version}, {"Program", "Test"}}.dump(2));
        if (!flaws.no_slicer)
            add_text("slicer.json", Json{{"PWidth", flaws.slicer_width >= 0 ? flaws.slicer_width : int(columns)},
                                         {"PHeight", int(nano_rows)}, {"Thickness", 50}}.dump(2));
        add_text("plate.json", Json{{"LayersCount", flaws.layers_count >= 0 ? flaws.layers_count : int(masks.size())}}.dump(2));
        add_text("profile.json", "{\"Title\":\"Test\",\"CureTime\":2.5}");
        add_text("info.json", "[{\"TotalSolidArea\":1.5},{\"TotalSolidArea\":2.5}]");
        add_text("3d.png", "preview-bytes");
        for (size_t i = 0; i < masks.size(); ++i) {
            if (int(i) + 1 == flaws.missing_layer) continue;
            auto bytes = nano_png(masks[i], columns, nano_rows, shape);
            if (int(i) == flaws.text_layer) bytes = text_bytes("this is not a png file at all, just some long enough text");
            if (int(i) == flaws.resized_layer)
                bytes = nano_png(std::vector<uint8_t>(shape == NanoShape::Packed ? 30 * nano_rows : 10 * nano_rows, 9),
                                 shape == NanoShape::Packed ? 30 : 10, nano_rows, shape);
            zip.add_entry(std::to_string(i + 1) + ".png", bytes.data(), bytes.size());
        }
        zip.finalize();
    }
    return read_file(file.str());
}

const char *nano_format(NanoShape shape, int version)
{
    if (shape == NanoShape::FullRgb) return version == 1 ? "nanodlp_rgb_v1" : "nanodlp_rgb";
    return version == 1 ? "nanodlp_v1" : "nanodlp";
}
std::string nano_suffix(NanoShape shape, int version)
{
    return std::string(version == 1 ? "v1." : "") + (shape == NanoShape::FullRgb ? "rgb." : "") + "nanodlp";
}

} // namespace

TEST_CASE("NanoDLP reopens every pixel in all three layer shapes and saves byte-identically when unchanged",
          "[sla_layers][sla_more_readers]")
{
    const NanoShape shape = GENERATE(NanoShape::Gray, NanoShape::Packed, NanoShape::FullRgb);
    const int version = GENERATE(1, 2);
    const size_t columns = shape == NanoShape::Packed ? 66 : 64; // packed needs a multiple of three
    CAPTURE(int(shape), version);
    const auto masks = test_masks(columns, nano_rows, 5, false);
    const auto bytes = make_nanodlp_zip(masks, columns, shape, NanoFlaws{version});
    const auto suffix = nano_suffix(shape, version);
    TempFile source("src." + suffix), saved("out." + suffix);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    REQUIRE(document.archive_format() == nano_format(shape, version));
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        const auto image = document.image(i);
        REQUIRE(image.cols == columns);
        REQUIRE(image.rows == nano_rows);
        CHECK(image.buf == masks[i]); // a packed mask must come back in the original pixel order
    }
    CHECK_FALSE(document.dirty());
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == bytes);

    // The other family's suffix (shared ".nanodlp" tail) is refused before any write.
    TempFile wrong("out." + nano_suffix(shape == NanoShape::FullRgb ? NanoShape::Gray : NanoShape::FullRgb, version));
    CHECK_THROWS_WITH(document.save_archive(wrong.str()), ContainsSubstring("extension"));
    CHECK_FALSE(boost::filesystem::exists(wrong.path));
}

TEST_CASE("NanoDLP edit rewrites only the changed layer image in the file's own shape", "[sla_layers][sla_more_readers]")
{
    const NanoShape shape = GENERATE(NanoShape::Gray, NanoShape::Packed, NanoShape::FullRgb);
    const size_t columns = shape == NanoShape::Packed ? 66 : 64;
    CAPTURE(int(shape));
    const auto masks = test_masks(columns, nano_rows, 5, false);
    const auto bytes = make_nanodlp_zip(masks, columns, shape);
    const auto suffix = nano_suffix(shape, 2);
    TempFile source("src." + suffix), edited("out." + suffix);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    auto painted = masks[1];
    for (size_t i = 0; i < 200; ++i) painted[i] = uint8_t((i * 5) % 256);
    document.replace_image(1, make_image(columns, nano_rows, painted));
    document.save_archive(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.archive_format() == nano_format(shape, 2));
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(reopened.image(i).buf == (i == 1 ? painted : masks[i]));
    }
    const auto before = zip_entries(source.str()), after = zip_entries(edited.str());
    REQUIRE(after.size() == before.size());
    for (const auto &[name, data] : before) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK((after.at(name) == data) == (name != "2.png"));
    }
    CHECK(after.at("2.png") == nano_png(painted, columns, nano_rows, shape));
}

TEST_CASE("NanoDLP files from the real exporter reopen with the exported masks", "[sla_layers][sla_more_readers]")
{
    const std::string format = GENERATE("nanodlp", "nanodlp_v1", "nanodlp_rgb", "nanodlp_rgb_v1");
    const int columns = GENERATE(200, 201); // 201 is divisible by three: the mono writer packs it
    CAPTURE(format, columns);
    const int rows = 120;
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, format);
    full.set("display_pixels_x", columns); full.set("display_pixels_y", rows);
    DynamicPrintConfig cfg; cfg.apply(full);
    const auto masks = test_masks(size_t(columns), size_t(rows), 3, false);
    std::vector<sla::EncodedRaster> encoded;
    for (const auto &mask : masks) encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), size_t(columns), size_t(rows), 1));
    const auto source_document = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source_document, {.05, .1, .15});
    const std::string suffix = std::string(format.find("_v1") != std::string::npos ? "v1." : "") +
                               (format.find("rgb") != std::string::npos ? "rgb." : "") + "nanodlp";
    TempFile exported("exported." + suffix), same("same." + suffix), edited("edited." + suffix);
    REQUIRE_NOTHROW(job->export_print(exported.str()));

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == format);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(document.image(i).buf == masks[i]);
    }
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));
    document.invert(2);
    document.save_archive(edited.str());
    const auto before = zip_entries(exported.str()), after = zip_entries(edited.str());
    for (const char *name : {"meta.json", "slicer.json", "plate.json", "profile.json", "info.json"}) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK(after.at(name) == before.at(name));
    }
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    auto inverted = masks[2];
    for (auto &value : inverted) value = uint8_t(255 - value);
    CHECK(reopened.image(2).buf == inverted);
    CHECK(reopened.image(0).buf == masks[0]);
}

TEST_CASE("Damaged NanoDLP files fail with specific messages", "[sla_layers][sla_more_readers]")
{
    const auto masks = test_masks(64, nano_rows, 4, false);
    TempFile file("damaged.nanodlp");
    auto load = [&](const std::vector<uint8_t> &data) { write_file(file.str(), data); return sla::LayerDocument::load_archive(file.str()); };
    SECTION("layer image missing") {
        NanoFlaws flaws; flaws.missing_layer = 3;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("layer image 3 is missing"));
    }
    SECTION("unsupported FormatVersion") {
        NanoFlaws flaws; flaws.format_version = 3;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("FormatVersion"));
    }
    SECTION("meta.json is not JSON") {
        NanoFlaws flaws; flaws.bad_meta = true;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("meta.json is not valid JSON"));
    }
    SECTION("plate declares fewer layers than there are images") {
        NanoFlaws flaws; flaws.layers_count = 3;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("beyond its declared 3 layers"));
    }
    SECTION("plate declares more layers than there are images") {
        NanoFlaws flaws; flaws.layers_count = 6;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("layer image 5 is missing"));
    }
    SECTION("slicer.json width disagrees with the images") {
        NanoFlaws flaws; flaws.slicer_width = 128;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("slicer.json declares 128x24"));
    }
    SECTION("a layer is not a PNG") {
        NanoFlaws flaws; flaws.text_layer = 2;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("layer 3 is not a PNG"));
    }
    SECTION("a layer has other dimensions") {
        NanoFlaws flaws; flaws.resized_layer = 1;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(masks, 64, NanoShape::Gray, flaws)), ContainsSubstring("layer 2 differs in size"));
    }
    SECTION("a packed layer has other dimensions") {
        NanoFlaws flaws; flaws.resized_layer = 3;
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip(test_masks(66, nano_rows, 4, false), 66, NanoShape::Packed, flaws)),
                            ContainsSubstring("layer 4 differs in size"));
    }
    SECTION("no layer images at all") {
        REQUIRE_THROWS_WITH(load(make_nanodlp_zip({}, 64, NanoShape::Gray)), ContainsSubstring("no layer images"));
    }
    SECTION("truncated archive") {
        auto bytes = make_nanodlp_zip(masks, 64, NanoShape::Gray);
        bytes.resize(bytes.size() / 2);
        REQUIRE_THROWS(load(bytes));
    }
}

TEST_CASE("NanoDLP loading and saving honour cancellation", "[sla_layers][sla_more_readers]")
{
    const auto masks = test_masks(64, nano_rows, 5, false);
    TempFile source("src.nanodlp"), out("out.nanodlp");
    write_file(source.str(), make_nanodlp_zip(masks, 64, NanoShape::Gray));
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(source.str(), [](size_t, size_t) { return false; }),
                      sla::LayerOperationCancelled);
    auto document = sla::LayerDocument::load_archive(source.str());
    document.invert(0);
    REQUIRE_THROWS_AS(document.save_archive(out.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_FALSE(boost::filesystem::exists(out.path));
    CHECK(document.dirty());
}

// ---------------------------------------------------------------------------
// Phrozen PHZ v2 and Voxelab FDG v2 (one codec, different 216-byte headers)
// ---------------------------------------------------------------------------
namespace {

constexpr size_t phz_columns = 40, phz_rows = 16, phz_header = 216, phz_record = 36;
constexpr size_t phz_blob = 64;

// Header offsets written from PHZFile.cs / FDGFile.cs, not from the reader.
struct PhrozenSpec {
    const char *id;
    uint32_t magic;
    size_t count, table, key, volume;
};
const PhrozenSpec &phrozen_spec(bool fdg)
{
    static const PhrozenSpec phz{"phz", 0x9FDA83AEu, 40, 36, 88, 128};
    static const PhrozenSpec fdg_spec{"fdg", 0xBD3C7AC8u, 8, 52, 92, 104};
    return fdg ? fdg_spec : phz;
}

void put32le(std::vector<uint8_t> &b, size_t at, uint32_t v) { for (unsigned i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
void put_float_le(std::vector<uint8_t> &b, size_t at, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32le(b, at, v); }
uint32_t get32le(const std::vector<uint8_t> &b, size_t at) { return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24); }
float get_float_le(const std::vector<uint8_t> &b, size_t at) { const uint32_t v = get32le(b, at); float f; std::memcpy(&f, &v, 4); return f; }

// Gray value as stored by the 7-bit RLE and read back (white is 0xf8).
uint8_t phz_stored(uint8_t value) { const unsigned g = std::min<unsigned>(value >> 1, 0x7c); return uint8_t((g << 1) | (g & 1)); }

// Test-local RLE: pixel runs may not cross the half-width column, 125 pixels per count byte.
std::vector<uint8_t> phz_rle(const std::vector<uint8_t> &pixels, size_t columns, size_t rows)
{
    std::vector<uint8_t> out;
    auto gray = [&](size_t index) { return std::min<unsigned>(pixels[index] >> 1, 0x7c); };
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < columns;) {
            const size_t stop = x < columns / 2 ? columns / 2 : columns;
            size_t length = 1;
            while (x + length < stop && gray(y * columns + x + length) == gray(y * columns + x)) ++length;
            out.push_back(uint8_t(gray(y * columns + x) | 0x80));
            for (size_t rest = length - 1; rest;) { const size_t part = std::min<size_t>(rest, 125); out.push_back(uint8_t(part)); rest -= part; }
            x += length;
        }
    return out;
}

// FDGFile.cs LayerRleCryptBuffer.
void fdg_xor(std::vector<uint8_t> &bytes, uint32_t seed, uint32_t layer)
{
    if (!seed) return;
    const uint32_t step = (seed - 0x1dcb76c3u) ^ 0x257e2431u;
    uint32_t key = step * 0x82391efdu * (layer ^ 0x110bdacdu);
    unsigned shift = 0;
    for (auto &byte : bytes) {
        byte ^= uint8_t(key >> (8 * shift));
        if (++shift == 4) { key += step; shift = 0; }
    }
}

// PHZFile.cs LayerRleCryptBuffer.
void phz_xor(std::vector<uint8_t> &bytes, uint32_t seed, uint32_t layer)
{
    if (!seed) return;
    seed %= 0x4324u;
    const uint32_t step = seed * 0x34a32231u;
    uint32_t key = (layer ^ 0x3fad2212u) * seed * 0x4910913du;
    unsigned shift = 0;
    for (auto &byte : bytes) {
        byte ^= uint8_t(key >> (8 * shift));
        if (++shift == 4) { key += step; shift = 0; }
    }
}

struct PhzFlaws {
    int overrun_layer = -1;  // extra pixels after a full raster
    int short_layer = -1;    // RLE stops early
    int payload_past_end = -1;
    int empty_layer = -1;
    bool zero_resolution = false;
    uint32_t count_override = 0;
    uint32_t table_override = 0;
};

// Layers 0 and 2 of `masks` should be equal to exercise the shared-payload case (unencrypted only).
std::vector<uint8_t> make_phz(const std::vector<std::vector<uint8_t>> &masks, uint32_t seed, PhzFlaws flaws = {}, bool fdg = false)
{
    const auto &spec = phrozen_spec(fdg);
    std::vector<uint8_t> file(phz_header, 0);
    put32le(file, 0, spec.magic); put32le(file, 4, 2);
    put32le(file, 24, flaws.zero_resolution ? 0 : phz_columns); put32le(file, 28, phz_rows);
    put32le(file, spec.count, flaws.count_override ? flaws.count_override : uint32_t(masks.size()));
    put32le(file, spec.key, seed);
    put_float_le(file, spec.volume, 10.f); put_float_le(file, spec.volume + 4, 11.f); put_float_le(file, spec.volume + 8, 12.f);
    for (size_t i = 0; i < phz_blob; ++i) file.push_back(uint8_t(0xA0 + i)); // stands in for previews/model name; must survive
    const size_t table = file.size();
    put32le(file, spec.table, flaws.table_override ? flaws.table_override : uint32_t(table));
    file.resize(table + phz_record * masks.size());
    std::vector<std::vector<uint8_t>> payloads;
    for (size_t i = 0; i < masks.size(); ++i) {
        auto pixels = masks[i];
        if (int(i) == flaws.overrun_layer) pixels.insert(pixels.end(), 50, 255);
        if (int(i) == flaws.short_layer) pixels.resize(pixels.size() - 40);
        auto rle = (int(i) == flaws.overrun_layer || int(i) == flaws.short_layer)
            ? phz_rle(pixels, pixels.size() / phz_rows, phz_rows) : phz_rle(pixels, phz_columns, phz_rows);
        if (int(i) == flaws.overrun_layer) rle.insert(rle.end(), {uint8_t(0x80 | 0x20), 5}); // legal runs beyond the raster
        if (int(i) == flaws.empty_layer) rle.clear();
        if (fdg) fdg_xor(rle, seed, uint32_t(i)); else phz_xor(rle, seed, uint32_t(i));
        payloads.push_back(rle);
    }
    for (size_t i = 0; i < masks.size(); ++i) {
        const size_t record = table + phz_record * i;
        put_float_le(file, record, 0.05f * float(i + 1)); put_float_le(file, record + 4, 2.f + float(i));
        put_float_le(file, record + 8, 1.f);
        if (fdg) put32le(file, record + 24, 84);
        // The exporter shares identical unencrypted payloads between layers.
        size_t shared = i;
        if (!seed) for (size_t j = 0; j < i; ++j) if (payloads[j] == payloads[i]) { shared = j; break; }
        if (shared == i) {
            put32le(file, record + 12, uint32_t(file.size())); put32le(file, record + 16, uint32_t(payloads[i].size()));
            file.insert(file.end(), payloads[i].begin(), payloads[i].end());
        } else {
            const size_t other = table + phz_record * shared;
            put32le(file, record + 12, get32le(file, other + 12)); put32le(file, record + 16, get32le(file, other + 16));
        }
    }
    if (flaws.payload_past_end >= 0) {
        // A plausible size whose block starts a few bytes before the end of the file.
        const size_t record = table + phz_record * size_t(flaws.payload_past_end);
        put32le(file, record + 12, uint32_t(file.size() - 5)); put32le(file, record + 16, 100);
    }
    return file;
}

std::vector<std::vector<uint8_t>> phz_masks(size_t count)
{
    auto masks = test_masks(phz_columns, phz_rows, count, false);
    if (count > 2) masks[2] = masks[0]; // identical layers share one payload
    return masks;
}
std::vector<uint8_t> stored(const std::vector<uint8_t> &mask)
{
    std::vector<uint8_t> result = mask;
    for (auto &value : result) value = phz_stored(value);
    return result;
}

} // namespace

TEST_CASE("PHZ and FDG reopen every pixel and save byte-identically when unchanged", "[sla_layers][sla_more_readers]")
{
    const bool fdg = GENERATE(false, true);
    const uint32_t seed = GENERATE(0u, 0x12345u, 0xFFFFFFFFu);
    CAPTURE(fdg, seed);
    const std::string ext = phrozen_spec(fdg).id;
    const auto masks = phz_masks(5);
    const auto bytes = make_phz(masks, seed, {}, fdg);
    TempFile source("src." + ext), saved("out." + ext);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    REQUIRE(document.archive_format() == ext);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        const auto image = document.image(i);
        REQUIRE(image.cols == phz_columns);
        REQUIRE(image.rows == phz_rows);
        CHECK(image.buf == stored(masks[i]));
    }
    CHECK_FALSE(document.dirty());
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == bytes);

    // PHZ and FDG share a codec but not a container: the other's extension is refused.
    TempFile wrong(fdg ? "out.phz" : "out.fdg");
    CHECK_THROWS_WITH(document.save_archive(wrong.str()), ContainsSubstring("extension"));
    CHECK_FALSE(boost::filesystem::exists(wrong.path));
}

TEST_CASE("PHZ and FDG edit appends only the changed layer, keeps the rest and rescales material", "[sla_layers][sla_more_readers]")
{
    const bool fdg = GENERATE(false, true);
    const uint32_t seed = GENERATE(0u, 0x12345u);
    CAPTURE(fdg, seed);
    const auto &spec = phrozen_spec(fdg);
    const std::string ext = spec.id;
    const auto masks = phz_masks(5);
    const auto bytes = make_phz(masks, seed, {}, fdg);
    TempFile source("src." + ext), edited("out." + ext), restored("restored." + ext);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    // Layer 0 shares its payload with layer 2 when the file is unencrypted.
    auto painted = stored(masks[0]);
    for (size_t i = 0; i < 100; ++i) painted[i] = 0xF8;
    document.replace_image(0, make_image(phz_columns, phz_rows, painted));
    CHECK(document.image(0).buf == painted);
    document.save_archive(edited.str());
    const auto out = read_file(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(reopened.image(i).buf == (i == 0 ? painted : stored(masks[i])));
    }
    // Header (except the three material numbers), preview blob and all other records are untouched.
    REQUIRE(out.size() > bytes.size());
    CHECK(std::equal(bytes.begin(), bytes.begin() + spec.volume, out.begin()));
    CHECK(std::equal(bytes.begin() + spec.volume + 12, bytes.begin() + phz_header + phz_blob, out.begin() + spec.volume + 12));
    const size_t table = phz_header + phz_blob;
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        const bool same = std::equal(bytes.begin() + table + phz_record * i, bytes.begin() + table + phz_record * (i + 1),
                                     out.begin() + table + phz_record * i);
        CHECK(same == (i != 0));
    }
    // The old payload bytes remain (layer 2 still points at them).
    CHECK(std::equal(bytes.begin() + table + phz_record * masks.size(), bytes.end(), out.begin() + table + phz_record * masks.size()));
    // Volume follows the thickness-weighted lit pixel count.
    auto lit = [&](const std::vector<std::vector<uint8_t>> &m) {
        double total = 0;
        for (size_t i = 0; i < m.size(); ++i)
            total += double(std::count_if(m[i].begin(), m[i].end(), [](uint8_t p) { return p != 0; })) * 0.05;
        return total;
    };
    std::vector<std::vector<uint8_t>> before, after;
    for (const auto &m : masks) { before.push_back(stored(m)); after.push_back(stored(m)); }
    after[0] = painted;
    const double ratio = lit(after) / lit(before);
    CHECK(ratio != Catch::Approx(1.).margin(1e-6));
    CHECK(get_float_le(out, spec.volume) == Catch::Approx(10.f * ratio).epsilon(1e-4));
    CHECK(get_float_le(out, spec.volume + 4) == Catch::Approx(11.f * ratio).epsilon(1e-4));
    CHECK(get_float_le(out, spec.volume + 8) == Catch::Approx(12.f * ratio).epsilon(1e-4));

    // Undo: no layer differs from the file any more, so the original bytes come back.
    REQUIRE(document.undo());
    document.save_archive(restored.str());
    CHECK(read_file(restored.str()) == bytes);
}

TEST_CASE("PHZ and FDG store white as 0xf8 and the editor shows what will be saved", "[sla_layers][sla_more_readers]")
{
    const bool fdg = GENERATE(false, true);
    const std::string ext = phrozen_spec(fdg).id;
    const auto masks = phz_masks(3);
    TempFile source("src." + ext), edited("out." + ext);
    write_file(source.str(), make_phz(masks, 0, {}, fdg));
    auto document = sla::LayerDocument::load_archive(source.str());
    document.replace_image(1, make_image(phz_columns, phz_rows, std::vector<uint8_t>(phz_columns * phz_rows, 255)));
    CHECK(document.image(1).buf == std::vector<uint8_t>(phz_columns * phz_rows, 0xF8));
    document.save_archive(edited.str());
    CHECK(sla::LayerDocument::load_archive(edited.str()).image(1).buf == std::vector<uint8_t>(phz_columns * phz_rows, 0xF8));
}

TEST_CASE("PHZ and FDG files from the real exporter reopen with the exported masks", "[sla_layers][sla_more_readers]")
{
    const bool fdg = GENERATE(false, true);
    const std::string seed = GENERATE(std::string("0"), std::string("0x1234"));
    CAPTURE(fdg, seed);
    const std::string ext = phrozen_spec(fdg).id;
    constexpr size_t columns = 200, rows = 120;
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, ext);
    if (fdg) full.set("display_mirror_y", false); // FDG cannot describe a vertical mirror
    else full.set("sla_phz_encryption_key", seed);
    DynamicPrintConfig cfg; cfg.apply(full);
    auto masks = test_masks(columns, rows, 3, false);
    std::vector<sla::EncodedRaster> encoded;
    for (auto &mask : masks) {
        for (auto &value : mask) value = phz_stored(value); // the exporter quantises identically
        encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), columns, rows, 1));
    }
    const auto source_document = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source_document, {.05, .1, .15});
    TempFile exported("exported." + ext), same("same." + ext), edited("edited." + ext);
    REQUIRE_NOTHROW(job->export_print(exported.str()));

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == ext);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(document.image(i).buf == masks[i]);
    }
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));
    document.invert(1);
    document.save_archive(edited.str());
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    auto inverted = masks[1];
    for (auto &value : inverted) value = phz_stored(uint8_t(255 - value));
    CHECK(reopened.image(1).buf == inverted);
    CHECK(reopened.image(0).buf == masks[0]);
    CHECK(reopened.image(2).buf == masks[2]);
}

TEST_CASE("Damaged PHZ and FDG files fail with specific messages", "[sla_layers][sla_more_readers]")
{
    const bool fdg = GENERATE(false, true);
    CAPTURE(fdg);
    const auto &spec = phrozen_spec(fdg);
    const std::string ext = spec.id, family = fdg ? "FDG" : "PHZ";
    const auto masks = phz_masks(4);
    TempFile file("damaged." + ext);
    auto load = [&](const std::vector<uint8_t> &data) { write_file(file.str(), data); return sla::LayerDocument::load_archive(file.str()); };
    auto make = [&](uint32_t seed, PhzFlaws flaws = {}) { return make_phz(masks, seed, flaws, fdg); };
    SECTION("shorter than the header") {
        auto bytes = make(0); bytes.resize(100);
        REQUIRE_THROWS_WITH(load(bytes), ContainsSubstring(family + " file is truncated"));
    }
    SECTION("zero resolution") {
        PhzFlaws flaws; flaws.zero_resolution = true;
        REQUIRE_THROWS_WITH(load(make(0, flaws)), ContainsSubstring("resolution"));
    }
    SECTION("zero layers") {
        PhzFlaws flaws; flaws.count_override = 0;
        auto bytes = make(0, flaws); put32le(bytes, spec.count, 0);
        REQUIRE_THROWS_WITH(load(bytes), ContainsSubstring("layer count"));
    }
    SECTION("layer count larger than the file can hold") {
        PhzFlaws flaws; flaws.count_override = 90000;
        REQUIRE_THROWS_WITH(load(make(0, flaws)), ContainsSubstring("layer table is truncated"));
    }
    SECTION("layer table outside the file") {
        PhzFlaws flaws; flaws.table_override = 5;
        REQUIRE_THROWS_WITH(load(make(0, flaws)), ContainsSubstring("layer table is truncated or outside"));
    }
    SECTION("payload beyond the end") {
        PhzFlaws flaws; flaws.payload_past_end = 1;
        REQUIRE_THROWS_WITH(load(make(0, flaws)), ContainsSubstring("layer 2 data is truncated"));
    }
    SECTION("empty payload") {
        PhzFlaws flaws; flaws.empty_layer = 3;
        REQUIRE_THROWS_WITH(load(make(0, flaws)), ContainsSubstring("layer 4 has an empty or oversized"));
    }
    SECTION("RLE runs past the raster") {
        PhzFlaws flaws; flaws.overrun_layer = 1;
        REQUIRE_THROWS_WITH(load(make(0, flaws)), ContainsSubstring("layer 2 has corrupted RLE data"));
    }
    SECTION("RLE ends before the raster is full") {
        PhzFlaws flaws; flaws.short_layer = 3;
        REQUIRE_THROWS_WITH(load(make(0, flaws)), ContainsSubstring("layer 4 ends short"));
    }
    SECTION("truncated file") {
        auto bytes = make(0);
        bytes.resize(bytes.size() - 20);
        REQUIRE_THROWS(load(bytes));
    }
}

TEST_CASE("PHZ and FDG loading and saving honour cancellation", "[sla_layers][sla_more_readers]")
{
    const bool fdg = GENERATE(false, true);
    const std::string ext = phrozen_spec(fdg).id;
    TempFile source("src." + ext), out("out." + ext);
    write_file(source.str(), make_phz(phz_masks(5), 0, {}, fdg));
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(source.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    auto document = sla::LayerDocument::load_archive(source.str());
    document.invert(0);
    REQUIRE_THROWS_AS(document.save_archive(out.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_FALSE(boost::filesystem::exists(out.path));
    CHECK(document.dirty());
}

// ---------------------------------------------------------------------------
// ZCode (UnizMaker IBEE) and ZCodex (Z-Suite)
// ---------------------------------------------------------------------------
namespace {

constexpr size_t zc_columns = 48, zc_rows = 20;

struct ZcFlaws {
    int missing_layer = -1;    // 0-based layer whose image is absent
    int declared = -1;         // layer count written to the manifest
    bool bad_manifest = false; // not XML / not JSON
    bool doctype = false;      // ZCode only
    int color_layer = -1;      // layer stored as RGB
    int text_layer = -1;
    int resized_layer = -1;
    bool no_layers = false;
};

std::string zc_layer_name(bool zcodex, size_t layer)
{
    if (!zcodex) return std::to_string(layer + 1) + ".png";
    char number[16];
    std::snprintf(number, sizeof number, "%05zu", layer);
    return std::string("ResinSlicesData/Slice") + number + ".png";
}

std::vector<uint8_t> make_zcode(bool zcodex, const std::vector<std::vector<uint8_t>> &masks, ZcFlaws flaws = {})
{
    TempFile file("build.zip");
    {
        Zipper zip(file.str());
        auto add_text = [&](const std::string &name, const std::string &text) { zip.add_entry(name, text.data(), text.size()); };
        const size_t declared = flaws.declared >= 0 ? size_t(flaws.declared) : masks.size();
        if (!zcodex) {
            std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
            if (flaws.doctype) xml += "<!DOCTYPE Print [<!ENTITY x \"y\">]>\n";
            xml += "<Print><Device z=\"100\"/><Job><name>t</name><layers>" + std::to_string(declared) + "</layers></Job></Print>";
            add_text("task.xml", flaws.bad_manifest ? "<Print><Job>" : xml);
            add_text("lcd.gcode", "ENCRYPTED-LINE-1==\nENCRYPTED-LINE-2==\n");
            add_text("preview.png", "preview-bytes");
        } else {
            add_text("ResinMetadata", flaws.bad_manifest ? "{broken" : Json{{"TotalLayersCount", declared}, {"LayerThickness", 0.05}}.dump(2));
            add_text("UserSettingsData", "{\"MaxLayer\":4}");
            add_text("ZCodeMetadata", "{\"ZCodexVersion\":\"2.0.0.0\"}");
            add_text("ResinGCodeData", "M6054 \"0.png\";\n");
            add_text("Preview.png", "preview-bytes");
        }
        if (!flaws.no_layers)
            for (size_t i = 0; i < masks.size(); ++i) {
                if (int(i) == flaws.missing_layer) continue;
                auto bytes = png_bytes(masks[i], zc_columns, zc_rows, int(i) == flaws.color_layer);
                if (int(i) == flaws.text_layer) bytes = text_bytes("this is not a png file at all, just some long enough text");
                if (int(i) == flaws.resized_layer) bytes = png_bytes(std::vector<uint8_t>(16 * zc_rows, 9), 16, zc_rows, false);
                zip.add_entry(zc_layer_name(zcodex, i), bytes.data(), bytes.size());
            }
        zip.finalize();
    }
    return read_file(file.str());
}

} // namespace

TEST_CASE("ZCode and ZCodex reopen every pixel and save byte-identically when unchanged", "[sla_layers][sla_more_readers]")
{
    const bool zcodex = GENERATE(false, true);
    CAPTURE(zcodex);
    const std::string ext = zcodex ? "zcodex" : "zcode";
    const auto masks = test_masks(zc_columns, zc_rows, 5, false);
    const auto bytes = make_zcode(zcodex, masks);
    TempFile source("src." + ext), saved("out." + ext);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    REQUIRE(document.archive_format() == ext);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        const auto image = document.image(i);
        REQUIRE(image.cols == zc_columns);
        REQUIRE(image.rows == zc_rows);
        CHECK(image.buf == masks[i]);
    }
    CHECK_FALSE(document.dirty());
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == bytes);

    TempFile wrong(zcodex ? "out.zcode" : "out.zcodex");
    CHECK_THROWS_WITH(document.save_archive(wrong.str()), ContainsSubstring("extension"));
    CHECK_FALSE(boost::filesystem::exists(wrong.path));
}

TEST_CASE("ZCode and ZCodex edit rewrites only the changed layer image", "[sla_layers][sla_more_readers]")
{
    const bool zcodex = GENERATE(false, true);
    CAPTURE(zcodex);
    const std::string ext = zcodex ? "zcodex" : "zcode";
    const auto masks = test_masks(zc_columns, zc_rows, 5, false);
    const auto bytes = make_zcode(zcodex, masks);
    TempFile source("src." + ext), edited("out." + ext);
    write_file(source.str(), bytes);

    auto document = sla::LayerDocument::load_archive(source.str());
    auto painted = masks[3];
    for (size_t i = 0; i < 150; ++i) painted[i] = uint8_t((i * 3) % 256);
    document.replace_image(3, make_image(zc_columns, zc_rows, painted));
    document.save_archive(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(reopened.image(i).buf == (i == 3 ? painted : masks[i]));
    }
    const auto before = zip_entries(source.str()), after = zip_entries(edited.str());
    REQUIRE(after.size() == before.size());
    const auto changed = zc_layer_name(zcodex, 3);
    for (const auto &[name, data] : before) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK((after.at(name) == data) == (name != changed));
    }
    CHECK(after.at(changed) == png_bytes(painted, zc_columns, zc_rows, false));
}

TEST_CASE("ZCode and ZCodex files from the real exporter reopen with the exported masks", "[sla_layers][sla_more_readers]")
{
    const bool zcodex = GENERATE(false, true);
    CAPTURE(zcodex);
    const std::string ext = zcodex ? "zcodex" : "zcode";
    // The fixture sets the screen each family needs: 200x120 (zcode) / 1440x2560 (zcodex).
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, ext);
    const size_t columns = size_t(full.display_pixels_x.value), rows = size_t(full.display_pixels_y.value);
    DynamicPrintConfig cfg; cfg.apply(full);
    const auto masks = test_masks(columns, rows, 3, false);
    std::vector<sla::EncodedRaster> encoded;
    for (const auto &mask : masks) encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), columns, rows, 1));
    const auto source_document = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source_document, {.05, .1, .15});
    TempFile exported("exported." + ext), same("same." + ext), edited("edited." + ext);
    REQUIRE_NOTHROW(job->export_print(exported.str()));

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == ext);
    REQUIRE(document.layer_count() == masks.size());
    for (size_t i = 0; i < masks.size(); ++i) {
        CAPTURE(i);
        CHECK(document.image(i).buf == masks[i]);
    }
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));
    document.invert(1);
    document.save_archive(edited.str());
    const auto before = zip_entries(exported.str()), after = zip_entries(edited.str());
    // The program (the encrypted G-code in ZCode) and the machine metadata are not re-generated by a
    // mask edit. The exporter's files also carry the material estimate (volume, weight, price), which
    // follows the lit area (wave 11): everything else in those manifests stays byte for byte.
    for (const char *name : zcodex ? std::vector<const char *>{"ZCodeMetadata", "ResinGCodeData"} : std::vector<const char *>{"lcd.gcode"}) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK(after.at(name) == before.at(name)); // includes the encrypted G-code, which must not be re-generated
    }
    auto without_estimate = [&](const std::map<std::string, std::vector<uint8_t>> &entries, const char *name) {
        std::string text(entries.at(name).begin(), entries.at(name).end());
        if (zcodex) {
            auto json = nlohmann::json::parse(text);
            for (const char *key : {"TotalMaterialVolumeUsed", "TotalMaterialWeightUsed", "Layers", "MaterialVolume"}) json.erase(key);
            return json.dump();
        }
        for (const char *tag : {"volumn", "weight", "price"})
            text = std::regex_replace(text, std::regex(std::string("<") + tag + ">[^<]*</" + tag + ">"), "");
        return text;
    };
    for (const char *name : zcodex ? std::vector<const char *>{"ResinMetadata", "UserSettingsData"} : std::vector<const char *>{"task.xml"}) {
        CAPTURE(name);
        REQUIRE(after.count(name) == 1);
        CHECK(without_estimate(after, name) == without_estimate(before, name));
    }
    CHECK(after.at(zcodex ? "ResinMetadata" : "task.xml") != before.at(zcodex ? "ResinMetadata" : "task.xml")); // the volume moved
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    auto inverted = masks[1];
    for (auto &value : inverted) value = uint8_t(255 - value);
    CHECK(reopened.image(1).buf == inverted);
    CHECK(reopened.image(0).buf == masks[0]);
    CHECK(reopened.image(2).buf == masks[2]);
}

TEST_CASE("Damaged ZCode and ZCodex files fail with specific messages", "[sla_layers][sla_more_readers]")
{
    const bool zcodex = GENERATE(false, true);
    CAPTURE(zcodex);
    const std::string ext = zcodex ? "zcodex" : "zcode", family = zcodex ? "ZCodex" : "ZCode";
    const size_t first = zcodex ? 0 : 1; // first layer image number
    const auto masks = test_masks(zc_columns, zc_rows, 4, false);
    TempFile file("damaged." + ext);
    auto load = [&](const std::vector<uint8_t> &data) { write_file(file.str(), data); return sla::LayerDocument::load_archive(file.str()); };
    SECTION("layer image missing") {
        ZcFlaws flaws; flaws.missing_layer = 2;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring("layer image " + std::to_string(first + 2) + " is missing"));
    }
    SECTION("manifest declares fewer layers than there are images") {
        ZcFlaws flaws; flaws.declared = 3;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring("beyond its declared 3 layers"));
    }
    SECTION("manifest declares more layers than there are images") {
        ZcFlaws flaws; flaws.declared = 6;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring("layer image " + std::to_string(first + 4) + " is missing"));
    }
    SECTION("manifest is not valid") {
        ZcFlaws flaws; flaws.bad_manifest = true;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring(zcodex ? "not valid JSON" : "not valid XML"));
    }
    SECTION("a layer is RGB") {
        ZcFlaws flaws; flaws.color_layer = 1;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring("layer 2 must be an 8-bit grayscale PNG"));
    }
    SECTION("a layer is not a PNG") {
        ZcFlaws flaws; flaws.text_layer = 2;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring("layer 3 is not a PNG"));
    }
    SECTION("a layer has other dimensions") {
        ZcFlaws flaws; flaws.resized_layer = 1;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring("layer 2 differs in size"));
    }
    SECTION("no layer images at all") {
        ZcFlaws flaws; flaws.no_layers = true;
        REQUIRE_THROWS_WITH(load(make_zcode(zcodex, masks, flaws)), ContainsSubstring(family + " contains no layer images"));
    }
    SECTION("truncated archive") {
        auto bytes = make_zcode(zcodex, masks);
        bytes.resize(bytes.size() / 2);
        REQUIRE_THROWS(load(bytes));
    }
    if (!zcodex) SECTION("task.xml with a DTD") {
        ZcFlaws flaws; flaws.doctype = true;
        REQUIRE_THROWS_WITH(load(make_zcode(false, masks, flaws)), ContainsSubstring("DTD"));
    }
}

TEST_CASE("ZCode and ZCodex loading and saving honour cancellation", "[sla_layers][sla_more_readers]")
{
    const bool zcodex = GENERATE(false, true);
    const std::string ext = zcodex ? "zcodex" : "zcode";
    TempFile source("src." + ext), out("out." + ext);
    write_file(source.str(), make_zcode(zcodex, test_masks(zc_columns, zc_rows, 5, false)));
    REQUIRE_THROWS_AS(sla::LayerDocument::load_archive(source.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    auto document = sla::LayerDocument::load_archive(source.str());
    document.invert(0);
    REQUIRE_THROWS_AS(document.save_archive(out.str(), [](size_t, size_t) { return false; }), sla::LayerOperationCancelled);
    CHECK_FALSE(boost::filesystem::exists(out.path));
    CHECK(document.dirty());
}
