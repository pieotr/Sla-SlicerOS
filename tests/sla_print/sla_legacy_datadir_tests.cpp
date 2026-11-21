// W9b-1: a data directory and configuration files written by an FFF-capable version must load in the
// SLA/MSLA-only application without an exception and without touching (or losing) the SLA data.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include <string>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct TempDatadir {
    std::string previous = data_dir();
    fs::path    path     = fs::absolute(fs::unique_path("legacy-datadir-%%%%-%%%%"));
    TempDatadir() { fs::create_directories(path); set_data_dir(path.string()); }
    ~TempDatadir() { set_data_dir(previous); boost::system::error_code ec; fs::remove_all(path, ec); }
};

void write(const fs::path &file, const std::string &text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file.string()) << text;
}

std::string read(const fs::path &file)
{
    std::ifstream in(file.string());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A data directory as left behind by PrusaSlicer 2.9 with FFF printers plus a few SLA user profiles.
void seed_legacy_datadir(const fs::path &dir)
{
    write(dir / "print" / "0.20mm DRAFT.ini", "perimeters = 3\nlayer_height = 0.2\ncompatible_printers_condition = nozzle_diameter[0]==0.4\n");
    write(dir / "filament" / "My PLA.ini", "filament_diameter = 1.75\ntemperature = 215\nfilament_type = PLA\n");
    write(dir / "physical_printer" / "Garage.ini", "preset_name = Old MK3\npreset_names = \"Old MK3\"\nprint_host = 192.168.0.5\nhost_type = octoprint\n");
    write(dir / "printer" / "Old MK3.ini", "printer_technology = FFF\nnozzle_diameter = 0.4\nbed_shape = 0x0,250x0,250x210,0x210\n");
    // Pre-2.0 FFF printer: no printer_technology key at all.
    write(dir / "printer" / "Ancient FFF.ini", "nozzle_diameter = 0.4\nfilament_diameter = 1.75\n");
    write(dir / "printer" / "My SLA.ini", "printer_technology = SLA\ndisplay_width = 120\ndisplay_height = 68\n"
                                          "display_pixels_x = 2400\ndisplay_pixels_y = 1360\nmax_print_height = 150\n");
    write(dir / "sla_print" / "My Process.ini", "layer_height = 0.05\n");
    write(dir / "sla_material" / "My Resin.ini", "initial_exposure_time = 30\nexposure_time = 6\n");

    // An FFF-only vendor bundle and a mixed one (SLA model next to an FFF model).
    write(dir / "vendor" / "OldFFFVendor.ini",
          "[vendor]\nname = Old FFF Vendor\nconfig_version = 1.0.0\n\n"
          "[printer_model:MK3]\nname = MK3\ntechnology = FFF\nvariants = 0.4\n\n"
          "[print:*common*]\nperimeters = 2\n\n[print:0.15mm]\ninherits = *common*\nlayer_height = 0.15\n\n"
          "[filament:*PLA*]\nfilament_type = PLA\n\n[filament:Generic PLA]\ninherits = *PLA*\n\n"
          "[printer:*fff*]\nprinter_technology = FFF\nnozzle_diameter = 0.4\n\n"
          "[printer:MK3 0.4]\ninherits = *fff*\nprinter_model = MK3\nprinter_variant = 0.4\n");
    write(dir / "vendor" / "MixedVendor.ini",
          "[vendor]\nname = Mixed Vendor\nconfig_version = 1.0.0\n\n"
          "[printer_model:MK4]\nname = MK4\ntechnology = FFF\nvariants = 0.4\n\n"
          "[printer_model:SLX]\nname = SLX\ntechnology = SLA\nvariants = default\n\n"
          "[print:Draft]\nlayer_height = 0.3\n\n[filament:Gen]\nfilament_type = PETG\n\n"
          "[printer:MK4 0.4]\nprinter_technology = FFF\nprinter_model = MK4\nprinter_variant = 0.4\nnozzle_diameter = 0.4\n\n"
          "[printer:SLX default]\nprinter_technology = SLA\nprinter_model = SLX\nprinter_variant = default\n"
          "display_width = 192\ndisplay_height = 120\ndisplay_pixels_x = 3840\ndisplay_pixels_y = 2400\nmax_print_height = 200\n\n"
          "[sla_print:Vendor Process]\nlayer_height = 0.05\n\n"
          "[sla_material:Vendor Resin]\ninitial_exposure_time = 25\nexposure_time = 4\n");

    // The application config: FFF selections and sections next to SLA ones.
    AppConfig app(AppConfig::EAppMode::Editor);
    app.set("presets", "print", "0.20mm DRAFT");
    app.set("presets", "filament", "My PLA");
    app.set("presets", "filament_1", "My PLA");
    app.set("presets", "physical_printer", "Garage");
    app.set("presets", "sla_print", "My Process");
    app.set("presets", "sla_material", "My Resin");
    app.set("presets", "printer", "My SLA");
    app.set("filaments", "Generic PLA", "1");
    app.set("no_templates", "1");
    app.set("associate_gcode", "1");
    app.set_variant("OldFFFVendor", "MK3", "0.4", true);
    app.set_variant("MixedVendor", "MK4", "0.4", true);
    app.set_variant("MixedVendor", "SLX", "default", true);
    app.save();
}

} // namespace

