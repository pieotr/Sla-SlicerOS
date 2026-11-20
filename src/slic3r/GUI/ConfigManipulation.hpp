///|/ Copyright (c) Prusa Research 2019 - 2021 Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ConfigManipulation_hpp_
#define slic3r_ConfigManipulation_hpp_

/*	 Class for validation config options
 *	 and update (enable/disable) IU components
 *	 
 *	 Used for config validation for global config (Print Settings Tab)
 *	 and local config (overrides options on sidebar)
 * */

#include "libslic3r/PrintConfig.hpp"
#include "Field.hpp"

namespace Slic3r {

class ModelConfig;

namespace GUI {

class ConfigManipulation
{
    bool                is_msg_dlg_already_exist{ false };

    // function to loading of changed configuration 
    std::function<void()>                                       load_config = nullptr;
    std::function<void (const std::string&, bool toggle, int opt_index)>   cb_toggle_field = nullptr;
    // callback to propagation of changed value, if needed 
    std::function<void(const std::string&, const boost::any&)>  cb_value_change = nullptr;
    ModelConfig* local_config = nullptr;
    wxWindow*    m_msg_dlg_parent {nullptr};

public:
    ConfigManipulation(std::function<void()> load_config,
        std::function<void(const std::string&, bool toggle, int opt_index)> cb_toggle_field,
        std::function<void(const std::string&, const boost::any&)>  cb_value_change,
        ModelConfig* local_config = nullptr,
        wxWindow* msg_dlg_parent  = nullptr) :
        load_config(load_config),
        cb_toggle_field(cb_toggle_field),
        cb_value_change(cb_value_change),
        m_msg_dlg_parent(msg_dlg_parent),
        local_config(local_config) {}
    ConfigManipulation() {}

    ~ConfigManipulation() {
        load_config = nullptr;
        cb_toggle_field = nullptr;
        cb_value_change = nullptr;
    }

    void    apply(DynamicPrintConfig* config, DynamicPrintConfig* new_config);
    void    toggle_field(const std::string& field_key, const bool toggle, int opt_index = -1);

    // SLA print
    void    toggle_print_sla_options(DynamicPrintConfig* config);
};

} // GUI
} // Slic3r

#endif /* slic3r_ConfigManipulation_hpp_ */
