///|/ Copyright (c) Prusa Research 2020 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, Filip Sykala @Jony01, Lukáš Matěna @lukasmatena, Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) 2021 Scott Mudge @ScottMudge
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PresetComboBoxes.hpp"

#include <cstddef>
#include <vector>
#include <string>
#include <boost/algorithm/string.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/statbox.h>
#include <wx/colordlg.h>
#include <wx/wupdlock.h>
#include <wx/menu.h>
#include <wx/odcombo.h>
#include <wx/listbook.h>
#include <wx/generic/stattextg.h>

#ifdef _WIN32
#include <wx/msw/dcclient.h>
#include <wx/msw/private.h>
#endif

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Color.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Tab.hpp"
#include "ConfigWizard.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "../Utils/FixModelByWin10.hpp"
#include "../Utils/UndoRedo.hpp"
#include "BitmapCache.hpp"
#include "MsgDialog.hpp"

#include "Widgets/ComboBox.hpp"

// A workaround for a set of issues related to text fitting into gtk widgets:
// See e.g.: https://github.com/prusa3d/PrusaSlicer/issues/4584

using Slic3r::GUI::format_wxstr;

namespace Slic3r {
namespace GUI {

#define BORDER_W 10

// ---------------------------------
// ***  PresetComboBox  ***
// ---------------------------------

/* For PresetComboBox we use bitmaps that are created from images that are already scaled appropriately for Retina
 * (Contrary to the intuition, the `scale` argument for Bitmap's constructor doesn't mean
 * "please scale this to such and such" but rather
 * "the wxImage is already sized for backing scale such and such". )
 * Unfortunately, the constructor changes the size of wxBitmap too.
 * Thus We need to use unscaled size value for bitmaps that we use
 * to avoid scaled size of control items.
 * For this purpose control drawing methods and
 * control size calculation methods (virtual) are overridden.
 **/

PresetComboBox::PresetComboBox(wxWindow* parent, Preset::Type preset_type, const wxSize& size, PresetBundle* preset_bundle/* = nullptr*/) :
    BitmapComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, size, 0, nullptr, wxCB_READONLY),
    m_type(preset_type),
    m_last_selected(wxNOT_FOUND),
    m_em_unit(em_unit(this))
{
    init_from_bundle(preset_bundle);

    // Accessible name (see ComboBox::update_accessible): the description carries the value and "(modified)".
    {
        const char *accessible_name = "Preset";
        switch (m_type) {
        case Preset::TYPE_SLA_PRINT:    accessible_name = "SLA print preset"; break;
        case Preset::TYPE_SLA_MATERIAL: accessible_name = "SLA material preset"; break;
        case Preset::TYPE_PRINTER:      accessible_name = "Printer preset"; break;
        default: break;
        }
        SetName(accessible_name);
        publish_accessible_value();
    }

    m_bitmapCompatible   = get_bmp_bundle("flag_green");
    m_bitmapIncompatible = get_bmp_bundle("flag_red");

    // parameters for an icon's drawing
    fill_width_height();

    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
        if (m_suppress_change)
            e.StopPropagation();
        else
            e.Skip();
    });
    Bind(wxEVT_COMBOBOX_DROPDOWN, [this](wxCommandEvent&) { m_suppress_change = false; });
    Bind(wxEVT_COMBOBOX_CLOSEUP,  [this](wxCommandEvent&) { m_suppress_change = true;  });

    Bind(wxEVT_COMBOBOX, &PresetComboBox::OnSelect, this);
}

void PresetComboBox::init_from_bundle(PresetBundle* preset_bundle)
{
    m_preset_bundle = preset_bundle ? preset_bundle : wxGetApp().preset_bundle;

    switch (m_type)
    {
    case Preset::TYPE_SLA_PRINT: {
        m_collection = &m_preset_bundle->sla_prints;
        m_main_bitmap_name = "cog";
        break;
    }
    case Preset::TYPE_SLA_MATERIAL: {
        m_collection = &m_preset_bundle->sla_materials;
        m_main_bitmap_name = "resin";
        break;
    }
    case Preset::TYPE_PRINTER: {
        m_collection = &m_preset_bundle->printers;
        m_main_bitmap_name = "printer";
        break;
    }
    default: break;
    }
}

