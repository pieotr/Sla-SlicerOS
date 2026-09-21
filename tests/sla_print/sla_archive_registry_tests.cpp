#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/SL1.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/Format/UVToolsCompatArchive.hpp"
#include "libslic3r/Format/SL1_SVG.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"

using namespace Slic3r;

TEST_CASE("Explicit SLA reader IDs override filenames", "[sla_registry]")
{
    auto reader = SLAArchiveReader::create("renamed.data", "sl1");
    REQUIRE(dynamic_cast<SL1Reader *>(reader.get()) != nullptr);
    reader = SLAArchiveReader::create("no_extension", "SL1SVG");
    REQUIRE(dynamic_cast<SL1_SVGReader *>(reader.get()) != nullptr);
    REQUIRE(SLAArchiveReader::create("valid.sl1", "unknown") == nullptr);
}

TEST_CASE("SLA readers preserve compound suffixes and case", "[sla_registry]")
{
    auto reader = SLAArchiveReader::create("example.SL1S", "");
    REQUIRE(dynamic_cast<SL1Reader *>(reader.get()) != nullptr);
    reader = SLAArchiveReader::create("example.RGB.ZIP", "");
    REQUIRE(dynamic_cast<SL1Reader *>(reader.get()) != nullptr);
    reader = SLAArchiveReader::create("example.SL1_SVG", "");
    REQUIRE(dynamic_cast<SL1_SVGReader *>(reader.get()) != nullptr);
    REQUIRE(SLAArchiveReader::create("example.sl1.backup", "") == nullptr);
    REQUIRE(SLAArchiveReader::create("no_extension", "") == nullptr);
}

TEST_CASE("Unimplemented binary readers do not masquerade as SL1", "[sla_registry]")
{
    for (const char *id : {"ctb", "cxdlpv4", "goo", "pwmx"}) {
        INFO(id);
        REQUIRE(get_archive_entry(id) != nullptr);
        REQUIRE_FALSE(bool(get_reader_factory(id)));
        REQUIRE(SLAArchiveReader::create(std::string("sample.") + id, "") == nullptr);
        REQUIRE(SLAArchiveReader::create("sample.sl1", id) == nullptr);
    }
}

TEST_CASE("SLA format suffixes are validated without losing punctuation", "[sla_registry]")
{
    for (const char *ext : {".CTB", "ctb", "encrypted.ctb"}) {
        const auto *entry = get_archive_entry_by_extension(ext);
        REQUIRE(entry != nullptr);
        REQUIRE(std::string(entry->id) == "ctb");
    }
    const auto *entry = get_archive_entry_by_extension(".SL1_SVG");
    REQUIRE(entry != nullptr);
    REQUIRE(std::string(entry->id) == "SL1SVG");
    for (const char *ext : {"ctb)", "*.ctb", "encryptedctb", "sl1svg", "..ctb", "ctb.", "", "ctb/"}) {
        INFO(ext);
        REQUIRE(get_archive_entry_by_extension(ext) == nullptr);
    }
    REQUIRE(get_archive_entry_by_extension(nullptr) == nullptr);
}

