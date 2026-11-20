#include "libslic3r/SLA/ResinBottle.hpp"
#include "libslic3r/SLA/ExportFormatPreference.hpp"
#include "libslic3r/SLA/GeneratedLayerOwnership.hpp"
#include "libslic3r/SLA/AnycubicSettingsPreflight.hpp"
///|/ Copyright (c) Prusa Research 2017 - 2023 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv, Pavel Mikuš @Godrak, Tomáš Mészáros @tamasmeszaros, David Kocík @kocikdav, Enrico Turri @enricoturri1966, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2021 Martin Budden
///|/ Copyright (c) 2021 Ilya @xorza
///|/ Copyright (c) 2019 John Drake @foxox
///|/ Copyright (c) 2019 Matthias Urlichs @smurfix
///|/ Copyright (c) 2019 Thomas Moore
///|/ Copyright (c) 2019 Sijmen Schoon
///|/ Copyright (c) 2018 Martin Loidl @LoidlM
///|/
///|/ ported from lib/Slic3r/GUI/Tab.pm:
///|/ Copyright (c) Prusa Research 2016 - 2018 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/ Copyright (c) 2015 - 2017 Joseph Lenox @lordofhyphens
///|/ Copyright (c) Slic3r 2012 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) 2012 QuantumConcepts
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "slic3r/GUI/BedShapeDialog.hpp"
#include <set>
#include "Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/PrinterGeometry.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/NanoDLPArchive.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include <nlohmann/json.hpp>
#include <boost/nowide/fstream.hpp>
#include "libslic3r/Thumbnails/Thumbnails.hpp"
#include "libslic3r/CustomParametersHandling.hpp"

#include "slic3r/Utils/Http.hpp"
#include "ButtonsDescription.hpp"
#include "Search.hpp"
#include "OG_CustomCtrl.hpp"

#include <tuple>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/settings.h>
#include <wx/filedlg.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/exception/diagnostic_information.hpp>

#include "wxExtensions.hpp"
#include "PresetComboBoxes.hpp"
#include <wx/wupdlock.h>

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "UnsavedChangesDialog.hpp"
#include "SavePresetDialog.hpp"
#include "MsgDialog.hpp"
#include "TopBar.hpp"

#include "Widgets/CheckBox.hpp"

#ifdef WIN32
	#include <CommCtrl.h>
#endif // WIN32

namespace Slic3r {
namespace GUI {

Tab::Tab(wxBookCtrlBase* parent, const wxString& title, Preset::Type type) :
    m_parent(parent), m_type(type), m_title(title)
{
    Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL/*, name*/);
    this->SetFont(Slic3r::GUI::wxGetApp().normal_font());

#ifdef __WXMSW__
    wxGetApp().UpdateDarkUI(this);
#elif __WXOSX__
    SetBackgroundColour(parent->GetBackgroundColour());
#elif __WXGTK3__
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif

    m_compatible_printers.type			= Preset::TYPE_PRINTER;
    m_compatible_printers.key_list		= "compatible_printers";
    m_compatible_printers.key_condition	= "compatible_printers_condition";
    m_compatible_printers.dialog_title  = _L("Compatible printers");
    m_compatible_printers.dialog_label  = _L("Select the printers this profile is compatible with.");

    m_compatible_prints.type			= Preset::TYPE_SLA_PRINT;
    m_compatible_prints.key_list 		= "compatible_prints";
    m_compatible_prints.key_condition	= "compatible_prints_condition";
    m_compatible_prints.dialog_title 	= _L("Compatible print profiles");
    m_compatible_prints.dialog_label 	= _L("Select the print profiles this profile is compatible with.");

    wxGetApp().tabs_list.push_back(this);

    m_em_unit = em_unit(m_parent); //wxGetApp().em_unit();

    m_config_manipulation = get_config_manipulation();

    Bind(wxEVT_SIZE, ([](wxSizeEvent &evt) {
        //for (auto page : m_pages)
        //    if (! page.get()->IsShown())
        //        page->layout_valid = false;
        evt.Skip();
    }));

    m_highlighter.set_timer_owner(this, 0);
}

void Tab::set_type()
{
    if (m_name == "sla_print")     { m_type = Slic3r::Preset::TYPE_SLA_PRINT; }
    else if (m_name == "sla_material")  { m_type = Slic3r::Preset::TYPE_SLA_MATERIAL; }
    else if (m_name == "printer")       { m_type = Slic3r::Preset::TYPE_PRINTER; }
    else                                { m_type = Slic3r::Preset::TYPE_INVALID; assert(false); }
}

// sub new
void Tab::create_preset_tab()
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_preset_bundle = wxGetApp().preset_bundle;

    // Vertical sizer to hold the choice menu and the rest of the page.
#ifdef __WXOSX__
    auto  *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->SetSizeHints(this);
    this->SetSizer(main_sizer);

    // Create additional panel to Fit() it from OnActivate()
    // It's needed for tooltip showing on OSX
    m_tmp_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    auto panel = m_tmp_panel;
    auto  sizer = new wxBoxSizer(wxVERTICAL);
    m_tmp_panel->SetSizer(sizer);
    m_tmp_panel->Layout();

    main_sizer->Add(m_tmp_panel, 1, wxEXPAND | wxALL, 0);
#else
    Tab *panel = this;
    auto  *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(panel);
    panel->SetSizer(sizer);
#endif //__WXOSX__

    // preset chooser
    m_presets_choice = new TabPresetComboBox(panel, m_type);
    m_presets_choice->set_selection_changed_function([this](int selection) {
        std::string preset_name = m_presets_choice->GetString(selection).ToUTF8().data();
        select_preset(Preset::remove_suffix_modified(preset_name));
    });

    //buttons
    m_scaled_buttons.reserve(6);
    m_scaled_buttons.reserve(2);

    add_scaled_button(panel, &m_btn_compare_preset, "compare");
    add_scaled_button(panel, &m_btn_save_preset, "save");
    add_scaled_button(panel, &m_btn_rename_preset, "edit");
    add_scaled_button(panel, &m_btn_delete_preset, "cross");
    // Single-profile exchange is available for the SLA profile types only.
    add_scaled_button(panel, &m_btn_export_preset, "export_config");
    add_scaled_button(panel, &m_btn_import_preset, "import_config");

    m_show_incompatible_presets = false;

    add_scaled_button(panel, &m_btn_hide_incompatible_presets, "flag_green");

    //TRN Settings Tab: tooltip for toolbar button
    m_btn_compare_preset->SetToolTip(_L("Compare preset with another"));
    //TRN Settings Tab: tooltip for toolbar button
    m_btn_save_preset  ->SetToolTip(_L("Save preset"));
    //TRN Settings Tab: tooltip for toolbar button
    m_btn_rename_preset->SetToolTip(_L("Rename preset"));
    m_btn_rename_preset->Hide();
    //TRN Settings Tab: tooltip for toolbar button
    m_btn_delete_preset->SetToolTip(_(L("Delete preset")));
    m_btn_delete_preset->Hide();
    m_btn_export_preset->SetToolTip(_L("Export this preset to an .ini file"));
    m_btn_import_preset->SetToolTip(_L("Import a preset from an .ini file"));

    add_scaled_button(panel, &m_question_btn, "question");
    m_question_btn->SetToolTip(_(L("Hover the cursor over buttons to find more information \n"
                                   "or click this button.")));

    // Bitmaps to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    add_scaled_bitmap(this, m_bmp_value_lock  , "lock_closed");
    add_scaled_bitmap(this, m_bmp_value_unlock, "lock_open");
    m_bmp_non_system = &m_bmp_white_bullet;
    // Bitmaps to be shown on the "Undo user changes" button next to each input field.
    add_scaled_bitmap(this, m_bmp_value_revert, "undo");
    add_scaled_bitmap(this, m_bmp_white_bullet, "dot");
    // Bitmap to be shown on the "edit" button before to each editable input field.
    add_scaled_bitmap(this, m_bmp_edit_value, "edit");

    fill_icon_descriptions();
    set_tooltips_text();

    add_scaled_button(panel, &m_undo_btn,        m_bmp_white_bullet.name());
    add_scaled_button(panel, &m_undo_to_sys_btn, m_bmp_white_bullet.name());

    m_undo_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent) { on_roll_back_value(); }));
    m_undo_to_sys_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent) { on_roll_back_value(true); }));
    m_question_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent) {
        GUI_Descriptions::Dialog dlg(this, m_icon_descriptions);
        if (dlg.ShowModal() == wxID_OK)
            wxGetApp().update_label_colours();
    });

    // Colors for ui "decoration"
    m_sys_label_clr			= wxGetApp().get_label_clr_sys();
    m_modified_label_clr	= wxGetApp().get_label_clr_modified();
    m_default_text_clr		= wxGetApp().get_label_clr_default();

    const float scale_factor = em_unit(this)*0.1;// GetContentScaleFactor();
    m_top_hsizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_top_hsizer, 0, wxEXPAND | wxBOTTOM, 3);
    m_top_hsizer->Add(m_presets_choice, 0, wxLEFT | wxRIGHT | wxTOP | wxALIGN_CENTER_VERTICAL, 3);
    m_top_hsizer->AddSpacer(int(4*scale_factor));

    m_h_buttons_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_h_buttons_sizer->Add(m_btn_save_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(4*scale_factor));
    m_h_buttons_sizer->Add(m_btn_rename_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(4 * scale_factor));
    m_h_buttons_sizer->Add(m_btn_delete_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(4 * scale_factor));
    m_h_buttons_sizer->Add(m_btn_export_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(4 * scale_factor));
    m_h_buttons_sizer->Add(m_btn_import_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(/*16*/8 * scale_factor));
    m_h_buttons_sizer->Add(m_btn_hide_incompatible_presets, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(8 * scale_factor));
    m_h_buttons_sizer->Add(m_question_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(32 * scale_factor));
    m_h_buttons_sizer->Add(m_undo_to_sys_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->Add(m_undo_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_h_buttons_sizer->AddSpacer(int(8*scale_factor));
    m_h_buttons_sizer->Add(m_btn_compare_preset, 0, wxALIGN_CENTER_VERTICAL);

    m_top_hsizer->Add(m_h_buttons_sizer, 1, wxEXPAND);
    m_top_hsizer->AddSpacer(int(16*scale_factor));
    // StretchSpacer has a strange behavior under OSX, so
    // hide whole top sizer to correct layout later
    m_top_hsizer->ShowItems(false);

    //Horizontal sizer to hold the tree and the selected page.
    m_hsizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_hsizer, 1, wxEXPAND, 0);

    //left vertical sizer
    m_left_sizer = new wxBoxSizer(wxVERTICAL);
    m_hsizer->Add(m_left_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 3);

    // tree
    m_treectrl = new wxTreeCtrl(panel, wxID_ANY, wxDefaultPosition, wxSize(20 * m_em_unit, -1),
        wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_SUNKEN | wxWANTS_CHARS);
    m_treectrl->SetFont(wxGetApp().normal_font());
#ifdef __linux__
    m_treectrl->SetBackgroundColour(m_parent->GetBackgroundColour());
#endif
    m_left_sizer->Add(m_treectrl, 1, wxEXPAND);
    // Index of the last icon inserted into m_treectrl
    m_icon_count = -1;
    m_treectrl->AddRoot("root");
    m_treectrl->SetIndent(0);
    wxGetApp().UpdateDarkUI(m_treectrl);

    // Delay processing of the following handler until the message queue is flushed.
    // This helps to process all the cursor key events on Windows in the tree control,
    // so that the cursor jumps to the last item.
    m_treectrl->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) {
#ifdef __linux__
        // Events queue is opposite On Linux. wxEVT_SET_FOCUS invokes after wxEVT_TREE_SEL_CHANGED,
        // and a result wxEVT_KILL_FOCUS doesn't invoke for the TextCtrls.
        // see https://github.com/prusa3d/PrusaSlicer/issues/5720
        // So, call SetFocus explicitly for this control before changing of the selection
        m_treectrl->SetFocus();
#endif
            if (!m_disable_tree_sel_changed_event && !m_pages.empty()) {
                if (m_page_switch_running)
                    m_page_switch_planned = true;
                else {
                    m_page_switch_running = true;
                    do {
                        m_page_switch_planned = false;
                        m_treectrl->Update();
                    } while (this->tree_sel_change_delayed());
                    m_page_switch_running = false;
                }
            }
        });

    m_treectrl->Bind(wxEVT_KEY_DOWN, &Tab::OnKeyDown, this);

    // Initialize the page.
#ifdef __WXOSX__
    auto page_parent = m_tmp_panel;
#else
    auto page_parent = this;
#endif

    m_page_view = new wxScrolledWindow(page_parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);
    m_page_view->SetSizer(m_page_sizer);
    m_page_view->SetScrollbars(1, 20, 1, 2);
    m_hsizer->Add(m_page_view, 1, wxEXPAND | wxLEFT, 5);

    m_btn_compare_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { compare_preset(); }));
    // Let GTK finish the activation event before opening a modal dialog;
    // keyboard/accessibility activation must remain responsive inside it.
    m_btn_save_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { CallAfter([this] { save_preset(); }); }));
    m_btn_rename_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { CallAfter([this] { rename_preset(); }); }));
    m_btn_export_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { CallAfter([this] { export_preset_ini(); }); }));
    m_btn_import_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { CallAfter([this] { import_preset_ini(); }); }));
    m_btn_delete_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { CallAfter([this] { delete_preset(); }); }));
    m_btn_hide_incompatible_presets->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) {
        toggle_show_hide_incompatible();
    }));

    // Initialize the DynamicPrintConfig by default keys/values.
    build();

    if (!m_scaled_icons_list.empty()) {
        // update icons for tree_ctrl
        wxVector<wxBitmapBundle> img_bundles;
        for (const ScalableBitmap& bmp : m_scaled_icons_list)
            img_bundles.push_back(bmp.bmp());
        m_treectrl->SetImages(img_bundles);
    }

    // ys_FIXME: Following should not be needed, the function will be called later
    // (update_mode->update_visibility->rebuild_page_tree). This does not work, during the
    // second call of rebuild_page_tree m_treectrl->GetFirstVisibleItem(); returns zero
    // for some unknown reason (and the page is not refreshed until user does a selection).
    rebuild_page_tree();

    m_completed = true;
}

void Tab::add_scaled_button(wxWindow* parent,
                            ScalableButton** btn,
                            const std::string& icon_name,
                            const wxString& label/* = wxEmptyString*/,
                            long style /*= wxBU_EXACTFIT | wxNO_BORDER*/)
{
    *btn = new ScalableButton(parent, wxID_ANY, icon_name, label, wxDefaultSize, wxDefaultPosition, style);
    m_scaled_buttons.push_back(*btn);
}

void Tab::add_scaled_bitmap(wxWindow* parent,
                            ScalableBitmap& bmp,
                            const std::string& icon_name)
{
    bmp = ScalableBitmap(parent, icon_name);
    m_scaled_bitmaps.push_back(&bmp);
}

void Tab::load_initial_data()
{
    m_config = &m_presets->get_edited_preset().config;
    bool has_parent = m_presets->get_selected_preset_parent() != nullptr;
    m_bmp_non_system = has_parent ? &m_bmp_value_unlock : &m_bmp_white_bullet;
    m_ttg_non_system = has_parent ? &m_ttg_value_unlock : &m_ttg_white_bullet_ns;
    m_tt_non_system  = has_parent ? &m_tt_value_unlock  : &m_ttg_white_bullet_ns;
}

Slic3r::GUI::PageShp Tab::add_options_page(const wxString& title, const std::string& icon)
{
    // Index of icon in an icon list $self->{icons}.
    auto icon_idx = 0;
    if (!icon.empty()) {
        icon_idx = (m_icon_index.find(icon) == m_icon_index.end()) ? -1 : m_icon_index.at(icon);
        if (icon_idx == -1) {
            // Add a new icon to the icon list.
            m_scaled_icons_list.push_back(ScalableBitmap(this, icon));
            icon_idx = ++m_icon_count;
            m_icon_index[icon] = icon_idx;
        }

        if (m_category_icon.find(title) == m_category_icon.end()) {
            // Add new category to the category_to_icon list.
            m_category_icon[title] = icon;
        }
    }
    // Initialize the page.
    PageShp page(new Page(m_page_view, title, icon_idx));

    m_pages.push_back(page);

    page->set_config(m_config);
    return page;
}

// Names of categories is save in English always. We translate them only for UI.
wxString Tab::translate_category(const wxString& title, Preset::Type /*preset_type*/)
{
    return _(title);
}