void PresetComboBox::OnSelect(wxCommandEvent& evt)
{
    // see https://github.com/prusa3d/PrusaSlicer/issues/3889
    // Under OSX: in case of use of a same names written in different case (like "ENDER" and "Ender")
    // m_presets_choice->GetSelection() will return first item, because search in PopupListCtrl is case-insensitive.
    // So, use GetSelection() from event parameter 
    auto selected_item = evt.GetSelection();
    if (selected_item < 0 || size_t(selected_item) >= GetCount()) {
        evt.StopPropagation();
        return;
    }

    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX)
        this->SetSelection(m_last_selected);
    else if (on_selection_changed && (m_last_selected != selected_item || m_collection->current_is_dirty())) {
        m_last_selected = selected_item;
        on_selection_changed(selected_item);
        evt.StopPropagation();
    }
    evt.Skip();
}

PresetComboBox::~PresetComboBox()
{
}

BitmapCache& PresetComboBox::bitmap_cache()
{
    static BitmapCache bmps;
    return bmps;
}

void PresetComboBox::set_label_marker(int item, LabelItemType label_item_type)
{
    this->SetClientData(item, (void*)label_item_type);
}

bool PresetComboBox::set_printer_technology(PrinterTechnology pt)
{
    if (printer_technology != pt) {
        printer_technology = pt;
        return true;
    }
    return false;
}

void PresetComboBox::invalidate_selection()
{
    m_last_selected = INT_MAX; // this value means that no one item is selected
}

void PresetComboBox::validate_selection(bool predicate/*=false*/)
{
    if (predicate ||
        // just in case: mark m_last_selected as a first added element
        m_last_selected == INT_MAX)
        m_last_selected = GetCount() - 1;
}

void PresetComboBox::update_selection()
{
    /* If selected_preset_item is still equal to INT_MAX, it means that
     * there is no presets added to the list.
     * So, select last combobox item ("Add/Remove preset")
     */
    validate_selection();

    // An empty list leaves m_last_selected at wxNOT_FOUND (GetCount() - 1 on 0 items);
    // GetString() would assert on it.
    if (GetCount() == 0)
        return;
    SetSelection(m_last_selected);
#ifdef __WXMSW__
    // From the Windows 2004 the tooltip for preset combobox doesn't work after next call of SetTooltip()
    // (There was an issue, when tooltip doesn't appears after changing of the preset selection)
    // But this workaround seems to work: We should to kill tooltip and than set new tooltip value
    SetToolTip(NULL);
#endif
    SetToolTip(GetString(m_last_selected));

// A workaround for a set of issues related to text fitting into gtk widgets:
// See e.g.: https://github.com/prusa3d/PrusaSlicer/issues/4584
}

static std::string suffix(const Preset& preset)
{
    return (preset.is_dirty ? Preset::suffix_modified() : "");
}

static std::string suffix(Preset* preset)
{
    return (preset->is_dirty ? Preset::suffix_modified() : "");
}

wxString PresetComboBox::get_preset_name(const Preset & preset)
{
    return from_u8(preset.name);
}

static wxString get_preset_name_with_suffix(const Preset & preset)
{
    return from_u8(preset.name + Preset::suffix_modified());
}

