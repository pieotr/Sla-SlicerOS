///|/ Copyright (c) Prusa Research 2017 - 2023 Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, David Kocík @kocikdav, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2019 John Drake @foxox
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PresetBundle_hpp_
#define slic3r_PresetBundle_hpp_

#include "Preset.hpp"
#include "AppConfig.hpp"
#include "enum_bitmask.hpp"

#include <memory>
#include <unordered_map>
#include <array>
#include <boost/filesystem/path.hpp>

namespace Slic3r {

// Bundle of SLA print + SLA material + printer presets (the FFF print / filament / physical printer layers are gone).
class PresetBundle
{
public:
    PresetBundle();
    PresetBundle(const PresetBundle &rhs);
    PresetBundle& operator=(const PresetBundle &rhs);

    // Remove all the presets but the "-- default --".
    // Optionally remove all the files referenced by the presets from the user profile directory.
    void            reset(bool delete_files);

    void            setup_directories();
    void            import_newer_configs(const std::string& from);

    struct PresetPreferences {
        std::string printer_model_id;// name of a preferred printer model
        std::string printer_variant; // name of a preferred printer variant
        std::string sla_material;    // name of a preferred sla_material preset
    };

    // Load ini files of all types (sla_print, sla_material, printer) from Slic3r::data_dir().
    // Legacy FFF data of an old data directory (print/, filament/, physical_printer/ folders, FFF printers,
    // FFF vendor sections, [presets] print / filament / physical_printer keys) is ignored, never deleted.
    // Load selections (current print, current material, current printer) from config.ini
    // select preferred presets, if any exist
    PresetsConfigSubstitutions load_presets(AppConfig &config, ForwardCompatibilitySubstitutionRule rule,
                                            const PresetPreferences& preferred_selection = PresetPreferences());

    // Export selections (current print, current material, current printer) into config.ini
    void            export_selections(AppConfig &config);

    PresetCollection            sla_prints;
    PresetCollection            sla_materials;
    PrinterPresetCollection     printers;

    const PresetCollection&           get_presets(Preset::Type preset_type) const;
          PresetCollection&           get_presets(Preset::Type preset_type);

    // The project configuration values are kept separated from the print/material/printer preset,
    // they are being serialized / deserialized from / to the .3mf, .config, 
    // and they are being used by slicing core.
    DynamicPrintConfig          project_config;

    // There will be an entry for each system profile loaded, 
    // and the system profiles will point to the VendorProfile instances owned by PresetBundle::vendors.
    VendorMap                   vendors;

    struct ObsoletePresets {
        std::vector<std::string> sla_prints;
        std::vector<std::string> sla_materials;
        std::vector<std::string> printers;
    };
    ObsoletePresets             obsolete_presets;

    std::set<std::string>       tmp_installed_presets;

    bool                        has_defauls_only() const 
        { return printers.has_defaults_only(); }

    DynamicPrintConfig          full_config() const;
    // Same as full_config() (the legacy print host credentials are no longer part of the configuration).
    DynamicPrintConfig          full_config_secure() const;

    // Load user configuration and store it into the user profiles.
    // This method is called by the configuration wizard.
    void                        load_config_from_wizard(const std::string &name, DynamicPrintConfig config)
        { this->load_config_file_config(name, false, std::move(config)); }

    // Load configuration that comes from a model file containing configuration, such as 3MF et al.
    // This method is called by the Plater.
    void                        load_config_model(const std::string &name, DynamicPrintConfig config)
        { this->load_config_file_config(name, true, std::move(config)); }

    // Load an external config file (ini or config bundle) containing the SLA print, material and printer presets.
    // A configuration with "printer_technology = FFF" is rejected with an FffConfigurationException message.
    // If the file is loaded successfully, its print / material / printer profiles will be activated.
    ConfigSubstitutions         load_config_file(const std::string &path, ForwardCompatibilitySubstitutionRule compatibility_rule);

    // Load a config bundle file, into presets and store the loaded presets into separate files
    // of the local configuration directory.
    // Load settings into the provided settings instance.
    // Activate the presets stored in the config bundle.
    // Returns the number of presets loaded successfully. [print:], [filament:], [physical_printer:], FFF [printer:]
    // sections and FFF printer models are skipped (counted in skipped_fff_sections()).
    enum LoadConfigBundleAttribute { 
        // Save the profiles, which have been loaded.
        SaveImported,
        // Delete all old config profiles before loading.
        ResetUserProfile,
        // Load a system config bundle.
        LoadSystem,
        LoadVendorOnly,
    };
    using LoadConfigBundleAttributes = enum_bitmask<LoadConfigBundleAttribute>;
    // Load the config bundle based on the flags.
    // Don't do any config substitutions when loading a system profile, perform and report substitutions otherwise.
    std::pair<PresetsConfigSubstitutions, size_t> load_configbundle(
        const std::string &path, LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule);

    // Export a config bundle file containing all the presets and the names of the active presets.
    void                        export_configbundle(const std::string &path, bool export_system_settings = false);

    // Number of FFF-only sections ([print:], [filament:], [physical_printer:], FFF [printer:]) skipped by the most
    // recent load_configbundle() of this bundle. The GUI uses it to tell the user that an imported bundle
    // contained FFF data that was ignored.
    size_t                      skipped_fff_sections() const { return m_skipped_fff_sections; }

    // Enable / disable the "- default -" preset.
    void                        set_default_suppressed(bool default_suppressed);

