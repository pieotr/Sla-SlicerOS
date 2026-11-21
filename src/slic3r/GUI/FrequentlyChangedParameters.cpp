///|/ Copyright (c) Prusa Research 2018 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Tomáš Mészáros @tamasmeszaros, David Kocík @kocikdav, Lukáš Hejl @hejllukas, Pavel Mikuš @Godrak, Filip Sykala @Jony01, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2022 Michael Kirsch
///|/ Copyright (c) 2021 Boleslaw Ciesielski
///|/ Copyright (c) 2019 John Drake @foxox
///|/
///|/ ported from lib/Slic3r/GUI/Plater.pm:
///|/ Copyright (c) Prusa Research 2016 - 2019 Vojtěch Bubník @bubnikv, Vojtěch Král @vojtechkral, Enrico Turri @enricoturri1966, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) 2018 Martin Loidl @LoidlM
///|/ Copyright (c) 2017 Matthias Gazzari @qtux
///|/ Copyright (c) Slic3r 2012 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2017 Joseph Lenox @lordofhyphens
///|/ Copyright (c) 2015 Daren Schwenke
///|/ Copyright (c) 2014 Mark Hindess
///|/ Copyright (c) 2012 Mike Sheldrake @mesheldrake
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/ Copyright (c) 2012 Sam Wong
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "FrequentlyChangedParameters.hpp"
#include "Plater.hpp"

#include <string>

#include <wx/sizer.h>
#include <wx/button.h>

#include "libslic3r/PresetBundle.hpp"

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "format.hpp"
#include "Tab.hpp"
#include "I18N.hpp"

using Slic3r::Preset;
using Slic3r::GUI::format_wxstr;

namespace Slic3r {
namespace GUI {

// Trigger Plater::schedule_background_process().
wxDEFINE_EVENT(EVT_SCHEDULE_BACKGROUND_PROCESS,     SimpleEvent);

FreqChangedParams::FreqChangedParams(wxWindow* parent)
{

    // Frequently changed parameters for SLA_technology

    m_og_sla = std::make_shared<ConfigOptionsGroup>(parent, "");
    m_og_sla->hide_labels();
    DynamicPrintConfig*	config_sla = &wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    m_og_sla->set_config(config_sla);

    m_og_sla->on_change = [config_sla](t_config_option_key opt_key, boost::any value) {
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_SLA_PRINT);
        if (!tab) return;

        DynamicPrintConfig new_conf = *config_sla;
        if (opt_key == "pad") {
            const wxString& selection = boost::any_cast<wxString>(value);

            const bool pad_enable = selection == _("None") ? false : true;
            new_conf.set_key_value("pad_enable", new ConfigOptionBool(pad_enable));

            if (selection == _("Below object"))
                new_conf.set_key_value("pad_around_object", new ConfigOptionBool(false));
            else if (selection == _("Around object"))
                new_conf.set_key_value("pad_around_object", new ConfigOptionBool(true));
        }
        else
        {
            assert(opt_key == "support");
            const wxString& selection = boost::any_cast<wxString>(value);

            const bool supports_enable = selection == _("None") ? false : true;
            new_conf.set_key_value("supports_enable", new ConfigOptionBool(supports_enable));

            std::string treetype = get_sla_suptree_prefix(new_conf);

            if (selection == _("Everywhere")) {
                new_conf.set_key_value(treetype + "support_buildplate_only", new ConfigOptionBool(false));
                new_conf.set_key_value("support_enforcers_only", new ConfigOptionBool(false));
            }
            else if (selection == _("Support on build plate only")) {
                new_conf.set_key_value(treetype + "support_buildplate_only", new ConfigOptionBool(true));
                new_conf.set_key_value("support_enforcers_only", new ConfigOptionBool(false));
            }
            else if (selection == _("For support enforcers only")) {
                new_conf.set_key_value("support_enforcers_only", new ConfigOptionBool(true));
            }
        }

        tab->load_config(new_conf);
        tab->update_dirty();
    };

    Line line = Line{ "", "" };
    auto empty_widget_sla = [this](wxWindow* parent) {
        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        auto btn = new ScalableButton(parent, wxID_ANY, "mirroring_transparent", wxEmptyString,
            wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER | wxTRANSPARENT_WINDOW);
        sizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, int(0.3 * wxGetApp().em_unit()));
        m_empty_buttons.push_back(btn);
        return sizer;
    };

    ConfigOptionDef support_def_sla;
    support_def_sla.label = L("Supports");
    support_def_sla.type = coStrings;
    support_def_sla.tooltip = L("Select what kind of support do you need");
    support_def_sla.set_enum_labels(ConfigOptionDef::GUIType::select_close, {
        L("None"),
        L("Support on build plate only"),
        L("For support enforcers only"),
        L("Everywhere")
    });
    support_def_sla.set_default_value(new ConfigOptionStrings{ "None" });
    Option option_support(support_def_sla, "support");
    option_support.opt.full_width = true;
    line.append_option(option_support);
    line.append_widget(empty_widget_sla);
    m_og_sla->append_line(line);

    line = Line{ "", "" };

    ConfigOptionDef pad_def;
    pad_def.label = L("Pad");
    pad_def.type = coStrings;
    pad_def.tooltip = L("Select what kind of pad do you need");
    pad_def.set_enum_labels(ConfigOptionDef::GUIType::select_close, {
        L("None"),
        L("Below object"),
        L("Around object")
    });
    pad_def.set_default_value(new ConfigOptionStrings{ "Below object" });
    Option option_pad(pad_def, "pad");
    option_pad.opt.full_width = true;
    line.append_option(option_pad);
    line.append_widget(empty_widget_sla);

    m_og_sla->append_line(line);

    m_og_sla->activate();
    Choice* choice = dynamic_cast<Choice*>(m_og_sla->get_field("support"));
    choice->suppress_scroll();
    choice = dynamic_cast<Choice*>(m_og_sla->get_field("pad"));
    choice->suppress_scroll();

    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->Add(m_og_sla->sizer, 0, wxEXPAND);
}

void FreqChangedParams::msw_rescale()
{
    m_og_sla->msw_rescale();
}

void FreqChangedParams::sys_color_changed()
{
    m_og_sla->sys_color_changed();

    for (auto btn: m_empty_buttons)
        btn->sys_color_changed();
}

void FreqChangedParams::Show() const
{
    m_og_sla->Show(true);
}

ConfigOptionsGroup* FreqChangedParams::get_og()
{
    return m_og_sla.get();
}

}}    // namespace Slic3r::GUI