TEST_CASE("is_legacy_fff_technology recognises only the raw FFF value", "[sla_legacy_datadir]")
{
    CHECK(is_legacy_fff_technology("FFF"));
    CHECK(is_legacy_fff_technology(" fff "));
    CHECK(is_legacy_fff_technology("\"FFF\""));
    CHECK_FALSE(is_legacy_fff_technology("SLA"));
    CHECK_FALSE(is_legacy_fff_technology(""));
    CHECK_FALSE(is_legacy_fff_technology("FFFF"));

    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    CHECK_THROWS_AS(config.set_deserialize("printer_technology", "FFF", substitutions), FffConfigurationException);
    // The enum parser must not substitute the removed value by the default (that would turn an FFF config into a bogus SLA one).
    CHECK(config.option("printer_technology") == nullptr);
    CHECK_NOTHROW(config.set_deserialize("printer_technology", "SLA", substitutions));
    CHECK(config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology")->value == ptSLA);
    CHECK(substitutions.substitutions.empty());
}

TEST_CASE("A datadir written by an FFF-capable version loads as SLA-only", "[sla_legacy_datadir]")
{
    TempDatadir datadir;
    save_main_thread_id();
    seed_legacy_datadir(datadir.path);

    AppConfig app(AppConfig::EAppMode::Editor);
    REQUIRE(app.load().empty());

    PresetBundle bundle;
    PresetsConfigSubstitutions substitutions;
    REQUIRE_NOTHROW(bundle.setup_directories());
    REQUIRE_NOTHROW(substitutions = bundle.load_presets(app, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem));
    // Nothing FFF may surface as a substitution dialog for an SLA user.
    CHECK(substitutions.empty());

    // SLA selections survive, FFF ones are ignored.
    CHECK(bundle.printers.get_selected_preset_name() == "My SLA");
    CHECK(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    CHECK(bundle.sla_prints.get_selected_preset_name() == "My Process");
    CHECK(bundle.sla_materials.get_selected_preset_name() == "My Resin");

    // FFF user printers are not loaded; SLA user and system printers are.
    CHECK(bundle.printers.find_preset("Old MK3", false) == nullptr);
    CHECK(bundle.printers.find_preset("Ancient FFF", false) == nullptr);
    CHECK(bundle.printers.find_preset("My SLA", false) != nullptr);
    const Preset *system_sla = bundle.printers.find_preset("SLX default", false);
    REQUIRE(system_sla != nullptr);
    CHECK(system_sla->is_system);
    CHECK(system_sla->printer_technology() == ptSLA);
    CHECK(bundle.printers.find_preset("MK4 0.4", false) == nullptr);
    CHECK(bundle.printers.find_preset("MK3 0.4", false) == nullptr);
    CHECK(bundle.sla_prints.find_preset("Vendor Process", false) != nullptr);
    CHECK(bundle.sla_materials.find_preset("Vendor Resin", false) != nullptr);

    // FFF-only vendor: not registered. Mixed vendor: kept with its SLA model only.
    CHECK(bundle.vendors.count("OldFFFVendor") == 0);
    REQUIRE(bundle.vendors.count("MixedVendor") == 1);
    const VendorProfile &mixed = bundle.vendors.at("MixedVendor");
    REQUIRE(mixed.models.size() == 1);
    CHECK(mixed.models.front().id == "SLX");
    CHECK(mixed.models.front().technology == ptSLA);

    // The legacy FFF files stay on disk, byte for byte (ignored, never deleted).
    CHECK(read(datadir.path / "print" / "0.20mm DRAFT.ini").find("perimeters = 3") != std::string::npos);
    CHECK(fs::exists(datadir.path / "filament" / "My PLA.ini"));
    CHECK(fs::exists(datadir.path / "physical_printer" / "Garage.ini"));
    CHECK(fs::exists(datadir.path / "printer" / "Old MK3.ini"));

    // Saving the selections writes only the SLA keys.
    bundle.export_selections(app);
    CHECK(app.get("presets", "print").empty());
    CHECK(app.get("presets", "filament").empty());
    CHECK(app.get("presets", "physical_printer").empty());
    CHECK(app.get("presets", "printer") == "My SLA");
    CHECK(app.get("presets", "sla_material") == "My Resin");
    app.save();
    AppConfig again(AppConfig::EAppMode::Editor);
    REQUIRE(again.load().empty());
    CHECK(again.get("presets", "sla_print") == "My Process");

    // A fresh PresetBundle from the same directory behaves identically (second start of the application).
    PresetBundle second;
    REQUIRE_NOTHROW(second.load_presets(again, ForwardCompatibilitySubstitutionRule::EnableSystemSilent));
    CHECK(second.printers.get_selected_preset_name() == "My SLA");
}

TEST_CASE("A stale FFF printer selection falls back to an SLA printer", "[sla_legacy_datadir]")
{
    TempDatadir datadir;
    save_main_thread_id();
    seed_legacy_datadir(datadir.path);
    AppConfig app(AppConfig::EAppMode::Editor);
    REQUIRE(app.load().empty());
    app.set("presets", "printer", "Old MK3");
    app.set("presets", "sla_material", "");

    PresetBundle bundle;
    bundle.setup_directories();
    REQUIRE_NOTHROW(bundle.load_presets(app, ForwardCompatibilitySubstitutionRule::EnableSystemSilent));
    CHECK(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    CHECK(bundle.printers.get_selected_preset_name() != "Old MK3");
    // The former "- default FFF -" slot is gone: the only default printer is SLA at index 0.
    CHECK(bundle.printers.num_default_presets() == 1);
    CHECK(bundle.printers.default_preset(0).name == "- default SLA -");
}

TEST_CASE("A config bundle with FFF and SLA sections imports the SLA ones and counts the skipped FFF ones", "[sla_legacy_datadir]")
{
    TempDatadir datadir;
    save_main_thread_id();
    const fs::path ini = datadir.path / "mixed_bundle.ini";
    write(ini,
          "[print:*common*]\nperimeters = 2\n\n[print:Draft]\ninherits = *common*\nlayer_height = 0.3\n\n"
          "[filament:PLA]\nfilament_diameter = 1.75\n\n"
          "[physical_printer:Garage]\npreset_name = Old MK3\nprint_host = 1.2.3.4\n\n"
          "[printer:Old MK3]\nprinter_technology = FFF\nnozzle_diameter = 0.4\n\n"
          "[printer:Ancient]\nnozzle_diameter = 0.4\n\n"
          "[printer:Imported SLA]\nprinter_technology = SLA\ndisplay_width = 150\ndisplay_height = 84\n"
          "display_pixels_x = 3000\ndisplay_pixels_y = 1680\nmax_print_height = 180\n\n"
          "[sla_print:Imported Process]\nlayer_height = 0.04\n\n"
          "[sla_material:Imported Resin]\nexposure_time = 7\n\n"
          "[presets]\nprint = Draft\nfilament = PLA\nphysical_printer = Garage\nprinter = Imported SLA\n"
          "sla_print = Imported Process\nsla_material = Imported Resin\n");

    PresetBundle bundle;
    bundle.setup_directories();
    std::pair<PresetsConfigSubstitutions, size_t> result;
    REQUIRE_NOTHROW(result = bundle.load_configbundle(ini.string(), PresetBundle::LoadConfigBundleAttribute::SaveImported,
                                                      ForwardCompatibilitySubstitutionRule::Enable));
    CHECK(result.second == 3); // SLA printer + SLA process + SLA material
    CHECK(bundle.skipped_fff_sections() == 5); // print:Draft, filament:PLA, physical_printer:Garage, printer:Old MK3, printer:Ancient
    CHECK(bundle.printers.find_preset("Old MK3", false) == nullptr);
    CHECK(bundle.printers.find_preset("Ancient", false) == nullptr);
    REQUIRE(bundle.printers.find_preset("Imported SLA", false) != nullptr);
    CHECK(bundle.printers.get_selected_preset_name() == "Imported SLA");
    CHECK(bundle.sla_prints.get_selected_preset_name() == "Imported Process");
    CHECK(bundle.sla_materials.get_selected_preset_name() == "Imported Resin");
    // Only SLA profiles were written to the user directory.
    CHECK(fs::exists(datadir.path / "printer" / "Imported SLA.ini"));
    CHECK_FALSE(fs::exists(datadir.path / "printer" / "Old MK3.ini"));
    CHECK_FALSE(fs::exists(datadir.path / "print"));
    CHECK_FALSE(fs::exists(datadir.path / "filament"));
    CHECK_FALSE(fs::exists(datadir.path / "physical_printer"));
}

TEST_CASE("Loading a configuration file with printer_technology = FFF is rejected with a clear message", "[sla_legacy_datadir]")
{
    TempDatadir datadir;
    save_main_thread_id();
    const fs::path fff_config = datadir.path / "fff_config.ini";
    write(fff_config, "printer_technology = FFF\nnozzle_diameter = 0.4\nlayer_height = 0.2\n");
    const fs::path ancient = datadir.path / "ancient_config.ini";
    write(ancient, "nozzle_diameter = 0.4\nfilament_diameter = 1.75\nlayer_height = 0.2\n");
    const fs::path fff_bundle = datadir.path / "fff_bundle.ini";
    write(fff_bundle, "[print:Draft]\nlayer_height = 0.3\n\n[filament:PLA]\nfilament_diameter = 1.75\n\n"
                      "[printer:MK3]\nprinter_technology = FFF\nnozzle_diameter = 0.4\n\n[presets]\nprint = Draft\n");

    PresetBundle bundle;
    bundle.setup_directories();
    for (const fs::path &file : {fff_config, ancient, fff_bundle}) {
        INFO(file.string());
        CHECK_THROWS_WITH(bundle.load_config_file(file.string(), ForwardCompatibilitySubstitutionRule::Disable),
                          Catch::Matchers::ContainsSubstring("not supported"));
    }
    // The failed loads left the SLA-only state untouched.
    CHECK(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    CHECK(bundle.printers.num_default_presets() == 1);

    // DynamicPrintConfig::load of the same file throws the dedicated exception (CLI "--load", Plater).
    DynamicPrintConfig config;
    CHECK_THROWS_AS(config.load(fff_config.string(), ForwardCompatibilitySubstitutionRule::Enable), FffConfigurationException);
}
