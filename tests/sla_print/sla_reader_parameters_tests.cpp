// SPDX-License-Identifier: AGPL-3.0-or-later
// Per-layer parameter editing for the format families added in waves 7-8:
// PHZ, FDG, OSLA, VDT, Anycubic ZIP and Klipper ZIP. Sources are files from the
// real exporters; every saved result is checked twice: by reopening it through
// LayerDocument and by a test-local reader that shares no code with the
// library (raw record offsets, JSON members, G-code text).
//
// Wave 11 adds the text-program families ZCode (RSA-framed lcd.gcode), ZCodex
// (ResinGCodeData) and JXS (control.json action list); their tests are at the
// end of the file and use test-local readers of the raw program text.
//
// Families that are masks-only by design are asserted to stay read-only: LGS,
// MDLP, GR1, OSF, Anet and NanoDLP keep only global values (UVtools has no
// PrintParameterPerLayerModifiers for them, their per-layer records hold
// geometry and statistics only), QDT stores no print parameters at all.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerExportProfile.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <test_utils.hpp>
#include "sla_archive_test_config.hpp"

using namespace Slic3r;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::json;
using P = sla::LayerParameter;

namespace {

struct TempFile {
    boost::filesystem::path path;
    explicit TempFile(const std::string &name)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("reader-params-%%%%-%%%%-" + name)) {}
    ~TempFile() { boost::system::error_code ec; boost::filesystem::remove(path, ec); }
    std::string str() const { return path.string(); }
};

std::vector<uint8_t> read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

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

// Two fresh saves of equal documents: ZIP containers are compared by entry contents, because
// ZIP entry timestamps differ when the saves straddle a second boundary; other files byte for byte.
bool same_saved_content(const std::string &a, const std::string &b)
{
    const auto bytes_a = read_file(a), bytes_b = read_file(b);
    const auto is_zip = [](const std::vector<uint8_t> &v) { return v.size() >= 4 && v[0] == 'P' && v[1] == 'K' && v[2] == 3 && v[3] == 4; };
    if (is_zip(bytes_a) && is_zip(bytes_b)) return zip_entries(a) == zip_entries(b);
    return bytes_a == bytes_b;
}

uint32_t u32le(const std::vector<uint8_t> &bytes, size_t at)
{
    return uint32_t(bytes.at(at)) | (uint32_t(bytes.at(at + 1)) << 8) | (uint32_t(bytes.at(at + 2)) << 16) | (uint32_t(bytes.at(at + 3)) << 24);
}
float f32le(const std::vector<uint8_t> &bytes, size_t at)
{
    const uint32_t bits = u32le(bytes, at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
Json json_entry(const std::map<std::string, std::vector<uint8_t>> &entries, const std::string &name)
{
    const auto &bytes = entries.at(name);
    return Json::parse(bytes.begin(), bytes.end());
}

constexpr size_t layers_total = 4;
const std::vector<double> heights = {.05, .10, .15, .20};

struct Family {
    std::string id;
    size_t columns, rows;
    std::vector<P> fields;            // what the format stores per layer
    std::function<void(SLAFullPrintConfig &)> tweak;
};

const std::vector<Family> &families()
{
    using L = std::vector<P>;
    const L full = {P::PositionZ, P::Exposure, P::LiftDistance, P::LiftSpeed, P::LiftDistance2, P::LiftSpeed2, P::RetractSpeed,
                    P::RetractDistance2, P::RetractSpeed2, P::WaitBefore, P::WaitAfter, P::WaitAfterLift, P::LightPWM};
    L vdt = full;
    vdt.push_back(P::LightOff);
    static const std::vector<Family> list = {
        {"phz", 200, 120, {P::PositionZ, P::Exposure, P::LightOff}, [](SLAFullPrintConfig &) {}},
        {"fdg", 200, 120, {P::PositionZ, P::Exposure, P::LightOff}, [](SLAFullPrintConfig &) {}},
        {"osla", 200, 120, full, [](SLAFullPrintConfig &) {}},
        {"vdt", 200, 120, vdt, [](SLAFullPrintConfig &) {}},
        {"pm4u", 200, 120, {P::PositionZ, P::Exposure, P::LiftDistance, P::LiftSpeed}, [](SLAFullPrintConfig &c) {
            // The Anycubic ZIP schema has one pre-cure wait, no PWM, no after-cure wait or second retract.
            for (const char *prefix : {"sla_", "sla_bottom_"}) {
                c.set(std::string(prefix) + "lift_height", 5.); c.set(std::string(prefix) + "lift_height2", 0.);
                c.set(std::string(prefix) + "lift_speed", 90.); c.set(std::string(prefix) + "retract_speed", 150.);
                c.set(std::string(prefix) + "retract_height2", 0.); c.set(std::string(prefix) + "wait_before_cure", 1.);
                c.set(std::string(prefix) + "wait_after_cure", 0.); c.set(std::string(prefix) + "wait_after_lift", 0.);
                c.set(std::string(prefix) + "light_pwm", 255);
            }
        }},
        {"klipper", 200, 120, full, [](SLAFullPrintConfig &) {}},
        {"klipper_rgb", 200, 120, full, [](SLAFullPrintConfig &) {}},
    };
    return list;
}

const Family &family(const std::string &id)
{
    for (const auto &candidate : families())
        if (candidate.id == id) return candidate;
    FAIL("unknown family " + id);
    return families().front();
}

std::vector<std::vector<uint8_t>> family_masks(size_t columns, size_t rows)
{
    std::vector<std::vector<uint8_t>> masks(layers_total, std::vector<uint8_t>(columns * rows, 0));
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < columns; ++x) {
            if (x > columns / 5 && x < columns / 2 && y > rows / 6 && y < rows / 2) masks[0][y * columns + x] = 255;
            if (x > columns / 4 && x < 3 * columns / 4 && y > rows / 4 && y < 3 * rows / 4) masks[1][y * columns + x] = 255;
            if (x < columns / 3 && y < rows / 3) masks[3][y * columns + x] = 255; // masks[2] stays black
        }
    return masks;
}

void export_family(const Family &f, const std::vector<std::vector<uint8_t>> &masks, const std::string &path)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, f.id);
    full.set("sla_archive_format", std::string("SL1"));
    f.tweak(full);
    DynamicPrintConfig cfg;
    cfg.apply(full);
    std::vector<sla::EncodedRaster> encoded;
    for (const auto &mask : masks) encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), f.columns, f.rows, 1));
    auto source = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source, heights);
    ThumbnailData thumbnail;
    thumbnail.set(120, 150);
    for (size_t i = 0; i < thumbnail.pixels.size(); i += 4) { thumbnail.pixels[i] = 200; thumbnail.pixels[i + 3] = 255; }
    job->export_layer_document(source, f.id, path, {thumbnail}, f.id);
}

// ---- test-local readers: raw offsets, JSON members, G-code text ---------------------------------
// Exposure, Z and (where the format has them) lift speed of one layer, from the bytes of the file.
struct Raw { double z = 0, exposure = 0, lift_speed = -1; };

Raw raw_layer(const std::string &id, const std::string &path, size_t layer)
{
    Raw raw;
    if (id == "phz" || id == "fdg") {
        const auto bytes = read_file(path);
        const size_t table = u32le(bytes, id == "phz" ? 36 : 52), record = table + 36 * layer;
        raw.z = f32le(bytes, record); raw.exposure = f32le(bytes, record + 4);
    } else if (id == "osla") {
        const auto bytes = read_file(path);
        const size_t table = u32le(bytes, 150 + 79), size = u32le(bytes, 150 + 75), record = table + size * layer;
        raw.z = f32le(bytes, record + 4); raw.lift_speed = f32le(bytes, record + 12); raw.exposure = f32le(bytes, record + 44);
    } else if (id == "vdt") {
        const auto layers = json_entry(zip_entries(path), "manifest.json")["layers"];
        raw.z = layers[layer]["height"].get<double>(); raw.exposure = layers[layer]["exposure_time"].get<double>();
        raw.lift_speed = layers[layer]["lift_speed"].get<double>();
    } else if (id == "pm4u") {
        const auto paras = json_entry(zip_entries(path), "layers_controller.conf")["paras"];
        double z = 0;
        for (size_t i = 0; i <= layer; ++i) z += paras[i]["layer_thickness"].get<double>();
        raw.z = z; raw.exposure = paras[layer]["exposure_time"].get<double>(); raw.lift_speed = paras[layer]["zup_speed"].get<double>() * 60;
    } else { // klipper
        const auto entries = zip_entries(path);
        std::string script;
        for (const auto &[name, bytes] : entries)
            if (name.size() > 6 && name.substr(name.size() - 6) == ".gcode") script.assign(bytes.begin(), bytes.end());
        const size_t start = script.find(";LAYER_START: " + std::to_string(layer) + "\n");
        REQUIRE(start != std::string::npos);
        const size_t end = script.find(";LAYER_END", start);
        const std::string block = script.substr(start, end - start);
        std::smatch m;
        REQUIRE(std::regex_search(block, m, std::regex("HEIGHT=([0-9.]+)")));
        raw.z = std::stod(m[1]);
        REQUIRE(std::regex_search(block, m, std::regex("M1400 S[0-9]+ P([0-9]+)")));
        raw.exposure = std::stod(m[1]) / 1000.;
        REQUIRE(std::regex_search(block, m, std::regex("G1 Z[0-9.]+ F([0-9.]+)")));
        raw.lift_speed = std::stod(m[1]);
    }
    return raw;
}

