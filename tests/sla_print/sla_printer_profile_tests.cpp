#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <limits>
#include <fstream>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLA/PrinterGeometry.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"

using namespace Slic3r;
namespace { void require_bed(const DynamicPrintConfig &config, double width, double height); }

TEST_CASE("Local Creality templates identify the real model and output family", "[sla_profiles]") {
    const auto directory=boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path()/"resources/printer";
    size_t checked=0;
    for(const auto &entry:boost::filesystem::directory_iterator(directory)) {
        const auto name=entry.path().stem().string();
        if(name.find("Creality")!=0 || (name.find("CL")==std::string::npos && name.find("CT-")==std::string::npos)) continue;
        CAPTURE(name);
        DynamicPrintConfig source;source.load_from_ini(entry.path().string(),ForwardCompatibilitySubstitutionRule::Enable);
        REQUIRE(source.opt_enum<PrinterTechnology>("printer_technology")==ptSLA);
        const auto model=source.opt_string("printer_model");
        REQUIRE((model.find("CL")==0 || model.find("CT")==0));
        REQUIRE(source.opt_string("inherits").empty());
        REQUIRE(source.opt_string("sla_archive_format")==((name.find("Mage")!=std::string::npos)?"cxdlpv4":"cxdlp"));
        require_bed(source,source.opt_float("display_width"),source.opt_float("display_height"));
        ++checked;
    }
    REQUIRE(checked==12);
    DynamicPrintConfig synthetic;synthetic.load_from_ini((directory/"Creality_CXDLPv4_Test.ini").string(),ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(synthetic.opt_enum<PrinterTechnology>("printer_technology")==ptSLA);
    REQUIRE_FALSE(synthetic.has("nozzle_diameter"));
    REQUIRE(synthetic.opt_int("display_pixels_x")==1440);REQUIRE(synthetic.opt_int("display_pixels_y")==1080);
    require_bed(synthetic,164.64,92.16);
}

TEST_CASE("Local CWS templates select their actual schema and exact bed dimensions", "[sla_profiles]") {
    const auto directory=boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path()/"resources/printer";
    size_t checked=0;
    for(const auto &entry:boost::filesystem::directory_iterator(directory)) {
        if(!boost::filesystem::is_regular_file(entry.path()) || entry.path().extension()!=".ini")continue;
        DynamicPrintConfig source;source.load_from_ini(entry.path().string(),ForwardCompatibilitySubstitutionRule::Enable);
        if(!source.has("printer_notes"))continue;
        const auto notes=source.opt_string("printer_notes");
        const std::string format=notes.find("FILEFORMAT_RGB.CWS")!=std::string::npos?"cwsrgb":
            notes.find("FILEFORMAT_XML.CWS")!=std::string::npos?"cwsxml":
            notes.find("FILEFORMAT_CWS")!=std::string::npos?"cwsnovamaker":"";
        if(format.empty())continue;
        CAPTURE(entry.path().filename().string(),format);
        REQUIRE(source.opt_enum<PrinterTechnology>("printer_technology")==ptSLA);
        REQUIRE(source.opt_string("sla_archive_format")==format);
        REQUIRE(source.opt_string("inherits").empty());
        REQUIRE(source.opt_string("printer_model")==entry.path().stem().string());
        REQUIRE_FALSE(source.opt_string("printer_vendor").empty());
        require_bed(source,source.opt_float("display_width"),source.opt_float("display_height"));
        if(format=="cwsrgb") {
            REQUIRE(source.opt_enum<SLADisplayOrientation>("display_orientation")==sladoLandscape);
            REQUIRE(source.opt_int("display_pixels_x")%3==0);
        }
        ++checked;
    }
    REQUIRE(checked==11);
}

TEST_CASE("Bundled Anycubic profiles resolve presets and use matching geometry", "[sla_profiles]") {
    PresetBundle bundle;
    const auto path=boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path()/"resources/profiles/AnycubicSLA.ini";
    const auto loaded=bundle.load_configbundle(path.string(),PresetBundle::LoadSystem,ForwardCompatibilitySubstitutionRule::Enable);
    REQUIRE(loaded.second>0);
    for(const char *name : {"Anycubic Photon Mono", "Anycubic Photon Mono SE", "Anycubic Photon Mono X", "Anycubic Photon Mono X 6K"}) {
        CAPTURE(name);
        const auto *printer=bundle.printers.find_preset(name); REQUIRE(printer!=nullptr);
        const auto &config=printer->config;
        const auto *machine=anycubic_legacy_geometry(config.opt_string("sla_archive_format")); REQUIRE(machine!=nullptr);
        REQUIRE(std::abs(config.opt_float("display_width")-machine->width)<.0001);
        REQUIRE(std::abs(config.opt_float("display_height")-machine->height)<.0001);
        REQUIRE(std::abs(config.opt_float("max_print_height")-machine->machine_z)<.0001);
        require_bed(config,machine->width,machine->height);
        const auto *process=bundle.sla_prints.find_preset(config.opt_string("default_sla_print_profile")); REQUIRE(process!=nullptr);
        REQUIRE(process->config.opt_string("output_filename_format")=="[input_filename_base][default_output_extension]");
        const auto *material=bundle.sla_materials.find_preset(config.opt_string("default_sla_material_profile")); REQUIRE(material!=nullptr);
        REQUIRE(material->config.opt_float("sla_lift_height")==8.);
        REQUIRE(material->config.opt_float("sla_bottom_lift_height")==9.);
        REQUIRE(material->config.opt_float("sla_wait_before_cure")==.5);
    }
}

TEST_CASE("Failed atomic replacement preserves its destination", "[sla_profiles]")
{
    struct Directory {
        boost::filesystem::path path = boost::filesystem::absolute(boost::filesystem::unique_path("atomic-replace-%%%%"));
        Directory() { boost::filesystem::create_directory(path); }
        ~Directory() { boost::system::error_code error; boost::filesystem::remove_all(path, error); }
    } directory;
    const auto target = directory.path / "existing.ini";
    std::ofstream(target.string()) << "original configuration";
    REQUIRE(bool(rename_file((directory.path / "missing.tmp").string(), target.string())));
    std::ifstream preserved(target.string());
    const std::string contents{std::istreambuf_iterator<char>(preserved), std::istreambuf_iterator<char>()};
    REQUIRE(contents == "original configuration");
    preserved.close();
    const auto source = directory.path / "complete.tmp";
    std::ofstream(source.string()) << "replacement configuration";
    const auto occupied = directory.path / "occupied";
    boost::filesystem::create_directory(occupied);
    REQUIRE(bool(rename_file(source.string(), occupied.string())));
    REQUIRE(boost::filesystem::is_directory(occupied));
    REQUIRE(boost::filesystem::is_regular_file(source));
    REQUIRE_FALSE(bool(rename_file(source.string(), target.string())));
    std::ifstream replacement(target.string());
    const std::string replaced{std::istreambuf_iterator<char>(replacement), std::istreambuf_iterator<char>()};
    REQUIRE(replaced == "replacement configuration");
}

TEST_CASE("Preset IO failures preserve edits saved data and selection", "[sla_profiles]")
{
    struct Directory {
        boost::filesystem::path path = boost::filesystem::absolute(boost::filesystem::unique_path("preset-io-%%%%"));
        Directory() { boost::filesystem::create_directories(path / "printer"); }
        ~Directory() { boost::system::error_code error; boost::filesystem::remove_all(path, error); }
    } directory;
    PresetBundle bundle;
    PresetsConfigSubstitutions substitutions;
    bundle.printers.load_presets(directory.path.string(), "printer", substitutions, ForwardCompatibilitySubstitutionRule::Enable);
    auto &collection = bundle.printers;
    collection.get_edited_preset().config.set("display_width", 192.);
    REQUIRE(collection.save_current_preset("Original"));
    const auto original_path = directory.path / "printer" / "Original.ini";
    collection.get_edited_preset().config.set("display_width", 220.);
    collection.update_dirty();
    REQUIRE(collection.current_is_dirty());
    // An occupied directory causes a portable write/rename failure, including
    // when tests run with privileges that would bypass ordinary chmod checks.
    const auto occupied = directory.path / "printer" / "Occupied.ini";
    boost::filesystem::create_directory(occupied);
    REQUIRE_THROWS(collection.save_current_preset("Occupied"));
    REQUIRE(collection.find_preset("Occupied") == nullptr);
    REQUIRE(collection.get_selected_preset_name() == "Original");
    REQUIRE(collection.get_edited_preset().config.opt_float("display_width") == 220.);
    REQUIRE(collection.current_is_dirty());
    REQUIRE(collection.get_selected_preset().config.opt_float("display_width") == 192.);
    DynamicPrintConfig disk;
    disk.load(original_path.string(), ForwardCompatibilitySubstitutionRule::Enable);
    REQUIRE(disk.opt_float("display_width") == 192.);
    const auto saved_file = original_path.string() + ".backup";
    boost::filesystem::rename(original_path, saved_file);
    boost::filesystem::create_directory(original_path);
    std::ofstream((original_path / "keep").string()) << "unrelated content";
    REQUIRE_THROWS(collection.save_current_preset("Original"));
    REQUIRE_THROWS(collection.delete_current_preset());
    REQUIRE(collection.find_preset("Original") != nullptr);
    REQUIRE(collection.current_is_dirty());
    REQUIRE_FALSE(collection.delete_preset("Unknown preset"));
    for (const auto &file : boost::filesystem::directory_iterator(directory.path / "printer"))
        REQUIRE(file.path().filename().string().find(".sla-config-") != 0);
    boost::filesystem::remove_all(original_path);
    boost::filesystem::rename(saved_file, original_path);
    REQUIRE_FALSE(collection.save_current_preset("Original"));
    REQUIRE_FALSE(collection.current_is_dirty());
    disk.load(original_path.string(), ForwardCompatibilitySubstitutionRule::Enable);
    REQUIRE(disk.opt_float("display_width") == 220.);
    REQUIRE(collection.delete_current_preset());
    REQUIRE_FALSE(boost::filesystem::exists(original_path));
}

TEST_CASE("Export preferences survive restart independently for UTF-8 printer names", "[sla_profiles]")
{
    struct Directory {
        std::string previous = data_dir();
        boost::filesystem::path path = boost::filesystem::absolute(boost::filesystem::unique_path("export-preferences-%%%%"));
        Directory() { boost::filesystem::create_directories(path); set_data_dir(path.string()); }
        ~Directory() { set_data_dir(previous); boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
    } directory;
    save_main_thread_id();
    AppConfig saved(AppConfig::EAppMode::Editor);
    saved.set_sla_export_format("Drukarka Łódź [A]=1", "cxdlpv4");
    saved.set_sla_export_format("Drukarka Łódź [A]=2", "goo");
    saved.save();
    AppConfig restored(AppConfig::EAppMode::Editor);
    REQUIRE(restored.load().empty());
    REQUIRE(restored.sla_export_format("Drukarka Łódź [A]=1") == "cxdlpv4");
    REQUIRE(restored.sla_export_format("Drukarka Łódź [A]=2") == "goo");
    REQUIRE(restored.sla_export_format("Unconfigured printer").empty());
    restored.set_sla_export_format("Drukarka Łódź [A]=1", "prz");
    REQUIRE(restored.sla_export_format("Drukarka Łódź [A]=2") == "goo");
    restored.set_sla_export_format("Drukarka Łódź [A]=1", "");
    restored.save();
    AppConfig after_delete(AppConfig::EAppMode::Editor);
    REQUIRE(after_delete.load().empty());
    REQUIRE(after_delete.sla_export_format("Drukarka Łódź [A]=1").empty());
    REQUIRE(after_delete.sla_export_format("Drukarka Łódź [A]=2") == "goo");
}

TEST_CASE("LCD motion and exposure settings belong to material presets", "[sla_profiles]")
{
    PresetBundle bundle;
    const auto &material = bundle.sla_materials.get_edited_preset().config;
    for (const auto &setting : sla::machine_settings) {
        CAPTURE(setting.key);
        REQUIRE(material.has(setting.key));
        const auto *value = material.option(setting.key);
        REQUIRE((setting.integer ? double(value->getInt()) : value->getFloat()) == setting.initial);
    }
}

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
TEST_CASE("Importing a const SLA printer configuration uses the SLA schema", "[sla_profiles]")
{
    PresetBundle bundle;
    auto config = printer_config();
    config.set("display_width", 192.);
    config.set("display_pixels_x", 3840);
    config.set("sla_archive_format", std::string("cxdlpv4"));
    const auto &source = config;
    const auto &loaded = bundle.printers.load_preset("", "Imported SLA", source, true);
    REQUIRE(loaded.printer_technology() == ptSLA);
    REQUIRE(loaded.config.has("display_width"));
    REQUIRE(loaded.config.opt_float("display_width") == 192.);
    REQUIRE(loaded.config.opt_int("display_pixels_x") == 3840);
    REQUIRE(loaded.config.opt_string("sla_archive_format") == "cxdlpv4");
    REQUIRE_FALSE(loaded.config.has("nozzle_diameter"));
    REQUIRE(bundle.printers.get_edited_preset().name == "Imported SLA");
}

TEST_CASE("Two SLA printer presets retain independent formats and geometry after reload", "[sla_profiles]")
{
    struct Directory {
        boost::filesystem::path path = boost::filesystem::absolute(boost::filesystem::unique_path("sla-profiles-%%%%-%%%%"));
        Directory() { boost::filesystem::create_directories(path / "printer"); }
        ~Directory() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
    } directory;
    auto load = [&](PresetBundle &bundle) {
        PresetsConfigSubstitutions substitutions;
        bundle.printers.load_presets(directory.path.string(), "printer", substitutions,
                                    ForwardCompatibilitySubstitutionRule::Enable);
        REQUIRE(substitutions.empty());
    };
    PresetBundle bundle;
    load(bundle);
    auto &config = bundle.printers.get_edited_preset().config;
    config.set("display_width", 192.);
    config.set("display_height", 120.);
    config.set("sla_archive_format", std::string("cxdlpv4"));
    Preset::normalize(config);
    REQUIRE(bundle.printers.save_current_preset("Printer A"));
    bundle.printers.get_edited_preset().config.set("display_width", 80.);
    bundle.printers.get_edited_preset().config.set("display_height", 45.);
    bundle.printers.get_edited_preset().config.set("sla_archive_format", std::string("ctb"));
    Preset::normalize(bundle.printers.get_edited_preset().config);
    REQUIRE(bundle.printers.save_current_preset("Printer B"));

    PresetBundle restarted;
    load(restarted);
    for (const auto &name : {"Printer A", "Printer B"}) {
        restarted.printers.select_preset_by_name(name, true);
        const auto &printer = restarted.printers.get_edited_preset();
        REQUIRE(printer.name == name);
        REQUIRE(printer.printer_technology() == ptSLA);
        const bool first = printer.name == "Printer A";
        REQUIRE(printer.config.opt_string("sla_archive_format") == (first ? "cxdlpv4" : "ctb"));
        require_bed(printer.config, first ? 192. : 80., first ? 120. : 45.);
    }
    // Updating an existing profile must persist its own selection only.
    restarted.printers.select_preset_by_name("Printer A", true);
    restarted.printers.get_edited_preset().config.set("sla_archive_format", std::string("goo"));
    REQUIRE_FALSE(restarted.printers.save_current_preset("Printer A")); // Not a new preset.
    restarted.printers.select_preset_by_name("Printer B", true);
    // A copied profile may be removed without deleting its source on disk.
    REQUIRE(restarted.printers.save_current_preset("Printer B copy"));
    REQUIRE(restarted.printers.delete_current_preset());
    PresetBundle after_delete;
    load(after_delete);
    REQUIRE(after_delete.printers.find_preset("Printer B copy") == nullptr);
    REQUIRE(after_delete.printers.find_preset("Printer A") != nullptr);
    REQUIRE(after_delete.printers.find_preset("Printer B") != nullptr);
    REQUIRE(after_delete.printers.find_preset("Printer A")->config.opt_string("sla_archive_format") == "goo");
    REQUIRE(after_delete.printers.find_preset("Printer B")->config.opt_string("sla_archive_format") == "ctb");
}

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

TEST_CASE("New SLA process and material defaults do not inherit FDM layer heights", "[sla_profiles]")
{
    PresetBundle presets;
#ifdef SLIC3R_SLA_ONLY
    REQUIRE(presets.sla_prints.default_preset().config.opt_float("layer_height") == .05);
#endif
    REQUIRE(presets.sla_materials.default_preset().config.opt_float("initial_layer_height") == .05);
    REQUIRE(presets.sla_prints.default_preset().config.opt_string("output_filename_format") ==
        "[input_filename_base][default_output_extension]");
}