void Tab::OnActivate()
{
    wxWindowUpdateLocker noUpdates(this);
#ifdef __WXOSX__
//    wxWindowUpdateLocker noUpdates(this);
    auto size = GetSizer()->GetSize();
    m_tmp_panel->GetSizer()->SetMinSize(size.x + m_size_move, size.y);
    Fit();
    m_size_move *= -1;
#endif // __WXOSX__

#ifdef __WXMSW__
    // Workaround for tooltips over Tree Controls displayed over excessively long
    // tree control items, stealing the window focus.
    //
    // In case the Tab was reparented from the MainFrame to the floating dialog,
    // the tooltip created by the Tree Control before reparenting is not reparented, 
    // but it still points to the MainFrame. If the tooltip pops up, the MainFrame 
    // is incorrectly focussed, stealing focus from the floating dialog.
    //
    // The workaround is to delete the tooltip control.
    // Vojtech tried to reparent the tooltip control, but it did not work,
    // and if the Tab was later reparented back to MainFrame, the tooltip was displayed
    // at an incorrect position, therefore it is safer to just discard the tooltip control
    // altogether.
    HWND hwnd_tt = TreeView_GetToolTips(m_treectrl->GetHandle());
    if (hwnd_tt) {
	    HWND hwnd_toplevel 	= find_toplevel_parent(m_treectrl)->GetHandle();
	    HWND hwnd_parent 	= ::GetParent(hwnd_tt);
	    if (hwnd_parent != hwnd_toplevel) {
	    	::DestroyWindow(hwnd_tt);
			TreeView_SetToolTips(m_treectrl->GetHandle(), nullptr);
	    }
    }
#endif

    // create controls on active page
    activate_selected_page([](){});
    m_hsizer->Layout();

    if (m_presets_choice->IsShown())
        Refresh(); // Just refresh page, if m_presets_choice is already shown
    else {
        // From the tab creation whole top sizer is hidden to correct update of preset combobox's size
        // (see https://github.com/prusa3d/PrusaSlicer/issues/10746)

        // On first OnActivate call show top sizer
        m_top_hsizer->ShowItems(true);
        // Size and layouts of all items are correct now,
        // but ALL items of top sizer are visible.
        // So, update visibility of each item according to the ui settings
        update_btns_enabling();
        m_btn_hide_incompatible_presets->Show(m_show_btn_incompatible_presets && m_type != Slic3r::Preset::TYPE_PRINTER);

        Layout();
    }
}

void Tab::update_label_colours()
{
    m_default_text_clr = wxGetApp().get_label_clr_default();
    if (m_sys_label_clr == wxGetApp().get_label_clr_sys() && m_modified_label_clr == wxGetApp().get_label_clr_modified())
        return;
    m_sys_label_clr = wxGetApp().get_label_clr_sys();
    m_modified_label_clr = wxGetApp().get_label_clr_modified();

    //update options "decoration"
    for (const auto& opt : m_options_list)
    {
        const wxColour *color = &m_sys_label_clr;

        // value isn't equal to system value
        if ((opt.second & osSystemValue) == 0) {
            // value is equal to last saved
            if ((opt.second & osInitValue) != 0)
                color = &m_default_text_clr;
            // value is modified
            else
                color = &m_modified_label_clr;
        }
        if (OptionsGroup::is_option_without_field(opt.first)) {
            if (Line* line = get_line(opt.first))
                line->set_label_colour(color);
            continue;
        }

        Field* field = get_field(opt.first);
        if (field == nullptr) continue;
        field->set_label_colour(color);
    }

    auto cur_item = m_treectrl->GetFirstVisibleItem();
    if (!cur_item || !m_treectrl->IsVisible(cur_item))
        return;
    while (cur_item) {
        auto title = m_treectrl->GetItemText(cur_item);
        for (auto page : m_pages)
        {
            if (translate_category(page->title(), m_type) != title)
                continue;

            const wxColor *clr = !page->m_is_nonsys_values ? &m_sys_label_clr :
                page->m_is_modified_values ? &m_modified_label_clr :
                &m_default_text_clr;

            m_treectrl->SetItemTextColour(cur_item, *clr);
            break;
        }
        cur_item = m_treectrl->GetNextVisible(cur_item);
    }

    decorate();
}

void Tab::decorate()
{
    for (const auto& opt : m_options_list)
    {
        Field*      field = nullptr;
        bool        option_without_field = false;

        if(OptionsGroup::is_option_without_field(opt.first))
            option_without_field = true;

        if (!option_without_field) {
            field = get_field(opt.first);
            if (!field)
                continue;
        }

        bool is_nonsys_value = false;
        bool is_modified_value = true;
        const ScalableBitmap* sys_icon  = &m_bmp_value_lock;
        const ScalableBitmap* icon      = &m_bmp_value_revert;

        const wxColour* color = m_is_default_preset ? &m_default_text_clr : &m_sys_label_clr;

        const wxString* sys_tt  = &m_tt_value_lock;
        const wxString* tt      = &m_tt_value_revert;

        // value isn't equal to system value
        if ((opt.second & osSystemValue) == 0) {
            is_nonsys_value = true;
            sys_icon = m_bmp_non_system;
            sys_tt = m_tt_non_system;
            // value is equal to last saved
            if ((opt.second & osInitValue) != 0)
                color = &m_default_text_clr;
            // value is modified
            else
                color = &m_modified_label_clr;
        }
        if ((opt.second & osInitValue) != 0)
        {
            is_modified_value = false;
            icon = &m_bmp_white_bullet;
            tt = &m_tt_white_bullet;
        }

        if (option_without_field) {
            if (Line* line = get_line(opt.first)) {
                line->set_undo_bitmap(icon);
                line->set_undo_to_sys_bitmap(sys_icon);
                line->set_undo_tooltip(tt);
                line->set_undo_to_sys_tooltip(sys_tt);
                line->set_label_colour(color);
            }
            continue;
        }
        
        field->m_is_nonsys_value = is_nonsys_value;
        field->m_is_modified_value = is_modified_value;
        field->set_undo_bitmap(icon);
        field->set_undo_to_sys_bitmap(sys_icon);
        field->set_undo_tooltip(tt);
        field->set_undo_to_sys_tooltip(sys_tt);
        field->set_label_colour(color);

        if (field->has_edit_ui())
            field->set_edit_bitmap(&m_bmp_edit_value);

    }

    if (m_active_page)
        m_active_page->refresh();
}

// Update UI according to changes
void Tab::update_changed_ui()
{
    if (m_postpone_update_ui)
        return;

    auto dirty_options = m_presets->current_dirty_options(true);
    auto nonsys_options = m_presets->current_different_from_parent_options(true);
    if (m_type == Preset::TYPE_PRINTER) {
        {
            auto check_bed_custom_options = [](std::vector<std::string>& keys) {
                size_t old_keys_size = keys.size();
                keys.erase(std::remove_if(keys.begin(), keys.end(), [](const std::string& key) { 
                    return key == "bed_custom_texture" || key == "bed_custom_model"; }), keys.end());
                if (old_keys_size != keys.size() && std::find(keys.begin(), keys.end(), "bed_shape") == keys.end())
                    keys.emplace_back("bed_shape");
            };
            check_bed_custom_options(dirty_options);
            check_bed_custom_options(nonsys_options);
        }
    }

    for (auto& it : m_options_list)
        it.second = m_opt_status_value;

    for (auto opt_key : dirty_options)	m_options_list[opt_key] &= ~osInitValue;
    for (auto opt_key : nonsys_options)	m_options_list[opt_key] &= ~osSystemValue;

    decorate();

    wxTheApp->CallAfter([this]() {
        if (parent()) //To avoid a crash, parent should be exist for a moment of a tree updating
            update_changed_tree_ui();
    });
}

void Tab::init_options_list()
{
    m_options_list.clear();

    for (const std::string& opt_key : m_config->keys())
        emplace_option(opt_key, !PresetCollection::is_single_input_vector_option(opt_key));
}

template<class T>
void add_correct_opts_to_options_list(const std::string &opt_key, std::map<std::string, int>& map, Tab *tab, int value)
{
    T *opt_cur = static_cast<T*>(tab->m_config->option(opt_key));
    for (size_t i = 0; i < opt_cur->values.size(); i++)
        map.emplace(opt_key + "#" + std::to_string(i), value);
}

void Tab::emplace_option(const std::string& opt_key, bool respect_vec_values/* = false*/)
{
    if (respect_vec_values) {
        switch (m_config->option(opt_key)->type())
        {
        case coInts:	add_correct_opts_to_options_list<ConfigOptionInts		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coBools:	add_correct_opts_to_options_list<ConfigOptionBools		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloats:	add_correct_opts_to_options_list<ConfigOptionFloats		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coStrings:	add_correct_opts_to_options_list<ConfigOptionStrings	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPercents:add_correct_opts_to_options_list<ConfigOptionPercents	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPoints:	add_correct_opts_to_options_list<ConfigOptionPoints		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloatsOrPercents:	add_correct_opts_to_options_list<ConfigOptionFloatsOrPercents		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coEnums:	add_correct_opts_to_options_list<ConfigOptionEnumsGeneric>(opt_key, m_options_list, this, m_opt_status_value);	break;
        default:		m_options_list.emplace(opt_key, m_opt_status_value);		break;
        }
    }
    else 
        m_options_list.emplace(opt_key, m_opt_status_value);
}

void Tab::get_sys_and_mod_flags(const std::string& opt_key, bool& sys_page, bool& modified_page)
{
    auto opt = m_options_list.find(opt_key);
    if (opt == m_options_list.end()) 
        return;

    if (sys_page) sys_page = (opt->second & osSystemValue) != 0;
    modified_page |= (opt->second & osInitValue) == 0;
}

void Tab::update_changed_tree_ui()
{
    if (m_options_list.empty())
        return;
    auto cur_item = m_treectrl->GetFirstVisibleItem();
    if (!cur_item || !m_treectrl->IsVisible(cur_item))
        return;

    auto selected_item = m_treectrl->GetSelection();
    auto selection = selected_item ? m_treectrl->GetItemText(selected_item) : "";

    while (cur_item) {
        auto title = m_treectrl->GetItemText(cur_item);
        for (auto page : m_pages)
        {
            if (translate_category(page->title(), m_type) != title)
                continue;
            bool sys_page = true;
            bool modified_page = false;
            if (page->title() == "General") {
                std::initializer_list<const char*> optional_keys{ "bed_shape" };
                for (auto &opt_key : optional_keys) {
                    get_sys_and_mod_flags(opt_key, sys_page, modified_page);
                }
            }
            if (page->title() == "Dependencies") {
                if (m_type == Slic3r::Preset::TYPE_PRINTER) {
                    sys_page = m_presets->get_selected_preset_parent() != nullptr;
                    modified_page = false;
                } else {
                    if (m_type == Slic3r::Preset::TYPE_SLA_MATERIAL)
                        get_sys_and_mod_flags("compatible_prints", sys_page, modified_page);
                    get_sys_and_mod_flags("compatible_printers", sys_page, modified_page);
                }
            }
            for (auto group : page->m_optgroups)
            {
                if (!sys_page && modified_page)
                    break;
                for (const auto &kvp : group->opt_map()) {
                    const std::string& opt_key = kvp.first;
                    get_sys_and_mod_flags(opt_key, sys_page, modified_page);
                }
            }

            const wxColor *clr = sys_page		?	(m_is_default_preset ? &m_default_text_clr : &m_sys_label_clr) :
                                 modified_page	?	&m_modified_label_clr :
                                                    &m_default_text_clr;

            if (page->set_item_colour(clr))
                m_treectrl->SetItemTextColour(cur_item, *clr);

            page->m_is_nonsys_values = !sys_page;
            page->m_is_modified_values = modified_page;

            if (selection == title) {
                m_is_nonsys_values = page->m_is_nonsys_values;
                m_is_modified_values = page->m_is_modified_values;
            }
            break;
        }
        auto next_item = m_treectrl->GetNextVisible(cur_item);
        cur_item = next_item;
    }
    update_undo_buttons();
}

void Tab::update_undo_buttons()
{
    m_undo_btn->        SetBitmap_(m_is_modified_values ? m_bmp_value_revert.name(): m_bmp_white_bullet.name());
    m_undo_to_sys_btn-> SetBitmap_(m_is_nonsys_values   ? m_bmp_non_system->name() : m_bmp_value_lock.name());

    //m_undo_btn->        SetBitmap_(m_is_modified_values ? m_bmp_value_revert: m_bmp_white_bullet);
    //m_undo_to_sys_btn-> SetBitmap_(m_is_nonsys_values   ? *m_bmp_non_system : m_bmp_value_lock);

    m_undo_btn->SetToolTip(m_is_modified_values ? m_ttg_value_revert : m_ttg_white_bullet);
    m_undo_to_sys_btn->SetToolTip(m_is_nonsys_values ? *m_ttg_non_system : m_ttg_value_lock);
}

void Tab::on_roll_back_value(const bool to_sys /*= true*/)
{
    if (!m_active_page) return;

    int os;
    if (to_sys)	{
        if (!m_is_nonsys_values) return;
        os = osSystemValue;
    }
    else {
        if (!m_is_modified_values) return;
        os = osInitValue;
    }

    m_postpone_update_ui = true;

    for (auto group : m_active_page->m_optgroups) {
        if (group->title == "Size and coordinates") {
            if ((m_options_list["bed_shape"] & os) == 0) {
                to_sys ? group->back_to_sys_value("bed_shape") : group->back_to_initial_value("bed_shape");
                load_key_value("bed_shape", true/*some value*/, true);
            }
        }
        if (group->title == "Profile dependencies") {
            // "compatible_printers" option doesn't exists in Printer Settimgs Tab
            if (m_type != Preset::TYPE_PRINTER && (m_options_list["compatible_printers"] & os) == 0) {
                to_sys ? group->back_to_sys_value("compatible_printers") : group->back_to_initial_value("compatible_printers");
                load_key_value("compatible_printers", true/*some value*/, true);
            }
            // "compatible_prints" option exists only in Materials Tab
            if (m_type == Preset::TYPE_SLA_MATERIAL && (m_options_list["compatible_prints"] & os) == 0) {
                to_sys ? group->back_to_sys_value("compatible_prints") : group->back_to_initial_value("compatible_prints");
                load_key_value("compatible_prints", true/*some value*/, true);
            }
        }
        for (const auto &kvp : group->opt_map()) {
            const std::string& opt_key = kvp.first;
            if ((m_options_list[opt_key] & os) == 0)
                to_sys ? group->back_to_sys_value(opt_key) : group->back_to_initial_value(opt_key);
        }
    }

    m_postpone_update_ui = false;

    // When all values are rolled, then we have to update whole tab in respect to the reverted values
    update();

    update_changed_ui();
}

// Update the combo box label of the selected preset based on its "dirty" state,
// comparing the selected preset config with $self->{config}.
void Tab::update_dirty()
{
    m_presets_choice->update_dirty();
    on_presets_changed();
    update_changed_ui();
}

void Tab::update_tab_ui()
{
    m_presets_choice->update();
}

// Load a provied DynamicConfig into the tab, modifying the active preset.
// This could be used for example by setting a position by interactive manipulation in the 3D view.
void Tab::load_config(const DynamicPrintConfig& config)
{
    bool modified = 0;
    for(auto opt_key : m_config->diff(config)) {
        m_config->set_key_value(opt_key, config.option(opt_key)->clone());
        modified = 1;
    }
    if (modified) {
        update_dirty();
        //# Initialize UI components with the config values.
        reload_config();
        update();
    }
}

// Reload current $self->{config} (aka $self->{presets}->edited_preset->config) into the UI fields.
void Tab::reload_config()
{
    if (m_active_page)
        m_active_page->reload_config();
}

void Tab::update_mode()
{
    m_mode = wxGetApp().get_mode();

    update_visibility();
    update_sla_prusa_specific_visibility();

    update_changed_tree_ui();
}

void Tab::update_mode_markers()
{
    if (m_active_page)
        m_active_page->refresh();
}

void Tab::update_visibility()
{
    Freeze(); // There is needed Freeze/Thaw to avoid a flashing after Show/Layout

    for (auto page : m_pages)
        page->update_visibility(m_mode, page.get() == m_active_page);
    rebuild_page_tree();

    if (m_type != Preset::TYPE_PRINTER)
        update_description_lines();

    Layout();
    Thaw();
}

