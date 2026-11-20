///|/ Copyright (c) Prusa Research 2017 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Enrico Turri @enricoturri1966, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Vojtěch Král @vojtechkral
///|/ Copyright (c) SuperSlicer 2021 Remi Durand @supermerill
///|/ Copyright (c) 2019 John Drake @foxox
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <cassert>

#include "libslic3r.h"
#include "PresetBundle.hpp"
#include "Utils.hpp"
#include "SLA/PrinterGeometry.hpp"
#include "Model.hpp"
#include "format.hpp"
#include "CustomParametersHandling.hpp"
#include "SLA/ProjectSettings.hpp"

#include <algorithm>
#include <set>
#include <fstream>
#include <unordered_set>
#include <boost/filesystem.hpp>
#include <boost/algorithm/clamp.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/locale.hpp>
#include <boost/log/trivial.hpp>

// Store the sla_print/sla_material/printer presets into a "presets" subdirectory of the Slic3rPE config dir.
// This breaks compatibility with the upstream Slic3r if the --datadir is used to switch between the two versions.
// #define SLIC3R_PROFILE_USE_PRESETS_SUBDIR

namespace Slic3r {

const char *PresetBundle::PRUSA_BUNDLE = "PrusaResearch";

PresetBundle::PresetBundle() :
    sla_materials(Preset::TYPE_SLA_MATERIAL, Preset::sla_material_options(), static_cast<const SLAMaterialConfig&>(SLAFullPrintConfig::defaults())), 
    sla_prints(Preset::TYPE_SLA_PRINT, Preset::sla_print_options(), static_cast<const SLAPrintObjectConfig&>(SLAFullPrintConfig::defaults())),
    // A single default printer, "- default SLA -", at index 0 (the former "- default FFF -" slot is gone).
    printers(Preset::TYPE_PRINTER, Preset::sla_printer_options(), static_cast<const SLAMaterialConfig&>(SLAFullPrintConfig::defaults()), "- default SLA -")
{
    this->install_rename_hooks();
    // The following keys are handled by the UI, they do not have a counterpart in any StaticPrintConfig derived classes,
    // therefore they need to be handled differently. As they have no counterpart in StaticPrintConfig, they are not being
    // initialized based on PrintConfigDef(), but to empty values (zeros, empty vectors, empty strings).
    //
    // "compatible_printers", "compatible_printers_condition", "inherits",
    // "sla_print_settings_id", "sla_material_settings_id", "printer_settings_id"
    // "printer_vendor", "printer_model", "printer_variant", "default_sla_print_profile", "default_sla_material_profile"

    this->sla_materials.default_preset().config.optptr("sla_material_settings_id", true);
    this->sla_materials.default_preset().compatible_printers_condition();
    this->sla_materials.default_preset().inherits();
    // Set all the nullable values to nils.
    this->sla_materials.default_preset().config.null_nullables();

    this->sla_prints.default_preset().config.optptr("sla_print_settings_id", true);
    this->sla_prints.default_preset().config.opt_string("output_filename_format", true) = "[input_filename_base][default_output_extension]";
    this->sla_prints.default_preset().compatible_printers_condition();
    this->sla_prints.default_preset().inherits();

    {
        Preset &printer = this->printers.default_preset(0);
        printer.printer_technology_ref() = ptSLA;
        Preset::normalize(printer.config);
        for (const char *key : { 
            "printer_settings_id", "printer_vendor", "printer_model", "printer_variant", "thumbnails",
            "default_sla_print_profile", "default_sla_material_profile"
            })
            printer.config.optptr(key, true);
        printer.inherits();
    }

    // Re-activate the default presets, so their "edited" preset copies will be updated with the additional configuration values above.
    this->sla_prints   .select_preset(0);
    this->sla_materials.select_preset(0);
    this->printers     .select_preset(0);
}

void PresetBundle::install_rename_hooks()
{
    auto hook = [this](Preset::Type type, const std::string &old_name, const std::string &new_name, PresetFileTransaction &transaction) {
        return this->stage_renamed_preset_references(type, old_name, new_name, transaction);
    };
    for (PresetCollection *collection : std::initializer_list<PresetCollection*>{ &sla_prints, &sla_materials, &printers })
        collection->set_rename_hook(hook);
}

namespace {
using ConfigEdit = std::function<bool(DynamicPrintConfig&)>;

// Every edit is idempotent: the same function is applied to a copy (to stage the file) and later to memory.
ConfigEdit replace_in_list_edit(const char *key, std::string old_name, std::string new_name)
{
    return [=](DynamicPrintConfig &config) {
        auto *option = config.option<ConfigOptionStrings>(key, false);
        bool changed = false;
        if (option != nullptr)
            for (std::string &value : option->values)
                if (value == old_name) { value = new_name; changed = true; }
        return changed;
    };
}

ConfigEdit replace_string_edit(const char *key, std::string old_name, std::string new_name)
{
    return [=](DynamicPrintConfig &config) {
        auto *option = config.option<ConfigOptionString>(key, false);
        if (option == nullptr || option->value != old_name) return false;
        option->value = new_name;
        return true;
    };
}

// Remove `name` from a compatibility list. An emptied list would mean "compatible with everything" in the
// legacy PrusaSlicer reading, so it is converted to the explicit None state used by the Tab compatibility
// modes (empty list + condition "false"), which keeps the preset out of every selection it was bound to.
ConfigEdit remove_from_list_edit(const char *list_key, const char *condition_key, std::string name)
{
    return [=](DynamicPrintConfig &config) {
        auto *option = config.option<ConfigOptionStrings>(list_key, false);
        if (option == nullptr)
            return false;
        const auto last = std::remove(option->values.begin(), option->values.end(), name);
        if (last == option->values.end())
            return false;
        option->values.erase(last, option->values.end());
        if (option->values.empty())
            config.option<ConfigOptionString>(condition_key, true)->value = "false";
        return true;
    };
}

struct DependencyRule {
    Preset::Type                target;
    PresetDependent::Kind       kind;
    ConfigEdit                  edit;
};

// References that must change when the preset `name` of `type` is deleted. `parent_inherits` is what the
// deleted preset itself inherits, handed down to its children.
std::vector<DependencyRule> deletion_rules(Preset::Type type, const std::string &name, const std::string &parent_inherits)
{
    using Kind = PresetDependent::Kind;
    std::vector<DependencyRule> rules;
    rules.push_back({ type, Kind::Inherits, replace_string_edit("inherits", name, parent_inherits) });
    switch (type) {
    case Preset::TYPE_PRINTER:
        rules.push_back({ Preset::TYPE_SLA_PRINT,    Kind::CompatiblePrinters, remove_from_list_edit("compatible_printers", "compatible_printers_condition", name) });
        rules.push_back({ Preset::TYPE_SLA_MATERIAL, Kind::CompatiblePrinters, remove_from_list_edit("compatible_printers", "compatible_printers_condition", name) });
        break;
    case Preset::TYPE_SLA_PRINT:
        rules.push_back({ Preset::TYPE_SLA_MATERIAL, Kind::CompatiblePrints, remove_from_list_edit("compatible_prints", "compatible_prints_condition", name) });
        rules.push_back({ Preset::TYPE_PRINTER,      Kind::DefaultSlaPrint,  replace_string_edit("default_sla_print_profile", name, std::string()) });
        break;
    case Preset::TYPE_SLA_MATERIAL:
        rules.push_back({ Preset::TYPE_PRINTER,      Kind::DefaultSlaMaterial, replace_string_edit("default_sla_material_profile", name, std::string()) });
        break;
    default:
        break;
    }
    return rules;
}
} // namespace

std::function<void()> PresetBundle::stage_renamed_preset_references(Preset::Type type, const std::string &old_name, const std::string &new_name, PresetFileTransaction &transaction)
{
    std::vector<std::function<void()>> updates;
    auto stage = [&](Preset::Type target, const ConfigEdit &edit) {
        updates.push_back(this->get_presets(target).stage_user_preset_migration(edit, transaction));
    };
    switch (type) {
    case Preset::TYPE_PRINTER:
        stage(Preset::TYPE_SLA_PRINT,    replace_in_list_edit("compatible_printers", old_name, new_name));
        stage(Preset::TYPE_SLA_MATERIAL, replace_in_list_edit("compatible_printers", old_name, new_name));
        break;
    case Preset::TYPE_SLA_PRINT:
        stage(Preset::TYPE_PRINTER,      replace_string_edit("default_sla_print_profile", old_name, new_name));
        stage(Preset::TYPE_SLA_MATERIAL, replace_in_list_edit("compatible_prints", old_name, new_name));
        break;
    case Preset::TYPE_SLA_MATERIAL:
        stage(Preset::TYPE_PRINTER,      replace_string_edit("default_sla_material_profile", old_name, new_name));
        break;
    default:
        break;
    }
    return [updates = std::move(updates)]() { for (const auto &update : updates) update(); };
}

std::vector<PresetDependent> PresetBundle::find_preset_dependents(Preset::Type type, const std::string &name) const
{
    std::vector<PresetDependent> dependents;
    const PresetCollection &collection = this->get_presets(type);
    const Preset *preset = collection.find_preset(name, false, false);
    if (preset == nullptr || preset->is_default || preset->is_system || preset->is_external)
        return dependents;
    for (const DependencyRule &rule : deletion_rules(type, name, preset->inherits()))
        for (const std::string &dependent : this->get_presets(rule.target).user_presets_affected_by(rule.edit))
            dependents.push_back({ rule.target, dependent, rule.kind });
    return dependents;
}

bool PresetBundle::delete_preset_with_dependencies(Preset::Type type, const std::string &name)
{
    PresetCollection &collection = this->get_presets(type);
    const Preset *preset = collection.find_preset(name, false, false);
    if (preset == nullptr)
        return false;
    const auto rules = deletion_rules(type, name, preset->inherits());
    // The first rule is always the same-collection "inherits" flattening; the rest touch other collections.
    ConfigEdit flatten_children = rules.front().edit;
    std::vector<const DependencyRule*> others;
    for (size_t i = 1; i < rules.size(); ++i)
        others.push_back(&rules[i]);
    auto stage_others = [&](PresetFileTransaction &transaction) {
        std::vector<std::function<void()>> updates;
        for (const DependencyRule *rule : others)
            updates.push_back(this->get_presets(rule->target).stage_user_preset_migration(rule->edit, transaction));
        return std::function<void()>([updates = std::move(updates)]() { for (const auto &update : updates) update(); });
    };
    return collection.delete_preset_transactional(name, flatten_children, stage_others);
}

namespace {
bool is_sla_preset_type(Preset::Type type)
{
    return type == Preset::TYPE_PRINTER || type == Preset::TYPE_SLA_PRINT || type == Preset::TYPE_SLA_MATERIAL;
}

const char *preset_kind_label(Preset::Type type)
{
    return type == Preset::TYPE_PRINTER ? "printer" : type == Preset::TYPE_SLA_MATERIAL ? "material" : "process";
}

const std::vector<std::string> &keys_of_type(Preset::Type type)
{
    return type == Preset::TYPE_PRINTER ? Preset::sla_printer_options() :
           type == Preset::TYPE_SLA_MATERIAL ? Preset::sla_material_options() : Preset::sla_print_options();
}

// Number of keys of `config` that belong to the option list of `type` and are not shared with the
// compatibility/inheritance bookkeeping every profile type has.
size_t type_key_matches(const DynamicPrintConfig &config, Preset::Type type)
{
    static const std::set<std::string> shared = { "inherits", "compatible_printers", "compatible_printers_condition",
                                                  "compatible_prints", "compatible_prints_condition",
                                                  "sla_print_settings_id", "sla_material_settings_id", "printer_settings_id" };
    size_t count = 0;
    for (const std::string &key : keys_of_type(type))
        if (config.has(key) && shared.count(key) == 0)
            ++count;
    return count;
}

std::string safe_preset_file_stem(std::string name)
{
    boost::algorithm::trim(name);
    if (boost::iends_with(name, ".ini"))
        name.erase(name.size() - 4);
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos)
        throw std::runtime_error("Invalid preset name: \"" + name + "\"");
    return name;
}
} // namespace