TEST_CASE("SL1 slicing preserves geometry and exposure settings", "[sla_reference]")
{
    // A small synthetic model keeps the smoke test independent of external
    // fixtures and avoids generating hundreds of full-resolution layers.
    Model model;
    auto *object = model.add_object("reference", "", make_cube(4., 3., 0.3));
    object->add_instance()->set_offset(Vec3d(10., 10., 0.));

    SLAFullPrintConfig fullcfg;
    fullcfg.printer_technology.setInt(ptSLA);
    const bool portrait = GENERATE(false, true);
    fullcfg.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    fullcfg.display_mirror_x.value = GENERATE(false, true);
    fullcfg.display_mirror_y.value = GENERATE(false, true);
    fullcfg.set("sla_archive_format", std::string("SL1"));
    fullcfg.set("supports_enable", false);
    fullcfg.set("pad_enable", false);
    fullcfg.set("support_object_elevation", 0.0);
    fullcfg.set("layer_height", 0.05);
    fullcfg.set("initial_layer_height", 0.05);
    fullcfg.set("display_width", 100.0);
    fullcfg.set("display_height", 60.0);
    fullcfg.set("display_pixels_x", 1000);
    fullcfg.set("display_pixels_y", 300);
    fullcfg.set("exposure_time", 2.5);
    fullcfg.set("initial_exposure_time", 20.0);

    DynamicPrintConfig cfg;
    cfg.apply(fullcfg);
    SLAPrint print;
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(model, cfg);
    print.process();

    struct TemporaryArchive {
        boost::filesystem::path path = boost::filesystem::unique_path("sla-reference-%%%%-%%%%.sl1");
        ~TemporaryArchive() {
            boost::system::error_code error;
            boost::filesystem::remove(path, error);
        }
    } archive;
    print.export_print(archive.path.string(), {}, "reference");
    REQUIRE(boost::filesystem::file_size(archive.path) > 0);

    const auto contents = read_zipper_archive(archive.path.string(), {"png", "ini"}, {"thumbnail"});
    REQUIRE(contents.entries.size() == 6);
    REQUIRE(contents.config.get<std::string>("slaSlicerProfileGeometry") == "raster-v1");
    REQUIRE(contents.profile.get<int>("display_pixels_x") == (portrait ? 300 : 1000));
    REQUIRE(contents.profile.get<double>("display_width") == Catch::Approx(portrait ? 60. : 100.));
    for (const auto &entry : contents.entries) {
        png::ImageGreyscale image;
        REQUIRE(png::decode_png(png::ReadBuf{entry.buf.data(), entry.buf.size()}, image));
        REQUIRE(image.cols == (portrait ? 300 : 1000));
        REQUIRE(image.rows == (portrait ? 1000 : 300));
        REQUIRE(std::any_of(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p > 128; }));
        REQUIRE(std::any_of(image.buf.begin(), image.buf.end(), [](uint8_t p) { return p == 0; }));
    }

    // Exercise explicit format selection and the default progress callback:
    // both previously prevented this normal read path from completing.
    auto reader = SLAArchiveReader::create(archive.path.string(), "SL1");
    REQUIRE(reader != nullptr);
    DynamicPrintConfig restored;
    std::vector<ExPolygons> slices;
    reader->read(slices, restored);
    REQUIRE(slices.size() == 6);
    for (const auto &slice : slices)
        REQUIRE_FALSE(slice.empty());
    REQUIRE(restored.opt_float("exposure_time") == Catch::Approx(2.5));
    REQUIRE(restored.opt_float("initial_exposure_time") == Catch::Approx(20.0));
    REQUIRE(restored.opt_float("layer_height") == Catch::Approx(0.05));
    REQUIRE(restored.opt_int("display_pixels_x") == 1000);
    REQUIRE(restored.opt_int("display_pixels_y") == 300);
    REQUIRE(restored.opt_float("display_width") == Catch::Approx(100.));
    REQUIRE(restored.opt_float("display_height") == Catch::Approx(60.));
    DynamicPrintConfig profile_only;
    reader->read(profile_only);
    REQUIRE(profile_only == restored);
    auto document = sla::LayerDocument::load_sl1(archive.path.string());
    REQUIRE(document.layer_count() == slices.size());
    document.paint(0, 5, 5, 2, 255);
    document.save_sl1(archive.path.string());
    REQUIRE(sla::LayerDocument::load_sl1(archive.path.string()).image(0).get(5, 5) == 255);
    DynamicPrintConfig edited_profile;
    std::vector<ExPolygons> edited_slices;
    reader->read(edited_slices, edited_profile);
    REQUIRE(edited_profile == restored);
    REQUIRE(edited_slices.size() == slices.size());
    for (const auto &slice : slices) {
        REQUIRE(std::any_of(slice.begin(), slice.end(), [](const ExPolygon &poly) {
            return poly.contains(Point(scaled(12.), scaled(11.5)));
        }));
    }
}

template<class Base> struct ExposedRasterArchive : Base {
    using Base::Base;
    using Base::create_raster;
};

