///|/ Copyright (c) Prusa Research 2017 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2019 John Drake @foxox
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Preset_hpp_
#define slic3r_Preset_hpp_

#include <deque>
#include <functional>
#include <set>
#include <unordered_map>

#include <boost/filesystem/path.hpp>
#include <boost/property_tree/ptree_fwd.hpp>

#include "PrintConfig.hpp"
#include "Semver.hpp"

namespace Slic3r {

class AppConfig;
class PresetBundle;

enum ConfigFileType
{
    CONFIG_FILE_TYPE_UNKNOWN,
    CONFIG_FILE_TYPE_APP_CONFIG,
    CONFIG_FILE_TYPE_CONFIG,
    CONFIG_FILE_TYPE_CONFIG_BUNDLE,
};

extern ConfigFileType guess_config_file_type(const boost::property_tree::ptree &tree);

class VendorProfile
{
public:
    std::string                     name;
    std::string                     id;
    Semver                          config_version;
    std::string                     config_update_url;
    std::string                     changelog_url;
    std::string                     repo_id;
    std::string                     repo_prefix;

    struct PrinterVariant {
        PrinterVariant() {}
        PrinterVariant(const std::string &name) : name(name) {}
        std::string                 name;
    };

    struct PrinterModel {
        PrinterModel() {}
        std::string                 id;
        std::string                 name;
        PrinterTechnology           technology;
        std::string                 family;
        std::vector<PrinterVariant> variants;
        std::vector<std::string>	default_materials;
        // Vendor & Printer Model specific print bed model & texture.
        std::string 			 	bed_model;
        std::string 				bed_texture;
        std::string                 thumbnail;

        PrinterVariant*       variant(const std::string &name) {
            for (auto &v : this->variants)
                if (v.name == name)
                    return &v;
            return nullptr;
        }

        const PrinterVariant* variant(const std::string &name) const { return const_cast<PrinterModel*>(this)->variant(name); }
    };
    std::vector<PrinterModel>          models;

    std::set<std::string>              default_sla_materials;

    VendorProfile() {}
    VendorProfile(std::string id) : id(std::move(id)) {}

    bool 		valid() const { return ! name.empty() && ! id.empty() && config_version.valid(); }

    // Load VendorProfile from an ini file.
    // If `load_all` is false, only the header with basic info (name, version, URLs) is loaded.
    static VendorProfile from_ini(const boost::filesystem::path &path, bool load_all=true);
    static VendorProfile from_ini(const boost::property_tree::ptree &tree, const boost::filesystem::path &path, bool load_all=true);

    size_t      num_variants() const { size_t n = 0; for (auto &model : models) n += model.variants.size(); return n; }
    std::vector<std::string> families() const;

    bool        operator< (const VendorProfile &rhs) const { return this->id <  rhs.id; }
    bool        operator==(const VendorProfile &rhs) const { return this->id == rhs.id; }
};

class Preset;

// Helper to hold a profile with its vendor definition, where the vendor definition may have been extracted from a parent system preset.
// The parent preset is only accessible through PresetCollection, therefore to allow definition of the various is_compatible_with methods
// outside of the PresetCollection, this composite is returned by PresetCollection::get_preset_with_vendor_profile() when needed.
struct PresetWithVendorProfile {
	PresetWithVendorProfile(const Preset &preset, const VendorProfile *vendor) : preset(preset), vendor(vendor) {}
	const Preset 		&preset;
	const VendorProfile *vendor;
};

// Note: it is imporant that map is used here rather than unordered_map,
// because we need iterators to not be invalidated,
// because Preset and the ConfigWizard hold pointers to VendorProfiles.
// XXX: maybe set is enough (cf. changes in Wizard)
typedef std::map<std::string, VendorProfile> VendorMap;

class Preset
{
public:
    enum Type
    {
        // The numeric values are compacted after the removal of the FFF print / filament types (W9b-1).
        // They live only in memory (arrays indexed by type, Search keys, Undo stacks); AppConfig, snapshots,
        // preset files and 3MF store preset names and section names, never these numbers.
        TYPE_INVALID,
        TYPE_SLA_PRINT,
        TYPE_SLA_MATERIAL,
        TYPE_PRINTER,
        TYPE_COUNT,
        // This type is here to support search through the Preferences
        TYPE_PREFERENCES,
    };

    Type                type        = TYPE_INVALID;

