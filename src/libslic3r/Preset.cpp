///|/ Copyright (c) Prusa Research 2017 - 2023 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Tomáš Mészáros @tamasmeszaros, Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, David Kocík @kocikdav, Enrico Turri @enricoturri1966, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2021 Martin Budden
///|/ Copyright (c) 2021 Ilya @xorza
///|/ Copyright (c) 2019 John Drake @foxox
///|/ Copyright (c) 2018 Martin Loidl @LoidlM
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <cassert>

#include "Exception.hpp"
#include "Preset.hpp"
#include "AppConfig.hpp"
#include "SLA/ProjectSettings.hpp"
#include "I18N.hpp"

#ifdef _MSC_VER
    #define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
    #define NOMINMAX
#endif
    #include <Windows.h>
#endif /* _MSC_VER */


#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <boost/range/join.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/locale.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r.h"
#include "Utils.hpp"
#include "PlaceholderParser.hpp"
#include "libslic3r/Thumbnails/Thumbnails.hpp"

#include "PresetBundle.hpp"
#include "SLA/PrinterGeometry.hpp"

using boost::property_tree::ptree;

namespace Slic3r {

ConfigFileType guess_config_file_type(const ptree &tree)
{
    size_t app_config   = 0;
    size_t bundle       = 0;
    size_t config       = 0;
    for (const ptree::value_type &v : tree) {
        if (v.second.empty()) {
            if (v.first == "background_processing" ||
                v.first == "last_output_path" ||
                v.first == "no_controller" ||
                v.first == "no_defaults")
                ++ app_config;
            else if (v.first == "nozzle_diameter" ||
                v.first == "filament_diameter")
                ++ config;
        } else if (boost::algorithm::starts_with(v.first, "print:") ||
            boost::algorithm::starts_with(v.first, "filament:") ||
            boost::algorithm::starts_with(v.first, "printer:") ||
            v.first == "settings")
            ++ bundle;
        else if (v.first == "presets") {
            ++ app_config;
            ++ bundle;
        } else if (v.first == "recent") {
            for (auto &kvp : v.second)
                if (kvp.first == "config_directory" || kvp.first == "skein_directory")
                    ++ app_config;
        }
    }
    return (app_config > bundle && app_config > config) ? CONFIG_FILE_TYPE_APP_CONFIG :
           (bundle > config) ? CONFIG_FILE_TYPE_CONFIG_BUNDLE : CONFIG_FILE_TYPE_CONFIG;
}


VendorProfile VendorProfile::from_ini(const boost::filesystem::path &path, bool load_all)
{
    ptree tree;
    boost::nowide::ifstream ifs(path.string());
    boost::property_tree::read_ini(ifs, tree);
    return VendorProfile::from_ini(tree, path, load_all);
}

static const std::unordered_map<std::string, std::string> pre_family_model_map {{
    { "SL1",        "SL1" },
}};

VendorProfile VendorProfile::from_ini(const ptree &tree, const boost::filesystem::path &path, bool load_all)
{
    static const std::string printer_model_key = "printer_model:";
    static const std::string materials_section = "default_sla_materials";

    const std::string id = path.stem().string();

    if (! boost::filesystem::exists(path)) {
        throw Slic3r::RuntimeError((boost::format("Cannot load Vendor Config Bundle `%1%`: File not found: `%2%`.") % id % path).str());
    }

    VendorProfile res(id);

    // Helper to get compulsory fields
    auto get_or_throw = [&](const ptree &tree, const std::string &key) -> ptree::const_assoc_iterator
    {
        auto res = tree.find(key);
        if (res == tree.not_found()) {
            throw Slic3r::RuntimeError((boost::format("Vendor Config Bundle `%1%` is not valid: Missing secion or key: `%2%`.") % id % key).str());
        }
        return res;
    };

    // Load the header
    const auto &vendor_section = get_or_throw(tree, "vendor")->second;
    res.name = get_or_throw(vendor_section, "name")->second.data();

    auto config_version_str = get_or_throw(vendor_section, "config_version")->second.data();
    auto config_version = Semver::parse(config_version_str);
    if (! config_version) {
        throw Slic3r::RuntimeError((boost::format("Vendor Config Bundle `%1%` is not valid: Cannot parse config_version: `%2%`.") % id % config_version_str).str());
    } else {
        res.config_version = std::move(*config_version);
    }

    // Load URLs
    const auto config_update_url = vendor_section.find("config_update_url");
    if (config_update_url != vendor_section.not_found()) {
        res.config_update_url = config_update_url->second.data();
    }

    const auto changelog_url = vendor_section.find("changelog_url");
    if (changelog_url != vendor_section.not_found()) {
        res.changelog_url = changelog_url->second.data();
    }

    const auto repo_id = vendor_section.find("repo_id");
    if (repo_id != vendor_section.not_found()) {
        res.repo_id = repo_id->second.data();
    } else {
        // For backward compatibility assume all profiles without repo_id are from "prod" repo
        // DK: "No, dont!"
        res.repo_id = "";
    }

    const auto repo_prefix = vendor_section.find("repo_prefix");
    if (repo_prefix != vendor_section.not_found()) {
         res.repo_prefix = repo_prefix->second.data();
    } else {
        res.repo_prefix = "";
    }

    if (! load_all) {
        return res;
    }

    // Load printer models
    for (auto &section : tree) {
        if (boost::starts_with(section.first, printer_model_key)) {
            VendorProfile::PrinterModel model;
            model.id = section.first.substr(printer_model_key.size());
            model.name = section.second.get<std::string>("name", model.id);

            const char *technology_fallback = boost::algorithm::starts_with(model.id, "SL") ? "SLA" : "FFF";

            auto technology_field = section.second.get<std::string>("technology", technology_fallback);
            if (is_legacy_fff_technology(technology_field)) {
                // FFF printer models of an old vendor bundle are not supported: ignore them, the SLA models stay.
                BOOST_LOG_TRIVIAL(info) << boost::format("Vendor bundle: `%1%`: ignoring FFF printer model `%2%` (SLA/MSLA-only application)") % id % model.id;
                continue;
            }
            if (! ConfigOptionEnum<PrinterTechnology>::from_string(technology_field, model.technology)) {
                BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: Invalid printer technology field: `%2%`") % id % technology_field;
                model.technology = ptSLA;
            }

            model.family = section.second.get<std::string>("family", std::string());
            if (model.family.empty() && res.name == "Prusa Research") {
                // If no family is specified, it can be inferred for known printers
                const auto from_pre_map = pre_family_model_map.find(model.id);
                if (from_pre_map != pre_family_model_map.end()) { model.family = from_pre_map->second; }
            }
#if 0
            // Remove SLA printers from the initial alpha.
            if (model.technology == ptSLA)
                continue;
#endif
            section.second.get<std::string>("variants", "");
            const auto variants_field = section.second.get<std::string>("variants", "");
            std::vector<std::string> variants;
            if (Slic3r::unescape_strings_cstyle(variants_field, variants)) {
                for (const std::string &variant_name : variants) {
                    if (model.variant(variant_name) == nullptr)
                        model.variants.emplace_back(VendorProfile::PrinterVariant(variant_name));
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: Malformed variants field: `%2%`") % id % variants_field;
            }
            const auto default_materials_field = section.second.get<std::string>("default_materials", "");
            if (Slic3r::unescape_strings_cstyle(default_materials_field, model.default_materials)) {
            	Slic3r::sort_remove_duplicates(model.default_materials);
            	if (! model.default_materials.empty() && model.default_materials.front().empty())
            		// An empty material was inserted into the list of default materials. Remove it.
            		model.default_materials.erase(model.default_materials.begin());
            } else {
                BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: Malformed default_materials field: `%2%`") % id % default_materials_field;
            }
            model.bed_model   = section.second.get<std::string>("bed_model", "");
            model.bed_texture = section.second.get<std::string>("bed_texture", "");
            model.thumbnail   = section.second.get<std::string>("thumbnail", "");
            if (model.thumbnail.empty())
                model.thumbnail = model.id + "_thumbnail.png";

            if (! model.id.empty() && ! model.variants.empty())
                res.models.push_back(std::move(model));
        }
    }

    // Load sla materials to be installed by default (the [default_filaments] section of old FFF bundles is ignored).
    const auto materials = tree.find(materials_section);
    if (materials != tree.not_found()) {
        for (auto &pair : materials->second) {
            if (pair.second.data() == "1") {
                res.default_sla_materials.insert(pair.first);
            }
        }
    }

    return res;
}

std::vector<std::string> VendorProfile::families() const
{
    std::vector<std::string> res;
    unsigned num_familiies = 0;

    for (auto &model : models) {
        if (std::find(res.begin(), res.end(), model.family) == res.end()) {
            res.push_back(model.family);
            num_familiies++;
        }
    }

    return res;
}

// Suffix to be added to a modified preset name in the combo box.
static std::string g_suffix_modified = " (modified)";
const std::string& Preset::suffix_modified()
{
    return g_suffix_modified;
}

void Preset::update_suffix_modified(const std::string& new_suffix_modified)
{
    g_suffix_modified = new_suffix_modified;
}
// Remove an optional "(modified)" suffix from a name.
// This converts a UI name to a unique preset identifier.
std::string Preset::remove_suffix_modified(const std::string &name)
{
    return boost::algorithm::ends_with(name, g_suffix_modified) ?
        name.substr(0, name.size() - g_suffix_modified.size()) :
        name;
}

// Normalize a loaded SLA profile (legacy keys, bed shape derived from the display).
void Preset::normalize(DynamicPrintConfig &config)
{
    handle_legacy_sla(config);
    sla::synchronize_bed_shape(config);
}

std::string Preset::remove_invalid_keys(DynamicPrintConfig &config, const DynamicPrintConfig &default_config)
{
    std::string incorrect_keys;
    for (const std::string &key : config.keys())
        if (! default_config.has(key)) {
            if (incorrect_keys.empty())
                incorrect_keys = key;
            else {
                incorrect_keys += ", ";
                incorrect_keys += key;
            }
            config.erase(key);
        }
    return incorrect_keys;
}

void Preset::save()
{
    this->config.save(this->file);
}

// Return a label of this preset, consisting of a name and a "(modified)" suffix, if this preset is dirty.
std::string Preset::label() const
{
    return this->name + (this->is_dirty ? g_suffix_modified : "");
}

namespace {
const std::string &compatibility_identity(const Preset &preset)
{
    const char *key=nullptr;
    if(preset.is_external) {
        if(preset.type==Preset::TYPE_SLA_PRINT) key="sla_print_settings_id";
        else if(preset.type==Preset::TYPE_PRINTER && preset.printer_technology()==ptSLA) key="printer_settings_id";
    }
    if(key) {
        const auto *identity=preset.config.option<ConfigOptionString>(key);
        if(identity && !identity->value.empty()) return identity->value;
    }
    return preset.name;
}
}

PresetCompatibilityResult evaluate_compatibility_with_print(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_print, const PresetWithVendorProfile &active_printer)
{
	if (preset.vendor != nullptr && preset.vendor != active_printer.vendor)
		// The current profile has a vendor assigned and it is different from the active print's vendor.
		return {false, {}};
    auto &condition             = preset.preset.compatible_prints_condition();
    auto *compatible_prints     = dynamic_cast<const ConfigOptionStrings*>(preset.preset.config.option("compatible_prints"));
    bool  has_compatible_prints = compatible_prints != nullptr && ! compatible_prints->values.empty();
    if (! has_compatible_prints && ! condition.empty()) {
        try {
            DynamicPrintConfig context;
            context.set_key_value("print_preset", new ConfigOptionString(compatibility_identity(active_print.preset)));
            return {PlaceholderParser::evaluate_boolean_expression(condition, active_print.preset.config, &context), {}};
        } catch (const std::runtime_error &err) {
            return {false, err.what()};
        }
    }
    return {preset.preset.is_default || compatibility_identity(active_print.preset).empty() || ! has_compatible_prints ||
        std::find(compatible_prints->values.begin(), compatible_prints->values.end(), compatibility_identity(active_print.preset)) !=
            compatible_prints->values.end(), {}};
}

PresetCompatibilityResult evaluate_compatibility_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer, const DynamicPrintConfig *extra_config)
{
	if (preset.vendor != nullptr && preset.vendor != active_printer.vendor)
		// The current profile has a vendor assigned and it is different from the active print's vendor.
		return {false, {}};
    auto &condition               = preset.preset.compatible_printers_condition();
    auto *compatible_printers     = dynamic_cast<const ConfigOptionStrings*>(preset.preset.config.option("compatible_printers"));
    bool  has_compatible_printers = compatible_printers != nullptr && ! compatible_printers->values.empty();
    if (! has_compatible_printers && ! condition.empty()) {
        try {
            return {PlaceholderParser::evaluate_boolean_expression(condition, active_printer.preset.config, extra_config), {}};
        } catch (const std::runtime_error &err) {
            return {false, err.what()};
        }
    }
    return {preset.preset.is_default || compatibility_identity(active_printer.preset).empty() || ! has_compatible_printers ||
        std::find(compatible_printers->values.begin(), compatible_printers->values.end(), compatibility_identity(active_printer.preset)) !=
            compatible_printers->values.end(), {}};
}

PresetCompatibilityResult evaluate_compatibility_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer)
{
    DynamicPrintConfig config;
    config.set_key_value("printer_preset", new ConfigOptionString(compatibility_identity(active_printer.preset)));
    return evaluate_compatibility_with_printer(preset, active_printer, &config);
}

