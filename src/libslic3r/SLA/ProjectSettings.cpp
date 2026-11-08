// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectSettings.hpp"
#include "PrinterGeometry.hpp"
#include "LayerExportProfile.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include <algorithm>
#include <set>
#include <nlohmann/json.hpp>

namespace Slic3r::sla {
namespace {
using Metadata = std::map<std::string,std::string>;
using RoleMetadata = std::optional<Metadata>;
constexpr size_t metadata_budget = 4*1024*1024;

const std::vector<std::string> &metadata_keys(const std::string &role) {
    static const std::vector<std::string> printer{
        "inherits", "printer_vendor", "printer_variant",
        "default_sla_print_profile", "default_sla_material_profile"};
    static const std::vector<std::string> material{
        "inherits", "compatible_printers", "compatible_printers_condition",
        "compatible_prints", "compatible_prints_condition", "default_sla_material_profile"};
    static const std::vector<std::string> process{
        "inherits", "compatible_printers", "compatible_printers_condition", "default_sla_print_profile"};
    if(role=="printer") return printer;
    if(role=="material") return material;
    if(role=="process") return process;
    throw RuntimeError("Unknown project SLA preset role");
}

DynamicPrintConfig restore_metadata(const Metadata &metadata, const std::string &role) {
    const auto &keys=metadata_keys(role);
    if(metadata.size()!=keys.size()) throw RuntimeError("Incomplete project SLA preset metadata");
    DynamicPrintConfig result;
    for(const auto &key:keys) {
        const auto found=metadata.find(key);
        if(found==metadata.end() || found->second.size()>1024*1024 || found->second.find('\0')!=std::string::npos)
            throw RuntimeError("Missing or oversized project SLA preset metadata option");
        result.set_deserialize_strict(key,found->second);
        if(result.opt_serialize(key)!=found->second)
            throw RuntimeError("Noncanonical project SLA preset metadata option");
    }
    return result;
}

Metadata capture_metadata(const ConfigBase &configuration, const char *role) {
    DynamicPrintConfig values;
    for(const auto &key:metadata_keys(role)) values.optptr(key,true);
    values.apply_only(configuration,metadata_keys(role),true);
    Metadata result;
    for(const auto &key:values.keys()) result.emplace(key,values.opt_serialize(key));
    return result;
}

void check_metadata_budget(const std::vector<RoleMetadata> &roles) {
    size_t bytes=0;
    for(const auto &role:roles) if(role)
        for(const auto &[key,value]:*role) {
            if(key.size()+value.size()>metadata_budget-bytes)
                throw RuntimeError("Project SLA preset metadata exceeds the 4 MiB budget");
            bytes+=key.size()+value.size();
        }
}

std::vector<RoleMetadata> parse_metadata(const ConfigBase &configuration) {
    const auto *option=configuration.option<ConfigOptionString>("sla_preset_metadata");
    if(!option || option->value.empty()) return {std::nullopt,std::nullopt,std::nullopt};
    if(option->value.size()>metadata_budget) throw RuntimeError("Project SLA preset metadata exceeds the 4 MiB budget");
    // Duplicate object members are ambiguous even if a JSON parser normally
    // accepts them by keeping the last value. Reject before interpreting roles.
    std::vector<std::set<std::string>> members;
    const auto data=nlohmann::json::parse(option->value,[&](int,nlohmann::json::parse_event_t event,nlohmann::json &value) {
        if(event==nlohmann::json::parse_event_t::object_start) members.emplace_back();
        else if(event==nlohmann::json::parse_event_t::object_end) members.pop_back();
        else if(event==nlohmann::json::parse_event_t::key && !members.back().insert(value.get<std::string>()).second)
            throw RuntimeError("Duplicate project SLA preset metadata member");
        return true;
    });
    if(!data.is_object() || data.size()!=4 || !data.at("version").is_number_integer() || data.at("version")!=1)
        throw RuntimeError("Invalid project SLA preset metadata version or members");
    std::vector<RoleMetadata> result;
    for(const char *role:{"printer","material","process"}) {
        const auto &value=data.at(role);
        if(value.is_null()) result.emplace_back(std::nullopt);
        else {
            if(!value.is_object()) throw RuntimeError("Invalid project SLA preset metadata role");
            result.emplace_back(value.get<Metadata>());
            restore_metadata(*result.back(),role);
        }
    }
    check_metadata_budget(result);
    return result;
}

std::string encode_metadata(const std::vector<RoleMetadata> &roles) {
    check_metadata_budget(roles);
    nlohmann::json data{{"version",1}};
    size_t index=0;
    for(const char *role:{"printer","material","process"}) {
        const auto &metadata=roles.at(index++);
        if(metadata) {restore_metadata(*metadata,role);data[role]=*metadata;}
        else data[role]=nullptr;
    }
    const auto encoded=data.dump();
    if(encoded.size()>metadata_budget) throw RuntimeError("Project SLA preset metadata exceeds the 4 MiB budget");
    return encoded;
}

DynamicPrintConfig process_schema() {
    DynamicPrintConfig result;
    result.apply(SLAPrintConfig{});
    result.apply(SLAPrintObjectConfig{});
    return result;
}
void finite_option(const ConfigOption &option) {
    if (option.nullable() && option.is_nil()) return;
    auto finite=[](double value) {
        if (!std::isfinite(value)) throw RuntimeError("Nonfinite project SLA setting");
    };
    switch (option.type()) {
    case coFloat: case coPercent: case coFloatOrPercent:
        finite(option.getFloat()); break;
    case coFloats:
        for (double value:static_cast<const ConfigOptionFloats &>(option).values) finite(value);
        break;
    case coPoints:
        for (const auto &point:static_cast<const ConfigOptionPoints &>(option).values) {
            finite(point.x()); finite(point.y());
        }
        break;
    default: break;
    }
}
void restore_role(DynamicPrintConfig &output,const ProjectPresetSettings &role,const ConfigBase &schema) {
    if (role.name.size()>4096 || role.name.find('\0')!=std::string::npos || role.values.size()!=schema.keys().size())
        throw RuntimeError("Invalid or incomplete project SLA preset");
    for (const auto &key:schema.keys()) {
        const auto found=role.values.find(key);
        if (found==role.values.end() || found->second.size()>1024*1024)
            throw RuntimeError("Missing or oversized project SLA option");
        output.set_deserialize_strict(key,found->second);
        finite_option(*output.option(key));
        // The writer stores canonical option strings. Do not allow trailing
        // garbage, permissive legacy substitutions or silently narrowed input.
        if (output.opt_serialize(key)!=found->second)
            throw RuntimeError("Noncanonical project SLA option");
    }
}
ProjectPresetSettings capture_role(const ConfigBase &configuration,const ConfigBase &schema,const char *identity) {
    DynamicPrintConfig values;values.apply(schema);values.apply_only(configuration,schema.keys(),true);
    ProjectPresetSettings result;
    if (const auto *name=configuration.option<ConfigOptionString>(identity)) result.name=name->value;
    for (const auto &key:values.keys()) result.values.emplace(key,values.opt_serialize(key));
    return result;
}
} // namespace

bool is_sla_preset_metadata_key(const std::string &key) {
    for(const char *role:{"printer","material","process"}) {
        const auto &keys=metadata_keys(role);
        if(std::find(keys.begin(),keys.end(),key)!=keys.end()) return true;
    }
    return false;
}

std::string serialize_sla_preset_metadata(const ConfigBase &printer,
    const ConfigBase &material, const ConfigBase &process) {
    return encode_metadata({capture_metadata(printer,"printer"),
        capture_metadata(material,"material"),capture_metadata(process,"process")});
}

void apply_sla_preset_metadata(const ConfigBase &combined, DynamicPrintConfig &role,
    const char *role_name) {
    const auto metadata=parse_metadata(combined);
    const std::string name=role_name;
    const size_t index=name=="printer" ? 0 : name=="material" ? 1 : name=="process" ? 2 : 3;
    if(index>=metadata.size()) throw RuntimeError("Unknown project SLA preset role");
    if(metadata[index]) role.apply(restore_metadata(*metadata[index],name));
}

DynamicPrintConfig restore_project_settings(const ProjectSLASettings &settings) {
    size_t bytes=0;
    for (const auto *role:{&settings.printer,&settings.material,&settings.process})
        for (const auto &[key,value]:role->values) {
            if (key.size()+value.size()>4*1024*1024-bytes)
                throw RuntimeError("Project SLA settings exceed the 4 MiB budget");
            bytes+=key.size()+value.size();
        }
    const std::vector<RoleMetadata> metadata{settings.printer.metadata,settings.material.metadata,settings.process.metadata};
    check_metadata_budget(metadata);
    size_t role_index=0;
    for(const char *role_name:{"printer","material","process"}) {
        const auto *role=role_index==0 ? &settings.printer : role_index==1 ? &settings.material : &settings.process;
        if(metadata[role_index]) {
            const auto values=restore_metadata(*metadata[role_index],role_name);
            if(!role->name.empty() && values.opt_string("inherits")==role->name)
                throw RuntimeError("A project SLA preset cannot inherit itself");
            for(const auto &[key,value]:*metadata[role_index]) {
                if(key.size()+value.size()>metadata_budget-bytes)
                    throw RuntimeError("Project SLA settings exceed the 4 MiB budget");
                bytes+=key.size()+value.size();
            }
        }
        ++role_index;
    }
    DynamicPrintConfig result;
    restore_role(result,settings.printer,SLAPrinterConfig{});
    restore_role(result,settings.material,SLAMaterialConfig{});
    restore_role(result,settings.process,process_schema());
    if (result.option("printer_technology")->getInt()!=ptSLA)
        throw RuntimeError("A layer project requires SLA printer settings");
    const auto expected_bed=result.option<ConfigOptionPoints>("bed_shape")->values;
    auto normalized=result;
    synchronize_bed_shape(normalized);
    if (expected_bed!=normalized.option<ConfigOptionPoints>("bed_shape")->values)
        throw RuntimeError("Project printer bed does not match its LCD");
    LayerExportProfile physical;
    physical.columns=result.option("display_pixels_x")->getInt();
    physical.rows=result.option("display_pixels_y")->getInt();
    physical.screen_width_mm=result.option("display_width")->getFloat();
    physical.screen_height_mm=result.option("display_height")->getFloat();
    physical.density_g_ml=result.option("material_density")->getFloat();
    const double bottle=result.option("bottle_volume")->getFloat();
    if (!std::isfinite(bottle) || bottle<=0) throw RuntimeError("Invalid project material bottle volume");
    physical.cost_per_ml=result.option("bottle_cost")->getFloat()/bottle;
    physical.max_print_height_mm=result.option("max_print_height")->getFloat();
    if (!physical.columns || !physical.rows || physical.columns>32768 || physical.rows>32768 ||
        physical.columns*physical.rows>128*1024*1024 || physical.max_print_height_mm<=0)
        throw RuntimeError("Invalid project printer geometry");
    physical.validate(physical.columns,physical.rows);
    const std::string format=result.opt_string("sla_archive_format");
    if (!get_archive_entry(format.c_str())) throw RuntimeError("Unknown project SLA export format");
    const auto versions=anycubic_available_versions(format);
    if (!versions.empty()) anycubic_resolve_version(format,result.opt_string("sla_anycubic_version"),versions.front());
    if (result.option("layer_height")->getFloat()<=0 || result.option("initial_layer_height")->getFloat()<=0)
        throw RuntimeError("Invalid project layer height");
    if(std::any_of(metadata.begin(),metadata.end(),[](const auto &role){return role.has_value();})) {
        result.set_key_value("sla_preset_metadata",new ConfigOptionString(encode_metadata(metadata)));
        // Printer model/variant/defaults are needed by exporters and placeholders,
        // while all role relationships stay separately represented in the blob.
        if(settings.printer.metadata) result.apply(restore_metadata(*settings.printer.metadata,"printer"));
    }
    result.set_key_value("printer_settings_id",new ConfigOptionString(settings.printer.name));
    result.set_key_value("sla_material_settings_id",new ConfigOptionString(settings.material.name));
    result.set_key_value("sla_print_settings_id",new ConfigOptionString(settings.process.name));
    return result;
}
ProjectSLASettings capture_project_settings(const ConfigBase &configuration) {
    DynamicPrintConfig normalized;normalized.apply(SLAFullPrintConfig{});normalized.apply(configuration,true);
    synchronize_bed_shape(normalized);
    ProjectSLASettings result{
        capture_role(normalized,SLAPrinterConfig{},"printer_settings_id"),
        capture_role(normalized,SLAMaterialConfig{},"sla_material_settings_id"),
        capture_role(normalized,process_schema(),"sla_print_settings_id")};
    const auto metadata=parse_metadata(configuration);
    result.printer.metadata=metadata[0];
    // Older combined configurations may have printer identity/model fields,
    // but no role metadata blob. Preserve what is explicitly available without
    // guessing absent material/process compatibility lists or library defaults.
    const auto *blob=configuration.option<ConfigOptionString>("sla_preset_metadata");
    if((!blob || blob->value.empty()) && !result.printer.metadata) {
        bool has_printer_metadata=false;
        for(const char *key:{"printer_vendor","printer_variant",
                             "default_sla_print_profile","default_sla_material_profile"})
            has_printer_metadata |= configuration.has(key);
        if(has_printer_metadata) {
            auto printer=capture_metadata(configuration,"printer");
            if(const auto *parents=configuration.option<ConfigOptionStrings>("inherits_cummulative"))
                if(parents->values.size()==3)
                    printer["inherits"]=ConfigOptionString(parents->values[2]).serialize();
            result.printer.metadata=std::move(printer);
        }
    }
    result.material.metadata=metadata[1];
    result.process.metadata=metadata[2];
    restore_project_settings(result);
    return result;
}
} // namespace Slic3r::sla
