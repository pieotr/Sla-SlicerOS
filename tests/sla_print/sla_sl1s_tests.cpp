#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <fstream>
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"

using namespace Slic3r;

TEST_CASE("SL1S is a persistent export choice with native editable masks", "[sla_reference][sla_sl1s]")
{
    const bool portrait = GENERATE(false, true);
    const bool mirror_x = GENERATE(false, true);
    const bool mirror_y = GENERATE(false, true);
    const std::string format = GENERATE("SL1", "SL1S");
    CAPTURE(portrait, mirror_x, mirror_y, format);
    const auto *entry = get_archive_entry(format.c_str());
    REQUIRE(entry != nullptr);
    REQUIRE(std::string(entry->ext) == (format == "SL1S" ? "sl1s" : "sl1"));
    REQUIRE(get_archive_entry_by_extension(entry->ext) == entry);
    REQUIRE(sla_archive_filename("model.cxdlpv4", entry->ext) == std::string("model.") + entry->ext);
    DynamicPrintConfig cfg;
    cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", format);
    cfg.set("printer_model", "SL1S");
    cfg.set("display_width", 16.); cfg.set("display_height", 4.);
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    cfg.set("display_mirror_x", mirror_x); cfg.set("display_mirror_y", mirror_y);
    cfg.set("max_print_height", 150.);
    cfg.set("initial_layer_height", .1); cfg.set("layer_height", .05);
    cfg.set("sla_bottom_layers", 1); cfg.set("sla_transition_layers", 1);
    cfg.set("initial_exposure_time", 30.); cfg.set("exposure_time", 3.);
    cfg.set("sla_bottom_wait_before_cure", 2.5); cfg.set("sla_wait_before_cure", 1.25);
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("sl1s-%%%%");
        bool keep = false;
        ~Directory() { if (!keep) { boost::system::error_code error; boost::filesystem::remove_all(path, error); } }
    } directory;
    if (const char *path = std::getenv("SLA_SL1S_FIXTURES")) { directory.path = path; directory.keep = true; }
    boost::filesystem::create_directories(directory.path);
    const std::string stem = std::string(portrait ? "portrait" : "landscape") + (mirror_x ? "-x" : "-nx") + (mirror_y ? "-y" : "-ny");
    const auto path = directory.path / (stem + "." + entry->ext);
    const auto profile_path = directory.path / (stem + "-" + format + ".ini");
    cfg.save(profile_path.string());
    DynamicPrintConfig restored_cfg;
    restored_cfg.load(profile_path.string(), ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(restored_cfg.opt_string("sla_archive_format") == format);
    std::vector<uint8_t> pixels(32, 0);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 8);
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), portrait ? 4 : 8, portrait ? 8 : 4, 1);
    const auto source = sla::LayerDocument::from_rasters({png, png, png});
    auto job = SLAPrint::create_raster_job(restored_cfg, source, {.1, .15, .2});
    REQUIRE_NOTHROW(job->export_print(path.string(), {}, "fixture"));
    auto loaded = sla::LayerDocument::load_archive(path.string());
    REQUIRE(loaded.archive_format() == "SL1"); // Shared native SL1 schema.
    REQUIRE(loaded.layer_count() == 3);
    REQUIRE(loaded.image(0).buf == pixels);
    REQUIRE(SLAArchiveReader::create(path.string(), "") != nullptr);
    loaded.invert(1);
    const auto edited_path = directory.path / ("edited-" + stem + "." + entry->ext);
    REQUIRE_NOTHROW(loaded.save_archive(edited_path.string()));
    const auto edited = sla::LayerDocument::load_archive(edited_path.string());
    REQUIRE(edited.image(0).buf == pixels);
    REQUIRE(edited.image(1).buf == loaded.image(1).buf);
    REQUIRE(edited.image(2).buf == pixels);
    auto read = [](const boost::filesystem::path &p) {
        std::ifstream in(p.string(), std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}};
    };
    const auto previous = read(path);
    auto invalid = SLAPrint::create_raster_job(cfg, source, {.1, .18, .23});
    REQUIRE_THROWS(invalid->export_print(path.string()));
    REQUIRE(read(path) == previous);
    for (const auto &file : boost::filesystem::directory_iterator(directory.path))
        REQUIRE(file.path().filename().string().find(".sl1-") != 0);
}