bool is_compatible_with_print(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_print, const PresetWithVendorProfile &active_printer)
{
    return evaluate_compatibility_with_print(preset, active_print, active_printer).compatible;
}

bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer, const DynamicPrintConfig *extra_config)
{
    return evaluate_compatibility_with_printer(preset, active_printer, extra_config).compatible;
}

bool is_compatible_with_printer(const PresetWithVendorProfile &preset, const PresetWithVendorProfile &active_printer)
{
    return evaluate_compatibility_with_printer(preset, active_printer).compatible;
}

void Preset::set_visible_from_appconfig(const AppConfig &app_config)
{
    if (vendor == nullptr) { return; }

    if (type == TYPE_PRINTER) {
        const std::string &model = config.opt_string("printer_model");
        const std::string &variant = config.opt_string("printer_variant");
        if (model.empty() || variant.empty())
        	return;
        is_visible = app_config.get_variant(vendor->id, model, variant);
    } else if (type == TYPE_SLA_MATERIAL) {
    	const std::string &section_name = AppConfig::SECTION_MATERIALS;
    	if (app_config.has_section(section_name)) {
    		// Check whether this profile is marked as "installed" in PrusaSlicer.ini,
    		// or whether a profile is marked as "installed", which this profile may have been renamed from.
	    	const std::map<std::string, std::string> &installed = app_config.get_section(section_name);
	    	auto has = [&installed](const std::string &name) {
	    		auto it = installed.find(name);
				return it != installed.end() && ! it->second.empty();
	    	};
	    	is_visible = has(this->name);
	    	for (auto it = this->renamed_from.begin(); ! is_visible && it != this->renamed_from.end(); ++ it)
	    		is_visible = has(*it);
	    }
        else 
			is_visible = false;
    }
}

std::string Preset::trim_vendor_repo_prefix(const std::string& id) const
{
    return Preset::trim_vendor_repo_prefix(id, this->vendor);
}
std::string Preset::trim_vendor_repo_prefix(const std::string& id, const VendorProfile* vendor_profile) const
{
    if (!vendor_profile) {
        return id;
    }
    std::string res = id;
    if (boost::algorithm::starts_with(res, vendor_profile->repo_prefix)) {
        boost::algorithm::erase_head(res, vendor_profile->repo_prefix.size());
        boost::algorithm::trim_left(res);
    }
    return res;
}

static std::vector<std::string> s_Preset_sla_print_options {
    "layer_height",
    "faded_layers",
    "supports_enable",
    "support_tree_type",

    "support_head_front_diameter",
    "support_head_penetration",
    "support_head_width",
    "support_pillar_diameter",
    "support_small_pillar_diameter_percent",
    "support_max_bridges_on_pillar",
    "support_max_weight_on_model",
    "support_pillar_connection_mode",
    "support_buildplate_only",
    "support_enforcers_only",
    "support_pillar_widening_factor",
    "support_base_diameter",
    "support_base_height",
    "support_base_safety_distance",
    "support_critical_angle",
    "support_max_bridge_length",
    "support_max_pillar_link_distance",
    "support_object_elevation",

    "branchingsupport_head_front_diameter",
    "branchingsupport_head_penetration",
    "branchingsupport_head_width",
    "branchingsupport_pillar_diameter",
    "branchingsupport_small_pillar_diameter_percent",
    "branchingsupport_max_bridges_on_pillar",
    "branchingsupport_max_weight_on_model",
    "branchingsupport_pillar_connection_mode",
    "branchingsupport_buildplate_only",
    "branchingsupport_pillar_widening_factor",
    "branchingsupport_base_diameter",
    "branchingsupport_base_height",
    "branchingsupport_base_safety_distance",
    "branchingsupport_critical_angle",
    "branchingsupport_max_bridge_length",
    "branchingsupport_max_pillar_link_distance",
    "branchingsupport_object_elevation",

    "support_points_density_relative",
    "slice_closing_radius",
    "slicing_mode",
    "pad_enable",
    "pad_wall_thickness",
    "pad_wall_height",
    "pad_brim_size",
    "pad_max_merge_distance",
    // "pad_edge_radius",
    "pad_wall_slope",
    "pad_object_gap",
    "pad_around_object",
    "pad_around_object_everywhere",
    "pad_object_connector_stride",
    "pad_object_connector_width",
    "pad_object_connector_penetration",
    "hollowing_enable",
    "hollowing_min_thickness",
    "hollowing_quality",
    "hollowing_closing_distance",
    "output_filename_format",
    "default_sla_print_profile",
    "compatible_printers",
    "compatible_printers_condition",
    "inherits"
};

static std::vector<std::string> s_Preset_sla_material_options {
    "material_colour",
    "material_type",
    "initial_layer_height",
    "bottle_cost",
    "bottle_volume",
    "bottle_weight",
    "material_density",
    "exposure_time",
    "initial_exposure_time",
    "material_correction",
    "material_correction_x",
    "material_correction_y",
    "material_correction_z",
    "material_notes",
    "material_vendor",
    "material_print_speed",
    "area_fill",
    "default_sla_material_profile",
    "zcorrection_layers",
    "compatible_prints", "compatible_prints_condition",
    "compatible_printers", "compatible_printers_condition", "inherits",

    // overriden options
    "material_ow_support_head_front_diameter",
    "material_ow_support_head_penetration",
    "material_ow_support_head_width",
    "material_ow_support_pillar_diameter",

    "material_ow_branchingsupport_head_front_diameter",
    "material_ow_branchingsupport_head_penetration",
    "material_ow_branchingsupport_head_width",
    "material_ow_branchingsupport_pillar_diameter",

    "material_ow_support_points_density_relative",
    "material_ow_absolute_correction",
    "material_ow_elefant_foot_compensation"
};