void PresetComboBox::update(std::string select_preset_name)
{
    Freeze();
    Clear();
    invalidate_selection();

    const std::deque<Preset>& presets = m_collection->get_presets();

    struct PresetData {
        wxString        name;
        wxString        lower_name;
        wxBitmapBundle* bitmap;
        bool            enabled; // not used in incomp_presets
    };
    std::vector<PresetData> system_presets;
    std::vector<PresetData> nonsys_presets;
    std::vector<PresetData> incomp_presets;


    wxString selected = "";
    if (!presets.front().is_visible)
        set_label_marker(Append(separator(L("System presets")), NullBitmapBndl()));

    for (size_t i = 0; i < presets.size(); ++i)
    {
        const Preset& preset = presets[i];
        if (m_type == Preset::TYPE_PRINTER && preset.printer_technology() != ptSLA)
            continue;
        const bool is_compatible = preset.is_compatible;

        if ((preset.is_default && !preset.is_visible) ||
            (!m_show_all && (!preset.is_visible || !is_compatible)))
            continue;

        // marker used for disable incompatible printer models for the selected printer
        bool is_enabled = m_type == Preset::TYPE_PRINTER && printer_technology != ptAny ? preset.printer_technology() == printer_technology : true;
        if (select_preset_name.empty() && is_enabled)
            select_preset_name = preset.name;

        std::string   bitmap_key = "cb";
        if (m_type == Preset::TYPE_PRINTER) {
            bitmap_key += "_printer";
            if (preset.printer_technology() == ptSLA)
                bitmap_key += "_sla";
        }
        std::string main_icon_name = m_type == Preset::TYPE_PRINTER && preset.printer_technology() == ptSLA ? "sla_printer" : m_main_bitmap_name;

        auto bmp = get_bmp(bitmap_key, main_icon_name, "lock_closed", is_enabled, is_compatible, preset.is_system || preset.is_default);
        assert(bmp);

        if (!is_enabled) {
            incomp_presets.push_back({get_preset_name(preset), get_preset_name(preset).Lower(), bmp, false});
            if (preset.is_dirty && m_show_modif_preset_separately)
                incomp_presets.push_back({get_preset_name_with_suffix(preset), get_preset_name_with_suffix(preset).Lower(), bmp, false});
        }
        else if (preset.is_default || preset.is_system)
        {
            system_presets.push_back({ get_preset_name(preset), get_preset_name(preset).Lower(), bmp, is_enabled });
            if (preset.name == select_preset_name)
                selected = preset.name;

            if (preset.is_dirty && m_show_modif_preset_separately) {
                wxString preset_name = get_preset_name_with_suffix(preset);
                system_presets.push_back({preset_name, preset_name.Lower(), bmp, is_enabled});
                if (into_u8(preset_name) == select_preset_name)
                    selected = preset_name;
            }
        }
        else
        {
            nonsys_presets.push_back({get_preset_name(preset), get_preset_name(preset).Lower(), bmp, is_enabled});
            if (preset.name == select_preset_name || (select_preset_name.empty() && is_enabled))
                selected = get_preset_name(preset);
            if (preset.is_dirty && m_show_modif_preset_separately) {
                wxString preset_name = get_preset_name_with_suffix(preset);
                nonsys_presets.push_back({preset_name, preset_name.Lower(), bmp, is_enabled});
                if (preset_name == select_preset_name || (select_preset_name.empty() && is_enabled))
                    selected = preset_name;
            }
        }
        if (i + 1 == m_collection->num_default_presets())
            set_label_marker(Append(separator(L("System presets")), NullBitmapBndl()));
    }
    
    if (!system_presets.empty())
    {
        std::sort(system_presets.begin(), system_presets.end(), [](const PresetData& a, const PresetData& b) {
            return a.lower_name < b.lower_name;
            });

        for (std::vector<PresetData>::iterator it = system_presets.begin(); it != system_presets.end(); ++it) {
            int item_id = Append(it->name, *it->bitmap);
            if (!it->enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->name == selected);
        }
    }
    if (!nonsys_presets.empty())
    {
        std::sort(nonsys_presets.begin(), nonsys_presets.end(), [](const PresetData& a, const PresetData& b) {
            return a.lower_name < b.lower_name;
            });

        set_label_marker(Append(separator(L("User presets")), NullBitmapBndl()));
        for (std::vector<PresetData>::iterator it = nonsys_presets.begin(); it != nonsys_presets.end(); ++it) {
            int item_id = Append(it->name, *it->bitmap);
            if (!it->enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->name == selected);
        }
    }

    if (!incomp_presets.empty())
    {
        std::sort(incomp_presets.begin(), incomp_presets.end(), [](const PresetData& a, const PresetData& b) {
            return a.lower_name < b.lower_name;
            });

        set_label_marker(Append(separator(L("Incompatible presets")), NullBitmapBndl()));
        for (std::vector<PresetData>  ::iterator it = incomp_presets.begin(); it != incomp_presets.end(); ++it) {
            set_label_marker(Append(it->name, *it->bitmap), LABEL_ITEM_DISABLED);
        }
    }
    
    update_selection();
    Thaw();
}

void PresetComboBox::show_all(bool show_all)
{
    m_show_all = show_all;
    update();
}

void PresetComboBox::update()
{
    int n = this->GetSelection();
    this->update(n < 0 ? "" : into_u8(this->GetString(n)));
}

void PresetComboBox::update_from_bundle()
{
    this->update(m_collection->get_selected_preset().name);
}

void PresetComboBox::msw_rescale()
{
    m_em_unit = em_unit(this);
    ::ComboBox::Rescale();
}

