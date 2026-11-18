#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <functional>
#include <map>
#include <tuple>
#include <boost/filesystem.hpp>
#include <limits>
#include <fstream>
#include <set>
#include <algorithm>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLA/PrinterGeometry.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/SLA/ProjectSettings.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"

using namespace Slic3r;
namespace { void require_bed(const DynamicPrintConfig &config, double width, double height); }
namespace {
template<class C> bool contains(const C &list, const std::string &key) { return std::find(list.begin(), list.end(), key) != list.end(); }
}

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

TEST_CASE("Halot R6 template matches the Creality reference file header", "[sla_profiles]") {
    // Values read from a .cxdlpv4 written by Creality's slicer for a HALOT-R6 (UVtools print-properties):
    // PrinterModel R004, 1620x2560 px, 82.62x130.56 mm, BedSizeZ 160, ProjectorType 0.
    const auto path=boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path()/"resources/printer/Creality Halot R6.ini";
    DynamicPrintConfig source;source.load_from_ini(path.string(),ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(source.opt_enum<PrinterTechnology>("printer_technology")==ptSLA);
    REQUIRE(source.opt_string("printer_model")=="R004");
    REQUIRE(source.opt_string("sla_archive_format")=="cxdlpv4");
    REQUIRE(source.opt_string("inherits").empty());
    REQUIRE(source.opt_int("display_pixels_x")==1620);REQUIRE(source.opt_int("display_pixels_y")==2560);
    REQUIRE(std::abs(source.opt_float("display_width")-82.62)<1e-9);REQUIRE(std::abs(source.opt_float("display_height")-130.56)<1e-9);
    REQUIRE(std::abs(source.opt_float("max_print_height")-160.)<1e-9);
    REQUIRE(source.opt_enum<SLAArchiveHeaderMirror>("sla_archive_header_mirror")==slahmAuto);
    require_bed(source,82.62,130.56);
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

TEST_CASE("Renaming a user preset preserves its saved baseline and dirty draft", "[sla_profiles][sla_profile_rename]")
{
    struct Directory {
        boost::filesystem::path path = boost::filesystem::absolute(boost::filesystem::unique_path("preset-rename-%%%%"));
        Directory() { boost::filesystem::create_directory(path); }
        ~Directory() { boost::system::error_code error; boost::filesystem::remove_all(path, error); }
    } directory;
    const auto type = GENERATE(Preset::TYPE_PRINTER, Preset::TYPE_SLA_MATERIAL, Preset::TYPE_SLA_PRINT);
    PresetBundle bundle;
    auto &collection = bundle.get_presets(type);
    const auto folder = collection.section_name();
    boost::filesystem::create_directory(directory.path / folder);
    PresetsConfigSubstitutions substitutions;
    collection.load_presets(directory.path.string(), folder, substitutions, ForwardCompatibilitySubstitutionRule::Enable);
    const std::string key = type == Preset::TYPE_PRINTER ? "max_print_height" :
                           type == Preset::TYPE_SLA_MATERIAL ? "exposure_time" : "layer_height";
    collection.get_edited_preset().config.set(key, 1.);
    REQUIRE(collection.save_current_preset("Middle"));
    REQUIRE(collection.save_current_preset("Zebra"));
    collection.select_preset_by_name("Middle", true);
    collection.get_edited_preset().config.set(key, 2.);
    collection.update_dirty();
    REQUIRE(collection.current_is_dirty());
    const auto original = directory.path / folder / "Middle.ini";
    auto bytes = [](const boost::filesystem::path &path) {
        std::ifstream stream(path.string(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const auto saved = bytes(original);
    SECTION("Destination collision and missing source preserve the entire draft") {
        const auto occupied = directory.path / folder / "Occupied.ini";
        std::ofstream(occupied.string()) << "keep this unrelated file";
        REQUIRE_THROWS(collection.rename_current_preset("Occupied"));
        REQUIRE(bytes(occupied) == "keep this unrelated file");
        REQUIRE_THROWS(collection.rename_current_preset("Zebra"));
        REQUIRE_THROWS(collection.rename_current_preset("../outside"));
        boost::filesystem::rename(original, original.string() + ".retained");
        REQUIRE_THROWS(collection.rename_current_preset("Absent"));
        boost::filesystem::rename(original.string() + ".retained", original);
        REQUIRE(bytes(original) == saved);
        REQUIRE(collection.get_selected_preset_name() == "Middle");
        REQUIRE(collection.get_selected_preset().config.opt_float(key) == 1.);
        REQUIRE(collection.get_edited_preset().config.opt_float(key) == 2.);
        REQUIRE(collection.current_is_dirty());
    }
    SECTION("Sorting and UTF-8 rename do not reload or save the draft") {
        REQUIRE_FALSE(collection.rename_current_preset("Middle"));
        for (const std::string name : {"Alpha", "Łódź"}) {
            REQUIRE(collection.rename_current_preset(name));
            REQUIRE(collection.get_selected_preset_name() == name);
            REQUIRE(collection.get_edited_preset().name == name);
            REQUIRE(collection.get_selected_preset().file == collection.path_from_name(name));
            REQUIRE(collection.get_edited_preset().file == collection.path_from_name(name));
            REQUIRE(collection.get_selected_preset().config.opt_float(key) == 1.);
            REQUIRE(collection.get_edited_preset().config.opt_float(key) == 2.);
            REQUIRE(collection.current_is_dirty());
            REQUIRE(bytes(collection.path_from_name(name)) == saved);
        }
        REQUIRE_FALSE(boost::filesystem::exists(original));
        REQUIRE(collection.find_preset("Middle") == nullptr);
        REQUIRE_FALSE(collection.save_current_preset("Łódź"));
        REQUIRE_FALSE(collection.current_is_dirty());
        DynamicPrintConfig disk;
        disk.load(collection.path_from_name("Łódź"), ForwardCompatibilitySubstitutionRule::Enable);
        REQUIRE(disk.opt_float(key) == 2.);
    }
}

TEST_CASE("Renaming an SLA profile migrates saved and unsaved structured dependencies", "[sla_profiles][sla_profile_dependencies]")
{
    struct Directory {
        boost::filesystem::path path = boost::filesystem::absolute(boost::filesystem::unique_path("preset-dependencies-%%%%"));
        Directory() { boost::filesystem::create_directory(path); }
        ~Directory() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
    } directory;
    const auto renamed_type = GENERATE(Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL);
    PresetBundle bundle;
    PresetsConfigSubstitutions substitutions;
    for (const auto type : {Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL}) {
        auto &collection = bundle.get_presets(type);
        const auto folder = collection.section_name();
        boost::filesystem::create_directory(directory.path / folder);
        collection.load_presets(directory.path.string(), folder, substitutions, ForwardCompatibilitySubstitutionRule::Enable);
        auto &config = collection.get_edited_preset().config;
        if (type == Preset::TYPE_PRINTER) {
            config.set("default_sla_print_profile", std::string("Base"));
            config.set("default_sla_material_profile", std::string("Base"));
        } else {
            config.set_key_value("compatible_printers", new ConfigOptionStrings({"Base", "Base suffix"}));
            config.set("compatible_printers_condition", std::string("printer_preset == \"Base\""));
            if (type == Preset::TYPE_SLA_MATERIAL)
                config.set_key_value("compatible_prints", new ConfigOptionStrings({"Base", "Base suffix"}));
        }
        REQUIRE(collection.save_current_preset("Base"));
        collection.get_edited_preset().config.set("inherits", std::string("Base"));
        REQUIRE(collection.save_current_preset("Child"));
        collection.select_preset_by_name("Base", true);
        collection.update_saved_preset_from_current_preset();
        const char *key = type == Preset::TYPE_PRINTER ? "max_print_height" :
                          type == Preset::TYPE_SLA_PRINT ? "layer_height" : "exposure_time";
        collection.get_edited_preset().config.set(key, 42.);
        collection.update_dirty();
    }
    // This is the existing GUI entry point; the bundle-level transaction will
    // replace it once the failure is demonstrated on the current implementation.
    REQUIRE(bundle.get_presets(renamed_type).rename_current_preset("Łódź"));
    for (const auto type : {Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL}) {
        auto &collection = bundle.get_presets(type);
        CHECK(collection.get_selected_preset_name() == (type == renamed_type ? "Łódź" : "Base"));
        CHECK(collection.current_is_dirty());
        const char *key = type == Preset::TYPE_PRINTER ? "max_print_height" :
                          type == Preset::TYPE_SLA_PRINT ? "layer_height" : "exposure_time";
        CHECK(collection.get_edited_preset().config.opt_float(key) == 42.);
        CHECK(collection.get_selected_preset().config.opt_float(key) != 42.);
        DynamicPrintConfig disk;
        disk.load(collection.get_selected_preset().file, ForwardCompatibilitySubstitutionRule::Disable);
        auto check_references = [&](const DynamicPrintConfig &config) {
            const std::string expected = "Łódź";
            if (type == renamed_type) return;
            if (renamed_type == Preset::TYPE_PRINTER) {
                CHECK(config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{expected, "Base suffix"});
                CHECK(config.opt_string("compatible_printers_condition") == "printer_preset == \"Base\"");
            } else if (type == Preset::TYPE_PRINTER) {
                CHECK(config.opt_string(renamed_type == Preset::TYPE_SLA_PRINT ? "default_sla_print_profile" : "default_sla_material_profile") == expected);
            } else if (renamed_type == Preset::TYPE_SLA_PRINT && type == Preset::TYPE_SLA_MATERIAL) {
                CHECK(config.option<ConfigOptionStrings>("compatible_prints")->values == std::vector<std::string>{expected, "Base suffix"});
            }
        };
        check_references(disk);
        check_references(collection.get_selected_preset().config);
        check_references(collection.get_edited_preset().config);
        const auto *child = collection.find_preset("Child", false, false);
        REQUIRE(child);
        CHECK(child->inherits() == (type == renamed_type ? "Łódź" : "Base"));
        CHECK(collection.get_preset_parent(*child) != nullptr);
        DynamicPrintConfig child_disk;child_disk.load(child->file, ForwardCompatibilitySubstitutionRule::Disable);
        CHECK(child_disk.opt_string("inherits") == child->inherits());
        check_references(child_disk);
    }
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
    // SLA-only build: even the legacy slot 0 default carries the SLA schema.
    REQUIRE(bundle.printers.default_preset(0).printer_technology() == ptSLA);
    bundle.printers.select_preset_by_name("", true);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    bundle.printers.select_preset_by_name("- default FFF -", true);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    // The legacy slot is never what the SLA-only UI shows or stores: it resolves to "- default SLA -".
    CHECK(bundle.printers.get_selected_preset_name() == "- default SLA -");
    CHECK_FALSE(bundle.printers.default_preset(0).is_visible);
    bundle.printers.select_preset(0);
    CHECK(bundle.printers.get_selected_preset_name() == "- default SLA -");
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

TEST_CASE("Project-local SLA preset labels do not replace persisted role identities", "[sla_profiles][sla_project_identity]") {
    DynamicPrintConfig source;source.apply(SLAFullPrintConfig{});source.option("printer_technology")->setInt(ptSLA);
    source.set_key_value("printer_settings_id",new ConfigOptionString("Drukarka Żółta"));
    source.set_key_value("sla_material_settings_id",new ConfigOptionString("Żywica testowa"));
    source.set_key_value("sla_print_settings_id",new ConfigOptionString("Proces dokładny"));
    PresetBundle first;first.load_config_model("first.3mf",source);
    REQUIRE(first.printers.get_edited_preset().is_external);
    REQUIRE(first.sla_materials.get_edited_preset().is_external);
    REQUIRE(first.sla_prints.get_edited_preset().is_external);
    CHECK(first.printers.get_selected_preset_name()!=source.opt_string("printer_settings_id"));
    const auto saved=first.full_config();
    CHECK(saved.opt_string("printer_settings_id")==source.opt_string("printer_settings_id"));
    CHECK(saved.opt_string("sla_material_settings_id")==source.opt_string("sla_material_settings_id"));
    CHECK(saved.opt_string("sla_print_settings_id")==source.opt_string("sla_print_settings_id"));
    PresetBundle restarted;restarted.load_config_model("second.3mf",saved);
    const auto resaved=restarted.full_config();
    CHECK(sla::capture_project_settings(saved)==sla::capture_project_settings(resaved));
    // A user library preset remains identified by its actual name even when
    // copied from an imported config containing an older settings_id.
    restarted.printers.load_preset("","User printer",source,true);
    CHECK_FALSE(restarted.printers.get_edited_preset().is_external);
    CHECK(restarted.full_config().opt_string("printer_settings_id")=="User printer");
    // Legacy unnamed input receives one contextual identity, retained later.
    source.set("printer_settings_id","");source.set("sla_material_settings_id","");source.set("sla_print_settings_id","");
    PresetBundle legacy;legacy.load_config_model("unnamed.ini",source);
    const auto named=legacy.full_config();
    CHECK_FALSE(named.opt_string("printer_settings_id").empty());
    PresetBundle again;again.load_config_model("legacy.3mf",named);
    CHECK(again.full_config().opt_string("printer_settings_id")==named.opt_string("printer_settings_id"));
}

TEST_CASE("Printer export preference follows source identity across project labels", "[sla_profiles][sla_printer_preference_identity]")
{
    AppConfig preferences(AppConfig::EAppMode::Editor);
    preferences.set_sla_export_format("Drukarka Łódź", "cxdlpv4");
    preferences.set_sla_export_format("Other printer", "goo");
    DynamicPrintConfig source;
    source.apply(SLAFullPrintConfig::defaults());
    source.set_key_value("printer_settings_id", new ConfigOptionString("Drukarka Łódź"));
    source.set_key_value("sla_print_settings_id", new ConfigOptionString("Process"));
    source.set_key_value("sla_material_settings_id", new ConfigOptionString("Material"));
    for (const auto *filename : {"first.3mf", "saved-again.3mf"}) {
        PresetBundle bundle;
        bundle.load_config_model(filename, source);
        REQUIRE(bundle.printers.get_edited_preset().is_external);
        REQUIRE(bundle.printers.get_selected_preset_name() != "Drukarka Łódź");
        const auto identity=bundle.printers.get_selected_preset_source_name("printer_settings_id");
        REQUIRE(preferences.sla_export_format(identity) == "cxdlpv4");
        preferences.set_sla_export_format(identity, "prz");
        REQUIRE(preferences.sla_export_format("Drukarka Łódź") == "prz");
        REQUIRE(preferences.sla_export_format("Other printer") == "goo");
        source=bundle.full_config();
        preferences.set_sla_export_format(identity, "cxdlpv4");
    }
    PresetBundle library;
    library.printers.load_preset("", "New library printer", source, true);
    REQUIRE_FALSE(library.printers.get_edited_preset().is_external);
    REQUIRE(library.printers.get_selected_preset_source_name("printer_settings_id") == "New library printer");
    REQUIRE(preferences.sla_export_format("New library printer").empty());
    source.set("printer_settings_id", std::string());
    PresetBundle legacy;
    legacy.load_config_model("legacy.ini", source);
    REQUIRE(legacy.printers.get_selected_preset_source_name("printer_settings_id") == legacy.printers.get_selected_preset_name());
}

TEST_CASE("SLA project presets retain role metadata in a fresh library", "[sla_profiles][sla_project_preset_metadata]")
{
    PresetBundle source;
    auto printer=source.printers.get_edited_preset().config;
    printer.set("printer_model",std::string("CL-60"));
    printer.set("printer_vendor",std::string("Offline vendor"));
    printer.set("printer_variant",std::string("LCD revision B"));
    printer.set("default_sla_print_profile",std::string("Default process"));
    printer.set("default_sla_material_profile",std::string("Default resin"));
    printer.set("inherits",std::string("Missing printer base"));
    printer.set("display_width",192.);
    Preset::normalize(printer);
    source.printers.load_preset("","Drukarka Łódź",printer,true);
    auto material=source.sla_materials.get_edited_preset().config;
    material.set_key_value("compatible_printers",new ConfigOptionStrings({"Drukarka Łódź","Printer; \"B\""}));
    material.set_key_value("compatible_prints",new ConfigOptionStrings({"Process precise","Process; B"}));
    material.set("compatible_printers_condition",std::string("printer_model == \"CL-60\""));
    material.set_key_value("compatible_prints_condition",new ConfigOptionString("layer_height <= 0.05"));
    material.set("inherits",std::string("Missing material base"));
    material.set("default_sla_material_profile",std::string("Material role default"));
    material.set("exposure_time",7.25);
    source.sla_materials.load_preset("","Żywica testowa",material,true);
    auto process=source.sla_prints.get_edited_preset().config;
    process.set_key_value("compatible_printers",new ConfigOptionStrings({"Printer; \"B\"","Drukarka Łódź"}));
    process.set("compatible_printers_condition",std::string("display_pixels_x >= 1920"));
    process.set("inherits",std::string("Missing process base"));
    process.set("default_sla_print_profile",std::string("Process role default"));
    process.set("hollowing_min_thickness",2.7);
    source.sla_prints.load_preset("","Proces dokładny",process,true);
    const auto combined=source.full_config();
    auto verify=[&](const DynamicPrintConfig &configuration) {
        PresetBundle reopened;reopened.load_config_model("project.3mf",configuration);
        const auto &p=reopened.printers.get_edited_preset().config;
        const auto &m=reopened.sla_materials.get_edited_preset().config;
        const auto &s=reopened.sla_prints.get_edited_preset().config;
        for(const auto *key:{"printer_model","printer_vendor","printer_variant","default_sla_print_profile","default_sla_material_profile","inherits"}) {
            CAPTURE(key);CHECK(p.opt_serialize(key)==printer.opt_serialize(key));
        }
        for(const auto *key:{"compatible_printers","compatible_prints","compatible_printers_condition","compatible_prints_condition","default_sla_material_profile","inherits"}) {
            CAPTURE(key);CHECK(m.opt_serialize(key)==material.opt_serialize(key));
        }
        for(const auto *key:{"compatible_printers","compatible_printers_condition","default_sla_print_profile","inherits"}) {
            CAPTURE(key);CHECK(s.opt_serialize(key)==process.opt_serialize(key));
        }
        CHECK(p.opt_float("display_width")==192.);
        CHECK(m.opt_float("exposure_time")==7.25);
        CHECK(s.opt_float("hollowing_min_thickness")==2.7);
        // The project is independent of all three absent parent definitions.
        CHECK(reopened.printers.get_selected_preset_parent()==nullptr);
        CHECK(reopened.sla_materials.get_selected_preset_parent()==nullptr);
        CHECK(reopened.sla_prints.get_selected_preset_parent()==nullptr);
    };
    SECTION("model configuration") {verify(combined);}
    SECTION("layer project settings") {verify(sla::restore_project_settings(sla::capture_project_settings(combined)));}
}

TEST_CASE("SLA preset metadata rejects ambiguous and unsafe role definitions", "[sla_profiles][sla_preset_metadata_validation]")
{
    PresetBundle bundle;
    auto combined=bundle.full_config();
    auto settings=sla::capture_project_settings(combined);
    REQUIRE(settings.printer.metadata.has_value());
    CHECK_FALSE(settings.printer.metadata->count("printer_model"));
    REQUIRE(settings.material.metadata.has_value());
    REQUIRE(settings.process.metadata.has_value());
    SECTION("unknown metadata key") {(*settings.printer.metadata)["future_setting"]="must not disappear";}
    SECTION("credential replacing a required key") {settings.printer.metadata->erase("inherits");(*settings.printer.metadata)["printhost_apikey"]="test-placeholder";}
    SECTION("FFF field") {settings.process.metadata->erase("inherits");(*settings.process.metadata)["infill_density"]="20%";}
    SECTION("wrong role") {settings.material.metadata->erase("inherits");(*settings.material.metadata)["printer_model"]="CL-60";}
    SECTION("missing field") {settings.material.metadata->erase("compatible_prints");}
    SECTION("noncanonical list") {(*settings.material.metadata)["compatible_printers"]="\"unterminated";}
    SECTION("embedded NUL") {(*settings.printer.metadata)["printer_variant"]=std::string("CL\0-60",6);}
    SECTION("oversized field") {(*settings.process.metadata)["inherits"]=std::string(1024*1024+1,'x');}
    SECTION("self inheritance") {settings.printer.name="Loop";(*settings.printer.metadata)["inherits"]="Loop";}
    REQUIRE_THROWS(sla::restore_project_settings(settings));
}

TEST_CASE("Serialized SLA preset metadata rejects duplicate keys", "[sla_profiles][sla_preset_metadata_validation]")
{
    PresetBundle bundle;auto combined=bundle.full_config();
    auto encoded=combined.opt_string("sla_preset_metadata");
    encoded.insert(1,"\"version\":1,");combined.set("sla_preset_metadata",encoded);
    REQUIRE_THROWS(sla::capture_project_settings(combined));
}

TEST_CASE("SLA compatibility uses source identities of external project presets", "[sla_profiles][sla_project_preset_compatibility]")
{
    PresetBundle source;
    auto printer=source.printers.get_edited_preset().config;
    source.printers.load_preset("","Drukarka Łódź",printer,true);
    auto material=source.sla_materials.get_edited_preset().config;
    material.set_key_value("compatible_printers",new ConfigOptionStrings({"Drukarka Łódź"}));
    material.set_key_value("compatible_prints",new ConfigOptionStrings({"Proces dokładny"}));
    source.sla_materials.load_preset("","Żywica testowa",material,true);
    auto process=source.sla_prints.get_edited_preset().config;
    process.set_key_value("compatible_printers",new ConfigOptionStrings({"Drukarka Łódź"}));
    source.sla_prints.load_preset("","Proces dokładny",process,true);
    PresetBundle reopened;reopened.load_config_model("project.3mf",source.full_config());
    const auto &p=reopened.printers.get_edited_preset();
    const auto &s=reopened.sla_prints.get_edited_preset();
    const auto &m=reopened.sla_materials.get_edited_preset();
    CHECK(s.is_compatible);CHECK(m.is_compatible);
    CHECK(is_compatible_with_printer(PresetWithVendorProfile(m,nullptr),PresetWithVendorProfile(p,nullptr)));
    CHECK(is_compatible_with_print(PresetWithVendorProfile(m,nullptr),PresetWithVendorProfile(s,nullptr),PresetWithVendorProfile(p,nullptr)));
    auto other=p;other.config.set("printer_settings_id",std::string("Other printer"));
    CHECK_FALSE(is_compatible_with_printer(PresetWithVendorProfile(m,nullptr),PresetWithVendorProfile(other,nullptr)));
    other.is_external=false;other.name="New library printer";
    other.config.set("printer_settings_id",std::string("Drukarka Łódź"));
    CHECK_FALSE(is_compatible_with_printer(PresetWithVendorProfile(m,nullptr),PresetWithVendorProfile(other,nullptr)));
    auto conditional=process;conditional.option<ConfigOptionStrings>("compatible_printers")->values.clear();
    conditional.set("compatible_printers_condition",std::string("printer_preset == \"Drukarka Łódź\""));
    reopened.sla_prints.load_preset("","Conditional process",conditional,true);
    reopened.update_compatible(PresetSelectCompatibleType::Never);
    CHECK(reopened.sla_prints.get_edited_preset().is_compatible);
}

TEST_CASE("Invalid compatibility conditions never make an SLA preset universal", "[sla_profiles][sla_compatibility_validation]")
{
    PresetBundle bundle;
    auto config = bundle.sla_materials.get_edited_preset().config;
    config.set_key_value("compatible_printers", new ConfigOptionStrings{});
    config.set_key_value("compatible_prints", new ConfigOptionStrings{});
    bundle.sla_materials.load_preset("", "Restricted resin", config, true);
    auto &material = bundle.sla_materials.get_edited_preset();
    const auto printer = bundle.printers.get_edited_preset_with_vendor_profile();
    const auto process = bundle.sla_prints.get_edited_preset_with_vendor_profile();
    const auto condition = GENERATE(std::string("missing_option == 1"), std::string("layer_height =="), std::string("("));
    material.config.set("compatible_printers_condition", condition);
    material.config.set("compatible_prints_condition", condition);
    CHECK_FALSE(is_compatible_with_printer(PresetWithVendorProfile(material, nullptr), printer));
    CHECK_FALSE(is_compatible_with_print(PresetWithVendorProfile(material, nullptr), process, printer));
    CHECK_FALSE(evaluate_compatibility_with_printer(PresetWithVendorProfile(material, nullptr), printer).error.empty());
    CHECK_FALSE(evaluate_compatibility_with_print(PresetWithVendorProfile(material, nullptr), process, printer).error.empty());
    for (const auto &value : {std::string("false"), std::string("true"), std::string()}) {
        material.config.set("compatible_printers_condition", value);
        material.config.set("compatible_prints_condition", value);
        CHECK(is_compatible_with_printer(PresetWithVendorProfile(material, nullptr), printer) == (value != "false"));
        CHECK(is_compatible_with_print(PresetWithVendorProfile(material, nullptr), process, printer) == (value != "false"));
        CHECK(evaluate_compatibility_with_printer(PresetWithVendorProfile(material, nullptr), printer).error.empty());
        CHECK(evaluate_compatibility_with_print(PresetWithVendorProfile(material, nullptr), process, printer).error.empty());
    }
}

TEST_CASE("Compatibility diagnostics preserve explicit lists and dirty selections", "[sla_profiles][sla_compatibility_validation]")
{
    PresetBundle bundle;
    bundle.sla_prints.load_preset("", "Fine", bundle.sla_prints.get_edited_preset().config, true);
    auto resin = bundle.sla_materials.get_edited_preset().config;
    resin.set_key_value("compatible_prints", new ConfigOptionStrings{});
    resin.set_key_value("compatible_printers", new ConfigOptionStrings{});
    resin.set("compatible_prints_condition", std::string("print_preset == \"Fine\""));
    bundle.sla_materials.load_preset("", "Restricted resin", resin, true);
    auto &material = bundle.sla_materials.get_edited_preset();
    auto check = [&] { return evaluate_compatibility_with_print(PresetWithVendorProfile(material, nullptr),
        bundle.sla_prints.get_edited_preset_with_vendor_profile(), bundle.printers.get_edited_preset_with_vendor_profile()); };
    REQUIRE(check().compatible);
    REQUIRE(check().error.empty());
    auto &process = bundle.sla_prints.get_edited_preset();
    process.is_external = true;
    process.name = "Project-local label";
    process.config.set("sla_print_settings_id", std::string("Fine"));
    REQUIRE(check().compatible);
    process.config.set("sla_print_settings_id", std::string("Other"));
    REQUIRE_FALSE(check().compatible);
    material.config.set("compatible_prints_condition", std::string("broken +"));
    material.config.option<ConfigOptionStrings>("compatible_prints")->values = {"Other"};
    REQUIRE(check().compatible); // Explicit list takes precedence over a dormant condition.
    REQUIRE(check().error.empty());
    material.config.option<ConfigOptionStrings>("compatible_prints")->values = {"Missing profile"};
    REQUIRE_FALSE(check().compatible);
    REQUIRE(check().error.empty());
    material.config.option<ConfigOptionStrings>("compatible_prints")->values.clear();
    material.config.set("exposure_time", 4.25);
    bundle.sla_materials.update_dirty();
    bundle.update_compatible(PresetSelectCompatibleType::Never);
    REQUIRE(bundle.sla_materials.get_selected_preset_name() == "Restricted resin");
    REQUIRE_FALSE(material.is_compatible);
    REQUIRE(material.config.opt_float("exposure_time") == 4.25);
    REQUIRE(bundle.sla_materials.current_is_dirty());
}

TEST_CASE("Legacy resolved configs retain explicit printer model metadata", "[sla_profiles][sla_legacy_preset_metadata]")
{
    DynamicPrintConfig configuration;configuration.apply(SLAFullPrintConfig{});
    configuration.set_key_value("printer_settings_id",new ConfigOptionString("Legacy printer"));
    configuration.set_key_value("printer_model",new ConfigOptionString("CL-60"));
    configuration.set_key_value("printer_vendor",new ConfigOptionString("Offline vendor"));
    configuration.set_key_value("printer_variant",new ConfigOptionString("LCD revision B"));
    configuration.set_key_value("default_sla_print_profile",new ConfigOptionString("Legacy default process"));
    configuration.set_key_value("inherits_cummulative",new ConfigOptionStrings({"Process base","Material base","Printer base"}));
    configuration.set_key_value("printhost_apikey",new ConfigOptionString("test-placeholder"));
    const auto settings=sla::capture_project_settings(configuration);
    REQUIRE(settings.printer.metadata.has_value());
    CHECK_FALSE(settings.material.metadata.has_value());CHECK_FALSE(settings.process.metadata.has_value());
    const auto restored=sla::restore_project_settings(settings);
    for(const auto *key:{"printer_model","printer_vendor","printer_variant","default_sla_print_profile"})
        CHECK(restored.opt_serialize(key)==configuration.opt_serialize(key));
    CHECK(settings.printer.metadata->at("inherits")=="Printer base");
    CHECK_FALSE(restored.has("printhost_apikey"));
    CHECK(sla::capture_project_settings(restored)==settings);
}

TEST_CASE("Import preserves changed SLA metadata when an installed preset has equal print values", "[sla_profiles][sla_installed_preset_metadata]")
{
    PresetBundle source,installed;
    auto prepare=[&](PresetCollection &from,PresetCollection &to,const char *name) {
        auto config=from.get_edited_preset().config;
        config.set("inherits",std::string("Source base"));
        if(from.type()==Preset::TYPE_PRINTER) {
            config.set("printer_model",std::string("CL-60"));
            config.set("default_sla_print_profile",std::string("Source default process"));
        } else config.set_key_value("compatible_printers",new ConfigOptionStrings({"Source printer"}));
        from.load_preset("",name,config,true);
        auto old=config;old.set("inherits",std::string("Installed base"));
        if(from.type()==Preset::TYPE_PRINTER) {
            old.set("printer_model",std::string("CL-92"));
            old.set("default_sla_print_profile",std::string("Installed default process"));
        } else old.set_key_value("compatible_printers",new ConfigOptionStrings({"Other printer"}));
        to.load_preset("",name,old,true);
    };
    prepare(source.printers,installed.printers,"Source printer");
    prepare(source.sla_materials,installed.sla_materials,"Source material");
    prepare(source.sla_prints,installed.sla_prints,"Source process");
    const auto expected=sla::capture_project_settings(source.full_config());
    installed.load_config_model("source.3mf",source.full_config());
    CHECK(sla::capture_project_settings(installed.full_config())==expected);
    for(const auto *role:std::vector<const PresetCollection*>{&installed.printers,&installed.sla_materials,&installed.sla_prints}) {
        CHECK(role->current_is_dirty());
        CHECK(role->get_selected_preset().config.opt_string("inherits")=="Installed base");
        CHECK(role->get_edited_preset().config.opt_string("inherits")=="Source base");
    }
    CHECK(installed.printers.get_selected_preset().config.opt_string("printer_model")=="CL-92");
    CHECK(installed.printers.get_edited_preset().config.opt_string("printer_model")=="CL-60");
}

TEST_CASE("A missing SLA child keeps its source identity when its base is installed", "[sla_profiles][sla_installed_preset_metadata]")
{
    PresetBundle source,installed;
    auto prepare=[&](PresetCollection &from,PresetCollection &to,const char *base,const char *child) {
        auto config=from.get_edited_preset().config;
        to.load_preset("",base,config,true);
        config.set("inherits",std::string(base));
        from.load_preset("",child,config,true);
    };
    prepare(source.printers,installed.printers,"Printer base","Child printer");
    prepare(source.sla_materials,installed.sla_materials,"Material base","Child material");
    prepare(source.sla_prints,installed.sla_prints,"Process base","Child process");
    const auto expected=sla::capture_project_settings(source.full_config());
    installed.load_config_model("child.3mf",source.full_config());
    CHECK(sla::capture_project_settings(installed.full_config())==expected);
    for(const auto *role:std::vector<const PresetCollection*>{&installed.printers,&installed.sla_materials,&installed.sla_prints}) {
        CHECK(role->get_edited_preset().is_external);
        CHECK(role->get_selected_preset_parent()!=nullptr);
    }
    CHECK(installed.printers.find_preset("Printer base")->config.opt_string("inherits").empty());
    CHECK(installed.sla_materials.find_preset("Material base")->config.opt_string("inherits").empty());
    CHECK(installed.sla_prints.find_preset("Process base")->config.opt_string("inherits").empty());
}

TEST_CASE("Unnamed legacy SLA children retain a project context above installed bases", "[sla_profiles][sla_unnamed_preset_metadata]")
{
    PresetBundle source,installed;
    auto prepare=[&](PresetCollection &from,PresetCollection &to,const char *base,const char *child) {
        auto config=from.get_edited_preset().config;
        to.load_preset("",base,config,true);
        config.set("inherits",std::string(base));from.load_preset("",child,config,true);
    };
    prepare(source.printers,installed.printers,"Printer base","Child printer");
    prepare(source.sla_materials,installed.sla_materials,"Material base","Child material");
    prepare(source.sla_prints,installed.sla_prints,"Process base","Child process");
    auto config=source.full_config();
    for(const auto *key:{"printer_settings_id","sla_material_settings_id","sla_print_settings_id"})config.set(key,std::string());
    installed.load_config_model("legacy.ini",config);
    for(const auto *role:std::vector<const PresetCollection*>{&installed.printers,&installed.sla_materials,&installed.sla_prints}) {
        CHECK(role->get_edited_preset().is_external);
        CHECK(role->get_selected_preset_parent()!=nullptr);
        CHECK(role->get_edited_preset().inherits()!=role->get_selected_preset_name());
    }
    const auto saved=installed.full_config();
    REQUIRE_NOTHROW(sla::capture_project_settings(saved));
    for(const auto *key:{"printer_settings_id","sla_material_settings_id","sla_print_settings_id"})CHECK_FALSE(saved.opt_string(key).empty());
    PresetBundle restarted;restarted.load_config_model("saved.3mf",saved);
    CHECK(sla::capture_project_settings(restarted.full_config())==sla::capture_project_settings(saved));
}

TEST_CASE("Invalid complete LCD geometry cannot publish a new physical bed",
          "[sla_profiles][sla_geometry_validation]")
{
    // Runtime RED: the old synchronizer accepts bad pixels/Z/orientation and
    // publishes changed dimensions. Rejection must preserve both user input
    // and the last valid bed; a caller can then report the invalid field.
    const auto key = GENERATE("display_width", "display_height", "max_print_height",
                             "display_pixels_x", "display_pixels_y", "display_orientation");
    const auto scenario = GENERATE(0, 1, 2, 3);
    auto config = printer_config();
    Preset::normalize(config);
    const auto original_bed = config.option<ConfigOptionPoints>("bed_shape")->values;
    config.set("display_width", 192.);
    config.set("display_height", 120.);
    if (std::string(key) == "display_pixels_x" || std::string(key) == "display_pixels_y") {
        const int invalid[] = {0, -1, 32769, std::numeric_limits<int>::max()};
        config.set(key, invalid[scenario]);
    } else if (std::string(key) == "display_orientation") {
        const int invalid[] = {-1, 2, 3, std::numeric_limits<int>::max()};
        config.set_key_value(key, new ConfigOptionEnum<SLADisplayOrientation>(
            static_cast<SLADisplayOrientation>(invalid[scenario])));
    } else {
        const double invalid[] = {0., -1., std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN()};
        config.set(key, invalid[scenario]);
    }
    const bool integer_field = std::string(key) == "display_orientation" ||
        std::string(key) == "display_pixels_x" || std::string(key) == "display_pixels_y";
    // Invalid floats/enums must not be serialized merely to compare snapshots:
    // serialization correctly rejects NaN/inf and unknown enum indices.
    const int before_integer = integer_field ? config.option(key)->getInt() : 0;
    const double before_float = integer_field ? 0. : config.option(key)->getFloat();
    CAPTURE(key, scenario);
    CHECK_FALSE(sla::synchronize_bed_shape(config));
    CHECK(config.option<ConfigOptionPoints>("bed_shape")->values == original_bed);
    if (integer_field)
        CHECK(config.option(key)->getInt() == before_integer);
    else if (std::isnan(before_float))
        CHECK(std::isnan(config.option(key)->getFloat()));
    else
        CHECK(config.option(key)->getFloat() == before_float);
}

TEST_CASE("LCD raster and coordinate limits protect bed synchronization",
          "[sla_profiles][sla_geometry_validation]")
{
    const auto scenario = GENERATE(0, 1, 2, 3);
    auto config = printer_config();
    Preset::normalize(config);
    const auto original_bed = config.option<ConfigOptionPoints>("bed_shape")->values;
    config.set("display_width", 192.);
    if (scenario == 0) {
        // Individual dimensions are legal, but total allocation exceeds the
        // same 128 Mi-pixel budget used by create_printer_raster.
        config.set("display_pixels_x", 32768);
        config.set("display_pixels_y", 4097);
    } else {
        config.set(scenario == 1 ? "display_width" : scenario == 2 ?
                   "display_height" : "max_print_height", 1e100);
    }
    CAPTURE(scenario);
    CHECK_FALSE(sla::synchronize_bed_shape(config));
    CHECK(config.option<ConfigOptionPoints>("bed_shape")->values == original_bed);
}

TEST_CASE("Legal anisotropic custom LCD retains physical axes in every orientation",
          "[sla_profiles][sla_geometry_validation]")
{
    const auto orientation = GENERATE(sladoLandscape, sladoPortrait);
    const auto mirror_x = GENERATE(false, true);
    const auto mirror_y = GENERATE(false, true);
    auto config = printer_config();
    config.set("display_width", 192.);
    config.set("display_height", 120.);
    config.set("display_pixels_x", 3840);
    config.set("display_pixels_y", 1600);
    config.set("max_print_height", 245.);
    config.set_key_value("display_orientation", new ConfigOptionEnum<SLADisplayOrientation>(orientation));
    config.set("display_mirror_x", mirror_x);
    config.set("display_mirror_y", mirror_y);
    REQUIRE(sla::synchronize_bed_shape(config));
    require_bed(config, 192., 120.);
    REQUIRE(config.opt_int("display_pixels_x") == 3840);
    REQUIRE(config.opt_int("display_pixels_y") == 1600);
    REQUIRE(config.opt_float("max_print_height") == 245.);
    REQUIRE(config.opt_enum<SLADisplayOrientation>("display_orientation") == orientation);
    REQUIRE(config.opt_bool("display_mirror_x") == mirror_x);
    REQUIRE(config.opt_bool("display_mirror_y") == mirror_y);
    REQUIRE_FALSE(sla::synchronize_bed_shape(config));
}

TEST_CASE("Geometry validation supports small native fixtures and leaves partial roles alone",
          "[sla_profiles][sla_geometry_validation]")
{
    auto config = printer_config();
    config.set("display_width", 16.);
    config.set("display_height", 4.);
    config.set("display_pixels_x", 8);
    config.set("display_pixels_y", 4);
    REQUIRE(sla::printer_geometry_error(config).empty());
    REQUIRE(sla::synchronize_bed_shape(config));
    require_bed(config, 16., 4.);

    DynamicPrintConfig partial;
    partial.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    partial.set_key_value("display_width", new ConfigOptionFloat(16.));
    partial.set_key_value("display_height", new ConfigOptionFloat(4.));
    REQUIRE_FALSE(sla::printer_geometry_error(partial).empty());
    REQUIRE_FALSE(sla::synchronize_bed_shape(partial));
    REQUIRE_FALSE(partial.has("bed_shape"));
}

TEST_CASE("Every default printer preset is SLA and fallbacks never yield FFF", "[sla_profiles][sla_defaults]")
{
    PresetBundle bundle;
    REQUIRE(bundle.printers.num_default_presets() >= 1);
    for (size_t i = 0; i < bundle.printers.num_default_presets(); ++i) {
        INFO("default printer preset " << i);
        const Preset &preset = bundle.printers.default_preset(i);
        REQUIRE(preset.printer_technology() == ptSLA);
        REQUIRE(preset.config.has("display_width"));
        REQUIRE_FALSE(preset.config.has("nozzle_diameter"));
    }
    // Selecting index 0 explicitly, resetting (what deleting the last user printer ends in)
    // and an unknown index must all stay SLA.
    bundle.printers.select_preset(0);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    bundle.printers.select_preset(1000);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    bundle.reset(false);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
    bundle.printers.select_preset(0);
    REQUIRE(bundle.printers.get_edited_preset().printer_technology() == ptSLA);
}

TEST_CASE("Offline bundles ignore physical printers and print host keys", "[sla_profiles][sla_offline_profiles]")
{
    const auto dir = boost::filesystem::absolute(boost::filesystem::unique_path("sla-offline-%%%%-%%%%"));
    boost::filesystem::create_directories(dir);
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{ dir };
    const auto ini = dir / "bundle.ini";
    {
        std::ofstream out(ini.string());
        out << "[printer:Legacy Host SLA]\n"
               "printer_technology = SLA\n"
               "display_width = 120\n"
               "display_height = 68\n"
               "print_host = 192.168.0.5\n"
               "printhost_apikey = secret\n"
               "host_type = octoprint\n"
               "\n"
               "[physical_printer:Garage]\n"
               "preset_name = Legacy Host SLA\n"
               "preset_names = \"Legacy Host SLA\"\n"
               "print_host = 192.168.0.5\n"
               "host_type = octoprint\n"
               "\n"
               "[presets]\n"
               "printer = Legacy Host SLA\n"
               "physical_printer = Garage\n";
    }
    PresetBundle bundle;
    REQUIRE_NOTHROW(bundle.load_configbundle(ini.string(), PresetBundle::LoadConfigBundleAttributes(),
                                             ForwardCompatibilitySubstitutionRule::Enable));
    REQUIRE_FALSE(bundle.physical_printers.has_selection());
    REQUIRE(bundle.physical_printers.find_printer("Garage", true) == nullptr);
    size_t physical = 0;
    for (const PhysicalPrinter &printer : bundle.physical_printers) { (void) printer; ++physical; }
    REQUIRE(physical == 0);

    const Preset *printer = bundle.printers.find_preset("Legacy Host SLA");
    REQUIRE(printer != nullptr);
    REQUIRE(printer->printer_technology() == ptSLA);
    for (const char *key : { "print_host", "printhost_apikey", "printhost_cafile" })
        REQUIRE_FALSE(printer->config.has(key));
    REQUIRE_FALSE(contains(Preset::sla_printer_options(), std::string("print_host")));

    // Export must not contain host data, even when physical printers are requested.
    const auto out_path = dir / "export.ini";
    bundle.export_configbundle(out_path.string(), false, true);
    std::ifstream in(out_path.string());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(text.find("print_host") == std::string::npos);
    REQUIRE(text.find("printhost_") == std::string::npos);
    REQUIRE(text.find("[physical_printer") == std::string::npos);
    REQUIRE(text.find("[printer:Legacy Host SLA]") != std::string::npos);
}

TEST_CASE("SLA option lists and the hidden-key whitelist stay consistent", "[sla_profiles][sla_option_editors]")
{
    const auto &hidden = Preset::sla_hidden_option_keys();
    std::set<std::string> unique(hidden.begin(), hidden.end());
    REQUIRE(unique.size() == hidden.size());

    // No stale whitelist entries: each must be a real key of one of the SLA option lists.
    for (const std::string &key : hidden) {
        INFO(key);
        REQUIRE((contains(Preset::sla_print_options(), key) || contains(Preset::sla_material_options(), key) ||
                 contains(Preset::sla_printer_options(), key)));
    }
    // Options that used to lack an editor must stay editable, never be hidden again.
    for (const char *key : { "fast_tilt_time", "slow_tilt_time", "high_viscosity_tilt_time", "material_type", "material_vendor" }) {
        INFO(key);
        REQUIRE_FALSE(contains(hidden, std::string(key)));
    }
    REQUIRE(contains(Preset::sla_printer_options(), std::string("fast_tilt_time")));
    REQUIRE(contains(Preset::sla_material_options(), std::string("material_type")));
    // All material overrides are listed so the Material Overrides page covers them.
    for (const char *key : { "material_ow_support_head_front_diameter", "material_ow_support_head_penetration",
                             "material_ow_support_head_width", "material_ow_support_pillar_diameter",
                             "material_ow_branchingsupport_head_front_diameter", "material_ow_branchingsupport_head_penetration",
                             "material_ow_branchingsupport_head_width", "material_ow_branchingsupport_pillar_diameter",
                             "material_ow_support_points_density_relative", "material_ow_absolute_correction",
                             "material_ow_elefant_foot_compensation" })
        REQUIRE(contains(Preset::sla_material_options(), std::string(key)));
}

// ---------------------------------------------------------------------------
// Profile transactions, delete with dependencies, single-profile import/export
// ---------------------------------------------------------------------------
namespace {

// Temporary user-profile directory with one loaded bundle (printer + SLA print + material collections).
struct ProfileFixture {
    boost::filesystem::path path = boost::filesystem::absolute(boost::filesystem::unique_path("preset-txn-%%%%-%%%%"));
    PresetBundle bundle;
    static constexpr Preset::Type types[3] = { Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };

    ProfileFixture()
    {
        boost::filesystem::create_directories(path);
        PresetsConfigSubstitutions substitutions;
        for (const auto type : types) {
            auto &collection = bundle.get_presets(type);
            boost::filesystem::create_directory(path / collection.section_name());
            collection.load_presets(path.string(), collection.section_name(), substitutions, ForwardCompatibilitySubstitutionRule::Enable);
        }
    }
    ~ProfileFixture()
    {
        PresetFileTransaction::set_fault_injector({});
        boost::system::error_code ignored;
        boost::filesystem::remove_all(path, ignored);
    }

    // Save the edited preset of `type` under `name` after `edit`.
    void save(Preset::Type type, const std::string &name, const std::function<void(DynamicPrintConfig&)> &edit = {})
    {
        auto &collection = bundle.get_presets(type);
        if (edit) edit(collection.get_edited_preset().config);
        REQUIRE(collection.save_current_preset(name));
    }

    static void set_list(DynamicPrintConfig &config, const char *key, std::vector<std::string> values)
    {
        config.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    }

    // Bytes of every profile file plus a dump of every in-memory user preset (saved, edited), for exact comparisons.
    std::map<std::string, std::string> state() const
    {
        std::map<std::string, std::string> result;
        for (boost::filesystem::recursive_directory_iterator it(path), end; it != end; ++it)
            if (boost::filesystem::is_regular_file(it->path())) {
                std::ifstream stream(it->path().string(), std::ios::binary);
                result["disk:" + it->path().string()] = std::string(std::istreambuf_iterator<char>(stream), {});
            }
        auto dump = [](const DynamicPrintConfig &config) {
            std::string text;
            for (const auto &key : config.keys()) text += key + "=" + config.opt_serialize(key) + "\n";
            return text;
        };
        for (const auto type : types) {
            const auto &collection = bundle.get_presets(type);
            for (const Preset &preset : collection())
                result["mem:" + std::to_string(int(type)) + ":" + preset.name] = preset.file + "\n" + dump(preset.config);
            result["edited:" + std::to_string(int(type))] = collection.get_edited_preset().name + "\n" + dump(collection.get_edited_preset().config);
            result["selected:" + std::to_string(int(type))] = collection.get_selected_preset_name();
        }
        return result;
    }

    bool has_temp_files() const
    {
        for (boost::filesystem::recursive_directory_iterator it(path), end; it != end; ++it)
            if (it->path().filename().string().find(".sla-") == 0)
                return true;
        return false;
    }

    // Printer "Base" referenced by a child, by a process and by a material; process "Base" referenced by a
    // child, a material and the printer defaults; material "Base" referenced by the printer defaults.
    void build_referenced_set()
    {
        save(Preset::TYPE_SLA_MATERIAL, "Base", [](DynamicPrintConfig &c) {
            set_list(c, "compatible_printers", {"Base", "Other"});
            set_list(c, "compatible_prints", {"Base", "Other"});
        });
        save(Preset::TYPE_SLA_MATERIAL, "OnlyBase", [](DynamicPrintConfig &c) {
            set_list(c, "compatible_printers", {"Base"});
            set_list(c, "compatible_prints", {"Base"});
        });
        save(Preset::TYPE_SLA_MATERIAL, "Child", [](DynamicPrintConfig &c) {
            c.set("inherits", std::string("Base"));
            c.set("exposure_time", 7.);
            set_list(c, "compatible_printers", {});
            set_list(c, "compatible_prints", {});
        });
        save(Preset::TYPE_SLA_PRINT, "Base", [](DynamicPrintConfig &c) { set_list(c, "compatible_printers", {"Base", "Other"}); });
        save(Preset::TYPE_SLA_PRINT, "OnlyBase", [](DynamicPrintConfig &c) { set_list(c, "compatible_printers", {"Base"}); });
        save(Preset::TYPE_SLA_PRINT, "Child", [](DynamicPrintConfig &c) {
            c.set("inherits", std::string("Base"));
            c.set("layer_height", 0.07);
            set_list(c, "compatible_printers", {});
        });
        save(Preset::TYPE_PRINTER, "Other");
        save(Preset::TYPE_PRINTER, "Base", [](DynamicPrintConfig &c) {
            c.set("default_sla_print_profile", std::string("Base"));
            c.set("default_sla_material_profile", std::string("Base"));
        });
        save(Preset::TYPE_PRINTER, "Child", [](DynamicPrintConfig &c) { c.set("inherits", std::string("Base")); });
        // Keep "Other" selected so that Base can be deleted as a non-selected preset.
        for (const auto type : types) bundle.get_presets(type).select_preset_by_name(type == Preset::TYPE_PRINTER ? "Other" : "Child", true);
    }

    DynamicPrintConfig disk_config(Preset::Type type, const std::string &name) const
    {
        DynamicPrintConfig config;
        config.load(bundle.get_presets(type).path_from_name(name), ForwardCompatibilitySubstitutionRule::Disable);
        return config;
    }
};

std::vector<std::string> list_values(const DynamicPrintConfig &config, const char *key)
{
    return config.option<ConfigOptionStrings>(key)->values;
}

bool lists_none(const DynamicPrintConfig &config, const char *list_key, const char *condition_key)
{
    return list_values(config, list_key).empty() && config.opt_string(condition_key) == "false";
}
} // namespace

TEST_CASE("Rename with dependents is all-or-nothing when a dependent write fails", "[sla_profiles][sla_profile_transaction]")
{
    ProfileFixture fixture;
    fixture.build_referenced_set();
    const auto before = fixture.state();
    auto &printers = fixture.bundle.printers;
    printers.select_preset_by_name("Base", true);
    const auto selected_before = fixture.state();
    // Files published by the rename: new file, old file removal and every dependent; fail at each position.
    const size_t changes = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(5), size_t(6));
    CAPTURE(changes);
    size_t reached = 0;
    PresetFileTransaction::set_fault_injector([&](size_t index, bool rollback) {
        if (rollback) return;
        reached = index + 1;
        if (index == changes) throw std::runtime_error("injected write failure");
    });
    bool threw = false;
    try { printers.rename_current_preset("Renamed"); } catch (const std::runtime_error &error) {
        threw = true;
        CHECK(std::string(error.what()).find("injected write failure") != std::string::npos);
        CHECK(std::string(error.what()).find("No profile was changed") != std::string::npos);
    }
    PresetFileTransaction::set_fault_injector({});
    if (!threw) {
        // The fault index was beyond the number of changes: the rename succeeded completely.
        REQUIRE(reached <= changes);
        REQUIRE(printers.get_selected_preset_name() == "Renamed");
        return;
    }
    CHECK(fixture.state() == selected_before);
    CHECK_FALSE(fixture.has_temp_files());
    CHECK(printers.get_selected_preset_name() == "Base");
}

TEST_CASE("A failed rollback names the files that could not be restored", "[sla_profiles][sla_profile_transaction]")
{
    ProfileFixture fixture;
    fixture.build_referenced_set();
    fixture.bundle.printers.select_preset_by_name("Base", true);
    PresetFileTransaction::set_fault_injector([](size_t index, bool rollback) {
        if (!rollback && index == 3) throw std::runtime_error("injected write failure");
        if (rollback && index == 1) throw std::runtime_error("injected restore failure");
    });
    try {
        fixture.bundle.printers.rename_current_preset("Renamed");
        FAIL("rename must fail");
    } catch (const std::runtime_error &error) {
        const std::string message = error.what();
        CHECK(message.find("could not be restored") != std::string::npos);
        CHECK(message.find(".ini") != std::string::npos);
    }
    PresetFileTransaction::set_fault_injector({});
    CHECK_FALSE(fixture.has_temp_files());
}

TEST_CASE("Rename really failing on a read-only dependent leaves everything untouched", "[sla_profiles][sla_profile_transaction]")
{
    ProfileFixture fixture;
    fixture.build_referenced_set();
    fixture.bundle.printers.select_preset_by_name("Base", true);
    const auto before = fixture.state();
    // A directory in place of a dependent file makes the real rename fail (also for privileged users).
    const auto blocked = fixture.path / "sla_material" / "OnlyBase.ini";
    boost::filesystem::remove(blocked);
    boost::filesystem::create_directory(blocked);
    const auto with_blocker = fixture.state();
    REQUIRE_THROWS(fixture.bundle.printers.rename_current_preset("Renamed"));
    CHECK(fixture.state() == with_blocker);
    CHECK_FALSE(fixture.has_temp_files());
    CHECK(before.size() == with_blocker.size() + 1); // the blocker replaced exactly one file
}

TEST_CASE("Deleting a printer removes it from compatible_printers and never widens compatibility", "[sla_profiles][sla_profile_delete]")
{
    ProfileFixture fixture;
    fixture.build_referenced_set();
    const auto dependents = fixture.bundle.find_preset_dependents(Preset::TYPE_PRINTER, "Base");
    auto has = [&](Preset::Type type, const std::string &name, PresetDependent::Kind kind) {
        return std::any_of(dependents.begin(), dependents.end(), [&](const PresetDependent &d) { return d.type == type && d.name == name && d.kind == kind; });
    };
    CHECK(has(Preset::TYPE_PRINTER, "Child", PresetDependent::Kind::Inherits));
    CHECK(has(Preset::TYPE_SLA_PRINT, "Base", PresetDependent::Kind::CompatiblePrinters));
    CHECK(has(Preset::TYPE_SLA_PRINT, "OnlyBase", PresetDependent::Kind::CompatiblePrinters));
    CHECK(has(Preset::TYPE_SLA_MATERIAL, "OnlyBase", PresetDependent::Kind::CompatiblePrinters));
    CHECK_FALSE(has(Preset::TYPE_SLA_MATERIAL, "Child", PresetDependent::Kind::CompatiblePrinters));

    REQUIRE(fixture.bundle.delete_preset_with_dependencies(Preset::TYPE_PRINTER, "Base"));
    CHECK(fixture.bundle.printers.find_preset("Base", false, false) == nullptr);
    CHECK_FALSE(boost::filesystem::exists(fixture.bundle.printers.path_from_name("Base")));
    for (const auto type : {Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL}) {
        const auto &collection = fixture.bundle.get_presets(type);
        for (const bool from_disk : {false, true}) {
            const auto config = [&](const std::string &name) {
                return from_disk ? fixture.disk_config(type, name) : collection.find_preset(name, false, false)->config;
            };
            CHECK(list_values(config("Base"), "compatible_printers") == std::vector<std::string>{"Other"});
            // The only entry was removed: explicit None, not an empty list that means "all printers".
            CHECK(lists_none(config("OnlyBase"), "compatible_printers", "compatible_printers_condition"));
        }
    }
    // The child keeps its own values and no longer points to the removed parent.
    const auto *child = fixture.bundle.printers.find_preset("Child", false, false);
    REQUIRE(child != nullptr);
    CHECK(child->inherits().empty());
    CHECK(fixture.disk_config(Preset::TYPE_PRINTER, "Child").opt_string("inherits").empty());
    CHECK(fixture.bundle.printers.get_selected_preset_name() == "Other");
}

TEST_CASE("Deleting a process or material clears default profiles and compatible_prints", "[sla_profiles][sla_profile_delete]")
{
    ProfileFixture fixture;
    fixture.build_referenced_set();
    SECTION("process") {
        const auto dependents = fixture.bundle.find_preset_dependents(Preset::TYPE_SLA_PRINT, "Base");
        auto has = [&](Preset::Type type, const std::string &name, PresetDependent::Kind kind) {
            return std::any_of(dependents.begin(), dependents.end(), [&](const PresetDependent &d) { return d.type == type && d.name == name && d.kind == kind; });
        };
        CHECK(has(Preset::TYPE_PRINTER, "Base", PresetDependent::Kind::DefaultSlaPrint));
        CHECK(has(Preset::TYPE_SLA_MATERIAL, "OnlyBase", PresetDependent::Kind::CompatiblePrints));
        CHECK(has(Preset::TYPE_SLA_PRINT, "Child", PresetDependent::Kind::Inherits));
        REQUIRE(fixture.bundle.delete_preset_with_dependencies(Preset::TYPE_SLA_PRINT, "Base"));
        for (const bool from_disk : {false, true}) {
            const auto material = [&](const std::string &name) {
                return from_disk ? fixture.disk_config(Preset::TYPE_SLA_MATERIAL, name) : fixture.bundle.sla_materials.find_preset(name, false, false)->config;
            };
            CHECK(list_values(material("Base"), "compatible_prints") == std::vector<std::string>{"Other"});
            CHECK(lists_none(material("OnlyBase"), "compatible_prints", "compatible_prints_condition"));
            const auto printer = from_disk ? fixture.disk_config(Preset::TYPE_PRINTER, "Base") : fixture.bundle.printers.find_preset("Base", false, false)->config;
            CHECK(printer.opt_string("default_sla_print_profile").empty());
            CHECK(printer.opt_string("default_sla_material_profile") == "Base");
        }
        const auto *child = fixture.bundle.sla_prints.find_preset("Child", false, false);
        REQUIRE(child != nullptr);
        CHECK(child->inherits().empty());
        CHECK(child->config.opt_float("layer_height") == 0.07);
        CHECK(fixture.bundle.sla_prints.get_selected_preset_name() == "Child");
    }
    SECTION("material") {
        const auto dependents = fixture.bundle.find_preset_dependents(Preset::TYPE_SLA_MATERIAL, "Base");
        CHECK(std::any_of(dependents.begin(), dependents.end(), [](const PresetDependent &d) {
            return d.type == Preset::TYPE_PRINTER && d.kind == PresetDependent::Kind::DefaultSlaMaterial; }));
        REQUIRE(fixture.bundle.delete_preset_with_dependencies(Preset::TYPE_SLA_MATERIAL, "Base"));
        CHECK(fixture.disk_config(Preset::TYPE_PRINTER, "Base").opt_string("default_sla_material_profile").empty());
        CHECK(fixture.bundle.printers.find_preset("Base", false, false)->config.opt_string("default_sla_material_profile").empty());
        const auto *child = fixture.bundle.sla_materials.find_preset("Child", false, false);
        REQUIRE(child != nullptr);
        CHECK(child->inherits().empty());
        CHECK(child->config.opt_float("exposure_time") == 7.);
    }
}

TEST_CASE("Deleting the selected preset or failing midway keeps the rest consistent", "[sla_profiles][sla_profile_delete][sla_profile_transaction]")
{
    ProfileFixture fixture;
    fixture.build_referenced_set();
    const auto before = fixture.state();
    // Fail while publishing each possible change: nothing may change on disk or in memory.
    for (size_t failing = 0; failing < 6; ++failing) {
        CAPTURE(failing);
        PresetFileTransaction::set_fault_injector([&](size_t index, bool rollback) {
            if (!rollback && index == failing) throw std::runtime_error("injected write failure");
        });
        REQUIRE_THROWS(fixture.bundle.delete_preset_with_dependencies(Preset::TYPE_SLA_PRINT, "Base"));
        PresetFileTransaction::set_fault_injector({});
        CHECK(fixture.state() == before);
        CHECK_FALSE(fixture.has_temp_files());
    }
    // Deleting the selected preset selects a neighbour and keeps other collections' drafts.
    fixture.bundle.printers.select_preset_by_name("Base", true);
    fixture.bundle.sla_materials.get_edited_preset().config.set("exposure_time", 33.);
    fixture.bundle.sla_materials.update_dirty();
    REQUIRE(fixture.bundle.delete_preset_with_dependencies(Preset::TYPE_PRINTER, "Base"));
    CHECK(fixture.bundle.printers.get_selected_preset_name() != "Base");
    CHECK(fixture.bundle.sla_materials.get_edited_preset().config.opt_float("exposure_time") == 33.);
    // System, default and unknown presets are refused or reported as missing.
    CHECK_FALSE(fixture.bundle.delete_preset_with_dependencies(Preset::TYPE_PRINTER, "No such preset"));
    CHECK(fixture.bundle.find_preset_dependents(Preset::TYPE_PRINTER, "No such preset").empty());
}

TEST_CASE("Single SLA profiles round-trip through .ini export and import", "[sla_profiles][sla_profile_import_export]")
{
    ProfileFixture source;
    source.save(Preset::TYPE_PRINTER, "Printer A", [](DynamicPrintConfig &c) {
        c.set("display_width", 192.);
        c.set("display_height", 120.);
    });
    source.save(Preset::TYPE_SLA_MATERIAL, "Resin A", [](DynamicPrintConfig &c) { c.set("exposure_time", 4.5); });
    source.save(Preset::TYPE_SLA_PRINT, "Process A", [](DynamicPrintConfig &c) { c.set("layer_height", 0.04); });
    ProfileFixture target;
    const auto exports = source.path / "exports";
    boost::filesystem::create_directory(exports);

    for (const auto &[type, name, key, value] : std::vector<std::tuple<Preset::Type, std::string, std::string, double>>{
             {Preset::TYPE_PRINTER, "Printer A", "display_width", 192.},
             {Preset::TYPE_SLA_MATERIAL, "Resin A", "exposure_time", 4.5},
             {Preset::TYPE_SLA_PRINT, "Process A", "layer_height", 0.04}}) {
        CAPTURE(name);
        const auto file = (exports / (name + ".ini")).string();
        source.bundle.export_preset_ini(type, name, file);
        auto result = target.bundle.import_preset_ini(type, file);
        CHECK(result.name == name);
        CHECK_FALSE(result.renamed);
        const Preset *imported = target.bundle.get_presets(type).find_preset(name, false, false);
        REQUIRE(imported != nullptr);
        CHECK(imported->config.opt_float(key) == value);
        CHECK(boost::filesystem::is_regular_file(target.bundle.get_presets(type).path_from_name(name)));
        // The imported file loads back with the same values.
        CHECK(target.disk_config(type, name).opt_float(key) == value);

        // Name collision: a suggestion with a counter, never an overwrite.
        const auto bytes_before = target.state();
        result = target.bundle.import_preset_ini(type, file);
        CHECK(result.renamed);
        CHECK(result.name == name + " (2)");
        result = target.bundle.import_preset_ini(type, file);
        CHECK(result.name == name + " (3)");
        CHECK(target.bundle.get_presets(type).find_preset(name, false, false)->config.opt_float(key) == value);
        CHECK(target.bundle.import_preset_ini(type, file, "Custom name").name == "Custom name");
        CHECK(bytes_before.size() < target.state().size());
    }
    // Wrong kind and invalid suggested names are rejected without leaving files behind.
    const auto material_file = (exports / "Resin A.ini").string();
    const auto before = target.state();
    REQUIRE_THROWS_WITH(target.bundle.import_preset_ini(Preset::TYPE_PRINTER, material_file), Catch::Matchers::ContainsSubstring("material"));
    REQUIRE_THROWS(target.bundle.import_preset_ini(Preset::TYPE_SLA_MATERIAL, material_file, "../escape"));
    CHECK(target.state() == before);
    CHECK_FALSE(target.has_temp_files());
}

TEST_CASE("Importing a printer with invalid geometry or an FFF profile gives a readable error", "[sla_profiles][sla_profile_import_export]")
{
    ProfileFixture fixture;
    fixture.save(Preset::TYPE_PRINTER, "Valid");
    const auto exports = fixture.path / "exports";
    boost::filesystem::create_directory(exports);
    fixture.bundle.export_preset_ini(Preset::TYPE_PRINTER, "Valid", (exports / "valid.ini").string());

    // Rewrite one value of an otherwise valid exported file.
    auto broken = [&](const std::string &key, const std::string &value) {
        std::ifstream in((exports / "valid.ini").string());
        std::string text, line;
        const auto path = (exports / ("broken_" + key + ".ini")).string();
        std::ofstream out(path);
        while (std::getline(in, line))
            out << (line.rfind(key + " = ", 0) == 0 ? key + " = " + value : line) << "\n";
        return path;
    };
    // Inputs written by this test live under "exports"; compare only the profile storage.
    auto profiles_only = [&] {
        auto result = fixture.state();
        for (auto it = result.begin(); it != result.end();)
            it = it->first.find("exports") != std::string::npos ? result.erase(it) : std::next(it);
        return result;
    };
    const auto before = profiles_only();
    for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>>{
             {"display_width", "0"}, {"display_height", "-5"}, {"display_pixels_x", "0"}, {"max_print_height", "0"}}) {
        CAPTURE(key);
        REQUIRE_THROWS_WITH(fixture.bundle.import_preset_ini(Preset::TYPE_PRINTER, broken(key, value)),
                            Catch::Matchers::ContainsSubstring("Invalid printer geometry") && Catch::Matchers::ContainsSubstring(key));
    }
    // No preset or profile file appeared.
    CHECK(profiles_only() == before);

    const auto fff = (exports / "fff.ini").string();
    std::ofstream(fff) << "printer_technology = FFF\nnozzle_diameter = 0.4\n";
    REQUIRE_THROWS_WITH(fixture.bundle.import_preset_ini(Preset::TYPE_PRINTER, fff), Catch::Matchers::ContainsSubstring("not an SLA"));
    const auto empty = (exports / "empty.ini").string();
    std::ofstream(empty) << "# nothing here\n";
    REQUIRE_THROWS(fixture.bundle.import_preset_ini(Preset::TYPE_SLA_PRINT, empty));
    REQUIRE_THROWS(fixture.bundle.import_preset_ini(Preset::TYPE_SLA_PRINT, (exports / "missing.ini").string()));
}

TEST_CASE("SLA-only config bundle export has no FFF sections and imports back", "[sla_profiles][sla_profile_import_export]")
{
    ProfileFixture source;
    source.save(Preset::TYPE_PRINTER, "Bundle Printer", [](DynamicPrintConfig &c) { c.set("display_width", 150.); });
    source.save(Preset::TYPE_SLA_MATERIAL, "Bundle Resin", [](DynamicPrintConfig &c) { c.set("exposure_time", 6.); });
    const auto file = (source.path / "bundle.ini").string();
    source.bundle.export_configbundle(file, false, true);
    std::ifstream in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("[print:") == std::string::npos);
    CHECK(text.find("[filament:") == std::string::npos);
    CHECK(text.find("[physical_printer") == std::string::npos);
    CHECK(text.find("[printer:Bundle Printer]") != std::string::npos);
    CHECK(text.find("[sla_material:Bundle Resin]") != std::string::npos);
#ifdef SLIC3R_SLA_ONLY
    CHECK(text.find("\nprint = ") == std::string::npos);
    CHECK(text.find("\nfilament = ") == std::string::npos);
#endif
    CHECK_FALSE(source.has_temp_files());

    ProfileFixture target;
    const auto result = target.bundle.load_configbundle(file, PresetBundle::LoadConfigBundleAttributes(), ForwardCompatibilitySubstitutionRule::Enable);
    CHECK(result.second >= 2);
    REQUIRE(target.bundle.printers.find_preset("Bundle Printer", false, false) != nullptr);
    CHECK(target.bundle.printers.find_preset("Bundle Printer", false, false)->config.opt_float("display_width") == 150.);
    REQUIRE(target.bundle.sla_materials.find_preset("Bundle Resin", false, false) != nullptr);
}