static std::vector<std::string> s_Preset_sla_tilt_options{
     "delay_before_exposure"
    ,"delay_after_exposure"
    ,"tower_hop_height"
    ,"tower_speed"
    ,"use_tilt"
    ,"tilt_down_initial_speed"
    ,"tilt_down_offset_steps"
    ,"tilt_down_offset_delay"
    ,"tilt_down_finish_speed"
    ,"tilt_down_cycles"
    ,"tilt_down_delay"
    ,"tilt_up_initial_speed"
    ,"tilt_up_offset_steps"
    ,"tilt_up_offset_delay"
    ,"tilt_up_finish_speed"
    ,"tilt_up_cycles"
    ,"tilt_up_delay"
};
const std::vector<std::string>& tilt_options() { return s_Preset_sla_tilt_options; }

static std::vector<std::string> s_Preset_sla_material_options_all = [] {
    auto keys = boost::copy_range<std::vector<std::string>>(boost::join(s_Preset_sla_material_options, s_Preset_sla_tilt_options));
    for (const auto &setting : sla::machine_settings) keys.emplace_back(setting.key);
    return keys;
}();

static std::vector<std::string> s_Preset_sla_printer_options {
    "printer_technology",
    "bed_shape", "bed_custom_texture", "bed_custom_model", "max_print_height",
    "display_width", "display_height", "display_pixels_x", "display_pixels_y",
    "display_mirror_x", "display_mirror_y",
    "display_orientation",
    "fast_tilt_time", "slow_tilt_time", "high_viscosity_tilt_time", //"area_fill",
    "relative_correction",
    "relative_correction_x",
    "relative_correction_y",
    "relative_correction_z",
    "absolute_correction",
    "elefant_foot_compensation",
    "elefant_foot_min_width",
    "gamma_correction",
    "min_exposure_time", "max_exposure_time",
    "min_initial_exposure_time", "max_initial_exposure_time", "sla_archive_format", "sla_anycubic_version", "sla_cbddlp_aa", "sla_archive_header_mirror", "sla_phz_encryption_key", "nanodlp_device_profile", "sla_output_precision",
    "printer_notes",
    "inherits"
};

const std::vector<std::string>& Preset::sla_print_options()      { return s_Preset_sla_print_options; }
const std::vector<std::string>& Preset::sla_material_options()   { return s_Preset_sla_material_options_all; }
const std::vector<std::string>& Preset::sla_printer_options()    { return s_Preset_sla_printer_options; }

const std::vector<std::string>& Preset::sla_hidden_option_keys()
{
    static const std::vector<std::string> s_keys {
        "printer_technology",               // fixed by the Tab type, never user-editable
        "bed_shape",                        // derived from display_width/height, an editor would let scene and raster disagree
        "inherits",                         // set by the preset machinery (parent preset)
        // Dependency widgets are created with custom widgets, not option lines.
        "compatible_printers", "compatible_printers_condition",
        "compatible_prints", "compatible_prints_condition",
        // Written by vendor profiles, shown only as a description line.
        "default_sla_print_profile", "default_sla_material_profile"
    };
    return s_keys;
}

PresetCollection::PresetCollection(Preset::Type type, const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &default_name) :
    m_type(type),
    m_edited_preset(type, "", false),
    m_saved_preset(type, "", false),
    m_idx_selected(0)
{
    // Insert just the default preset.
    this->add_default_preset(keys, defaults, default_name);
    m_edited_preset.config.apply(m_presets.front().config);
    update_saved_preset_from_current_preset();
}

void PresetCollection::reset(bool delete_files)
{
    if (m_presets.size() > m_num_default_presets) {
        if (delete_files) {
            // Erase the preset files.
            for (Preset &preset : m_presets)
                if (! preset.is_default && ! preset.is_external && ! preset.is_system)
                    boost::nowide::remove(preset.file.c_str());
        }
        // Don't use m_presets.resize() here as it requires a default constructor for Preset.
        m_presets.erase(m_presets.begin() + m_num_default_presets, m_presets.end());
        this->select_preset(0);
    }
    m_map_alias_to_profile_name.clear();
    m_map_system_profile_renamed.clear();
}

void PresetCollection::add_default_preset(const std::vector<std::string> &keys, const Slic3r::StaticPrintConfig &defaults, const std::string &preset_name)
{
    // Insert just the default preset.
    m_presets.emplace_back(Preset(this->type(), preset_name, true));
    m_presets.back().config.apply_only(defaults, keys.empty() ? defaults.keys() : keys);
    m_presets.back().loaded = true;
    ++ m_num_default_presets;
}

// Load all presets found in dir_path.
// Throws an exception on error.
void PresetCollection::load_presets(
    const std::string &dir_path, const std::string &subdir, 
    PresetsConfigSubstitutions& substitutions, ForwardCompatibilitySubstitutionRule substitution_rule)
{
    // Don't use boost::filesystem::canonical() on Windows, it is broken in regard to reparse points,
    // see https://github.com/prusa3d/PrusaSlicer/issues/732
    boost::filesystem::path dir = boost::filesystem::absolute(boost::filesystem::path(dir_path) / subdir).make_preferred();
    m_dir_path = dir.string();
    std::string errors_cummulative;
    // Store the loaded presets into a new vector, otherwise the binary search for already existing presets would be broken.
    // (see the "Preset already present, not loading" message).
    std::deque<Preset> presets_loaded;
    for (auto &dir_entry : boost::filesystem::directory_iterator(dir))
        if (Slic3r::is_ini_file(dir_entry)) {
            std::string name = dir_entry.path().filename().string();
            // Remove the .ini suffix.
            name.erase(name.size() - 4);
            if (this->find_preset(name, false)) {
                // This happens when there's is a preset (most likely legacy one) with the same name as a system preset
                // that's already been loaded from a bundle.
                BOOST_LOG_TRIVIAL(warning) << "Preset already present, not loading: " << name;
                continue;
            }
            try {
                Preset preset(m_type, name, false);
                preset.file = dir_entry.path().string();
                // Load the preset file, apply preset values on top of defaults.
                try {
                    DynamicPrintConfig config;
                    ConfigSubstitutions config_substitutions;
                    try {
                        config_substitutions = config.load_from_ini(preset.file, substitution_rule);
                    } catch (const FffConfigurationException &) {
                        // "printer_technology = FFF": a leftover of an FFF-capable version. Ignore it (no dialog, no data loss:
                        // the file stays on disk untouched).
                        BOOST_LOG_TRIVIAL(info) << "Ignoring FFF preset in the SLA/MSLA-only application: " << preset.file;
                        continue;
                    }
                    if (! config_substitutions.empty())
                        substitutions.push_back({ preset.name, m_type, PresetConfigSubstitutions::Source::UserFile, preset.file, std::move(config_substitutions) });
                    // A legacy FFF printer profile (or one without printer_technology, which has always
                    // meant FFF) cannot be represented in the SLA-only schema. Skip it explicitly instead of
                    // grafting it onto the SLA default and silently dropping its nozzle/extruder keys.
                    if (m_type == Preset::TYPE_PRINTER) {
                        const ConfigOption *technology = config.option("printer_technology");
                        if (technology == nullptr || technology->getInt() != ptSLA) {
                            BOOST_LOG_TRIVIAL(info) << "Skipping non-SLA printer profile in the SLA-only build: " << preset.file;
                            continue;
                        }
                    }
                    // Find a default preset for the config. The PrintPresetCollection provides different default preset based on the "printer_technology" field.
                    const Preset &default_preset = this->default_preset_for(config);
                    preset.config = default_preset.config;
                    preset.config.apply(std::move(config));
                    Preset::normalize(preset.config);
                    // Report configuration fields, which are misplaced into a wrong group.
                    std::string incorrect_keys = Preset::remove_invalid_keys(preset.config, default_preset.config);
                    if (! incorrect_keys.empty())
                        BOOST_LOG_TRIVIAL(error) << "Error in a preset file: The preset \"" <<
                            preset.file << "\" contains the following incorrect keys: " << incorrect_keys << ", which were removed";
                    preset.loaded = true;
                } catch (const std::ifstream::failure &err) {
                    throw Slic3r::RuntimeError(std::string("The selected preset cannot be loaded: ") + preset.file + "\n\tReason: " + err.what());
                } catch (const std::runtime_error &err) {
                    throw Slic3r::RuntimeError(std::string("Failed loading the preset file: ") + preset.file + "\n\tReason: " + err.what());
                }
                presets_loaded.emplace_back(preset);
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
            }
        }
    m_presets.insert(m_presets.end(), std::make_move_iterator(presets_loaded.begin()), std::make_move_iterator(presets_loaded.end()));
    std::sort(m_presets.begin() + m_num_default_presets, m_presets.end());
    this->select_preset(first_visible_idx());
    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);
}

// Load a preset from an already parsed config file, insert it into the sorted sequence of presets
// and select it, losing previous modifications.
Preset& PresetCollection::load_preset(const std::string &path, const std::string &name, const DynamicPrintConfig &config, bool select)
{
    DynamicPrintConfig cfg(this->default_preset_for(config).config);
    cfg.apply_only(config, cfg.keys(), true);
    return this->load_preset(path, name, std::move(cfg), select);
}

static bool sla_preset_import(Preset::Type type, const DynamicPrintConfig &configuration)
{
    return type==Preset::TYPE_SLA_PRINT || type==Preset::TYPE_SLA_MATERIAL ||
        (type==Preset::TYPE_PRINTER && Preset::printer_technology(configuration)==ptSLA);
}

