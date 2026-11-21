#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <fstream>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/Format/KlipperArchive.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
using namespace Slic3r;

TEST_CASE("Klipper MSLA archive preserves masks and two-stage parameters", "[sla_reference][sla_klipper]")
{
    const bool rgb = GENERATE(false, true), portrait = GENERATE(false, true);
    const bool mirror_x = GENERATE(false, true), mirror_y = GENERATE(false, true);
    const int mode = GENERATE(0, 1, 2);
    CAPTURE(rgb, portrait, mirror_x, mirror_y, mode);
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    const std::string format = rgb ? "klipper_rgb" : "klipper";
    const std::string extension = rgb ? "rgb.zip" : "zip";
    cfg.set("sla_archive_format", format);
    cfg.set("printer_model", std::string("Klipper μLED"));
    cfg.option("sla_material_settings_id", true);
    cfg.set("sla_material_settings_id", std::string("Resin Żółty"));
    cfg.set("display_width", 16.); cfg.set("display_height", 4.);
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    cfg.set("display_mirror_x", mirror_x); cfg.set("display_mirror_y", mirror_y);
    cfg.set("max_print_height", 200.);
    cfg.set("layer_height", .05); cfg.set("initial_layer_height", .1);
    cfg.set("sla_bottom_layers", 1); cfg.set("sla_transition_layers", 1);
    cfg.set("initial_exposure_time", 30.); cfg.set("exposure_time", 3.);
    cfg.set("sla_bottom_lift_height", 5.); cfg.set("sla_lift_height", 4.);
    cfg.set("sla_bottom_lift_height2", mode ? 3. : 0.); cfg.set("sla_lift_height2", mode ? 2. : 0.);
    cfg.set("sla_bottom_lift_speed", 60.); cfg.set("sla_lift_speed", 90.);
    cfg.set("sla_bottom_lift_speed2", 120.); cfg.set("sla_lift_speed2", 150.);
    cfg.set("sla_bottom_retract_speed", 120.); cfg.set("sla_retract_speed", 150.);
    cfg.set("sla_bottom_retract_height2", mode ? 2. : 0.); cfg.set("sla_retract_height2", mode ? 1. : 0.);
    cfg.set("sla_bottom_retract_speed2", 180.); cfg.set("sla_retract_speed2", 210.);
    cfg.set("sla_bottom_wait_before_cure", mode == 2 ? 0. : 2.5); cfg.set("sla_wait_before_cure", mode == 2 ? 0. : 1.25);
    cfg.set("sla_bottom_wait_after_cure", .3); cfg.set("sla_wait_after_cure", .2);
    cfg.set("sla_bottom_wait_after_lift", .75); cfg.set("sla_wait_after_lift", .5);
    cfg.set("sla_bottom_light_pwm", 200); cfg.set("sla_light_pwm", 180);
    cfg.set("material_density", 1.1); cfg.set("bottle_volume", 500.); cfg.set("bottle_cost", 50.);
    struct Directory {
        boost::filesystem::path path = boost::filesystem::unique_path("klipper-%%%%"); bool keep = false;
        ~Directory() { if (!keep) { boost::system::error_code e; boost::filesystem::remove_all(path, e); } }
    } directory;
    if (const char *path = std::getenv("SLA_KLIPPER_FIXTURES")) { directory.path = path; directory.keep = true; }
    boost::filesystem::create_directories(directory.path);
    const std::string stem = std::string(portrait ? "portrait" : "landscape") + (mirror_x ? "-x" : "-nx") +
                             (mirror_y ? "-y-" : "-ny-") + std::to_string(mode);
    const auto path = directory.path / (stem + "." + extension);
    cfg.save((directory.path / (stem + (rgb ? "-rgb.ini" : "-mono.ini"))).string());
    std::vector<uint8_t> pixels(32);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 8);
    const auto png = sla::PNGRasterEncoder{}(pixels.data(), portrait ? 4 : 8, portrait ? 8 : 4, 1);
    const auto document = sla::LayerDocument::from_rasters({png, png, png});
    auto job = SLAPrint::create_raster_job(cfg, document, {.1, .15, .2});
    ThumbnailsList previews(2);
    for (size_t i = 0; i < previews.size(); ++i) {
        previews[i].set(3, 2);
        for (size_t j = 0; j < previews[i].pixels.size(); j += 4) {
            previews[i].pixels[j] = uint8_t(j + 10 * i); previews[i].pixels[j + 1] = 80;
            previews[i].pixels[j + 2] = 180; previews[i].pixels[j + 3] = 255;
        }
    }
    REQUIRE_NOTHROW(job->export_print(path.string(), previews, "fixture"));
    const auto archive = read_zipper_archive(path.string(), {""}, {});
    REQUIRE(archive.entries.size() == 7); // marker, script, 3 masks, 2 previews
    size_t masks = 0; bool marker = false, commands = false;
    for (const auto &entry : archive.entries) {
        if (entry.fname == ".klipper") { marker = true; REQUIRE(entry.buf.empty()); }
        else if (entry.fname == "job.gcode") {
            commands = true; const std::string text(entry.buf.begin(), entry.buf.end());
            REQUIRE(text.find("MSLA_DISPLAY_VALIDATE RESOLUTION=") != std::string::npos);
            REQUIRE(text.find("M1451 F\"1.png\"") != std::string::npos);
            REQUIRE(text.find("M1400 S200 P30000") != std::string::npos);
            REQUIRE(text.find("PRINT_END") != std::string::npos);
            if (mode == 2) REQUIRE(text.find("G4 P.0") != std::string::npos);
        } else if (entry.fname == "1.png" || entry.fname == "2.png" || entry.fname == "3.png") {
            ++masks; png::ImageGreyscale image;
            REQUIRE(entry.buf.size() > 26);
            REQUIRE(entry.buf[25] == (rgb ? 2 : 0)); // PNG24 / grayscale IHDR
            REQUIRE(png::decode_color_png({entry.buf.data(), entry.buf.size()}, image));
            REQUIRE(image.cols == (portrait ? 4 : 8)); REQUIRE(image.rows == (portrait ? 8 : 4));
            REQUIRE(image.buf == pixels);
        }
    }
    REQUIRE(marker); REQUIRE(commands); REQUIRE(masks == 3);
    // Invalid commands must not replace an earlier valid result.
    auto bytes = [&]() { std::ifstream in(path.string(), std::ios::binary); return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}}; };
    const auto previous = bytes();
    for (const char *key : {"sla_lift_speed", "sla_light_pwm"}) {
        auto invalid = cfg;
        if (std::string(key) == "sla_light_pwm") invalid.set(key, 0);
        else invalid.set(key, 0.);
        REQUIRE_THROWS([&] { auto rejected = SLAPrint::create_raster_job(invalid, document, {.1, .15, .2});
                            rejected->export_print(path.string()); }());
        REQUIRE(bytes() == previous);
    }
    auto invalid = cfg; invalid.set("printer_model", std::string("name\nM1400 S255"));
    REQUIRE_THROWS([&] { auto rejected = SLAPrint::create_raster_job(invalid, document, {.1, .15, .2});
                        rejected->export_print(path.string()); }());
    REQUIRE(bytes() == previous);
    // An explicit destination creates a new raster context. Metadata outside
    // the typed slicing configuration must survive that same GUI path.
    auto source_config = cfg;
    source_config.set("sla_archive_format", std::string("genericzip"));
    auto source_job = SLAPrint::create_raster_job(source_config, document, {.1, .15, .2});
    // Generic ZIP and Klipper Mono share .zip. The explicit destination must
    // choose a codec rather than inferring the first registered ZIP family.
    REQUIRE_NOTHROW(source_job->export_layer_document(document, format, path.string(), previews, "fixture"));
    const auto destination = read_zipper_archive(path.string(), {""}, {});
    bool material = false;
    for (const auto &entry : destination.entries)
        if (entry.fname == "job.gcode")
            material = std::string(entry.buf.begin(), entry.buf.end()).find(";MaterialName:Resin Żółty\n") != std::string::npos;
    REQUIRE(material);
    REQUIRE(source_job->printer_config().sla_archive_format.value == "genericzip");
    REQUIRE(job->printer_config().sla_archive_format.value == format);
}