std::string PresetBundle::unique_preset_name(Preset::Type type, const std::string &name) const
{
    const PresetCollection &collection = this->get_presets(type);
    auto taken = [&](const std::string &candidate) {
        const Preset *existing = collection.find_preset(candidate, false, false);
        const std::string path = collection.path_from_name(candidate);
        return (existing != nullptr && existing->name == candidate) ||
               boost::filesystem::exists(path) || boost::filesystem::is_symlink(path);
    };
    if (!taken(name))
        return name;
    for (int n = 2; n < 10000; ++n) {
        const std::string candidate = name + " (" + std::to_string(n) + ")";
        if (!taken(candidate))
            return candidate;
    }
    throw std::runtime_error("Cannot find a free name for preset \"" + name + "\"");
}

void PresetBundle::export_preset_ini(Preset::Type type, const std::string &name, const std::string &path) const
{
    if (!is_sla_preset_type(type))
        throw std::runtime_error("Only printer, material and process presets can be exported");
    const Preset *preset = this->get_presets(type).find_preset(name, false, false);
    if (preset == nullptr || preset->is_default)
        throw std::runtime_error("There is no saved preset named \"" + name + "\" to export");
    DynamicPrintConfig config = preset->config;
    // Local project bookkeeping, not a property of the profile.
    config.erase("sla_print_settings_id");
    config.erase("sla_material_settings_id");
    config.erase("printer_settings_id");
    PresetFileTransaction transaction;
    transaction.stage_config(path, config); // temp file + rename; a failed export keeps the previous file
    transaction.commit();
}

PresetBundle::PresetImportResult PresetBundle::import_preset_ini(Preset::Type type, const std::string &path, const std::string &suggested_name)
{
    if (!is_sla_preset_type(type))
        throw std::runtime_error("Only printer, material and process presets can be imported");
    PresetCollection &collection = this->get_presets(type);

    DynamicPrintConfig loaded;
    try {
        loaded.load_from_ini(path, ForwardCompatibilitySubstitutionRule::Enable);
    } catch (const FffConfigurationException &) {
        throw std::runtime_error("This is not an SLA profile (printer technology is not SLA); FFF profiles are not supported");
    } catch (const std::exception &error) {
        throw std::runtime_error("Cannot read the profile file \"" + path + "\": " + error.what());
    }
    // Reject files of another kind before any value is grafted onto the defaults, because
    // apply_only() would silently drop the foreign keys and produce a plausible but wrong profile.
    const ConfigOption *technology = loaded.option("printer_technology");
    if (technology != nullptr && technology->getInt() != ptSLA)
        throw std::runtime_error("This is not an SLA profile (printer technology is not SLA); FFF profiles are not supported");
    const size_t own = type_key_matches(loaded, type);
    for (Preset::Type other : { Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL })
        if (other != type && type_key_matches(loaded, other) > own)
            throw std::runtime_error(std::string("This file looks like a ") + preset_kind_label(other) +
                                     " profile, but a " + preset_kind_label(type) + " profile was expected");
    if (own == 0)
        throw std::runtime_error(std::string("The file contains no ") + preset_kind_label(type) + " profile settings");
    if (type == Preset::TYPE_PRINTER && technology == nullptr)
        throw std::runtime_error("The printer profile does not declare printer_technology = SLA");

    DynamicPrintConfig config = collection.default_preset_for(loaded).config;
    config.apply_only(loaded, config.keys(), true);
    // A dangling parent would make the preset look like a modified copy of something that is not installed.
    if (const auto *inherits = config.option<ConfigOptionString>("inherits", false))
        if (!inherits->value.empty() && collection.find_preset(inherits->value, false, false) == nullptr)
            config.option<ConfigOptionString>("inherits")->value.clear();
    Preset::normalize(config);
    if (type == Preset::TYPE_PRINTER) {
        const std::string geometry_error = sla::printer_geometry_error(config);
        if (!geometry_error.empty())
            throw std::runtime_error("Invalid printer geometry in the imported profile: " + geometry_error);
    }

    const std::string requested = safe_preset_file_stem(suggested_name.empty() ?
        boost::filesystem::path(path).stem().string() : suggested_name);
    PresetImportResult result;
    result.name = this->unique_preset_name(type, requested);
    result.renamed = result.name != requested;
    const std::string target = collection.path_from_name(result.name);

    PresetFileTransaction transaction;
    transaction.stage_config(target, config, true);
    transaction.commit();
    collection.load_preset(target, result.name, std::move(config), false);
    return result;
}

PresetBundle::PresetBundle(const PresetBundle &rhs)
{
    *this = rhs;
}

PresetBundle& PresetBundle::operator=(const PresetBundle &rhs)
{
    sla_prints          = rhs.sla_prints;
    sla_materials       = rhs.sla_materials;
    printers            = rhs.printers;

    project_config      = rhs.project_config;
    vendors             = rhs.vendors;
    obsolete_presets    = rhs.obsolete_presets;

    // Adjust Preset::vendor pointers to point to the copied vendors map.
    sla_prints   .update_vendor_ptrs_after_copy(this->vendors);
    sla_materials.update_vendor_ptrs_after_copy(this->vendors);
    printers     .update_vendor_ptrs_after_copy(this->vendors);
    // The collections' hooks are never copied; make sure they point to this bundle.
    this->install_rename_hooks();

    return *this;
}

void PresetBundle::reset(bool delete_files)
{
    // Clear the existing presets, delete their respective files.
    this->vendors.clear();
    this->sla_prints   .reset(delete_files);
    this->sla_materials.reset(delete_files);
    this->printers     .reset(delete_files);
    this->printers     .select_preset(0);
    this->obsolete_presets.sla_prints.clear();
    this->obsolete_presets.sla_materials.clear();
    this->obsolete_presets.printers.clear();
}

void PresetBundle::setup_directories()
{
    boost::filesystem::path data_dir = boost::filesystem::path(Slic3r::data_dir());
    std::initializer_list<boost::filesystem::path> paths = { 
        data_dir,
		data_dir / "vendor",
        data_dir / "cache",
        data_dir / "cache" / "vendor",
        data_dir / "shapes",
#ifdef SLIC3R_PROFILE_USE_PRESETS_SUBDIR
        // Store the presets into a "presets" directory.
        data_dir / "presets", 
        data_dir / "presets" / "sla_print",  
        data_dir / "presets" / "sla_material", 
        data_dir / "presets" / "printer"
#else
        // Store the presets at the same location as the upstream Slic3r.
        // The legacy print/, filament/ and physical_printer/ folders of an old data directory are left alone
        // (never created here, never read, never deleted).
        data_dir / "sla_print", 
        data_dir / "sla_material", 
        data_dir / "printer"
#endif
    };
    for (const boost::filesystem::path &path : paths) {
		boost::filesystem::path subdir = path;
        subdir.make_preferred();
        if (! boost::filesystem::is_directory(subdir) && 
            ! boost::filesystem::create_directory(subdir))
            throw Slic3r::RuntimeError(std::string("Slic3r was unable to create its data directory at ") + subdir.string());
    }
}