double stored_print_time(const std::string &id, const std::string &path)
{
    if (id == "phz" || id == "fdg") return u32le(read_file(path), id == "phz" ? 48 : 56);
    if (id == "osla") return u32le(read_file(path), 150 + 87);
    const auto entries = zip_entries(path);
    if (id == "vdt") return json_entry(entries, "manifest.json")["print_statistics"]["estimated_time"].get<double>();
    if (id == "pm4u") return json_entry(entries, "print_info.json")["print_time"].get<double>();
    for (const auto &[name, bytes] : entries)
        if (name.size() > 6 && name.substr(name.size() - 6) == ".gcode") {
            const std::string script(bytes.begin(), bytes.end());
            std::smatch m;
            REQUIRE(std::regex_search(script, m, std::regex(";PrintTime:([0-9.]+)")));
            return std::stod(m[1]);
        }
    FAIL("no print time");
    return 0;
}

double stored_volume(const std::string &id, const std::string &path)
{
    if (id == "phz" || id == "fdg") return f32le(read_file(path), id == "phz" ? 128 : 104);
    if (id == "osla") return f32le(read_file(path), 150 + 91);
    const auto entries = zip_entries(path);
    if (id == "vdt") return json_entry(entries, "manifest.json")["print_statistics"]["volume"].get<double>();
    if (id == "pm4u") return json_entry(entries, "print_info.json")["volume"].get<double>();
    for (const auto &[name, bytes] : entries)
        if (name.size() > 6 && name.substr(name.size() - 6) == ".gcode") {
            const std::string script(bytes.begin(), bytes.end());
            std::smatch m;
            REQUIRE(std::regex_search(script, m, std::regex(";Volume:([0-9.]+)")));
            return std::stod(m[1]);
        }
    FAIL("no volume");
    return 0;
}

// The save rule ties a family to its file extension; the two Klipper variants share ".zip".
std::string extension_of(const std::string &id)
{
    return id == "klipper" ? "zip" : id == "klipper_rgb" ? "rgb.zip" : id;
}

const auto family_ids = [] { return GENERATE(std::string("phz"), std::string("fdg"), std::string("osla"), std::string("vdt"),
                                              std::string("pm4u"), std::string("klipper"), std::string("klipper_rgb")); };

std::vector<sla::LayerParameters> all_rows(const sla::LayerDocument &document)
{
    std::vector<sla::LayerParameters> rows;
    for (size_t i = 0; i < document.layer_count(); ++i) rows.push_back(document.layer_parameters(i));
    return rows;
}

} // namespace

TEST_CASE("Exported files expose exactly the fields their container stores", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + extension_of(id));
    export_family(f, family_masks(f.columns, f.rows), exported.str());

    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == id);
    const auto capabilities = document.parameter_capabilities();
    std::vector<P> listed;
    for (const auto &capability : capabilities) {
        CHECK(capability.editable);
        listed.push_back(capability.field);
    }
    std::vector<P> expected = f.fields;
    std::sort(listed.begin(), listed.end());
    std::sort(expected.begin(), expected.end());
    CHECK(listed == expected);
    CHECK_FALSE(document.has_parameter_edits());
    CHECK_FALSE(document.has_convertible_parameter_table());
    for (size_t i = 0; i < layers_total; ++i) {
        CAPTURE(i);
        const auto row = document.layer_parameters(i);
        CHECK(row.size() == f.fields.size());
        const auto raw = raw_layer(id, exported.str(), i);
        CHECK(row.at(P::PositionZ) == Approx(heights[i]).margin(1e-6));
        CHECK(row.at(P::PositionZ) == Approx(raw.z).margin(1e-6));
        CHECK(row.at(P::Exposure) == Approx(raw.exposure).margin(1e-6));
        if (raw.lift_speed >= 0) CHECK(row.at(P::LiftSpeed) == Approx(raw.lift_speed).margin(1e-3));
    }
}

TEST_CASE("Containers with only global values stay read-only", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = GENERATE(std::string("lgs"), std::string("mdlp"), std::string("gr1"), std::string("qdt"),
                                    std::string("osf"), std::string("n4"), std::string("nanodlp"));
    CAPTURE(id);
    // The exporters need a printer matching each container; reuse the fixture's choices.
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, id);
    full.set("sla_archive_format", std::string("SL1"));
    // The fixture owns each printer's geometry.
    const size_t columns = size_t(full.display_pixels_x.value), rows = size_t(full.display_pixels_y.value);
    DynamicPrintConfig cfg;
    cfg.apply(full);
    std::vector<sla::EncodedRaster> encoded;
    const auto masks = family_masks(columns, rows);
    for (const auto &mask : masks) encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), columns, rows, 1));
    auto source = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source, heights);
    TempFile exported("exported." + extension_of(id));
    ThumbnailData thumbnail;
    thumbnail.set(120, 150);
    try {
        job->export_layer_document(source, id, exported.str(), {thumbnail}, id);
    } catch (const std::exception &) {
        SKIP("this fixture cannot export " + id);
    }
    auto document = sla::LayerDocument::load_archive(exported.str());
    CHECK(document.parameter_capabilities().empty());
    CHECK_FALSE(document.has_parameter_edits());
    CHECK_THROWS(document.set_layer_parameters(0, {{P::Exposure, 5.}}));
}

TEST_CASE("Single-layer and range edits survive save and reopen", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    const auto masks = family_masks(f.columns, f.rows);
    TempFile exported("exported." + extension_of(id)), same("same." + extension_of(id)), edited("edited." + extension_of(id));
    export_family(f, masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);

    // An unchanged save is the exporter's own file, byte for byte.
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));

    // One layer: exposure and Z (between its neighbours). A range: exposure of layers 2..3.
    document.set_layer_parameters(1, {{P::Exposure, 7.25}, {P::PositionZ, .12}});
    for (size_t i = 2; i < layers_total; ++i) document.set_layer_parameters(i, {{P::Exposure, 6.5}});
    const bool has_lift = std::find(f.fields.begin(), f.fields.end(), P::LiftSpeed) != f.fields.end();
    if (has_lift) document.set_layer_parameters(2, {{P::LiftSpeed, 111.}});
    CHECK(document.dirty());
    CHECK(document.has_parameter_edits());
    const auto after = all_rows(document);
    CHECK(after[1].at(P::Exposure) == Approx(7.25));
    CHECK(after[1].at(P::PositionZ) == Approx(.12));
    CHECK(after[0] == before[0]);
    document.save_archive(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.layer_count() == layers_total);
    const auto reread = all_rows(reopened);
    for (size_t i = 0; i < layers_total; ++i) {
        CAPTURE(i);
        REQUIRE(reread[i].size() == after[i].size());
        for (const auto &[field, value] : after[i]) {
            CAPTURE(int(field));
            CHECK(reread[i].at(field) == Approx(value).margin(1e-6));
        }
        // Pixels are untouched by a parameter edit.
        CHECK(reopened.image(i).buf == document.image(i).buf);
        // And the independent reader agrees.
        const auto raw = raw_layer(id, edited.str(), i);
        CHECK(raw.z == Approx(after[i].at(P::PositionZ)).margin(1e-6));
        CHECK(raw.exposure == Approx(after[i].at(P::Exposure)).margin(1e-6));
        if (raw.lift_speed >= 0) CHECK(raw.lift_speed == Approx(after[i].at(P::LiftSpeed)).margin(1e-3));
    }
    CHECK(reopened.has_parameter_edits() == false); // the edited file is a new source
    // Fields of untouched layers are bit-identical to the source.
    CHECK(reread[0] == before[0]);
}