TEST_CASE("Klipper retains fixed-point Z on tall models", "[sla_reference][sla_klipper]")
{
    DynamicPrintConfig cfg; cfg.apply(SLAFullPrintConfig{});
    cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format", std::string("klipper"));
    cfg.set("display_pixels_x", 8); cfg.set("display_pixels_y", 4);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    cfg.set("max_print_height", 200.);
    const auto raster = sla::PNGRasterEncoder{}(std::vector<uint8_t>(32, 255).data(), 8, 4, 1);
    const auto doc = sla::LayerDocument::from_rasters({raster, raster, raster});
    struct File {
        boost::filesystem::path path = boost::filesystem::unique_path("klipper-tall-%%%%.zip");
        ~File() { boost::system::error_code e; boost::filesystem::remove(path, e); }
    } file;
    auto job = SLAPrint::create_raster_job(cfg, doc, {.1, 32.1, 64.3});
    REQUIRE_NOTHROW(job->export_print(file.path.string()));
    const auto archive = read_zipper_archive(file.path.string(), {""}, {});
    std::string script;
    for (const auto &entry : archive.entries)
        if (entry.fname == "job.gcode") script.assign(entry.buf.begin(), entry.buf.end());
    REQUIRE(script.find("HEIGHT=32.100000") != std::string::npos);
    REQUIRE(script.find("HEIGHT=64.300000") != std::string::npos);
    auto unsupported = SLAPrint::create_raster_job(cfg, doc, {.1, 32.1, 64.3001});
    REQUIRE_THROWS(unsupported->export_print(file.path.string()));
    const auto preserved = read_zipper_archive(file.path.string(), {""}, {});
    REQUIRE(preserved.entries.size() == archive.entries.size());
    for (size_t i = 0; i < archive.entries.size(); ++i) {
        REQUIRE(preserved.entries[i].fname == archive.entries[i].fname);
        REQUIRE(preserved.entries[i].buf == archive.entries[i].buf);
    }
}
