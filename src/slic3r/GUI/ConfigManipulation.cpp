///|/ Copyright (c) Prusa Research 2019 - 2023 Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, Pavel Mikuš @Godrak, Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ConfigManipulation.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "format.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "MsgDialog.hpp"

#include <string>
#include <wx/msgdlg.h>

namespace Slic3r {
namespace GUI {

void ConfigManipulation::apply(DynamicPrintConfig* config, DynamicPrintConfig* new_config)
{
    bool modified = false;
    for (auto opt_key : config->diff(*new_config)) {
        config->set_key_value(opt_key, new_config->option(opt_key)->clone());
        modified = true;
    }

    if (modified && load_config != nullptr)
        load_config();
}

void ConfigManipulation::toggle_field(const std::string& opt_key, const bool toggle, int opt_index/* = -1*/)
{
    if (local_config) {
        if (local_config->option(opt_key) == nullptr)
            return;
    }
    cb_toggle_field(opt_key, toggle, opt_index);
}

void ConfigManipulation::toggle_print_sla_options(DynamicPrintConfig* config)
{
    bool supports_en = config->opt_bool("supports_enable");
    sla::SupportTreeType treetype = config->opt_enum<sla::SupportTreeType>("support_tree_type");
    bool is_default_tree = treetype == sla::SupportTreeType::Default;
    // The organic tree shares the branching option family.
    bool is_branching_tree = treetype == sla::SupportTreeType::Branching ||
                             treetype == sla::SupportTreeType::Organic ||
                             treetype == sla::SupportTreeType::OrganicBranching;

    toggle_field("support_tree_type", supports_en);

    toggle_field("support_head_front_diameter", supports_en && is_default_tree);
    toggle_field("support_head_penetration", supports_en && is_default_tree);
    toggle_field("support_head_width", supports_en && is_default_tree);
    toggle_field("support_pillar_diameter", supports_en && is_default_tree);
    toggle_field("support_small_pillar_diameter_percent", supports_en && is_default_tree);
    toggle_field("support_max_bridges_on_pillar", supports_en && is_default_tree);
    toggle_field("support_pillar_connection_mode", supports_en && is_default_tree);
    toggle_field("support_buildplate_only", supports_en && is_default_tree);
    toggle_field("support_base_diameter", supports_en && is_default_tree);
    toggle_field("support_base_height", supports_en && is_default_tree);
    toggle_field("support_base_safety_distance", supports_en && is_default_tree);
    toggle_field("support_critical_angle", supports_en && is_default_tree);
    toggle_field("support_max_bridge_length", supports_en && is_default_tree);
    toggle_field("support_enforcers_only", supports_en);
    toggle_field("support_max_pillar_link_distance", supports_en && is_default_tree);
    toggle_field("support_pillar_widening_factor", false);
    toggle_field("support_max_weight_on_model", false);

    toggle_field("branchingsupport_head_front_diameter", supports_en && is_branching_tree);
    toggle_field("branchingsupport_head_penetration", supports_en && is_branching_tree);
    toggle_field("branchingsupport_head_width", supports_en && is_branching_tree);
    toggle_field("branchingsupport_pillar_diameter", supports_en && is_branching_tree);
    toggle_field("branchingsupport_small_pillar_diameter_percent", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_bridges_on_pillar", false);
    toggle_field("branchingsupport_pillar_connection_mode", false);
    toggle_field("branchingsupport_buildplate_only", supports_en && is_branching_tree);
    toggle_field("branchingsupport_base_diameter", supports_en && is_branching_tree);
    toggle_field("branchingsupport_base_height", supports_en && is_branching_tree);
    toggle_field("branchingsupport_base_safety_distance", supports_en && is_branching_tree);
    toggle_field("branchingsupport_critical_angle", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_bridge_length", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_pillar_link_distance", false);
    toggle_field("branchingsupport_pillar_widening_factor", supports_en && is_branching_tree);
    toggle_field("branchingsupport_max_weight_on_model", supports_en && is_branching_tree);

    toggle_field("support_points_density_relative", supports_en);

    bool pad_en = config->opt_bool("pad_enable");

    toggle_field("pad_wall_thickness", pad_en);
    toggle_field("pad_wall_height", pad_en);
    toggle_field("pad_brim_size", pad_en);
    toggle_field("pad_max_merge_distance", pad_en);
 // toggle_field("pad_edge_radius", supports_en);
    toggle_field("pad_wall_slope", pad_en);
    toggle_field("pad_around_object", pad_en);
    toggle_field("pad_around_object_everywhere", pad_en);

    bool zero_elev = config->opt_bool("pad_around_object") && pad_en;

    toggle_field("support_object_elevation", supports_en && is_default_tree && !zero_elev);
    toggle_field("branchingsupport_object_elevation", supports_en && is_branching_tree && !zero_elev);
    toggle_field("pad_object_gap", zero_elev);
    toggle_field("pad_around_object_everywhere", zero_elev);
    toggle_field("pad_object_connector_stride", zero_elev);
    toggle_field("pad_object_connector_width", zero_elev);
    toggle_field("pad_object_connector_penetration", zero_elev);
}


} // GUI
} // Slic3r