    // The preset represents a "default" set of properties,
    // pulled from the default values of the PrintConfig (see PrintConfigDef for their definitions).
    bool                is_default = false;
    // External preset points to a configuration, which has been loaded but not imported
    // into the Slic3r default configuration location.
    bool                is_external = false;
    // System preset is read-only.
    bool                is_system   = false;
    // Preset is visible, if it is associated with a printer model / variant that is enabled in the AppConfig
    // or if it has no printer model / variant association.
    // Also the "default" preset is only visible, if it is the only preset in the list.
    bool                is_visible  = true;
    // Has this preset been modified?
    bool                is_dirty    = false;
    // Is this preset compatible with the currently active printer?
    bool                is_compatible = true;

    bool                is_user() const { return ! this->is_default && ! this->is_system; }

    // Name of the preset, usually derived form the file name.
    std::string         name;
    // File name of the preset. This could be an SLA print / material / printer preset, 
    // or a Configuration file bundling the SLA print + material + printer presets (in that case is_external and possibly is_system will be true).
    std::string         file;
    // If this is a system profile, then there should be a vendor data available to display at the UI.
    const VendorProfile *vendor      = nullptr;

    // Has this profile been loaded?
    bool                loaded      = false;

    // Configuration data, loaded from a file, or set from the defaults.
    DynamicPrintConfig  config;

    // Alias of the preset
    std::string         alias;
    // List of profile names, from which this profile was renamed at some point of time.
    // This list is then used to match profiles by their names when loaded from .3mf, .amf,
    // and to match the "inherits" field of user profiles with updated system profiles.
    std::vector<std::string> renamed_from;

    void                save();

    // Return a label of this preset, consisting of a name and a "(modified)" suffix, if this preset is dirty.
    std::string         label() const;

    // Set the is_dirty flag if the provided config is different from the active one.
    void                set_dirty(const DynamicPrintConfig &config) { this->is_dirty = ! this->config.diff(config).empty(); }
    void                set_dirty(bool dirty = true) { this->is_dirty = dirty; }
    void                reset_dirty() { this->is_dirty = false; }

    // Returns the name of the preset, from which this preset inherits.
    static std::string& inherits(DynamicPrintConfig &cfg) { return cfg.option<ConfigOptionString>("inherits", true)->value; }
    std::string&        inherits() { return Preset::inherits(this->config); }
    const std::string&  inherits() const { return Preset::inherits(const_cast<Preset*>(this)->config); }

    // Returns the "compatible_prints_condition".
    static std::string& compatible_prints_condition(DynamicPrintConfig &cfg) { return cfg.option<ConfigOptionString>("compatible_prints_condition", true)->value; }
    std::string&        compatible_prints_condition() { 
		assert(this->type == TYPE_SLA_MATERIAL);
        return Preset::compatible_prints_condition(this->config);
    }
    const std::string&  compatible_prints_condition() const { return const_cast<Preset*>(this)->compatible_prints_condition(); }

    // Returns the "compatible_printers_condition".
    static std::string& compatible_printers_condition(DynamicPrintConfig &cfg) { return cfg.option<ConfigOptionString>("compatible_printers_condition", true)->value; }
    std::string&        compatible_printers_condition() {
		assert(this->type == TYPE_SLA_PRINT || this->type == TYPE_SLA_MATERIAL);
        return Preset::compatible_printers_condition(this->config);
    }
    const std::string&  compatible_printers_condition() const { return const_cast<Preset*>(this)->compatible_printers_condition(); }