TEST_CASE("Invalid and unsupported parameter edits are refused without changing the document", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + extension_of(id));
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);
    const auto revision = document.revision();

    CHECK_THROWS(document.set_layer_parameters(1, {{P::Exposure, -1.}}));
    CHECK_THROWS(document.set_layer_parameters(1, {{P::Exposure, std::nan("")}}));
    CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::PositionZ, .2}}), ContainsSubstring("increase"));   // beyond neighbour
    CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::PositionZ, .05}}), ContainsSubstring("increase")); // equal to previous
    CHECK_THROWS(document.set_layer_parameters(1, {{P::PositionZ, 10000.}}));
    CHECK_THROWS(document.set_layer_parameters(layers_total, {{P::Exposure, 1.}}));
    // A field the container does not store is rejected (here a global of the machine).
    const bool has_retract2 = std::find(f.fields.begin(), f.fields.end(), P::RetractDistance2) != f.fields.end();
    if (!has_retract2) CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::RetractDistance2, 1.}}), ContainsSubstring("cannot edit"));
    const bool has_pwm = std::find(f.fields.begin(), f.fields.end(), P::LightPWM) != f.fields.end();
    if (has_pwm) {
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LightPWM, 300.}}), ContainsSubstring("PWM"));
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LightPWM, 10.5}}), ContainsSubstring("PWM"));
    }
    if (std::find(f.fields.begin(), f.fields.end(), P::LiftSpeed) != f.fields.end() && id != "pm4u") {
        // An active lift needs a speed.
        CHECK_THROWS(document.set_layer_parameters(1, {{P::LiftDistance, 5.}, {P::LiftSpeed, 0.}}));
    }
    CHECK(all_rows(document) == before);
    CHECK(document.revision() == revision);
    CHECK_FALSE(document.dirty());
}

TEST_CASE("Undo and redo restore parameter rows", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + extension_of(id)), restored("restored." + extension_of(id));
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);
    document.set_layer_parameters(2, {{P::Exposure, 9.}});
    const auto changed = all_rows(document);
    REQUIRE(changed != before);
    REQUIRE(document.undo());
    CHECK(all_rows(document) == before);
    CHECK_FALSE(document.has_parameter_edits());
    REQUIRE(document.redo());
    CHECK(all_rows(document) == changed);
    // A whole table replaced at once is also one step.
    auto table = changed;
    for (auto &row : table) row[P::Exposure] = 4.5;
    document.replace_layer_parameter_table(table);
    CHECK(document.layer_parameters(0).at(P::Exposure) == Approx(4.5));
    REQUIRE(document.undo());
    CHECK(all_rows(document) == changed);
    // A snapshot keeps the rows and saves like the original.
    const auto copy = document.snapshot();
    CHECK(all_rows(copy) == changed);
}

TEST_CASE("Print time and material statistics follow the edit", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    const auto masks = family_masks(f.columns, f.rows);
    TempFile exported("exported." + extension_of(id)), edited("edited." + extension_of(id)), painted("painted." + extension_of(id));
    export_family(f, masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);
    const double time_before = stored_print_time(id, exported.str());

    // Exposure only: the time moves by the exposure difference (the exporters round the total up
    // to whole seconds, except the Anycubic ZIP and Klipper which keep fractions).
    double delta = 0;
    for (size_t i = 0; i < layers_total; ++i) {
        delta += 3.0;
        document.set_layer_parameters(i, {{P::Exposure, before[i].at(P::Exposure) + 3.0}});
    }
    document.save_archive(edited.str());
    const double time_after = stored_print_time(id, edited.str());
    if (id == "pm4u" || id.rfind("klipper", 0) == 0) CHECK(time_after == Approx(time_before + delta).margin(0.02));
    else CHECK(time_after == Approx(time_before + delta).margin(1.0001));
    // The thickness did not change and no mask was touched: the volume stays.
    CHECK(stored_volume(id, edited.str()) == Approx(stored_volume(id, exported.str())).epsilon(1e-6));

    // A mask edit scales the volume by the thickness-weighted lit area.
    auto second = sla::LayerDocument::load_archive(exported.str());
    auto lit = [&](size_t layer) {
        const auto image = second.image(layer);
        return double(std::count_if(image.buf.begin(), image.buf.end(), [](uint8_t v) { return v != 0; }));
    };
    double weighted_before = 0, previous = 0;
    for (size_t i = 0; i < layers_total; ++i) { weighted_before += lit(i) * (heights[i] - previous); previous = heights[i]; }
    second.invert(2); // the black layer becomes fully lit
    double weighted_after = 0;
    previous = 0;
    for (size_t i = 0; i < layers_total; ++i) { weighted_after += lit(i) * (heights[i] - previous); previous = heights[i]; }
    REQUIRE(weighted_after > weighted_before);
    second.save_archive(painted.str());
    const double volume_before = stored_volume(id, exported.str());
    REQUIRE(volume_before > 0);
    CHECK(stored_volume(id, painted.str()) == Approx(volume_before * weighted_after / weighted_before).epsilon(1e-4));
}

TEST_CASE("A Z edit moves the thickness weights and every place that stores Z", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    const auto masks = family_masks(f.columns, f.rows);
    TempFile exported("exported." + extension_of(id)), edited("edited." + extension_of(id));
    export_family(f, masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    // Layer 2 (0.15) becomes 0.18: layer 2 gets thicker, layer 3 thinner.
    document.set_layer_parameters(2, {{P::PositionZ, .18}});
    document.save_archive(edited.str());
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    CHECK(reopened.layer_parameters(2).at(P::PositionZ) == Approx(.18).margin(1e-6));
    CHECK(reopened.layer_parameters(3).at(P::PositionZ) == Approx(.20).margin(1e-6));
    CHECK(raw_layer(id, edited.str(), 2).z == Approx(.18).margin(1e-6));
    CHECK(raw_layer(id, edited.str(), 3).z == Approx(.20).margin(1e-6));

    double lit[layers_total];
    for (size_t i = 0; i < layers_total; ++i) {
        const auto image = reopened.image(i);
        lit[i] = double(std::count_if(image.buf.begin(), image.buf.end(), [](uint8_t v) { return v != 0; }));
    }
    const double old_weight = lit[0] * .05 + lit[1] * .05 + lit[2] * .05 + lit[3] * .05;
    const double new_weight = lit[0] * .05 + lit[1] * .05 + lit[2] * .08 + lit[3] * .02;
    CHECK(stored_volume(id, edited.str()) == Approx(stored_volume(id, exported.str()) * new_weight / old_weight).epsilon(1e-4));
}

TEST_CASE("Edits that leave every row alone do not change the saved parameters", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    auto masks = family_masks(f.columns, f.rows);
    TempFile exported("exported." + extension_of(id)), edited("edited." + extension_of(id));
    export_family(f, masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);
    // Setting a field to the value it has is not an edit.
    document.set_layer_parameters(1, {{P::Exposure, before[1].at(P::Exposure)}});
    CHECK_FALSE(document.dirty());
    document.paint(0, 5, 5, 3, 255);
    document.save_archive(edited.str());
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    CHECK(all_rows(reopened) == before);
    for (size_t i = 1; i < layers_total; ++i) CHECK(reopened.image(i).buf == document.image(i).buf);
}

TEST_CASE("Edited parameter rows survive a layer-project round trip", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + extension_of(id)), expected("expected." + extension_of(id)), actual("actual." + extension_of(id)),
             project("project.sla-project");
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    document.set_layer_parameters(1, {{P::Exposure, 7.25}});
    document.set_layer_parameters(3, {{P::PositionZ, .21}});
    document.paint(2, 20, 20, 4, 255);
    const sla::LayerExportProfile profile{unsigned(f.columns), unsigned(f.rows), 100., 60., 1.1, .1, 200.};
    sla::ProjectRasterState state{document, {}, "", profile, sla::ProjectLayerOrigin::ImportedArchive};
    REQUIRE_NOTHROW(sla::save_layer_project(project.str(), state));
    auto restored = sla::read_project_layers(project.str());
    REQUIRE(restored.has_value());
    CHECK(restored->document.parameter_capabilities().size() == document.parameter_capabilities().size());
    CHECK(all_rows(restored->document) == all_rows(document));
    // The restored document saves the same file as the one it was captured from.
    document.save_archive(expected.str());
    restored->document.save_archive(actual.str());
    CHECK(same_saved_content(actual.str(), expected.str()));
    CHECK(restored->document.has_parameter_edits());
}