static bool profile_import_matches(const DynamicPrintConfig &cfg_old, const DynamicPrintConfig &cfg_new, Preset::Type type)
{
    t_config_option_keys diff = cfg_old.diff(cfg_new);
    // SLA project metadata belongs to the imported preset contract even when
    // raster settings match. Preserve the legacy comparison policy.
    const bool sla_import=sla_preset_import(type,cfg_new);
    for (const char *key : { "compatible_prints", "compatible_prints_condition",
                             "compatible_printers", "compatible_printers_condition", "inherits",
                             "sla_print_settings_id", "sla_material_settings_id", "printer_settings_id",
                             "printer_model", "printer_variant", "default_sla_print_profile", "default_sla_material_profile" })
        if(!sla_import || (!sla::is_sla_preset_metadata_key(key) && std::string(key)!="printer_model"))
            diff.erase(std::remove(diff.begin(), diff.end(), key), diff.end());
    return diff.empty();
}

// Load a preset from an already parsed config file, insert it into the sorted sequence of presets
// and select it, losing previous modifications.
std::string PresetCollection::get_selected_preset_source_name(const char *settings_id) const
{
    if (m_idx_selected == size_t(-1)) return {};
    const auto &preset = get_edited_preset();
    if (preset.is_external) {
        const auto *source = preset.config.option<ConfigOptionString>(settings_id);
        if (source && !source->value.empty()) return source->value;
    }
    return get_selected_preset_name();
}

ExternalPreset PresetCollection::load_external_preset(
    // Path to the profile source file (an AMF or 3MF file, a config file)
    const std::string           &path,
    // Name of the profile, derived from the source file name.
    const std::string           &name,
    // Original name of the profile, extracted from the loaded config. Empty, if the name has not been stored.
    const std::string           &original_name,
    // Config to initialize the preset from. It may contain configs of all presets merged in a single dictionary!
    const DynamicPrintConfig    &combined_config,
    // Select the preset after loading?
    LoadAndSelect                select)
{
    // Load the preset over a default preset, so that the missing fields are filled in from the default preset.
    DynamicPrintConfig cfg(this->default_preset_for(combined_config).config);
    t_config_option_keys keys = cfg.keys();
    cfg.apply_only(combined_config, keys, true);
    std::string                 &inherits = Preset::inherits(cfg);
    const bool sla_import=sla_preset_import(m_type,cfg);
    if (select == LoadAndSelect::Never) {
        // Some profile has been selected and modified already.
        // Check whether this profile is equal to the modified edited profile.
        const Preset &edited = this->get_edited_preset();
        if ((edited.name == original_name || edited.name == inherits) && profile_import_matches(edited.config, cfg,m_type))
            // Just point to that already selected and edited profile.
            return ExternalPreset(&(*this->find_preset_internal(edited.name)), false);
    }
    // Is there a preset already loaded with the name stored inside the config?
    std::deque<Preset>::iterator it       = this->find_preset_internal(original_name);
    bool                         found    = it != m_presets.end() && it->name == original_name;
    if (! found) {
    	// Try to match the original_name against the "renamed_from" profile names of loaded system profiles.
		it = this->find_preset_renamed(original_name);
		found = it != m_presets.end();
    }
    // A same-name legacy FFF profile cannot provide an SLA import baseline.
    if(found && sla_import && m_type==Preset::TYPE_PRINTER && it->printer_technology()!=ptSLA) {
        it=m_presets.end(); found=false;
    }
    if (found && profile_import_matches(it->config, cfg,m_type) && it->is_visible) {
        // The preset exists and is visible and it matches the values stored inside config.
        if (select == LoadAndSelect::Always)
            this->select_preset(it - m_presets.begin());
        return ExternalPreset(&(*it), false);
    }
    if (! found && select != LoadAndSelect::Never && ! inherits.empty()) {
        // Try to use a system profile as a base to select the system profile
        // and override its settings with the loaded ones.
        assert(it == m_presets.end());
        it    = this->find_preset_internal(inherits);
        found = it != m_presets.end() && it->name == inherits;
        // A resolved child remains a project child when its installed parent
        // has equal print values. Selecting the parent replaces the source ID
        // and changes compatibility and export preferences.
        if(found && sla_import && it->name!=original_name) {
            it=m_presets.end(); found=false;
        }
        if (found && profile_import_matches(it->config, cfg,m_type)) {
            // The system preset exists and it matches the values stored inside config.
            if (select == LoadAndSelect::Always)
                this->select_preset(it - m_presets.begin());
            return ExternalPreset(&(*it), false);
        }
    }
    if (found) {
        if (select != LoadAndSelect::Never) {
            const size_t idx = it - m_presets.begin();
            // The newly selected preset can be activated AND have to be make as visible.
            bool is_installed = !m_presets[idx].is_visible;

            // Select the existing preset and override it with new values, so that
            // the differences will be shown in the preset editor against the referenced profile.
            this->select_preset(idx);

            // update dirty state only if it's needed
            if (!profile_import_matches(it->config, cfg,m_type)) {
                // The source config may contain keys from many possible preset types. Just copy those that relate to this preset.

                // Following keys are not used neither by the UI nor by the slicing core, therefore they are not important 
                // Erase them from config apply to avoid redundant "dirty" parameter in loaded preset.
                for (const char* key : { "sla_print_settings_id", "sla_material_settings_id", "printer_settings_id",
                                         "printer_model", "printer_variant", "default_sla_print_profile", "default_sla_material_profile" })
                    if(!sla_import || (!sla::is_sla_preset_metadata_key(key) && std::string(key)!="printer_model"))
                        keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());

                this->get_edited_preset().config.apply_only(combined_config, keys, true);
                this->update_dirty();
                // Don't save the newly loaded project as a "saved into project" state.
                //update_saved_preset_from_current_preset();
                assert(this->get_edited_preset().is_dirty);
            }
            return ExternalPreset(&(*it), this->get_edited_preset().is_dirty, is_installed);
        }
        if (inherits.empty()) {
            // Update the "inherits" field.
            // There is a profile with the same name already loaded. Should we update the "inherits" field?
            inherits = it->vendor ? it->name : it->inherits();
        }
    }

    // The external preset does not match an internal preset, load the external preset.
    std::string new_name;
    for (size_t idx = 0;; ++ idx) {
        std::string suffix;
        if (original_name.empty()) {
            if (idx > 0)
                suffix = " (" + std::to_string(idx) + ")";
        } else {
            if (idx == 0)
                suffix = " (" + original_name + ")";
            else
                suffix = " (" + original_name + "-" + std::to_string(idx) + ")";
        }
        new_name = name + suffix;
        it = this->find_preset_internal(new_name);
        if (it == m_presets.end() || it->name != new_name)
            // Unique profile name. Insert a new profile.
            break;
        if (profile_import_matches(it->config, cfg,m_type)) {
            // The preset exists and it matches the values stored inside config.
            if (select == LoadAndSelect::Always)
                this->select_preset(it - m_presets.begin());
            return ExternalPreset(&(*it), false);
        }
        // Form another profile name.
    }
    // Insert a new profile.
    Preset &preset = this->load_preset(path, new_name, std::move(cfg), select == LoadAndSelect::Always);
    preset.is_external = true;
    if (this->m_idx_selected != size_t(-1) && &this->get_selected_preset() == &preset)
        this->get_edited_preset().is_external = true;

    return ExternalPreset(&preset, false);
}

Preset& PresetCollection::load_preset(const std::string &path, const std::string &name, DynamicPrintConfig &&config, bool select)
{
    auto it = this->find_preset_internal(name);
    if (it == m_presets.end() || it->name != name) {
        // The preset was not found. Create a new preset.
        it = m_presets.emplace(it, Preset(m_type, name, false));
    }
    Preset &preset = *it;
    preset.file = path;
    preset.config = std::move(config);
    preset.loaded = true;
    preset.is_dirty = false;
    if (select)
        this->select_preset_by_name(name, true);
    return preset;
}

namespace {
PresetFileTransaction::FaultInjector &fault_injector()
{
    static PresetFileTransaction::FaultInjector injector;
    return injector;
}

std::string read_regular_file(const boost::filesystem::path &path)
{
    if (boost::filesystem::is_symlink(path) || !boost::filesystem::is_regular_file(path))
        throw std::runtime_error("Not a regular file: " + path.string());
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad())
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}

// Write bytes to a fresh temp file next to `target`; returns its path. Removes the temp on failure.
std::string write_sibling_temp(const boost::filesystem::path &target, const std::string &bytes)
{
    const auto temp = target.parent_path() / boost::filesystem::unique_path(".sla-preset-%%%%-%%%%.tmp");
    {
        boost::nowide::ofstream stream(temp.string(), std::ios::out | std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), std::streamsize(bytes.size()));
        stream.close();
        if (!stream) {
            boost::system::error_code ignored;
            boost::filesystem::remove(temp, ignored);
            throw std::runtime_error("Cannot write " + temp.string());
        }
    }
    return temp.string();
}
} // namespace

void PresetFileTransaction::set_fault_injector(FaultInjector injector) { fault_injector() = std::move(injector); }

PresetFileTransaction::~PresetFileTransaction() { this->discard_temps(); }

void PresetFileTransaction::discard_temps() noexcept
{
    for (Change &change : m_changes)
        if (!change.temp.empty()) {
            boost::system::error_code ignored;
            boost::filesystem::remove(change.temp, ignored);
            change.temp.clear();
        }
}

void PresetFileTransaction::record_original(Change &change, bool must_not_exist)
{
    for (const Change &other : m_changes)
        if (other.path == change.path)
            throw std::runtime_error("The same preset file is changed twice: " + change.path);
    const boost::filesystem::path path(change.path);
    if (boost::filesystem::exists(path) || boost::filesystem::is_symlink(path)) {
        if (must_not_exist)
            throw std::runtime_error("A preset file already exists: " + change.path);
        change.original = read_regular_file(path);
        change.had_original = true;
    }
}