    // Return a printer technology, return ptSLA if the printer technology is not set.
    static PrinterTechnology printer_technology(const DynamicPrintConfig &cfg) {
        auto *opt = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
        // The following assert may trigger when importing some legacy profile, 
        // but it is safer to keep it here to capture the cases where the "printer_technology" key is queried, where it should not.
//        assert(opt != nullptr);
        return (opt == nullptr) ? ptSLA : opt->value;
    }
    PrinterTechnology   printer_technology() const { return Preset::printer_technology(this->config); }
    // This call returns a reference, it may add a new entry into the DynamicPrintConfig.
    PrinterTechnology&  printer_technology_ref() { return this->config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology", true)->value; }

    // Set is_visible according to application config
    void                set_visible_from_appconfig(const AppConfig &app_config);

    // Sort lexicographically by a preset name. The preset name shall be unique across a single PresetCollection.
    bool                operator<(const Preset &other) const { return this->name < other.name; }

    // Returns id without trimmed prefix if present and vendor has any.
    std::string         trim_vendor_repo_prefix(const std::string& id) const;
    std::string         trim_vendor_repo_prefix(const std::string& id, const VendorProfile* vendor_profile) const;

    static const std::vector<std::string>&  sla_printer_options();
    static const std::vector<std::string>&  sla_material_options();
    static const std::vector<std::string>&  sla_print_options();
    // Keys of sla_print/material/printer_options() that intentionally have no direct editor field in the
    // Tab pages (derived, widget-managed or internal). Every other key in those lists must be exposed by a Tab;
    // Tab.cpp verifies this in debug builds and the libslic3r tests pin the whitelist (no stale entries).
    static const std::vector<std::string>&  sla_hidden_option_keys();

	static void                             update_suffix_modified(const std::string& new_suffix_modified);
    static const std::string&               suffix_modified();
    static std::string                      remove_suffix_modified(const std::string& name);
    static void                             normalize(DynamicPrintConfig &config);
    // Report configuration fields, which are misplaced into a wrong group, remove them from the config.
    static std::string                      remove_invalid_keys(DynamicPrintConfig &config, const DynamicPrintConfig &default_config);

protected:
    Preset(Type type, const std::string &name, bool is_default = false) : type(type), is_default(is_default), name(name) {}
    Preset() = default;

    friend class        PresetCollection;
    friend class        PresetBundle;
};

// Keep a malformed rule distinct from a valid rule that simply does not match.
// Both are incompatible; the GUI can explain the former without duplicating
// expression parsing or changing the user's saved rule.
struct PresetCompatibilityResult {
    bool compatible;
    std::string error;
};
PresetCompatibilityResult evaluate_compatibility_with_print(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_print, const PresetWithVendorProfile &active_printer);
PresetCompatibilityResult evaluate_compatibility_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer, const DynamicPrintConfig *extra_config);
PresetCompatibilityResult evaluate_compatibility_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer);
bool is_compatible_with_print  (const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_print, const PresetWithVendorProfile &active_printer);
bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer, const DynamicPrintConfig *extra_config);
bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer);

enum class PresetSelectCompatibleType {
	// Never select a compatible preset if the newly selected profile is not compatible.
	Never,
	// Only select a compatible preset if the active profile used to be compatible, but it is no more.
	OnlyIfWasCompatible,
	// Always select a compatible preset if the active profile is no more compatible.
	Always
};

// Substitutions having been performed during parsing a single configuration file.
struct PresetConfigSubstitutions {
    // User readable preset name.
    std::string                             preset_name;
    // Type of the preset (SLA print / material / printer ...)
    Preset::Type                            preset_type;
    enum class Source {
        UserFile,
        ConfigBundle,
    };
    Source                                  preset_source;
    // Source of the preset. It may be empty in case of a ConfigBundle being loaded.
    std::string                             preset_file;
    // What config value has been substituted with what.
    ConfigSubstitutions                     substitutions;
};

// This struct is used to get an information about loaded external preset
struct ExternalPreset
{
    Preset* preset          { nullptr };
    bool    is_modified     { false };
    bool    is_installed    { false };

    ExternalPreset(Preset* preset, bool modified, bool installed = false)
    : preset(preset)
    , is_modified(modified)
    , is_installed(installed) {}

    virtual ~ExternalPreset() = default;
};

// Substitutions having been performed during parsing a set of configuration files, for example when starting up
// PrusaSlicer and reading the user SLA print / material / printer profiles.
using PresetsConfigSubstitutions = std::vector<PresetConfigSubstitutions>;

// Collections of presets of the same type (one of the SLA print, SLA material or Printer type).
// All-or-nothing change of several preset INI files (rename with dependents, delete with dependents,
// import). New contents are first written to temp files next to their targets; only when every temp file
// exists are they published by renames. A failure before publication deletes the temps; a failure during
// publication restores the already published files from in-memory copies of their previous bytes. Callers
// must change their in-memory state only after commit() returned, so memory and disk never diverge.
// Process-crash recovery (journal) is out of scope here: the window is a few renames of small files.
class PresetFileTransaction
{
public:
    // Test hook: called before publishing change `index`; throwing simulates a failed write/rename.
    // The rollback phase calls it with `rollback == true` so tests can also break the restore.
    using FaultInjector = std::function<void(size_t index, bool rollback)>;
    static void set_fault_injector(FaultInjector injector);

    PresetFileTransaction() = default;
    PresetFileTransaction(const PresetFileTransaction&) = delete;
    PresetFileTransaction& operator=(const PresetFileTransaction&) = delete;
    ~PresetFileTransaction();