TEST_CASE("UVtools-style parameter operations run on these documents", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = family_ids();
    CAPTURE(id);
    const auto &f = family(id);
    TempFile exported("exported." + extension_of(id)), edited("edited." + extension_of(id));
    export_family(f, family_masks(f.columns, f.rows), exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    CHECK(document.parameter_operation_refusal().empty());
    const auto table = document.parameter_table();
    REQUIRE(table.size() == layers_total);
    // The container's own bottom layer count marks the first layer.
    CHECK(table.front().is_bottom);
    const auto before = all_rows(document);
    REQUIRE(document.apply_parameter_operation(
        [](std::vector<sla::layerops::Mask> &, std::vector<double> &, std::vector<sla::layerops::LayerParams> &rows, const sla::LayerProgress &) {
            for (auto &row : rows) row.exposure_s += 1.;
            return sla::LayerDocument::LayerListChange{};
        }, false, .05));
    for (size_t i = 0; i < layers_total; ++i) CHECK(document.layer_parameters(i).at(P::Exposure) == Approx(before[i].at(P::Exposure) + 1.).margin(1e-6));
    document.save_archive(edited.str());
    CHECK(sla::LayerDocument::load_archive(edited.str()).layer_parameters(3).at(P::Exposure) == Approx(before[3].at(P::Exposure) + 1.).margin(1e-6));
}


// =================================================================================================
// Wave 11: ZCode, ZCodex and JXS. Their per-layer values are a text program. The readers below
// parse that program from the raw bytes (RSA with the public protocol key written out here, JSON,
// plain lines) and share no code with the library.
// =================================================================================================
namespace {

struct TextFamily {
    std::string id;
    std::vector<P> fields;
    P edit_speed;                  // a speed every one of them stores
    bool z_editable;
};
const std::vector<TextFamily> &text_families()
{
    static const std::vector<TextFamily> list = {
        {"zcode", {P::PositionZ, P::Exposure, P::LiftDistance, P::LiftSpeed, P::LiftDistance2, P::LiftSpeed2, P::WaitAfterLift,
                   P::RetractSpeed, P::RetractDistance2, P::RetractSpeed2, P::WaitBefore, P::WaitAfter, P::LightPWM}, P::LiftSpeed, true},
        {"zcodex", {P::PositionZ, P::LiftDistance, P::LiftSpeed, P::RetractSpeed, P::LightPWM}, P::LiftSpeed, true},
        {"jxs", {P::PositionZ, P::Exposure, P::LiftDistance, P::LiftSpeed, P::LiftDistance2, P::LiftSpeed2, P::WaitAfterLift,
                 P::RetractSpeed, P::RetractDistance2, P::RetractSpeed2, P::WaitBefore, P::WaitAfter, P::LightPWM}, P::LiftSpeed, false},
    };
    return list;
}
const TextFamily &text_family(const std::string &id)
{
    for (const auto &candidate : text_families()) if (candidate.id == id) return candidate;
    FAIL("unknown text family " + id);
    return text_families().front();
}
bool stores(const TextFamily &f, P field) { return std::find(f.fields.begin(), f.fields.end(), field) != f.fields.end(); }

std::pair<size_t, size_t> fixture_size(const std::string &id)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, id);
    return {size_t(full.display_pixels_x.value), size_t(full.display_pixels_y.value)};
}

void export_text(const TextFamily &f, const std::vector<std::vector<uint8_t>> &masks, const std::string &path)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, f.id);
    full.set("sla_archive_format", std::string("SL1"));
    const auto [columns, rows] = fixture_size(f.id);
    DynamicPrintConfig cfg;
    cfg.apply(full);
    std::vector<sla::EncodedRaster> encoded;
    for (const auto &mask : masks) encoded.push_back(sla::PNGRasterEncoder{}(mask.data(), columns, rows, 1));
    auto source = sla::LayerDocument::from_rasters(encoded);
    auto job = SLAPrint::create_raster_job(cfg, source, heights);
    ThumbnailData thumbnail;
    thumbnail.set(120, 150);
    for (size_t i = 0; i < thumbnail.pixels.size(); i += 4) { thumbnail.pixels[i] = 200; thumbnail.pixels[i + 3] = 255; }
    job->export_layer_document(source, f.id, path, {thumbnail}, f.id);
}

const auto text_ids = [] { return GENERATE(std::string("zcode"), std::string("zcodex"), std::string("jxs")); };

std::string entry_text(const std::string &path, const std::string &name)
{
    const auto bytes = zip_entries(path).at(name);
    return {bytes.begin(), bytes.end()};
}

// ZCode: the lcd.gcode lines decrypted with the public key (raw RSA, type-1 padding).
std::vector<std::string> zcode_program(const std::string &path)
{
    using boost::multiprecision::cpp_int;
    const cpp_int n("0xe819c5de84fd029f8ec9916ca74d9946d4e96cc500f7a3e972d4d77afd5c3c27252eea305d635addfd890693db3ebccc"
                    "c0b6a6dc94903771e3bd87d2571edeeb");
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::string> lines;
    std::istringstream in(entry_text(path, "lcd.gcode"));
    for (std::string line; std::getline(in, line);) {
        std::vector<uint8_t> bytes;
        unsigned bits = 0; int have = 0;
        for (char c : line) {
            if (c == '=') break;
            bits = (bits << 6) | unsigned(alphabet.find(c)); have += 6;
            if (have >= 8) { have -= 8; bytes.push_back(uint8_t((bits >> have) & 255)); }
        }
        REQUIRE(bytes.size() == 64);
        cpp_int c = 0;
        for (auto b : bytes) c = (c << 8) | b;
        cpp_int m = boost::multiprecision::powm(c, cpp_int(65537), n);
        std::vector<uint8_t> plain(64);
        for (size_t i = 64; i > 0; --i) { plain[i - 1] = uint8_t((m & 255).convert_to<unsigned>()); m >>= 8; }
        REQUIRE(plain[0] == 0);
        REQUIRE(plain[1] == 1);
        size_t at = 2;
        while (plain[at] == 255) ++at;
        REQUIRE(plain[at] == 0);
        lines.emplace_back(plain.begin() + at + 1, plain.end());
    }
    return lines;
}

// What each layer of the raw program says, for the values every family stores.
struct TextRaw { double exposure = -1, lift_speed = -1, pwm = -1, z = -1; };
std::vector<TextRaw> raw_text_layers(const std::string &id, const std::string &path)
{
    std::vector<TextRaw> layers;
    if (id == "zcode") {
        const auto lines = zcode_program(path);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].rfind("M6054", 0) == 0) layers.emplace_back();
            if (layers.empty()) continue;
            auto &layer = layers.back();
            std::smatch m;
            if (layer.lift_speed < 0 && std::regex_match(lines[i], m, std::regex("G0 Z([0-9.]+) F([0-9.]+);"))) layer.lift_speed = std::stod(m[2]) * 10;
            if (std::regex_match(lines[i], m, std::regex("M106 S([1-9][0-9]*);"))) {
                layer.pwm = std::floor(std::stod(m[1]) * 255 / 300);
                REQUIRE(std::regex_match(lines[i + 1], m, std::regex("G4 P([0-9.]+);")));
                layer.exposure = m[1] == ".0" ? 0. : std::stod(m[1]) / 1000.;
            }
        }
    } else if (id == "zcodex") {
        std::istringstream in(entry_text(path, "ResinGCodeData"));
        std::string line;
        double z = 0;
        bool first_move = false;
        while (std::getline(in, line)) {
            std::smatch m;
            if (line.rfind("<Slice>", 0) == 0) { layers.emplace_back(); first_move = true; continue; }
            if (layers.empty()) continue;
            if (std::regex_match(line, m, std::regex("G1 Z(-?[0-9.]+) F([0-9.]+)"))) {
                if (first_move) layers.back().lift_speed = std::stod(m[2]);
                first_move = false;
            }
            if (std::regex_match(line, m, std::regex("M106 S([1-9][0-9]*)"))) layers.back().pwm = std::stod(m[1]);
        }
        (void) z;
    } else {
        const auto control = json_entry(zip_entries(path), "control.json");
        for (const auto &action : control["action_list"]) {
            const auto key = action[0].get<std::string>();
            if (key == "layerno") { layers.emplace_back(); layers.back().exposure = 0; continue; }
            if (layers.empty()) continue;
            auto &layer = layers.back();
            std::smatch m;
            if (key == "gcode") {
                const auto text = action[1].get<std::string>();
                if (layer.lift_speed < 0 && std::regex_match(text, m, std::regex("G0 Z([0-9.]+) F([0-9.]+)"))) layer.lift_speed = std::stod(m[2]);
                if (std::regex_match(text, m, std::regex("M106 S([1-9][0-9]*)"))) layer.pwm = std::stod(m[1]);
            }
        }
        // exposure: the delay between "M106 S<n>" and "M106 S0"
        layers.clear();
        TextRaw current; bool on = false, any = false;
        for (const auto &action : control["action_list"]) {
            const auto key = action[0].get<std::string>();
            if (key == "layerno") { if (any) layers.push_back(current); current = {}; current.exposure = 0; any = true; on = false; continue; }
            if (!any) continue;
            if (key == "gcode") {
                const auto text = action[1].get<std::string>();
                std::smatch m;
                if (text == "M106 S0") on = false;
                else if (std::regex_match(text, m, std::regex("M106 S([0-9]+)"))) { on = true; current.pwm = std::stod(m[1]); }
                else if (current.lift_speed < 0 && std::regex_match(text, m, std::regex("G0 Z([0-9.]+) F([0-9.]+)"))) current.lift_speed = std::stod(m[2]);
            } else if (key == "delay" && on) current.exposure += action[1].get<double>() / 1000.;
        }
        if (any) layers.push_back(current);
    }
    return layers;
}