void PresetFileTransaction::stage_config(const std::string &path, const DynamicPrintConfig &config, bool must_not_exist)
{
    Change change;
    change.path = path;
    this->record_original(change, must_not_exist);
    // ConfigBase::save serializes to its own sibling and renames, so `temp` is complete or absent.
    const auto temp = boost::filesystem::path(path).parent_path() / boost::filesystem::unique_path(".sla-preset-%%%%-%%%%.tmp");
    config.save(temp.string());
    change.temp = temp.string();
    m_changes.push_back(std::move(change));
}

void PresetFileTransaction::stage_rename(const std::string &from, const std::string &to)
{
    Change created;
    created.path = to;
    this->record_original(created, true);
    created.temp = write_sibling_temp(boost::filesystem::path(to), read_regular_file(from));
    m_changes.push_back(std::move(created));
    this->stage_remove(from);
}

void PresetFileTransaction::stage_remove(const std::string &path)
{
    Change change;
    change.path = path;
    change.removal = true;
    this->record_original(change, false);
    if (!change.had_original)
        throw std::runtime_error("Cannot delete a missing preset file: " + path);
    m_changes.push_back(std::move(change));
}

void PresetFileTransaction::commit()
{
    if (m_done)
        throw std::logic_error("PresetFileTransaction committed twice");
    size_t published = 0;
    std::string failure;
    try {
        for (; published < m_changes.size(); ++published) {
            Change &change = m_changes[published];
            if (fault_injector()) fault_injector()(published, false);
            if (change.removal) {
                boost::system::error_code error;
                boost::filesystem::remove(change.path, error);
                if (error) throw std::runtime_error("Cannot delete " + change.path + ": " + error.message());
            } else {
                if (const auto error = rename_file(change.temp, change.path))
                    throw std::runtime_error("Cannot write " + change.path + ": " + error.message());
                change.temp.clear(); // now owned by the target
            }
        }
    } catch (const std::exception &error) {
        failure = error.what();
    } catch (...) {
        failure = "unknown error";
    }
    m_done = true;
    if (failure.empty())
        return;

    // Publication failed after `published` changes: restore them from the saved bytes, newest first.
    std::string unrestored;
    for (size_t i = published; i-- > 0;) {
        const Change &change = m_changes[i];
        try {
            if (fault_injector()) fault_injector()(i, true);
            if (change.had_original) {
                const std::string temp = write_sibling_temp(boost::filesystem::path(change.path), change.original);
                if (const auto error = rename_file(temp, change.path)) {
                    boost::system::error_code ignored;
                    boost::filesystem::remove(temp, ignored);
                    throw std::runtime_error(error.message());
                }
            } else {
                boost::system::error_code error;
                boost::filesystem::remove(change.path, error);
                if (error) throw std::runtime_error(error.message());
            }
        } catch (const std::exception &) {
            unrestored += "\n  " + change.path;
        }
    }
    this->discard_temps();
    if (unrestored.empty())
        throw std::runtime_error(failure + "\nNo profile was changed.");
    throw std::runtime_error(failure + "\nThe previous version of these profile files could not be restored:" + unrestored);
}

bool PresetCollection::save_current_preset(const std::string &new_name, bool detach)
{
    auto existing = this->find_preset_internal(new_name);
    const bool created = existing == m_presets.end() || existing->name != new_name;
    if (!created && (existing->is_default || existing->is_external || existing->is_system))
        return false;

    // Prepare and persist a candidate before changing the collection or moving
    // the edited config. Failure keeps selection, dirty edits and saved values.
    Preset candidate = created ? m_edited_preset : *existing;
    candidate.config = m_edited_preset.config;
    if (created) {
        const std::string parent_name = candidate.name;
        candidate.name = new_name;
        candidate.file = this->path_from_name(new_name);
        if (candidate.is_system && !detach) candidate.inherits() = parent_name;
        candidate.vendor = nullptr;
        candidate.alias.clear();
        candidate.renamed_from.clear();
        candidate.is_default = candidate.is_system = candidate.is_external = false;
    }
    if (detach) {
        candidate.vendor = nullptr;
        candidate.inherits().clear();
        candidate.alias.clear();
        candidate.renamed_from.clear();
    }
    candidate.is_visible = true;
    candidate.is_dirty = false;
    if (candidate.printer_technology() == ptSLA) Preset::normalize(candidate.config);
    candidate.save();
    if (created) m_presets.insert(existing, std::move(candidate));
    else *existing = std::move(candidate);
    this->select_preset_by_name(new_name, true);
    return created;
}

bool PresetCollection::rename_current_preset(const std::string &new_name)
{
    Preset &selected = this->get_selected_preset();
    if (selected.is_default || selected.is_system || selected.is_external)
        throw std::runtime_error("Only a saved user preset can be renamed");
    if (new_name == selected.name)
        return false;
    if (new_name.empty() || new_name.find_first_of("/\\") != std::string::npos)
        throw std::runtime_error("Invalid preset name");
    const std::string old_name = selected.name;
    const auto existing = this->find_preset_internal(new_name);
    const auto destination = boost::filesystem::path(this->path_from_name(new_name));
    if ((existing != m_presets.end() && existing->name == new_name) ||
        boost::filesystem::exists(destination) || boost::filesystem::is_symlink(destination))
        throw std::runtime_error("A preset file with this name already exists");
    if (!boost::filesystem::is_regular_file(selected.file))
        throw std::runtime_error("The selected preset file is missing or is not a regular file");

    // Stage every file change first (the renamed file, children of this collection, and referrers in other
    // collections through the hook). Nothing in memory changes until the whole set is published, so a
    // failure on the N-th dependent leaves disk and memory exactly as before.
    PresetFileTransaction transaction;
    transaction.stage_rename(selected.file, destination.string());
    const auto migrate_children = this->stage_user_preset_migration([&](DynamicPrintConfig &config) {
        auto *inherits = config.option<ConfigOptionString>("inherits", false);
        if (inherits == nullptr || inherits->value != old_name) return false;
        inherits->value = new_name;
        return true;
    }, transaction);
    std::function<void()> migrate_others;
    if (m_rename_hook.function)
        migrate_others = m_rename_hook.function(m_type, old_name, new_name, transaction);
    // Prepare allocations before the commit; the memory update below must not throw.
    std::string saved_name = new_name, edited_name = new_name;
    std::string saved_file = destination.string(), edited_file = saved_file;
    const bool rename_snapshot = m_saved_preset.name == selected.name;
    std::string snapshot_name = new_name, snapshot_file = saved_file;
    transaction.commit();

    // Renaming changes identity, not configuration: selected is the saved baseline, edited is the draft.
    selected.name.swap(saved_name);
    selected.file.swap(saved_file);
    m_edited_preset.name.swap(edited_name);
    m_edited_preset.file.swap(edited_file);
    if (rename_snapshot) {
        m_saved_preset.name.swap(snapshot_name);
        m_saved_preset.file.swap(snapshot_file);
    }
    std::sort(m_presets.begin() + m_num_default_presets, m_presets.end());
    m_idx_selected = this->find_preset_internal(new_name) - m_presets.begin();
    // select_preset() would overwrite the draft and clear every dirty flag.
    migrate_children();
    if (migrate_others)
        migrate_others();
    return true;
}

std::function<void()> PresetCollection::stage_user_preset_migration(const std::function<bool(DynamicPrintConfig&)> &edit, PresetFileTransaction &transaction)
{
    std::vector<std::string> changed;
    for (size_t idx = m_num_default_presets; idx < m_presets.size(); ++idx) {
        const Preset &preset = m_presets[idx];
        if (preset.is_default || preset.is_system || preset.is_external || preset.file.empty() || !preset.loaded)
            continue;
        DynamicPrintConfig candidate = preset.config;
        if (!edit(candidate))
            continue;
        transaction.stage_config(preset.file, candidate);
        changed.push_back(preset.name);
    }
    // The edit must be idempotent: it is re-applied to the in-memory configs after the commit.
    return [this, edit, changed = std::move(changed)]() {
        for (const std::string &name : changed) {
            const auto it = this->find_preset_internal(name);
            if (it == m_presets.end() || it->name != name)
                continue;
            edit(it->config);
            if (size_t(it - m_presets.begin()) == m_idx_selected) {
                edit(m_edited_preset.config);
                if (m_saved_preset.name == it->name)
                    edit(m_saved_preset.config);
            }
        }
        this->update_dirty();
    };
}

void PresetCollection::migrate_user_preset_configs(const std::function<bool(DynamicPrintConfig&)> &edit)
{
    PresetFileTransaction transaction;
    const auto apply = this->stage_user_preset_migration(edit, transaction);
    transaction.commit();
    apply();
}

std::vector<std::string> PresetCollection::user_presets_affected_by(const std::function<bool(DynamicPrintConfig&)> &edit) const
{
    std::vector<std::string> names;
    for (size_t idx = m_num_default_presets; idx < m_presets.size(); ++idx) {
        const Preset &preset = m_presets[idx];
        if (preset.is_default || preset.is_system || preset.is_external || preset.file.empty() || !preset.loaded)
            continue;
        DynamicPrintConfig candidate = preset.config;
        if (edit(candidate))
            names.push_back(preset.name);
    }
    return names;
}