    // Serialize `config` into a temp file next to `path`. `must_not_exist` rejects overwriting an existing
    // file (rename target, imported preset); otherwise an existing regular file is replaced.
    void stage_config(const std::string &path, const DynamicPrintConfig &config, bool must_not_exist = false);
    // Publish the exact bytes of `from` under the new path `to` and remove `from` (rename without
    // re-serializing, so the file stays byte-identical). `to` must not exist.
    void stage_rename(const std::string &from, const std::string &to);
    // Delete `path` on publication (it must be a regular file).
    void stage_remove(const std::string &path);
    size_t size() const { return m_changes.size(); }
    // Publish everything or nothing; throws std::runtime_error with a readable message.
    void commit();

private:
    struct Change {
        std::string path;
        std::string temp;                 // empty for a removal or once published
        bool        removal = false;
        bool        had_original = false;
        std::string original;             // previous bytes, for rollback
    };
    void record_original(Change &change, bool must_not_exist);
    void discard_temps() noexcept;
    std::vector<Change> m_changes;
    bool                m_done = false;
};

// A user preset (or project default) that references another preset which is about to be deleted.
struct PresetDependent {
    enum class Kind {
        Inherits,                 // child of the deleted preset (same collection)
        CompatiblePrinters,       // compatible_printers list of a print/material
        CompatiblePrints,         // compatible_prints list of a material
        DefaultSlaPrint,          // printer's default_sla_print_profile
        DefaultSlaMaterial,       // printer's default_sla_material_profile
    };
    Preset::Type type;
    std::string  name;
    Kind         kind;
};

class PresetCollection
{
public:
    // Initialize the PresetCollection with the "- default -" preset.
    PresetCollection(Preset::Type type, const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &default_name = "- default -");

    typedef std::deque<Preset>::iterator Iterator;
    typedef std::deque<Preset>::const_iterator ConstIterator;
    Iterator        begin() { return m_presets.begin() + m_num_default_presets; }
    ConstIterator   begin() const { return m_presets.cbegin() + m_num_default_presets; }
    ConstIterator   cbegin() const { return m_presets.cbegin() + m_num_default_presets; }
    Iterator        end() { return m_presets.end(); }
    ConstIterator   end() const { return m_presets.cend(); }
    ConstIterator   cend() const { return m_presets.cend(); }

    void            reset(bool delete_files);

    Preset::Type    type() const { return m_type; }
    // Name, to be used on the screen and in error messages. Not localized.
    std::string     name() const;
    // Name, to be used as a section name in config bundle, and as a folder name for presets.
    std::string     section_name() const;
    const std::deque<Preset>& operator()() const { return m_presets; }

    // Add default preset at the start of the collection, increment the m_default_preset counter.
    void            add_default_preset(const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &preset_name);

    // Load ini files of the particular type from the provided directory path.
    void            load_presets(const std::string &dir_path, const std::string &subdir, PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule rule);

    // Load a preset from an already parsed config file, insert it into the sorted sequence of presets
    // and select it, losing previous modifications.
    Preset&         load_preset(const std::string &path, const std::string &name, const DynamicPrintConfig &config, bool select = true);
    Preset&         load_preset(const std::string &path, const std::string &name, DynamicPrintConfig &&config, bool select = true);

    // Returns a loaded preset,
    // returns is_modified as true if an existing preset was selected AND modified from config,
    // returns is_installed as true if a preset was selected AND set as visible during selection.
    enum class LoadAndSelect {
        // Never select
        Never,
        // Always select
        Always,
        // Select a profile only if it was modified.
        OnlyIfModified,
    };
    ExternalPreset load_external_preset(
        // Path to the profile source file (an AMF or 3MF file, a config file)
        const std::string           &path,
        // Name of the profile, derived from the source file name.
        const std::string           &name,
        // Original name of the profile, extracted from the loaded config. Empty, if the name has not been stored.
        const std::string           &original_name,
        // Config to initialize the preset from.
        const DynamicPrintConfig    &config,
        // Select the preset after loading?
        LoadAndSelect                select = LoadAndSelect::Always);

    // Save the preset under a new name. If the name is different from the old one,
    // a new preset is stored into the list of presets.
    // All presets are marked as not modified and the new preset is activated.
    // return true, if new preset is stored
    bool            save_current_preset(const std::string &new_name, bool detach = false);