double text_print_time(const std::string &id, const std::string &path)
{
    if (id == "zcode") {
        std::smatch m;
        const auto xml = entry_text(path, "task.xml");
        REQUIRE(std::regex_search(xml, m, std::regex("<time>([0-9]+)</time>")));
        return std::stod(m[1]);
    }
    if (id == "zcodex") return json_entry(zip_entries(path), "ResinMetadata")["PrintTime"].get<double>();
    return json_entry(zip_entries(path), "control.json")["total_time"].get<double>();
}
double text_volume(const std::string &id, const std::string &path)
{
    if (id == "zcode") {
        std::smatch m;
        const auto xml = entry_text(path, "task.xml");
        REQUIRE(std::regex_search(xml, m, std::regex("<volumn>([-0-9.e+]+)</volumn>")));
        return std::stod(m[1]);
    }
    if (id == "zcodex") return json_entry(zip_entries(path), "ResinMetadata")["TotalMaterialVolumeUsed"].get<double>();
    return json_entry(zip_entries(path), "control.json")["used_material"].get<double>();
}

// The entries a parameter edit must carry over untouched.
std::set<std::string> program_entries(const std::string &id)
{
    if (id == "zcode") return {"lcd.gcode", "task.xml"};
    if (id == "zcodex") return {"ResinGCodeData", "ResinMetadata", "UserSettingsData", "ZCodeMetadata"};
    return {"control.json", "config.ini"};
}

struct TextFixture {
    const TextFamily &f;
    size_t columns, rows;
    std::vector<std::vector<uint8_t>> masks;
    explicit TextFixture(const std::string &id) : f(text_family(id))
    {
        std::tie(columns, rows) = fixture_size(id);
        masks = family_masks(columns, rows);
    }
};

} // namespace

TEST_CASE("Text-program exports expose exactly the fields their program stores", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = text_ids();
    CAPTURE(id);
    TextFixture fx(id);
    TempFile exported("exported." + id);
    export_text(fx.f, fx.masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    REQUIRE(document.archive_format() == id);
    std::vector<P> listed, expected = fx.f.fields;
    for (const auto &capability : document.parameter_capabilities()) {
        listed.push_back(capability.field);
        CHECK(capability.editable == (capability.field != P::PositionZ || fx.f.z_editable));
    }
    std::sort(listed.begin(), listed.end());
    std::sort(expected.begin(), expected.end());
    CHECK(listed == expected);
    CHECK_FALSE(document.has_parameter_edits());
    CHECK_FALSE(document.has_convertible_parameter_table());
    const auto raw = raw_text_layers(id, exported.str());
    REQUIRE(raw.size() == layers_total);
    for (size_t i = 0; i < layers_total; ++i) {
        CAPTURE(i);
        const auto row = document.layer_parameters(i);
        CHECK(row.size() == fx.f.fields.size());
        CHECK(row.at(P::PositionZ) == Approx(heights[i]).margin(1e-6));
        CHECK(row.at(P::LiftSpeed) == Approx(raw[i].lift_speed).margin(1e-3));
        CHECK(row.at(P::LightPWM) == raw[i].pwm);
        if (id != "zcodex") CHECK(row.at(P::Exposure) == Approx(raw[i].exposure).margin(1e-6));
    }
    // An unchanged document saves the exporter's file byte for byte.
    TempFile same("same." + id);
    document.save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(exported.str()));
}

TEST_CASE("Text-program single-layer and range edits survive save and reopen", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = text_ids();
    CAPTURE(id);
    TextFixture fx(id);
    TempFile exported("exported." + id), edited("edited." + id);
    export_text(fx.f, fx.masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);

    // One layer: speed, PWM (and Z where the format stores it). A range: layers 2..3.
    sla::LayerParameters single = {{P::LiftSpeed, 111.}, {P::LightPWM, 200.}};
    if (fx.f.z_editable) single[P::PositionZ] = .12;
    if (id == "zcode") { single[P::LiftDistance2] = 2.; single[P::LiftSpeed2] = 150.; single[P::WaitAfterLift] = .5; }
    document.set_layer_parameters(1, single);
    for (size_t i = 2; i < layers_total; ++i) {
        sla::LayerParameters patch = {{P::LiftSpeed, 77.}};
        if (stores(fx.f, P::Exposure)) patch[P::Exposure] = 6.5;
        if (id == "zcodex") patch[P::RetractSpeed] = 88.;
        document.set_layer_parameters(i, patch);
    }
    CHECK(document.dirty());
    CHECK(document.has_parameter_edits());
    const auto after = all_rows(document);
    CHECK(after[0] == before[0]);
    CHECK(after[1].at(P::LightPWM) == 200.);
    document.save_archive(edited.str());

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.layer_count() == layers_total);
    const auto reread = all_rows(reopened);
    for (size_t i = 0; i < layers_total; ++i) {
        CAPTURE(i);
        REQUIRE(reread[i].size() == after[i].size());
        for (const auto &[field, value] : after[i]) {
            CAPTURE(int(field));
            CHECK(reread[i].at(field) == Approx(value).margin(1e-6));
        }
        CHECK(reopened.image(i).buf == document.image(i).buf); // pixels untouched
    }
    CHECK_FALSE(reopened.has_parameter_edits());
    CHECK(reread[0] == before[0]);
    // The independent reader agrees on what the program says.
    const auto raw = raw_text_layers(id, edited.str());
    REQUIRE(raw.size() == layers_total);
    CHECK(raw[1].lift_speed == Approx(111.).margin(1e-3));
    CHECK(raw[1].pwm == 200.);
    CHECK(raw[2].lift_speed == Approx(77.).margin(1e-3));
    CHECK(raw[3].lift_speed == Approx(77.).margin(1e-3));
    if (id != "zcodex") CHECK(raw[3].exposure == Approx(6.5).margin(1e-6));
    // Everything outside the program (layer images, previews) is carried over unchanged.
    const auto original_entries = zip_entries(exported.str()), edited_entries = zip_entries(edited.str());
    REQUIRE(original_entries.size() == edited_entries.size());
    const auto changed = program_entries(id);
    for (const auto &[name, bytes] : original_entries) {
        CAPTURE(name);
        if (!changed.count(name)) CHECK(edited_entries.at(name) == bytes);
    }
    // Unedited layers keep their program text: the first layer's lines are identical.
    if (id == "zcode") {
        const auto a = zcode_program(exported.str()), b = zcode_program(edited.str());
        const size_t first_block = 5, second_block = std::find_if(a.begin() + 6, a.end(), [](const auto &l) { return l.rfind("M6054", 0) == 0; }) - a.begin();
        CHECK(std::vector<std::string>(a.begin(), a.begin() + second_block) == std::vector<std::string>(b.begin(), b.begin() + second_block));
        (void) first_block;
    }
}