bool PresetCollection::delete_preset_transactional(const std::string &name, const std::function<bool(DynamicPrintConfig&)> &flatten_children,
    const std::function<std::function<void()>(PresetFileTransaction&)> &stage_extra)
{
    const auto it = this->find_preset_internal(name);
    if (it == m_presets.end() || it->name != name)
        return false;
    if (it->is_default || it->is_system || it->is_external)
        throw std::runtime_error("Only a saved user preset can be deleted with its references");
    const size_t index = it - m_presets.begin();
    const std::string file = it->file;

    // One transaction: the preset file, its children and every referrer in other collections.
    PresetFileTransaction transaction;
    if (boost::filesystem::exists(file))
        transaction.stage_remove(file);
    const auto update_children = flatten_children ? this->stage_user_preset_migration(flatten_children, transaction) : std::function<void()>();
    const auto update_others = stage_extra ? stage_extra(transaction) : std::function<void()>();
    transaction.commit();

    if (update_children) update_children();
    if (update_others) update_others();
    m_presets.erase(m_presets.begin() + index);
    if (index == m_idx_selected) {
        size_t new_selected_idx = std::min(index, m_presets.size() - 1);
        for (; new_selected_idx < m_presets.size() && !m_presets[new_selected_idx].is_visible; ++new_selected_idx) ;
        if (new_selected_idx == m_presets.size())
            for (--new_selected_idx; new_selected_idx > 0 && !m_presets[new_selected_idx].is_visible; --new_selected_idx);
        this->select_preset(new_selected_idx);
    } else if (index < m_idx_selected) {
        // Keep the selection and its unsaved draft; only the index moved.
        --m_idx_selected;
    }
    return true;
}

Preset& PresetCollection::get_preset_with_name(const std::string& new_name, const Preset* initial_preset)
{
    // 1) Find the preset with a new_name or create a new one,
    // initialize it with the preset_to config.
    auto it = this->find_preset_internal(new_name);
    if (it != m_presets.end() && it->name == new_name) {
        // Preset with the same name found.
        Preset& preset = *it;
        if (!preset.is_default && !preset.is_external && !preset.is_system && initial_preset->name != new_name) {
            // Overwriting an existing preset if it isn't default/external/system or isn't an initial_preset
            preset.config = initial_preset->config;
            // The newly saved preset can be activated -> make it visible.
            preset.is_visible = true;
        }
        return preset;
    }

    const std::string selected_preset_name = this->get_selected_preset_name();

    // Creating a new preset.
    Preset& preset = *m_presets.insert(it, *initial_preset);
    std::string& inherits = preset.inherits();
    std::string old_name = preset.name;
    preset.name = new_name;
    preset.file = this->path_from_name(new_name);
    preset.vendor = nullptr;
    preset.alias.clear();
    preset.renamed_from.clear();
    if (preset.is_system) {
        // Inheriting from a system preset.
        inherits = old_name;
    }
    else if (inherits.empty()) {
        // Inheriting from a user preset. Link the new preset to the old preset.
        // inherits = old_name;
    }
    else {
        // Inherited from a user preset. Just maintain the "inherited" flag,
        // meaning it will inherit from either the system preset, or the inherited user preset.
    }
    preset.is_default = false;
    preset.is_system = false;
    preset.is_external = false;
    // The newly saved preset can be activated -> make it visible.
    preset.is_visible = true;
    // Just system presets have aliases
    preset.alias.clear();

    // sort printers and get new it
    std::sort(m_presets.begin(), m_presets.end());

    // set initial preset selection
    this->select_preset_by_name(selected_preset_name, true);

    it = this->find_preset_internal(new_name);
    assert(it != m_presets.end());

    return *it;
}

bool PresetCollection::delete_current_preset()
{
    const Preset &selected = this->get_selected_preset();
    if (selected.is_default)
        return false;
    if (! selected.is_external && ! selected.is_system) {
        boost::system::error_code error;
        boost::filesystem::remove(selected.file, error);
        if (error) throw FileIOError("Cannot delete preset '" + selected.name + "': " + error.message());
    }
    // Remove the preset from the list.
    m_presets.erase(m_presets.begin() + m_idx_selected);
    // Find the next visible preset.
    size_t new_selected_idx = m_idx_selected;
    if (new_selected_idx < m_presets.size())
        for (; new_selected_idx < m_presets.size() && ! m_presets[new_selected_idx].is_visible; ++ new_selected_idx) ;
    if (new_selected_idx == m_presets.size())
        for (--new_selected_idx; new_selected_idx > 0 && !m_presets[new_selected_idx].is_visible; --new_selected_idx);
    this->select_preset(new_selected_idx);
    return true;
}

bool PresetCollection::delete_preset(const std::string& name)
{
    if (name == this->get_selected_preset().name)
        return delete_current_preset();

    const std::string selected_preset_name = this->get_selected_preset_name();

    auto it = this->find_preset_internal(name);
    if (it == m_presets.end() || it->name != name) return false;

    const Preset& preset = *it;
    if (preset.is_default)
        return false;
    if (!preset.is_external && !preset.is_system) {
        boost::system::error_code error;
        boost::filesystem::remove(preset.file, error);
        if (error) throw FileIOError("Cannot delete preset '" + preset.name + "': " + error.message());
    }
    m_presets.erase(it);

    // update selected preset
    this->select_preset_by_name(selected_preset_name, true);

    return true;
}

const Preset* PresetCollection::get_selected_preset_parent() const
{
    if (this->get_selected_idx() == size_t(-1))
        // This preset collection has no preset activated yet. Only the get_edited_preset() is valid.
        return nullptr;

    const Preset 	  &selected_preset = this->get_selected_preset();
    if (selected_preset.is_system || selected_preset.is_default)
        return &selected_preset;

    const Preset 	  &edited_preset   = this->get_edited_preset();
    const std::string &inherits        = edited_preset.inherits();
    const Preset      *preset          = nullptr;
    if (inherits.empty()) {
        if (selected_preset.is_external)
            return nullptr;
        preset = &this->default_preset(m_type == Preset::Type::TYPE_PRINTER && edited_preset.printer_technology() == ptSLA ? 1 : 0);
    } else
        preset = this->find_preset(inherits, false);
    if (preset == nullptr) {
	    // Resolve the "renamed_from" field.
    	assert(! inherits.empty());
    	auto it = this->find_preset_renamed(inherits);
		if (it != m_presets.end())
			preset = &(*it);
    }
    return (preset == nullptr/* || preset->is_default*/ || preset->is_external) ? nullptr : preset;
}

const Preset* PresetCollection::get_preset_parent(const Preset& child) const
{
    const std::string &inherits = child.inherits();
    if (inherits.empty())
// 		return this->get_selected_preset().is_system ? &this->get_selected_preset() : nullptr;
        return nullptr;
    const Preset* preset = this->find_preset(inherits, false);
    if (preset == nullptr) {
    	auto it = this->find_preset_renamed(inherits);
		if (it != m_presets.end())
			preset = &(*it);
    }
    return
         // not found
        (preset == nullptr/* || preset->is_default */||
         // this should not happen, user profile should not derive from an external profile
         preset->is_external ||
         // this should not happen, however people are creative, see GH #4996
         preset == &child) ?
            nullptr :
            preset;
}

// Return vendor of the first parent profile, for which the vendor is defined, or null if such profile does not exist.
PresetWithVendorProfile PresetCollection::get_preset_with_vendor_profile(const Preset &preset) const
{
	const Preset		*p = &preset;
	const VendorProfile *v = nullptr;
	do {
		if (p->vendor != nullptr) {
			v = p->vendor;
			break;
		}
		p = this->get_preset_parent(*p);
	} while (p != nullptr);
	return PresetWithVendorProfile(preset, v);
}

const std::string& PresetCollection::get_preset_name_by_alias(const std::string& alias) const
{
	for (
		// Find the 1st profile name with the alias.
		auto it = Slic3r::lower_bound_by_predicate(m_map_alias_to_profile_name.begin(), m_map_alias_to_profile_name.end(), [&alias](auto &l){ return l.first < alias; });
		// Continue over all profile names with the same alias.
		it != m_map_alias_to_profile_name.end() && it->first == alias; ++ it)
		if (auto it_preset = this->find_preset_internal(it->second);
			it_preset != m_presets.end() && it_preset->name == it->second &&
            it_preset->is_visible && (it_preset->is_compatible || size_t(it_preset - m_presets.begin()) == m_idx_selected))
	        return it_preset->name;
    return alias;
}

const std::string& PresetCollection::get_preset_name_by_alias_invisible(const std::string& alias) const
{
    for (
        // Find the 1st profile name with the alias.
        auto it = Slic3r::lower_bound_by_predicate(m_map_alias_to_profile_name.begin(), m_map_alias_to_profile_name.end(), [&alias](auto& l) { return l.first < alias; });
        // Continue over all profile names with the same alias.
        it != m_map_alias_to_profile_name.end() && it->first == alias; ++it)
        if (auto it_preset = this->find_preset_internal(it->second);
            it_preset != m_presets.end() && it_preset->name == it->second &&
            it_preset->is_compatible)
            return it_preset->name;
    return alias;
}

const std::string* PresetCollection::get_preset_name_renamed(const std::string &old_name) const
{
	auto it_renamed = m_map_system_profile_renamed.find(old_name);
	if (it_renamed != m_map_system_profile_renamed.end())
		return &it_renamed->second;
	return nullptr;
}

const std::string& PresetCollection::get_suffix_modified() {
    return g_suffix_modified;
}