void PresetComboBox::sys_color_changed()
{
    m_bitmapCompatible = get_bmp_bundle("flag_green");
    m_bitmapIncompatible = get_bmp_bundle("flag_red");
    wxGetApp().UpdateDarkUI(this);

    // update the control to redraw the icons
    update();
}

void PresetComboBox::fill_width_height()
{
    icon_height     = 16;
    norm_icon_width = 16;

    thin_icon_width = 8;
    wide_icon_width = norm_icon_width + thin_icon_width;

    null_icon_width = 2 * norm_icon_width;

    space_icon_width      = 2;
    thin_space_icon_width = 4;
    wide_space_icon_width = 6;
}

wxString PresetComboBox::separator(const std::string& label)
{
    return wxString::FromUTF8(separator_head()) + _(label) + wxString::FromUTF8(separator_tail());
}


wxBitmapBundle* PresetComboBox::get_bmp(  std::string bitmap_key, bool wide_icons, const std::string& main_icon_name,
                                    bool is_compatible/* = true*/, bool is_system/* = false*/,
                                    const std::string& material_rgb/* = ""*/)
{
    // If the preset is not compatible and there is a "red flag" icon loaded, show it left
    // to the preset icon.
    if (wide_icons)
        bitmap_key += is_compatible ? ",cmpt" : ",ncmpt";

    bitmap_key += is_system ? ",syst" : ",nsyst";
    bitmap_key += ",h" + std::to_string(icon_height);
    bool dark_mode = wxGetApp().dark_mode();
    if (dark_mode)
        bitmap_key += ",dark";
    bitmap_key += material_rgb;

    wxBitmapBundle* bmp_bndl = bitmap_cache().find_bndl(bitmap_key);
    if (bmp_bndl == nullptr) {
        // Create the bitmap with color bars.
        std::vector<wxBitmapBundle*> bmps;
        if (wide_icons)
            // Paint a red flag for incompatible presets.
            bmps.emplace_back(is_compatible ? get_empty_bmp_bundle(norm_icon_width, icon_height) : m_bitmapIncompatible);

        // Paint the color bars.
        bmps.emplace_back(get_empty_bmp_bundle(thin_space_icon_width, icon_height));
        if (m_type == Preset::TYPE_SLA_MATERIAL)
            bmps.emplace_back(bitmap_cache().from_svg(main_icon_name, 16, 16, dark_mode, material_rgb));
        else
            bmps.emplace_back(get_bmp_bundle(main_icon_name));
        // Paint a lock at the system presets.
        bmps.emplace_back(get_empty_bmp_bundle(wide_space_icon_width, icon_height));
        bmps.emplace_back(is_system ? get_bmp_bundle("lock_closed") : get_empty_bmp_bundle(norm_icon_width, icon_height));
        bmp_bndl = bitmap_cache().insert_bndl(bitmap_key, bmps);
    }

    return bmp_bndl;
}

wxBitmapBundle* PresetComboBox::get_bmp(  std::string bitmap_key, const std::string& main_icon_name, const std::string& next_icon_name,
                                    bool is_enabled/* = true*/, bool is_compatible/* = true*/, bool is_system/* = false*/)
{
    bitmap_key += !is_enabled ? "_disabled" : "";
    bitmap_key += is_compatible ? ",cmpt" : ",ncmpt";
    bitmap_key += is_system ? ",syst" : ",nsyst";
    bitmap_key += ",h" + std::to_string(icon_height);
    if (wxGetApp().dark_mode())
        bitmap_key += ",dark";

    wxBitmapBundle* bmp = bitmap_cache().find_bndl(bitmap_key);
    if (bmp == nullptr) {
        // Create the bitmap with color bars.
        std::vector<wxBitmapBundle*> bmps;
        bmps.emplace_back(m_type == Preset::TYPE_PRINTER ? get_bmp_bundle(main_icon_name) :
                          is_compatible ? m_bitmapCompatible : m_bitmapIncompatible);
        // Paint a lock at the system presets.
        bmps.emplace_back(is_system ? get_bmp_bundle(next_icon_name) : get_empty_bmp_bundle(norm_icon_width, icon_height));
        bmp = bitmap_cache().insert_bndl(bitmap_key, bmps);
    }

    return bmp;
}

wxBitmapBundle PresetComboBox::NullBitmapBndl()
{
    assert(null_icon_width > 0);
    return *get_empty_bmp_bundle(null_icon_width, icon_height);
}