void Tab::msw_rescale()
{
    m_em_unit = em_unit(m_parent);

    m_presets_choice->msw_rescale();
    m_treectrl->SetMinSize(wxSize(20 * m_em_unit, -1));

    if (m_compatible_printers.mode)
        m_compatible_printers.mode->SetMinSize(wxSize(16 * m_em_unit, -1));
    if (m_compatible_prints.mode)
        m_compatible_prints.mode->SetMinSize(wxSize(16 * m_em_unit, -1));

    // rescale options_groups
    if (m_active_page)
        m_active_page->msw_rescale();

    Layout();
}

void Tab::sys_color_changed()
{
    m_presets_choice->sys_color_changed();

    // update buttons and cached bitmaps
    for (const auto btn : m_scaled_buttons)
        btn->sys_color_changed();
    for (const auto bmp : m_scaled_bitmaps)
        bmp->sys_color_changed();
    if (m_detach_preset_btn)
        m_detach_preset_btn->sys_color_changed();

    m_btn_hide_incompatible_presets->SetBitmap(*get_bmp_bundle(m_show_incompatible_presets ? "flag_red" : "flag_green"));

    // update icons for tree_ctrl
    wxVector <wxBitmapBundle> img_bundles;
    for (ScalableBitmap& bmp : m_scaled_icons_list) {
        bmp.sys_color_changed();
        img_bundles.push_back(bmp.bmp());
    }
    m_treectrl->SetImages(img_bundles);

    // Colors for ui "decoration"
    update_label_colours();
#ifdef _WIN32
    wxWindowUpdateLocker noUpdates(this);
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDarkUI(m_treectrl);
#endif
    update_changed_tree_ui();

    // update options_groups
    if (m_active_page)
        m_active_page->sys_color_changed();

    Layout();
    Refresh();
}

// Debug guard against options that SLAPrint consumes but no page edits (the fast_tilt_time class of bug).
// A key must either be registered by some option group of this tab or be whitelisted in
// Preset::sla_hidden_option_keys(). Release builds skip the scan.
void Tab::check_options_have_editors(const std::vector<std::string>& option_keys) const
{
#ifndef NDEBUG
    std::set<std::string> registered;
    for (const PageShp& page : m_pages)
        for (const ConfigOptionsGroupShp& group : page->m_optgroups)
            for (const auto& [id, key_and_idx] : group->opt_map())
                registered.insert(key_and_idx.first);

    const std::vector<std::string>& hidden = Preset::sla_hidden_option_keys();
    for (const std::string& key : option_keys) {
        if (registered.count(key) || std::find(hidden.begin(), hidden.end(), key) != hidden.end())
            continue;
        BOOST_LOG_TRIVIAL(error) << "SLA option \"" << key << "\" has no editor in the tab and is not in Preset::sla_hidden_option_keys()";
        assert(false && "SLA option without an editor");
    }
#else
    (void)option_keys;
#endif
}

Field* Tab::get_field(const t_config_option_key& opt_key, int opt_index/* = -1*/) const
{
    return m_active_page ? m_active_page->get_field(opt_key, opt_index) : nullptr;
}

Line* Tab::get_line(const t_config_option_key& opt_key)
{
    return m_active_page ? m_active_page->get_line(opt_key) : nullptr;
}

std::pair<OG_CustomCtrl*, bool*> Tab::get_custom_ctrl_with_blinking_ptr(const t_config_option_key& opt_key, int opt_index/* = -1*/)
{
    if (!m_active_page)
        return {nullptr, nullptr};

    std::pair<OG_CustomCtrl*, bool*> ret = {nullptr, nullptr};

    for (auto opt_group : m_active_page->m_optgroups) {
        ret = opt_group->get_custom_ctrl_with_blinking_ptr(opt_key, opt_index);
        if (ret.first && ret.second)
            break;
    }
    return ret;
}

Field* Tab::get_field(const t_config_option_key& opt_key, Page** selected_page, int opt_index/* = -1*/)
{
    Field* field = nullptr;
    for (auto page : m_pages) {
        field = page->get_field(opt_key, opt_index);
        if (field != nullptr) {
            *selected_page = page.get();
            return field;
        }
    }
    return field;
}

void Tab::toggle_option(const std::string& opt_key, bool toggle, int opt_index/* = -1*/)
{
    if (!m_active_page)
        return;
    Field* field = m_active_page->get_field(opt_key, opt_index);
    if (field)
        field->toggle(toggle);
};

// To be called by custom widgets, load a value into a config,
// update the preset selection boxes (the dirty flags)
// If value is saved before calling this function, put saved_value = true,
// and value can be some random value because in this case it will not been used
void Tab::load_key_value(const std::string& opt_key, const boost::any& value, bool saved_value /*= false*/)
{
    if (!saved_value) OptionsGroup::change_opt_value(*m_config, opt_key, value);
    // Mark the print & material enabled if they are compatible with the currently selected preset.
    if (opt_key == "compatible_printers" || opt_key == "compatible_prints") {
        // Don't select another profile if this profile happens to become incompatible.
        m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    }
    m_presets_choice->update_dirty();
    on_presets_changed();
    update();
}

static wxString support_combo_value_for_config(const DynamicPrintConfig &config)
{
    const std::string buildplate_only = get_sla_suptree_prefix(config) + "support_buildplate_only";

    return
        ! config.opt_bool("supports_enable") ?
            _("None") :
               config.opt_bool("support_enforcers_only") ?
                _("For support enforcers only") :
                (config.opt_bool(buildplate_only) ? _("Support on build plate only") :
                                                    _("Everywhere"));
}

static wxString pad_combo_value_for_config(const DynamicPrintConfig &config)
{
    return config.opt_bool("pad_enable") ? (config.opt_bool("pad_around_object") ? _("Around object") : _("Below object")) : _("None");
}

void Tab::on_value_change(const std::string& opt_key, const boost::any& value)
{
    if (wxGetApp().plater() == nullptr) {
        return;
    }

    if (opt_key == "compatible_prints_condition" || opt_key == "compatible_printers_condition") {
        m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
        m_presets_choice->update();
    }
    if (opt_key == "compatible_prints" || opt_key == "compatible_prints_condition")
        this->compatible_widget_reload(m_compatible_prints);
    if (opt_key == "compatible_printers" || opt_key == "compatible_printers_condition")
        this->compatible_widget_reload(m_compatible_printers);

    ConfigOptionsGroup* og_freq_chng_params = wxGetApp().sidebar().og_freq_chng_params();
    if (opt_key == "pad_enable")
    {
        boost::any val = og_freq_chng_params->get_config_value(*m_config, opt_key);
        og_freq_chng_params->set_value(opt_key, val);
    }
    
    if (opt_key == "pad_around_object") {
        for (PageShp &pg : m_pages) {
            Field * fld = pg->get_field(opt_key); /// !!! ysFIXME ????
            if (fld) fld->set_value(value, false);
        }
    }

    if (opt_key == "supports_enable"  || opt_key == "support_tree_type" || opt_key == get_sla_suptree_prefix(*m_config) + "support_buildplate_only" || opt_key == "support_enforcers_only")
        og_freq_chng_params->set_value("support", support_combo_value_for_config(*m_config));

    if (opt_key == "pad_enable" || opt_key == "pad_around_object")
        og_freq_chng_params->set_value("pad", pad_combo_value_for_config(*m_config));

    if (m_postpone_update_ui) {
        // It means that not all values are rolled to the system/last saved values jet.
        // And call of the update() can causes a redundant check of the config values,
        // see https://github.com/prusa3d/PrusaSlicer/issues/7146
        return;
    }

    update();
}

void Tab::activate_option(const std::string &opt_key, const wxString &category, const std::vector<std::string> &another_blinking_opt_keys)
{
    wxString page_title = translate_category(category, m_type);

    // We should to activate a tab with searched option, if it doesn't.
    // And do it before finding of the cur_item to avoid a case when Tab isn't activated jet and all treeItems are invisible
    wxGetApp().mainframe->select_tab(this);

    auto cur_item = m_treectrl->GetFirstVisibleItem();
    if (!cur_item)
        return;

    while (cur_item) {
        auto title = m_treectrl->GetItemText(cur_item);
        if (page_title != title) {
            cur_item = m_treectrl->GetNextVisible(cur_item);
            continue;
        }

        m_treectrl->SelectItem(cur_item);
        break;
    }

    auto set_focus = [](wxWindow* win) {
        win->SetFocus();
#ifdef WIN32
        if (wxTextCtrl* text = dynamic_cast<wxTextCtrl*>(win))
            text->SetSelection(-1, -1);
        else if (wxSpinCtrl* spin = dynamic_cast<wxSpinCtrl*>(win))
            spin->SetSelection(-1, -1);
#endif // WIN32
    };

    Field* field = get_field(opt_key);

    // focused selected field
    if (field)
        set_focus(field->getWindow());

    std::vector<std::pair<OG_CustomCtrl *, bool *>> custom_blinking_ctrls = { get_custom_ctrl_with_blinking_ptr(opt_key) };
    for (const std::string &another_blinking_opt_key : another_blinking_opt_keys) {
        custom_blinking_ctrls.emplace_back(get_custom_ctrl_with_blinking_ptr(another_blinking_opt_key));
    }

    m_highlighter.init(custom_blinking_ctrls);
}

void Tab::cache_config_diff(const std::vector<std::string>& selected_options, const DynamicPrintConfig* config/* = nullptr*/)
{
    m_cache_config.apply_only(config ? *config : m_presets->get_edited_preset().config, selected_options);
}

void Tab::apply_config_from_cache()
{
    bool was_applied = false;

    if (!m_cache_config.empty()) {
        m_presets->get_edited_preset().config.apply(m_cache_config);
        m_cache_config.clear();

        was_applied = true;
    }

    if (was_applied)
        update_dirty();
}


// Call a callback to update the selection of presets on the plater:
// To update the content of the selection boxes,
// to update the "dirty" flags of the selection boxes,
void Tab::on_presets_changed()
{
    if (wxGetApp().plater() == nullptr)
        return;

    // Instead of PostEvent (EVT_TAB_PRESETS_CHANGED) just call update_presets
    wxGetApp().plater()->sidebar().update_presets(m_type);

    // Printer selected at the Printer tab, update "compatible" marks at the print and material selectors.
    for (auto t: m_dependent_tabs)
    {
        Tab* tab = wxGetApp().get_tab(t);
        // If the printer tells us that the sla_print or sla_material preset has been switched or invalidated,
        // refresh the sla_print or sla_material tab page.
        // But if there are options, moved from the previously selected preset, update them to edited preset
        tab->apply_config_from_cache();
        tab->load_current_preset();
    }
    // clear m_dependent_tabs after first update from select_preset()
    // to avoid needless preset loading from update() function
    m_dependent_tabs.clear();

    // Update Project dirty state, update application title bar.
    if (wxGetApp().mainframe)
        wxGetApp().plater()->update_project_dirty_from_presets();
}

void Tab::build_preset_description_line(ConfigOptionsGroup* optgroup)
{
    auto description_line = [this](wxWindow* parent) {
        return description_line_widget(parent, &m_parent_preset_description_line);
    };

    auto detach_preset_btn = [this](wxWindow* parent) {
        m_detach_preset_btn = new ScalableButton(parent, wxID_ANY, "lock_open_sys", _L("Detach from system preset"), 
                                                 wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT);
        ScalableButton* btn = m_detach_preset_btn;
        btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());

        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(btn);

        btn->Bind(wxEVT_BUTTON, [this, parent](wxCommandEvent&)
        {
        	bool system = m_presets->get_edited_preset().is_system;
        	bool dirty  = m_presets->get_edited_preset().is_dirty;
            wxString msg_text = system ? 
            	_(L("A copy of the current system preset will be created, which will be detached from the system preset.")) :
                _(L("The current custom preset will be detached from the parent system preset."));
            if (dirty) {
	            msg_text += "\n\n";
            	msg_text += _(L("Modifications to the current profile will be saved."));
            }
            msg_text += "\n\n";
            msg_text += _(L("This action is not revertible.\nDo you want to proceed?"));

            //wxMessageDialog dialog(parent, msg_text, _(L("Detach preset")), wxICON_WARNING | wxYES_NO | wxCANCEL);
            MessageDialog dialog(parent, msg_text, _(L("Detach preset")), wxICON_WARNING | wxYES_NO | wxCANCEL);
            if (dialog.ShowModal() == wxID_YES)
                save_preset(m_presets->get_edited_preset().is_system ? std::string() : m_presets->get_edited_preset().name, true);
        });

        btn->Hide();

        return sizer;
    };

    Line line = Line{ "", "" };
    line.full_width = 1;

    line.append_widget(description_line);
    line.append_widget(detach_preset_btn);
    optgroup->append_line(line);
}

void Tab::update_preset_description_line()
{
    const Preset* parent = m_presets->get_selected_preset_parent();
    const Preset& preset = m_presets->get_edited_preset();

    wxString description_line;

    if (preset.is_default) {
        description_line = _(L("This is a default preset."));
    } else if (preset.is_system) {
        description_line = _(L("This is a system preset."));
    } else if (parent == nullptr) {
        description_line = _(L("Current preset is inherited from the default preset."));
    } else {
        std::string name = parent->name;
        boost::replace_all(name, "&", "&&");
        description_line = _(L("Current preset is inherited from")) + ":\n\t" + from_u8(name);
    }

    if (preset.is_default || preset.is_system)
        description_line += "\n\t" + _(L("It can't be deleted or modified.")) +
                            "\n\t" + _(L("Any modifications should be saved as a new preset inherited from this one.")) +
                            "\n\t" + _(L("To do that please specify a new name for the preset."));

    if (parent && parent->vendor)
    {
        description_line += "\n\n" + _(L("Additional information:")) + "\n";
        description_line += "\t" + _(L("vendor")) + ": " + (m_type == Slic3r::Preset::TYPE_PRINTER ? "\n\t\t" : "") + parent->vendor->name +
                            ", ver: " + parent->vendor->config_version.to_string();
        if (m_type == Slic3r::Preset::TYPE_PRINTER) {
            const std::string &printer_model = preset.config.opt_string("printer_model");
            if (! printer_model.empty())
                description_line += "\n\n\t" + _(L("printer model")) + ": \n\t\t" + printer_model;
            switch (preset.printer_technology()) {
            case ptSLA:
            {
                //FIXME add prefered_sla_material_profile for SLA
                const std::string &default_sla_material_profile = preset.config.opt_string("default_sla_material_profile");
                if (!default_sla_material_profile.empty())
                    description_line += "\n\n\t" + _(L("default SLA material profile")) + ": \n\t\t" + default_sla_material_profile;

                const std::string &default_sla_print_profile = preset.config.opt_string("default_sla_print_profile");
                if (!default_sla_print_profile.empty())
                    description_line += "\n\n\t" + _(L("default SLA print profile")) + ": \n\t\t" + default_sla_print_profile;
                break;
            }
            default: break;
            }
        }
        else if (!preset.alias.empty())
        {
            description_line += "\n\n\t" + _(L("full profile name"))     + ": \n\t\t" + preset.name;
            description_line += "\n\t"   + _(L("symbolic profile name")) + ": \n\t\t" + preset.alias;
        }
    }

    m_parent_preset_description_line->SetText(description_line, false);

    if (m_detach_preset_btn)
        m_detach_preset_btn->Show(parent && parent->is_system && !preset.is_default);
    Layout();
}

void Tab::update_frequently_changed_parameters()
{
    auto og_freq_chng_params = wxGetApp().sidebar().og_freq_chng_params();
    if (!og_freq_chng_params) return;

    og_freq_chng_params->set_value("support", support_combo_value_for_config(*m_config));
    og_freq_chng_params->set_value("pad", pad_combo_value_for_config(*m_config));

    const boost::any val = og_freq_chng_params->get_config_value(*m_config, "pad_enable");
    og_freq_chng_params->set_value("pad_enable", val);
}

static void validate_custom_parameters(Tab* tab, const t_config_option_key& opt_key, const boost::any& value)
{
    if (! bool(parse_custom_parameters(boost::any_cast<std::string>(value)))) {
        MessageDialog dialog(wxGetApp().mainframe, _L("Unable to parse custom parameters."), _L("Error"), wxICON_WARNING | wxOK);
        dialog.ShowModal();
    }
    tab->update_dirty();
    tab->on_value_change(opt_key, value);
}

wxSizer* Tab::description_line_widget(wxWindow* parent, ogStaticText* *StaticText, wxString text /*= wxEmptyString*/)
{
    *StaticText = new ogStaticText(parent, text);

//	auto font = (new wxSystemSettings)->GetFont(wxSYS_DEFAULT_GUI_FONT);
    (*StaticText)->SetFont(wxGetApp().normal_font());

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(*StaticText, 1, wxEXPAND|wxALL, 0);
    return sizer;
}