TEST_CASE("Text-program edits that the container cannot store are refused", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = text_ids();
    CAPTURE(id);
    TextFixture fx(id);
    TempFile exported("exported." + id);
    export_text(fx.f, fx.masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);
    const auto revision = document.revision();

    // Fields the container does not hold per layer, and read-only Z.
    for (const P field : {P::Exposure, P::WaitBefore, P::WaitAfter, P::WaitAfterLift, P::LiftDistance2, P::LiftSpeed2, P::RetractDistance2,
                          P::RetractSpeed2, P::LightOff})
        if (!stores(fx.f, field))
            CHECK_THROWS_WITH(document.set_layer_parameters(1, {{field, 1.}}), ContainsSubstring("cannot edit"));
    if (!fx.f.z_editable) CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::PositionZ, .12}}), ContainsSubstring("cannot edit"));
    CHECK_THROWS(document.set_layer_parameters(1, {{P::LiftSpeed, -1.}}));
    CHECK_THROWS(document.set_layer_parameters(1, {{P::LiftSpeed, std::nan("")}}));
    CHECK_THROWS(document.set_layer_parameters(layers_total, {{P::LiftSpeed, 1.}}));
    CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LightPWM, 300.}}), ContainsSubstring("PWM"));
    CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LightPWM, 10.5}}), ContainsSubstring("PWM"));
    if (id != "jxs") CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LightPWM, 0.}}), ContainsSubstring("PWM")); // PWM 0 cannot be told from "off"
    if (fx.f.z_editable) {
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::PositionZ, .2}}), ContainsSubstring("increase"));
        CHECK_THROWS(document.set_layer_parameters(1, {{P::PositionZ, 10000.}}));
    }
    if (id == "zcode") {
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LiftSpeed2, 5.}}), ContainsSubstring("lift speed 2"));
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::RetractSpeed2, 5.}}), ContainsSubstring("retract speed 2"));
        CHECK_THROWS(document.set_layer_parameters(1, {{P::LiftDistance, 0.}, {P::LiftDistance2, 2.}, {P::LiftSpeed2, 100.}}));
        CHECK_THROWS(document.set_layer_parameters(1, {{P::LiftDistance, 5.}, {P::LiftSpeed, 0.}}));
        // A retract equal to the whole lift would read back as a plain retract.
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::RetractDistance2, document.layer_parameters(1).at(P::LiftDistance)},
                                                            {P::RetractSpeed2, 90.}}), ContainsSubstring("whole lift"));
    }
    if (id == "zcodex") {
        // The retract speed exists only with a lift; a lift must exceed the layer thickness.
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LiftDistance, 0.}}), ContainsSubstring("retract speed"));
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LiftDistance, .01}}), ContainsSubstring("thickness"));
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LiftSpeed, .4}}), ContainsSubstring("lift speed"));
    }
    if (id == "jxs") {
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::LiftSpeed2, 5.}}), ContainsSubstring("lift speed 2"));
        CHECK_THROWS_WITH(document.set_layer_parameters(1, {{P::RetractSpeed2, 5.}}), ContainsSubstring("retract speed 2"));
        CHECK_THROWS(document.set_layer_parameters(1, {{P::LiftDistance, 0.}, {P::LiftDistance2, 2.}, {P::LiftSpeed2, 100.}}));
    }
    CHECK(all_rows(document) == before);
    CHECK(document.revision() == revision);
    CHECK_FALSE(document.dirty());
}

TEST_CASE("Text-program undo and redo restore the rows and the saved bytes", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = text_ids();
    CAPTURE(id);
    TextFixture fx(id);
    TempFile exported("exported." + id), restored("restored." + id), redone("redone." + id), edited("edited." + id);
    export_text(fx.f, fx.masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);
    document.set_layer_parameters(2, {{P::LiftSpeed, 99.}});
    const auto changed = all_rows(document);
    REQUIRE(changed != before);
    REQUIRE(document.undo());
    CHECK(all_rows(document) == before);
    CHECK_FALSE(document.has_parameter_edits());
    document.save_archive(restored.str());
    CHECK(read_file(restored.str()) == read_file(exported.str()));
    REQUIRE(document.redo());
    CHECK(all_rows(document) == changed);
    document.save_archive(redone.str());
    CHECK(sla::LayerDocument::load_archive(redone.str()).layer_parameters(2).at(P::LiftSpeed) == Approx(99.).margin(1e-3));
    // A whole table (one operation) is one undo step.
    auto table = changed;
    for (auto &row : table) row[P::LiftSpeed] = 66.;
    document.replace_layer_parameter_table(table);
    CHECK(document.layer_parameters(0).at(P::LiftSpeed) == Approx(66.));
    REQUIRE(document.undo());
    CHECK(all_rows(document) == changed);
    REQUIRE(document.undo());
    CHECK(all_rows(document) == before);
    CHECK_FALSE(document.undo());
}