    // Rename a saved user preset without saving or discarding its edited draft.
    // I/O/name failures leave the collection and selection unchanged; throws.
    // Returns false when the name is unchanged. Same-collection "inherits" references of user presets
    // (in memory, in the edited draft and on disk) are migrated, then the rename hook is invoked so that
    // the owner (PresetBundle) can migrate references held by other collections. All file changes form one
    // PresetFileTransaction: if any dependent file cannot be written, disk and memory stay exactly as before.
    bool            rename_current_preset(const std::string &new_name);

    // Called by rename_current_preset() BEFORE anything is changed, with (type, old name, new name, transaction).
    // The owner (PresetBundle) stages the files of other collections that reference the old name into the
    // transaction and returns a closure that applies the same edits in memory after the commit. Not copied.
    using RenameHookFunction = std::function<std::function<void()>(Preset::Type, const std::string&, const std::string&, PresetFileTransaction&)>;
    struct RenameHook {
        RenameHookFunction function;
        RenameHook() = default;
        RenameHook(const RenameHook&) {}
        RenameHook& operator=(const RenameHook&) { return *this; }
    };
    void            set_rename_hook(RenameHookFunction hook) { m_rename_hook.function = std::move(hook); }

    // Apply `edit` (returns true if it changed the config) to every loaded user preset with a file; changed
    // presets are written in one transaction before the in-memory copies are updated. The edited and saved
    // copies of the selected preset get the same edit so unsaved changes are preserved. Throws on failure
    // with disk and memory unchanged.
    void            migrate_user_preset_configs(const std::function<bool(DynamicPrintConfig&)> &edit);
    // Two-phase variant for callers that combine several collections in one transaction: stage the new
    // files of the presets `edit` would change (memory untouched) and return the closure that applies `edit`
    // in memory. Call the closure only after the transaction committed.
    std::function<void()> stage_user_preset_migration(const std::function<bool(DynamicPrintConfig&)> &edit, PresetFileTransaction &transaction);

    // Delete a user preset together with the references to it, as one transaction. Callers must have shown
    // the list from PresetBundle::find_preset_dependents(); see PresetBundle::delete_preset_with_dependencies().
    // `stage_extra` stages file changes of other collections and returns their in-memory closure.
    bool            delete_preset_transactional(const std::string &name, const std::function<bool(DynamicPrintConfig&)> &flatten_children,
                        const std::function<std::function<void()>(PresetFileTransaction&)> &stage_extra);
    // Names of the loaded user presets `edit` would change (the edit is applied to copies only).
    std::vector<std::string> user_presets_affected_by(const std::function<bool(DynamicPrintConfig&)> &edit) const;

    // Find the preset with a new_name or create a new one,
    // initialize it with the initial_preset config.
    Preset&         get_preset_with_name(const std::string& new_name, const Preset* initial_preset);

    // Delete the current preset, activate the first visible preset.
    // returns true if the preset was deleted successfully.
    bool            delete_current_preset();
    // Delete the current preset, activate the first visible preset.
    // returns true if the preset was deleted successfully.
    bool            delete_preset(const std::string& name);

    // Enable / disable the "- default -" preset.
    void            set_default_suppressed(bool default_suppressed);
    bool            is_default_suppressed() const { return m_default_suppressed; }

    // Select a preset. If an invalid index is provided, the first visible preset is selected.
    Preset&         select_preset(size_t idx);
    // Return the selected preset, without the user modifications applied.
    Preset&         get_selected_preset()       { return m_presets[m_idx_selected]; }
    const Preset&   get_selected_preset() const { return m_presets[m_idx_selected]; }
    size_t          get_selected_idx()    const { return m_idx_selected; }
    // Returns the name of the selected preset, or an empty string if no preset is selected.
    std::string     get_selected_preset_name() const { return (m_idx_selected == size_t(-1)) ? std::string() : this->get_selected_preset().name; }
    // Source ID of an imported SLA preset; library presets keep their selected name.
    std::string     get_selected_preset_source_name(const char *settings_id) const;
    // For the current edited preset, return the parent preset if there is one.
    // If there is no parent preset, nullptr is returned.
    // The parent preset may be a system preset or a user preset, which will be
    // reflected by the UI.
    const Preset*   get_selected_preset_parent() const;
	// Get parent preset for a child preset, based on the "inherits" field of a child,
	// where the "inherits" profile name is searched for in both m_presets and m_map_system_profile_renamed.
	const Preset*	get_preset_parent(const Preset& child) const;
	// Return the selected preset including the user modifications.
    Preset&         get_edited_preset()         { return m_edited_preset; }
    const Preset&   get_edited_preset() const   { return m_edited_preset; }

    // Return the last saved preset.
//  const Preset&   get_saved_preset() const { return m_saved_preset; }