    // Update the is_compatible flag of all SLA print and material presets depending on whether they are marked
    // as compatible with the currently selected printer (and print in case of material presets).
    // Also updates the is_visible flag of each preset.
    // If select_other_if_incompatible is true, then the print or material preset is switched to some compatible
    // preset if the current print or material preset is not compatible.
    void                        update_compatible(PresetSelectCompatibleType select_other_print_if_incompatible, PresetSelectCompatibleType select_other_material_if_incompatible);
    void                        update_compatible(PresetSelectCompatibleType select_other_if_incompatible) { this->update_compatible(select_other_if_incompatible, select_other_if_incompatible); }

    // Set the is_visible flag for printer vendors, printer models and printer variants
    // based on the user configuration.
    // If the "vendor" section is missing, enable all models and variants of the particular vendor.
    void                        load_installed_printers(const AppConfig &config);

    const std::string&          get_preset_name_by_alias(const Preset::Type& preset_type, const std::string& alias);
    const std::string&          get_preset_name_by_alias_invisible(const Preset::Type& preset_type, const std::string& alias) const;

    // Save current preset of a provided type under a new name. If the name is different from the old one,
    // Unselected option would be reverted to the beginning values
    void                        save_changes_for_preset(const std::string& new_name, Preset::Type type, const std::vector<std::string>& unselected_options);
    // Transfer options form preset_from_name preset to preset_to_name preset and save preset_to_name preset as new new_name preset
    // Return false, if new preset wasn't saved
    bool                        transfer_and_save(Preset::Type type, const std::string& preset_from_name, const std::string& preset_to_name,
                                                  const std::string& new_name, const std::vector<std::string>& options);

    static const char *PRUSA_BUNDLE;

    static std::array<Preset::Type, 3>  types_list() {
        return      { Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };
    }

    // User presets that reference `name` of the given SLA type and would have to change if it were deleted:
    // children (inherits), compatible_printers / compatible_prints entries and printers' default profiles.
    // Empty when nothing depends on it. System, external and default presets are never listed.
    std::vector<PresetDependent> find_preset_dependents(Preset::Type type, const std::string &name) const;

    // Delete a saved user preset and fix every dependent in ONE file transaction (all or nothing; throws
    // with disk and memory unchanged). The GUI lists find_preset_dependents() and asks before calling it.
    //  - children get "inherits" of the deleted parent; their config already holds the complete values;
    //  - the name is removed from compatible_printers / compatible_prints; a list that becomes empty turns
    //    into the explicit "None" state (empty list + condition "false"), never into "compatible with all";
    //  - default_sla_print_profile / default_sla_material_profile pointing to it are cleared.
    // Returns false when no such preset exists.
    bool delete_preset_with_dependencies(Preset::Type type, const std::string &name);

    // Export one SLA preset (printer, material or process) as a stand-alone .ini file (temp file + rename).
    void export_preset_ini(Preset::Type type, const std::string &name, const std::string &path) const;

    // Result of import_preset_ini().
    struct PresetImportResult {
        std::string name;          // name under which the preset was stored
        bool        renamed = false; // the suggested name was taken, `name` differs from the file stem
    };
    // Import a single SLA preset .ini into the user presets of `type`. The file is normalized
    // (Preset::normalize) and, for printers, checked with sla::printer_geometry_error; a readable error is
    // thrown for FFF/foreign files and invalid geometry, leaving everything unchanged. When the name is taken
    // the preset is stored as "<name> (2)", "(3)" ... and `renamed` is set. `suggested_name` may be empty
    // (the file stem is used).
    PresetImportResult import_preset_ini(Preset::Type type, const std::string &path, const std::string &suggested_name = std::string());
    // First free name derived from `name` ("name", "name (2)", ...) in the collection of `type`.
    std::string unique_preset_name(Preset::Type type, const std::string &name) const;

private:
    // Hooks the collections' rename notification to cross-collection reference migration.
    void install_rename_hooks();
    std::function<void()> stage_renamed_preset_references(Preset::Type type, const std::string &old_name, const std::string &new_name, PresetFileTransaction &transaction);

    std::pair<PresetsConfigSubstitutions, std::string> load_system_presets(ForwardCompatibilitySubstitutionRule compatibility_rule);
    // Merge one vendor's presets with the other vendor's presets, report duplicates.
    std::vector<std::string>    merge_presets(PresetBundle &&other);
    // Update renamed_from and alias maps of system profiles.
    void 						update_system_maps();
    // Update alias maps
    void 						update_alias_maps();

    // Set the is_visible flag for sla materials,
    // apply defaults based on enabled printers when no materials are installed.
    void                        load_installed_sla_materials(AppConfig &config);

    // Load selections (current print, current material, current printer) from config.ini
    // This is done just once on application start up.
    void                        load_selections(AppConfig &config, const PresetPreferences& preferred_selection = PresetPreferences());

    // Load SLA print, material & printer presets from a config. If it is an external config, then the name is extracted from the external path.
    // and the external config is just referenced, not stored into user profile directory.
    // If it is not an external config, then the config will be stored into the user profile directory.
    void                        load_config_file_config(const std::string &name_or_path, bool is_external, DynamicPrintConfig &&config);
    ConfigSubstitutions         load_config_file_config_bundle(
        const std::string &path, const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule);

    DynamicPrintConfig          full_sla_config() const;

    size_t                      m_skipped_fff_sections { 0 };
};

ENABLE_ENUM_BITMASK_OPERATORS(PresetBundle::LoadConfigBundleAttribute)

// Copies bed texture and model files to 'data_dir()\printer' folder, if needed
// and updates the config accordingly
extern void copy_bed_model_and_texture_if_needed(DynamicPrintConfig& config);

} // namespace Slic3r

#endif /* slic3r_PresetBundle_hpp_ */