// Return a preset by its name. If the preset is active, a temporary copy is returned.
// If a preset is not found by its name, null is returned.
Preset* PresetCollection::find_preset(const std::string &name, bool first_visible_if_not_found, bool respect_active_preset /*= true*/)
{
    Preset key(m_type, name, false);
    auto it = this->find_preset_internal(name);
    // Ensure that a temporary copy is returned if the preset found is currently selected.
    return (it != m_presets.end() && it->name == key.name) ? &this->preset(it - m_presets.begin(), respect_active_preset) :
        first_visible_if_not_found ? &this->first_visible() : nullptr;
}

size_t PresetCollection::get_preset_idx_by_name(const std::string name) const
{
    auto it = this->find_preset_internal(name);
    return it != m_presets.end() ? it - m_presets.begin() : size_t(-1);
}

// Return index of the first visible preset. Certainly at least the '- default -' preset shall be visible.
size_t PresetCollection::first_visible_idx() const
{
    size_t idx = m_default_suppressed ? m_num_default_presets : 0;
    for (; idx < m_presets.size(); ++ idx)
        if (m_presets[idx].is_visible
            && (m_type != Preset::TYPE_PRINTER || m_presets[idx].printer_technology() == ptSLA)
        )
            break;
    if (idx == m_presets.size())
        idx = 0;
    return idx;
}

void PresetCollection::set_default_suppressed(bool default_suppressed)
{
    if (m_default_suppressed != default_suppressed) {
        m_default_suppressed = default_suppressed;
        bool default_visible = ! default_suppressed || m_idx_selected < m_num_default_presets;
        for (size_t i = 0; i < m_num_default_presets; ++ i)
            m_presets[i].is_visible = default_visible;
    }
}

size_t PresetCollection::update_compatible_internal(const PresetWithVendorProfile &active_printer, const PresetWithVendorProfile *active_print, PresetSelectCompatibleType unselect_if_incompatible)
{
    DynamicPrintConfig config;
    config.set_key_value("printer_preset", new ConfigOptionString(compatibility_identity(active_printer.preset)));
    bool some_compatible = false;
    for (size_t idx_preset = m_num_default_presets; idx_preset < m_presets.size(); ++ idx_preset) {
        bool    selected        = idx_preset == m_idx_selected;
        Preset &preset_selected = m_presets[idx_preset];
        Preset &preset_edited   = selected ? m_edited_preset : preset_selected;

        const PresetWithVendorProfile this_preset_with_vendor_profile = this->get_preset_with_vendor_profile(preset_edited);
        bool    was_compatible  = preset_edited.is_compatible;
        preset_edited.is_compatible = is_compatible_with_printer(this_preset_with_vendor_profile, active_printer, &config);
        some_compatible |= preset_edited.is_compatible;
	    if (active_print != nullptr)
	        preset_edited.is_compatible &= is_compatible_with_print(this_preset_with_vendor_profile, *active_print, active_printer);
        if (! preset_edited.is_compatible && selected &&
        	(unselect_if_incompatible == PresetSelectCompatibleType::Always || (unselect_if_incompatible == PresetSelectCompatibleType::OnlyIfWasCompatible && was_compatible)))
            m_idx_selected = size_t(-1);
        if (selected)
            preset_selected.is_compatible = preset_edited.is_compatible;
    }
    
    // Update visibility of the default profiles here if the defaults are suppressed, the current profile is not compatible and we don't want to select another compatible profile.
    if (m_idx_selected >= m_num_default_presets && m_default_suppressed)
	    for (size_t i = 0; i < m_num_default_presets; ++ i)
	        m_presets[i].is_visible = ! some_compatible;
    return m_idx_selected;
}

// Update a dirty flag of the current preset
// Return true if the dirty flag changed.
bool PresetCollection::update_dirty()
{
    bool was_dirty = this->get_selected_preset().is_dirty;
    bool is_dirty  = current_is_dirty();
    this->get_selected_preset().is_dirty = is_dirty;
    this->get_edited_preset().is_dirty = is_dirty;

    return was_dirty != is_dirty;
}

template<class T>
void add_correct_opts_to_diff(const std::string &opt_key, t_config_option_keys& vec, const ConfigBase &other, const ConfigBase &this_c)
{
    const T* opt_init = static_cast<const T*>(other.option(opt_key));
    const T* opt_cur = static_cast<const T*>(this_c.option(opt_key));
    int opt_init_max_id = opt_init->values.size() - 1;
    for (int i = 0; i < int(opt_cur->values.size()); i++)
    {
        int init_id = i <= opt_init_max_id ? i : 0;
        if (opt_cur->values[i] != opt_init->values[init_id])
            vec.emplace_back(opt_key + "#" + std::to_string(i));
    }
}

// list of options with vector variable, which is presented to the user as a single input
static const std::set<std::string> single_input_vector_options = {
    "bed_shape",
    "compatible_printers",
    "compatible_prints",
};

bool PresetCollection::is_single_input_vector_option(const std::string& opt_key)
{
    return single_input_vector_options.find(opt_key) != single_input_vector_options.end();
}

// Use deep_diff to correct return of changed options, considering vector options with several values.
inline t_config_option_keys deep_diff(const ConfigBase &config_this, const ConfigBase &config_other)
{
    t_config_option_keys diff;
    for (const t_config_option_key &opt_key : config_this.keys()) {
        const ConfigOption *this_opt  = config_this.option(opt_key);
        const ConfigOption *other_opt = config_other.option(opt_key);
        if (this_opt != nullptr && other_opt != nullptr && *this_opt != *other_opt)
        {
            if (PresetCollection::is_single_input_vector_option(opt_key)) {
                // Scalar variable, or a vector variable, which is presented as a single input,
                // thus the vector is presented to the user as a single input.
                diff.emplace_back(opt_key);
            } else if (opt_key == "thumbnails") {
                // "thumbnails" can not contain extensions in old config but they are valid and use PNG extension by default
                // So, check if "thumbnails" is really changed
                // We will compare full thumbnails instead of exactly config values
                auto [thumbnails, er]         = GCodeThumbnails::make_and_check_thumbnail_list(config_this);
                auto [thumbnails_new, er_new] = GCodeThumbnails::make_and_check_thumbnail_list(config_other);
                if (thumbnails != thumbnails_new || er != er_new)
                    // if those strings are actually the same, erase them from the list of dirty oprions
                    diff.emplace_back(opt_key);
            } else {
                switch (other_opt->type()) {
                case coInts:    add_correct_opts_to_diff<ConfigOptionInts       >(opt_key, diff, config_other, config_this);  break;
                case coBools:   add_correct_opts_to_diff<ConfigOptionBools      >(opt_key, diff, config_other, config_this);  break;
                case coFloats:  add_correct_opts_to_diff<ConfigOptionFloats     >(opt_key, diff, config_other, config_this);  break;
                case coStrings: add_correct_opts_to_diff<ConfigOptionStrings    >(opt_key, diff, config_other, config_this);  break;
                case coPercents:add_correct_opts_to_diff<ConfigOptionPercents   >(opt_key, diff, config_other, config_this);  break;
                case coPoints:  add_correct_opts_to_diff<ConfigOptionPoints     >(opt_key, diff, config_other, config_this);  break;
                case coFloatsOrPercents:    add_correct_opts_to_diff<ConfigOptionFloatsOrPercents   >(opt_key, diff, config_other, config_this);  break;
                case coEnums:   add_correct_opts_to_diff<ConfigOptionEnumsGeneric>(opt_key, diff, config_other, config_this);  break;
                default:        diff.emplace_back(opt_key);     break;
                }
            }
        }
    }
    return diff;
}

static constexpr const std::initializer_list<const char*> optional_keys { "compatible_prints", "compatible_printers" };

bool PresetCollection::is_dirty(const Preset *edited, const Preset *reference)
{
    if (edited != nullptr && reference != nullptr) {
        // Only compares options existing in both configs.
        bool is_dirty = !reference->config.equals(edited->config);
        if (is_dirty) {
            // check deep difference for compared configs
            // there can be cases (as for thumbnails), when configs can logically equal
            // even when their values are not equal.
            is_dirty = !deep_diff(edited->config, reference->config).empty();
        }
        if (is_dirty)
            return true;
        // The "compatible_printers" option key is handled differently from the others:
        // It is not mandatory. If the key is missing, it means it is compatible with any printer.
        // If the key exists and it is empty, it means it is compatible with no printer.
        for (auto &opt_key : optional_keys)
            if (reference->config.has(opt_key) != edited->config.has(opt_key))
                return true;
    }
    return false;
}

std::vector<std::string> PresetCollection::dirty_options(const Preset *edited, const Preset *reference, const bool deep_compare /*= false*/)
{
    std::vector<std::string> changed;
    if (edited != nullptr && reference != nullptr) {
        // Only compares options existing in both configs.
        changed = deep_compare ?
                deep_diff(edited->config, reference->config) :
                reference->config.diff(edited->config);
        // The "compatible_printers" option key is handled differently from the others:
        // It is not mandatory. If the key is missing, it means it is compatible with any printer.
        // If the key exists and it is empty, it means it is compatible with no printer.
        for (auto &opt_key : optional_keys)
            if (reference->config.has(opt_key) != edited->config.has(opt_key))
                changed.emplace_back(opt_key);
    }
    return changed;
}

// Select a new preset. This resets all the edits done to the currently selected preset.
// If the preset with index idx does not exist, a first visible preset is selected.
Preset& PresetCollection::select_preset(size_t idx)
{
    for (Preset &preset : m_presets)
        preset.is_dirty = false;
    if (idx >= m_presets.size())
        idx = first_visible_idx();
    m_idx_selected = idx;
    if (!m_presets[idx].is_visible)
        // The newly selected preset can be activated -> make it visible.
        m_presets[idx].is_visible = true;

    m_edited_preset = m_presets[idx];
    bool default_visible = ! m_default_suppressed || m_idx_selected < m_num_default_presets;
    for (size_t i = 0; i < m_num_default_presets; ++i)
        m_presets[i].is_visible = default_visible;
    return m_presets[idx];
}