// ---------------------------------
// ***  PlaterPresetComboBox  ***
// ---------------------------------

PlaterPresetComboBox::PlaterPresetComboBox(wxWindow *parent, Preset::Type preset_type) :
    PresetComboBox(parent, preset_type, wxSize(15 * wxGetApp().em_unit(), -1))
{
    edit_btn = new ScalableButton(parent, wxID_ANY, "cog");
    edit_btn->SetToolTip(_L("Click to edit preset"));

    edit_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent)
    {
        if (m_type == Preset::TYPE_PRINTER)
            show_edit_menu();
        else
            switch_to_tab();
    });

}

PlaterPresetComboBox::~PlaterPresetComboBox()
{
    if (edit_btn)
        edit_btn->Destroy();
}

static void run_wizard(ConfigWizard::StartPage sp)
{
    wxGetApp().run_wizard(ConfigWizard::RR_USER, sp);
}

void PlaterPresetComboBox::OnSelect(wxCommandEvent &evt)
{
    auto selected_item = evt.GetSelection();
    if (selected_item < 0 || size_t(selected_item) >= GetCount()) {
        evt.StopPropagation();
        return;
    }

    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    if (marker >= LABEL_ITEM_MARKER && marker < LABEL_ITEM_MAX) {
        this->SetSelection(m_last_selected);
        evt.StopPropagation();
        if (marker == LABEL_ITEM_MARKER)
            return;
        if (marker == LABEL_ITEM_WIZARD_PRINTERS)
            show_add_menu();
        else {
            ConfigWizard::StartPage sp = ConfigWizard::SP_WELCOME;
            switch (marker) {
            case LABEL_ITEM_WIZARD_MATERIALS: sp = ConfigWizard::SP_MATERIALS; break;
            default: break;
            }
            wxTheApp->CallAfter([sp]() { run_wizard(sp); });
        }
        return;
    }
    else if (m_last_selected != selected_item || m_collection->current_is_dirty())
        m_last_selected = selected_item;
        
    evt.Skip();
}

void PlaterPresetComboBox::switch_to_tab()
{
    Tab* tab = wxGetApp().get_tab(m_type);
    if (!tab)
        return;

    if (int page_id = wxGetApp().tab_panel()->FindPage(tab); page_id != wxNOT_FOUND)
    {
        wxGetApp().tab_panel()->SetSelection(page_id);
        // Switch to Settings NotePad
        wxGetApp().mainframe->select_tab();
    }
}

void PlaterPresetComboBox::show_add_menu()
{
    wxMenu* menu = new wxMenu();

    append_menu_item(menu, wxID_ANY, _L("Add/Remove presets"), "",
        [](wxCommandEvent&) {
            wxTheApp->CallAfter([]() { run_wizard(ConfigWizard::SP_PRINTERS); });
        }, "edit_uni", menu, []() { return true; }, wxGetApp().plater());


    wxGetApp().plater()->PopupMenu(menu);
}

void PlaterPresetComboBox::show_edit_menu()
{
    wxMenu* menu = new wxMenu();

    append_menu_item(menu, wxID_ANY, _L("Edit preset"), "",
        [this](wxCommandEvent&) { this->switch_to_tab(); }, "cog", menu, []() { return true; }, wxGetApp().plater());

        append_menu_item(menu, wxID_ANY, _L("Add/Remove presets"), "",
            [](wxCommandEvent&) {
                wxTheApp->CallAfter([]() { run_wizard(ConfigWizard::SP_PRINTERS); });
            }, "edit_uni", menu, []() { return true; }, wxGetApp().plater());


    wxGetApp().plater()->PopupMenu(menu);
}

wxString PlaterPresetComboBox::get_preset_name(const Preset& preset)
{
    std::string name = preset.alias.empty() ? preset.name : preset.alias;
    return from_u8(name + suffix(preset));
}