bool Tab::saved_preset_is_dirty() const { return m_presets->saved_is_dirty(); }
void Tab::update_saved_preset_from_current_preset() { m_presets->update_saved_preset_from_current_preset(); }
bool Tab::current_preset_is_dirty() const { return m_presets->current_is_dirty(); }

void TabPrinter::build()
{
    m_presets = &m_preset_bundle->printers;
    load_initial_data();
    build_sla();
}

void TabPrinter::build_sla()
{
    if (!m_pages.empty())
        m_pages.resize(0);
    auto page = add_options_page(L("General"), "printer");
    auto optgroup = page->new_optgroup(L("Size and coordinates"));

    // SLA build dimensions come from the display; an independent polygon
    // editor would allow the scene and exported raster to disagree.
    optgroup->append_single_option_line("bed_custom_texture");
    optgroup->append_single_option_line("bed_custom_model");
    optgroup->append_single_option_line("max_print_height");

    optgroup = page->new_optgroup(L("Display"));
    optgroup->append_single_option_line("display_width");
    optgroup->append_single_option_line("display_height");

    auto option = optgroup->get_option("display_pixels_x");
    Line line = { option.opt.full_label, "" };
    line.append_option(option);
    line.append_option(optgroup->get_option("display_pixels_y"));
    optgroup->append_line(line);
    optgroup->append_single_option_line("display_orientation");

    // FIXME: This should be on one line in the UI
    optgroup->append_single_option_line("display_mirror_x");
    optgroup->append_single_option_line("display_mirror_y");
    optgroup->append_single_option_line("sla_archive_header_mirror");

    optgroup = page->new_optgroup(L("Corrections"));
    line = Line{ m_config->def()->get("relative_correction")->full_label, "" };
    for (auto& axis : { "X", "Y", "Z" }) {
        auto opt = optgroup->get_option(std::string("relative_correction_") + char(std::tolower(axis[0])));
        opt.opt.label = axis;
        line.append_option(opt);
    }
    optgroup->append_line(line);
    optgroup->append_single_option_line("absolute_correction");
    optgroup->append_single_option_line("elefant_foot_compensation");
    optgroup->append_single_option_line("elefant_foot_min_width");
    optgroup->append_single_option_line("gamma_correction");
    
    optgroup = page->new_optgroup(L("Exposure"));
    optgroup->append_single_option_line("min_exposure_time");
    optgroup->append_single_option_line("max_exposure_time");
    optgroup->append_single_option_line("min_initial_exposure_time");
    optgroup->append_single_option_line("max_initial_exposure_time");

    // Consumed by SLAPrint for the print-time estimate; previously only editable in the INI.
    optgroup = page->new_optgroup(L("Tilt timing"));
    optgroup->append_single_option_line("fast_tilt_time");
    optgroup->append_single_option_line("slow_tilt_time");
    optgroup->append_single_option_line("high_viscosity_tilt_time");

    optgroup = page->new_optgroup(L("Output"));
    auto model_option = optgroup->get_option("printer_model");
    model_option.opt.label = L("Printer model (firmware)");
    model_option.opt.tooltip = L("Model identifier stored in the printer file. CXDLPv4 requires a nonempty identifier (up to 255 bytes). Creality CXDLP v2/v3 requires the actual CL or CT model, for example CL-60. Saved with this printer preset.");
    model_option.opt.mode = comSimple;
    optgroup->append_single_option_line(model_option);
    option = optgroup->get_option("sla_archive_format");
    // Store stable registry IDs, never translated descriptions or list indexes.
    // Keep the field editable so an imported, unknown ID remains visible rather
    // than silently becoming the first supported format when opening the tab.
    std::vector<std::string> archive_ids;
    std::vector<std::string> archive_labels;
    for (const auto &entry : registered_sla_archives()) {
        archive_ids.emplace_back(entry.id);
        std::string label = I18N::translate_utf8(entry.desc) + " (." + entry.ext + ")";
        if (!archive_export_unavailable_reason(entry.id).empty()) label += " — " + I18N::translate_utf8(L("Export unavailable"));
        archive_labels.push_back(std::move(label));
    }
    option.opt.set_enum_values(std::move(archive_ids), std::move(archive_labels));
    option.opt.gui_type = ConfigOptionDef::GUIType::select_open;
    optgroup->append_single_option_line(option);
    optgroup->append_single_option_line("sla_anycubic_version");
    optgroup->append_single_option_line("sla_cbddlp_aa");
    optgroup->append_single_option_line("sla_phz_encryption_key");
    optgroup->append_single_option_line("sla_output_precision");

    // Make controller configuration reachable from the format selector without
    // requiring navigation of the platform-specific settings tree.
    line = Line{"", ""};
    line.full_width = true;
    line.widget = [this](wxWindow *parent) {
        auto *sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *button = new wxButton(parent, wxID_ANY, _("Configure NanoDLP controller..."));
        button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            // Selecting a page destroys its old controls. Finish dispatching
            // the button event before replacing the page that owns it.
            CallAfter([this] { activate_option("nanodlp_device_profile", "NanoDLP controller"); });
        });
        sizer->Add(button, 0);
        return sizer;
    };
    optgroup->append_line(line);

    page = add_options_page(L("NanoDLP controller"), "wrench");
    optgroup = page->new_optgroup(L("Controller profile"));
    optgroup->append_single_option_line("nanodlp_device_profile");
    line = Line{"", ""};
    line.full_width = true;
    line.widget = [this](wxWindow *parent) {
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        auto *description = new wxStaticText(parent, wxID_ANY,
            _("Load profile.json from your NanoDLP controller. Save the printer preset to keep it. "
              "Controller templates determine movement speeds and hardware commands; generic SLA speeds do not rewrite them."));
        description->Wrap(550);
        sizer->Add(description, 0, wxEXPAND | wxBOTTOM, 8);
        auto *button = new wxButton(parent, wxID_ANY, _("Load controller profile..."));
        button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
          // Let native/accessibility button dispatch return before entering a
          // modal event loop. The tab remains the parent across page rebuilds.
          CallAfter([this] {
            wxFileDialog dialog(this, _("Load NanoDLP controller profile"), wxEmptyString,
                                "profile.json", _("Controller profile (*.json)|*.json"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() != wxID_OK) return;
            try {
                // Own UTF-8 storage: GetPath() returns a temporary wxString,
                // whose conversion buffer must not outlive that expression.
                const std::string path = into_u8(dialog.GetPath());
                boost::nowide::ifstream input(path, std::ios::binary);
                if (!input) throw std::runtime_error("Cannot open NanoDLP controller profile");
                input.seekg(0, std::ios::end);
                const auto length = input.tellg();
                if (length < 0 || length > 4 * 1024 * 1024)
                    throw std::runtime_error("NanoDLP controller profile exceeds 4 MiB");
                input.seekg(0);
                std::string text(size_t(length), '\0');
                input.read(text.data(), text.size());
                if (!input && !text.empty()) throw std::runtime_error("Cannot read NanoDLP controller profile");
                const auto profile = NanoDLPArchive::parse_device_profile(text);
                DynamicPrintConfig changed = *m_config;
                changed.set("nanodlp_device_profile", profile.dump(2));
                load_config(changed);
            } catch (const std::exception &error) {
                wxMessageBox(from_u8(error.what()), _("Invalid NanoDLP controller profile"), wxOK | wxICON_ERROR, this);
            }
          });
        });
        sizer->Add(button, 0, wxALIGN_LEFT);
        return sizer;
    };
    optgroup->append_line(line);

    const int notes_field_height = 25; // 250

    page = add_options_page(L("Notes"), "note");
    optgroup = page->new_optgroup(L("Notes"), 0);
    option = optgroup->get_option("printer_notes");
    option.opt.full_width = true;
    option.opt.height = notes_field_height;//250;
    optgroup->append_single_option_line(option);

    page = add_options_page(L("Dependencies"), "wrench");
    optgroup = page->new_optgroup(L("Profile dependencies"));

    build_preset_description_line(optgroup.get());
    check_options_have_editors(Preset::sla_printer_options());
}

void TabPrinter::toggle_options()
{
    if (!m_active_page) return;
    const auto &versions = anycubic_available_versions(m_config->opt_string("sla_archive_format"));
    if (auto *field = dynamic_cast<Choice*>(get_field("sla_anycubic_version"))) {
        std::vector<std::string> values{"auto"}, labels{L("Automatic (format default)")};
        for (uint16_t version : versions) {
            values.push_back(std::to_string(version)); labels.push_back(std::to_string(version));
        }
        field->set_enum_values(std::move(values), std::move(labels), m_config->opt_string("sla_anycubic_version"));
    }
    toggle_option("sla_anycubic_version", !versions.empty());
    toggle_option("sla_cbddlp_aa", m_config->opt_string("sla_archive_format") == "cbddlp" || m_config->opt_string("sla_archive_format") == "photon");
    toggle_option("sla_phz_encryption_key", m_config->opt_string("sla_archive_format") == "phz");
}

void TabPrinter::update()
{
    m_update_cnt++;
    update_sla();
    m_update_cnt--;

    update_description_lines();
    Layout();

    if (m_update_cnt == 0)
        wxGetApp().mainframe->on_config_changed(m_config);
}

void TabPrinter::update_sla()
{
    // Propagate the derived bed before update() notifies the plater, and include
    // it in dirty tracking so saving a printer persists the same geometry.
    if (sla::synchronize_bed_shape(*m_config))
        update_dirty();
    // Re-evaluate format-specific fields immediately after editing the format;
    // page activation alone leaves PHZ's seed disabled until reopening the tab.
    toggle_options();
}

void Tab::update_ui_items_related_on_parent_preset(const Preset* selected_preset_parent)
{
    m_is_default_preset = selected_preset_parent != nullptr && selected_preset_parent->is_default;

    m_bmp_non_system = selected_preset_parent ? &m_bmp_value_unlock : &m_bmp_white_bullet;
    m_ttg_non_system = selected_preset_parent ? &m_ttg_value_unlock : &m_ttg_white_bullet_ns;
    m_tt_non_system  = selected_preset_parent ? &m_tt_value_unlock  : &m_ttg_white_bullet_ns;
}

// Initialize the UI from the current preset
void Tab::load_current_preset()
{
    update_btns_enabling();

    update();
    if (m_type == Slic3r::Preset::TYPE_PRINTER) {
    }
    // Reload preset pages with the new configuration values.
    reload_config();

    update_ui_items_related_on_parent_preset(m_presets->get_selected_preset_parent());

//	m_undo_to_sys_btn->Enable(!preset.is_default);

    {
        // checking out if this Tab exists till this moment
        if (!wxGetApp().checked_tab(this))
            return;
        update_tab_ui();

        on_presets_changed();
        if (m_type == Preset::TYPE_SLA_PRINT)
            update_frequently_changed_parameters();

        m_opt_status_value = (m_presets->get_selected_preset_parent() ? osSystemValue : 0) | osInitValue;
        init_options_list();
        update_visibility();
        update_sla_prusa_specific_visibility();
        update_changed_ui();
    }
#if 0
    );
#endif
}

//Regerenerate content of the page tree.
void Tab::rebuild_page_tree()
{
    // get label of the currently selected item
    const auto sel_item = m_treectrl->GetSelection();
    const auto selected = sel_item ? m_treectrl->GetItemText(sel_item) : "";
    const auto rootItem = m_treectrl->GetRootItem();

    wxTreeItemId item;

    // Delete/Append events invoke wxEVT_TREE_SEL_CHANGED event.
    // To avoid redundant clear/activate functions call
    // suppress activate page before page_tree rebuilding
    m_disable_tree_sel_changed_event = true;
    m_treectrl->DeleteChildren(rootItem);

    for (auto p : m_pages)
    {
        if (!p->get_show())
            continue;
        auto itemId = m_treectrl->AppendItem(rootItem, translate_category(p->title(), m_type), p->iconID());
        m_treectrl->SetItemTextColour(itemId, p->get_item_colour());
        m_treectrl->SetItemFont(itemId, wxGetApp().normal_font());
        if (translate_category(p->title(), m_type) == selected)
            item = itemId;
    }
    if (!item) {
        // this is triggered on first load, so we don't disable the sel change event
        item = m_treectrl->GetFirstVisibleItem();
    }

    // allow activate page before selection of a page_tree item
    m_disable_tree_sel_changed_event = false;
    if (item)
        m_treectrl->SelectItem(item);
}

void Tab::update_btns_enabling()
{
    // any user preset can be deleted
    const Preset& preset = m_presets->get_edited_preset();

    m_btn_delete_preset->Show(!preset.is_default && !preset.is_system);

    m_btn_rename_preset->Show(!preset.is_default && !preset.is_system && !preset.is_external);
    // Export needs a saved, non-default preset; import is always possible (it creates a new user preset).
    m_btn_export_preset->Show(!preset.is_default && !preset.is_external);
    m_btn_import_preset->Show(true);

    m_h_buttons_sizer->Layout();
}

void Tab::update_preset_choice()
{
    m_presets_choice->update();
    update_btns_enabling();
}

// Called by the UI combo box when the user switches profiles, and also to delete the current profile.
// Select a preset by a name.If !defined(name), then the default preset is selected.
// If the current profile is modified, user is asked to save the changes.
bool Tab::select_preset(std::string preset_name, bool delete_current /*=false*/)
{
    if (preset_name.empty()) {
        if (delete_current) {
            // Find an alternate preset to be selected after the current preset is deleted.
            const std::deque<Preset> &presets 		= m_presets->get_presets();
            size_t    				  idx_current   = m_presets->get_idx_selected();
            // Find the next visible preset.
            size_t 				      idx_new       = idx_current + 1;
            if (idx_new < presets.size())
                for (; idx_new < presets.size() && ! presets[idx_new].is_visible; ++ idx_new) ;
            if (idx_new == presets.size())
                for (idx_new = idx_current - 1; idx_new > 0 && ! presets[idx_new].is_visible; -- idx_new);
            preset_name = presets[idx_new].name;
        } else {
            // If no name is provided, select the "-- default --" preset.
            preset_name = m_presets->default_preset().name;
        }
    }
    assert(! delete_current || (m_presets->get_edited_preset().name != preset_name && m_presets->get_edited_preset().is_user()));
    bool current_dirty = ! delete_current && m_presets->current_is_dirty();
    bool print_tab     = m_presets->type() == Preset::TYPE_SLA_PRINT;
    bool printer_tab   = m_presets->type() == Preset::TYPE_PRINTER;
    bool canceled      = false;
    m_dependent_tabs.clear();
    if (current_dirty && ! may_discard_current_dirty_preset(nullptr, preset_name)) {
        canceled = true;
    } else if (print_tab) {
        // Before switching the print profile to a new one, verify, whether the currently active SLA material
        // is compatible with the new print.
        // If it is not compatible and the current SLA material is dirty, let user decide
        // whether to discard the changes or keep the current print selection.
        PresetWithVendorProfile printer_profile = m_preset_bundle->printers.get_edited_preset_with_vendor_profile();
        PresetCollection  &dependent = m_preset_bundle->sla_materials;
        bool 			   old_preset_dirty = dependent.current_is_dirty();
        const Preset *new_print_preset = m_presets->find_preset(preset_name, true);
        if (new_print_preset == nullptr) {
            canceled = true;
        }
        bool 			   new_preset_compatible = !canceled && is_compatible_with_print(
            dependent.get_edited_preset_with_vendor_profile(),
	        m_presets->get_preset_with_vendor_profile(*new_print_preset), printer_profile);
        if (! canceled)
            canceled = old_preset_dirty && ! new_preset_compatible && ! may_discard_current_dirty_preset(&dependent, preset_name);
        if (! canceled) {
            // The preset will be switched to a different, compatible preset, or the '-- default --'.
            m_dependent_tabs.emplace_back(Preset::Type::TYPE_SLA_MATERIAL);
            if (old_preset_dirty && ! new_preset_compatible)
                dependent.discard_current_changes();
        }
    } else if (printer_tab) {
        // Before switching the printer to a new one, verify, whether the currently active print and SLA material
        // are compatible with the new printer.
        // If they are not compatible and the current print or material are dirty, let user decide
        // whether to discard the changes or keep the current printer selection.
        const Preset *new_printer_preset_ptr = m_presets->find_preset(preset_name, true);
        if (new_printer_preset_ptr == nullptr)
            canceled = true;

        const Preset &new_printer_preset = canceled ? m_presets->get_edited_preset() : *new_printer_preset_ptr;
		const PresetWithVendorProfile new_printer_preset_with_vendor_profile = m_presets->get_preset_with_vendor_profile(new_printer_preset);
        {
            struct PresetUpdate {
                Preset::Type         tab_type;
                PresetCollection 	*presets;
                bool    	         old_preset_dirty;
                bool         	     new_preset_compatible;
            };
            std::vector<PresetUpdate> updates = {
                { Preset::Type::TYPE_SLA_PRINT,     &m_preset_bundle->sla_prints },
                { Preset::Type::TYPE_SLA_MATERIAL,  &m_preset_bundle->sla_materials }
            };
            for (PresetUpdate &pu : updates) {
                pu.old_preset_dirty = pu.presets->current_is_dirty();
                pu.new_preset_compatible = is_compatible_with_printer(pu.presets->get_edited_preset_with_vendor_profile(), new_printer_preset_with_vendor_profile);
                if (!canceled)
                    canceled = pu.old_preset_dirty && !pu.new_preset_compatible && !may_discard_current_dirty_preset(pu.presets, preset_name);
            }
            if (!canceled) {
                for (PresetUpdate &pu : updates) {
                    // The preset will be switched to a different, compatible preset, or the '-- default --'.
                    m_dependent_tabs.emplace_back(pu.tab_type);
                    if (pu.old_preset_dirty && !pu.new_preset_compatible)
                        pu.presets->discard_current_changes();
                }
            }
        }
    }

    if (! canceled && delete_current) {
        // Delete the file and select some other reasonable preset.
        // It does not matter which preset will be made active as the preset will be re-selected from the preset_name variable.
        // The 'external' presets will only be removed from the preset list, their files will not be deleted.
        try {
            // cache previously selected names
            delete_current_preset();
        } catch (const std::exception &error) {
            wxMessageBox(from_u8(error.what()), _L("Delete preset"), wxOK | wxICON_ERROR, this);
            canceled = true;
        }
    }

    if (canceled) {
        // ! update preset combobox, to revert previously selection
        update_tab_ui();

        // Trigger the on_presets_changed event so that we also restore the previous value in the plater selector,
        // if this action was initiated from the plater.
        on_presets_changed();
    } else {
        if (current_dirty)
            m_presets->discard_current_changes();

        const bool is_selected = select_preset_by_name(preset_name, false) || delete_current;
        assert(m_presets->get_edited_preset().name == preset_name || ! is_selected);
        // Mark the print & material enabled if they are compatible with the currently selected preset.
        // The following method should not discard changes of current print or material presets on change of a printer profile,
        // if they are compatible with the current printer.
        if (current_dirty || delete_current || print_tab || printer_tab) {
            m_preset_bundle->update_compatible(
	            (delete_current || !print_tab) ? PresetSelectCompatibleType::Always : PresetSelectCompatibleType::Never,
	            PresetSelectCompatibleType::Always);
        }

        if (! is_selected && printer_tab)
        {
            /* There is a case, when after Config Wizard applying we try to select previously selected preset,
             * but in a current configuration this one doesn't exist now.
             * So, it is necessary to update list of dependent tabs.
             */
            if (m_dependent_tabs.empty() || m_dependent_tabs.front() != Preset::Type::TYPE_SLA_PRINT)
                m_dependent_tabs = { Preset::Type::TYPE_SLA_PRINT, Preset::Type::TYPE_SLA_MATERIAL };
        }

        // check if there is something in the cache to move to the new selected preset
        apply_config_from_cache();

        load_current_preset();
    }

    return !canceled;
}

