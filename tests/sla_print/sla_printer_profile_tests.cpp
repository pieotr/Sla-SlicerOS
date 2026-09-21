#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <limits>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLA/PrinterGeometry.hpp"

using namespace Slic3r;

namespace {
DynamicPrintConfig printer_config()
{
    DynamicPrintConfig config;
    config.apply(SLAPrinterConfig{});
    config.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    config.set("display_width", 120.0);
    config.set("display_height", 68.0);
    return config;
}

void require_bed(const DynamicPrintConfig &config, double width, double height)
{
    const auto *bed = config.option<ConfigOptionPoints>("bed_shape");
    REQUIRE(bed != nullptr);
    const std::vector<Vec2d> expected{{0., 0.}, {width, 0.}, {width, height}, {0., height}};
    REQUIRE(bed->values == expected);
}
}

TEST_CASE("SLA printer normalization derives bed dimensions from the display", "[sla_profiles]")
{
    auto config = printer_config();
    Preset::normalize(config);
    require_bed(config, 120., 68.);
    REQUIRE_FALSE(sla::synchronize_bed_shape(config));

    config.set("display_width", 192.0);
    config.set("display_height", 120.0);
    REQUIRE(sla::synchronize_bed_shape(config));
    require_bed(config, 192., 120.);
    REQUIRE_FALSE(sla::synchronize_bed_shape(config));
}

TEST_CASE("SLA image orientation does not swap scene bed dimensions", "[sla_profiles]")
{
    for (const auto orientation : {sladoLandscape, sladoPortrait})
        for (const bool mirror_x : {false, true})
            for (const bool mirror_y : {false, true}) {
                auto config = printer_config();
                config.set_key_value("display_orientation", new ConfigOptionEnum<SLADisplayOrientation>(orientation));
                config.set("display_mirror_x", mirror_x);
                config.set("display_mirror_y", mirror_y);
                Preset::normalize(config);
                require_bed(config, 120., 68.);
            }
}

TEST_CASE("Bed synchronization ignores incomplete invalid and FFF configs", "[sla_profiles]")
{
    DynamicPrintConfig partial;
    REQUIRE_FALSE(sla::synchronize_bed_shape(partial));
    REQUIRE_FALSE(partial.has("bed_shape"));

    auto config = printer_config();
    const auto original = config.option<ConfigOptionPoints>("bed_shape")->values;
    config.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptFFF));
    REQUIRE_FALSE(sla::synchronize_bed_shape(config));
    REQUIRE(config.option<ConfigOptionPoints>("bed_shape")->values == original);

    config.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        config.set("display_width", invalid);
        REQUIRE_FALSE(sla::synchronize_bed_shape(config));
        REQUIRE(config.option<ConfigOptionPoints>("bed_shape")->values == original);
    }
}

TEST_CASE("Normalized SLA printer geometry and format survive an INI roundtrip", "[sla_profiles]")
{
    auto config = printer_config();
    config.set("display_width", 192.0);
    config.set("sla_archive_format", std::string("cxdlpv4"));
    Preset::normalize(config);

    struct TemporaryProfile {
        boost::filesystem::path path = boost::filesystem::unique_path("sla-printer-%%%%-%%%%.ini");
        ~TemporaryProfile() {
            boost::system::error_code error;
            boost::filesystem::remove(path, error);
        }
    } file;
    config.save(file.path.string());
    DynamicPrintConfig restored;
    restored.load(file.path.string(), ForwardCompatibilitySubstitutionRule::Enable);
    Preset::normalize(restored);
    require_bed(restored, 192., 68.);
    REQUIRE(restored.opt_string("sla_archive_format") == "cxdlpv4");
    REQUIRE_FALSE(sla::synchronize_bed_shape(restored));
}

#ifdef SLIC3R_SLA_ONLY
TEST_CASE("Fresh and reset SLA bundles select a complete SLA printer schema", "[sla_profiles]")
{
    PresetBundle bundle;
    REQUIRE(bundle.printers.default_preset(0).printer_technology() == ptFFF);
    bundle.printers.select_preset_by_name("", true);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    bundle.printers.select_preset_by_name("- default FFF -", true);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    bundle.printers.set_default_suppressed(true);
    bundle.printers.select_preset_by_name("missing-printer", true);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    for (const bool reset : {false, true}) {
        if (reset) {
            bundle.printers.select_preset(0);
            bundle.reset(false);
        }
        const auto &printer = bundle.printers.get_edited_preset();
        REQUIRE(printer.printer_technology() == ptSLA);
        for (const char *key : {"display_width", "display_height", "display_pixels_x",
                               "display_pixels_y", "display_orientation", "min_exposure_time",
                               "max_exposure_time", "sla_archive_format"}) {
            INFO(key);
            REQUIRE(printer.config.has(key));
        }
        REQUIRE_FALSE(printer.config.has("nozzle_diameter"));
        require_bed(printer.config, 120., 68.);
    }
}
#endif