// recursively copy all files and dirs in from_dir to to_dir
static void copy_dir(const boost::filesystem::path& from_dir, const boost::filesystem::path& to_dir)
{
    if(!boost::filesystem::is_directory(from_dir))
        return;
    // i assume to_dir.parent surely exists
    if (!boost::filesystem::is_directory(to_dir))
        boost::filesystem::create_directory(to_dir);
    for (auto& dir_entry : boost::filesystem::directory_iterator(from_dir)) {
        if (!boost::filesystem::is_directory(dir_entry.path())) {
            std::string em;
            CopyFileResult cfr = copy_file(dir_entry.path().string(), (to_dir / dir_entry.path().filename()).string(), em, false);
            if (cfr != SUCCESS) {
                BOOST_LOG_TRIVIAL(error) << "Error when copying files from " << from_dir << " to " << to_dir << ": " << em;
            }
        } else {
            copy_dir(dir_entry.path(), to_dir / dir_entry.path().filename());
        }
    }
}

// Import newer configuration from alternate PrusaSlicer configuration directory.
// AppConfig from the alternate location is already loaded.
// User profiles are being merged (old files are not being deleted),
// while old vendors and cache folders are being deleted before newer are copied.
void PresetBundle::import_newer_configs(const std::string& from)
{
    boost::filesystem::path data_dir = boost::filesystem::path(Slic3r::data_dir());
    // Clean-up vendors from the target directory, as the existing vendors will not be referenced
    // by the copied PrusaSlicer.ini
    try {
        boost::filesystem::remove_all(data_dir / "cache");
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Error deleting old cache " << (data_dir / "cache").string() << ": " << ex.what();
    }
    try {
        boost::filesystem::remove_all(data_dir / "vendor");
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Error deleting old vendors " << (data_dir / "vendor").string() << ": " << ex.what();
    }
    // list of searched paths based on current directory system in setup_directories()
    // do not copy cache and snapshots
    boost::filesystem::path from_data_dir = boost::filesystem::path(from);
    std::initializer_list<boost::filesystem::path> from_dirs= {
        from_data_dir / "cache",
        from_data_dir / "vendor",
        from_data_dir / "shapes",
#ifdef SLIC3R_PROFILE_USE_PRESETS_SUBDIR
        // Store the presets into a "presets" directory.
        data_dir / "presets" / "sla_print",
        data_dir / "presets" / "sla_material",
        data_dir / "presets" / "printer"
#else
        // Store the presets at the same location as the upstream Slic3r (the legacy print/filament/physical_printer
        // folders of the older data directory are not imported).
        from_data_dir / "sla_print",
        from_data_dir / "sla_material",
        from_data_dir / "printer"
#endif
    };
    // copy recursively all files
    for (const boost::filesystem::path& from_dir : from_dirs) {
        copy_dir(from_dir, data_dir / from_dir.filename());
    }
    // copy ArchiveRepositoryManifest.json file in main directory
    std::string em;
    CopyFileResult cfr = copy_file((from_data_dir / "ArchiveRepositoryManifest.json").string(), (data_dir / "ArchiveRepositoryManifest.json").string(), em, false);
    if (cfr != SUCCESS) {
        BOOST_LOG_TRIVIAL(error) << "Error when copying files from " << from_data_dir << " to " << data_dir << ": " << em;
    }
}

