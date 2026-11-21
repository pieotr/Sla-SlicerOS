// sla_archive_header_mirror: the mirror/flip flag written into archive headers.
// `auto` must reproduce what each writer has always emitted; the explicit values
// must land in the documented header field. The layer images are never changed.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <vector>
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "sla_archive_test_config.hpp"

using namespace Slic3r;

namespace {
constexpr size_t HDR_CXDLPV4=73,HDR_OSLA=174,HDR_OSF=349881;

struct Flag { bool x, y; };

std::string blob_of(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::string raw{std::istreambuf_iterator<char>(in), {}};
    if (raw.size() < 4 || raw[0] != 'P' || raw[1] != 'K') return raw;
    std::string text;
    MZ_Archive archive;
    REQUIRE(open_zip_reader(&archive.arch, path));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive.arch); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&archive.arch, i, &stat));
        if (mz_zip_reader_is_file_a_directory(&archive.arch, i)) continue;
        std::vector<char> data(size_t(stat.m_uncomp_size));
        if (!data.empty()) REQUIRE(mz_zip_reader_extract_to_mem(&archive.arch, i, data.data(), data.size(), 0));
        const std::string name = stat.m_filename;
        // Only small text-like entries can hold a header flag (skip layer images).
        if (name.find(".png") == std::string::npos && name.find(".bin") == std::string::npos &&
            name.find("Img") == std::string::npos && data.size() < (1u << 20))
            text.append(data.begin(), data.end()).push_back('\n');
    }
    close_zip_reader(&archive.arch);
    return text;
}

std::string export_blob(const std::string &id, const std::string &mode, bool mx, bool my)
{
    SLAFullPrintConfig full;
    Test::configure_archive_fixture(full, id);
    full.set("display_mirror_x", mx);
    full.set("display_mirror_y", my);
    DynamicPrintConfig cfg;
    cfg.apply(full);
    if (!mode.empty()) cfg.set_deserialize_strict("sla_archive_header_mirror", mode);
    const size_t width = 200, height = 120;
    std::vector<uint8_t> pixels(width * height, 0);
    for (size_t y = 0; y < 40; ++y)
        for (size_t x = 0; x < 60; ++x) pixels[y * width + x] = 255; // asymmetric
    std::vector<sla::EncodedRaster> masks;
    for (int i = 0; i < 4; ++i) masks.push_back(sla::PNGRasterEncoder{}(pixels.data(), width, height, 1));
    const auto document = sla::LayerDocument::from_rasters(std::move(masks));
    const auto print = SLAPrint::create_raster_job(cfg, document, {.05, .1, .15, .2});
    const auto dir = boost::filesystem::unique_path("hdrmirror-%%%%%%");
    boost::filesystem::create_directories(dir);
    const auto path = dir / ("job." + std::string(get_archive_entry(id.c_str())->ext));
    print->export_layer_document(document, id, path.string(), {}, "job");
    auto text = blob_of(path.string());
    boost::system::error_code ignored;
    boost::filesystem::remove_all(dir, ignored);
    return text;
}

uint32_t u32(const std::string &b, size_t at)
{
    REQUIRE(at + 4 <= b.size());
    uint32_t v = 0;
    for (unsigned n = 0; n < 4; ++n) v |= uint32_t(uint8_t(b[at + n])) << (8 * n);
    return v;
}

int rx(const std::string &b, const char *pattern)
{
    std::smatch m;
    REQUIRE(std::regex_search(b, m, std::regex(pattern)));
    return m[1] == "true" || m[1] == "True" || m[1] == "LCD_mirror" ? 1 : (m[1] == "false" || m[1] == "False" || m[1] == "Normal" ? 0 : std::stoi(m[1]));
}

struct Format {
    const char *id;
    // Reads the header field the way the format encodes it.
    int (*read)(const std::string &);
    // Encoded value for a requested mirror.
    int (*encode)(Flag);
    // What `auto` writes for display_mirror_x=true, display_mirror_y=false.
    Flag legacy_auto; // value in the format's own encoding is encode(legacy_auto)
    bool config_mirror_y_allowed;
};

int bit(Flag f) { return f.x || f.y; }
int bits(Flag f) { return (f.x ? 1 : 0) | (f.y ? 2 : 0); }
int horiz_only(Flag f) { return f.x ? 1 : 0; }