// If the current preset is dirty, the user is asked whether the changes may be discarded.
// if the current preset was not dirty, or the user agreed to discard the changes, 1 is returned.
bool Tab::may_discard_current_dirty_preset(PresetCollection* presets /*= nullptr*/, const std::string& new_printer_name /*= ""*/)
{
    if (presets == nullptr) presets = m_presets;

    UnsavedChangesDialog dlg(m_type, presets, new_printer_name);
    if (wxGetApp().app_config->get("default_action_on_select_preset") == "none" && dlg.ShowModal() == wxID_CANCEL)
        return false;

    if (dlg.save_preset())  // save selected changes
    {
        const std::vector<std::string>& unselected_options = dlg.get_unselected_options(presets->type());
        const std::string& name = dlg.get_preset_name();

        if (m_type == presets->type()) // save changes for the current preset from this tab
        {
            // revert unselected options to the old values
            presets->get_edited_preset().config.apply_only(presets->get_selected_preset().config, unselected_options);
            save_preset(name);
        }
        else
        {
            m_preset_bundle->save_changes_for_preset(name, presets->type(), unselected_options);
        }
    }
    else if (dlg.transfer_changes()) // move selected changes
    {
        std::vector<std::string> selected_options = dlg.get_selected_options();
        if (m_type == presets->type()) // move changes for the current preset from this tab
        {
            // copy selected options to the cache from edited preset
            cache_config_diff(selected_options);
        }
        else
            wxGetApp().get_tab(presets->type())->cache_config_diff(selected_options);
    }

    return true;
}

void Tab::clear_pages()
{
    // invalidated highlighter, if any exists
    m_highlighter.invalidate();
    m_page_sizer->Clear(true);
    // clear pages from the controlls
    for (auto p : m_pages)
        p->clear();

    // nulling pointers
    m_parent_preset_description_line = nullptr;
    m_detach_preset_btn = nullptr;

    m_compatible_printers.mode  = nullptr;
    m_compatible_printers.btn       = nullptr;
    m_compatible_printers.status    = nullptr;

    m_compatible_prints.mode    = nullptr;
    m_compatible_prints.btn         = nullptr;
    m_compatible_prints.status      = nullptr;
}

void Tab::update_description_lines()
{
    if (m_active_page && m_active_page->title() == "Dependencies" && m_parent_preset_description_line)
        update_preset_description_line();
}

void Tab::activate_selected_page(std::function<void()> throw_if_canceled)
{
    if (!m_active_page)
        return;

    m_active_page->activate(m_mode, throw_if_canceled);

    if (m_active_page->title() == "Dependencies") {
        if (m_compatible_printers.mode)
            this->compatible_widget_reload(m_compatible_printers);
        if (m_compatible_prints.mode)
            this->compatible_widget_reload(m_compatible_prints);
    }

    update_sla_prusa_specific_visibility();
    update_changed_ui();
    update_description_lines();
    toggle_options();
}

#ifdef WIN32
// Override the wxCheckForInterrupt to process inperruptions just from key or mouse
// and to avoid an unwanted early call of CallAfter()
static bool CheckForInterrupt(wxWindow* wnd)
{
    wxCHECK(wnd, false);

    MSG msg;
    while (::PeekMessage(&msg, ((HWND)((wnd)->GetHWND())), WM_KEYFIRST, WM_KEYLAST, PM_REMOVE))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
    while (::PeekMessage(&msg, ((HWND)((wnd)->GetHWND())), WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
    return true;
}
#endif //WIN32

bool Tab::tree_sel_change_delayed()
{
    // There is a bug related to Ubuntu overlay scrollbars, see https://github.com/prusa3d/PrusaSlicer/issues/898 and https://github.com/prusa3d/PrusaSlicer/issues/952.
    // The issue apparently manifests when Show()ing a window with overlay scrollbars while the UI is frozen. For this reason,
    // we will Thaw the UI prematurely on Linux. This means destroing the no_updates object prematurely.
#ifdef __linux__
    std::unique_ptr<wxWindowUpdateLocker> no_updates(new wxWindowUpdateLocker(this));
#else
    /* On Windows we use DoubleBuffering during rendering,
     * so on Window is no needed to call a Freeze/Thaw functions.
     * But under OSX (builds compiled with MacOSX10.14.sdk) wxStaticBitmap rendering is broken without Freeze/Thaw call.
     */
//#ifdef __WXOSX__  // Use Freeze/Thaw to avoid flickering during clear/activate new page
    wxWindowUpdateLocker noUpdates(this);
//#endif
#endif

    Page* page = nullptr;
    const auto sel_item = m_treectrl->GetSelection();
    const auto selection = sel_item ? m_treectrl->GetItemText(sel_item) : "";
    for (auto p : m_pages)
        if (translate_category(p->title(), m_type) == selection)
        {
            page = p.get();
            m_is_nonsys_values = page->m_is_nonsys_values;
            m_is_modified_values = page->m_is_modified_values;
            break;
        }
    if (page == nullptr || m_active_page == page)
        return false;

    // clear pages from the controls
    m_active_page = page;
    
    auto throw_if_canceled = std::function<void()>([this](){
#ifdef WIN32
            CheckForInterrupt(m_treectrl);
            if (m_page_switch_planned)
                throw UIBuildCanceled();
#else // WIN32
            (void)this; // silence warning
#endif
        });

    try {
        clear_pages();
        throw_if_canceled();

        if (wxGetApp().mainframe!=nullptr && wxGetApp().mainframe->is_active_and_shown_tab(this))
            activate_selected_page(throw_if_canceled);

        #ifdef __linux__
            no_updates.reset(nullptr);
        #endif

        update_undo_buttons();
        throw_if_canceled();

        m_hsizer->Layout();
        throw_if_canceled();
        Refresh();
    } catch (const UIBuildCanceled&) {
	    if (m_active_page)
		    m_active_page->clear();
        return true;
    }

    return false;
}

void Tab::OnKeyDown(wxKeyEvent& event)
{
    if (event.GetKeyCode() == WXK_TAB)
        m_treectrl->Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    else
        event.Skip();
}

void Tab::compare_preset()
{
    wxGetApp().mainframe->diff_dialog.show(m_type);
}

void Tab::transfer_options(const std::string &name_from, const std::string &name_to, std::vector<std::string> options)
{
    if (options.empty())
        return;

    Preset* preset_from = m_presets->find_preset(name_from);
    Preset* preset_to = m_presets->find_preset(name_to);

    cache_config_diff(options, &preset_from->config);

    if (name_to != m_presets->get_edited_preset().name )
        select_preset(preset_to->name);

    apply_config_from_cache();
    load_current_preset();
}

// Save the current preset into file.
// This removes the "dirty" flag of the preset, possibly creates a new preset under a new name,
// and activates the new preset.
// Wizard calls save_preset with a name "My Settings", otherwise no name is provided and this method
// opens a Slic3r::GUI::SavePresetDialog dialog.
void Tab::save_preset(std::string name /*= ""*/, bool detach)
{
    // Saving through a toolbar or accessibility action need not move focus
    // out of an editable field. Commit its pending value before the save
    // dialog snapshots the preset, including translated archive labels.
    if (m_treectrl)
        m_treectrl->SetFocus();

    Preset& edited_preset = m_presets->get_edited_preset();

    if (name.empty()) {
        SavePresetDialog dlg(m_parent, std::vector<Preset::Type>{ m_type }, detach ? _u8L("Detached") : "");
        if (dlg.ShowModal() != wxID_OK)
            return;
        name = dlg.get_name();
    }

    if (detach && m_type == Preset::TYPE_PRINTER)
        m_config->opt_string("printer_model", true) = "";

    // Save the preset into Slic3r::data_dir / presets / section_name / preset_name.ini
    const auto previous_name = edited_preset.name;
    const auto previous_export = m_type == Preset::TYPE_PRINTER && edited_preset.printer_technology()==ptSLA ?
        sla::preferred_export_format(m_presets->get_selected_preset().config.opt_string("sla_archive_format"),
            edited_preset.config.opt_string("sla_archive_format"),
            wxGetApp().app_config->sla_export_format(m_presets->get_selected_preset_source_name("printer_settings_id")),
            edited_preset.config.opt_string("sla_anycubic_version")) : std::string{};
    try {
        save_current_preset(name, detach);
    } catch (const std::exception &error) {
        wxMessageBox(from_u8(error.what()), _L("Save preset"), wxOK | wxICON_ERROR, this);
        return;
    }
    if (m_type == Preset::TYPE_PRINTER && !previous_export.empty())
        wxGetApp().app_config->set_sla_export_format(name, previous_export);

    if (detach && m_type == Preset::TYPE_PRINTER)
        wxGetApp().mainframe->on_config_changed(m_config);


    // Mark the print & material enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are no more compatible.
    m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    // Add the new item into the UI component, remove dirty flags and activate the saved item.
    update_tab_ui();
    // Update the selection boxes at the plater.
    on_presets_changed();
    // If current profile is saved, "delete/rename preset" buttons have to be shown
    m_btn_delete_preset->Show();
    m_btn_rename_preset->Show();
    m_btn_delete_preset->GetParent()->Layout();

    // Parent preset is "default" after detaching, so we should to update UI values, related on parent preset  
    if (detach)
        update_ui_items_related_on_parent_preset(m_presets->get_selected_preset_parent());

    update_changed_ui();

    {
        // Profile compatiblity is updated first when the profile is saved.
        // Update profile selection combo boxes at the depending tabs to reflect modifications in profile compatibility.
        std::vector<Preset::Type> dependent;
        switch (m_type) {
        case Preset::TYPE_SLA_PRINT:
            dependent = { Preset::TYPE_SLA_MATERIAL };
            break;
        case Preset::TYPE_PRINTER:
            dependent = { Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };
            break;
        default:
            break;
        }
        for (Preset::Type preset_type : dependent)
            wxGetApp().get_tab(preset_type)->update_tab_ui();
    }

    // update preset comboboxes in DiffPresetDlg
    wxGetApp().mainframe->diff_dialog.update_presets(m_type);

    if (detach)
        update_description_lines();
}

void Tab::rename_preset()
{
    wxString msg;

    // get new name

    SavePresetDialog dlg(m_parent, m_type, msg);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::string new_name = dlg.get_name();
    if (new_name.empty() || new_name == m_presets->get_selected_preset().name)
        return;

    const std::string old_name = m_presets->get_selected_preset_name();

    bool was_renamed = false;
    try {
        was_renamed = m_presets->rename_current_preset(new_name);
    }
    catch (const std::exception& ex) {
        wxMessageBox(from_u8(ex.what()), _L("Rename preset"), wxOK | wxICON_ERROR, this);
        // Keep the actual file/name pair rather than claiming a failed rename was undone.
        was_renamed = m_presets->get_selected_preset_name() == new_name;
    }

    if (was_renamed && m_type == Preset::TYPE_PRINTER) {
        const auto preference = wxGetApp().app_config->sla_export_format(old_name);
        if (!preference.empty()) {
            wxGetApp().app_config->set_sla_export_format(new_name, preference);
            wxGetApp().app_config->set_sla_export_format(old_name, "");
        }
    }
    // The collection keeps the selected index and unsaved draft together.
    // Reloading the selection here would silently discard edits, even on error.
    m_presets_choice->update();
    update_changed_ui();
    on_presets_changed();
}

// Called for a currently selected preset.
void Tab::delete_preset()
{
    auto current_preset = m_presets->get_selected_preset();
    // Don't let the user delete the ' - default - ' configuration.
    wxString action = current_preset.is_external ? _L("remove") : _L("delete");

    wxString msg;
    {
        // TRN "remove/delete"
        msg += from_u8((boost::format(_u8L("Are you sure you want to %1% the selected preset?")) % action).str());

        // Other user presets that point to this one. "No" cancels; "Yes" deletes the preset and updates them
        // in one all-or-nothing file transaction.
        if (!current_preset.is_external && !current_preset.is_system && !current_preset.is_default) {
            const auto dependents = m_preset_bundle->find_preset_dependents(m_type, current_preset.name);
            if (!dependents.empty()) {
                msg += "\n\n" + _L("These presets refer to it and will be updated:");
                for (const PresetDependent &dependent : dependents) {
                    wxString what;
                    switch (dependent.kind) {
                    case PresetDependent::Kind::Inherits:           what = _L("inherits from it; its current values are kept"); break;
                    case PresetDependent::Kind::CompatiblePrinters: what = _L("compatible printers list; an emptied list becomes \"None\""); break;
                    case PresetDependent::Kind::CompatiblePrints:   what = _L("compatible print profiles list; an emptied list becomes \"None\""); break;
                    case PresetDependent::Kind::DefaultSlaPrint:    what = _L("default process profile is cleared"); break;
                    case PresetDependent::Kind::DefaultSlaMaterial: what = _L("default material profile is cleared"); break;
                    }
                    msg += "\n    \"" + from_u8(dependent.name) + "\" (" + (dependent.type == Preset::TYPE_PRINTER ? _L("printer") :
                                dependent.type == Preset::TYPE_SLA_MATERIAL ? _L("material") : _L("process")) + "): " + what;
                }
            }
        }
    }

    action = current_preset.is_external ? _L("Remove") : _L("Delete");
    // TRN Settings Tabs: Button in toolbar: "Remove/Delete"
    wxString title = format_wxstr(_L("%1% Preset"), action);
    if (current_preset.is_default ||
        //wxID_YES != wxMessageDialog(parent(), msg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal())
        wxID_YES != MessageDialog(parent(), msg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal())
        return;

    // Select will handle of the preset dependencies, of saving & closing the depending profiles, and
    // finally of deleting the preset.
    if (this->select_preset("", true) && m_type == Preset::TYPE_PRINTER && !current_preset.is_external)
        wxGetApp().app_config->set_sla_export_format(current_preset.name, "");
}

void Tab::toggle_show_hide_incompatible()
{
    m_show_incompatible_presets = !m_show_incompatible_presets;
    update_compatibility_ui();
}

void Tab::update_compatibility_ui()
{
    m_btn_hide_incompatible_presets->SetBitmap(*get_bmp_bundle(m_show_incompatible_presets ? "flag_red" : "flag_green"));
    m_btn_hide_incompatible_presets->SetToolTip(m_show_incompatible_presets ?
        "Both compatible an incompatible presets are shown. Click to hide presets not compatible with the current printer." :
        "Only compatible presets are shown. Click to show both the presets compatible and not compatible with the current printer.");

    m_presets_choice->set_show_incompatible_presets(m_show_incompatible_presets);
    m_presets_choice->update();
}

void Tab::update_ui_from_settings()
{
    // Show the 'show / hide presets' button only for the print and material tabs
    if (m_type == Slic3r::Preset::TYPE_PRINTER)
        return;

    // and only if enabled in application preferences.
    bool show = wxGetApp().app_config->get_bool("show_incompatible_presets");
    if (m_show_btn_incompatible_presets == show)
        return;

    m_show_btn_incompatible_presets = show;
    m_btn_hide_incompatible_presets->Show(m_show_btn_incompatible_presets);
    Layout();
    
    if (show)
        update_compatibility_ui();
    else {
        m_presets_choice->set_show_incompatible_presets(false);
        m_presets_choice->update();
    }
}

void Tab::create_line_with_widget(ConfigOptionsGroup* optgroup, const std::string& opt_key, const std::string& path, widget_t widget)
{
    Line line = optgroup->create_single_option_line(opt_key);
    line.widget = widget;
    line.label_path = path;

    // set default undo ui
    line.set_undo_bitmap(&m_bmp_white_bullet);
    line.set_undo_to_sys_bitmap(&m_bmp_white_bullet);
    line.set_undo_tooltip(&m_tt_white_bullet);
    line.set_undo_to_sys_tooltip(&m_tt_white_bullet);
    line.set_label_colour(&m_default_text_clr);

    optgroup->append_line(line);
}

// Return a callback to create a Tab widget to mark the preferences as compatible / incompatible to the current printer.
namespace {
enum class CompatibilityMode { All, Selected, Condition, None };
}

wxSizer* Tab::compatible_widget_create(wxWindow* parent, PresetDependencies &deps)
{
    deps.mode = create_readonly_combo(parent, wxString(), wxString(),
        wxArrayString{_L("All"), _L("Selected profiles"), _L("Condition"), _L("None")}, -1);
    deps.mode->SetName(deps.type == Preset::TYPE_PRINTER ? "Printer compatibility mode" : "Print compatibility mode");
    deps.mode->SetToolTip(deps.mode->GetName());
    deps.mode->SetFont(wxGetApp().normal_font());
    deps.mode->SetMinSize(wxSize(16 * m_em_unit, -1));
    wxGetApp().UpdateDarkUI(deps.mode);
    deps.btn = new ScalableButton(parent, wxID_ANY, "", _L("Select"),
        wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT);
    deps.btn->SetName(deps.type == Preset::TYPE_PRINTER ? "Select compatible printers" : "Select compatible print profiles");
    deps.btn->SetToolTip(deps.btn->GetName());
    deps.status = new wxStaticText(parent, wxID_ANY, "");
    deps.status->SetFont(wxGetApp().normal_font());
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(deps.mode, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, m_em_unit);
    row->Add(deps.btn, 0, wxALIGN_CENTER_VERTICAL);
    // OG_CustomCtrl positions the immediate windows of a widget sizer itself.
    // Keep the children flat; nested sizers would leave their controls at (0,0).
    row->Add(deps.status, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, m_em_unit);

    // Publish both fields together: an empty list alone means unrestricted in
    // legacy profiles. An explicit None is represented by the rule "false".
    const auto apply = [this, &deps](std::vector<std::string> names, std::string condition) {
        DynamicPrintConfig candidate = *m_config;
        candidate.set_key_value(deps.key_list, new ConfigOptionStrings(std::move(names)));
        candidate.set_key_value(deps.key_condition, new ConfigOptionString(std::move(condition)));
        // These metadata keys may be absent in a legacy imported profile.
        // load_config() compares only shared keys, so install both explicitly.
        m_config->apply_only(candidate, {deps.key_list, deps.key_condition});
        m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
        reload_config();
        update_dirty();
        update();
        compatible_widget_reload(deps);
    };
    const auto select = [this, parent, &deps, apply]() {
        const auto technology = m_preset_bundle->printers.get_edited_preset().printer_technology();
        const auto &collection = deps.type == Preset::TYPE_PRINTER ? static_cast<const PresetCollection&>(m_preset_bundle->printers) :
            m_preset_bundle->sla_prints;
        wxArrayString labels;
        std::vector<std::string> names;
        for (const auto &preset : collection) {
            if (preset.is_default || preset.is_external ||
                (deps.type == Preset::TYPE_PRINTER && preset.printer_technology() != technology))
                continue;
            names.push_back(preset.name);
            labels.Add(from_u8(preset.name));
        }
        const auto *current = m_config->option<ConfigOptionStrings>(deps.key_list);
        // Missing/imported references stay visible and selected. Opening this
        // dialog must not silently erase a restriction to an unavailable profile.
        if (current) for (const auto &name : current->values) {
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
                labels.Add(from_u8(name) + " " + _L("(not installed)"));
            }
        }
        wxMultiChoiceDialog dialog(parent, deps.dialog_title, deps.dialog_label, labels);
        wxGetApp().UpdateDlgDarkUI(&dialog);
        wxArrayInt selections;
        if (current) for (size_t i = 0; i < names.size(); ++i)
            if (std::find(current->values.begin(), current->values.end(), names[i]) != current->values.end())
                selections.Add(int(i));
        dialog.SetSelections(selections);
        if (dialog.ShowModal() != wxID_OK) {
            compatible_widget_reload(deps);
            return;
        }
        std::vector<std::string> selected;
        for (const int index : dialog.GetSelections()) selected.push_back(names.at(size_t(index)));
        const auto *condition = m_config->option<ConfigOptionString>(deps.key_condition);
        const std::string rule = selected.empty() ? "false" : condition ? condition->value : std::string();
        apply(std::move(selected), rule);
    };
    deps.btn->Bind(wxEVT_BUTTON, [select](wxCommandEvent&) { select(); });
    deps.mode->Bind(wxEVT_COMBOBOX, [this, &deps, apply, select](wxCommandEvent&) {
        const auto mode = static_cast<CompatibilityMode>(deps.mode->GetSelection());
        if (mode == CompatibilityMode::Selected) {
            select();
            return;
        }
        std::string condition;
        if (mode == CompatibilityMode::None) condition = "false";
        if (mode == CompatibilityMode::Condition) {
            const auto *current = m_config->option<ConfigOptionString>(deps.key_condition);
            condition = current ? current->value : std::string();
            if (condition.empty() || boost::trim_copy(condition) == "false") condition = "true";
        }
        apply({}, condition);
    });
    return row;
}

wxSizer* TabPrinter::create_bed_shape_widget(wxWindow* parent)
{
    ScalableButton* btn = new ScalableButton(parent, wxID_ANY, "printer", " " + _(L("Set")) + " " + dots,
        wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT);
    btn->SetFont(wxGetApp().normal_font());
    btn->SetSize(btn->GetBestSize());

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL);

    btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e)
        {
            BedShapeDialog dlg(this);
            dlg.build_dialog(*m_config->option<ConfigOptionPoints>("bed_shape"),
                *m_config->option<ConfigOptionString>("bed_custom_texture"),
                *m_config->option<ConfigOptionString>("bed_custom_model"));
            if (dlg.ShowModal() == wxID_OK) {
                const std::vector<Vec2d>& shape = dlg.get_shape();
                const std::string& custom_texture = dlg.get_custom_texture();
                const std::string& custom_model = dlg.get_custom_model();
                if (!shape.empty())
                {
                    load_key_value("bed_shape", shape);
                    load_key_value("bed_custom_texture", custom_texture);
                    load_key_value("bed_custom_model", custom_model);
                    update_changed_ui();
                }
            }
        }));

    // may be it is not a best place, but 
    // add information about Category/Grope for "bed_custom_texture" and "bed_custom_model" as a copy from "bed_shape" option
    {
        Search::OptionsSearcher& searcher = wxGetApp().searcher();
        const Search::GroupAndCategory& gc = searcher.get_group_and_category("bed_shape");
        searcher.add_key("bed_custom_texture", m_type, gc.group, gc.category);
        searcher.add_key("bed_custom_model", m_type, gc.group, gc.category);
    }

    return sizer;
}