    // Return vendor of the first parent profile, for which the vendor is defined, or null if such profile does not exist.
    PresetWithVendorProfile get_preset_with_vendor_profile(const Preset &preset) const;
    PresetWithVendorProfile get_edited_preset_with_vendor_profile() const { return this->get_preset_with_vendor_profile(this->get_edited_preset()); }

    const std::string& 		get_preset_name_by_alias(const std::string& alias) const;
    const std::string&      get_preset_name_by_alias_invisible(const std::string& alias) const;
	const std::string*		get_preset_name_renamed(const std::string &old_name) const;

	// used to update preset_choice from Tab
	const std::deque<Preset>&	get_presets() const	{ return m_presets; }
    size_t                      get_idx_selected()	{ return m_idx_selected; }
	static const std::string&	get_suffix_modified();

    // Return a preset possibly with modifications.
	Preset&			default_preset(size_t idx = 0)		 { assert(idx < m_num_default_presets); return m_presets[idx]; }
	const Preset&   default_preset(size_t idx = 0) const { assert(idx < m_num_default_presets); return m_presets[idx]; }
	virtual const Preset& default_preset_for(const DynamicPrintConfig & /* config */) const { return this->default_preset(); }
    // Return a preset by an index. If the preset is active, a temporary copy is returned.
    Preset&         preset(size_t idx, bool respect_active_preset = true)          { return (idx == m_idx_selected && respect_active_preset) ? m_edited_preset : m_presets[idx]; }
    const Preset&   preset(size_t idx, bool respect_active_preset = true) const    { return const_cast<PresetCollection*>(this)->preset(idx); }
    void            discard_current_changes() {
        m_presets[m_idx_selected].reset_dirty();
        m_edited_preset = m_presets[m_idx_selected];
        // Don't save the resetted preset state as a "saved into project" state.
//        update_saved_preset_from_current_preset();
    }

    // Return a preset by its name. If the preset is active, a temporary copy is returned.
    // If a preset is not found by its name, null is returned.
    Preset*         find_preset(const std::string &name, bool first_visible_if_not_found = false, bool respect_active_preset = true);
    const Preset*   find_preset(const std::string &name, bool first_visible_if_not_found = false, bool respect_active_preset = true) const 
        { return const_cast<PresetCollection*>(this)->find_preset(name, first_visible_if_not_found, respect_active_preset); }

    size_t          get_preset_idx_by_name(const std::string preset_name) const;

    size_t          first_visible_idx() const;
    // Return index of the first compatible preset. Certainly at least the '- default -' preset shall be compatible.
    // If one of the prefered_alternates is compatible, select it.
    template<typename PreferedCondition>
    size_t          first_compatible_idx(PreferedCondition prefered_condition) const
    {
        size_t i = m_default_suppressed ? m_num_default_presets : 0;
        size_t n = m_presets.size();
        size_t i_compatible = n;
        int    match_quality = -1;
        for (; i < n; ++ i)
            // Since we use the material selection from Wizard, it's needed to control the preset visibility too 
            if (m_presets[i].is_compatible && m_presets[i].is_visible) {
                int this_match_quality = prefered_condition(m_presets[i]);
                if (this_match_quality > match_quality) {
                    if (match_quality == std::numeric_limits<int>::max())
                        // Better match will not be found.
                        return i;
                    // Store the first compatible profile with highest match quality into i_compatible.
                    i_compatible = i;
                    match_quality = this_match_quality;
                } 
            }
        return (i_compatible == n) ? 
            // No compatible preset found, return the default preset.
            0 :
            // Compatible preset found.
            i_compatible;
    }
    // Return index of the first compatible preset. Certainly at least the '- default -' preset shall be compatible.
    size_t          first_compatible_idx() const { return this->first_compatible_idx([](const Preset&) -> int { return 0; }); }

    // Return index of the first visible preset. Certainly at least the '- default -' preset shall be visible.
    // Return the first visible preset. Certainly at least the '- default -' preset shall be visible.
    Preset&         first_visible()             { return this->preset(this->first_visible_idx()); }
    const Preset&   first_visible() const       { return this->preset(this->first_visible_idx()); }
    Preset&         first_compatible()          { return this->preset(this->first_compatible_idx()); }
    template<typename PreferedCondition>
    Preset&         first_compatible(PreferedCondition prefered_condition) { return this->preset(this->first_compatible_idx(prefered_condition)); }
    const Preset&   first_compatible() const    { return this->preset(this->first_compatible_idx()); }