bool PresetCollection::select_preset_by_name(const std::string &name_w_suffix, bool force, bool force_invisible /*= false*/)
{
    std::string name = Preset::remove_suffix_modified(name_w_suffix);
    // 1) Try to find the preset by its name.
    auto it = this->find_preset_internal(name);
    size_t idx = 0;
    if (it != m_presets.end() && it->name == name && (force_invisible || it->is_visible))
        // Preset found by its name and it is visible.
        idx = it - m_presets.begin();
    else {
        // Find the first visible preset.
        for (size_t i = m_default_suppressed ? m_num_default_presets : 0; i < m_presets.size(); ++ i)
            if (m_presets[i].is_visible) {
                idx = i;
                break;
            }
        // If the first visible preset was not found, return the 0th element, which is the default preset.
    }

    // 2) Select the new preset.
    if (m_idx_selected != idx || force) {
        this->select_preset(idx);
        return true;
    }

    return false;
}

bool PresetCollection::select_preset_by_name_strict(const std::string &name)
{
    // 1) Try to find the preset by its name.
    auto it = this->find_preset_internal(name);
    size_t idx = (size_t)-1;
    if (it != m_presets.end() && it->name == name && it->is_visible)
        // Preset found by its name.
        idx = it - m_presets.begin();
    // 2) Select the new preset.
    if (idx != (size_t)-1) {
        this->select_preset(idx);
        return true;
    }
    m_idx_selected = idx;
    return false;
}

// Merge one vendor's presets with the other vendor's presets, report duplicates.
std::vector<std::string> PresetCollection::merge_presets(PresetCollection &&other, const VendorMap &new_vendors)
{
    std::vector<std::string> duplicates;
    for (Preset &preset : other.m_presets) {
        if (preset.is_default || preset.is_external)
            continue;
        Preset key(m_type, preset.name);
        auto it = std::lower_bound(m_presets.begin() + m_num_default_presets, m_presets.end(), key);
        if (it == m_presets.end() || it->name != preset.name) {
            if (preset.vendor != nullptr) {
                // Re-assign a pointer to the vendor structure in the new PresetBundle.
                auto it = new_vendors.find(preset.vendor->id);
                assert(it != new_vendors.end());
                preset.vendor = &it->second;
            }
            m_presets.emplace(it, std::move(preset));
        } else
            duplicates.emplace_back(std::move(preset.name));
    }
    return duplicates;
}

void PresetCollection::update_vendor_ptrs_after_copy(const VendorMap &new_vendors)
{
    auto update = [&new_vendors](Preset& preset) {
        if (preset.vendor != nullptr) {
            assert(! preset.is_default && ! preset.is_external);
            // Re-assign a pointer to the vendor structure in the new PresetBundle.
            auto it = new_vendors.find(preset.vendor->id);
            assert(it != new_vendors.end());
            preset.vendor = &it->second;
        }
    };

    for (Preset& preset : m_presets)
        update(preset);
    // update vendor for edited preset too
    update(m_edited_preset);
}

void PresetCollection::update_map_alias_to_profile_name()
{
	m_map_alias_to_profile_name.clear();
	for (const Preset &preset : m_presets)
		m_map_alias_to_profile_name.emplace_back(preset.alias, preset.name);
	std::sort(m_map_alias_to_profile_name.begin(), m_map_alias_to_profile_name.end(), [](auto &l, auto &r) { return l.first < r.first; });
}

void PresetCollection::update_map_system_profile_renamed()
{
	m_map_system_profile_renamed.clear();
	for (Preset &preset : m_presets)
		for (const std::string &renamed_from : preset.renamed_from) {
            const auto [it, success] = m_map_system_profile_renamed.insert(std::pair<std::string, std::string>(renamed_from, preset.name));
			if (! success)
                BOOST_LOG_TRIVIAL(error) << boost::format("Preset name \"%1%\" was marked as renamed from \"%2%\", though preset name \"%3%\" was marked as renamed from \"%2%\" as well.") % preset.name % renamed_from % it->second;
		}
}

std::string PresetCollection::name() const
{
    switch (this->type()) {
    case Preset::TYPE_SLA_PRINT:    return L("SLA print");
    case Preset::TYPE_SLA_MATERIAL: return L("SLA material");
    case Preset::TYPE_PRINTER:      return L("printer");
    default:                        return "invalid";
    }
}

std::string PresetCollection::section_name() const
{
    switch (this->type()) {
    case Preset::TYPE_SLA_PRINT:    return "sla_print";
    case Preset::TYPE_SLA_MATERIAL: return "sla_material";
    case Preset::TYPE_PRINTER:      return "printer";
    default:                        return "invalid";
    }
}

// Used for validating the "inherits" flag when importing user's config bundles.
// Returns names of all system presets including the former names of these presets.
std::vector<std::string> PresetCollection::system_preset_names() const
{
    size_t num = 0;
    for (const Preset &preset : m_presets)
        if (preset.is_system)
            ++ num;
    std::vector<std::string> out;
    out.reserve(num);
    for (const Preset &preset : m_presets)
        if (preset.is_system) {
            out.emplace_back(preset.name);
            out.insert(out.end(), preset.renamed_from.begin(), preset.renamed_from.end());
        }
    std::sort(out.begin(), out.end());
    return out;
}

// Generate a file path from a profile name. Add the ".ini" suffix if it is missing.
std::string PresetCollection::path_from_name(const std::string &new_name) const
{
    std::string file_name = boost::iends_with(new_name, ".ini") ? new_name : (new_name + ".ini");
    return (boost::filesystem::path(m_dir_path) / file_name).make_preferred().string();
}

const Preset* PrinterPresetCollection::find_system_preset_by_model_and_variant(const std::string &model_id, const std::string& variant) const
{
    if (model_id.empty()) { return nullptr; }

    const auto it = std::find_if(cbegin(), cend(), [&](const Preset &preset) {
        if (!preset.is_system || preset.config.opt_string("printer_model") != model_id)
            return false;
        if (variant.empty())
            return true;
        return preset.config.opt_string("printer_variant") == variant;
    });

    return it != cend() ? &*it : nullptr;
}

bool  PrinterPresetCollection::only_default_printers() const
{
    for (const auto& printer : get_presets()) {
        if (! printer.is_default)
            return false;
    }
    return true;
}
namespace PresetUtils {
	const VendorProfile::PrinterModel* system_printer_model(const Preset &preset)
	{
		const VendorProfile::PrinterModel *out = nullptr;
		if (preset.vendor != nullptr) {
			const auto *printer_model = preset.config.opt<ConfigOptionString>("printer_model");
			if (printer_model != nullptr && ! printer_model->value.empty()) {
				auto it = std::find_if(preset.vendor->models.begin(), preset.vendor->models.end(), [printer_model](const VendorProfile::PrinterModel &pm) { return pm.id == printer_model->value; });
				if (it != preset.vendor->models.end())
					out = &(*it);
			}
		}
		return out;
	}

    std::string system_printer_bed_model(const Preset& preset)
    {
        std::string out;
        const VendorProfile::PrinterModel* pm = PresetUtils::system_printer_model(preset);
        if (pm != nullptr && !pm->bed_model.empty()) {
            out = Slic3r::data_dir() + "/vendor/" + preset.vendor->id + "/" + pm->bed_model;
            if (!boost::filesystem::exists(boost::filesystem::path(out)))
                out = Slic3r::resources_dir() + "/profiles/" + preset.vendor->id + "/" + pm->bed_model;
        }
        return out;
    }

    std::string system_printer_bed_texture(const Preset& preset)
    {
        std::string out;
        const VendorProfile::PrinterModel* pm = PresetUtils::system_printer_model(preset);
        if (pm != nullptr && !pm->bed_texture.empty()) {
            out = Slic3r::data_dir() + "/vendor/" + preset.vendor->id + "/" + pm->bed_texture;
            if (!boost::filesystem::exists(boost::filesystem::path(out)))
                out = Slic3r::resources_dir() + "/profiles/" + preset.vendor->id + "/" + pm->bed_texture;
        }
        return out;
    }

    bool vendor_profile_has_all_resources(const VendorProfile& vp)
    {
        namespace fs = boost::filesystem;

        std::string vendor_folder = Slic3r::data_dir()      + "/vendor/"   + vp.id + "/";
        std::string rsrc_folder   = Slic3r::resources_dir() + "/profiles/" + vp.id + "/";
        std::string cache_folder  = Slic3r::data_dir()      + "/cache/"    + vp.id + "/";
        for (const VendorProfile::PrinterModel& model : vp.models) {
            for (const std::string& res : { model.bed_texture, model.bed_model, model.thumbnail } ) {
                if (! res.empty()
                 && !fs::exists(fs::path(vendor_folder + res))
                 && !fs::exists(fs::path(rsrc_folder   + res))
                 && !fs::exists(fs::path(cache_folder  + res)))
                    return false;
            }
        }
        return true;
    }
    bool compare_vendor_profile_printers(const VendorProfile& vp_old, const VendorProfile& vp_new, std::vector<std::string>& new_printers)
    {
        for (const VendorProfile::PrinterModel& model : vp_new.models)
        {
            if (std::find_if(vp_old.models.begin(), vp_old.models.end(), [model](const VendorProfile::PrinterModel& pm) { return pm.id == model.id; }) == vp_old.models.end())
                new_printers.push_back(model.name);
        }
        return new_printers.empty();
    }
} // namespace PresetUtils

} // namespace Slic3r