PresetsConfigSubstitutions PresetBundle::load_presets(AppConfig &config, ForwardCompatibilitySubstitutionRule substitution_rule, 
                                                      const PresetPreferences& preferred_selection/* = PresetPreferences()*/)
{
    // First load the vendor specific system presets.
    PresetsConfigSubstitutions substitutions;
    std::string errors_cummulative;
    std::tie(substitutions, errors_cummulative) = this->load_system_presets(substitution_rule);

    const std::string& dir_user_presets = data_dir()
#ifdef SLIC3R_PROFILE_USE_PRESETS_SUBDIR
        // Store the presets into a "presets" directory.
        + "/presets"
#else
        // Store the presets at the same location as the upstream Slic3r.
#endif
        ;

    try {
        this->sla_prints.load_presets(dir_user_presets, "sla_print", substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        this->sla_materials.load_presets(dir_user_presets, "sla_material", substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    try {
        this->printers.load_presets(dir_user_presets, "printer", substitutions, substitution_rule);
    } catch (const std::runtime_error &err) {
        errors_cummulative += err.what();
    }
    this->update_compatible(PresetSelectCompatibleType::Never);
    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);

    this->load_selections(config, preferred_selection);

    return substitutions;
}

// Load system presets into this PresetBundle.
// For each vendor, there will be a single PresetBundle loaded.
std::pair<PresetsConfigSubstitutions, std::string> PresetBundle::load_system_presets(ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSystemSilent)
        // Loading system presets, don't log substitutions.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    else if (compatibility_rule == ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem)
        // Loading system presets, throw on unknown option value.
        compatibility_rule = ForwardCompatibilitySubstitutionRule::Disable;

    // Here the vendor specific read only Config Bundles are stored.
    boost::filesystem::path     dir = (boost::filesystem::path(data_dir()) / "vendor").make_preferred();
    PresetsConfigSubstitutions  substitutions;
    std::string                 errors_cummulative;
    bool                        first = true;
    for (auto &dir_entry : boost::filesystem::directory_iterator(dir))
        if (Slic3r::is_ini_file(dir_entry)) {
            std::string name = dir_entry.path().filename().string();
            // Remove the .ini suffix.
            name.erase(name.size() - 4);
            try {
                // Load the config bundle, flatten it.
                if (first) {
                    // Reset this PresetBundle and load the first vendor config.
                    append(substitutions, this->load_configbundle(dir_entry.path().string(), PresetBundle::LoadSystem, compatibility_rule).first);
                    first = false;
                } else {
                    // Load the other vendor configs, merge them with this PresetBundle.
                    // Report duplicate profiles.
                    PresetBundle other;
                    append(substitutions, other.load_configbundle(dir_entry.path().string(), PresetBundle::LoadSystem, compatibility_rule).first);
                    std::vector<std::string> duplicates = this->merge_presets(std::move(other));
                    if (! duplicates.empty()) {
                        errors_cummulative += "Vendor configuration file " + name + " contains the following presets with names used by other vendors: ";
                        for (size_t i = 0; i < duplicates.size(); ++ i) {
                            if (i > 0)
                                errors_cummulative += ", ";
                            errors_cummulative += duplicates[i];
                        }
                    }
                }
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
            }
        }
    if (first) {
		// No config bundle loaded, reset.
		this->reset(false);
	}

	this->update_system_maps();
    return std::make_pair(std::move(substitutions), errors_cummulative);
}

// Merge one vendor's presets with the other vendor's presets, report duplicates.
std::vector<std::string> PresetBundle::merge_presets(PresetBundle &&other)
{
    this->vendors.insert(other.vendors.begin(), other.vendors.end());
    std::vector<std::string> duplicate_sla_prints    = this->sla_prints   .merge_presets(std::move(other.sla_prints),    this->vendors);
    std::vector<std::string> duplicate_sla_materials = this->sla_materials.merge_presets(std::move(other.sla_materials), this->vendors);
    std::vector<std::string> duplicate_printers      = this->printers     .merge_presets(std::move(other.printers),      this->vendors);
	append(this->obsolete_presets.sla_prints,    std::move(other.obsolete_presets.sla_prints));
    append(this->obsolete_presets.sla_materials, std::move(other.obsolete_presets.sla_materials));
	append(this->obsolete_presets.printers,      std::move(other.obsolete_presets.printers));
    append(duplicate_sla_prints, std::move(duplicate_sla_materials));
    append(duplicate_sla_prints, std::move(duplicate_printers));
    return duplicate_sla_prints;
}

void PresetBundle::update_system_maps()
{
    this->sla_prints   .update_map_system_profile_renamed();
    this->sla_materials.update_map_system_profile_renamed();
    this->printers     .update_map_system_profile_renamed();

    update_alias_maps();
}

void PresetBundle::update_alias_maps()
{
    this->sla_prints   .update_map_alias_to_profile_name();
    this->sla_materials.update_map_alias_to_profile_name();
}

static inline std::string remove_ini_suffix(const std::string &name)
{
    std::string out = name;
    if (boost::iends_with(out, ".ini"))
        out.erase(out.end() - 4, out.end());
    return out;
}

// Set the "enabled" flag for printer vendors, printer models and printer variants
// based on the user configuration.
// If the "vendor" section is missing, enable all models and variants of the particular vendor.
void PresetBundle::load_installed_printers(const AppConfig &config)
{
	this->update_system_maps();
    for (auto &preset : printers) {
        preset.set_visible_from_appconfig(config);
    }
}

const PresetCollection& PresetBundle::get_presets(Preset::Type type) const
{
    assert(type >= Preset::TYPE_SLA_PRINT && type <= Preset::TYPE_PRINTER);

    return  type == Preset::TYPE_SLA_PRINT      ? sla_prints    :
            type == Preset::TYPE_SLA_MATERIAL   ? sla_materials : printers;
}


PresetCollection& PresetBundle::get_presets(Preset::Type type)
{
    return const_cast<PresetCollection&>(const_cast<const PresetBundle*>(this)->get_presets(type));
}


const std::string& PresetBundle::get_preset_name_by_alias( const Preset::Type& preset_type, const std::string& alias)
{
    // there are not aliases for Printers profiles
    if (preset_type == Preset::TYPE_PRINTER || preset_type == Preset::TYPE_INVALID)
        return alias;

    const PresetCollection& presets = get_presets(preset_type);

    return presets.get_preset_name_by_alias(alias);
}

const std::string& PresetBundle::get_preset_name_by_alias_invisible(const Preset::Type& preset_type, const std::string& alias) const
{
    // there are not aliases for Printers profiles
    if (preset_type == Preset::TYPE_PRINTER || preset_type == Preset::TYPE_INVALID)
        return alias;

    const PresetCollection& presets = preset_type == Preset::TYPE_SLA_PRINT ? sla_prints : sla_materials;

    return presets.get_preset_name_by_alias_invisible(alias);
}

void PresetBundle::save_changes_for_preset(const std::string& new_name, Preset::Type type,
                                           const std::vector<std::string>& unselected_options)
{
    PresetCollection& presets = get_presets(type);

    // if we want to save just some from selected options
    if (!unselected_options.empty()) {
        // revert unselected options to the old values
        presets.get_edited_preset().config.apply_only(presets.get_selected_preset().config, unselected_options);
    }

    if (type == Preset::TYPE_PRINTER)
        copy_bed_model_and_texture_if_needed(presets.get_edited_preset().config);

    // Save the preset into Slic3r::data_dir / presets / section_name / preset_name.ini
    presets.save_current_preset(new_name);
    // Mark the print & material enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are no more compatible.
    update_compatible(PresetSelectCompatibleType::Never);
}

bool PresetBundle::transfer_and_save(Preset::Type type, const std::string& preset_from_name, const std::string& preset_to_name,
                                     const std::string& preset_new_name, const std::vector<std::string>& options)
{
    if (options.empty())
        return false;

    PresetCollection& presets = get_presets(type);

    const Preset* preset_to   = presets.find_preset(preset_to_name, false, false);
    if (!preset_to)
        return false;

    // Find the preset with a new_name or create a new one,
    // initialize it with the preset_to config.
    Preset& preset = presets.get_preset_with_name(preset_new_name, preset_to);
    if (preset.is_default || preset.is_external || preset.is_system)
        // Cannot overwrite the default preset.
        return false;

    // Apply options from the preset_from_name.
    const Preset* preset_from = presets.find_preset(preset_from_name, false, false);
    if (!preset_from)
        return false;
    preset.config.apply_only(preset_from->config, options);

    // Store new_name preset to disk.
    preset.save();

    // Mark the SLA print & material enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are no more compatible.
    update_compatible(PresetSelectCompatibleType::Never);

    if (type == Preset::TYPE_PRINTER)
        copy_bed_model_and_texture_if_needed(preset.config);

    return true;
}

void PresetBundle::load_installed_sla_materials(AppConfig &config)
{
    if (! config.has_section(AppConfig::SECTION_MATERIALS)) {
        std::unordered_set<const Preset*> comp_sla_materials;
		// Compatibility with the PrusaSlicer 2.1.1 and older, where the SLA material profiles were not installable yet.
		// Find all SLA material profiles, which are compatible with installed printers, and act as if these SLA material profiles
		// were installed.
        for (const Preset &printer : printers)
            if (printer.is_visible && printer.printer_technology() == ptSLA) {
				const PresetWithVendorProfile printer_with_vendor_profile = printers.get_preset_with_vendor_profile(printer);
				for (const Preset &material : sla_materials)
					if (material.is_system && is_compatible_with_printer(sla_materials.get_preset_with_vendor_profile(material), printer_with_vendor_profile))
						comp_sla_materials.insert(&material);
			}
		// and mark these SLA materials as installed, therefore this code will not be executed at the next start of the application.
		for (const auto &material: comp_sla_materials)
            config.set(AppConfig::SECTION_MATERIALS, material->name, "1");
    }

    for (auto &preset : sla_materials)
        preset.set_visible_from_appconfig(config);
}

// Load selections (current print, current material, current printer) from config.ini
// This is done on application start up or after updates are applied.
// The legacy keys of the removed FFF layer in [presets] (print, filament, filament_N, physical_printer) are ignored
// (they are dropped from the file the next time the application config is saved).
void PresetBundle::load_selections(AppConfig &config, const PresetPreferences& preferred_selection/* = PresetPreferences()*/)
{
	// Update visibility of presets based on application vendor / model / variant configuration.
	this->load_installed_printers(config);

    // Update visibility of sla material presets
    this->load_installed_sla_materials(config);

    // Parse the initial print / material / printer profile names.
    std::string initial_sla_print_profile_name    = remove_ini_suffix(config.get("presets", "sla_print"));
    std::string initial_sla_material_profile_name = remove_ini_suffix(config.get("presets", "sla_material"));
	std::string initial_printer_profile_name      = remove_ini_suffix(config.get("presets", "printer"));

    // Activate print / material / printer profiles from either the config,
    // or from the preferred_model_id suggestion passed in by ConfigWizard.
    // If the printer profile enumerated by the config are not visible, select an alternate preset.
    // Do not select alternate profiles for the print / material profiles as those presets
    // will be selected by the following call of this->update_compatible(PresetSelectCompatibleType::Always).

    const Preset *initial_printer = printers.find_preset(initial_printer_profile_name);
    // If executed due to a Config Wizard update, preferred_printer contains the first newly installed printer, otherwise nullptr.
    const Preset *preferred_printer = printers.find_system_preset_by_model_and_variant(preferred_selection.printer_model_id, preferred_selection.printer_variant);
    printers.select_preset_by_name(preferred_printer ? preferred_printer->name : initial_printer_profile_name, true);

    // Selects the profile, leaves it to -1 if the initial profile name is empty or if it was not found.
	sla_prints.select_preset_by_name_strict(initial_sla_print_profile_name);
    sla_materials.select_preset_by_name_strict(initial_sla_material_profile_name);

    // Update visibility of presets based on their compatibility with the active printer.
    // Always try to select a compatible print and material preset to the current printer preset,
    // as the application may have been closed with an active "external" preset, which does not
    // exist.
    this->update_compatible(PresetSelectCompatibleType::Always);

    if (initial_printer != nullptr && (preferred_printer == nullptr || initial_printer == preferred_printer)) {
        // Don't run the following code, as we want to activate default SLA material profiles when installing and selecting a new printer.
        // Only run this code if just a SLA material was installed by Config Wizard for an active Printer.
        if (! preferred_selection.sla_material.empty()) {
            const std::string& preferred_preset_name = get_preset_name_by_alias(Preset::Type::TYPE_SLA_MATERIAL, preferred_selection.sla_material);
            if (auto it = sla_materials.find_preset_internal(preferred_preset_name);
                it != sla_materials.end() && it->is_visible && it->is_compatible)
                sla_materials.select_preset_by_name_strict(preferred_preset_name);
        }
    }

}

// Export selections (current print, current material, current printer) into config.ini
void PresetBundle::export_selections(AppConfig &config)
{
    config.clear_section("presets");
    config.set("presets", "sla_print",    sla_prints.get_selected_preset_name());
    config.set("presets", "sla_material", sla_materials.get_selected_preset_name());
    config.set("presets", "printer",      printers.get_selected_preset_name());
}

DynamicPrintConfig PresetBundle::full_config() const
{
    return this->full_sla_config();
}

DynamicPrintConfig PresetBundle::full_config_secure() const
{
    // The legacy print host / API key options are no longer defined, so there is nothing to strip.
    return this->full_config();
}

DynamicPrintConfig PresetBundle::full_sla_config() const
{    
    DynamicPrintConfig out;
    out.apply(SLAFullPrintConfig::defaults());
    out.apply(this->sla_prints.get_edited_preset().config);
    out.apply(this->sla_materials.get_edited_preset().config);
    out.apply(this->printers.get_edited_preset().config);

    // Collect the "compatible_printers_condition" and "inherits" values over all presets (sla_prints, sla_materials, printers) into a single vector.
    std::vector<std::string> compatible_printers_condition;
	std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    compatible_printers_condition.emplace_back(this->sla_prints.get_edited_preset().compatible_printers_condition());
	inherits					 .emplace_back(this->sla_prints.get_edited_preset().inherits());
    compatible_printers_condition.emplace_back(this->sla_materials.get_edited_preset().compatible_printers_condition());
	compatible_prints_condition  .emplace_back(this->sla_materials.get_edited_preset().compatible_prints_condition());
    inherits                     .emplace_back(this->sla_materials.get_edited_preset().inherits());
    inherits                     .emplace_back(this->printers.get_edited_preset().inherits());

    // These two value types clash between the print and material profiles. They should be renamed.
    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");
    
    // External presets have contextual GUI labels to keep imported projects
    // distinct. Persist their source identity, so Save As/reopen does not nest
    // project filenames into printer/material/process IDs. Library presets use
    // their actual selected name, including after Save As from an import.
    out.option<ConfigOptionString >("sla_print_settings_id",    true)->value = sla_prints.get_selected_preset_source_name("sla_print_settings_id");
    out.option<ConfigOptionString >("sla_material_settings_id", true)->value = sla_materials.get_selected_preset_source_name("sla_material_settings_id");
    out.option<ConfigOptionString >("printer_settings_id",      true)->value = printers.get_selected_preset_source_name("printer_settings_id");

    // Serialize the collected "compatible_printers_condition" and "inherits" fields.
    // The vector will not be stored if all fields are empty strings.
    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (! v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty)
            out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_printers_condition_cummulative");
    add_if_some_non_empty(std::move(compatible_prints_condition),   "compatible_prints_condition_cummulative");
    add_if_some_non_empty(std::move(inherits),                      "inherits_cummulative");

    out.set_key_value("sla_preset_metadata",new ConfigOptionString(sla::serialize_sla_preset_metadata(
        printers.get_edited_preset().config,sla_materials.get_edited_preset().config,sla_prints.get_edited_preset().config)));
	out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptSLA;
	return out;
}

// Load an external config file containing the SLA print, material and printer presets.
// A configuration with "printer_technology = FFF" is rejected (FffConfigurationException message).
// If the file is loaded successfully, its print / material / printer profiles will be activated.
ConfigSubstitutions PresetBundle::load_config_file(const std::string &path, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    m_skipped_fff_sections = 0;
    // 1) Try to load the config file into a boost property tree.
    boost::property_tree::ptree tree;
    try {
        boost::nowide::ifstream ifs(path);
        boost::property_tree::read_ini(ifs, tree);
    } catch (const std::ifstream::failure &err) {
        throw Slic3r::RuntimeError(std::string("The Config Bundle cannot be loaded: ") + path + "\n\tReason: " + err.what());
    } catch (const boost::property_tree::file_parser_error &err) {
        throw Slic3r::RuntimeError(format("Failed loading the Config Bundle \"%1%\": %2% at line %3%",
        	err.filename(), err.message(), err.line()));
    } catch (const std::runtime_error &err) {
        throw Slic3r::RuntimeError(std::string("Failed loading the preset file: ") + path + "\n\tReason: " + err.what());
    }

    // 2) Continue based on the type of the configuration file.
    ConfigFileType config_file_type = guess_config_file_type(tree);
    ConfigSubstitutions config_substitutions;
    try {
        switch (config_file_type) {
        case CONFIG_FILE_TYPE_UNKNOWN:
            throw Slic3r::RuntimeError(std::string("Unknown configuration file type: ") + path);   
        case CONFIG_FILE_TYPE_APP_CONFIG:
            throw Slic3r::RuntimeError(std::string("Invalid configuration file: ") + path + ". This is an application config file.");
    	case CONFIG_FILE_TYPE_CONFIG:
    	{
    		// Initialize a config from the SLA defaults. An FFF config throws FffConfigurationException here
    		// (a ConfigurationError, reported as "Invalid configuration file ...: FFF/FDM configurations are not supported").
    		DynamicPrintConfig config;
    		if (tree.find("printer_technology") == tree.not_found() &&
    		    (tree.find("nozzle_diameter") != tree.not_found() || tree.find("filament_diameter") != tree.not_found()))
    		    // A pre-2.0 FFF configuration without the technology key.
    		    throw FffConfigurationException();
    		config.apply(static_cast<const ConfigBase&>(SLAFullPrintConfig::defaults()));
            config_substitutions = config.load(tree, compatibility_rule);
    		Preset::normalize(config);
    		load_config_file_config(path, true, std::move(config));
            return config_substitutions;
        }
        case CONFIG_FILE_TYPE_CONFIG_BUNDLE:
            return load_config_file_config_bundle(path, tree, compatibility_rule);
        }
    } catch (const ConfigurationError &e) {
        throw Slic3r::RuntimeError(format("Invalid configuration file %1%: %2%", path, e.what()));
    }

    // This shall never happen. Suppres compiler warnings.
    assert(false);
    return ConfigSubstitutions{};
}

// Load a config file from a boost property_tree. This is a private method called from load_config_file.
// is_external == false on if called from ConfigWizard
void PresetBundle::load_config_file_config(const std::string &name_or_path, bool is_external, DynamicPrintConfig &&config)
{
    PrinterTechnology printer_technology = Preset::printer_technology(config);

    tmp_installed_presets.clear();

    // The "compatible_printers" field should not have been exported into a config.ini anyway, 
    // but some of the alpha versions of Slic3r did.
    {
        ConfigOption *opt_compatible = config.optptr("compatible_printers");
        if (opt_compatible != nullptr) {
            assert(opt_compatible->type() == coStrings);
            if (opt_compatible->type() == coStrings)
                static_cast<ConfigOptionStrings*>(opt_compatible)->values.clear();
        }
    }

    // 1 SLA material
    const size_t num_materials = 1;
    // Make a copy of the "compatible_printers_condition_cummulative" and "inherits_cummulative" vectors, which 
    // accumulate values over all presets (print, materials, printers).
    // These values will be distributed into their particular presets when loading.
    std::vector<std::string> compatible_printers_condition_values   = std::move(config.option<ConfigOptionStrings>("compatible_printers_condition_cummulative", true)->values);
    std::vector<std::string> compatible_prints_condition_values     = std::move(config.option<ConfigOptionStrings>("compatible_prints_condition_cummulative",   true)->values);
    std::vector<std::string> inherits_values                        = std::move(config.option<ConfigOptionStrings>("inherits_cummulative", true)->values);
    std::string &compatible_printers_condition  = Preset::compatible_printers_condition(config);
    std::string &compatible_prints_condition    = Preset::compatible_prints_condition(config);
    std::string &inherits                       = Preset::inherits(config);
    compatible_printers_condition_values.resize(num_materials + 2, std::string());
    compatible_prints_condition_values.resize(num_materials, std::string());
    inherits_values.resize(num_materials + 2, std::string());
	if (printer_technology == ptSLA) {
		config.option<ConfigOptionString>("default_sla_print_profile", true);
		config.option<ConfigOptionString>("default_sla_material_profile", true);
	}

    // 1) Create a name from the file name.
    // Keep the suffix (.ini, .amf, .3mf etc) to differentiate it from the normal profiles.
    std::string name = is_external ? boost::filesystem::path(name_or_path).filename().string() : name_or_path;

    // 2) If the loading succeeded, split and load the config into SLA print / material / printer settings.
    // First load the print and printer presets.

    std::set<std::string>& tmp_installed_presets_ref = tmp_installed_presets;
	auto load_preset = 
		[&config, &inherits, &inherits_values, 
         &compatible_printers_condition, &compatible_printers_condition_values, 
         &compatible_prints_condition, &compatible_prints_condition_values, 
         is_external, &name, &name_or_path, &tmp_installed_presets_ref]
		(PresetCollection &presets, size_t idx, const std::string &key) {
		// Split the "compatible_printers_condition" and "inherits" values one by one from a single vector to the print & printer profiles.
		inherits = inherits_values[idx];
		compatible_printers_condition = compatible_printers_condition_values[idx];
        if (idx > 0 && idx - 1 < compatible_prints_condition_values.size())
            compatible_prints_condition = compatible_prints_condition_values[idx - 1];
        if (Preset::printer_technology(config)==ptSLA) {
            const char *role=idx==0 ? "process" : idx==1 ? "material" : "printer";
            sla::apply_sla_preset_metadata(config,config,role);
        }
		if (is_external) {
			ExternalPreset ext_preset = presets.load_external_preset(name_or_path, name, config.opt_string(key, true), config);
			if (ext_preset.is_installed)
				tmp_installed_presets_ref.emplace(ext_preset.preset->name);
		}
		else
			presets.load_preset(presets.path_from_name(name), name, config).save();
	};

    switch (Preset::printer_technology(config)) {
    case ptSLA:
        load_preset(this->sla_prints,    0, "sla_print_settings_id");
        load_preset(this->sla_materials, 1, "sla_material_settings_id");
        load_preset(this->printers,      2, "printer_settings_id");
        break;
    default:
        break;
    }

	this->update_compatible(PresetSelectCompatibleType::Never);

    update_alias_maps();
}

// Load the active configuration of a config bundle from a boost property_tree. This is a private method called from load_config_file.
ConfigSubstitutions PresetBundle::load_config_file_config_bundle(
    const std::string &path, const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    // 1) Load the config bundle into a temp data.
    PresetBundle tmp_bundle;
    // Load the config bundle, but don't save the loaded presets to user profile directory, as only the presets marked as active in the loaded preset bundle
    // will be loaded into the master PresetBundle and activated.
    auto [presets_substitutions, presets_imported] = tmp_bundle.load_configbundle(path, {}, compatibility_rule);
    if (presets_imported == 0 && tmp_bundle.skipped_fff_sections() > 0)
        throw FffConfigurationException();
    this->m_skipped_fff_sections = tmp_bundle.skipped_fff_sections();

    std::string bundle_name = std::string(" - ") + boost::filesystem::path(path).filename().string();

    // 2) Extract active configs from the config bundle, copy them and activate them in this bundle.
    ConfigSubstitutions config_substitutions;
    auto load_one = [&path, &bundle_name, &presets_substitutions = presets_substitutions, &config_substitutions](
            PresetCollection &collection_dst, PresetCollection &collection_src, const std::string &preset_name_src, bool activate) -> std::string {
        // If there are substitutions reported for this preset, move them to config_substitutions.
        if (auto it = std::find_if(presets_substitutions.begin(), presets_substitutions.end(), [&preset_name_src](const PresetConfigSubstitutions& subs){ return subs.preset_name == preset_name_src; });
            it != presets_substitutions.end() && ! it->substitutions.empty())
            append(config_substitutions, std::move(it->substitutions));
        Preset *preset_src = collection_src.find_preset(preset_name_src, false);
        Preset *preset_dst = collection_dst.find_preset(preset_name_src, false);
        assert(preset_src != nullptr);
        std::string preset_name_dst;
        if (preset_dst != nullptr && preset_dst->is_default) {
            // No need to copy a default preset, it always exists in collection_dst.
            if (activate)
                collection_dst.select_preset(0);
            return preset_name_src;
        } else if (preset_dst != nullptr && preset_src->config == preset_dst->config) {
            // Don't save as the config exists in the current bundle and its content is the same.
            return preset_name_src;
        } else {
            // Generate a new unique name.
            preset_name_dst = preset_name_src + bundle_name;
            Preset *preset_dup = nullptr;
            for (size_t i = 1; (preset_dup = collection_dst.find_preset(preset_name_dst, false)) != nullptr; ++ i) {
                if (preset_src->config == preset_dup->config)
                    // The preset has been already copied into collection_dst.
                    return preset_name_dst;
                // Try to generate another name.
                char buf[64];
                sprintf(buf, " (%d)", (int)i);
                preset_name_dst = preset_name_src + buf + bundle_name;
            }
        }
        assert(! preset_name_dst.empty());
        // Save preset_src->config into collection_dst under preset_name_dst.
        // The "compatible_printers" field should not have been exported into a config.ini anyway, 
        // but some of the alpha versions of Slic3r did.
        ConfigOption *opt_compatible = preset_src->config.optptr("compatible_printers");
        if (opt_compatible != nullptr) {
            assert(opt_compatible->type() == coStrings);
            if (opt_compatible->type() == coStrings)
                static_cast<ConfigOptionStrings*>(opt_compatible)->values.clear();
        }
        collection_dst.load_preset(path, preset_name_dst, std::move(preset_src->config), activate).is_external = true;
        return preset_name_dst;
    };
    load_one(this->sla_prints,    tmp_bundle.sla_prints,    tmp_bundle.sla_prints   .get_selected_preset_name(), true);
    load_one(this->sla_materials, tmp_bundle.sla_materials, tmp_bundle.sla_materials.get_selected_preset_name(), true);
    load_one(this->printers,      tmp_bundle.printers,      tmp_bundle.printers     .get_selected_preset_name(), true);

    this->update_compatible(PresetSelectCompatibleType::Never);
    update_alias_maps();

    sort_remove_duplicates(config_substitutions);
    return config_substitutions;
}

// Process the Config Bundle loaded as a Boost property tree.
// For each SLA print, material and printer preset (group defined by group_name), apply the inherited presets.
// The presets starting with '*' are considered non-terminal and they are
// removed through the flattening process by this function.
// This function will never fail, but it will produce error messages through boost::log.
// system_profiles will not be flattened, and they will be kept inside the "inherits" field
static void flatten_configbundle_hierarchy(boost::property_tree::ptree &tree, const std::string &group_name, const std::vector<std::string> &system_profiles)
{
    namespace pt = boost::property_tree;

    // 1) For the group given by group_name, initialize the presets.
    struct Prst {
        Prst(const std::string &name, pt::ptree *node) : name(name), node(node) {}
        // Name of this preset. If the name starts with '*', it is an intermediate preset,
        // which will not make it into the result.
        const std::string           name;
        // Link to the source boost property tree node, owned by tree.
        pt::ptree                  *node;
        // Link to the presets, from which this preset inherits.
        std::vector<Prst*>          inherits;
        // Link to the presets, for which this preset is a direct parent.
        std::vector<Prst*>          parent_of;
        // When running the Kahn's Topological sorting algorithm, this counter is decreased from inherits.size() to zero.
        // A cycle is indicated, if the number does not drop to zero after the Kahn's algorithm finishes.
        size_t                      num_incoming_edges_left = 0;
        // Sorting by the name, to be used when inserted into std::set.
        bool operator==(const Prst &rhs) const { return this->name == rhs.name; }
        bool operator< (const Prst &rhs) const { return this->name < rhs.name; }
    };
    // Find the presets, store them into a std::map, addressed by their names.
    std::set<Prst> presets;
    std::string group_name_preset = group_name + ":";
    for (auto &section : tree)
        if (boost::starts_with(section.first, group_name_preset) && section.first.size() > group_name_preset.size())
            presets.emplace(section.first.substr(group_name_preset.size()), &section.second);
    // Fill in the "inherits" and "parent_of" members, report invalid inheritance fields.
    for (const Prst &prst : presets) {
        // Parse the list of comma separated values, possibly enclosed in quotes.
        std::vector<std::string> inherits_names;
        std::vector<std::string> inherits_system;
        if (Slic3r::unescape_strings_cstyle(prst.node->get<std::string>("inherits", ""), inherits_names)) {
            // Resolve the inheritance by name.
            std::vector<Prst*> &inherits_nodes = const_cast<Prst&>(prst).inherits;
            for (const std::string &node_name : inherits_names) {
                auto it_system = std::lower_bound(system_profiles.begin(), system_profiles.end(), node_name);
                if (it_system != system_profiles.end() && *it_system == node_name) {
                    // Loading a user config budnle, this preset is derived from a system profile.
                    inherits_system.emplace_back(node_name);
                } else {
                    auto it = presets.find(Prst(node_name, nullptr));
                    if (it == presets.end())
                        BOOST_LOG_TRIVIAL(error) << "flatten_configbundle_hierarchy: The preset " << prst.name << " inherits an unknown preset \"" << node_name << "\"";
                    else {
                        inherits_nodes.emplace_back(const_cast<Prst*>(&(*it)));
                        inherits_nodes.back()->parent_of.emplace_back(const_cast<Prst*>(&prst));
                    }
                }
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << "flatten_configbundle_hierarchy: The preset " << prst.name << " has an invalid \"inherits\" field";
        }
        // Remove the "inherits" key, it has no meaning outside of the config bundle.
        const_cast<pt::ptree*>(prst.node)->erase("inherits");
        if (! inherits_system.empty()) {
            // Loaded a user config bundle, where a profile inherits a system profile.
			// User profile should be derived from a single system profile only.
			assert(inherits_system.size() == 1);
			if (inherits_system.size() > 1)
				BOOST_LOG_TRIVIAL(error) << "flatten_configbundle_hierarchy: The preset " << prst.name << " inherits from more than single system preset";
			prst.node->put("inherits", Slic3r::escape_string_cstyle(inherits_system.front()));
        }
    }

    // 2) Create a linear ordering for the directed acyclic graph of preset inheritance.
    // https://en.wikipedia.org/wiki/Topological_sorting
    // Kahn's algorithm.
    std::vector<Prst*> sorted;
    {
        // Initialize S with the set of all nodes with no incoming edge.
        std::deque<Prst*> S;
        for (const Prst &prst : presets)
            if (prst.inherits.empty())
                S.emplace_back(const_cast<Prst*>(&prst));
            else
                const_cast<Prst*>(&prst)->num_incoming_edges_left = prst.inherits.size();
        while (! S.empty()) {
            Prst *n = S.front();
            S.pop_front();
            sorted.emplace_back(n);
            for (Prst *m : n->parent_of) {
                assert(m->num_incoming_edges_left > 0);
                if (-- m->num_incoming_edges_left == 0) {
                    // We have visited all parents of m.
                    S.emplace_back(m);
                }
            }
        }
        if (sorted.size() < presets.size()) {
            for (const Prst &prst : presets)
                if (prst.num_incoming_edges_left)
                    BOOST_LOG_TRIVIAL(error) << "flatten_configbundle_hierarchy: The preset " << prst.name << " has cyclic dependencies";
        }
    }

    // Apply the dependencies in their topological ordering.
    for (Prst *prst : sorted) {
        // Merge the preset nodes in their order of application.
        // Iterate in a reverse order, so the last change will be placed first in merged.
        for (auto it_inherits = prst->inherits.rbegin(); it_inherits != prst->inherits.rend(); ++ it_inherits)
            for (auto it = (*it_inherits)->node->begin(); it != (*it_inherits)->node->end(); ++ it)
				if (it->first == "renamed_from") {
            		// Don't inherit "renamed_from" flag, it does not make sense. The "renamed_from" flag only makes sense for a concrete preset.
            		if (boost::starts_with((*it_inherits)->name, "*"))
			            BOOST_LOG_TRIVIAL(error) << boost::format("Nonpublic intermediate preset %1% contains a \"renamed_from\" field, which is ignored") % (*it_inherits)->name;
                } else {
                    if (prst->node->find(it->first) == prst->node->not_found()) {
                        // The child does not have this. Propagate value from parent.
                        prst->node->add_child(it->first, it->second);
                    } else {
                        // Child redefines this. Nothing needs to be done except for one case:
                        // custom parameter JSONs shall be merged, not replaced.
                        if (boost::starts_with(it->first, "custom_parameters_")) {
                            auto&           child_node     = prst->node->find(it->first)->second;
                            const auto&     parent_node    = it->second;
                            const auto&     child_value    = child_node.get_value<std::string>();
                            const auto&     parent_value   = parent_node.get_value<std::string>();
                            child_node.put_value(merge_json(parent_value, child_value));
                        }
                    }
                }
    }

    // Remove the "internal" presets from the ptree. These presets are marked with '*'.
    group_name_preset += '*';
    for (auto it_section = tree.begin(); it_section != tree.end(); ) {
        if (boost::starts_with(it_section->first, group_name_preset) && it_section->first.size() > group_name_preset.size())
            // Remove the "internal" preset from the ptree.
            it_section = tree.erase(it_section);
        else
            // Keep the preset.
            ++ it_section;
    }
}

// preset_bundle is set when loading user config bundles, which must not overwrite the system profiles.
static void flatten_configbundle_hierarchy(boost::property_tree::ptree &tree, const PresetBundle *preset_bundle)
{
    flatten_configbundle_hierarchy(tree, "sla_print",       preset_bundle ? preset_bundle->sla_prints.system_preset_names()    : std::vector<std::string>());
    flatten_configbundle_hierarchy(tree, "sla_material",    preset_bundle ? preset_bundle->sla_materials.system_preset_names() : std::vector<std::string>());
    flatten_configbundle_hierarchy(tree, "printer",         preset_bundle ? preset_bundle->printers.system_preset_names()      : std::vector<std::string>());
}

// Load a config bundle file, into presets and store the loaded presets into separate files
// of the local configuration directory.
std::pair<PresetsConfigSubstitutions, size_t> PresetBundle::load_configbundle(
    const std::string &path, LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule)
{
    // Enable substitutions for user config bundle, throw an exception when loading a system profile.
    ConfigSubstitutionContext  substitution_context { compatibility_rule };
    PresetsConfigSubstitutions substitutions;
    m_skipped_fff_sections = 0;

    if (flags.has(LoadConfigBundleAttribute::ResetUserProfile) || flags.has(LoadConfigBundleAttribute::LoadSystem))
        // Reset this bundle, delete user profile files if SaveImported.
        this->reset(flags.has(LoadConfigBundleAttribute::SaveImported));

    // 1) Read the complete config file into a boost::property_tree.
    namespace pt = boost::property_tree;
    pt::ptree tree;
    {
        boost::nowide::ifstream ifs(path);
        try {
            pt::read_ini(ifs, tree);
        } catch (const boost::property_tree::ini_parser::ini_parser_error &err) {
            throw Slic3r::RuntimeError(format("Failed loading config bundle \"%1%\"\nError: \"%2%\" at line %3%", path, err.message(), err.line()).c_str());
        }
    }

    const VendorProfile *vendor_profile = nullptr;
    if (flags.has(LoadConfigBundleAttribute::LoadSystem) || flags.has(LoadConfigBundleAttribute::LoadVendorOnly)) {
        VendorProfile vp = VendorProfile::from_ini(tree, path);
        if (vp.models.size() == 0) {
            // E.g. a legacy FFF-only vendor bundle: nothing for an SLA/MSLA-only application, not an error.
            BOOST_LOG_TRIVIAL(info) << boost::format("Vendor bundle: `%1%`: No SLA printer model defined, the vendor is ignored.") % path;
            return std::make_pair(PresetsConfigSubstitutions{}, 0);
        } else if (vp.num_variants() == 0) {
            BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: No printer variant defined") % path;
            return std::make_pair(PresetsConfigSubstitutions{}, 0);
        }
        vendor_profile = &this->vendors.insert({vp.id, vp}).first->second;
    }

    if (flags.has(LoadConfigBundleAttribute::LoadVendorOnly))
        return std::make_pair(PresetsConfigSubstitutions{}, 0);

    // 1.5) Flatten the config bundle by applying the inheritance rules. Internal profiles (with names starting with '*') are removed.
    // If loading a user config bundle, do not flatten with the system profiles, but keep the "inherits" flag intact.
    flatten_configbundle_hierarchy(tree, flags.has(LoadConfigBundleAttribute::LoadSystem) ? nullptr : this);
    // [print:] and [filament:] groups are not flattened: they are skipped below.

    // 2) Parse the property_tree, extract the active preset names and the profiles, save them into local config files.
    // Parse the obsolete preset names, to be deleted when upgrading from the old configuration structure.
    std::string              active_sla_print;
    std::string              active_sla_material;
    std::string              active_printer;
    size_t                   presets_loaded = 0;

    for (const auto &section : tree) {
        PresetCollection         *presets = nullptr;
        std::string               preset_name;
        if (boost::starts_with(section.first, "print:") || boost::starts_with(section.first, "filament:") ||
            boost::starts_with(section.first, "physical_printer:")) {
            // FFF print / filament profiles and physical printers (print hosts) are not supported: skip the whole section.
            // Intermediate "*name*" sections only feed inheritance and are not counted.
            if (section.first.find(":*") == std::string::npos)
                ++ m_skipped_fff_sections;
            continue;
        } else if (boost::starts_with(section.first, "sla_print:")) {
            presets = &this->sla_prints;
            preset_name = section.first.substr(10);
        } else if (boost::starts_with(section.first, "sla_material:")) {
            presets = &this->sla_materials;
            preset_name = section.first.substr(13);
        } else if (boost::starts_with(section.first, "printer:")) {
            presets = &this->printers;
            preset_name = section.first.substr(8);
        } else if (section.first == "presets") {
            // Load the names of the active presets.
            for (auto &kvp : section.second) {
                if (kvp.first == "sla_print") {
                    active_sla_print = kvp.second.data();
                } else if (kvp.first == "sla_material") {
                    active_sla_material = kvp.second.data();
                } else if (kvp.first == "printer") {
                    active_printer = kvp.second.data();
                }
            }
        } else if (section.first == "obsolete_presets") {
            // Parse the names of obsolete presets. These presets will be deleted from user's
            // profile directory on installation of this vendor preset.
            for (auto &kvp : section.second) {
                std::vector<std::string> *dst = nullptr;
                // The "print" and "filament" lists of old vendor bundles refer to FFF profiles and are ignored.
                if (kvp.first == "sla_print")
                    dst = &this->obsolete_presets.sla_prints;
                else if (kvp.first == "sla_material")
                    dst = &this->obsolete_presets.sla_materials;
                else if (kvp.first == "printer")
                    dst = &this->obsolete_presets.printers;
                if (dst)
                    unescape_strings_cstyle(kvp.second.data(), *dst);
            }
        } else
            // Ignore an unknown section.
            continue;
        if (presets != nullptr) {
            // Load the SLA print, material or printer preset.
            const DynamicPrintConfig *default_config = nullptr;
            DynamicPrintConfig        config;
            std::string 			  alias_name;
            std::vector<std::string>  renamed_from;
            bool skip_fff = false;
            try {
                auto parse_config_section = [&section, &alias_name, &renamed_from, &substitution_context, &path](DynamicPrintConfig &config) {
                    substitution_context.substitutions.clear();
                    for (auto &kvp : section.second) {
                    	if (kvp.first == "alias")
                    		alias_name = kvp.second.data();
                    	else if (kvp.first == "renamed_from") {
                    		if (! unescape_strings_cstyle(kvp.second.data(), renamed_from)) {
    			                BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The preset \"" << 
    			                    section.first << "\" contains invalid \"renamed_from\" key, which is being ignored.";
                       		}
                    	}
                        // Throws on parsing error. For system presets, no substituion is being done, but an exception is thrown.
                        config.set_deserialize(kvp.first, kvp.second.data(), substitution_context);
                    }
                };
                if (presets == &this->printers) {
                    DynamicPrintConfig config_src;
                    parse_config_section(config_src);
                    // A user bundle printer without "printer_technology" is a pre-2.x FFF profile.
                    if (! flags.has(LoadConfigBundleAttribute::LoadSystem) && config_src.option("printer_technology") == nullptr)
                        throw FffConfigurationException();
                    default_config = &presets->default_preset_for(config_src).config;
                    config = *default_config;
                    config.apply(config_src);
                } else {
                    default_config = &presets->default_preset().config;
                    config = *default_config;
                    parse_config_section(config);
                }
            } catch (const FffConfigurationException &) {
                // "printer_technology = FFF" (directly or inherited): not supported, skip the printer.
                skip_fff = true;
            } catch (const ConfigurationError &e) {
                throw ConfigurationError(format("Invalid configuration bundle \"%1%\", section [%2%]: ", path, section.first) + e.what());
            }
            if (skip_fff) {
                if (preset_name.find('*') != 0)
                    ++ m_skipped_fff_sections;
                BOOST_LOG_TRIVIAL(debug) << "Config bundle \"" << path << "\": skipping FFF printer [" << section.first << "]";
                continue;
            }
            Preset::normalize(config);
            // Report configuration fields, which are misplaced into a wrong group.
            std::string incorrect_keys = Preset::remove_invalid_keys(config, *default_config);
            if (! incorrect_keys.empty())
                BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" << 
                    section.first << "\" contains the following incorrect keys: " << incorrect_keys << ", which were removed";
            if (flags.has(LoadConfigBundleAttribute::LoadSystem) && presets == &printers) {
                // Filter out printer presets, which are not mentioned in the vendor profile.
                // These presets are considered not installed.
                auto printer_model   = config.opt_string("printer_model");
                if (printer_model.empty()) {
                    BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" << 
                        section.first << "\" defines no printer model, it will be ignored.";
                    continue;
                }
                auto printer_variant = config.opt_string("printer_variant");
                if (printer_variant.empty()) {
                    BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" << 
                        section.first << "\" defines no printer variant, it will be ignored.";
                    continue;
                }
                auto it_model = std::find_if(vendor_profile->models.cbegin(), vendor_profile->models.cend(),
                    [&](const VendorProfile::PrinterModel &m) { return m.id == printer_model; }
                );
                if (it_model == vendor_profile->models.end()) {
                    BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" << 
                        section.first << "\" defines invalid printer model \"" << printer_model << "\", it will be ignored.";
                    continue;
                }
                auto it_variant = it_model->variant(printer_variant);
                if (it_variant == nullptr) {
                    BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" << 
                        section.first << "\" defines invalid printer variant \"" << printer_variant << "\", it will be ignored.";
                    continue;
                }
                const Preset *preset_existing = presets->find_preset(section.first, false);
                if (preset_existing != nullptr) {
                    BOOST_LOG_TRIVIAL(error) << "Error in a Vendor Config Bundle \"" << path << "\": The printer preset \"" << 
                        section.first << "\" has already been loaded from another Confing Bundle.";
                    continue;
                }
            } else if (! flags.has(LoadConfigBundleAttribute::LoadSystem)) {
                // This is a user config bundle.
                const Preset *existing = presets->find_preset(preset_name, false);
                if (existing != nullptr) {
                    if (existing->is_system) {
    					assert(existing->vendor != nullptr);
                        BOOST_LOG_TRIVIAL(error) << "Error in a user provided Config Bundle \"" << path << "\": The " << presets->name() << " preset \"" << 
    						existing->name << "\" is a system preset of vendor " << existing->vendor->name << " and it will be ignored.";
                        continue;
                    } else {
                        assert(existing->vendor == nullptr);
                        BOOST_LOG_TRIVIAL(trace) << "A " << presets->name() << " preset \"" << existing->name << "\" was overwritten with a preset from user Config Bundle \"" << path << "\"";
                    }
                } else {
					BOOST_LOG_TRIVIAL(trace) << "A new " << presets->name() << " preset \"" << preset_name << "\" was imported from user Config Bundle \"" << path << "\"";
                }
            }
            // Decide a full path to this .ini file.
            auto file_name = boost::algorithm::iends_with(preset_name, ".ini") ? preset_name : preset_name + ".ini";
            auto file_path = (boost::filesystem::path(data_dir()) 
#ifdef SLIC3R_PROFILE_USE_PRESETS_SUBDIR
                // Store the presets into a "presets" directory.
                / "presets" 
#else
                // Store the presets at the same location as the upstream Slic3r.
#endif
                / presets->section_name() / file_name).make_preferred();
            // Load the preset into the list of presets, save it to disk.
            Preset &loaded = presets->load_preset(file_path.string(), preset_name, std::move(config), false);
            if (flags.has(LoadConfigBundleAttribute::SaveImported))
                loaded.save();
            if (flags.has(LoadConfigBundleAttribute::LoadSystem)) {
                loaded.is_system = true;
                loaded.vendor = vendor_profile;
            }

            // Derive the profile logical name aka alias from the preset name if the alias was not stated explicitely.
            if (alias_name.empty()) {
                size_t end_pos = preset_name.find_first_of("@");
	            if (end_pos != std::string::npos) {
	                alias_name = preset_name.substr(0, end_pos);
	                if (renamed_from.empty())
	                	// Add the preset name with the '@' character removed into the "renamed_from" list.
	                	renamed_from.emplace_back(alias_name + preset_name.substr(end_pos + 1));
                    boost::trim_right(alias_name);
	            }
	        }
	        if (alias_name.empty())
	        	loaded.alias = preset_name;
	        else 
	         	loaded.alias = std::move(alias_name);
	        loaded.renamed_from = std::move(renamed_from);
            if (! substitution_context.empty())
                substitutions.push_back({ 
                    preset_name, presets->type(), PresetConfigSubstitutions::Source::ConfigBundle, 
                    std::string(), std::move(substitution_context.substitutions) });
            ++ presets_loaded;
        }

    }

    // 3) Activate the presets if any exists.
    if (! flags.has(LoadConfigBundleAttribute::LoadSystem)) {
        if (! active_sla_print.empty()) 
            sla_prints.select_preset_by_name(active_sla_print, true);
        if (! active_sla_material.empty()) 
            sla_materials.select_preset_by_name(active_sla_material, true);
        if (! active_printer.empty())
            printers.select_preset_by_name(active_printer, true);
        this->update_compatible(PresetSelectCompatibleType::Never);
    }

    update_alias_maps();

    if (m_skipped_fff_sections > 0)
        BOOST_LOG_TRIVIAL(info) << "Config bundle \"" << path << "\": skipped " << m_skipped_fff_sections << " FFF section(s) (SLA/MSLA-only application)";

    return std::make_pair(std::move(substitutions), presets_loaded);
}



void PresetBundle::update_compatible(PresetSelectCompatibleType select_other_print_if_incompatible, PresetSelectCompatibleType select_other_material_if_incompatible)
{
    const Preset					&printer_preset					    = this->printers.get_edited_preset();
	const PresetWithVendorProfile    printer_preset_with_vendor_profile = this->printers.get_preset_with_vendor_profile(printer_preset);

    class PreferedProfileMatch
    {
    public:
        PreferedProfileMatch(const std::string &prefered_alias, const std::string &prefered_name) :
            m_prefered_alias(prefered_alias), m_prefered_name(prefered_name) {}

        int operator()(const Preset &preset) const
        {
            return 
                preset.is_default || preset.is_external ?
                    // Don't match any properties of the "-- default --" profile or the external profiles when switching printer profile.
                    0 :
                    ! m_prefered_alias.empty() && m_prefered_alias == preset.alias ?
                        // Matching an alias, always take this preset with priority.
                        std::numeric_limits<int>::max() :
                        // Otherwise take the prefered profile, or the first compatible.
                        preset.name == m_prefered_name;
        }

    private:
        const std::string  m_prefered_alias;
        const std::string &m_prefered_name;
    };

    // Matching by the layer height in addition.
    class PreferedPrintProfileMatch : public PreferedProfileMatch
    {
    public:
        PreferedPrintProfileMatch(const Preset *preset, const std::string &prefered_name) :
            PreferedProfileMatch(preset ? preset->alias : std::string(), prefered_name), m_prefered_layer_height(preset ? preset->config.opt_float("layer_height") : 0) {}

        int operator()(const Preset &preset) const
        {
            // Don't match any properties of the "-- default --" profile or the external profiles when switching printer profile.
            if (preset.is_default || preset.is_external)
                return 0;
            int match_quality = PreferedProfileMatch::operator()(preset);
            if (match_quality < std::numeric_limits<int>::max()) {
                match_quality += 1;
                if (m_prefered_layer_height > 0. && std::abs(preset.config.opt_float("layer_height") - m_prefered_layer_height) < 0.0005)
                    match_quality *= 10;
            }
            return match_quality;
        }

    private:
        const double m_prefered_layer_height;
    };

	switch (printer_preset.printer_technology()) {
    case ptSLA:
    {
		assert(printer_preset.config.has("default_sla_print_profile"));
		assert(printer_preset.config.has("default_sla_material_profile"));
		this->sla_prints.update_compatible(printer_preset_with_vendor_profile, nullptr, select_other_print_if_incompatible,
            PreferedPrintProfileMatch(this->sla_prints.get_selected_idx() == size_t(-1) ? nullptr : &this->sla_prints.get_edited_preset(), printer_preset.config.opt_string("default_sla_print_profile")));
        const PresetWithVendorProfile sla_print_preset_with_vendor_profile = this->sla_prints.get_edited_preset_with_vendor_profile();
		this->sla_materials.update_compatible(printer_preset_with_vendor_profile, &sla_print_preset_with_vendor_profile, select_other_material_if_incompatible,
            PreferedProfileMatch(this->sla_materials.get_selected_idx() == size_t(-1) ? std::string() : this->sla_materials.get_edited_preset().alias, printer_preset.config.opt_string("default_sla_material_profile")));
		break;
	}
    default: break;
    }
}

void PresetBundle::export_configbundle(const std::string &path, bool export_system_settings)
{
    // Write to a sibling and rename: a full disk or cancelled export must not destroy the previous bundle.
    const boost::filesystem::path temp_path = boost::filesystem::path(path).parent_path() / boost::filesystem::unique_path(".sla-bundle-%%%%-%%%%.tmp");
    boost::nowide::ofstream c;
    c.open(temp_path.string(), std::ios::out | std::ios::trunc);
    if (!c)
        throw Slic3r::RuntimeError("Cannot write the configuration bundle: " + temp_path.string());

    // Put a comment at the first line including the time stamp and Slic3r version.
    c << "# " << Slic3r::header_slic3r_generated() << std::endl;

    // Export the SLA print, material and printer profiles (an SLA-only bundle has no FFF sections).
    for (const PresetCollection *presets : { (const PresetCollection*)&this->sla_prints,
                                              (const PresetCollection*)&this->sla_materials,
                                              (const PresetCollection*)&this->printers }) {
        for (const Preset &preset : (*presets)()) {
            if (preset.is_default || preset.is_external || (preset.is_system && ! export_system_settings))
                // Only export the common presets, not external files or the default preset.
                continue;
            c << std::endl << "[" << presets->section_name() << ":" << preset.name << "]" << std::endl;
            for (const std::string &opt_key : preset.config.keys())
                c << opt_key << " = " << preset.config.opt_serialize(opt_key) << std::endl;
        }
    }

    // Export the names of the active presets.
    c << std::endl << "[presets]" << std::endl;
    c << "sla_print = " << this->sla_prints.get_selected_preset_name() << std::endl;
    c << "sla_material = " << this->sla_materials.get_selected_preset_name() << std::endl;
    c << "printer = " << this->printers.get_selected_preset_name() << std::endl;

    c.close();
    if (!c) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temp_path, ignored);
        throw Slic3r::RuntimeError("Cannot write the configuration bundle: " + path);
    }
    if (const auto error = rename_file(temp_path.string(), path)) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temp_path, ignored);
        throw Slic3r::RuntimeError("Cannot write the configuration bundle \"" + path + "\": " + error.message());
    }
}