    // Return number of presets including the "- default -" preset.
    size_t          size() const                { return m_presets.size(); }
    bool            has_defaults_only() const   { return m_presets.size() <= m_num_default_presets; }

    // For SLA print / material presets, disable those, which are not compatible with the printer.
    template<typename PreferedCondition>
    void            update_compatible(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType select_other_if_incompatible, PreferedCondition prefered_condition)
    {
        if (this->update_compatible_internal(active_printer, active_print, select_other_if_incompatible) == (size_t)-1)
            // Find some other compatible preset, or the "-- default --" preset.
            this->select_preset(this->first_compatible_idx(prefered_condition));        
    }
    void            update_compatible(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType select_other_if_incompatible)
        { this->update_compatible(active_printer, active_print, select_other_if_incompatible, [](const Preset&) -> int { return 0; }); }

    size_t          num_visible() const { return std::count_if(m_presets.begin(), m_presets.end(), [](const Preset &preset){return preset.is_visible;}); }

    // Compare the content of get_selected_preset() with get_edited_preset() configs, return true if they differ.
    bool                        current_is_dirty() const 
        { return is_dirty(&this->get_edited_preset(), &this->get_selected_preset()); }
    // Compare the content of get_selected_preset() with get_edited_preset() configs, return the list of keys where they differ.
    std::vector<std::string>    current_dirty_options(const bool deep_compare = false) const
        { return dirty_options(&this->get_edited_preset(), &this->get_selected_preset(), deep_compare); }
    // Compare the content of get_selected_preset() with get_edited_preset() configs, return the list of keys where they differ.
    std::vector<std::string>    current_different_from_parent_options(const bool deep_compare = false) const
        { return dirty_options(&this->get_edited_preset(), this->get_selected_preset_parent(), deep_compare); }

    // Compare the content of get_saved_preset() with get_edited_preset() configs, return true if they differ.
    bool                        saved_is_dirty() const 
        { return is_dirty(&this->get_edited_preset(), &m_saved_preset); }
    // Compare the content of get_saved_preset() with get_edited_preset() configs, return the list of keys where they differ.
//    std::vector<std::string>    saved_dirty_options() const
//        { return dirty_options(&this->get_edited_preset(), &this->get_saved_preset(), /* deep_compare */ false); }
    // Copy edited preset into saved preset.
    void                        update_saved_preset_from_current_preset() { m_saved_preset = m_edited_preset; }

    // Return a sorted list of system preset names.
    // Used for validating the "inherits" flag when importing user's config bundles.
    // Returns names of all system presets including the former names of these presets.
    std::vector<std::string>    system_preset_names() const;

    // Update a dirty flag of the current preset
    // Return true if the dirty flag changed.
    bool            update_dirty();
    
    // Select a profile by its name. Return true if the selection changed.
    // Without force, the selection is only updated if the index changes.
    // With force, the changes are reverted if the new index is the same as the old index.
    // With force_invisible, force preset selection even it's invisible.
    bool            select_preset_by_name(const std::string &name, bool force, bool force_invisible = false);

    // Generate a file path from a profile name. Add the ".ini" suffix if it is missing.
    std::string     path_from_name(const std::string &new_name) const;

    size_t num_default_presets() const { return m_num_default_presets; }

protected:
    PresetCollection() = default;
    // Copy constructor and copy operators are not to be used from outside PresetBundle,
    // as the Profile::vendor points to an instance of VendorProfile stored at parent PresetBundle!
    PresetCollection(const PresetCollection &other) = default;
    PresetCollection& operator=(const PresetCollection &other) = default;
    // After copying a collection with the default operators above, call this function
    // to adjust Profile::vendor pointers.
    void            update_vendor_ptrs_after_copy(const VendorMap &vendors);

    // Select a preset, if it exists. If it does not exist, select an invalid (-1) index.
    // This is a temporary state, which shall be fixed immediately by the following step.
    bool            select_preset_by_name_strict(const std::string &name);

    // Merge one vendor's presets with the other vendor's presets, report duplicates.
    std::vector<std::string> merge_presets(PresetCollection &&other, const VendorMap &new_vendors);

    // Update m_map_alias_to_profile_name from loaded system profiles.
	void 			update_map_alias_to_profile_name();