TEST_CASE("Archive rasters preserve physical geometry with rectangular pixels", "[sla_reference]")
{
    // Expose the production raster factory without involving a second renderer.
    struct RasterArchive : SL1Archive {
        using SL1Archive::SL1Archive;
        using SL1Archive::create_raster;
    };
    const int writer = GENERATE(0, 1, 2, 3);
    CAPTURE(writer);
    for (bool portrait : {false, true})
        for (bool mirror_x : {false, true})
            for (bool mirror_y : {false, true}) {
                CAPTURE(portrait, mirror_x, mirror_y);
                SLAPrinterConfig cfg;
                cfg.display_width.value = 100.;
                cfg.display_height.value = 60.;
                cfg.display_pixels_x.value = 1000;
                cfg.display_pixels_y.value = 300;
                cfg.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
                cfg.display_mirror_x.value = mirror_x;
                cfg.display_mirror_y.value = mirror_y;
                std::unique_ptr<sla::RasterBase> raster;
                switch (writer) {
                case 0: raster = RasterArchive(cfg).create_raster(); break;
                case 1: raster = ExposedRasterArchive<AnycubicSLAArchive>(cfg).create_raster(); break;
                case 2: raster = ExposedRasterArchive<UVToolsFDGArchive>(cfg).create_raster(); break;
                case 3: raster = ExposedRasterArchive<UVToolsCWSArchive>(cfg).create_raster(); break;
                }
                // An off-centre rectangle catches rotation, scaling and clipping.
                raster->draw(ExPolygon(Polygon(Points{
                    Point(scaled(72.), scaled(42.)), Point(scaled(80.), scaled(42.)),
                    Point(scaled(80.), scaled(48.)), Point(scaled(72.), scaled(48.))})));
                const auto encoded = raster->encode(sla::PNGRasterEncoder{});
                png::ImageGreyscale image;
                REQUIRE(png::decode_png(png::ReadBuf{encoded.data(), encoded.size()}, image));
                REQUIRE(image.cols == (portrait ? 300 : 1000));
                REQUIRE(image.rows == (portrait ? 1000 : 300));
                // Independent pixel-space oracle: rectangle [720,800) x [210,240).
                size_t lit = 0;
                for (size_t y = 0; y < image.rows; ++y)
                    for (size_t x = 0; x < image.cols; ++x) {
                        size_t u = (portrait ? !mirror_x : mirror_x) ? image.cols - 1 - x : x;
                        size_t v = !mirror_y ? image.rows - 1 - y : y;
                        if (portrait) std::swap(u, v);
                        const bool expected = u >= 720 && u < 800 && v >= 210 && v < 240;
                        if ((image.get(y, x) > 128) != expected)
                            FAIL("Unexpected exposure at pixel " << x << ", " << y);
                        lit += image.get(y, x) > 128;
                    }
                REQUIRE(lit == 2400);
            }
}

TEST_CASE("SL1 profile geometry conventions are explicit", "[sla_reference]")
{
    ZipperArchive archive;
    archive.profile.put("display_orientation", "portrait");
    archive.profile.put("display_width", "100");
    archive.profile.put("display_height", "60");
    archive.profile.put("display_pixels_x", "1000");
    archive.profile.put("display_pixels_y", "300");
    DynamicPrintConfig legacy;
    extract_profile(archive, legacy);
    REQUIRE(legacy.opt_float("display_width") == 100.);
    REQUIRE(legacy.opt_int("display_pixels_x") == 1000);
    archive.config.put("slaSlicerProfileGeometry", "raster-v1");
    DynamicPrintConfig tagged;
    extract_profile(archive, tagged);
    REQUIRE(tagged.opt_float("display_width") == 60.);
    REQUIRE(tagged.opt_int("display_pixels_x") == 300);
    archive.config.put("slaSlicerProfileGeometry", "raster-v99");
    REQUIRE_THROWS(extract_profile(archive, tagged));
}

TEST_CASE("SL1 rejects invalid display dimensions before reconstruction", "[sla_reference]")
{
    DynamicPrintConfig cfg;
    cfg.apply(SLAFullPrintConfig{});
    cfg.set("display_pixels_x", 0);
    REQUIRE_THROWS(get_raster_params(cfg));
    cfg.set("display_pixels_x", 1000);
    cfg.set("display_height", -1.);
    REQUIRE_THROWS(get_raster_params(cfg));
}

TEST_CASE("Unavailable exports fail before drawing or writing a file", "[sla_registry]")
{
    SLAPrinterConfig cfg;
    SLAPrint print;
    for (const char *id : {"n4", "n7", "cbddlp", "cbt", "photon", "photons", "cxdlp",
                          "qdt", "svgx", "lgs", "lgs30", "lgs120", "lgs4k", "gr1",
                          "mdlp", "nanodlp", "osla", "uvj", "phz", "prz", "osf",
                          "jxs", "vdt", "zcode", "zcodex", "PWSZ"}) {
        CAPTURE(id);
        auto writer = SLAArchiveWriter::create(id, cfg);
        REQUIRE(writer != nullptr);
        bool drawn = false;
        REQUIRE_THROWS_WITH(writer->draw_layers(1, [&](sla::RasterBase &, size_t) { drawn = true; }, [] { return false; }),
                            archive_export_unavailable_reason(id));
        REQUIRE_FALSE(drawn);
        const auto path = boost::filesystem::unique_path("unsupported-%%%%-%%%%.tmp");
        REQUIRE_THROWS_WITH(writer->export_print(path.string(), print, {}, "test"),
                            archive_export_unavailable_reason(id));
        REQUIRE_FALSE(boost::filesystem::exists(path));
    }
    REQUIRE(archive_export_unavailable_reason("SL1").empty());
    REQUIRE(archive_export_unavailable_reason("goo").empty());
}