void TabPrinter::update_sla_prusa_specific_visibility()
{
    if (m_active_page && m_active_page->title() == "General") {
        auto og_it = std::find_if(m_active_page->m_optgroups.begin(), m_active_page->m_optgroups.end(), [](const ConfigOptionsGroupShp og) { return og->title == "Tilt"; });
        if (og_it != m_active_page->m_optgroups.end()) {            
            og_it->get()->Show(m_mode == comExpert && !is_prusa_printer());
            Layout();
        }
    }
}

void Tab::compatible_widget_reload(PresetDependencies &deps)
{
    Field *field = this->get_field(deps.key_condition);
    if (!field || !deps.mode || !deps.status) return;
    const auto *names = m_config->option<ConfigOptionStrings>(deps.key_list);
    const auto *rule = m_config->option<ConfigOptionString>(deps.key_condition);
    const std::string condition = rule ? boost::trim_copy(rule->value) : std::string();
    const auto mode = names && !names->values.empty() ? CompatibilityMode::Selected :
        condition.empty() ? CompatibilityMode::All :
        condition == "false" ? CompatibilityMode::None : CompatibilityMode::Condition;
    deps.mode->SetSelection(static_cast<int>(mode));
    deps.btn->Enable(mode == CompatibilityMode::Selected);
    field->toggle(mode == CompatibilityMode::Condition);

    const auto printer = m_preset_bundle->printers.get_edited_preset_with_vendor_profile();
    const auto current = m_presets->get_edited_preset_with_vendor_profile();
    const auto &prints = m_preset_bundle->sla_prints;
    const auto result = deps.type == Preset::TYPE_PRINTER ? evaluate_compatibility_with_printer(current, printer) :
        evaluate_compatibility_with_print(current, prints.get_edited_preset_with_vendor_profile(), printer);
    wxString text;
    // Option rows have a fixed height. Keep the status to one line; full parser
    // diagnostics remain available through the tooltip and accessibility API.
    if (!result.error.empty()) text = _L("Invalid condition (hover for details).");
    else if (deps.type == Preset::TYPE_PRINTER)
        text = result.compatible ? _L("Compatible with the current printer.") : _L("Not compatible with the current printer.");
    else text = result.compatible ? _L("Compatible with the current print profile.") : _L("Not compatible with the current print profile.");
    if (result.error.empty()) deps.status->UnsetToolTip();
    else deps.status->SetToolTip(from_u8(result.error));
    if (deps.status->GetLabel() != text) {
        deps.status->SetLabel(text);
        deps.status->GetParent()->Layout();
    }
}

void Tab::fill_icon_descriptions()
{
    m_icon_descriptions.emplace_back(&m_bmp_value_lock, L("LOCKED LOCK"),
        // TRN Description for "LOCKED LOCK"
        L("indicates that the settings are the same as the system (or default) values for the current option group"));

    m_icon_descriptions.emplace_back(&m_bmp_value_unlock, L("UNLOCKED LOCK"),
        // TRN Description for "UNLOCKED LOCK"
        L("indicates that some settings were changed and are not equal to the system (or default) values for "
        "the current option group.\n"
        "Click the UNLOCKED LOCK icon to reset all settings for current option group to "
        "the system (or default) values."));

    m_icon_descriptions.emplace_back(&m_bmp_white_bullet, L("WHITE BULLET"),
        // TRN Description for "WHITE BULLET"
        L("for the left button: indicates a non-system (or non-default) preset,\n"
          "for the right button: indicates that the settings hasn't been modified."));

    m_icon_descriptions.emplace_back(&m_bmp_value_revert, L("BACK ARROW"),
        // TRN Description for "BACK ARROW"
        L("indicates that the settings were changed and are not equal to the last saved preset for "
        "the current option group.\n"
        "Click the BACK ARROW icon to reset all settings for the current option group to "
        "the last saved preset."));

    m_icon_descriptions.emplace_back(&m_bmp_edit_value, L("EDIT VALUE"),
        // TRN Description for "EDIT VALUE" in the Help dialog (the icon is currently used only to edit custom outputs).
        L("clicking this icon opens a dialog allowing to edit this value."));
}

void Tab::set_tooltips_text()
{
    // --- Tooltip text for reset buttons (for whole options group)
    // Text to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    m_ttg_value_lock =		_(L("LOCKED LOCK icon indicates that the settings are the same as the system (or default) values "
                                "for the current option group"));
    m_ttg_value_unlock =	_(L("UNLOCKED LOCK icon indicates that some settings were changed and are not equal "
                                "to the system (or default) values for the current option group.\n"
                                "Click to reset all settings for current option group to the system (or default) values."));
    m_ttg_white_bullet_ns =	_(L("WHITE BULLET icon indicates a non system (or non default) preset."));
    m_ttg_non_system =		&m_ttg_white_bullet_ns;
    // Text to be shown on the "Undo user changes" button next to each input field.
    m_ttg_white_bullet =	_(L("WHITE BULLET icon indicates that the settings are the same as in the last saved "
                                "preset for the current option group."));
    m_ttg_value_revert =	_(L("BACK ARROW icon indicates that the settings were changed and are not equal to "
                                "the last saved preset for the current option group.\n"
                                "Click to reset all settings for the current option group to the last saved preset."));

    // --- Tooltip text for reset buttons (for each option in group)
    // Text to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    m_tt_value_lock =		_(L("LOCKED LOCK icon indicates that the value is the same as the system (or default) value."));
    m_tt_value_unlock =		_(L("UNLOCKED LOCK icon indicates that the value was changed and is not equal "
                                "to the system (or default) value.\n"
                                "Click to reset current value to the system (or default) value."));
    // 	m_tt_white_bullet_ns=	_(L("WHITE BULLET icon indicates a non system preset."));
    m_tt_non_system =		&m_ttg_white_bullet_ns;
    // Text to be shown on the "Undo user changes" button next to each input field.
    m_tt_white_bullet =		_(L("WHITE BULLET icon indicates that the value is the same as in the last saved preset."));
    m_tt_value_revert =		_(L("BACK ARROW icon indicates that the value was changed and is not equal to the last saved preset.\n"
                                "Click to reset current value to the last saved preset."));
}

bool Tab::select_preset_by_name(const std::string &name_w_suffix, bool force)
{
    return m_presets->select_preset_by_name(name_w_suffix, force);
}

bool Tab::save_current_preset(const std::string& new_name, bool detach)
{
    return m_presets->save_current_preset(new_name, detach);
}

bool Tab::delete_current_preset()
{
    const Preset &selected = m_presets->get_selected_preset();
    if (!selected.is_default && !selected.is_system && !selected.is_external &&
        (m_type == Preset::TYPE_PRINTER || m_type == Preset::TYPE_SLA_PRINT || m_type == Preset::TYPE_SLA_MATERIAL))
        // The user has seen the dependents in Tab::delete_preset(); the references are fixed in the same
        // file transaction as the deletion, so a failure leaves every profile untouched.
        return m_preset_bundle->delete_preset_with_dependencies(m_type, selected.name);
    return m_presets->delete_current_preset();
}