// Only the compatible presets are shown.
// If an incompatible preset is selected, it is shown as well.
void PlaterPresetComboBox::update()
{
    // Otherwise fill in the list from scratch.
    this->Freeze();
    this->Clear();
    invalidate_selection();

    // Show wide icons if the currently selected preset is not compatible with the current printer,
    // and draw a red flag in front of the selected preset.
    bool wide_icons = m_collection->get_selected_idx() != size_t(-1) && !m_collection->get_selected_preset().is_compatible;

    null_icon_width = (wide_icons ? 3 : 2) * norm_icon_width + thin_space_icon_width + wide_space_icon_width;

    struct PresetData {
        wxString        name;
        wxString        lower_name;
        wxBitmapBundle* bitmap;
    };
    std::vector<PresetData> system_presets;
    std::vector<PresetData>  nonsys_presets;


    wxString selected_user_preset;
    wxString tooltip;
    const std::deque<Preset>& presets = m_collection->get_presets();

    if (!presets.front().is_visible)
        this->set_label_marker(this->Append(separator(L("System presets")), NullBitmapBndl()));

    for (size_t i = 0; i < presets.size(); ++i)
    {
        const Preset& preset = presets[i];
        if (m_type == Preset::TYPE_PRINTER && preset.printer_technology() != ptSLA)
            continue;
        const bool is_selected =
                            i == m_collection->get_selected_idx();

        const bool is_compatible = preset.is_compatible;

        if (!preset.is_visible || (!is_compatible && !is_selected))
            continue;

        std::string bitmap_key, material_rgb;
        std::string bitmap_type_name = bitmap_key = m_type == Preset::TYPE_PRINTER && preset.printer_technology() == ptSLA ? "sla_printer" : m_main_bitmap_name;

        if (m_type == Preset::TYPE_PRINTER) {
        }

        if (m_type == Preset::TYPE_SLA_MATERIAL) {
            material_rgb = is_selected ? m_preset_bundle->sla_materials.get_edited_preset().config.opt_string("material_colour") : preset.config.opt_string("material_colour");
            if (material_rgb.empty())
                material_rgb = print_config_def.get("material_colour")->get_default_value<ConfigOptionString>()->value;
        }

        auto bmp = get_bmp(bitmap_key, wide_icons, bitmap_type_name,
                                is_compatible, preset.is_system || preset.is_default, 
                                material_rgb);
        assert(bmp);

        const std::string name = preset.alias.empty() ? preset.name : preset.alias;
        if (preset.is_default || preset.is_system) {
            system_presets.push_back({ get_preset_name(preset), get_preset_name(preset).Lower(), bmp });
            if (is_selected) {
                selected_user_preset = get_preset_name(preset);
                tooltip = from_u8(preset.name);
            }
        }
        else
        {
            nonsys_presets.push_back({ get_preset_name(preset), get_preset_name(preset).Lower(), bmp });
            if (is_selected) {
                selected_user_preset = get_preset_name(preset);
                tooltip = from_u8(preset.name);
            }
        }
        if (i + 1 == m_collection->num_default_presets())
            set_label_marker(Append(separator(L("System presets")), NullBitmapBndl()));
    }
    
    if(!system_presets.empty())
    {
        std::sort(system_presets.begin(), system_presets.end(), [](const PresetData& a, const PresetData& b) {
            return a.lower_name < b.lower_name;
            });
        
        for (std::vector<PresetData>::iterator it = system_presets.begin(); it != system_presets.end(); ++it) {
            Append(it->name, *it->bitmap);
            validate_selection(it->name == selected_user_preset);
        }
    }
    
    if (!nonsys_presets.empty())
    {
        std::sort(nonsys_presets.begin(), nonsys_presets.end(), [](const PresetData& a, const PresetData& b) {
            return a.lower_name < b.lower_name;
            });

        set_label_marker(Append(separator(L("User presets")), NullBitmapBndl()));
        for (std::vector<PresetData>::iterator it = nonsys_presets.begin(); it != nonsys_presets.end(); ++it) {
            Append(it->name, *it->bitmap);
            validate_selection(it->name == selected_user_preset);
        }
    }

    if (m_type == Preset::TYPE_PRINTER || m_type == Preset::TYPE_SLA_MATERIAL) {
        auto bmp = get_bmp("edit_preset_list", wide_icons, "edit_uni");
        assert(bmp);

        if (m_type == Preset::TYPE_SLA_MATERIAL)
            set_label_marker(Append(separator(L("Add/Remove materials")), *bmp), LABEL_ITEM_WIZARD_MATERIALS);
        else
            set_label_marker(Append(separator(L("Add/Remove printers")), *bmp), LABEL_ITEM_WIZARD_PRINTERS);
    }

    update_selection();
    Thaw();

    if (!tooltip.IsEmpty()) {
#ifdef __WXMSW__
        // From the Windows 2004 the tooltip for preset combobox doesn't work after next call of SetTooltip()
        // (There was an issue, when tooltip doesn't appears after changing of the preset selection)
        // But this workaround seems to work: We should to kill tooltip and than set new tooltip value
        // See, https://groups.google.com/g/wx-users/c/mOEe3fgHrzk
        SetToolTip(NULL);
#endif
        SetToolTip(tooltip);
    }

#ifdef __WXMSW__
    // Use this part of code just on Windows to avoid of some layout issues on Linux
    // see https://github.com/prusa3d/PrusaSlicer/issues/5163 and https://github.com/prusa3d/PrusaSlicer/issues/5505
    // Update control min size after rescale (changed Display DPI under MSW)
    if (GetMinWidth() != 20 * m_em_unit)
        SetMinSize(wxSize(20 * m_em_unit, GetSize().GetHeight()));
#endif //__WXMSW__
}