    // Update m_map_system_profile_renamed from loaded system profiles.
    void 			update_map_system_profile_renamed();

private:
    // Find a preset position in the sorted list of presets.
    // The "-- default -- " preset is always the first, so it needs
    // to be handled differently.
    // If a preset does not exist, an iterator is returned indicating where to insert a preset with the same name.
    std::deque<Preset>::iterator find_preset_internal(const std::string &name)
    {
        auto it = Slic3r::lower_bound_by_predicate(m_presets.begin() + m_num_default_presets, m_presets.end(), [&name](const auto& l) { return l.name < name;  });
        if (it == m_presets.end() || it->name != name) {
            // Preset has not been not found in the sorted list of non-default presets. Try the defaults.
            for (size_t i = 0; i < m_num_default_presets; ++ i)
                if (m_presets[i].name == name) {
                    it = m_presets.begin() + i;
                    break;
                }
        }
        return it;
    }
    std::deque<Preset>::const_iterator find_preset_internal(const std::string &name) const
        { return const_cast<PresetCollection*>(this)->find_preset_internal(name); }
    std::deque<Preset>::iterator 	   find_preset_renamed(const std::string &name) {
    	auto it_renamed = m_map_system_profile_renamed.find(name);
    	auto it = (it_renamed == m_map_system_profile_renamed.end()) ? m_presets.end() : this->find_preset_internal(it_renamed->second);
    	assert((it_renamed == m_map_system_profile_renamed.end()) || (it != m_presets.end() && it->name == it_renamed->second));
    	return it;
    }
    std::deque<Preset>::const_iterator find_preset_renamed(const std::string &name) const
        { return const_cast<PresetCollection*>(this)->find_preset_renamed(name); }

    size_t update_compatible_internal(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType unselect_if_incompatible);
public:
    static bool                     is_dirty(const Preset *edited, const Preset *reference);
    static std::vector<std::string> dirty_options(const Preset *edited, const Preset *reference, const bool deep_compare = false);
    static bool                     is_single_input_vector_option(const std::string& opt_key);

    const std::vector<std::pair<std::string, std::string>>& map_alias_to_profile_name() { return m_map_alias_to_profile_name; }
private:
    // Type of this PresetCollection: TYPE_SLA_PRINT, TYPE_SLA_MATERIAL or TYPE_PRINTER.
    Preset::Type            m_type;
    // List of presets, starting with the "- default -" preset.
    // Use deque to force the container to allocate an object per each entry, 
    // so that the addresses of the presets don't change during resizing of the container.
    std::deque<Preset>      m_presets;
    // System profiles may have aliases. Map to the full profile name.
    std::vector<std::pair<std::string, std::string>> m_map_alias_to_profile_name;
    // Map from old system profile name to a current system profile name.
    std::map<std::string, std::string> m_map_system_profile_renamed;
    // Initially this preset contains a copy of the selected preset. Later on, this copy may be modified by the user.
    Preset                  m_edited_preset;
    // Contains a copy of the last saved selected preset.
    Preset                  m_saved_preset;
    RenameHook              m_rename_hook;

    // Selected preset.
    size_t                  m_idx_selected;
    // Is the "- default -" preset suppressed?
    bool                    m_default_suppressed  = true;
    size_t                  m_num_default_presets = 0;

    // Path to the directory to store the config files into.
    std::string             m_dir_path;

    // to access select_preset_by_name_strict() and the default & copy constructors.
    friend class PresetBundle;
};

// The printer collection has a single "- default SLA -" preset (index 0).
class PrinterPresetCollection : public PresetCollection
{
public:
    PrinterPresetCollection(Preset::Type type, const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &default_name = "- default -") :
		PresetCollection(type, keys, defaults, default_name) {}

    const Preset*   find_system_preset_by_model_and_variant(const std::string &model_id, const std::string &variant) const;

    bool            only_default_printers() const;
private:
    PrinterPresetCollection() = default;
    PrinterPresetCollection(const PrinterPresetCollection &other) = default;
    PrinterPresetCollection& operator=(const PrinterPresetCollection &other) = default;

    friend class PresetBundle;
};

namespace PresetUtils {
	// PrinterModel of a system profile, from which this preset is derived, or null if it is not derived from a system profile.
	const VendorProfile::PrinterModel* system_printer_model(const Preset &preset);
    std::string system_printer_bed_model(const Preset& preset);
    std::string system_printer_bed_texture(const Preset& preset);
    bool        vendor_profile_has_all_resources(const VendorProfile& vp);
    bool        compare_vendor_profile_printers(const VendorProfile& vp_old, const VendorProfile& vp_new, std::vector<std::string>& new_printers);
} // namespace PresetUtils


} // namespace Slic3r

#endif /* slic3r_Preset_hpp_ */
