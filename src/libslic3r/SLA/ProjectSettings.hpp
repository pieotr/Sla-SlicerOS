// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <map>
#include <string>
#include <optional>
namespace Slic3r {
class ConfigBase;
class DynamicPrintConfig;
namespace sla {
// Resolved effective options, not a dependency on installed preset/inheritance files.
// Role schemas intentionally contain SLA settings only and no host credentials.
struct ProjectPresetSettings {
    std::string name;
    std::map<std::string,std::string> values;
    // Absent in legacy settings: no claim about preset library relationships.
    std::optional<std::map<std::string,std::string>> metadata;
    bool operator==(const ProjectPresetSettings &other) const {
        return name==other.name && values==other.values && metadata==other.metadata;
    }
};
struct ProjectSLASettings {
    ProjectPresetSettings printer, material, process;
    bool operator==(const ProjectSLASettings &other) const {
        return printer==other.printer && material==other.material && process==other.process;
    }
};
// Persist role-specific library settings independently of flattened runtime values.
// Only offline SLA metadata is accepted; credentials and FFF fields are excluded.
bool is_sla_preset_metadata_key(const std::string &key);
std::string serialize_sla_preset_metadata(const ConfigBase &printer,
    const ConfigBase &material, const ConfigBase &process);
void apply_sla_preset_metadata(const ConfigBase &combined, DynamicPrintConfig &role,
    const char *role_name);
ProjectSLASettings capture_project_settings(const ConfigBase &configuration);
// Strict role/schema validation. Returns a local config, never changes global presets.
DynamicPrintConfig restore_project_settings(const ProjectSLASettings &settings);
} // namespace sla
} // namespace Slic3r