void PlaterPresetComboBox::msw_rescale()
{
    PresetComboBox::msw_rescale();
#ifdef __WXMSW__
    // Use this part of code just on Windows to avoid of some layout issues on Linux
    // see https://github.com/prusa3d/PrusaSlicer/issues/5163 and https://github.com/prusa3d/PrusaSlicer/issues/5505
    // Update control min size after rescale (changed Display DPI under MSW)
    if (GetMinWidth() != 20 * m_em_unit)
        SetMinSize(wxSize(20 * m_em_unit, GetSize().GetHeight()));
#endif //__WXMSW__
}

void PlaterPresetComboBox::sys_color_changed()
{
    PresetComboBox::sys_color_changed();
    edit_btn->sys_color_changed();
}

// ---------------------------------
// ***  TabPresetComboBox  ***
// ---------------------------------

TabPresetComboBox::TabPresetComboBox(wxWindow* parent, Preset::Type preset_type) :
    PresetComboBox(parent, preset_type, wxSize(35 * wxGetApp().em_unit(), -1))
{
}

void TabPresetComboBox::OnSelect(wxCommandEvent &evt)
{
    // see https://github.com/prusa3d/PrusaSlicer/issues/3889
    // Under OSX: in case of use of a same names written in different case (like "ENDER" and "Ender")
    // m_presets_choice->GetSelection() will return first item, because search in PopupListCtrl is case-insensitive.
    // So, use GetSelection() from event parameter 
    auto selected_item = evt.GetSelection();
    if (selected_item < 0 || size_t(selected_item) >= GetCount()) {
        evt.StopPropagation();
        return;
    }

    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX) {
        this->SetSelection(m_last_selected);
        if (marker == LABEL_ITEM_WIZARD_PRINTERS)
            wxTheApp->CallAfter([this]() {
            run_wizard(ConfigWizard::SP_PRINTERS);
        });
    }
    else if (on_selection_changed && (m_last_selected != selected_item || m_collection->current_is_dirty())) {
        m_last_selected = selected_item;
        on_selection_changed(selected_item);
    }

    evt.StopPropagation();
#ifdef __WXMSW__
    // From the Win 2004 preset combobox lose a focus after change the preset selection
    // and that is why the up/down arrow doesn't work properly
    // (see https://github.com/prusa3d/PrusaSlicer/issues/5531 ).
    // So, set the focus to the combobox explicitly
    this->SetFocus();
#endif
}

wxString TabPresetComboBox::get_preset_name(const Preset& preset)
{
    return from_u8(preset.name + suffix(preset));
}