const std::vector<Format> &formats()
{
    static const std::vector<Format> list = {
        {"cbddlp", [](const std::string &b) { return int(u32(b, 80)); }, bit, {true, false}, true},
        {"ctb_v1", [](const std::string &b) { return int(u32(b, 80)); }, bit, {true, false}, false},
        {"phz", [](const std::string &b) { return int(u32(b, 52)); }, bit, {true, false}, true},
        {"fdg", [](const std::string &b) { return int(u32(b, 16)); }, bit, {true, false}, false},
        {"cxdlpv4", [](const std::string &b) { return int(u32(b, HDR_CXDLPV4)); }, bit, {false, false}, true},
        {"goo", [](const std::string &b) { return int(uint8_t(b[195318])) | (int(uint8_t(b[195319])) << 1); }, bits, {true, false}, true},
        {"osla", [](const std::string &b) { return int(uint8_t(b[HDR_OSLA])); }, bits, {false, false}, true},
        {"osf", [](const std::string &b) { return int(uint8_t(b[HDR_OSF])); }, bits, {false, false}, true},
        {"chitubox_zip", [](const std::string &b) { return rx(b, "projectType[:=\" ]+(\\w+)"); }, bit, {false, false}, true},
        {"cws", [](const std::string &b) { return rx(b, "Flip X = (\\w+)") | (rx(b, "Flip Y = (\\w+)") << 1); }, bits, {false, false}, true},
        {"klipper", [](const std::string &b) { return rx(b, ";Mirror:(\\d)"); }, bits, {true, false}, true},
        {"vdt", [](const std::string &b) { return rx(b, "\"x_mirror\"\\s*:\\s*(\\w+)") | (rx(b, "\"y_mirror\"\\s*:\\s*(\\w+)") << 1); }, bits, {true, false}, true},
        {"nanodlp", [](const std::string &b) { return rx(b, "\"ImageMirror\"\\s*:\\s*(\\d)"); }, horiz_only, {true, false}, false},
    };
    return list;
}

} // namespace

TEST_CASE("Header mirror option is an expert printer-preset setting with a historical default",
          "[sla_header_mirror][sla_profiles]")
{
    const auto &opts = Preset::sla_printer_options();
    REQUIRE(std::find(opts.begin(), opts.end(), "sla_archive_header_mirror") != opts.end());
    const auto *def = print_config_def.get("sla_archive_header_mirror");
    REQUIRE(def != nullptr);
    CHECK(def->mode == comExpert);
    REQUIRE(def->enum_def);
    CHECK(def->enum_def->values().size() == 5);
    SLAPrinterConfig printer;
    CHECK(printer.sla_archive_header_mirror.value == slahmAuto);
    // ini-style round trip
    printer.sla_archive_header_mirror.value = slahmVertical;
    DynamicPrintConfig dyn;
    dyn.apply(printer);
    CHECK(dyn.opt_serialize("sla_archive_header_mirror") == "vertical");
    DynamicPrintConfig loaded;
    loaded.apply(SLAPrinterConfig{});
    loaded.set_deserialize_strict("sla_archive_header_mirror", "both");
    SLAPrinterConfig restored;
    restored.apply(loaded);
    CHECK(restored.sla_archive_header_mirror.value == slahmBoth);
    for (const char *v : {"auto", "none", "horizontal", "vertical", "both"}) {
        CAPTURE(v);
        DynamicPrintConfig c;
        c.apply(SLAPrinterConfig{});
        REQUIRE_NOTHROW(c.set_deserialize_strict("sla_archive_header_mirror", v));
        CHECK(c.opt_serialize("sla_archive_header_mirror") == v);
    }
}

TEST_CASE("Header mirror auto keeps every writer's historical bytes", "[sla_header_mirror]")
{
    for (const auto &f : formats()) {
        CAPTURE(f.id);
        const auto absent = export_blob(f.id, "", true, false);          // option never set = default
        const auto automatic = export_blob(f.id, "auto", true, false);
        // Some writers embed a creation time (GOO) or ZIP mtimes with one second resolution: a pair of
        // exports straddling a second boundary may differ, so accept either neighbouring pair.
        const bool identical = absent == automatic || automatic == export_blob(f.id, "", true, false);
        CHECK(identical);
        // Historical value: constant for CXDLPv4/OSLA/OSF/Chitubox ZIP/CWS, display_mirror_* for the others.
        CHECK(f.read(absent) == f.encode(f.legacy_auto));
        if (f.config_mirror_y_allowed) {
            CHECK(f.read(export_blob(f.id, "auto", false, true)) ==
                  f.encode(f.legacy_auto.x || f.legacy_auto.y ? Flag{false, true} : Flag{false, false}));
            CHECK(f.read(export_blob(f.id, "auto", false, false)) == f.encode({false, false}));
        }
    }
}

TEST_CASE("Header mirror overrides land in the documented header field", "[sla_header_mirror]")
{
    const std::map<std::string, Flag> modes = {
        {"none", {false, false}}, {"horizontal", {true, false}}, {"vertical", {false, true}}, {"both", {true, true}}};
    for (const auto &f : formats())
        for (const auto &[mode, flag] : modes) {
            CAPTURE(f.id, mode);
            // Independent of display_mirror_*: the image configuration (x mirrored) stays the same.
            CHECK(f.read(export_blob(f.id, mode, true, false)) == f.encode(flag));
        }
}