TEST_CASE("Text-program print time and material statistics follow the edit", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = text_ids();
    CAPTURE(id);
    TextFixture fx(id);
    TempFile exported("exported." + id), edited("edited." + id), painted("painted." + id), moved("moved." + id);
    export_text(fx.f, fx.masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    const auto before = all_rows(document);
    const double time_before = text_print_time(id, exported.str());

    double delta = 0;
    for (size_t i = 0; i < layers_total; ++i) {
        if (id == "zcodex") {
            // Exposure is global in ZCodex: faster lift and retract shorten the move times.
            const double lift = before[i].at(P::LiftDistance), ls = before[i].at(P::LiftSpeed), rs = before[i].at(P::RetractSpeed);
            document.set_layer_parameters(i, {{P::LiftSpeed, ls / 2}, {P::RetractSpeed, rs / 2}});
            delta += lift * 60. / (ls / 2) - lift * 60. / ls + lift * 60. / (rs / 2) - lift * 60. / rs;
        } else {
            delta += 3.;
            document.set_layer_parameters(i, {{P::Exposure, before[i].at(P::Exposure) + 3.}});
        }
    }
    document.save_archive(edited.str());
    CHECK(text_print_time(id, edited.str()) == Approx(time_before + delta).margin(1.0001));
    CHECK(text_volume(id, edited.str()) == Approx(text_volume(id, exported.str())).epsilon(1e-6));

    // A mask edit scales the volume by the thickness-weighted lit area.
    auto second = sla::LayerDocument::load_archive(exported.str());
    auto lit = [&](const sla::LayerDocument &d, size_t layer) {
        const auto image = d.image(layer);
        return double(std::count_if(image.buf.begin(), image.buf.end(), [](uint8_t v) { return v != 0; }));
    };
    double weighted_before = 0, weighted_after = 0;
    for (size_t i = 0; i < layers_total; ++i) weighted_before += lit(second, i) * .05;
    second.invert(2);
    for (size_t i = 0; i < layers_total; ++i) weighted_after += lit(second, i) * .05;
    REQUIRE(weighted_after > weighted_before);
    second.save_archive(painted.str());
    CHECK(text_volume(id, painted.str()) == Approx(text_volume(id, exported.str()) * weighted_after / weighted_before).epsilon(1e-4));
    if (id == "zcodex") { // the changed layer's own entry is recomputed with the exporter's pixel area
        const auto layer = json_entry(zip_entries(painted.str()), "ResinMetadata")["Layers"][2]["UsedMaterialVolume"].get<double>();
        CHECK(layer == Approx(lit(second, 2) * (74.67 / 1440) * (132.88 / 2560) * .05 / 1000.).epsilon(1e-5));
    }

    if (fx.f.z_editable) { // a Z edit moves the thickness weights
        auto third = sla::LayerDocument::load_archive(exported.str());
        third.set_layer_parameters(2, {{P::PositionZ, .18}});
        third.save_archive(moved.str());
        const double lits[4] = {lit(third, 0), lit(third, 1), lit(third, 2), lit(third, 3)};
        const double old_weight = (lits[0] + lits[1] + lits[2] + lits[3]) * .05;
        const double new_weight = lits[0] * .05 + lits[1] * .05 + lits[2] * .08 + lits[3] * .02;
        CHECK(text_volume(id, moved.str()) == Approx(text_volume(id, exported.str()) * new_weight / old_weight).epsilon(1e-4));
        auto reopened = sla::LayerDocument::load_archive(moved.str());
        CHECK(reopened.layer_parameters(2).at(P::PositionZ) == Approx(.18).margin(1e-6));
        CHECK(reopened.layer_parameters(3).at(P::PositionZ) == Approx(.20).margin(1e-6));
    }
}

TEST_CASE("Text-program edited rows survive a layer-project round trip and parameter operations", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = text_ids();
    CAPTURE(id);
    TextFixture fx(id);
    TempFile exported("exported." + id), expected("expected." + id), actual("actual." + id), project("project.sla-project"),
             operated("operated." + id);
    export_text(fx.f, fx.masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    document.set_layer_parameters(1, {{P::LiftSpeed, 123.}});
    document.set_layer_parameters(3, {{P::LightPWM, 180.}});
    document.paint(2, 20, 20, 4, 255);
    const sla::LayerExportProfile profile{unsigned(fx.columns), unsigned(fx.rows), 100., 60., 1.1, .1, 200.};
    sla::ProjectRasterState state{document, {}, "", profile, sla::ProjectLayerOrigin::ImportedArchive};
    REQUIRE_NOTHROW(sla::save_layer_project(project.str(), state));
    auto restored = sla::read_project_layers(project.str());
    REQUIRE(restored.has_value());
    CHECK(all_rows(restored->document) == all_rows(document));
    document.save_archive(expected.str());
    restored->document.save_archive(actual.str());
    CHECK(same_saved_content(actual.str(), expected.str()));
    CHECK(restored->document.has_parameter_edits());

    // A UVtools-style parameter operation: one undo step, saved and reread.
    auto source = sla::LayerDocument::load_archive(exported.str());
    CHECK(source.parameter_operation_refusal().empty());
    const auto table = source.parameter_table();
    REQUIRE(table.size() == layers_total);
    CHECK(table.front().is_bottom);
    const auto before = all_rows(source);
    REQUIRE(source.apply_parameter_operation(
        [&](std::vector<sla::layerops::Mask> &, std::vector<double> &, std::vector<sla::layerops::LayerParams> &rows, const sla::LayerProgress &) {
            for (auto &row : rows) row.lift_speed += 10.;
            return sla::LayerDocument::LayerListChange{};
        }, false, .05));
    for (size_t i = 0; i < layers_total; ++i) CHECK(source.layer_parameters(i).at(P::LiftSpeed) == Approx(before[i].at(P::LiftSpeed) + 10.).margin(1e-6));
    source.save_archive(operated.str());
    CHECK(sla::LayerDocument::load_archive(operated.str()).layer_parameters(3).at(P::LiftSpeed) == Approx(before[3].at(P::LiftSpeed) + 10.).margin(1e-3));
    REQUIRE(source.undo());
    CHECK(all_rows(source) == before);
    CHECK_FALSE(source.undo());
    // An operation that needs a field the program does not store is refused whole.
    if (!stores(fx.f, P::Exposure))
        CHECK_THROWS(source.apply_parameter_operation(
            [](std::vector<sla::layerops::Mask> &, std::vector<double> &, std::vector<sla::layerops::LayerParams> &rows, const sla::LayerProgress &) {
                for (auto &row : rows) row.exposure_s += 1.;
                return sla::LayerDocument::LayerListChange{};
            }, false, .05));
    CHECK(all_rows(source) == before);
}

TEST_CASE("Text-program files not shaped like the exporter's stay read-only", "[sla_layers][sla_reader_parameters]")
{
    const std::string id = text_ids();
    CAPTURE(id);
    TextFixture fx(id);
    TempFile exported("exported." + id), foreign("foreign." + id), saved("saved." + id);
    export_text(fx.f, fx.masks, exported.str());
    auto entries = zip_entries(exported.str());
    // Reformatting / extending the program makes the layout unrecognizable: no guessing.
    const std::string name = id == "zcode" ? "lcd.gcode" : id == "zcodex" ? "ResinGCodeData" : "control.json";
    std::string text(entries.at(name).begin(), entries.at(name).end());
    if (id == "jxs") text = " " + text; // valid JSON, but not any layout that dumps back to its own bytes
    else if (id == "zcodex") text.insert(text.find("<Delay_blank>"), "G4 P1\n");
    else text = "not-a-protocol-line\n" + text;
    entries[name] = std::vector<uint8_t>(text.begin(), text.end());
    {
        Zipper zip(foreign.str());
        for (const auto &[entry, bytes] : entries) zip.add_entry(entry, bytes.data(), bytes.size());
        zip.finalize();
    }
    auto document = sla::LayerDocument::load_archive(foreign.str());
    CHECK(document.parameter_capabilities().empty());
    CHECK_FALSE(document.parameter_operation_refusal().empty());
    CHECK_THROWS(document.set_layer_parameters(0, {{P::LiftSpeed, 5.}}));
    // Its masks still save, and an unchanged document stays byte identical.
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == read_file(foreign.str()));
}

// =================================================================================================
// Wave 21: files written by other programs (UVtools). The fixtures in tests/data/foreign_text_params
// were produced by `UVtoolsCmd convert` (+ `set-properties` for per-layer values) from a tiny sliced
// file: they differ from the exporter's text (UVtools spells a zero wait "G4 P0;", writes the ZCodex
// metadata with its own key order, ...) but hold the same per-layer program.
// =================================================================================================
namespace {
// SLA_FOREIGN_PARAMS_OUTPUT=<dir> keeps the edited files for the UVtools readback.
void keep_for_oracle(const std::string &path, const char *name)
{
    if (const char *folder = std::getenv("SLA_FOREIGN_PARAMS_OUTPUT"))
        boost::filesystem::copy_file(path, boost::filesystem::path(folder) / name, boost::filesystem::copy_options::overwrite_existing);
}
std::string foreign_fixture(const char *name) { return std::string(TEST_DATA_DIR PATH_SEPARATOR) + "foreign_text_params/" + name; }


void write_zip(const std::string &path, const std::map<std::string, std::vector<uint8_t>> &entries)
{
    Zipper zip(path);
    for (const auto &[entry, bytes] : entries) zip.add_entry(entry, bytes.data(), bytes.size());
    zip.finalize();
}
} // namespace

TEST_CASE("Foreign ZCode (UVtools writer) exposes and edits its per-layer program", "[sla_layers][sla_reader_parameters][sla_foreign_params]")
{
    const auto source = foreign_fixture("uvtools_rich.zcode");
    TempFile edited("edited.zcode");
    auto document = sla::LayerDocument::load_archive(source);
    REQUIRE(document.archive_format() == "zcode");
    REQUIRE(document.layer_count() == 3);
    const auto &family = text_family("zcode");
    for (const auto &capability : document.parameter_capabilities()) CHECK(capability.editable);
    REQUIRE(document.parameter_capabilities().size() == family.fields.size());
    const auto before = all_rows(document);
    // What UVtools wrote for the second layer (see the fixture's lcd.gcode).
    CHECK(before[1].at(P::LiftDistance) == Approx(6.).margin(1e-6));
    CHECK(before[1].at(P::LiftDistance2) == Approx(2.).margin(1e-6));
    CHECK(before[1].at(P::WaitAfterLift) == Approx(.5).margin(1e-6));
    CHECK(before[1].at(P::Exposure) == Approx(7.).margin(1e-6));
    CHECK(before[1].at(P::LightPWM) == 199.); // 200 through the 0-300 power scale
    CHECK(before[2].at(P::WaitBefore) == Approx(1.5).margin(1e-6));
    CHECK(before[2].at(P::WaitAfter) == Approx(.25).margin(1e-6));
    CHECK(before[2].at(P::RetractDistance2) == Approx(1.).margin(1e-6));
    CHECK(before[0].at(P::WaitBefore) == 0.);

    // Edit the layers around the untouched middle one; layer 1 (index 0) goes to a zero pre-cure wait
    // from a nonzero one, which must be spelled the way this file spells it.
    document.set_layer_parameters(0, {{P::LiftSpeed, 120.}, {P::WaitBefore, 2.}});
    document.set_layer_parameters(2, {{P::Exposure, 9.}, {P::WaitBefore, 0.}, {P::PositionZ, .16}});
    document.save_archive(edited.str());
    keep_for_oracle(edited.str(), "edited-foreign.zcode");

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    REQUIRE(reopened.layer_count() == 3);
    const auto after = all_rows(reopened);
    CHECK(after[0].at(P::LiftSpeed) == Approx(120.).margin(1e-6));
    CHECK(after[0].at(P::WaitBefore) == Approx(2.).margin(1e-6));
    CHECK(after[1] == before[1]);
    CHECK(after[2].at(P::Exposure) == Approx(9.).margin(1e-6));
    CHECK(after[2].at(P::WaitBefore) == 0.);
    CHECK(after[2].at(P::PositionZ) == Approx(.16).margin(1e-6));
    CHECK(reopened.image(1).buf == document.image(1).buf);

    // The independent reader and the program text: layer 2 is byte for byte the foreign one,
    // the rewritten blocks use UVtools' "G4 P0;", and the tail is carried over.
    const auto original_lines = zcode_program(source), edited_lines = zcode_program(edited.str());
    auto block_of = [](const std::vector<std::string> &lines, size_t layer) {
        const auto begin = std::find(lines.begin(), lines.end(), "M6054 \"" + std::to_string(layer) + ".png\";");
        const auto end = std::find_if(begin + 1, lines.end(), [](const auto &l) { return l.rfind("M6054", 0) == 0; });
        return std::vector<std::string>(begin, end);
    };
    CHECK(block_of(original_lines, 2) == block_of(edited_lines, 2));
    for (const auto &line : edited_lines) CHECK(line != "G4 P.0;");
    CHECK(std::count(edited_lines.begin(), edited_lines.end(), "G4 P0;") == 2); // layers 2 (untouched) and 3; layer 1 now waits 2 s
    CHECK(std::vector<std::string>(original_lines.begin(), original_lines.begin() + 5) == std::vector<std::string>(edited_lines.begin(), edited_lines.begin() + 5));
    CHECK(edited_lines.back() == original_lines.back());
    // Everything else in the container is carried over byte for byte (images, preview); only the program and manifest change.
    const auto original_entries = zip_entries(source), edited_entries = zip_entries(edited.str());
    REQUIRE(original_entries.size() == edited_entries.size());
    for (const auto &[name, bytes] : original_entries)
        if (name != "lcd.gcode" && name != "task.xml") CHECK(edited_entries.at(name) == bytes);
    // Unchanged documents stay byte identical.
    TempFile same("same.zcode");
    sla::LayerDocument::load_archive(source).save_archive(same.str());
    CHECK(read_file(same.str()) == read_file(source));
}

TEST_CASE("Foreign ZCodex (UVtools writer) edits keep its metadata layout", "[sla_layers][sla_reader_parameters][sla_foreign_params]")
{
    const auto source = foreign_fixture("uvtools.zcodex");
    TempFile edited("edited.zcodex");
    auto document = sla::LayerDocument::load_archive(source);
    REQUIRE(document.archive_format() == "zcodex");
    REQUIRE(document.layer_count() == 3);
    REQUIRE_FALSE(document.parameter_capabilities().empty());
    for (const auto &capability : document.parameter_capabilities()) CHECK(capability.editable);
    const auto before = all_rows(document);
    CHECK(before[0].at(P::LiftDistance) == Approx(6.).margin(1e-6));
    CHECK(before[0].at(P::PositionZ) == Approx(.05).margin(1e-6));

    document.set_layer_parameters(1, {{P::LiftSpeed, 140.}, {P::RetractSpeed, 95.}, {P::LightPWM, 180.}});
    document.set_layer_parameters(2, {{P::PositionZ, .16}, {P::LiftDistance, 5.}});
    document.save_archive(edited.str());
    keep_for_oracle(edited.str(), "edited-foreign.zcodex");

    auto reopened = sla::LayerDocument::load_archive(edited.str());
    const auto after = all_rows(reopened);
    CHECK(after[0] == before[0]);
    CHECK(after[1].at(P::LiftSpeed) == 140.);
    CHECK(after[1].at(P::RetractSpeed) == 95.);
    CHECK(after[1].at(P::LightPWM) == 180.);
    CHECK(after[2].at(P::PositionZ) == Approx(.16).margin(1e-6));
    CHECK(after[2].at(P::LiftDistance) == Approx(5.).margin(1e-6));
    const auto original_entries = zip_entries(source), edited_entries = zip_entries(edited.str());
    REQUIRE(original_entries.size() == edited_entries.size());
    for (const auto &[name, bytes] : original_entries)
        if (name != "ResinGCodeData" && name != "ResinMetadata" && name != "UserSettingsData" && name != "ZCodeMetadata") CHECK(edited_entries.at(name) == bytes);
    // The metadata keeps UVtools' key order (the exporter's JSON sorts its keys): same keys, same order.
    auto keys = [](const std::string &text) {
        std::vector<std::string> out;
        const auto json = nlohmann::ordered_json::parse(text);
        for (const auto &[key, value] : json.items()) out.push_back(key);
        return out;
    };
    for (const char *name : {"ResinMetadata", "UserSettingsData", "ZCodeMetadata"}) {
        const std::string a(original_entries.at(name).begin(), original_entries.at(name).end()),
                          b(edited_entries.at(name).begin(), edited_entries.at(name).end());
        CHECK(keys(a) == keys(b));
    }
    CHECK(keys(entry_text(source, "ResinMetadata")).front() == "Guid");
    // The untouched first layer's program lines are unchanged.
    const auto original_text = entry_text(source, "ResinGCodeData"), edited_text = entry_text(edited.str(), "ResinGCodeData");
    const auto first_end = original_text.find("<Slice> 1");
    CHECK(edited_text.substr(0, first_end) == original_text.substr(0, first_end));
    CHECK(edited_text.substr(edited_text.rfind("G1 Z40.0")) == original_text.substr(original_text.rfind("G1 Z40.0")));
}

TEST_CASE("Foreign JXS control.json in another JSON layout is edited without reformatting it", "[sla_layers][sla_reader_parameters][sla_foreign_params]")
{
    TextFixture fx("jxs");
    TempFile exported("exported.jxs"), foreign("foreign.jxs"), edited("edited.jxs");
    export_text(fx.f, fx.masks, exported.str());
    auto entries = zip_entries(exported.str());
    // Another writer: members in another order, four-space indent, a final line break.
    auto control = nlohmann::ordered_json::parse(entry_text(exported.str(), "control.json"));
    nlohmann::ordered_json reordered = nlohmann::ordered_json::object();
    for (const char *key : {"total_time", "used_material", "total_slices", "action_list"}) reordered[key] = control[key];
    const std::string foreign_text = reordered.dump(4) + "\n";
    entries["control.json"] = std::vector<uint8_t>(foreign_text.begin(), foreign_text.end());
    write_zip(foreign.str(), entries);

    auto document = sla::LayerDocument::load_archive(foreign.str());
    REQUIRE_FALSE(document.parameter_capabilities().empty());
    const auto before = all_rows(document);
    document.set_layer_parameters(1, {{P::Exposure, 6.5}, {P::LiftSpeed, 111.}});
    document.save_archive(edited.str());
    auto reopened = sla::LayerDocument::load_archive(edited.str());
    CHECK(all_rows(reopened)[1].at(P::Exposure) == Approx(6.5).margin(1e-6));
    CHECK(all_rows(reopened)[1].at(P::LiftSpeed) == Approx(111.).margin(1e-6));
    CHECK(all_rows(reopened)[0] == before[0]);
    const auto saved = nlohmann::ordered_json::parse(entry_text(edited.str(), "control.json"));
    std::vector<std::string> order;
    for (const auto &[key, value] : saved.items()) order.push_back(key);
    CHECK(order == std::vector<std::string>{"total_time", "used_material", "total_slices", "action_list"});
    const auto saved_text = entry_text(edited.str(), "control.json");
    CHECK(saved_text == saved.dump(4) + "\n");
}

TEST_CASE("Foreign JXS without an action list keeps its parameters read-only", "[sla_layers][sla_reader_parameters][sla_foreign_params]")
{
    // UVtools' own JXS writer stores its settings in control.json and no action list, so no per-layer program exists.
    TextFixture fx("jxs");
    TempFile exported("exported.jxs"), foreign("foreign.jxs"), saved("saved.jxs");
    export_text(fx.f, fx.masks, exported.str());
    auto entries = zip_entries(exported.str());
    const std::string text = "{\n  \"Action\": \"print\",\n  \"LayerCount\": 4\n}";
    entries["control.json"] = std::vector<uint8_t>(text.begin(), text.end());
    write_zip(foreign.str(), entries);
    auto document = sla::LayerDocument::load_archive(foreign.str());
    CHECK(document.parameter_capabilities().empty());
    CHECK_THROWS(document.set_layer_parameters(0, {{P::LiftSpeed, 5.}}));
    document.save_archive(saved.str());
    CHECK(read_file(saved.str()) == read_file(foreign.str()));
}

TEST_CASE("ZCode light PWM survives the 0-300 power scale for every value", "[sla_layers][sla_reader_parameters]")
{
    TextFixture fx("zcode");
    TempFile exported("exported.zcode"), edited("edited.zcode");
    export_text(fx.f, fx.masks, exported.str());
    auto document = sla::LayerDocument::load_archive(exported.str());
    // Every whole PWM in 1..255, spread over the four layers and several saves.
    for (unsigned first = 1; first <= 255; first += layers_total) {
        auto table = all_rows(document);
        for (size_t i = 0; i < layers_total; ++i) table[i][P::LightPWM] = double(std::min(255u, first + unsigned(i)));
        document.replace_layer_parameter_table(table);
        document.save_archive(edited.str());
        const auto reread = all_rows(sla::LayerDocument::load_archive(edited.str()));
        for (size_t i = 0; i < layers_total; ++i) REQUIRE(reread[i].at(P::LightPWM) == double(std::min(255u, first + unsigned(i))));
    }
}