// Update the choice UI from the list of presets.
// If show_incompatible, all presets are shown, otherwise only the compatible presets are shown.
// If an incompatible preset is selected, it is shown as well.
void TabPresetComboBox::update()
{
    Freeze();
    Clear();
    invalidate_selection();

    const std::deque<Preset>& presets = m_collection->get_presets();
    
    struct PresetData {
        wxString        name;
        wxString        lower_name;
        wxBitmapBundle* bitmap;
        bool            enabled;
    };
    std::vector<PresetData> system_presets;
    std::vector<PresetData> nonsys_presets;

    wxString selected = "";
    if (!presets.front().is_visible)
        set_label_marker(Append(separator(L("System presets")), NullBitmapBndl()));
    size_t idx_selected = m_collection->get_selected_idx();

    for (size_t i = 0; i < presets.size(); ++i)
    {
        const Preset& preset = presets[i];
        if (m_type == Preset::TYPE_PRINTER && preset.printer_technology() != ptSLA)
            continue;

        const bool is_compatible = preset.is_compatible;

        if (!preset.is_visible || (!show_incompatible && !is_compatible && i != idx_selected))
            continue;
        
        // marker used for disable incompatible printer models for the selected printer
        bool is_enabled = true;

        std::string bitmap_key = "tab";
        if (m_type == Preset::TYPE_PRINTER) {
            bitmap_key += "_printer";
            if (preset.printer_technology() == ptSLA)
                bitmap_key += "_sla";
        }
        std::string main_icon_name = m_type == Preset::TYPE_PRINTER && preset.printer_technology() == ptSLA ? "sla_printer" : m_main_bitmap_name;

        auto bmp = get_bmp(bitmap_key, main_icon_name, "lock_closed", is_enabled, is_compatible, preset.is_system || preset.is_default);
        assert(bmp);

        if (preset.is_default || preset.is_system) {
            system_presets.push_back({get_preset_name(preset), get_preset_name(preset).Lower(), bmp, is_enabled});
            if (i == idx_selected)
                selected = get_preset_name(preset);
        }
        else
        {
            std::pair<wxBitmapBundle*, bool> pair(bmp, is_enabled);
            nonsys_presets.push_back({get_preset_name(preset), get_preset_name(preset).Lower(), bmp, is_enabled});
            if (i == idx_selected)
                selected = get_preset_name(preset);
        }
        if (i + 1 == m_collection->num_default_presets())
            set_label_marker(Append(separator(L("System presets")), NullBitmapBndl()));
    }
   
    if (!system_presets.empty()) 
    {
        std::sort(system_presets.begin(), system_presets.end(), [](const PresetData& a, const PresetData& b) {
            return a.lower_name < b.lower_name;
            });

        for (std::vector<PresetData>::iterator it = system_presets.begin(); it != system_presets.end(); ++it) {
            int item_id = Append(it->name, *it->bitmap);
            if (!it->enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->name == selected);
        }
    }
    
    if (!nonsys_presets.empty())
    {
        std::sort(nonsys_presets.begin(), nonsys_presets.end(), [](const PresetData& a, const PresetData& b) {
            return a.lower_name < b.lower_name;
            });

        set_label_marker(Append(separator(L("User presets")), NullBitmapBndl()));
        for (std::vector<PresetData>::iterator it = nonsys_presets.begin(); it != nonsys_presets.end(); ++it) {
            int item_id = Append(it->name, *it->bitmap);
            if (!it->enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->name == selected);
        }
    }

    if (m_type == Preset::TYPE_PRINTER)
    {
        // add "Add/Remove printers" item
        std::string icon_name = "edit_uni";
        auto bmp = get_bmp("edit_preset_list, tab,", icon_name, "");
        assert(bmp);

        set_label_marker(Append(separator(L("Add/Remove printers")), *bmp), LABEL_ITEM_WIZARD_PRINTERS);
    }

    update_selection();
    Thaw();
}

void TabPresetComboBox::msw_rescale()
{
    PresetComboBox::msw_rescale();
    wxSize sz = wxSize(35 * m_em_unit, -1);
    SetMinSize(sz);
    SetSize(sz);
}

void TabPresetComboBox::update_dirty()
{
    // 1) Update the dirty flag of the current preset.
    m_collection->update_dirty();

    // 2) Update the labels.
    wxWindowUpdateLocker noUpdates(this);
    for (unsigned int ui_id = 0; ui_id < GetCount(); ++ui_id) {
        auto marker = reinterpret_cast<Marker>(this->GetClientData(ui_id));
        if (marker >= LABEL_ITEM_MARKER)
            continue;

        std::string   old_label = GetString(ui_id).utf8_str().data();
        std::string   preset_name = Preset::remove_suffix_modified(old_label);

        Preset* preset = m_collection->find_preset(preset_name, false);
        if (preset) {
            std::string new_label = preset->name + suffix(preset);

            if (old_label != new_label)
                SetString(ui_id, from_u8(new_label));
        }
    }
#ifdef __APPLE__
    // wxWidgets on OSX do not upload the text of the combo box line automatically.
    // Force it to update by re-selecting.
    SetSelection(GetSelection());
#endif /* __APPLE __ */
}

}}    // namespace Slic3r::GUI