void Tab::export_preset_ini()
{
    const Preset &preset = m_presets->get_selected_preset();
    if (preset.is_default || preset.is_external)
        return;
    if (m_presets->current_is_dirty() &&
        wxID_YES != MessageDialog(parent(), _L("The preset has unsaved changes; only the saved version is exported. Continue?"),
                                  _L("Export preset"), wxYES_NO | wxICON_QUESTION).ShowModal())
        return;
    wxFileDialog dlg(this, _L("Export preset as:"), wxGetApp().app_config->get_last_dir(),
                     from_u8(preset.name) + ".ini", file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    try {
        m_preset_bundle->export_preset_ini(m_type, preset.name, into_u8(dlg.GetPath()));
    } catch (const std::exception &ex) {
        wxMessageBox(from_u8(ex.what()), _L("Export preset"), wxOK | wxICON_ERROR, this);
    }
}

void Tab::import_preset_ini()
{
    wxFileDialog dlg(this, _L("Import preset from:"), wxGetApp().app_config->get_last_dir(),
                     wxEmptyString, file_wildcards(FT_INI), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    try {
        const auto result = m_preset_bundle->import_preset_ini(m_type, into_u8(dlg.GetPath()));
        if (result.renamed)
            show_info(this, format_wxstr(_L("A preset with this name already exists. The imported preset was saved as \"%1%\"."), result.name),
                      _L("Import preset"));
        // Selecting the preset goes through the normal path so unsaved edits are handled as usual.
        select_preset(result.name);
    } catch (const std::exception &ex) {
        wxMessageBox(from_u8(ex.what()), _L("Import preset"), wxOK | wxICON_ERROR, this);
    }
}

Page::Page(wxWindow* parent, const wxString& title, int iconID) :
        m_parent(parent),
        m_title(title),
        m_iconID(iconID)
{
    m_vsizer = (wxBoxSizer*)parent->GetSizer();
    m_item_color = &wxGetApp().get_label_clr_default();
}

void Page::reload_config()
{
    for (auto group : m_optgroups)
        group->reload_config();
}

void Page::update_visibility(ConfigOptionMode mode, bool update_contolls_visibility)
{
    bool ret_val = false;
    for (auto group : m_optgroups) {
        ret_val = (update_contolls_visibility     ? 
                   group->update_visibility(mode) :  // update visibility for all controlls in group
                   group->is_visible(mode)           // just detect visibility for the group
                   ) || ret_val;
    }

    m_show = ret_val;
}

void Page::activate(ConfigOptionMode mode, std::function<void()> throw_if_canceled)
{
    for (auto group : m_optgroups) {
        if (!group->activate(throw_if_canceled))
            continue;
        m_vsizer->Add(group->sizer, 0, wxEXPAND | (group->is_legend_line() ? (wxLEFT|wxTOP) : wxALL), 10);
        group->update_visibility(mode);
        group->reload_config();
        throw_if_canceled();
    }
}

void Page::clear()
{
    for (auto group : m_optgroups)
        group->clear();
}

void Page::msw_rescale()
{
    for (auto group : m_optgroups)
        group->msw_rescale();
}

void Page::sys_color_changed()
{
    for (auto group : m_optgroups)
        group->sys_color_changed();
}

void Page::refresh()
{
    for (auto group : m_optgroups)
        group->refresh();
}

Field* Page::get_field(const t_config_option_key& opt_key, int opt_index /*= -1*/) const
{
    Field* field = nullptr;
    for (auto opt : m_optgroups) {
        field = opt->get_fieldc(opt_key, opt_index);
        if (field != nullptr)
            return field;
    }
    return field;
}

Line* Page::get_line(const t_config_option_key& opt_key)
{
    for (auto opt : m_optgroups)
        if (Line* line = opt->get_line(opt_key))
            return line;
    return nullptr;
}

bool Page::set_value(const t_config_option_key& opt_key, const boost::any& value) {
    bool changed = false;
    for(auto optgroup: m_optgroups) {
        if (optgroup->set_value(opt_key, value))
            changed = true ;
    }
    return changed;
}

// package Slic3r::GUI::Tab::Page;
ConfigOptionsGroupShp Page::new_optgroup(const wxString& title, int noncommon_label_width /*= -1*/)
{
    //! config_ have to be "right"
    ConfigOptionsGroupShp optgroup = std::make_shared<ConfigOptionsGroup>(m_parent, title, m_config, true);
    if (noncommon_label_width >= 0)
        optgroup->label_width = noncommon_label_width;

#ifdef __WXOSX__
    Tab* tab = static_cast<Tab*>(parent()->GetParent()->GetParent());
#else
    Tab* tab = static_cast<Tab*>(parent()->GetParent());
#endif
    optgroup->set_config_category_and_type(m_title, static_cast<Tab*>(tab)->type());
    optgroup->on_change = [tab](t_config_option_key opt_key, boost::any value) {
        // This function will be called from OptionGroup.
        tab->update_dirty();
        tab->on_value_change(opt_key, value);
    };

    optgroup->get_initial_config = [tab]() {
        return tab->m_presets->get_selected_preset().config;
    };

    optgroup->get_sys_config = [tab]() {
        return tab->m_presets->get_selected_preset_parent()->config;
    };

    optgroup->have_sys_config = [tab]() {
        return tab->m_presets->get_selected_preset_parent() != nullptr;
    };

    optgroup->rescale_extra_column_item = [](wxWindow* win) {
        auto *ctrl = dynamic_cast<wxStaticBitmap*>(win);
        if (ctrl == nullptr)
            return;

        ctrl->SetBitmap(reinterpret_cast<ScalableBitmap*>(ctrl->GetClientData())->bmp());
    };

    m_optgroups.push_back(optgroup);

    return optgroup;
}

const ConfigOptionsGroupShp Page::get_optgroup(const wxString& title) const
{
    for (ConfigOptionsGroupShp optgroup : m_optgroups) {
        if (optgroup->title == title)
            return optgroup;
    }

    return nullptr;
}

void TabSLAMaterial::build()
{
    m_presets = &m_preset_bundle->sla_materials;
    load_initial_data();

    auto page = add_options_page(L("Material"), "resin");

    auto optgroup = page->new_optgroup(L("Material"));
    optgroup->append_single_option_line("material_colour");
    optgroup->append_single_option_line("bottle_cost");
    optgroup->append_single_option_line("bottle_volume");
    optgroup->append_single_option_line("bottle_weight");
    optgroup->append_single_option_line("material_density");
    optgroup->append_single_option_line("material_type");
    {
        // material_vendor has no label in the option definition; label it here for the editor.
        Option vendor = optgroup->get_option("material_vendor");
        vendor.opt.label = L("Vendor");
        vendor.opt.tooltip = L("Resin manufacturer shown in the material list.");
        optgroup->append_single_option_line(vendor);
    }

    optgroup->on_change = [this](t_config_option_key opt_key, boost::any value)
    {
        if (opt_key == "material_colour") {
            update_dirty();
            on_value_change(opt_key, value); 
            return;
        }

        DynamicPrintConfig new_conf = *m_config;

        if (const auto derived = sla::derive_bottle_setting(opt_key,
                new_conf.opt_float("bottle_volume"), new_conf.opt_float("bottle_weight"),
                new_conf.opt_float("material_density")))
            new_conf.set(derived->key, derived->value);

        load_config(new_conf);

        update_dirty();
        // The edited primary option is already stored by ConfigOptionsGroup.
        // load_config only updates the engine when a derived option differs;
        // forward price-only edits (and unchanged dependencies) as well.
        on_value_change(opt_key, value);

        // Change of any from those options influences for an update of "Sliced Info"
        wxGetApp().sidebar().update_sliced_info_sizer();
        wxGetApp().sidebar().Layout();
    };

    optgroup = page->new_optgroup(L("Layers"));
    optgroup->append_single_option_line("initial_layer_height");

    optgroup = page->new_optgroup(L("Exposure"));
    optgroup->append_single_option_line("exposure_time");
    optgroup->append_single_option_line("initial_exposure_time");

    auto lcd_page = add_options_page(L("LCD exposure and motion"), "resin");
    auto compatibility=lcd_page->new_optgroup(L("Output compatibility"));
    Line compatibility_line{"", ""}; compatibility_line.full_width=1;
    compatibility_line.widget=[this](wxWindow *parent) {
        ogStaticText *description=nullptr;
        auto *layout=description_line_widget(parent,&description);
        description->SetName("SLA output compatibility");
        return layout;
    };
    compatibility->append_line(compatibility_line);
    auto counts = lcd_page->new_optgroup(L("Exposure layers"));
    counts->append_single_option_line("sla_bottom_layers");
    counts->append_single_option_line("sla_transition_layers");
    auto normal = lcd_page->new_optgroup(L("Normal layers"));
    auto bottom = lcd_page->new_optgroup(L("Bottom layers"));
    for (const auto &setting : sla::machine_settings) {
        if (std::string(setting.key) == "sla_bottom_layers" || std::string(setting.key) == "sla_transition_layers") continue;
        (setting.bottom ? bottom : normal)->append_single_option_line(setting.key);
    }

    optgroup = page->new_optgroup(L("Corrections"));
    auto line = Line{ m_config->def()->get("material_correction")->full_label, "" };
    for (auto& axis : { "X", "Y", "Z" }) {
        auto opt = optgroup->get_option(std::string("material_correction_") + char(std::tolower(axis[0])));
        opt.opt.label = axis;
        line.append_option(opt);
    }
    optgroup->append_line(line);

    optgroup->append_single_option_line("zcorrection_layers");

    line = Line{ "", "" };
    line.full_width = 1;
    // line.label_path = category_path + "recommended-thin-wall-thickness";
    line.widget = [this](wxWindow* parent) {
        return description_line_widget(parent, &m_z_correction_to_mm_description);
    };
    optgroup->append_line(line);

    add_material_overrides_page();

    page = add_options_page(L("Notes"), "note");
    optgroup = page->new_optgroup(L("Notes"), 0);
    optgroup->label_width = 0;
    Option option = optgroup->get_option("material_notes");
    option.opt.full_width = true;
    option.opt.height = 25;//250;
    optgroup->append_single_option_line(option);

    page = add_options_page(L("Dependencies"), "wrench");
    optgroup = page->new_optgroup(L("Profile dependencies"));

    create_line_with_widget(optgroup.get(), "compatible_printers", "", [this](wxWindow* parent) {
        return compatible_widget_create(parent, m_compatible_printers);
    });
    
    option = optgroup->get_option("compatible_printers_condition");
    option.opt.full_width = true;
    optgroup->append_single_option_line(option);

    create_line_with_widget(optgroup.get(), "compatible_prints", "", [this](wxWindow* parent) {
        return compatible_widget_create(parent, m_compatible_prints);
    });

    option = optgroup->get_option("compatible_prints_condition");
    option.opt.full_width = true;
    optgroup->append_single_option_line(option);

    build_preset_description_line(optgroup.get());

    page = add_options_page(L("Material printing profile"), "note");

#if 1
    optgroup = page->new_optgroup(L("Material printing profile"));
    optgroup->append_single_option_line("material_print_speed");

    line = { optgroup->get_option("tower_hop_height", 0).opt.full_label, "" };
    line.append_option(optgroup->get_option("tower_hop_height", 0));
    line.append_option(optgroup->get_option("tower_hop_height", 1));
    optgroup->append_line(line);

    optgroup = page->new_optgroup(L("Tilt"));
    optgroup->append_single_option_line("area_fill");

#else
    optgroup = page->new_optgroup(L("Material printing profile"));
    option = optgroup->get_option("material_print_speed");
    optgroup->append_single_option_line(option);

    optgroup->append_single_option_line("area_fill");
#endif

    build_tilt_group(page);
    check_options_have_editors(Preset::sla_material_options());
}

static void append_tilt_options_line(ConfigOptionsGroupShp optgroup, const std::string opt_key)
{
    auto option = optgroup->get_option(opt_key, 0);
    auto line = Line{ option.opt.full_label, "" };
    option.opt.width = Field::def_width/*_wider*/();
    line.append_option(option);

    option = optgroup->get_option(opt_key, 1);
    option.opt.width = Field::def_width/*_wider*/();
    line.append_option(option);

    optgroup->append_line(line);
}

static void create_tilt_legend(ConfigOptionsGroupShp optgroup)
{
    // Legend
    std::vector<std::pair<std::string, std::string>> columns = {
        // TRN: This is a label of a column of parameters in settings to be used when the area is below certain threshold.
        {L("Below"),
        L("Values in this column are applied when layer area is smaller than area_fill.")},
        // TRN: This is a label of a column of parameters in settings to be used when the area is above certain threshold.
        {L("Above"),
        L("Values in this column are applied when layer area is larger than area_fill.")},
    };

    auto legend = [columns](wxWindow* parent) {
        auto legend_sizer = new wxBoxSizer(wxHORIZONTAL);
        legend_sizer->Add(new wxStaticText(parent, wxID_ANY, "", wxDefaultPosition, wxSize(25*em_unit(parent), -1)));
        for (auto& [name, tooltip] : columns) {
            auto legend_item = new wxStaticText(parent, wxID_ANY, _(name), wxDefaultPosition, wxSize(20*em_unit(parent), -1));
            legend_item->SetToolTip(_(tooltip));
            legend_sizer->Add(legend_item);
        }

        return legend_sizer;
    };

    Line line = Line{ "", "" };
    line.full_width = 1;
    line.append_widget(legend);
    optgroup->append_line(line);
}

void TabSLAMaterial::build_tilt_group(Slic3r::GUI::PageShp page)
{
    // TRN: 'Profile' in this context denotes a group of parameters used to configure
    //      layer separation procedure for SLA printers.
    auto optgroup = page->new_optgroup(L("Profile settings"));
    optgroup->on_change = [this](const t_config_option_key& key, boost::any value)
    {
        if (key.find("use_tilt") == 0)
            toggle_tilt_options(key == "use_tilt#0");

        update_dirty();
        update();
    };

    create_line_with_tilt_defaults(optgroup);
    create_tilt_legend(optgroup);

    for (const std::string& opt_key : tilt_options())
        append_tilt_options_line(optgroup, opt_key);
}

std::vector<std::tuple<wxString, wxString, int>> default_tilt_buttons = {
    { _L("Fast"),           _L("Set default values for fast print speed"),           SLAMaterialSpeed::slamsFast },
    { _L("Slow"),           _L("Set default values for slow print speed"),           SLAMaterialSpeed::slamsSlow },
    { _L("High viscosity"), _L("Set default values for high viscosity print speed"), SLAMaterialSpeed::slamsHighViscosity }
};

void TabSLAMaterial::create_line_with_tilt_defaults(ConfigOptionsGroupShp optgroup)
{
    auto print_speed_btns = [this](wxWindow* parent) {
        m_tilt_defaults_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto grid_sizer = new wxGridSizer(3, 0, 0);
        for (const auto& [label, tooltip, material_speed] : default_tilt_buttons) {
            ScalableButton* btn;
            add_scaled_button(parent, &btn, "cog", label + "   ", wxBU_EXACTFIT);
            btn->SetToolTip(tooltip);
            btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());

            int tilt_mode = int(material_speed);
            btn->Bind(wxEVT_BUTTON, [this, tilt_mode](wxCommandEvent&) {
                DynamicPrintConfig new_conf = *m_config;
                update_tilts_by_mode(new_conf, tilt_mode, false);
                load_config(new_conf);
            });
            grid_sizer->Add(btn, 1, wxEXPAND | wxRIGHT, 5);
        }

        m_tilt_defaults_sizer->Add(grid_sizer, 0, wxALIGN_CENTRE_VERTICAL);
        return m_tilt_defaults_sizer;
    };

    Line line = Line{ "", "" };
    line.full_width = 1;
    line.append_widget(print_speed_btns);
    optgroup->append_line(line);
}

std::vector<std::string> disable_tilt_options = {
         "tilt_down_initial_speed"
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

void TabSLAMaterial::toggle_tilt_options(bool is_above)
{
    if (m_active_page && m_active_page->title() == "Material printing profile")
    {
        int column_id = is_above ? 0 : 1;
        auto optgroup = m_active_page->get_optgroup("Profile settings");
        bool use_tilt = boost::any_cast<bool>(optgroup->get_config_value(*m_config, "use_tilt", column_id));

        for (const std::string& opt_key : disable_tilt_options) {
            auto field = optgroup->get_fieldc(opt_key, column_id);
            if (field != nullptr)
                field->toggle(use_tilt);
        }
    }
}

void TabSLAMaterial::toggle_options()
{
    if (m_active_page->title() == "Material Overrides")
        update_material_overrides_page();
}

void TabSLAMaterial::update()
{
    toggle_options();

    update_description_lines();
    Layout();

// #ys_FIXME. Just a template for this function
//     m_update_cnt++;
//     ! something to update
//     m_update_cnt--;
//
//     if (m_update_cnt == 0)
    wxGetApp().mainframe->on_config_changed(m_config);
}

void TabSLAMaterial::update_description_lines()
{
    if (auto *description=dynamic_cast<ogStaticText*>(wxWindow::FindWindowByName("SLA output compatibility",this))) {
        try {
            const auto &printer=m_preset_bundle->printers.get_edited_preset().config;
            const auto format=printer.opt_string("sla_archive_format");
            const auto version=sla::generated_anycubic_version(format,printer.opt_string("sla_anycubic_version"));
            wxString text="Output format: ."+wxString::FromUTF8(format);
            if (version) text+=wxString::Format(" (version %u)",unsigned(*version));
            if (version && *version>=516 && *version<=518) {
                const auto error=sla::anycubic_generated_settings_error(*m_config);
                text+="\n";
                text+=error.empty()?"Normal and bottom wait before cure share one value. Wait after cure/lift and secondary retract distance must be zero; power must be 255.":wxString::FromUTF8(error);
            } else text+="\nExport checks the limitations of this format and the current layers.";
            description->SetText(text);
        } catch (const std::exception &error) {
            description->SetText("Output format: "+wxString::FromUTF8(error.what()));
        }
    }

    if (m_active_page && m_active_page->title() == "Material" &&  m_z_correction_to_mm_description) {
        double lh = m_preset_bundle->sla_prints.get_edited_preset().config.opt_float("layer_height");
        int zlayers = m_config->opt_int("zcorrection_layers");
        m_z_correction_to_mm_description->SetText(GUI::format_wxstr(_L("The current Z-axis height correction is: %1% mm"), zlayers * lh));
    }

    Tab::update_description_lines();
}

std::string Tab::printer_model() const
{
    const Preset& edited_preset = m_preset_bundle->printers.get_edited_preset();
    return edited_preset.trim_vendor_repo_prefix(edited_preset.config.opt_string("printer_model"));
}

bool Tab::is_prusa_printer() const
{
    return SLAPrint::is_prusa_print(printer_model());
}

void TabSLAMaterial::update_sla_prusa_specific_visibility()
{
    if (m_active_page && m_active_page->title() == "Material printing profile") {
        for (auto& title : { "", "Profile settings" }) {
            auto og_it = std::find_if(m_active_page->m_optgroups.begin(), m_active_page->m_optgroups.end(), 
                         [title](const ConfigOptionsGroupShp og) { return og->title == title; });
            if (og_it != m_active_page->m_optgroups.end()) {
                og_it->get()->Show(m_mode >= comAdvanced && is_prusa_printer());
                const std::string pr_model = printer_model();
                m_tilt_defaults_sizer->Show(pr_model == "SL1S" || pr_model == "M1");
            }
        }

        auto og_it = std::find_if(m_active_page->m_optgroups.begin(), m_active_page->m_optgroups.end(), 
                        [](const ConfigOptionsGroupShp og) { return og->title == "Material printing profile"; });
        if (og_it != m_active_page->m_optgroups.end())
            og_it->get()->Show(m_mode >= comAdvanced && !is_prusa_printer());

        Layout();
    }
}

void TabSLAMaterial::clear_pages()
{
    Tab::clear_pages();

    for (auto& over_opt : m_overrides_options)
        over_opt.second = nullptr;

    m_z_correction_to_mm_description = nullptr;
    m_tilt_defaults_sizer = nullptr;
}

void TabSLAMaterial::msw_rescale()
{
    for (const auto& over_opt : m_overrides_options)
        if (wxWindow* win = over_opt.second)
            win->SetInitialSize(win->GetBestSize());
    Tab::msw_rescale();
}

void TabSLAMaterial::sys_color_changed()
{
    Tab::sys_color_changed();

    for (const auto& over_opt : m_overrides_options)
        if (wxWindow* check_box = over_opt.second) {
            wxGetApp().UpdateDarkUI(check_box);
            CheckBox::SysColorChanged(check_box);
        }
}

static void add_options_into_line(ConfigOptionsGroupShp &optgroup,
                                  const std::vector<SamePair<std::string>> &prefixes,
                                  const std::string &optkey,
                                  const std::string &preprefix = std::string())
{
    auto opt = optgroup->get_option(preprefix + prefixes.front().first + optkey);
    Line line{ opt.opt.label, "" };
    line.full_width = 1;
    for (auto &prefix : prefixes) {
        opt = optgroup->get_option(preprefix + prefix.first + optkey);
        opt.opt.label = prefix.second;
        opt.opt.width = 12; // TODO
        line.append_option(opt);
    }
    optgroup->append_line(line);
}

void TabSLAPrint::build_sla_support_params(const std::vector<SamePair<std::string>> &prefixes,
                                           const Slic3r::GUI::PageShp &page)
{

    auto optgroup = page->new_optgroup(L("Support head"));
    add_options_into_line(optgroup, prefixes, "support_head_front_diameter");
    add_options_into_line(optgroup, prefixes, "support_head_penetration");
    add_options_into_line(optgroup, prefixes, "support_head_width");

    optgroup = page->new_optgroup(L("Support pillar"));
    add_options_into_line(optgroup, prefixes, "support_pillar_diameter");
    add_options_into_line(optgroup, prefixes, "support_small_pillar_diameter_percent");
    add_options_into_line(optgroup, prefixes, "support_max_bridges_on_pillar");

    add_options_into_line(optgroup, prefixes, "support_pillar_connection_mode");
    add_options_into_line(optgroup, prefixes, "support_buildplate_only");
    add_options_into_line(optgroup, prefixes, "support_pillar_widening_factor");
    add_options_into_line(optgroup, prefixes, "support_max_weight_on_model");
    add_options_into_line(optgroup, prefixes, "support_base_diameter");
    add_options_into_line(optgroup, prefixes, "support_base_height");
    add_options_into_line(optgroup, prefixes, "support_base_safety_distance");

    // Mirrored parameter from Pad page for toggling elevation on the same page
    add_options_into_line(optgroup, prefixes, "support_object_elevation");

    Line line{ "", "" };
    line.full_width = 1;
    line.widget = [this](wxWindow* parent) {
        return description_line_widget(parent, &m_support_object_elevation_description_line);
    };
    optgroup->append_line(line);

    optgroup = page->new_optgroup(L("Connection of the support sticks and junctions"));
    add_options_into_line(optgroup, prefixes, "support_critical_angle");
    add_options_into_line(optgroup, prefixes, "support_max_bridge_length");
    add_options_into_line(optgroup, prefixes, "support_max_pillar_link_distance");
}

static std::vector<std::string> get_override_opt_kyes_for_line(const std::string& title, const std::string& key)
{
    const std::string preprefix = "material_ow_";

    std::vector<std::string> opt_keys;
    opt_keys.reserve(3);

    if (title == "Support head" || title == "Support pillar") {
        for (auto& prefix : { "", "branching" })
            opt_keys.push_back(preprefix + prefix + key);
    }
    else
        opt_keys.push_back(preprefix + key);

    return opt_keys;
}

void TabSLAMaterial::create_line_with_near_label_widget(ConfigOptionsGroupShp optgroup, const std::string& key)
{
    if (optgroup->title == "Support head" || optgroup->title == "Support pillar")
        add_options_into_line(optgroup, { {"", L("Default")}, {"branching", L("Branching")} }, key, "material_ow_");
    else {
        const std::string opt_key = std::string("material_ow_") + key;
        optgroup->append_single_option_line(opt_key);
    }

    Line* line = optgroup->get_last_line();
    if (!line)
        return;

    line->near_label_widget = [this, optgroup_wk = ConfigOptionsGroupWkp(optgroup), key](wxWindow* parent) {
        wxWindow* check_box = CheckBox::GetNewWin(parent);
        wxGetApp().UpdateDarkUI(check_box);

        check_box->Bind(wxEVT_CHECKBOX, [this, optgroup_wk, key](wxCommandEvent& evt) {
            const bool is_checked = evt.IsChecked();
            if (auto optgroup_sh = optgroup_wk.lock(); optgroup_sh) {
                auto opt_keys = get_override_opt_kyes_for_line(optgroup_sh->title.ToStdString(), key);
                for (const std::string& opt_key : opt_keys)
                    if (Field* field = optgroup_sh->get_fieldc(opt_key, 0); field != nullptr) {
                        field->toggle(is_checked);
                        if (is_checked)
                            field->set_last_meaningful_value();
                        else
                            field->set_na_value();
                    }
            }

            toggle_options();
        });

        m_overrides_options[key] = check_box;
        return check_box;
    };
}

std::vector<std::pair<std::string, std::vector<std::string>>> material_overrides_option_keys{
    {"Support head", {
        "support_head_front_diameter",
        "support_head_penetration",
        "support_head_width"
    }},
    {"Support pillar", {
        "support_pillar_diameter",
    }},
    {"Automatic generation", {
        "support_points_density_relative"
    }},
    {"Corrections", {
        "absolute_correction",
        "elefant_foot_compensation"
    }}
};

void TabSLAMaterial::add_material_overrides_page()
{
    // TRN: Page title in Material Settings in SLA mode.
    PageShp page = add_options_page(L("Material Overrides"), "wrench");

    for (const auto& [title, keys] : material_overrides_option_keys) {
        ConfigOptionsGroupShp optgroup = page->new_optgroup(L(title));
        for (const std::string& opt_key : keys) {
            create_line_with_near_label_widget(optgroup, opt_key);
        }
    }
}

void TabSLAMaterial::update_line_with_near_label_widget(ConfigOptionsGroupShp optgroup, const std::string& key, bool is_checked/* = true*/)
{
    if (!m_overrides_options[key])
        return;

    const std::string preprefix = "material_ow_";

    std::vector<std::string> opt_keys;
    opt_keys.reserve(3);

    if (optgroup->title == "Support head" || optgroup->title == "Support pillar") {
        for (auto& prefix : { "", "branching" }) {
            std::string opt_key = preprefix + prefix + key;
            is_checked = !m_config->option(opt_key)->is_nil();
            opt_keys.push_back(opt_key);
        }
    }
    else if (key == "relative_correction") {
        for (auto& axis : { "x", "y", "z" }) {
            std::string opt_key = preprefix + key + "_" + char(axis[0]);
            is_checked = !m_config->option(opt_key)->is_nil();
            opt_keys.push_back(opt_key);
        }
    }
    else {
        std::string opt_key = preprefix + key;
        is_checked = !m_config->option(opt_key)->is_nil();
        opt_keys.push_back(opt_key);
    }

    CheckBox::SetValue(m_overrides_options[key], is_checked);

    for (const std::string& opt_key : opt_keys) {
        Field* field = optgroup->get_field(opt_key);
        if (field != nullptr)
            field->toggle(is_checked);
    }
}

static std::optional<ConfigOptionsGroupShp> get_option_group(const Page* page, const std::string& title) {
    auto og_it = std::find_if(
        page->m_optgroups.begin(), page->m_optgroups.end(),
        [&](const ConfigOptionsGroupShp& og) {
            return og->title == title;
        }
    );
    if (og_it == page->m_optgroups.end())
        return {};
    return *og_it;
}

void TabSLAMaterial::update_material_overrides_page()
{
    if (!m_active_page || m_active_page->title() != "Material Overrides")
        return;
    Page* page = m_active_page;

    for (const auto& [title, keys] : material_overrides_option_keys) {
        std::optional<ConfigOptionsGroupShp> optgroup{ get_option_group(page, title) };
        if (!optgroup) {
            continue;
        }

        for (const std::string& key : keys) {
            update_line_with_near_label_widget(*optgroup, key);
        }
    }
}

void TabSLAPrint::build()
{
    m_presets = &m_preset_bundle->sla_prints;
    load_initial_data();

    auto page = add_options_page(L("Layers"), "layers");

    auto optgroup = page->new_optgroup(L("Layers"));
    optgroup->append_single_option_line("layer_height");
    optgroup->append_single_option_line("faded_layers");

    page = add_options_page(L("Supports"), "support"/*"sla_supports"*/);

    optgroup = page->new_optgroup(L("Supports"));
    optgroup->append_single_option_line("supports_enable");
    optgroup->append_single_option_line("support_tree_type");
    optgroup->append_single_option_line("support_enforcers_only");
    
    build_sla_support_params({{"", L("Default")}, {"branching", L("Branching")}}, page);

    optgroup = page->new_optgroup(L("Automatic generation"));
    optgroup->append_single_option_line("support_points_density_relative");

    page = add_options_page(L("Pad"), "pad");
    optgroup = page->new_optgroup(L("Pad"));
    optgroup->append_single_option_line("pad_enable");
    optgroup->append_single_option_line("pad_wall_thickness");
    optgroup->append_single_option_line("pad_wall_height");
    optgroup->append_single_option_line("pad_brim_size");
    optgroup->append_single_option_line("pad_max_merge_distance");
    // TODO: Disabling this parameter for the beta release
//    optgroup->append_single_option_line("pad_edge_radius");
    optgroup->append_single_option_line("pad_wall_slope");

    optgroup->append_single_option_line("pad_around_object");
    optgroup->append_single_option_line("pad_around_object_everywhere");
    optgroup->append_single_option_line("pad_object_gap");
    optgroup->append_single_option_line("pad_object_connector_stride");
    optgroup->append_single_option_line("pad_object_connector_width");
    optgroup->append_single_option_line("pad_object_connector_penetration");
    
    page = add_options_page(L("Hollowing"), "hollowing");
    optgroup = page->new_optgroup(L("Hollowing"));
    optgroup->append_single_option_line("hollowing_enable");
    optgroup->append_single_option_line("hollowing_min_thickness");
    optgroup->append_single_option_line("hollowing_quality");
    optgroup->append_single_option_line("hollowing_closing_distance");

    page = add_options_page(L("Advanced"), "wrench");
    optgroup = page->new_optgroup(L("Slicing"));
    optgroup->append_single_option_line("slice_closing_radius");
    optgroup->append_single_option_line("slicing_mode");

    page = add_options_page(L("Output options"), "output+page_white");
    optgroup = page->new_optgroup(L("Output file"));
    Option option = optgroup->get_option("output_filename_format");
    option.opt.full_width = true;
    optgroup->append_single_option_line(option);

    page = add_options_page(L("Dependencies"), "wrench");
    optgroup = page->new_optgroup(L("Profile dependencies"));

    create_line_with_widget(optgroup.get(), "compatible_printers", "", [this](wxWindow* parent) {
        return compatible_widget_create(parent, m_compatible_printers);
    });

    option = optgroup->get_option("compatible_printers_condition");
    option.opt.full_width = true;
    optgroup->append_single_option_line(option);

    build_preset_description_line(optgroup.get());
    check_options_have_editors(Preset::sla_print_options());
}

void TabSLAPrint::update_description_lines()
{
    Tab::update_description_lines();

    if (m_active_page && m_active_page->title() == "Supports")
    {
        bool is_visible = m_config->def()->get("support_object_elevation")->mode <= m_mode;
        if (m_support_object_elevation_description_line)
        {
            m_support_object_elevation_description_line->Show(is_visible);
            if (is_visible)
            {
                bool elev = !m_config->opt_bool("pad_enable") || !m_config->opt_bool("pad_around_object");
                m_support_object_elevation_description_line->SetText(elev ? "" :
                    format_wxstr(_L("\"%1%\" is disabled because \"%2%\" is on in \"%3%\" category.\n"
                        "To enable \"%1%\", please switch off \"%2%\"")
                        , _L("Object elevation"), _L("Pad around object"), _L("Pad")));
            }
        }
    }
}

void TabSLAPrint::toggle_options()
{
    if (m_active_page)
        m_config_manipulation.toggle_print_sla_options(m_config);
}

void TabSLAPrint::update()
{
    m_update_cnt++;

    update_description_lines();
    Layout();

    m_update_cnt--;

    if (m_update_cnt == 0) {
        toggle_options();

        // update() could be called during undo/redo execution
        // Update of objectList can cause a crash in this case (because m_objects doesn't match ObjectList) 
        if (!wxGetApp().plater()->inside_snapshot_capture())
            wxGetApp().obj_list()->update_and_show_object_settings_item();

        wxGetApp().mainframe->on_config_changed(m_config);
    }
}

void TabSLAPrint::clear_pages()
{
    Tab::clear_pages();

    m_support_object_elevation_description_line = nullptr;
}

ConfigManipulation Tab::get_config_manipulation()
{
    auto load_config = [this]()
    {
        update_dirty();
        // Initialize UI components with the config values.
        reload_config();
        update();
    };

    auto cb_toggle_field = [this](const t_config_option_key& opt_key, bool toggle, int opt_index) {
        return toggle_option(opt_key, toggle, opt_index);
    };

    auto cb_value_change = [this](const std::string& opt_key, const boost::any& value) {
        return on_value_change(opt_key, value);
    };

    return ConfigManipulation(load_config, cb_toggle_field, cb_value_change, nullptr, this);
}


} // GUI
} // Slic3r