void PresetBundle::set_default_suppressed(bool default_suppressed)
{
    sla_prints.set_default_suppressed(default_suppressed);
    sla_materials.set_default_suppressed(default_suppressed);
    printers.set_default_suppressed(default_suppressed);
}

void copy_bed_model_and_texture_if_needed(DynamicPrintConfig& config)
{
    const boost::filesystem::path user_dir = boost::filesystem::absolute(boost::filesystem::path(data_dir()) / "printer").make_preferred();
    const boost::filesystem::path res_dir  = boost::filesystem::absolute(boost::filesystem::path(resources_dir()) / "profiles").make_preferred();

    auto do_copy = [&user_dir, &res_dir](ConfigOptionString* cfg, const std::string& type) {
        if (cfg == nullptr || cfg->value.empty())
            return;

        const boost::filesystem::path src_dir = boost::filesystem::absolute(boost::filesystem::path(cfg->value)).make_preferred().parent_path();
        if (src_dir != user_dir && src_dir.parent_path() != res_dir) {
            const std::string dst_value = (user_dir / boost::filesystem::path(cfg->value).filename()).string();
            std::string error;
            if (copy_file_inner(cfg->value, dst_value, error) == SUCCESS)
                cfg->value = dst_value;
            else {
                BOOST_LOG_TRIVIAL(error) << "Copying from " << cfg->value << " to " << dst_value << " failed. Unable to set custom bed " << type << ". [" << error << "]";
                cfg->value = "";
            }
        }
    };

    do_copy(config.option<ConfigOptionString>("bed_custom_texture"), "texture");
    do_copy(config.option<ConfigOptionString>("bed_custom_model"), "model");
}

} // namespace Slic3r
