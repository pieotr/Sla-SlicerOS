///|/ Copyright (c) Prusa Research 2018 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966, Lukáš Hejl @hejllukas, Vojtěch Král @vojtechkral
///|/
///|/ ported from lib/Slic3r/GUI/ConfigWizard.pm:
///|/ Copyright (c) Prusa Research 2016 - 2018 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2012 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
// FIXME: extract absolute units -> em

#include "ConfigWizard_private.hpp"

#include <algorithm>
#include <numeric>
#include <utility>
#include <unordered_map>
#include <stdexcept>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/dll/runtime_symbol_info.hpp>

#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/statbmp.h>
#include <wx/checkbox.h>
#include <wx/statline.h>
#include <wx/dataview.h>
#include <wx/notebook.h>
#include <wx/listbook.h>
#include <wx/display.h>
#include <wx/filefn.h>
#include <wx/wupdlock.h>
#include <wx/wrapsizer.h>
#include <wx/stdpaths.h>
#include <wx/debug.h>
#include <wx/weakref.h>

#ifdef WIN32
#include <wx/msw/registry.h>
#include <KnownFolders.h>
#include <Shlobj_core.h>
#endif // WIN32

#ifdef _MSW_DARK_MODE
#include <wx/msw/dark_mode.h>
#endif // _MSW_DARK_MODE

#include "libslic3r/Platform.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/SLA/PrinterGeometry.hpp"
#include <limits>
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "Field.hpp"
#include "DesktopIntegrationDialog.hpp"
#include "slic3r/Config/Snapshot.hpp"
#include "format.hpp"
#include "MsgDialog.hpp"
#include "UnsavedChangesDialog.hpp"
#include "Plater.hpp" // #ysFIXME - implement getter for preset_archive_database from GetApp()???
#include "slic3r/Utils/AppUpdater.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Config/Version.hpp"

#ifndef _L
#define _L(s) wxString::FromUTF8(s)
#endif
#ifndef _u8L
#define _u8L(s) std::string(s)
#endif


/* ysFIXME - delete after testing and release
// it looks like this workaround is no need any more after update of the wxWidgets to 3.2.0
#if defined(__linux__) && defined(__WXGTK3__)
#define wxLinux_gtk3 true
#else
#define wxLinux_gtk3 false
#endif //defined(__linux__) && defined(__WXGTK3__)
*/

namespace Slic3r {
namespace GUI {

static DynamicPrintConfig build_custom_sla_template(const PresetBundle &preset_bundle)
{
    DynamicPrintConfig cfg;
    cfg.apply(SLAFullPrintConfig::defaults());
    cfg.apply(preset_bundle.sla_prints.get_selected_preset().config, true);
    cfg.apply(preset_bundle.sla_materials.get_selected_preset().config, true);
    cfg.apply(preset_bundle.printers.get_selected_preset().config, true);

    cfg.option<ConfigOptionString>("sla_print_settings_id", true)->value = preset_bundle.sla_prints.get_selected_preset_name();
    cfg.option<ConfigOptionString>("sla_material_settings_id", true)->value = preset_bundle.sla_materials.get_selected_preset_name();
    cfg.option<ConfigOptionString>("printer_settings_id", true)->value = preset_bundle.printers.get_selected_preset_name();
    if (auto *pt = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology", true))
        pt->value = ptSLA;
    else
        cfg.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));

    return cfg;
}

static bool is_prusa_vendor_id(const std::string &vendor_id)
{
    return vendor_id == PresetBundle::PRUSA_BUNDLE || vendor_id.find("Prusa") != std::string::npos;
}

static bool is_prusa_vendor(const VendorProfile *vendor)
{
    return vendor != nullptr && (is_prusa_vendor_id(vendor->id) || vendor->name.find("Prusa") != std::string::npos);
}

static bool is_prusa_printer_page(const PagePrinters *page)
{
    return page != nullptr && (is_prusa_vendor_id(page->get_vendor_id()) || page->shortname.Find("Prusa") != wxNOT_FOUND);
}

// Summary label of the base printer profile: the internal "- default SLA -" preset is shown as plain words.
static wxString base_profile_label(const std::string &name)
{
    if (name.empty() || name.rfind("- default", 0) == 0)
        return _L("built-in default settings");
    return from_u8(name);
}

using Config::Snapshot;
using Config::SnapshotDB;

bool Bundle::load(fs::path source_path, BundleLocation location, bool ais_prusa_bundle)
{
    this->preset_bundle = std::make_unique<PresetBundle>();
    this->location = location;
    this->is_prusa_bundle = ais_prusa_bundle;

    std::string path_string = source_path.string();
    // Throw when parsing invalid configuration. Only valid configuration is supposed to be provided over the air.
    auto [config_substitutions, presets_loaded] = preset_bundle->load_configbundle(
        path_string, PresetBundle::LoadConfigBundleAttribute::LoadSystem, ForwardCompatibilitySubstitutionRule::Disable);
    UNUSED(config_substitutions);
    // No substitutions shall be reported when loading a system config bundle, no substitutions are allowed.
    assert(config_substitutions.empty());
    auto first_vendor = preset_bundle->vendors.begin();
    if (first_vendor == preset_bundle->vendors.end()) {
        BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: No vendor information defined, cannot install.") % path_string;
        return false;
    }
    if (presets_loaded == 0) {
        BOOST_LOG_TRIVIAL(error) << boost::format("Vendor bundle: `%1%`: No profile loaded.") % path_string;
        return false;
    } 

    BOOST_LOG_TRIVIAL(trace) << boost::format("Vendor bundle: `%1%`: %2% profiles loaded.") % path_string % presets_loaded;
    this->vendor_profile = &first_vendor->second;
    return true;
}

// Configuration data structures extensions needed for the wizard
Bundle::Bundle(Bundle &&other)
    : preset_bundle(std::move(other.preset_bundle))
    , vendor_profile(other.vendor_profile)
    , location(other.location)
    , is_prusa_bundle(other.is_prusa_bundle)
{
    other.vendor_profile = nullptr;
}

BundleMap BundleMap::load()
{
    BundleMap res;

    const Slic3r::PresetUpdaterWrapper* preset_updater_wrapper = wxGetApp().get_preset_updater_wrapper();
    const auto vendor_dir = (boost::filesystem::path(Slic3r::data_dir()) / "vendor").make_preferred();
    const auto archive_dir = (boost::filesystem::path(Slic3r::data_dir()) / "cache" / "vendor").make_preferred();
    const auto rsrc_vendor_dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();
    const auto cache_dir = boost::filesystem::path(Slic3r::data_dir()) / "cache"; // for Index
    // Load Prusa bundle from the datadir/vendor directory or from datadir/cache/vendor (archive) or from resources/profiles.
    auto prusa_bundle_path = (vendor_dir / PresetBundle::PRUSA_BUNDLE).replace_extension(".ini");
    BundleLocation prusa_bundle_loc = BundleLocation::IN_VENDOR;
    if (! boost::filesystem::exists(prusa_bundle_path)) {
        prusa_bundle_path = (archive_dir / PresetBundle::PRUSA_BUNDLE).replace_extension(".ini");
        prusa_bundle_loc = BundleLocation::IN_ARCHIVE;
    }
    if (!boost::filesystem::exists(prusa_bundle_path)) {
        prusa_bundle_path = (rsrc_vendor_dir / PresetBundle::PRUSA_BUNDLE).replace_extension(".ini");
        prusa_bundle_loc = BundleLocation::IN_RESOURCES;
    }
    // SLA-only resource packs need not contain the legacy FFF vendor bundle.
    if (fs::exists(prusa_bundle_path)) {
        Bundle prusa_bundle;
        if (prusa_bundle.load(std::move(prusa_bundle_path), prusa_bundle_loc, true))
            res.emplace(PresetBundle::PRUSA_BUNDLE, std::move(prusa_bundle)); 
    }

    // Load the other bundles in the datadir/vendor directory
    // and then additionally from datadir/cache/vendor (archive) and resources/profiles.
    // Should we concider case where archive has older profiles than resources (shouldnt happen)? -> YES, it happens during re-configuration when running older PS after newer version
    typedef std::pair<const fs::path&, BundleLocation> DirData;
    std::vector<DirData> dir_list { {vendor_dir, BundleLocation::IN_VENDOR},  {archive_dir, BundleLocation::IN_ARCHIVE},  {rsrc_vendor_dir, BundleLocation::IN_RESOURCES} };
    for ( auto dir : dir_list) {
        if (!fs::exists(dir.first))
            continue;
        for (const auto &dir_entry : boost::filesystem::directory_iterator(dir.first)) {
            if (Slic3r::is_ini_file(dir_entry)) {
                std::string id = dir_entry.path().stem().string();  // stem() = filename() without the trailing ".ini" part

                // Don't load this bundle if we've already loaded it.
                if (res.find(id) != res.end()) { continue; }

                // Fresh index should be in archive_dir, otherwise look for it in cache 
                // Then if not in archive or cache - it could be 3rd party profile that user just copied to vendor folder (both ini and cache)
                
                fs::path idx_path (archive_dir / (id + ".idx"));
                if (!boost::filesystem::exists(idx_path)) {
                    BOOST_LOG_TRIVIAL(info) << format("Missing index %1% when loading bundle %2%. Going to search for it in cache folder.", idx_path.string(), id);
                    idx_path = fs::path(cache_dir / (id + ".idx"));
                }
                if (!boost::filesystem::exists(idx_path)) {
                    BOOST_LOG_TRIVIAL(info) << format("Missing index %1% when loading bundle %2%. Going to search for it in vendor folder. Is it a 3rd party profile?", idx_path.string(), id);
                    idx_path = fs::path(vendor_dir / (id + ".idx"));
                }
                if (!boost::filesystem::exists(idx_path))
                    idx_path = rsrc_vendor_dir / (id + ".idx");
                if (!boost::filesystem::exists(idx_path)) {
                    BOOST_LOG_TRIVIAL(error) << format("Could not load bundle %1% due to missing index %2%.", id, idx_path.string());
                    continue;
                }

                Slic3r::GUI::Config::Index index;
                try {
                    index.load(idx_path);
                }
                catch (const std::exception& /* err */) {
                    BOOST_LOG_TRIVIAL(error) << format("Could not load bundle %1% due to invalid index %2%.", id, idx_path.string());
                    continue;
                }
                const auto recommended_it = index.recommended();
                if (recommended_it == index.end()) {
                    BOOST_LOG_TRIVIAL(error) << format("Could not load bundle %1% due to no recommended version in index %2%.", id, idx_path.string());
                    continue;
                }
                const auto recommended = recommended_it->config_version;
                VendorProfile vp;
                // Check if in selected repo.
                try {
                    vp = VendorProfile::from_ini(dir_entry, false);
                }
                catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << format("Could not load bundle %1% due to corrupted profile file %2%. Message: %3%", id, dir_entry.path().string(), e.what());
                    continue;
                }
                if (vp.repo_id.empty() || !preset_updater_wrapper->is_selected_repository_by_id(vp.repo_id)) {
                    continue;
                }
                // Don't load
                if (vp.config_version > recommended)
                    continue;
                // Load full VP.
                try {
                    vp = VendorProfile::from_ini(dir_entry, true);
                }
                catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << format("Could not load bundle %1% due to corrupted profile file %2%. Message: %3%", id, dir_entry.path().string(), e.what());
                    continue;
                }
                Bundle bundle;
                if (bundle.load(dir_entry.path(), dir.second))
                    res.emplace(std::move(id), std::move(bundle));
            }
        }
    }

    return res;
}

Bundle& BundleMap::prusa_bundle()
{
    auto it = find(PresetBundle::PRUSA_BUNDLE);
    if (it == end()) {
        throw Slic3r::RuntimeError("ConfigWizard: Internal error in BundleMap: PRUSA_BUNDLE not loaded");
    }

    return it->second;
}

const Bundle& BundleMap::prusa_bundle() const
{
    return const_cast<BundleMap*>(this)->prusa_bundle();
}


// Printer model picker GUI control

struct PrinterPickerEvent : public wxEvent
{
    std::string vendor_id;
    std::string model_id;
    std::string variant_name;
    bool enable;

    PrinterPickerEvent(wxEventType eventType, int winid, std::string vendor_id, std::string model_id, std::string variant_name, bool enable)
        : wxEvent(winid, eventType)
        , vendor_id(std::move(vendor_id))
        , model_id(std::move(model_id))
        , variant_name(std::move(variant_name))
        , enable(enable)
    {}

    virtual wxEvent *Clone() const
    {
        return new PrinterPickerEvent(*this);
    }
};

wxDEFINE_EVENT(EVT_PRINTER_PICK, PrinterPickerEvent);

const std::string PrinterPicker::PRINTER_PLACEHOLDER = "printer_placeholder.png";

PrinterPicker::PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig, const ModelFilter &filter)
    : wxPanel(parent)
    , vendor_id(vendor.id)
    , vendor_repo_id(vendor.repo_id)
    , width(0)
{
    wxGetApp().UpdateDarkUI(this);
    const auto &models = vendor.models;

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    const auto font_title = GetFont().MakeBold().Scaled(1.3f);
    const auto font_name = GetFont().MakeBold();
    const auto font_alt_variant = GetFont().Scaled(0.9f);

    // wxGrid appends widgets by rows, but we need to construct them in columns.
    // These vectors are used to hold the elements so that they can be appended in the right order.
    std::vector<wxStaticText*> titles;
    std::vector<wxStaticBitmap*> bitmaps;
    std::vector<wxPanel*> variants_panels;

    int max_row_width = 0;
    int current_row_width = 0;

    bool is_variants = false;

    const fs::path vendor_dir_path = (fs::path(Slic3r::data_dir()) / "vendor").make_preferred();
    const fs::path cache_dir_path = (fs::path(Slic3r::data_dir()) / "cache").make_preferred();
    const fs::path rsrc_dir_path = (fs::path(resources_dir()) / "profiles").make_preferred();

    for (const auto &model : models) {
        if (! filter(model)) { continue; }

        wxBitmap bitmap;
        int bitmap_width = 0;
        auto load_bitmap = [](const wxString& bitmap_file, wxBitmap& bitmap, int& bitmap_width) {
            bitmap.LoadFile(bitmap_file, wxBITMAP_TYPE_PNG);
            bitmap_width = bitmap.GetWidth();
        };
        
        bool found = false;
        for (const fs::path& res : { rsrc_dir_path   / vendor.id / model.thumbnail
                                   , vendor_dir_path / vendor.id / model.thumbnail
                                   , cache_dir_path  / vendor.id / model.thumbnail })
        {
            if (!fs::exists(res))
                continue;
            load_bitmap(GUI::from_u8(res.string()), bitmap, bitmap_width);
            found = true;
            break;
        }
        
        if (!found) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("Can't find bitmap file `%1%` for vendor `%2%`, printer `%3%`, using placeholder icon instead")
                % model.thumbnail
                % vendor.id
                % model.id;
            load_bitmap(GUI::from_u8(Slic3r::var(PRINTER_PLACEHOLDER)), bitmap, bitmap_width);
        }
        
        wxStaticText* title = new wxStaticText(this, wxID_ANY, from_u8(model.name), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
        title->SetFont(font_name);
        const int wrap_width = std::max((int)MODEL_MIN_WRAP, bitmap_width);
        title->Wrap(wrap_width);

        current_row_width += wrap_width;
        if (titles.size() % max_cols == max_cols - 1) {
            max_row_width = std::max(max_row_width, current_row_width);
            current_row_width = 0;
        }

        titles.push_back(title);

        wxStaticBitmap* bitmap_widget = new wxStaticBitmap(this, wxID_ANY, bitmap);
        // Picture of the printer model named by the label next to it; give it a name instead of an anonymous image.
        set_accessible_name(bitmap_widget, from_u8(model.name) + " picture", wxString(), AccessibleRole::Image);
        bitmaps.push_back(bitmap_widget);

        auto *variants_panel = new wxPanel(this);
        wxGetApp().UpdateDarkUI(variants_panel);
        auto *variants_sizer = new wxBoxSizer(wxVERTICAL);
        variants_panel->SetSizer(variants_sizer);
        const auto model_id = model.id;

        for (size_t i = 0; i < model.variants.size(); i++) {
            const auto &variant = model.variants[i];

            const auto label = from_u8(model.name);

            if (i == 1) {
                auto *alt_label = new wxStaticText(variants_panel, wxID_ANY, _L("Alternate variants:"));
                alt_label->SetFont(font_alt_variant);
                variants_sizer->Add(alt_label, 0, wxBOTTOM, 3);
                is_variants = true;
            }

            Checkbox* cbox = new Checkbox(variants_panel, label, model_id, variant.name);
            i == 0 ? cboxes.push_back(cbox) : cboxes_alt.push_back(cbox);

            const bool enabled = appconfig.get_variant(vendor.id, model_id, variant.name);
            cbox->SetValue(enabled);

            variants_sizer->Add(cbox, 0, wxBOTTOM, 3);

            cbox->Bind(wxEVT_CHECKBOX, [this, cbox](wxCommandEvent &event) {
                on_checkbox(cbox, event.IsChecked());
            });
        }

        variants_panels.push_back(variants_panel);
    }

    width = std::max(max_row_width, current_row_width);

    const size_t cols = std::min(max_cols, titles.size());

    auto *printer_grid = new wxFlexGridSizer(cols, 0, 20);
    printer_grid->SetFlexibleDirection(wxVERTICAL | wxHORIZONTAL);

    if (titles.size() > 0) {
        const size_t odd_items = titles.size() % cols;

        for (size_t i = 0; i < titles.size() - odd_items; i += cols) {
            for (size_t j = i; j < i + cols; j++) { printer_grid->Add(bitmaps[j], 0, wxBOTTOM, 20); }
            for (size_t j = i; j < i + cols; j++) { printer_grid->Add(titles[j], 0, wxBOTTOM, 3); }
            for (size_t j = i; j < i + cols; j++) { printer_grid->Add(variants_panels[j]); }

            // Add separator space to multiliners
            if (titles.size() > cols) {
                for (size_t j = i; j < i + cols; j++) { printer_grid->Add(1, 30); }
            }
        }
        if (odd_items > 0) {
            const size_t rem = titles.size() - odd_items;

            for (size_t i = rem; i < titles.size(); i++) { printer_grid->Add(bitmaps[i], 0, wxBOTTOM, 20); }
            for (size_t i = 0; i < cols - odd_items; i++) { printer_grid->AddSpacer(1); }
            for (size_t i = rem; i < titles.size(); i++) { printer_grid->Add(titles[i], 0, wxBOTTOM, 3); }
            for (size_t i = 0; i < cols - odd_items; i++) { printer_grid->AddSpacer(1); }
            for (size_t i = rem; i < titles.size(); i++) { printer_grid->Add(variants_panels[i]); }
        }
    }

    auto *title_sizer = new wxBoxSizer(wxHORIZONTAL);
    if (! title.IsEmpty()) {
        auto *title_widget = new wxStaticText(this, wxID_ANY, title);
        title_widget->SetFont(font_title);
        title_sizer->Add(title_widget);
    }
    title_sizer->AddStretchSpacer();

    if (titles.size() > 1 || is_variants) {
        // It only makes sense to add the All / None buttons if there's multiple printers
        // All Standard button is added when there are more variants for at least one printer
        auto *sel_all_std = new wxButton(this, wxID_ANY, titles.size() > 1 ? _L("All standard") : _L("Standard"));
        auto *sel_all = new wxButton(this, wxID_ANY, _L("All"));
        auto *sel_none = new wxButton(this, wxID_ANY, _L("None"));
        if (is_variants) 
            sel_all_std->Bind(wxEVT_BUTTON, [this](const wxCommandEvent& event) { this->select_all(true, false); });
        sel_all->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &event) { this->select_all(true, true); });
        sel_none->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &event) { this->select_all(false); });
        if (is_variants) 
            title_sizer->Add(sel_all_std, 0, wxRIGHT, BTN_SPACING);
        title_sizer->Add(sel_all, 0, wxRIGHT, BTN_SPACING);
        title_sizer->Add(sel_none);

        wxGetApp().SetWindowVariantForButton(sel_all_std);
        wxGetApp().SetWindowVariantForButton(sel_all);
        wxGetApp().SetWindowVariantForButton(sel_none);

        wxGetApp().UpdateDarkUI(sel_all_std);
        wxGetApp().UpdateDarkUI(sel_all);
        wxGetApp().UpdateDarkUI(sel_none);

        // fill button indexes used later for buttons rescaling
        if (is_variants)
            m_button_indexes = { sel_all_std->GetId(), sel_all->GetId(), sel_none->GetId() };
        else {
            sel_all_std->Destroy();
            m_button_indexes = { sel_all->GetId(), sel_none->GetId() };
        }
    }

    sizer->Add(title_sizer, 0, wxEXPAND | wxBOTTOM, BTN_SPACING);
    sizer->Add(printer_grid);

    SetSizer(sizer);
}

PrinterPicker::PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig)
    : PrinterPicker(parent, vendor, std::move(title), max_cols, appconfig, [](const VendorProfile::PrinterModel&) { return true; })
{}

void PrinterPicker::select_all(bool select, bool alternates)
{
    for (const auto &cb : cboxes) {
        if (cb->GetValue() != select) {
            cb->SetValue(select);
            on_checkbox(cb, select);
        }
    }

    if (! select) { alternates = false; }

    for (const auto &cb : cboxes_alt) {
        if (cb->GetValue() != alternates) {
            cb->SetValue(alternates);
            on_checkbox(cb, alternates);
        }
    }
}

void PrinterPicker::select_one(size_t i, bool select)
{
    if (i < cboxes.size() && cboxes[i]->GetValue() != select) {
        cboxes[i]->SetValue(select);
        on_checkbox(cboxes[i], select);
    }
}

bool PrinterPicker::untick(const std::string &model, const std::string &variant)
{
    for (auto *list : { &cboxes, &cboxes_alt })
        for (Checkbox *cb : *list)
            if (cb->GetValue() && cb->model == model && cb->variant == variant) {
                cb->SetValue(false);
                on_checkbox(cb, false);
                return true;
            }
    return false;
}

bool PrinterPicker::any_selected() const
{
    for (const auto &cb : cboxes) {
        if (cb->GetValue()) { return true; }
    }

    for (const auto &cb : cboxes_alt) {
        if (cb->GetValue()) { return true; }
    }

    return false;
}

std::set<std::string> PrinterPicker::get_selected_models() const 
{
    std::set<std::string> ret_set;

    for (const auto& cb : cboxes)
        if (cb->GetValue())
            ret_set.emplace(cb->model);

    for (const auto& cb : cboxes_alt)
        if (cb->GetValue())
            ret_set.emplace(cb->model);

    return ret_set;
}

void PrinterPicker::on_checkbox(const Checkbox *cbox, bool checked)
{
    PrinterPickerEvent evt(EVT_PRINTER_PICK, GetId(), vendor_id, cbox->model, cbox->variant, checked);
    AddPendingEvent(evt);
}


// Wizard page base

static wxColour wizard_accent_colour() { return wxColour(237, 107, 33); }
static wxColour wizard_error_colour() { return GUI_App::dark_mode() ? wxColour(255, 120, 110) : wxColour(190, 30, 30); }
static wxColour wizard_muted_colour()
{
    const wxColour fg = wxGetApp().get_label_clr_default();
    const wxColour bg = wxGetApp().get_window_default_clr();
    return wxColour((fg.Red() * 62 + bg.Red() * 38) / 100, (fg.Green() * 62 + bg.Green() * 38) / 100, (fg.Blue() * 62 + bg.Blue() * 38) / 100);
}

ConfigWizardPage::ConfigWizardPage(ConfigWizard *parent, wxString title, wxString shortname, unsigned indent)
    : wxPanel(parent->p->hscroll)
    , parent(parent)
    , shortname(std::move(shortname))
    , indent(indent)
{
    wxGetApp().UpdateDarkUI(this);
    const int em = em_unit(this);

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    m_step_label = new wxStaticText(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_step_label->SetForegroundColour(wizard_muted_colour());
    m_step_label->SetFont(GetFont().Scaled(0.9f));
    sizer->Add(m_step_label, 0, wxALIGN_LEFT | wxBOTTOM, em / 4);

    auto *text = new wxStaticText(this, wxID_ANY, std::move(title), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    const auto font = GetFont().MakeBold().Scaled(1.5);
    text->SetFont(font);
    sizer->Add(text, 0, wxALIGN_LEFT, 0);
    sizer->AddSpacer(em);

    // Inline validation message: shown right below the heading so it is never scrolled out of view.
    m_error_label = new wxStaticText(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_error_label->SetForegroundColour(wizard_error_colour());
    m_error_label->SetFont(GetFont().MakeBold());
    m_error_label->Hide();
    sizer->Add(m_error_label, 0, wxEXPAND | wxBOTTOM, em / 2);

    content = new wxBoxSizer(wxVERTICAL);
    sizer->Add(content, 1, wxEXPAND);

    SetSizer(sizer);

    this->Hide();

    Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
        this->Layout();
        event.Skip();
    });
}

ConfigWizardPage::~ConfigWizardPage() {}

int ConfigWizardPage::page_em() const { return em_unit(const_cast<ConfigWizardPage*>(this)); }
int ConfigWizardPage::wrap_width() const { return std::min(52 * page_em(), std::max(24 * page_em(), wizard_p()->content_width())); }

int ConfigWizardPage::fit(int em_count) const
{
    const int em = page_em();
    const int width = wizard_p()->content_width();
    return 60 * em <= width ? em_count * em : em_count * std::max(24 * em, width) / 60;
}

void ConfigWizardPage::set_step_info(size_t step, size_t total)
{
    if (step == 0 || total == 0) {
        m_step_label->SetLabel(wxString());
        return;
    }
    m_step_label->SetLabel(optional_step
        ? format_wxstr(_L("Step %1% of %2% (optional)"), step, total)
        : format_wxstr(_L("Step %1% of %2%"), step, total));
}

void ConfigWizardPage::show_error(const wxString &message)
{
    const bool show = !message.IsEmpty();
    if (m_error_label->GetLabel() == message && m_error_label->IsShown() == show)
        return;
    m_error_label->SetLabel(message);
    if (show) {
        m_error_label->Show();
        m_error_label->Wrap(wrap_width());
        set_accessible_name(m_error_label, message, wxString(), AccessibleRole::Label);
    } else
        m_error_label->Hide();
    Layout();
    if (IsShown())
        wizard_p()->hscroll->Layout();
}

bool ConfigWizardPage::has_error() const { return m_error_label->IsShown(); }

wxStaticText* ConfigWizardPage::append_text(wxString text)
{
    auto *widget = new wxStaticText(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    widget->Wrap(wrap_width());
    widget->SetMinSize(wxSize(wrap_width(), -1));
    append(widget);
    return widget;
}

wxStaticText* ConfigWizardPage::append_help(const wxString &text)
{
    auto *widget = new wxStaticText(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    widget->SetForegroundColour(wizard_muted_colour());
    widget->Wrap(wrap_width());
    widget->SetMinSize(wxSize(wrap_width(), -1));
    content->Add(widget, 0, wxEXPAND | wxBOTTOM, page_em() / 2);
    return widget;
}

wxStaticText* ConfigWizardPage::append_heading(const wxString &text)
{
    auto *widget = new wxStaticText(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    widget->SetFont(GetFont().MakeBold().Scaled(1.15f));
    content->Add(widget, 0, wxEXPAND | wxTOP | wxBOTTOM, page_em() / 2);
    return widget;
}

void ConfigWizardPage::append_spacer(int space)
{
    content->AddSpacer(space);
}

// Wizard pages

// ============================================================================
// Step 1: "Your printer" - three choice cards
// ============================================================================

static wxColour wizard_blend(const wxColour &a, const wxColour &b, int pct_b)
{
    return wxColour((a.Red() * (100 - pct_b) + b.Red() * pct_b) / 100,
                    (a.Green() * (100 - pct_b) + b.Green() * pct_b) / 100,
                    (a.Blue() * (100 - pct_b) + b.Blue() * pct_b) / 100);
}

PathChoice::PathChoice(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
{
    wxGetApp().UpdateDarkUI(this);
    m_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(m_sizer);
    Bind(wxEVT_PAINT, &PathChoice::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
        for (size_t i = 0; i < m_cards.size(); ++i)
            if (wxRect(m_cards[i].row->GetPosition(), m_cards[i].row->GetSize()).Contains(event.GetPosition()))
                pick(i);
        event.Skip();
    });
}

size_t PathChoice::add_card(const wxString &title, const wxString &description, const std::string &icon_name)
{
    const int em = em_unit(this);
    Card card;
    card.text = description;
    // wxRB_GROUP on the first card only: the radio buttons are consecutive siblings, i.e. one native group.
    card.radio = new wxRadioButton(this, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, m_cards.empty() ? wxRB_GROUP : 0);
    card.radio->SetFont(GetFont().MakeBold().Scaled(1.2f));
    set_accessible_name(card.radio, title, description);
    card.icon = new wxStaticBitmap(this, wxID_ANY, *get_bmp_bundle(icon_name, 5 * em));
    set_accessible_name(card.icon, title + " icon", wxString(), AccessibleRole::Image);
    card.description = new wxStaticText(this, wxID_ANY, description, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    card.description->SetForegroundColour(wizard_muted_colour());
    if (m_wrap == 0)
        m_wrap = 38 * em;
    card.description->Wrap(m_wrap);
    card.description->SetMinSize(wxSize(m_wrap, -1));

    auto *column = new wxBoxSizer(wxVERTICAL);
    // Tighter cards when the page is narrow (large text): the three cards must stay on the first screen.
    const int pad = m_wrap < 38 * em ? em / 2 : em;
    column->Add(card.radio, 0, wxTOP, pad);
    column->Add(card.description, 0, wxLEFT | wxTOP | wxBOTTOM, pad + pad / 2);
    card.row = new wxBoxSizer(wxHORIZONTAL);
    card.row->Add(card.icon, 0, wxALIGN_CENTER_VERTICAL | wxALL, em);
    card.row->Add(column, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_sizer->Add(card.row, 0, wxEXPAND | wxBOTTOM, pad);

    const size_t index = m_cards.size();
    card.radio->Bind(wxEVT_RADIOBUTTON, [this, index](wxCommandEvent &) { pick(index); });
    for (wxWindow *w : std::initializer_list<wxWindow*>{ card.icon, card.description })
        w->Bind(wxEVT_LEFT_DOWN, [this, index](wxMouseEvent &) { pick(index); });
    m_cards.push_back(card);
    return index;
}

void PathChoice::pick(size_t index)
{
    if (index >= m_cards.size() || !m_cards[index].available)
        return;
    set_selected(index);
    if (on_pick)
        on_pick(index);
}

void PathChoice::set_selected(size_t index)
{
    // An out-of-range index means "no card chosen" (after "Add another printer").
    m_selected = index < m_cards.size() ? index : size_t(-1);
    if (m_selected == size_t(-1)) {
        // A native radio group cannot be unchecked; an invisible member of the group takes the selection instead.
        if (m_none == nullptr) {
            m_none = new wxRadioButton(this, wxID_ANY, wxString(), wxDefaultPosition, wxSize(1, 1));
            m_none->Hide();
        }
        m_none->SetValue(true);
    } else
        m_cards[m_selected].radio->SetValue(true);
    Refresh();
}

void PathChoice::set_available(size_t index, bool available, const wxString &description_when_unavailable)
{
    Card &card = m_cards[index];
    card.available = available;
    card.radio->Enable(available);
    card.icon->Enable(available);
    card.description->SetLabel(available || description_when_unavailable.IsEmpty() ? card.text : description_when_unavailable);
    card.description->Wrap(m_wrap);
    Layout();
    Refresh();
}

void PathChoice::set_wrap_width(int pixels)
{
    m_wrap = pixels;
    for (Card &card : m_cards) {
        card.description->Wrap(m_wrap);
        card.description->SetMinSize(wxSize(m_wrap, -1));
    }
    Layout();
}

void PathChoice::on_paint(wxPaintEvent &)
{
    wxPaintDC dc(this);
    wxGCDC gdc(dc);
    const int em = em_unit(this);
    for (size_t i = 0; i < m_cards.size(); ++i) {
        wxRect rect(m_cards[i].row->GetPosition(), m_cards[i].row->GetSize());
        rect.Deflate(1);
        const bool selected = i == m_selected;
        gdc.SetBrush(selected ? wxBrush(wizard_blend(GetBackgroundColour(), wizard_accent_colour(), 14)) : *wxTRANSPARENT_BRUSH);
        gdc.SetPen(selected ? wxPen(wizard_accent_colour(), 2)
                            : wxPen(wizard_blend(GetBackgroundColour(), wxGetApp().get_label_clr_default(), 30), 1));
        gdc.DrawRoundedRectangle(rect, em / 2.);
    }
}

PagePath::PagePath(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Your printer"), _L("Your printer"))
{
    const int em = page_em();
    m_welcome = append_help(format_wxstr(_L("Welcome to %s. It takes only a few steps to get your printer ready."), SLIC3R_APP_NAME));
    m_welcome->Hide();
    append_help(_L("Where does your printer come from? Pick one; the following steps depend on your choice. "
                   "Nothing is installed or changed until you click Finish."));
    append_spacer(em / 2);

    m_choice = new PathChoice(this);
    // icon + radio indent + margins take about 11 em next to the text
    m_choice->set_wrap_width(std::min(38 * em, std::max(14 * em, wrap_width() - 11 * em)));
    m_choice->add_card(_L("A printer from a vendor"),
        _L("Ready-made profiles for supported printer models. The default materials are chosen for you."), "sla_printer");
    m_choice->add_card(_L("A profile from the library"),
        _L("Community printer profiles and profile files shipped with the application. Preview a profile before you pick it."), "import_config");
    m_choice->add_card(_L("Define my own printer"),
        _L("Enter the display size, resolution and build height of a printer that is not listed."), "wrench");
    m_queue = new wxPanel(this);
    wxGetApp().UpdateDarkUI(m_queue);
    m_queue->SetSizer(new wxWrapSizer(wxHORIZONTAL));
    m_queue->Hide();
    append(m_queue, 0, wxEXPAND | wxBOTTOM, em / 2);
    m_choice->on_pick = [this](size_t index) {
        m_path = index == 0 ? WizardPath::Vendor : index == 1 ? WizardPath::Library : WizardPath::Custom;
        wizard_p()->on_path_changed();
    };
    append(m_choice, 0, wxEXPAND, 0);
    append_help(_L("Just one printer? Ignore the rest. Need more? Finish this one, then use \"Add another printer\" on the Summary step."));
    m_choice->set_selected(0);
}

void PagePath::set_path(WizardPath path, bool notify)
{
    m_path = path;
    m_choice->set_selected(path == WizardPath::Vendor ? 0 : path == WizardPath::Library ? 1 : path == WizardPath::Custom ? 2 : size_t(-1));
    if (notify)
        wizard_p()->on_path_changed();
}

void PagePath::restore_default()
{
    set_path(m_vendors_available ? WizardPath::Vendor : m_library_available ? WizardPath::Library : WizardPath::Custom, false);
}

void PagePath::refresh_queue()
{
    auto *p = wizard_p();
    const std::vector<WizardQueueItem> items = p->queue_items();
    wxSizer *sizer = m_queue->GetSizer();
    sizer->Clear(true);
    m_queue->Show(!items.empty());
    if (!items.empty()) {
        const int em = page_em();
        auto *label = new wxStaticText(m_queue, wxID_ANY, _L("Queued:"));
        label->SetFont(GetFont().MakeBold());
        sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
        for (const auto &item : items) {
            auto *chip = new wxButton(m_queue, wxID_ANY, item.label + wxString::FromUTF8("  \xC3\x97"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            wxGetApp().UpdateDarkUI(chip);
            const wxString hint = format_wxstr(_L("Remove queued printer \"%1%\""), item.label);
            chip->SetToolTip(hint);
            set_accessible_name(chip, hint);
            chip->Bind(wxEVT_BUTTON, [this, remove = item.remove](wxCommandEvent &) { CallAfter(remove); });
            sizer->Add(chip, 0, wxRIGHT | wxBOTTOM, em / 2);
        }
    }
    m_queue->Layout();
    Layout();
    p->hscroll->FitInside();
}

void PagePath::refresh_cards(bool vendors_available, bool library_available)
{
    m_vendors_available = vendors_available;
    m_library_available = library_available;
    m_choice->set_available(0, vendors_available, _L("No vendor printer profiles are available in this installation."));
    m_choice->set_available(1, library_available, _L("No library profiles were found in this installation."));
    if (!m_initialised) {
        m_initialised = true;
        // First usable card; a re-run keeps vendor printers (shown as installed) in view.
        set_path(vendors_available ? WizardPath::Vendor : library_available ? WizardPath::Library : WizardPath::Custom, false);
    } else if ((m_path == WizardPath::Vendor && !vendors_available) || (m_path == WizardPath::Library && !library_available))
        set_path(library_available ? WizardPath::Library : WizardPath::Custom, false);
    Layout();
}

void PagePath::set_run_reason(ConfigWizard::RunReason run_reason)
{
    m_welcome->Show(run_reason == ConfigWizard::RR_DATA_EMPTY);
}

PagePrinters::PagePrinters(ConfigWizard *parent,
    wxString title,
    wxString shortname,
    const VendorProfile &vendor,
    unsigned indent,
    Technology technology)
    : ConfigWizardPage(parent, std::move(title), std::move(shortname), indent)
    , technology(technology)
    , install(false)   // only used for 3rd party vendors
    , vendor_profile(&vendor)
{
    enum {
        COL_SIZE = 200,
    };

    AppConfig *appconfig = &this->wizard_p()->appconfig_new;

    append_help(_L("Tick every printer model you own. Installed models are already ticked. The default materials of each "
                   "model are selected for you in the next step."));

    const auto families = vendor.families();
    for (const auto &family : families) {
        const auto filter = [&](const VendorProfile::PrinterModel &model) {
            return (model.technology == ptSLA && technology & T_SLA)
                && model.family == family;
        };

        if (std::find_if(vendor.models.begin(), vendor.models.end(), filter) == vendor.models.end()) {
            continue;
        }

        const auto picker_title = family.empty() ? wxString() : format_wxstr(_L("%s Family"), family);
        // Fewer model columns when the page area is narrow (large text / small screens).
        const size_t max_cols = size_t(std::clamp(wizard_p()->content_width() / 180, 1, int(MAX_COLS)));
        auto *picker = new PrinterPicker(this, vendor, picker_title, max_cols, *appconfig, filter);

        picker->Bind(EVT_PRINTER_PICK, [this, appconfig](const PrinterPickerEvent &evt) {
            appconfig->set_variant(evt.vendor_id, evt.model_id, evt.variant_name, evt.enable);
            wizard_p()->on_printer_pick(this, evt);
        });

        append(new StaticLine(this));

        append(picker);
        printer_pickers.push_back(picker);
        has_printers = true;
    }

}

void PagePrinters::select_all(bool select, bool alternates)
{
    for (auto picker : printer_pickers) {
        picker->select_all(select, alternates);
    }
}

void PagePrinters::unselect_all_presets()
{
    assert(!printer_pickers.empty());
    const std::string vendor_id = printer_pickers[0]->vendor_id;

    PresetBundle* preset_bundle{ nullptr };
    for (const auto& [bundle_name, bundle] : wizard_p()->bundles) {
        if (bundle_name == vendor_id) {
            preset_bundle = bundle.preset_bundle.get();
            break; 
        }
    }

    if (preset_bundle) {
        auto unselect = [preset_bundle](const std::string& vendor_id, const std::string& model, const std::string& variant) {
            for (auto& preset : preset_bundle->printers) {
                if (preset.config.opt_string("printer_model") == model
                    && preset.config.opt_string("printer_variant") == variant) {
                    preset.is_visible = false;
                }
            }
        };

        // unselect presets in preset bundle, if related model and variant was checked in Picker
        for (auto picker : printer_pickers) {
            for (const auto& cb : picker->cboxes) {
                if (cb->GetValue())
                    unselect(picker->vendor_id, cb->model, cb->variant);
            }

            for (const auto& cb : picker->cboxes_alt) {
                if (cb->GetValue())
                    unselect(picker->vendor_id, cb->model, cb->variant);
            }
        }
    }

    // remove vendor from appconfig_new
    AppConfig* appconfig = &wizard_p()->appconfig_new;

    AppConfig::VendorMap new_vendors = appconfig->vendors();
    if (new_vendors.find(vendor_id) != new_vendors.end()) {
        new_vendors.erase(vendor_id);
        appconfig->set_vendors(new_vendors);
    }
}

int PagePrinters::get_width() const
{
    return std::accumulate(printer_pickers.begin(), printer_pickers.end(), 0,
        [](int acc, const PrinterPicker *picker) { return std::max(acc, picker->get_width()); });
}

bool PagePrinters::any_selected() const
{
    for (const auto *picker : printer_pickers) {
        if (picker->any_selected()) { return true; }
    }

    return false;
}

std::set<std::string> PagePrinters::get_selected_models()
{
    std::set<std::string> ret_set;

    for (const auto *picker : printer_pickers)
    {
        std::set<std::string> tmp_models = picker->get_selected_models();
        ret_set.insert(tmp_models.begin(), tmp_models.end());
    }

    return ret_set;
}

std::vector<std::string> PagePrinters::selected_model_names()
{
    std::vector<std::string> names;
    auto add = [&names](const PrinterPicker::Checkbox *cb, bool alternate) {
        if (cb->GetValue())
            names.push_back(into_u8(cb->GetLabel()) + (alternate ? " (" + cb->variant + ")" : std::string()));
    };
    for (const auto *picker : printer_pickers) {
        for (const auto *cb : picker->cboxes) add(cb, false);
        for (const auto *cb : picker->cboxes_alt) add(cb, true);
    }
    return names;
}

std::vector<PagePrinters::TickedModel> PagePrinters::ticked_models() const
{
    std::vector<TickedModel> models;
    for (const auto *picker : printer_pickers)
        for (const auto *list : { &picker->cboxes, &picker->cboxes_alt })
            for (const auto *cb : *list)
                if (cb->GetValue())
                    models.push_back({ into_u8(cb->GetLabel()) + (list == &picker->cboxes_alt ? " (" + cb->variant + ")" : std::string()),
                                       picker->vendor_id, cb->model, cb->variant });
    return models;
}

bool PagePrinters::untick_model(const TickedModel &model)
{
    for (auto *picker : printer_pickers)
        if (picker->vendor_id == model.vendor_id && picker->untick(model.model_id, model.variant))
            return true;
    return false;
}

wxString PagePrinters::validate()
{
    // Only the last vendor step insists on a choice, so that a user may skip a vendor page on the way.
    auto *p = wizard_p();
    if (p->path() == WizardPath::Vendor && !p->pages_msla.empty() && p->pages_msla.back() == this &&
        !p->check_sla_selected() && !p->existing_printers_kept() && p->queue.empty())
        return _L("Tick at least one printer model to continue.");
    return wxString();
}

void PagePrinters::set_run_reason(ConfigWizard::RunReason run_reason)
{
    if (is_primary_printer_page
        && (run_reason == ConfigWizard::RR_DATA_EMPTY || run_reason == ConfigWizard::RR_DATA_LEGACY)
        && printer_pickers.size() > 0 
        && printer_pickers[0]->vendor_id == PresetBundle::PRUSA_BUNDLE) {
        printer_pickers[0]->select_one(0, true);
    }
}


const std::string PageMaterials::EMPTY;

PageMaterials::PageMaterials(ConfigWizard *parent, Materials *materials, wxString title, wxString shortname, wxString list1name)
    : ConfigWizardPage(parent, std::move(title), std::move(shortname))
    , materials(materials)
	, list_printer(new  StringList(this, wxLB_MULTIPLE))
    , list_type(new StringList(this))
    , list_vendor(new StringList(this))
    , list_profile(new PresetList(this))
{
    append_help(_L("Tick the resins you use. The defaults for your printer are already ticked, so you can simply continue."));

    const int em = parent->em_unit();
    const int list_h = 22*em;


	list_printer->SetMinSize(wxSize(fit(17), list_h));
    list_type->SetMinSize(wxSize(fit(9), list_h));
    list_vendor->SetMinSize(wxSize(fit(10), list_h));
    list_profile->SetMinSize(wxSize(fit(24), list_h));

#ifdef __APPLE__
    for (wxWindow* win : std::initializer_list<wxWindow*>{ list_printer, list_type, list_vendor, list_profile })
        win->SetBackgroundColour(wxGetApp().get_window_default_clr());
#endif


    grid = new wxFlexGridSizer(4, em/2, em);
    grid->AddGrowableCol(3, 1);
    grid->AddGrowableRow(1, 1);

	grid->Add(new wxStaticText(this, wxID_ANY, _L("Printer:")));
    grid->Add(new wxStaticText(this, wxID_ANY, list1name));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Vendor:")));
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Profile:")));

	grid->Add(list_printer, 0, wxEXPAND);
    grid->Add(list_type, 0, wxEXPAND);
    grid->Add(list_vendor, 0, wxEXPAND);
    grid->Add(list_profile, 1, wxEXPAND);

    auto *btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *sel_all = new wxButton(this, wxID_ANY, _L("All"));
    auto *sel_none = new wxButton(this, wxID_ANY, _L("None"));
    btn_sizer->Add(sel_all, 0, wxRIGHT, em / 2);
    btn_sizer->Add(sel_none);

    wxGetApp().UpdateDarkUI(list_printer);
    wxGetApp().UpdateDarkUI(list_type);
    wxGetApp().UpdateDarkUI(list_vendor);
    wxGetApp().UpdateDarkUI(sel_all);
    wxGetApp().UpdateDarkUI(sel_none);

    wxGetApp().SetWindowVariantForButton(sel_all);
    wxGetApp().SetWindowVariantForButton(sel_none);

    grid->Add(new wxBoxSizer(wxHORIZONTAL));
    grid->Add(new wxBoxSizer(wxHORIZONTAL));
    grid->Add(new wxBoxSizer(wxHORIZONTAL));
    grid->Add(btn_sizer, 0, wxALIGN_RIGHT);

    append(grid, 1, wxEXPAND);

    append_spacer(VERTICAL_SPACING);

    html_window = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition,
        wxSize(fit(60), 7 * em), wxHW_SCROLLBAR_AUTO);
    html_window->Bind(wxEVT_HTML_LINK_CLICKED, [](wxHtmlLinkEvent& event) {
        wxGetApp().open_browser_with_warning_dialog(event.GetLinkInfo().GetHref());
        event.Skip(false);
        });
    append(html_window, 0, wxEXPAND);

	list_printer->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& evt) {
		update_lists(list_type->GetSelection(), list_vendor->GetSelection(), evt.GetInt());
		});
    list_type->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        update_lists(list_type->GetSelection(), list_vendor->GetSelection());
    });
    list_vendor->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        update_lists(list_type->GetSelection(), list_vendor->GetSelection());
    });

    list_profile->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent &evt) { select_material(evt.GetInt()); });
    list_profile->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& evt) { on_material_highlighted(evt.GetInt()); });

    sel_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { select_all(true); });
    sel_none->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { select_all(false); });
    /*
    Bind(wxEVT_PAINT, [this](wxPaintEvent& evt) {on_paint();});

    list_profile->Bind(wxEVT_MOTION, [this](wxMouseEvent& evt) { on_mouse_move_on_profiles(evt); });
    list_profile->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& evt) { on_mouse_enter_profiles(evt); });
    list_profile->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) { on_mouse_leave_profiles(evt); });
    */
    reload_presets();
    set_compatible_printers_html_window(std::vector<std::string>(), false);
}

void PageMaterials::check_and_update_presets(bool force_reload_presets /*= false*/)
{
    if (presets_loaded)
        return;
    wizard_p()->update_materials(materials->technology);
//    if (force_reload_presets)
        reload_presets();
}

void PageMaterials::on_paint()
{
}
void PageMaterials::on_mouse_move_on_profiles(wxMouseEvent& evt)
{
    const wxClientDC dc(list_profile);
    const wxPoint pos = evt.GetLogicalPosition(dc);
    int item = list_profile->HitTest(pos);
    on_material_hovered(item);
}
void PageMaterials::on_mouse_enter_profiles(wxMouseEvent& evt)
{}
void PageMaterials::on_mouse_leave_profiles(wxMouseEvent& evt)
{
    on_material_hovered(-1);
}
void PageMaterials::reload_presets()
{
    clear();

	list_printer->append(_L("(All)"), &EMPTY);

    //list_printer->SetLabelMarkup("<b>bald</b>");
	for (const Preset* printer : materials->printers) {
		list_printer->append(printer->name, &printer->name);
	}
    sort_list_data(list_printer, true, false);
    if (list_printer->GetCount() > 0) {
        list_printer->SetSelection(0);
        sel_printers_prev.Clear();
        sel_type_prev = wxNOT_FOUND;
        sel_vendor_prev = wxNOT_FOUND;
        update_lists(0, 0, 0);
    }

    presets_loaded = true;
}

void PageMaterials::set_compatible_printers_html_window(const std::vector<std::string>& printer_names, bool all_printers)
{
    const auto text_clr = wxGetApp().get_label_clr_default();
    const auto text_clr_str = encode_color(ColorRGB(text_clr.Red(), text_clr.Green(), text_clr.Blue()));
    const auto bgr_clr_str = wxGetApp().get_html_bg_color(parent);
    wxString text;
    {
        bool has_medical = false;
        for (const Preset *printer : materials->printers) {
            if (printer->vendor && printer->vendor->id == "PrusaProMedical") {
                has_medical = true;
            }
        }
        // TRN PrusaSlicer-Medical ConfigWizard: Materials"
        wxString zero_line = _L("The list of validated workflows for Medical One can be found in this <a href=\"prusa.io/m1-validation\">article</a>. Profiles for other materials are not verified by the material manufacturer and therefore may not correspond to the current version of the material.");
        if (!has_medical) {
            zero_line.Clear();
        }
                wxString first_line = format_wxstr(_L("%1% marked with <b>*</b> are <b>not</b> compatible with some installed printers."), _L("SLA materials"));
        if (all_printers) {
                        wxString second_line = format_wxstr(_L("All installed printers are compatible with the selected %1%."), _L("SLA material"));
            text = wxString::Format(
                "<html>"
                "<style>"
                "table{border-spacing: 1px;}"
                "</style>"
                "<body bgcolor= %s>"
                "<font color=%s>"
                "<font size=\"3\">"
                "%s<br /><br />%s<br /><br />%s"
                "</font>"
                "</body>"
                "</html>"
                , bgr_clr_str
                , text_clr_str
                , zero_line
                , first_line
                , second_line
            );
        }
        else {
            wxString second_line;
            if (!printer_names.empty())
                second_line = _L("Only the following installed printers are compatible with the selected SLA materials") + ":";
            text = wxString::Format(
                "<html>"
                "<style>"
                "table{border-spacing: 1px;}"
                "</style>"
                "<body bgcolor= %s>"
                "<font color=%s>"
                "<font size=\"3\">"
                "%s<br /><br />%s<br /><br />%s"
                "<table>"
                "<tr>"
                , bgr_clr_str
                , text_clr_str
                , zero_line
                , first_line
                , second_line
            );
            for (size_t i = 0; i < printer_names.size(); ++i)
            {
                text += wxString::Format("<td>%s</td>", boost::nowide::widen(printer_names[i]));
                if (i % 3 == 2) {
                    text += wxString::Format(
                        "</tr>"
                        "<tr>");
                }
            }
            text += wxString::Format(
                "</tr>"
                "</table>"
                "</font>"
                "</body>"
                "</html>"
            );
        }
    }
   
    wxFont font = wxGetApp().normal_font();// get_default_font_for_dpi(this, get_dpi_for_window(this));
    const int fs = font.GetPointSize();
    int size[] = { fs,fs,fs,fs,fs,fs,fs };
    html_window->SetFonts(font.GetFaceName(), font.GetFaceName(), size);
    html_window->SetPage(text);
}

void PageMaterials::clear_compatible_printers_label()
{
    set_compatible_printers_html_window(std::vector<std::string>(), false);
}

void PageMaterials::on_material_hovered(int sel_material)
{

}

void PageMaterials::on_material_highlighted(int sel_material)
{
    if (sel_material == last_hovered_item)
        return;
    if (sel_material == -1) {
        clear_compatible_printers_label();
        return;
    }
    last_hovered_item = sel_material;
    std::vector<std::string> tabs;
    tabs.push_back(std::string());
    tabs.push_back(std::string());
    tabs.push_back(std::string());
    //selected material string
    std::string material_name = list_profile->get_data(sel_material);
    // get material preset
    const std::vector<const Preset*> matching_materials = materials->get_presets_by_alias(material_name);
    if (matching_materials.empty())
    {
        clear_compatible_printers_label();
        return;
    }
    //find matching printers
    std::vector<std::string> names;
    for (const Preset* printer : materials->printers) {
        for (const Preset* material : matching_materials) {
            if (is_compatible_with_printer(PresetWithVendorProfile(*material, material->vendor), PresetWithVendorProfile(*printer, printer->vendor))) {
                names.push_back(printer->name);
                break;
            }
        }
    }
    set_compatible_printers_html_window(names, names.size() == materials->printers.size());
}

void PageMaterials::update_lists(int sel_type, int sel_vendor, int last_selected_printer/* = -1*/)
{
	wxWindowUpdateLocker freeze_guard(this);
	(void)freeze_guard;

	wxArrayInt sel_printers;
	int sel_printers_count = list_printer->GetSelections(sel_printers);

    // Does our wxWidgets version support operator== for wxArrayInt ?
    // https://github.com/prusa3d/PrusaSlicer/issues/5152#issuecomment-787208614
#if wxCHECK_VERSION(3, 1, 1)
    if (sel_printers != sel_printers_prev) {
#else
    auto are_equal = [](const wxArrayInt& arr_first, const wxArrayInt& arr_second) {
        if (arr_first.GetCount() != arr_second.GetCount())
            return false;
        for (size_t i = 0; i < arr_first.GetCount(); i++)
            if (arr_first[i] != arr_second[i])
                return false;
        return true;
    };
    if (!are_equal(sel_printers, sel_printers_prev)) {
#endif
        // Refresh type list
		list_type->Clear();
		list_type->append(_L("(All)"), &EMPTY);
        std::vector<std::string> appended_types;
		if (sel_printers_count > 1) {
            // If all is selected with other printers
            // unselect "all" or all printers depending on last value
            if (sel_printers[0] == 0 && sel_printers_count > 1) {
                if (last_selected_printer == 0) {
                    list_printer->SetSelection(wxNOT_FOUND);
                    list_printer->SetSelection(0);
                } else {
                    list_printer->SetSelection(0, false);
                    sel_printers_count = list_printer->GetSelections(sel_printers);
                }
            }
        }
        if (sel_printers_count > 0 && sel_printers[0] != 0) {
            for (int i = 0; i < sel_printers_count; i++) {
                const std::string& printer_name = list_printer->get_data(sel_printers[i]);
                const Preset* printer = nullptr;
                for (const Preset* it : materials->printers) {
                    if (it->name == printer_name) {
                        printer = it;
                        break;
                    }
                }
                materials->filter_presets(printer, printer_name, EMPTY, EMPTY, [this, &appended_types](const Preset* p) {
                    const std::string& type = this->materials->get_type(p);
                    if (std::find(appended_types.begin(), appended_types.end(), type) == appended_types.end()) {
                        list_type->append(type, &type);
                        appended_types.emplace_back(type);
                    }
                    });
            }
        }
        else if (sel_printers_count > 0 && last_selected_printer == 0) {
            //clear selection except "ALL"
            list_printer->SetSelection(wxNOT_FOUND);
            list_printer->SetSelection(0);
            sel_printers_count = list_printer->GetSelections(sel_printers);

            materials->filter_presets(nullptr, EMPTY, EMPTY, EMPTY, [this, &appended_types](const Preset* p) {
                const std::string& type = this->materials->get_type(p);
                if (std::find(appended_types.begin(), appended_types.end(), type) == appended_types.end()) {
                    list_type->append(type, &type);
                    appended_types.emplace_back(type);
                }
                });
        }
        sort_list_data(list_type, true, true);

		sel_printers_prev = sel_printers;
		sel_type = 0;
		sel_type_prev = wxNOT_FOUND;
		list_type->SetSelection(sel_type);
		list_profile->Clear();
	}
	
	if (sel_type != sel_type_prev) {
		// Refresh vendor list

		// XXX: The vendor list is created with quadratic complexity here,
		// but the number of vendors is going to be very small this shouldn't be a problem.

		list_vendor->Clear();
		list_vendor->append(_L("(All)"), &EMPTY);
        std::vector<std::string> appended_vendors;
		if (sel_printers_count != 0 && sel_type != wxNOT_FOUND) {
			const std::string& type = list_type->get_data(sel_type);
			// find printer preset
            for (int i = 0; i < sel_printers_count; i++) {
				const std::string& printer_name = list_printer->get_data(sel_printers[i]);
				const Preset* printer = nullptr;
				for (const Preset* it : materials->printers) {
					if (it->name == printer_name) {
						printer = it;
						break;
					}
				}
				materials->filter_presets(printer, printer_name, type, EMPTY, [this, &appended_vendors](const Preset* p) {
					const std::string& vendor = this->materials->get_vendor(p);
					if (std::find(appended_vendors.begin(), appended_vendors.end(), vendor) == appended_vendors.end()) {
						list_vendor->append(vendor, &vendor);
                        appended_vendors.emplace_back(vendor);
					}
					});
			}
            sort_list_data(list_vendor, true, false);
		}

		sel_type_prev = sel_type;
		sel_vendor = 0;
		sel_vendor_prev = wxNOT_FOUND;
		list_vendor->SetSelection(sel_vendor);
		list_profile->Clear();
	}
         
	if (sel_vendor != sel_vendor_prev) {
		// Refresh material list

		list_profile->Clear();
        std::vector<std::string> appended_aliases;
        clear_compatible_printers_label();
		if (sel_printers_count != 0 && sel_type != wxNOT_FOUND && sel_vendor != wxNOT_FOUND) {
			const std::string& type = list_type->get_data(sel_type);
			const std::string& vendor = list_vendor->get_data(sel_vendor);
			// first printer preset
            std::vector<ProfilePrintData> to_list;
            for (int i = 0; i < sel_printers_count; i++) {
				const std::string& printer_name = list_printer->get_data(sel_printers[i]);
				const Preset* printer = nullptr;
				for (const Preset* it : materials->printers) {
					if (it->name == printer_name) {
						printer = it;
						break;
					}
				}
				materials->filter_presets(printer, printer_name, type, vendor, [this, &to_list, &appended_aliases](const Preset* p) {
					const std::string& section = materials->appconfig_section();
                    bool checked = wizard_p()->appconfig_new.has(section, p->name);
                    bool was_checked = false;

                    auto it = std::find(appended_aliases.begin(), appended_aliases.end(), p->alias);
                    size_t cur_i = 0;
                    if (it == appended_aliases.end()) {
                        cur_i = list_profile->append(p->alias + (materials->get_omnipresent(p) ? "" : " *"), &p->alias);
                        appended_aliases.emplace_back(p->alias);
                        to_list.emplace_back(p->alias, materials->get_omnipresent(p), checked);
                    }
                    else {
                        cur_i = it - appended_aliases.begin();
                        was_checked = list_profile->IsChecked(cur_i);
                        to_list[cur_i].checked = checked || was_checked;
                    }
                    list_profile->Check(cur_i, checked || was_checked);

					/* Update preset selection in config.
					 * If one preset from aliases bundle is selected,
					 * than mark all presets with this aliases as selected
					 * */
					if (checked && !was_checked)
						wizard_p()->update_presets_in_config(section, p->alias, true);
					else if (!checked && was_checked)
						wizard_p()->appconfig_new.set(section, p->name, "1");
					});
			}
            sort_list_data(list_profile, to_list);
		}

		sel_vendor_prev = sel_vendor;
	}
    wxGetApp().UpdateDarkUI(list_profile);
}

void PageMaterials::sort_list_data(StringList* list, bool add_All_item, bool material_type_ordering)
{
// get data from list
// sort data
// first should be <all>
// then prusa profiles
// then the rest
// in alphabetical order
    
    std::vector<std::reference_wrapper<const std::string>> prusa_profiles;
    std::vector<std::pair<std::wstring ,std::reference_wrapper<const std::string>>> other_profiles; // first is lower case id for sorting
    for (int i = 0 ; i < list->size(); ++i) {
        const std::string& data = list->get_data(i);
        if (data == EMPTY) // do not sort <all> item 
            continue;
        if (!material_type_ordering && data.find("Prusa") != std::string::npos)
            prusa_profiles.push_back(data);
        else 
            other_profiles.emplace_back(boost::algorithm::to_lower_copy(boost::nowide::widen(data)),data);
    }
    if (material_type_ordering) {
        // Material types keep the order in which the profiles list them (there is no type enum for SLA resins).
    } else {
        std::sort(prusa_profiles.begin(), prusa_profiles.end(), [](std::reference_wrapper<const std::string> a, std::reference_wrapper<const std::string> b) {
            return a.get() < b.get();
            });
        std::sort(other_profiles.begin(), other_profiles.end(), [](const std::pair<std::wstring, std::reference_wrapper<const std::string>>& a, const std::pair<std::wstring, std::reference_wrapper<const std::string>>& b) {
            return a.first <b.first;
            });
    }
    
    list->Clear();
    if (add_All_item)
        list->append(_L("(All)"), &EMPTY);
    for (const auto& item : prusa_profiles)
        list->append(item, &const_cast<std::string&>(item.get()));
    for (const auto& item : other_profiles)
        list->append(item.second, &const_cast<std::string&>(item.second.get()));
    
}     

void PageMaterials::sort_list_data(PresetList* list, const std::vector<ProfilePrintData>& data)
{
    // sort data
    // then prusa profiles
    // then the rest
    // in alphabetical order
    std::vector<ProfilePrintData> prusa_profiles;
    std::vector<std::pair<std::wstring, ProfilePrintData>> other_profiles; // first is lower case id for sorting
    for (const auto& item : data) {
        const std::string& name = item.name;
        if (name.find("Prusa") != std::string::npos)
            prusa_profiles.emplace_back(item);
        else
            other_profiles.emplace_back(boost::algorithm::to_lower_copy(boost::nowide::widen(name)), item);
    }
    std::sort(prusa_profiles.begin(), prusa_profiles.end(), [](ProfilePrintData a, ProfilePrintData b) {
        return a.name.get() < b.name.get();
        });
    std::sort(other_profiles.begin(), other_profiles.end(), [](const std::pair<std::wstring, ProfilePrintData>& a, const std::pair<std::wstring, ProfilePrintData>& b) {
        return a.first < b.first;
        });
    list->Clear();
    for (size_t i = 0; i < prusa_profiles.size(); ++i) {
        list->append(std::string(prusa_profiles[i].name) + (prusa_profiles[i].omnipresent ? "" : " *"), &const_cast<std::string&>(prusa_profiles[i].name.get()));
        list->Check(i, prusa_profiles[i].checked);
    }
    for (size_t i = 0; i < other_profiles.size(); ++i) {
        list->append(std::string(other_profiles[i].second.name) + (other_profiles[i].second.omnipresent ? "" : " *"), &const_cast<std::string&>(other_profiles[i].second.name.get()));
        list->Check(i + prusa_profiles.size(), other_profiles[i].second.checked);
    }
}

void PageMaterials::select_material(int i)
{
    const bool checked = list_profile->IsChecked(i);

    const std::string& alias_key = list_profile->get_data(i);
    wizard_p()->update_presets_in_config(materials->appconfig_section(), alias_key, checked);
}

void PageMaterials::select_all(bool select)
{
    wxWindowUpdateLocker freeze_guard(this);
    (void)freeze_guard;

    for (unsigned i = 0; i < list_profile->GetCount(); i++) {
        const bool current = list_profile->IsChecked(i);
        if (current != select) {
            list_profile->Check(i, select);
            select_material(i);
        }
    }
}

void PageMaterials::clear()
{
	list_printer->Clear();
    list_type->Clear();
    list_vendor->Clear();
    list_profile->Clear();
	sel_printers_prev.Clear();
    sel_type_prev = wxNOT_FOUND;
    sel_vendor_prev = wxNOT_FOUND;
    presets_loaded = false;
}

void PageMaterials::on_activate()
{
    check_and_update_presets(true);
    first_paint = true;
}

wxString PageMaterials::validate()
{
    check_and_update_presets();
    auto *p = wizard_p();
    const auto missing = p->printer_models_missing_materials();
    if (missing.empty())
        return wxString();
    wxString names;
    for (const VendorProfile::PrinterModel *model : missing)
        names += (names.IsEmpty() ? "" : ", ") + from_u8(model->name);
    return format_wxstr(_L("Select at least one material for: %1%."), names);
}

// ============================================================================
// Path (b): library of printer profiles
// ============================================================================

static wxTextCtrl* wizard_text(wxWindow *parent, const wxString &accessible_name, int width_em = 0, long style = 0)
{
    const int em = em_unit(parent);
    auto *ctrl = new wxTextCtrl(parent, wxID_ANY, wxString(), wxDefaultPosition, wxSize(width_em > 0 ? width_em * em : -1, -1), style);
    wxGetApp().UpdateDarkUI(ctrl);
    // a multi-line control is a scrolled window around the real text view: name the inner widget
    set_accessible_name(ctrl, accessible_name, wxString(), AccessibleRole::Default, (style & wxTE_MULTILINE) != 0);
    return ctrl;
}

PageProfilePrinters::PageProfilePrinters(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Library profile"), _L("Library profile"))
{
    const int em = page_em();
    append_help(_L("Pick a profile from the library. It is saved as a new printer profile when you click Finish; "
                   "nothing is activated before that."));

    filter = wizard_text(this, _L("Filter profiles"));
    filter->SetHint(_L("Type to filter the list"));
    filter->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { fill_list(); });
    append(filter, 0, wxEXPAND | wxBOTTOM, em / 2);

    auto *split = new wxBoxSizer(wxHORIZONTAL);
    profile_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(fit(26), 22 * em));
    wxGetApp().UpdateDarkUI(profile_list);
    set_accessible_name(profile_list, _L("Library profiles"));
    profile_list->Bind(wxEVT_LISTBOX, &PageProfilePrinters::on_profile_selected, this);
    split->Add(profile_list, 1, wxEXPAND | wxRIGHT, em);

    auto *preview = new wxBoxSizer(wxVERTICAL);
    preview_title = new wxStaticText(this, wxID_ANY, _L("No profile selected"), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    preview_title->SetFont(GetFont().MakeBold().Scaled(1.2f));
    preview_body = new wxStaticText(this, wxID_ANY, _L("Select a profile on the left to see its display, resolution and build height."),
                                    wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    preview_body->Wrap(fit(24));
    preview_note = new wxStaticText(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    preview_note->SetFont(GetFont().MakeBold());
    preview->Add(preview_title, 0, wxEXPAND | wxBOTTOM, em / 2);
    preview->Add(preview_body, 0, wxEXPAND | wxBOTTOM, em);
    preview->Add(preview_note, 0, wxEXPAND);
    split->Add(preview, 1, wxEXPAND);
    append(split, 1, wxEXPAND, 0);

    load_profiles();
    fill_list();
}

void PageProfilePrinters::load_profiles()
{
    // Load from resources/printer directory where we've copied all the profiles
    boost::filesystem::path resources_path = boost::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToStdString()).parent_path() / "resources" / "printer";

    // Fallback to the source directory if resources path doesn't exist
    if (!fs::is_directory(resources_path)) {
        resources_path = boost::filesystem::path(Slic3r::resources_dir()) / "printer";
    }

    profiles_dir = resources_path.string();
    profiles.clear();

    if (!fs::is_directory(profiles_dir)) {
        BOOST_LOG_TRIVIAL(warning) << "Profile printers directory not found: " << profiles_dir;
        return;
    }

    try {
        for (const auto& entry : fs::directory_iterator(profiles_dir))
            if (entry.path().extension() == ".ini")
                profiles.push_back({ entry.path().stem().string(), entry.path().string() });
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error reading profiles directory: " << e.what();
        return;
    }

    std::sort(profiles.begin(), profiles.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
}

void PageProfilePrinters::fill_list()
{
    const std::string needle = boost::algorithm::to_lower_copy(into_u8(filter->GetValue()));
    wxWindowUpdateLocker freeze(profile_list);
    profile_list->Clear();
    shown.clear();
    int restore = wxNOT_FOUND;
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (!needle.empty() && boost::algorithm::to_lower_copy(profiles[i].first).find(needle) == std::string::npos)
            continue;
        if (profiles[i].second == selected_path)
            restore = int(shown.size());
        shown.push_back(i);
        profile_list->Append(from_u8(profiles[i].first));
    }
    if (restore != wxNOT_FOUND)
        profile_list->SetSelection(restore);
}

bool PageProfilePrinters::any_selected() const
{
    return !selected_path.empty();
}

std::string PageProfilePrinters::get_selected_profile_path() const
{
    return selected_path;
}

std::string PageProfilePrinters::get_selected_profile_name() const
{
    return selected_path.empty() ? std::string() : fs::path(selected_path).stem().string();
}

DynamicPrintConfig PageProfilePrinters::get_selected_config() const
{
    DynamicPrintConfig cfg;
    if (!selected_path.empty()) {
        try {
            cfg.load_from_ini(selected_path, ForwardCompatibilitySubstitutionRule::Disable);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Error loading profile config: " << e.what();
        }
    }
    return cfg;
}

void PageProfilePrinters::on_profile_selected(wxCommandEvent &event)
{
    const int row = profile_list->GetSelection();
    selected_path = row >= 0 && size_t(row) < shown.size() ? profiles[shown[size_t(row)]].second : std::string();
    show_error(wxString());
    update_preview();
    event.Skip();
}

void PageProfilePrinters::update_preview()
{
    if (selected_path.empty()) {
        preview_title->SetLabel(_L("No profile selected"));
        preview_body->SetLabel(_L("Select a profile on the left to see its display, resolution and build height."));
        preview_note->SetLabel(wxString());
    } else {
        const DynamicPrintConfig cfg = get_selected_config();
        wxString text;
        auto real = [&cfg](const char *key) -> wxString {
            const auto *option = cfg.option<ConfigOptionFloat>(key);
            return option ? double_to_string(option->value) : wxString("?");
        };
        auto integer = [&cfg](const char *key) -> wxString {
            const auto *option = cfg.option<ConfigOptionInt>(key);
            return option ? wxString::Format("%d", option->value) : wxString("?");
        };
        text += format_wxstr(_L("Display: %1% x %2% mm"), real("display_width"), real("display_height")) + "\n";
        text += format_wxstr(_L("Resolution: %1% x %2% px"), integer("display_pixels_x"), integer("display_pixels_y")) + "\n";
        if (const auto *orientation = cfg.option<ConfigOptionEnum<SLADisplayOrientation>>("display_orientation"))
            text += _L("Orientation") + ": " + (orientation->value == sladoLandscape ? _L("Landscape") : _L("Portrait")) + "\n";
        text += format_wxstr(_L("Max print height: %1% mm"), real("max_print_height")) + "\n";
        if (cfg.has("sla_archive_format"))
            text += _L("Output format") + ": " + from_u8(cfg.opt_serialize("sla_archive_format")) + "\n";
        preview_title->SetLabel(from_u8(get_selected_profile_name()));
        preview_body->SetLabel(text);
        preview_note->SetLabel(_L("Will be saved as a new printer profile when you click Finish."));
    }
    preview_title->Wrap(fit(24));
    preview_body->Wrap(fit(24));
    preview_note->Wrap(fit(24));
    Layout();
}

wxString PageProfilePrinters::validate()
{
    if (!any_selected())
        return _L("Select a printer profile from the list to continue.");
    try {
        DynamicPrintConfig source;
        source.load_from_ini(selected_path, ForwardCompatibilitySubstitutionRule::Enable);
        if (!source.has("printer_technology") || source.opt_enum<PrinterTechnology>("printer_technology") != ptSLA)
            return _L("The selected file is not an SLA printer profile.");
    } catch (const std::exception &error) {
        return from_u8(error.what());
    }
    return wxString();
}

void PageProfilePrinters::clear_selection()
{
    selected_path.clear();
    if (profile_list)
        profile_list->SetSelection(wxNOT_FOUND);
    update_preview();
}

void PageProfilePrinters::install_selected_profile(PresetBundle &preset_bundle, AppConfig &app_config) const
{
    install_library_profile(preset_bundle, app_config, selected_path);
}

void install_library_profile(PresetBundle &preset_bundle, AppConfig &app_config, const std::string &profile_path)
{
    DynamicPrintConfig source;
    source.load_from_ini(profile_path, ForwardCompatibilitySubstitutionRule::Enable);
    if (!source.has("printer_technology") || source.opt_enum<PrinterTechnology>("printer_technology") != ptSLA)
        throw std::runtime_error("The selected file is not an SLA printer profile.");
    auto &printers = preset_bundle.printers;
    DynamicPrintConfig config = printers.default_preset_for(source).config;
    config.apply_only(source, config.keys(), true);
    // Bundled standalone profiles must not depend on an uninstalled parent.
    config.set("inherits", std::string());
    Preset::normalize(config);
    std::string name = fs::path(profile_path).stem().string();
    const std::string base_name = name;
    for (unsigned suffix = 2; printers.find_preset(name, false, false) || fs::exists(printers.path_from_name(name)); ++suffix)
        name = base_name + " (" + std::to_string(suffix) + ")";
    // Save before changing the active selection. A failed write leaves the
    // current printer and its configuration untouched.
    Preset candidate = printers.default_preset_for(source);
    candidate.name = name;
    candidate.is_default = candidate.is_system = candidate.is_external = false;
    candidate.vendor = nullptr;
    candidate.alias.clear();
    candidate.renamed_from.clear();
    candidate.file = printers.path_from_name(name);
    candidate.config = std::move(config);
    candidate.save();
    printers.load_preset(candidate.file, name, candidate.config, true);
    preset_bundle.update_compatible(PresetSelectCompatibleType::Always);
    preset_bundle.export_selections(app_config);
}

// ============================================================================
// Path (c): custom printer (name + base profile; geometry follows on PageBuildVolume)
// ============================================================================

const char *PageCustom::default_profile_name = "My SLA Printer";

PageCustom::PageCustom(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Custom printer"), _L("Custom printer"))
{
    const int em = page_em();
    append_help(_L("Name your printer and pick a profile to start from. The next step sets its display and build volume."));

    append_heading(_L("Printer name"));
    wxBoxSizer* profile_name_sizer = new wxBoxSizer(wxVERTICAL);
    profile_name_editor = new SavePresetDialog::Item{ this, profile_name_sizer, default_profile_name, wxGetApp().preset_bundle};
    profile_name_editor->set_accessible_label(_L("Printer name"));
    profile_name_editor->is_taken = [this](const std::string &name) {
        for (const std::string &queued : wizard_p()->queued_custom_names())
            if (boost::iequals(queued, name))
                return true;
        return false;
    };
    profile_name_editor->set_name(default_profile_name);
    append(profile_name_sizer, 0, wxEXPAND, 0);

    append_spacer(em);
    append_heading(_L("Start from"));
    append_help(_L("The display and build volume of the chosen profile are the starting values of the next step."));
    sample_sla_choice = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 9 * em));
    wxGetApp().UpdateDarkUI(sample_sla_choice);
    set_accessible_name(sample_sla_choice, _L("Base printer profile"));

    for (const Preset& preset : wxGetApp().preset_bundle->printers) {
        if (preset.printer_technology() != ptSLA)
            continue;
        sample_sla_profile_names.emplace_back(preset.name);
        sample_sla_choice->Append(from_u8(preset.name));
    }
    if (sample_sla_profile_names.empty()) {
        // Collection iteration excludes defaults, but a clean installation
        // still needs a usable SLA base for its first custom printer.
        const Preset &preset = wxGetApp().preset_bundle->printers.default_preset(0);
        sample_sla_profile_names.emplace_back(preset.name);
        sample_sla_choice->Append(from_u8(preset.name));
    }
    const auto &selected = wxGetApp().preset_bundle->printers.get_selected_preset().name;
    auto found = std::find(sample_sla_profile_names.begin(), sample_sla_profile_names.end(), selected);
    sample_sla_choice->SetSelection(found == sample_sla_profile_names.end() ? 0 : int(found - sample_sla_profile_names.begin()));

    sample_sla_choice->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        const std::string sample_name = sample_sla_profile_name();
        if (sample_name.empty())
            return;
        if (const Preset *preset = wxGetApp().preset_bundle->printers.find_preset(sample_name, false)) {
            wizard_p()->custom_config->apply(preset->config, true);
            wizard_p()->page_bvolume->load_config(*wizard_p()->custom_config);
        }
    });
    append(sample_sla_choice, 0, wxEXPAND, 0);
}

void PageCustom::prepare_next()
{
    const std::string base = default_profile_name;
    std::string name = base;
    for (unsigned suffix = 2; ; ++suffix) {
        profile_name_editor->set_name(name);
        if (profile_name_editor->is_valid() && profile_name_editor->preset_name() == name)
            break;
        if (suffix > 1000)
            break;
        name = base + " " + std::to_string(suffix);
    }
}

wxString PageCustom::validate()
{
    profile_name_editor->revalidate();
    if (!profile_name_editor->is_valid() || profile_name().empty())
        return _L("Enter a unique printer name (the name is already used or not allowed).");
    return wxString();
}

std::string PageCustom::sample_sla_profile_name() const
{
    if (sample_sla_choice == nullptr)
        return {};

    const int selection = sample_sla_choice->GetSelection();
    if (selection < 0 || size_t(selection) >= sample_sla_profile_names.size())
        return {};

    return sample_sla_profile_names[size_t(selection)];
}

// ============================================================================
// Optional: custom material
// ============================================================================

static double parse_text_double(wxTextCtrl *ctrl, double fallback)
{
    if (ctrl == nullptr)
        return fallback;
    wxString text = ctrl->GetValue();
    text.Replace(is_decimal_separator_point() ? ',' : '.', is_decimal_separator_point() ? '.' : ',', false);
    double value = fallback;
    if (!text.ToDouble(&value))
        return fallback;
    return value;
}

static bool text_is_number(wxTextCtrl *ctrl, double minimum, double maximum)
{
    wxString text = ctrl->GetValue();
    text.Replace(is_decimal_separator_point() ? ',' : '.', is_decimal_separator_point() ? '.' : ',', false);
    double value = 0.;
    return text.ToDouble(&value) && std::isfinite(value) && value >= minimum && value <= maximum;
}

PageCustomMaterial::PageCustomMaterial(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Custom material"), _L("Custom material"))
{
    optional_step = true;
    const int em = page_em();
    append_help(_L("Optional. Skip this step to use the materials you picked. Turn the option on only if you want "
                          "an own resin profile with its own exposure times."));

    cb_create = new wxCheckBox(this, wxID_ANY, _L("Create my own material profile"));
    cb_create->SetFont(GetFont().MakeBold());
    cb_create->SetValue(false);
    set_accessible_name(cb_create, _L("Create my own material profile"), wxString(), AccessibleRole::CheckBox);
    append(cb_create, 0, wxEXPAND | wxBOTTOM, em / 2);

    form = new wxPanel(this);
    wxGetApp().UpdateDarkUI(form);
    auto *form_sizer = new wxBoxSizer(wxVERTICAL);
    form->SetSizer(form_sizer);

    form_sizer->Add(new wxStaticText(form, wxID_ANY, _L("Start from material:")), 0, wxBOTTOM, em / 4);
    base_choice = new wxListBox(form, wxID_ANY, wxDefaultPosition, wxSize(-1, 6 * em));
    wxGetApp().UpdateDarkUI(base_choice);
    set_accessible_name(base_choice, _L("Base material profile"));
    form_sizer->Add(base_choice, 0, wxEXPAND | wxBOTTOM, em);

    auto *grid = new wxFlexGridSizer(3, em / 2, em);
    grid->AddGrowableCol(1, 1);
    auto row = [&](const wxString &label, const wxString &accessible, wxTextCtrl *&ctrl, const wxString &unit = wxString(), long style = 0) {
        ctrl = wizard_text(form, accessible, 0, style);
        grid->Add(new wxStaticText(form, wxID_ANY, label + ":"), 0, wxALIGN_CENTRE_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
        grid->Add(new wxStaticText(form, wxID_ANY, unit), 0, wxALIGN_CENTRE_VERTICAL);
    };
    row(_L("Material name"), _L("Material name"), name_input);
    row(_L("Brand"), _L("Material brand"), vendor_input);
    row(_L("Type"), _L("Material type"), type_input);
    row(_L("Bottle cost"), _L("Bottle cost"), cost_input, _L("currency"));
    row(_L("Exposure"), _L("Required exposure"), exposure_input, _L("s"));
    row(_L("Initial exposure"), _L("Required initial exposure"), initial_exposure_input, _L("s"));
    row(_L("Hop height (below)"), _L("Required hop height below"), hop_below_input, _L("mm"));
    row(_L("Hop height (above)"), _L("Required hop height above"), hop_above_input, _L("mm"));
    row(_L("Notes"), _L("Material notes"), notes_input, wxString(), wxTE_MULTILINE);
    notes_input->SetMinSize(wxSize(-1, 3 * em));
    form_sizer->Add(grid, 0, wxEXPAND);
    append(form, 0, wxEXPAND, 0);

    name_input->ChangeValue(_L("My Resin"));

    cb_create->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { update_form(); show_error(wxString()); });
    base_choice->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { load_defaults_from(base_sla_material_profile_name()); });
    update_form();
}

void PageCustomMaterial::update_form()
{
    form->Show(cb_create->GetValue());
    Layout();
    wizard_p()->hscroll->Layout();
    wizard_p()->hscroll->FitInside();
}

void PageCustomMaterial::on_activate()
{
    // Offer the materials known at this moment as the starting point (kept selection when still present).
    const std::string previous = base_sla_material_profile_name();
    base_names.clear();
    base_choice->Clear();
    auto add_name = [this](const std::string &name) {
        if (std::find(base_names.begin(), base_names.end(), name) == base_names.end()) {
            base_names.emplace_back(name);
            base_choice->Append(from_u8(name));
        }
    };
    // Materials ticked on the previous step come first (vendor path), then everything already installed.
    auto *wiz = wizard_p();
    if (wiz->vendor_active() && wiz->appconfig_new.has_section(AppConfig::SECTION_MATERIALS))
        for (const auto &kv : wiz->appconfig_new.get_section(AppConfig::SECTION_MATERIALS))
            if (kv.second == "1")
                add_name(kv.first);
    for (const Preset& preset : wxGetApp().preset_bundle->sla_materials) {
        if (!preset.is_visible && !preset.is_default)
            continue;
        add_name(preset.name);
    }
    if (base_names.empty()) {
        const Preset &preset = wxGetApp().preset_bundle->sla_materials.default_preset();
        base_names.emplace_back(preset.name);
        base_choice->Append(from_u8(preset.name));
    }
    auto found = std::find(base_names.begin(), base_names.end(), previous);
    base_choice->SetSelection(found == base_names.end() ? 0 : int(found - base_names.begin()));
    if (found == base_names.end())
        load_defaults_from(base_sla_material_profile_name());
}

void PageCustomMaterial::load_defaults_from(const std::string &base_name)
{
    const Preset *preset = base_name.empty() ? nullptr : wxGetApp().preset_bundle->sla_materials.find_preset(base_name, false);
    // A material ticked in this wizard run is not installed yet: read it from the wizard's own bundles.
    if (preset == nullptr && !base_name.empty())
        for (const auto &bundle : wizard_p()->bundles)
            if ((preset = bundle.second.preset_bundle->sla_materials.find_preset(base_name, false)) != nullptr)
                break;
    if (preset == nullptr)
        preset = wxGetApp().preset_bundle->sla_materials.find_preset(base_name, true);
    if (preset == nullptr)
        return;
    if (auto *opt = preset->config.option<ConfigOptionString>("material_vendor"); opt != nullptr)
        vendor_input->ChangeValue(from_u8(opt->value));
    if (auto *opt = preset->config.option<ConfigOptionString>("material_type"); opt != nullptr)
        type_input->ChangeValue(from_u8(opt->value));
    if (auto *opt = preset->config.option<ConfigOptionFloat>("bottle_cost"); opt != nullptr)
        cost_input->ChangeValue(double_to_string(opt->value));
    if (auto *opt = preset->config.option<ConfigOptionFloat>("exposure_time"); opt != nullptr)
        exposure_input->ChangeValue(double_to_string(opt->value));
    if (auto *opt = preset->config.option<ConfigOptionFloat>("initial_exposure_time"); opt != nullptr)
        initial_exposure_input->ChangeValue(double_to_string(opt->value));
    if (auto *opt = preset->config.option<ConfigOptionFloats>("tower_hop_height"); opt != nullptr) {
        if (!opt->values.empty())
            hop_below_input->ChangeValue(double_to_string(opt->values[0]));
        if (opt->values.size() > 1)
            hop_above_input->ChangeValue(double_to_string(opt->values[1]));
    }
    if (auto *opt = preset->config.option<ConfigOptionString>("material_notes"); opt != nullptr)
        notes_input->ChangeValue(from_u8(opt->value));
}

wxString PageCustomMaterial::validate()
{
    if (!custom_material_profile_enabled())
        return wxString();
    std::string name = custom_material_profile_name();
    boost::trim(name);
    if (name.empty())
        return _L("Enter a name for the material profile (or turn the option off).");
    // The material named like a (queued or current) custom printer is the one created for that printer.
    std::set<std::string> printer_names = wizard_p()->queued_custom_names();
    if (wizard_p()->path() == WizardPath::Custom && wizard_p()->page_custom)
        printer_names.insert(wizard_p()->page_custom->profile_name());
    if (printer_names.count(name) == 0 && wxGetApp().preset_bundle->sla_materials.find_preset(name, false) != nullptr)
        return format_wxstr(_L("A material profile named \"%1%\" already exists. Choose another name."), from_u8(name));
    if (!text_is_number(exposure_input, 0.1, 1000.))
        return _L("Exposure: enter a number of seconds between 0.1 and 1000.");
    if (!text_is_number(initial_exposure_input, 0.1, 10000.))
        return _L("Initial exposure: enter a number of seconds between 0.1 and 10000.");
    if (!text_is_number(cost_input, 0., 1e7))
        return _L("Bottle cost: enter a number (0 or more).");
    if (!text_is_number(hop_below_input, 0., 1000.) || !text_is_number(hop_above_input, 0., 1000.))
        return _L("Hop height: enter a number of millimetres (0 or more).");
    return wxString();
}

std::string PageCustomMaterial::base_sla_material_profile_name() const
{
    if (base_choice == nullptr)
        return {};
    const int selection = base_choice->GetSelection();
    if (selection < 0 || size_t(selection) >= base_names.size())
        return {};
    return base_names[size_t(selection)];
}

std::string PageCustomMaterial::custom_material_profile_name() const { return into_u8(name_input->GetValue()); }
std::string PageCustomMaterial::custom_material_vendor() const { return into_u8(vendor_input->GetValue()); }
std::string PageCustomMaterial::custom_material_type() const { return into_u8(type_input->GetValue()); }
std::string PageCustomMaterial::custom_material_notes() const { return into_u8(notes_input->GetValue()); }
double PageCustomMaterial::custom_material_cost() const { return parse_text_double(cost_input, 0.0); }
double PageCustomMaterial::custom_material_hop_below() const { return parse_text_double(hop_below_input, 5.0); }
double PageCustomMaterial::custom_material_hop_above() const { return parse_text_double(hop_above_input, 5.0); }
double PageCustomMaterial::custom_material_exposure_time() const { return parse_text_double(exposure_input, 2.0); }
double PageCustomMaterial::custom_material_initial_exposure_time() const { return parse_text_double(initial_exposure_input, 20.0); }

// ============================================================================
// Optional: preferences
// ============================================================================

PagePreferences::PagePreferences(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Preferences"), _L("Preferences"))
{
    optional_step = true;
    append_help(_L("Optional. The defaults are fine for most people; you can change all of this later in Preferences. "
                   "The application never checks for updates or downloads anything on its own."));

    check_inch = new wxCheckBox(this, wxID_ANY, _L("Show object sizes in inches"));
    check_inch->SetValue(wxGetApp().app_config->get_bool("use_inches"));
    set_accessible_name(check_inch, _L("Show object sizes in inches"), wxString(), AccessibleRole::CheckBox);
    append(check_inch);

    check_pathnames = new wxCheckBox(this, wxID_ANY, _L("Remember where model files came from (Reload from disk)"));
    check_pathnames->SetValue(wxGetApp().app_config->get_bool("export_sources_full_pathnames"));
    set_accessible_name(check_pathnames, _L("Remember where model files came from"), wxString(), AccessibleRole::CheckBox);
    append(check_pathnames);
    append_help(_L("When on, full file paths of the models are stored in 3MF files so that Reload from disk finds them by itself. "
                   "When off, Reload from disk asks for each file."));
}

// ============================================================================
// Summary
// ============================================================================

PageSummary::PageSummary(ConfigWizard *parent)
    : ConfigWizardPage(parent, _L("Summary"), _L("Summary"))
{
    append_help(_L("This is exactly what happens when you click Finish. Nothing has been installed, created or activated yet."));
    holder = new wxPanel(this);
    wxGetApp().UpdateDarkUI(holder);
    holder->SetSizer(new wxBoxSizer(wxVERTICAL));
    append(holder, 1, wxEXPAND, 0);
}

void PageSummary::on_activate()
{
    rebuild();
}

void PageSummary::rebuild()
{
    auto *p = wizard_p();
    const int em = page_em();
    wxSizer *sizer = holder->GetSizer();
    sizer->Clear(true);

    auto heading = [&](const wxString &text) {
        auto *label = new wxStaticText(holder, wxID_ANY, text);
        label->SetFont(GetFont().MakeBold().Scaled(1.15f));
        sizer->Add(label, 0, wxEXPAND | wxTOP | wxBOTTOM, em / 2);
    };
    auto line = [&](const wxString &text) {
        auto *label = new wxStaticText(holder, wxID_ANY, wxString::FromUTF8("\xE2\x80\xA2 ") + text);
        label->Wrap(wrap_width());
        sizer->Add(label, 0, wxEXPAND | wxLEFT | wxBOTTOM, em);
    };

    // A line with an optional "Remove" button (queued items only).
    auto removable_line = [&](const wxString &text, const std::function<void()> &remove) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        auto *label = new wxStaticText(holder, wxID_ANY, wxString::FromUTF8("\xE2\x80\xA2 ") + text);
        label->Wrap(wrap_width() - 12 * em);
        row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
        auto *button = new wxButton(holder, wxID_ANY, _L("Remove"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        wxGetApp().UpdateDarkUI(button);
        const wxString hint = format_wxstr(_L("Remove queued printer \"%1%\""), text);
        button->SetToolTip(hint);
        set_accessible_name(button, hint);
        button->Bind(wxEVT_BUTTON, [this, remove](wxCommandEvent &) { CallAfter(remove); });
        row->Add(button, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxBOTTOM, em);
    };

    const std::vector<WizardQueueItem> queued = p->queue_items();
    const bool multiple = !queued.empty();
    heading(multiple ? _L("Printers") : _L("Printer"));
    bool any_printer = false;
    if (p->vendor_active()) {
        if (p->vendor_queued) {
            // One line per model so that a new one can be taken out of the queue.
            const auto &installed = wxGetApp().app_config->vendors();
            size_t queued_index = 0;
            for (PagePrinters *page : p->pages_msla)
                for (const PagePrinters::TickedModel &model : page->ticked_models()) {
                    any_printer = true;
                    const auto vendor = installed.find(model.vendor_id);
                    const bool known = vendor != installed.end() && vendor->second.count(model.model_id) != 0 &&
                                       vendor->second.at(model.model_id).count(model.variant) != 0;
                    const wxString text = from_u8(page->vendor_profile ? page->vendor_profile->name : std::string()) + ": " + from_u8(model.label);
                    if (known)
                        line(text);
                    else if (queued_index < queued.size())
                        removable_line(text, queued[queued_index++].remove);
                }
        } else {
            for (PagePrinters *page : p->pages_msla) {
                const std::vector<std::string> names = page->selected_model_names();
                if (names.empty())
                    continue;
                any_printer = true;
                wxString joined;
                for (const std::string &name : names)
                    joined += (joined.IsEmpty() ? "" : ", ") + from_u8(name);
                line(from_u8(page->vendor_profile ? page->vendor_profile->name : std::string()) + ": " + joined);
            }
        }
    }
    // Library profiles and custom printers queued earlier (the vendor models above come first in `queued`).
    {
        const size_t first = queued.size() - p->queue.size();
        for (size_t i = 0; i < p->queue.size(); ++i) {
            const auto &item = p->queue[i];
            any_printer = true;
            if (item.kind == WizardQueuedPrinter::Kind::Library)
                removable_line(format_wxstr(_L("Library profile \"%1%\" is saved as a new printer profile."), from_u8(item.name)), queued[first + i].remove);
            else
                removable_line(format_wxstr(_L("New custom printer \"%1%\" (%2%), based on \"%3%\"."), from_u8(item.name), from_u8(item.detail), base_profile_label(item.base_name)),
                               queued[first + i].remove);
        }
    }
    switch (p->path()) {
    case WizardPath::Vendor:
        if (!any_printer)
            line(_L("No new vendor printer. Your existing printers are kept."));
        break;
    case WizardPath::Library:
        if (p->page_profile_printers && p->page_profile_printers->any_selected()) {
            any_printer = true;
            line(format_wxstr(_L("Library profile \"%1%\" is saved as a new printer profile and activated."),
                              from_u8(p->page_profile_printers->get_selected_profile_name())));
        } else if (!any_printer)
            line(_L("No library profile selected yet."));
        break;
    case WizardPath::Custom:
        if (p->page_custom && p->page_bvolume) {
            any_printer = true;
            line(format_wxstr(_L("New custom printer \"%1%\", based on \"%2%\"."), from_u8(p->page_custom->profile_name()),
                              base_profile_label(p->page_custom->sample_sla_profile_name())));
            const auto *bv = p->page_bvolume;
            line(format_wxstr(_L("Display %1% x %2% mm, %3% x %4% px, %5%; build height %6% mm."),
                              bv->display_width->GetValue(), bv->display_height->GetValue(),
                              bv->display_pixels_x->GetValue(), bv->display_pixels_y->GetValue(),
                              bv->display_orientation->GetSelection() == 0 ? _L("landscape") : _L("portrait"),
                              bv->build_volume->GetValue()));
            wxString extras;
            if (bv->display_mirror_x->GetValue()) extras += _L("mirrored horizontally") + "; ";
            if (bv->display_mirror_y->GetValue()) extras += _L("mirrored vertically") + "; ";
            extras += bv->use_tilt->GetValue() ? _L("tilt function on") : _L("tilt function off");
            line(extras);
        }
        break;
    case WizardPath::None:
        if (!any_printer)
            line(_L("No printer chosen yet."));
        break;
    }

    // Optional: more printers in this run. Nothing is required here; Finish works without it.
    {
        const bool narrow = wrap_width() < 40 * em;
        auto *row = new wxBoxSizer(narrow ? wxVERTICAL : wxHORIZONTAL);
        auto *add = new wxButton(holder, wxID_ANY, _L("Add another printer"));
        wxGetApp().UpdateDarkUI(add);
        const wxString hint = _L("Optional: keep what you chose so far and add one more printer in this run. Nothing is installed before Finish.");
        add->SetToolTip(hint);
        set_accessible_name(add, _L("Add another printer"), hint);
        add->Bind(wxEVT_BUTTON, [this, p](wxCommandEvent &) { CallAfter([p]() { p->add_another_printer(); }); });
        row->Add(add, 0, narrow ? wxBOTTOM : (wxALIGN_CENTER_VERTICAL | wxRIGHT), narrow ? em / 2 : em);
        auto *note = new wxStaticText(holder, wxID_ANY, _L("Optional - only if you have more than one printer."));
        note->SetForegroundColour(wizard_muted_colour());
        note->Wrap(narrow ? wrap_width() - 2 * em : wrap_width() - 24 * em);
        row->Add(note, 1, narrow ? 0 : wxALIGN_CENTER_VERTICAL);
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, em);
    }

    const bool custom_in_play = p->path() == WizardPath::Custom || p->queue_has(WizardQueuedPrinter::Kind::Custom);
    if (p->vendor_active() || custom_in_play) {
        heading(_L("Materials"));
        if (p->vendor_active()) {
            std::vector<std::string> names;
            if (p->appconfig_new.has_section(AppConfig::SECTION_MATERIALS))
                for (const auto &kv : p->appconfig_new.get_section(AppConfig::SECTION_MATERIALS))
                    if (kv.second == "1")
                        names.push_back(kv.first);
            if (names.empty())
                line(_L("No material selected."));
            else {
                wxString joined;
                size_t shown = 0;
                for (const std::string &name : names) {
                    if (shown++ >= 6) break;
                    joined += (joined.IsEmpty() ? "" : ", ") + from_u8(name);
                }
                if (names.size() > 6)
                    joined += format_wxstr(_L(" and %1% more"), names.size() - 6);
                line(format_wxstr(_L("%1% material profile(s): %2%"), names.size(), joined));
            }
        }
        if (custom_in_play) {
            wxString printers;
            for (const std::string &name : p->queued_custom_names())
                printers += (printers.IsEmpty() ? "" : ", ") + from_u8(name);
            if (p->path() == WizardPath::Custom && p->page_custom)
                printers += (printers.IsEmpty() ? "" : ", ") + from_u8(p->page_custom->profile_name());
            line(format_wxstr(_L("Material profile \"%1%\" is created with the defaults of the new printer."), printers));
        }
        if (p->page_custom_material && p->custom_material_step_active() && p->page_custom_material->custom_material_profile_enabled())
            line(format_wxstr(_L("New custom material \"%1%\" (exposure %2% s, initial exposure %3% s) becomes the active material."),
                              from_u8(p->page_custom_material->custom_material_profile_name()),
                              double_to_string(p->page_custom_material->custom_material_exposure_time()),
                              double_to_string(p->page_custom_material->custom_material_initial_exposure_time())));
        else
            line(_L("No custom material is created."));
    }

    heading(_L("Preferences"));
    if (p->page_preferences) {
        line(p->page_preferences->use_inches() ? _L("Object sizes in inches.") : _L("Object sizes in millimetres."));
        line(p->page_preferences->full_pathnames() ? _L("Model file paths are stored in 3MF files.") : _L("Model file paths are not stored."));
    }

    wxString message;
    ConfigWizardPage *bad = p->first_invalid_page(message);
    show_error(bad && bad != this ? format_wxstr(_L("Not ready: step \"%1%\" - %2%"), bad->shortname, message) : wxString());

    holder->Layout();
    Layout();
    p->hscroll->FitInside();
}

namespace DownloaderUtils
{
namespace {
#ifdef _WIN32
    wxString get_downloads_path()
    {
        wxString ret;
        PWSTR path = NULL;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &path);
        if (SUCCEEDED(hr)) {
            ret = wxString(path);
        }
        CoTaskMemFree(path);
        return ret;
    }
#elif  __APPLE__
    wxString get_downloads_path()
    {
        // call objective-c implementation
        return wxString::FromUTF8(get_downloads_path_mac());
    }
#else
    wxString get_downloads_path()
    {
        wxString command = "xdg-user-dir DOWNLOAD";
        wxArrayString output;
        GUI::desktop_execute_get_result(command, output);
        if (output.GetCount() > 0) {
            return output[0];
        }
        return wxString();
    }
#endif
 }
Worker::Worker(wxWindow* parent)
: wxBoxSizer(wxHORIZONTAL)
, m_parent(parent)
{
    m_input_path = new wxTextCtrl(m_parent, wxID_ANY);
    set_path_name(get_app_config()->get("url_downloader_dest"));

    auto* path_label = new wxStaticText(m_parent, wxID_ANY, _L("Download path") + ":");

    this->Add(path_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    this->Add(m_input_path, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 5);

    auto* button_path = new wxButton(m_parent, wxID_ANY, _L("Browse"));
    wxGetApp().SetWindowVariantForButton(button_path);
    this->Add(button_path, 0, wxEXPAND | wxTOP | wxLEFT, 5);
    button_path->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
        boost::filesystem::path chosen_dest(into_u8(m_input_path->GetValue()));

        wxDirDialog dialog(m_parent, _L("Choose folder") + ":", chosen_dest.string() );
        if (dialog.ShowModal() == wxID_OK)
            this->m_input_path->SetValue(dialog.GetPath());
    });

    for (wxSizerItem* item : this->GetChildren())
        if (item->IsWindow()) {
            wxWindow* win = item->GetWindow();
            wxGetApp().UpdateDarkUI(win);
        }
}

void Worker::set_path_name(wxString path)
{
    if (path.empty())
        path = boost::nowide::widen(get_app_config()->get("url_downloader_dest"));

    if (path.empty()) {
        // What should be default path? Each system has Downloads folder, that could be good one.
        // Other would be program location folder - not so good: access rights, apple bin is inside bundle...
        // default_path = boost::dll::program_location().parent_path().string();
        path = get_downloads_path();
    }

    m_input_path->SetValue(path);
}

void Worker::set_path_name(const std::string& name)
{
    if (!m_input_path)
        return;

    set_path_name(boost::nowide::widen(name));
}

} // DownLoader

#ifdef __linux__
bool DownloaderUtils::Worker::perform_registration_linux = false;
#endif // __linux__

bool DownloaderUtils::Worker::perform_download_register(const std::string& path)
{
    boost::filesystem::path aux_dest (path);
    boost::system::error_code ec;
    boost::filesystem::path chosen_dest = boost::filesystem::absolute(aux_dest, ec);
    if(ec)
        chosen_dest = aux_dest;
    ec.clear();
    if (chosen_dest.empty() || !boost::filesystem::is_directory(chosen_dest, ec) || ec) {
        std::string err_msg = GUI::format("%1%\n\n%2%",_L("Chosen directory for downloads does not exist.") ,chosen_dest.string());
        BOOST_LOG_TRIVIAL(error) << err_msg;
        show_error(/*m_parent*/ nullptr, err_msg);
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "Downloader registration: Directory for downloads: " << chosen_dest.string();
    wxGetApp().app_config->set("url_downloader_dest", chosen_dest.string());
    return perform_url_register();
}
bool DownloaderUtils::Worker::perform_url_register()
{
#ifdef _WIN32
    // Registry key creation for "prusaslicer://" URL

    boost::filesystem::path binary_path(boost::filesystem::canonical(boost::dll::program_location()));
    // the path to binary needs to be correctly saved in string with respect to localized characters
    wxString wbinary = wxString::FromUTF8(binary_path.string());
    std::string binary_string = (boost::format("%1%") % wbinary).str();
    BOOST_LOG_TRIVIAL(info) << "Downloader registration: Path of binary: " << binary_string;

    //std::string key_string = "\"" + binary_string + "\" \"-u\" \"%1\"";
    //std::string key_string = "\"" + binary_string + "\" \"%1\"";
    std::string key_string = "\"" + binary_string + "\" \"--single-instance\" \"%1\"";

    wxRegKey key_first(wxRegKey::HKCU, "Software\\Classes\\prusaslicer");
    wxRegKey key_full(wxRegKey::HKCU, "Software\\Classes\\prusaslicer\\shell\\open\\command");
    if (!key_first.Exists()) {
        key_first.Create(false);
    }
    key_first.SetValue("URL Protocol", "");

    if (!key_full.Exists()) {
        key_full.Create(false);
    }
    //key_full = "\"C:\\Program Files\\Prusa3D\\PrusaSlicer\\prusa-slicer-console.exe\" \"%1\"";
    key_full = key_string;
#elif __APPLE__
    // Apple registers for custom url in info.plist thus it has to be already registered since build.
    // The url will always trigger opening of prusaslicer and we have to check that user has allowed it. (GUI_App::MacOpenURL is the triggered method)
#elif defined(__linux__) && defined(SLIC3R_DESKTOP_INTEGRATION) 
    // the performation should be called later during desktop integration
    perform_registration_linux = true;
#endif
    return true;
}

void DownloaderUtils::Worker::deregister()
{
#ifdef _WIN32
    std::string key_string = "";
    wxRegKey key_full(wxRegKey::HKCU, "Software\\Classes\\prusaslicer\\shell\\open\\command");
    if (!key_full.Exists()) {
        return;
    }
    key_full = key_string;
#elif __APPLE__
    // TODO
#elif defined(__linux__) && defined(SLIC3R_DESKTOP_INTEGRATION) 
    BOOST_LOG_TRIVIAL(debug) << "DesktopIntegrationDialog::undo_downloader_registration";
    DesktopIntegrationDialog::undo_downloader_registration();
    perform_registration_linux = false;
#endif
}

bool DownloaderUtils::Worker::on_finish() {
    AppConfig* app_config = wxGetApp().app_config;
    bool ac_value = app_config->get_bool("downloader_url_registered");
    BOOST_LOG_TRIVIAL(debug) << "PageDownloader::on_finish_downloader ac_value " << ac_value << " downloader_checked " << downloader_checked;
    if (ac_value && downloader_checked) {
        // already registered but we need to do it again
        if (!perform_download_register(GUI::into_u8(path_name())))
            return false;
        app_config->set("downloader_url_registered", "1");
    } else if (!ac_value && downloader_checked) {
        // register
        if (!perform_download_register(GUI::into_u8(path_name())))
            return false;
        app_config->set("downloader_url_registered", "1");
    } else if (ac_value && !downloader_checked) {
        // deregister, downloads are banned now  
        deregister();
        app_config->set("downloader_url_registered", "0");
    } /*else if (!ac_value && !downloader_checked) {
        // not registered and we dont want to do it
        // do not deregister as other instance might be registered
    } */
    return true;
}


#ifdef _WIN32
PageFilesAssociation::PageFilesAssociation(ConfigWizard* parent)
    : ConfigWizardPage(parent, _L("Files association"), _L("Files association"))
{
    cb_3mf = new wxCheckBox(this, wxID_ANY, _L("Associate .3mf files to PrusaSlicer"));
    cb_stl = new wxCheckBox(this, wxID_ANY, _L("Associate .stl files to PrusaSlicer"));

    append(cb_3mf);
    append(cb_stl);
}
#endif // _WIN32

class DiamTextCtrl : public wxTextCtrl
{
public:
    DiamTextCtrl(wxWindow* parent)
    {
#ifdef _WIN32
        long style = wxBORDER_SIMPLE;
#else
        long style = 0;
#endif
        Create(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(std::max(Field::def_width_thinner(), 10) * wxGetApp().em_unit(), wxDefaultCoord), style);
        wxGetApp().UpdateDarkUI(this);
    }
    ~DiamTextCtrl() {}
};

PageBuildVolume::PageBuildVolume(ConfigWizard* parent)
    // TRN ConfigWizard : Size of possible print, related on printer size
    : ConfigWizardPage(parent, _L("Display and build volume"), _L("Display and volume"), 0)
    , build_volume(new DiamTextCtrl(this))
    , display_width(new DiamTextCtrl(this))
    , display_height(new DiamTextCtrl(this))
    , display_pixels_x(new DiamTextCtrl(this))
    , display_pixels_y(new DiamTextCtrl(this))
    , display_orientation(create_readonly_combo(this, "Display orientation"))
    , display_mirror_x(new wxCheckBox(this, wxID_ANY, _L("Mirror horizontally")))
    , display_mirror_y(new wxCheckBox(this, wxID_ANY, _L("Mirror vertically")))
    , use_tilt(new wxCheckBox(this, wxID_ANY, _L("Enable tilt function")))
{
    const int em = page_em();
    append_help(_L("The values of your printer's LCD and Z axis. They are checked while you type; "
                   "you can find them in the printer's manual."));

    display_orientation->Append(_L("Landscape"));
    display_orientation->Append(_L("Portrait"));
    load_config(*wizard_p()->custom_config);

    set_accessible_name(build_volume, _L("Max print height"));
    set_accessible_name(display_width, _L("Display width"));
    set_accessible_name(display_height, _L("Display height"));
    set_accessible_name(display_pixels_x, _L("Display pixels X"));
    set_accessible_name(display_pixels_y, _L("Display pixels Y"));
    set_accessible_name(display_mirror_x, _L("Mirror horizontally"), wxString(), AccessibleRole::CheckBox);
    set_accessible_name(display_mirror_y, _L("Mirror vertically"), wxString(), AccessibleRole::CheckBox);
    set_accessible_name(use_tilt, _L("Enable tilt function"), wxString(), AccessibleRole::CheckBox);

    build_volume->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) {
        event.Skip();
        // Normalize a valid locale decimal separator only. Invalid input stays
        // visible for correction; Next/Finish must not replace it with defaults.
        wxString text = build_volume->GetValue();
        const char separator = is_decimal_separator_point() ? '.' : ',';
        const bool replaced = text.Replace(separator == '.' ? ',' : '.', separator, false) != 0;
        double value = 0.;
        if (replaced && text.ToDouble(&value) && std::isfinite(value) && value > 0. && value <= 1200.)
            build_volume->ChangeValue(double_to_string(value));
    }, build_volume->GetId());

    auto *grid = new wxFlexGridSizer(3, em / 2, em);
    grid->AddGrowableCol(0, 1);
    auto add_row = [&](const wxString &label, wxWindow *ctrl, const wxString &unit) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTRE_VERTICAL);
        grid->Add(ctrl);
        grid->Add(new wxStaticText(this, wxID_ANY, unit), 0, wxALIGN_CENTRE_VERTICAL);
    };

    append_heading(_L("Height"));
    add_row(_L("Max print height") + ":", build_volume, _L("mm"));
    append(grid, 0, wxEXPAND, 0);

    append_spacer(em);
    append_heading(_L("Display (LCD)"));
    auto *display = new wxFlexGridSizer(3, em / 2, em);
    display->AddGrowableCol(0, 1);
    grid = display;
    add_row(_L("Display width") + ":", display_width, _L("mm"));
    add_row(_L("Display height") + ":", display_height, _L("mm"));
    add_row(_L("Display pixels X") + ":", display_pixels_x, wxString());
    add_row(_L("Display pixels Y") + ":", display_pixels_y, wxString());
    add_row(_L("Display orientation") + ":", display_orientation, wxString());
    append(display, 0, wxEXPAND, 0);

    append_spacer(em);
    append_heading(_L("Mechanics"));
    append(display_mirror_x, 0, wxEXPAND | wxBOTTOM, em / 4);
    append(display_mirror_y, 0, wxEXPAND | wxBOTTOM, em / 4);
    append(use_tilt, 0, wxEXPAND | wxBOTTOM, em / 4);

    for (wxTextCtrl *ctrl : { build_volume, display_width, display_height, display_pixels_x, display_pixels_y })
        ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent &event) { refresh_inline_error(); event.Skip(); });
    display_orientation->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &event) { refresh_inline_error(); event.Skip(); });
}

void PageBuildVolume::on_activate()
{
    refresh_inline_error();
}

void PageBuildVolume::refresh_inline_error()
{
    show_error(validate());
}

wxString PageBuildVolume::validate()
{
    try {
        DynamicPrintConfig candidate = *wizard_p()->custom_config;
        apply_custom_config(candidate);
    } catch (const std::exception &error) {
        return wxString::FromUTF8(error.what());
    }
    return wxString();
}

void PageBuildVolume::load_config(const DynamicPrintConfig &config)
{
    for (const auto &[key, field] : std::initializer_list<std::pair<const char *, wxTextCtrl *>>{
            {"max_print_height", build_volume}, {"display_width", display_width}, {"display_height", display_height}})
        if (const auto *option = config.option<ConfigOptionFloat>(key)) field->ChangeValue(double_to_string(option->value));
    for (const auto &[key, field] : std::initializer_list<std::pair<const char *, wxTextCtrl *>>{
            {"display_pixels_x", display_pixels_x}, {"display_pixels_y", display_pixels_y}})
        if (const auto *option = config.option<ConfigOptionInt>(key)) field->ChangeValue(std::to_string(option->value));
    if (const auto *option = config.option<ConfigOptionEnum<SLADisplayOrientation>>("display_orientation"))
        display_orientation->SetSelection(option->value == sladoLandscape ? 0 : 1);
    if (const auto *option = config.option<ConfigOptionBool>("display_mirror_x")) display_mirror_x->SetValue(option->value);
    if (const auto *option = config.option<ConfigOptionBool>("display_mirror_y")) display_mirror_y->SetValue(option->value);
    if (const auto *option = config.option<ConfigOptionBools>("use_tilt"); option && !option->values.empty())
        use_tilt->SetValue(option->values.front());
}

void PageBuildVolume::apply_custom_config(DynamicPrintConfig &config)
{
    // Parse and validate an isolated candidate. A bad field cannot partially
    // change the wizard's printer, bed, or previously saved profile.
    DynamicPrintConfig candidate = config;
    auto real = [](wxTextCtrl *field, const char *label, double minimum, double maximum) {
        wxString text = field->GetValue();
        const char separator = is_decimal_separator_point() ? '.' : ',';
        text.Replace(separator == '.' ? ',' : '.', separator, false);
        double value = 0.;
        if (!text.ToDouble(&value) || !std::isfinite(value) || value < minimum || value > maximum)
            throw RuntimeError(into_u8(_L(label)) + ": " + _u8L("enter a finite number within the printer setting limits"));
        return value;
    };
    auto pixels = [](wxTextCtrl *field, const char *label) {
        long value = 0;
        if (!field->GetValue().ToLong(&value) || value < 100 || value > 32768 ||
            value > std::numeric_limits<int>::max())
            throw RuntimeError(into_u8(_L(label)) + ": " + _u8L("enter an integer between 100 and 32768 pixels"));
        return int(value);
    };
    const double height = real(build_volume, "Max print height", std::numeric_limits<double>::min(), 1200.);
    const double width = real(display_width, "Display width", 1., std::numeric_limits<double>::max());
    const double screen_height = real(display_height, "Display height", 1., std::numeric_limits<double>::max());
    const int columns = pixels(display_pixels_x, "Display pixels X");
    const int rows = pixels(display_pixels_y, "Display pixels Y");
    const int orientation = display_orientation->GetSelection();
    if (orientation != 0 && orientation != 1)
        throw RuntimeError("Display orientation: select Landscape or Portrait");
    candidate.set("max_print_height", height);
    candidate.set("display_width", width);
    candidate.set("display_height", screen_height);
    candidate.set("display_pixels_x", columns);
    candidate.set("display_pixels_y", rows);
    candidate.set_key_value("display_orientation", new ConfigOptionEnum<SLADisplayOrientation>(
        orientation == 0 ? sladoLandscape : sladoPortrait));
    candidate.set("display_mirror_x", display_mirror_x->GetValue());
    candidate.set("display_mirror_y", display_mirror_y->GetValue());
    if (const auto error = sla::printer_geometry_error(candidate); !error.empty())
        throw RuntimeError(error);
    sla::synchronize_bed_shape(candidate);
    const bool tilt = use_tilt->GetValue();
    candidate.set_key_value("use_tilt", new ConfigOptionBools({tilt, tilt}));
    if (!tilt)
        candidate.set_key_value("tower_hop_height", new ConfigOptionFloats({5., 5.}));
    config = std::move(candidate);
}

wxDECLARE_EVENT(EVT_INDEX_PAGE, wxCommandEvent);

ConfigWizardIndex::ConfigWizardIndex(wxWindow *parent)
    : wxPanel(parent)
    , bg(ScalableBitmap(parent, "PrusaSlicer_192px_transparent.png", 192))
    , item_active(NO_ITEM)
    , item_hover(-1)
    , last_page(NO_ITEM)
{
    wxGetApp().UpdateDarkUI(this);
    msw_rescale();
    set_accessible_name(this, _L("Wizard steps"), wxString(), AccessibleRole::Default);

    Bind(wxEVT_PAINT, &ConfigWizardIndex::on_paint, this);
    Bind(wxEVT_MOTION, &ConfigWizardIndex::on_mouse_move, this);
    Bind(wxEVT_LEFT_UP, &ConfigWizardIndex::on_mouse_left_up, this);
    Bind(wxEVT_LEAVE_WINDOW, &ConfigWizardIndex::on_mouse_leave, this);
}

void ConfigWizardIndex::add_page(ConfigWizardPage *page)
{
    last_page = items.size();
    items.emplace_back(Item { page->shortname, page->indent, page->optional_step, page });
    Refresh();
}

void ConfigWizardIndex::update_steps()
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].page != nullptr)
            items[i].page->set_step_info(i + 1, items.size());
}

std::vector<ConfigWizardPage*> ConfigWizardIndex::pages() const
{
    std::vector<ConfigWizardPage*> out;
    for (const Item &item : items)
        if (item.page != nullptr)
            out.push_back(item.page);
    return out;
}

ConfigWizardPage* ConfigWizardIndex::active_page() const
{
    if (item_active >= items.size()) { return nullptr; }

    return items[item_active].page;
}

void ConfigWizardIndex::go_prev()
{
    // Search for a preceiding item that is a page (not a label, ie. page != nullptr)

    if (item_active == NO_ITEM) { return; }

    for (size_t i = item_active; i > 0; i--) {
        if (items[i - 1].page != nullptr) {
            go_to(i - 1);
            return;
        }
    }
}

void ConfigWizardIndex::go_next()
{
    // Search for a next item that is a page (not a label, ie. page != nullptr)

    if (item_active == NO_ITEM) { return; }

    for (size_t i = item_active + 1; i < items.size(); i++) {
        if (items[i].page != nullptr) {
            go_to(i);
            return;
        }
    }
}

// This one actually performs the go-to op
void ConfigWizardIndex::go_to(size_t i)
{
    if (i != item_active
        && i < items.size()
        && items[i].page != nullptr) {
        auto *former_active = active_page();
        if (former_active != nullptr) {
            former_active->Hide();
        }

        auto *new_active = items[i].page;
        item_active = i;
        new_active->Show();

        wxCommandEvent evt(EVT_INDEX_PAGE, GetId());
        AddPendingEvent(evt);

        Refresh();

        new_active->on_activate();
    }
}

void ConfigWizardIndex::go_to(const ConfigWizardPage *page)
{
    if (page == nullptr) { return; }

    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].page == page) {
            go_to(i);
            return;
        }
    }
}

void ConfigWizardIndex::clear()
{
    auto *former_active = active_page();
    if (former_active != nullptr) { former_active->Hide(); }

    items.clear();
    item_active = NO_ITEM;
}

void ConfigWizardIndex::on_paint(wxPaintEvent & evt)
{
    const auto size = GetClientSize();
    if (size.GetHeight() == 0 || size.GetWidth() == 0) { return; }

    wxPaintDC pdc(this);
    wxGCDC dc(pdc);

    const wxColour fg = wxGetApp().get_label_clr_default();
    const wxColour base = GetBackgroundColour();
    const wxColour accent = wizard_accent_colour();
    const wxColour muted = wizard_muted_colour();
    const int yinc = item_height();
    const int radius = em_h * 7 / 10;
    const int pad = em_w;

    int index_width = 0;
    wxFont normal = GetFont();
    wxFont bold = GetFont().MakeBold();
    wxFont small = GetFont().Scaled(0.85f);

    int y = em_h / 2;
    for (size_t i = 0; i < items.size(); i++) {
        const Item& item = items[i];
        const bool active = i == item_active;
        const bool done = item_active != NO_ITEM && i < item_active;
        const bool hover = item_hover >= 0 && i == size_t(item_hover);

        if (active || hover) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wizard_blend(base, accent, active ? 20 : 9)));
            dc.DrawRoundedRectangle(wxRect(em_w / 4, y, size.x - em_w / 2, yinc - em_h / 4), em_w / 2.);
        }

        // step marker: number in a circle (filled when current, check mark when done)
        const int cx = pad + radius, cy = y + (yinc - em_h / 4) / 2;
        if (active) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(accent));
        } else if (done) {
            dc.SetPen(wxPen(accent, 2));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
        } else {
            dc.SetPen(wxPen(muted, 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
        }
        dc.DrawCircle(cx, cy, radius);
        if (done) {
            dc.SetPen(wxPen(accent, 2));
            const int u = radius / 2;
            dc.DrawLine(cx - u, cy, cx - u / 3, cy + u * 2 / 3);
            dc.DrawLine(cx - u / 3, cy + u * 2 / 3, cx + u, cy - u * 2 / 3);
        } else {
            const wxString number = wxString::Format("%d", int(i + 1));
            dc.SetFont(small);
            const wxSize ts = dc.GetTextExtent(number);
            dc.SetTextForeground(active ? *wxWHITE : muted);
            dc.DrawText(number, cx - ts.x / 2, cy - ts.y / 2);
        }

        int x = cx + radius + em_w;
        dc.SetFont(active ? bold : normal);
        dc.SetTextForeground(fg);
        const wxSize ls = dc.GetTextExtent(item.label);
        dc.DrawText(item.label, x, cy - ls.y / 2);
        int right = x + ls.x;
        if (item.optional) {
            dc.SetFont(small);
            dc.SetTextForeground(muted);
            const wxString tag = _L("optional");
            const wxSize ts = dc.GetTextExtent(tag);
            dc.DrawText(tag, right + em_w / 2, cy - ts.y / 2);
            right += em_w / 2 + ts.x;
        }

        y += yinc;
        index_width = std::max(index_width, right + pad);
    }

    // draw the logo below the steps (its light disc does not suit a dark theme)
    if (int ly = size.y - bg.GetHeight(); ly >= y && !GUI_App::dark_mode()) {
        dc.DrawBitmap(bg.get_bitmap(), 0, ly, false);
        index_width = std::max(index_width, bg.GetWidth() + em_w / 2);
    }

    if (GetMinSize().x < index_width) {
        CallAfter([this, index_width]() {
            SetMinSize(wxSize(index_width, GetMinSize().y));
            wxWindow *dialog = GetParent();
            if (dialog) dialog->Layout();
            Refresh();
        });
    }
}

void ConfigWizardIndex::on_mouse_move(wxMouseEvent &evt)
{
    const wxClientDC dc(this);
    const wxPoint pos = evt.GetLogicalPosition(dc);

    ssize_t item_hover_new = (pos.y - em_h / 2) / item_height();
    if (pos.y < em_h / 2 || item_hover_new >= ssize_t(items.size()) || items[item_hover_new].page == nullptr)
        item_hover_new = -1;

    if (item_hover_new != item_hover) {
        item_hover = item_hover_new;
        Refresh();
    }

    evt.Skip();
}

void ConfigWizardIndex::on_mouse_leave(wxMouseEvent &evt)
{
    if (item_hover != -1) {
        item_hover = -1;
        Refresh();
    }
    evt.Skip();
}

void ConfigWizardIndex::on_mouse_left_up(wxMouseEvent &evt)
{
    const wxClientDC dc(this);
    const wxPoint pos = evt.GetLogicalPosition(dc);
    const ssize_t clicked = (pos.y - em_h / 2) / item_height();
    if (pos.y >= em_h / 2 && clicked >= 0 && clicked < (ssize_t)items.size() && items[clicked].page != nullptr)
        go_to((size_t)clicked);
    evt.Skip();
}

void ConfigWizardIndex::msw_rescale()
{
    const wxSize size = GetTextExtent("m");
    em_w = size.x;
    em_h = size.y;

    SetMinSize(wxSize(std::max(bg.GetWidth(), 22 * em_w), bg.GetHeight()));

    Refresh();
}


// Materials

const std::string Materials::UNKNOWN = "(Unknown)";

void Materials::push(const Preset *preset)
{
    presets.emplace_back(preset);
    types.insert(Materials::get_material_type(preset));
}

void  Materials::add_printer(const Preset* preset)
{
	printers.insert(preset);
}

void Materials::clear()
{
    presets.clear();
    types.clear();
	printers.clear();
    compatibility_counter.clear();
}

const std::string& Materials::appconfig_section() const
{
    return AppConfig::SECTION_MATERIALS;
}

const std::string& Materials::get_type(const Preset *preset) const
{
    return get_material_type(preset);
}

const std::string& Materials::get_vendor(const Preset *preset) const
{
    return get_material_vendor(preset);
}

const std::string& Materials::get_material_type(const Preset *preset)
{
    const auto *opt = preset->config.opt<ConfigOptionString>("material_type");
    if (opt != nullptr) {
        return opt->value;
    } else {
        return UNKNOWN;
    }
}

const std::string& Materials::get_material_vendor(const Preset *preset)
{
    const auto *opt = preset->config.opt<ConfigOptionString>("material_vendor");
    return opt != nullptr ? opt->value : UNKNOWN;
}

// priv

void ConfigWizard::priv::load_pages()
{
    wxWindowUpdateLocker freeze_guard(q);
    (void)freeze_guard;

    ConfigWizardPage *keep = index->active_page();
    index->clear();

    // Step 1 is always the same question; the following steps depend on the chosen path.
    if (page_path)
        index->add_page(page_path);

    switch (path()) {
    case WizardPath::Vendor:
        for (const auto page : pages_msla)
            if (page != nullptr && !is_prusa_printer_page(page))
                index->add_page(page);
        break;
    case WizardPath::Library:
        if (page_profile_printers)
            index->add_page(page_profile_printers);
        break;
    case WizardPath::Custom:
        if (page_custom)
            index->add_page(page_custom);
        if (page_bvolume)
            index->add_page(page_bvolume);
        break;
    case WizardPath::None:
        break;
    }
    // Materials follow the vendor printers that are going to be installed (current choice or queued earlier).
    if (page_sla_materials && (path() == WizardPath::Vendor || (vendor_queued && any_sla_selected)))
        index->add_page(page_sla_materials);
    if (page_custom_material && custom_material_step_active())
        index->add_page(page_custom_material);

    if (page_preferences)
        index->add_page(page_preferences);
#ifdef _WIN32
    if (page_files_association)
        index->add_page(page_files_association);
#endif // _WIN32
    if (page_summary)
        index->add_page(page_summary);

    index->update_steps();

    // Keep the user where they are when the page still exists in the new path.
    if (keep != nullptr)
        index->go_to(keep);
    if (index->active_page() == nullptr && page_path)
        index->go_to(page_path);

    q->Layout();
// This Refresh() is needed to avoid ugly artifacts after printer selection, when no one vendor was selected from the very beginnig
    q->Refresh();
}

bool ConfigWizard::priv::queue_has(QueuedPrinter::Kind kind) const
{
    return std::any_of(queue.begin(), queue.end(), [kind](const QueuedPrinter &item) { return item.kind == kind; });
}

bool ConfigWizard::priv::custom_material_step_active() const
{
    return path() == WizardPath::Vendor || path() == WizardPath::Custom || queue_has(QueuedPrinter::Kind::Custom) ||
           (vendor_queued && any_sla_selected);
}

std::set<std::string> ConfigWizard::priv::queued_custom_names() const
{
    std::set<std::string> names;
    for (const QueuedPrinter &item : queue)
        if (item.kind == QueuedPrinter::Kind::Custom)
            names.insert(item.name);
    return names;
}

bool ConfigWizard::priv::vendors_changed() const
{
    return appconfig_new.vendors() != wxGetApp().app_config->vendors();
}

std::vector<WizardQueueItem> ConfigWizard::priv::queue_items()
{
    std::vector<QueueItem> items;
    if (vendor_queued) {
        const auto &installed = wxGetApp().app_config->vendors();
        for (PagePrinters *page : pages_msla)
            for (const PagePrinters::TickedModel &model : page->ticked_models()) {
                const auto vendor = installed.find(model.vendor_id);
                const bool known = vendor != installed.end() && vendor->second.count(model.model_id) != 0 &&
                                   vendor->second.at(model.model_id).count(model.variant) != 0;
                if (known)
                    continue;   // installed already: it is not an addition of this run
                items.push_back({ format_wxstr(_L("%1% (vendor)"), from_u8(model.label)), [this, page, model]() {
                    page->untick_model(model);
                    // The tick reaches appconfig_new through a pending event; re-evaluate after it.
                    q->CallAfter([this]() { queue_changed(); });
                }});
            }
    }
    for (size_t i = 0; i < queue.size(); ++i) {
        const bool library = queue[i].kind == QueuedPrinter::Kind::Library;
        items.push_back({ format_wxstr(library ? _L("%1% (library)") : _L("%1% (custom)"), from_u8(queue[i].name)),
                          [this, i]() {
                              if (i < queue.size())
                                  queue.erase(queue.begin() + i);
                              queue_changed();
                          }});
    }
    return items;
}

void ConfigWizard::priv::queue_changed()
{
    any_sla_selected = vendor_active() && check_sla_selected();
    if (vendor_queued && !vendors_changed())
        vendor_queued = false;
    if (path() == WizardPath::None && queue.empty() && !vendor_queued && page_path)
        page_path->restore_default();
    custom_printer_selected = path() == WizardPath::Custom;
    for (ConfigWizardPage *page : all_pages)
        page->show_error(wxString());
    if (any_sla_selected && page_sla_materials) {
        update_materials(T_ANY);
        page_sla_materials->clear();
    }
    load_pages();
    if (page_path)
        page_path->refresh_queue();
    if (page_summary && index->active_page() == page_summary)
        page_summary->rebuild();
    if (page_path && path() != WizardPath::None && index->active_page() == nullptr)
        index->go_to(page_path);
}

WizardQueuedPrinter ConfigWizard::priv::make_library_entry() const
{
    QueuedPrinter item;
    item.kind = QueuedPrinter::Kind::Library;
    item.name = page_profile_printers->get_selected_profile_name();
    item.library_path = page_profile_printers->get_selected_profile_path();
    return item;
}

WizardQueuedPrinter ConfigWizard::priv::make_custom_entry(PresetBundle &preset_bundle) const
{
    QueuedPrinter item;
    item.kind = QueuedPrinter::Kind::Custom;
    item.name = page_custom->profile_name();
    item.base_name = page_custom->sample_sla_profile_name();

    DynamicPrintConfig custom_cfg = build_custom_sla_template(preset_bundle);
    if (!item.base_name.empty())
        if (const Preset *preset = preset_bundle.printers.find_preset(item.base_name, true))
            custom_cfg.apply(preset->config, true);
    custom_cfg.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    // No polygon bed page in the SLA flow: the build-volume page derives the bed rectangle from the display size
    // (sla::synchronize_bed_shape). Bed texture/model stay editable in the Printer Settings tab.
    page_bvolume->apply_custom_config(custom_cfg);
    item.config = std::move(custom_cfg);

    const auto real = [&item](const char *key) -> wxString {
        const auto *option = item.config.option<ConfigOptionFloat>(key);
        return option ? double_to_string(option->value) : wxString("?");
    };
    const auto integer = [&item](const char *key) -> wxString {
        const auto *option = item.config.option<ConfigOptionInt>(key);
        return option ? wxString::Format("%d", option->value) : wxString("?");
    };
    item.detail = into_u8(real("display_width") + " x " + real("display_height") + " mm, " +
                          integer("display_pixels_x") + " x " + integer("display_pixels_y") + " px");
    return item;
}

void ConfigWizard::priv::add_another_printer()
{
    // What was chosen so far must be complete; otherwise show the problem where it is.
    wxString message;
    if (ConfigWizardPage *bad = first_invalid_page(message)) {
        index->go_to(bad);
        bad->show_error(message);
        return;
    }
    switch (path()) {
    case WizardPath::Vendor:
        vendor_queued = check_sla_selected() && vendors_changed();
        break;
    case WizardPath::Library:
        if (page_profile_printers->any_selected()) {
            queue.push_back(make_library_entry());
            page_profile_printers->clear_selection();
        }
        break;
    case WizardPath::Custom:
        queue.push_back(make_custom_entry(*wxGetApp().preset_bundle));
        page_custom->prepare_next();
        break;
    case WizardPath::None:
        break;
    }
    page_path->set_path(WizardPath::None, false);
    custom_printer_selected = false;
    any_sla_selected = vendor_active() && check_sla_selected();
    for (ConfigWizardPage *page : all_pages)
        page->show_error(wxString());
    load_pages();
    index->go_to(page_path);
    page_path->refresh_queue();
}

void ConfigWizard::priv::on_path_changed()
{
    custom_printer_selected = path() == WizardPath::Custom;
    any_sla_selected = vendor_active() && check_sla_selected();
    for (ConfigWizardPage *page : all_pages)
        page->show_error(wxString());
    load_pages();
    if (any_sla_selected && page_sla_materials) {
        update_materials(T_ANY);
        page_sla_materials->clear();
    }
}

bool ConfigWizard::priv::existing_printers_kept() const
{
    return custom_printer_in_bundle;
}

ConfigWizardPage* ConfigWizard::priv::first_invalid_page(wxString &message)
{
    if (page_sla_materials && vendor_active())
        page_sla_materials->check_and_update_presets();
    for (ConfigWizardPage *page : index->pages()) {
        message = page->validate();
        if (!message.IsEmpty())
            return page;
    }
    // The vendor path needs at least one printer unless one already exists or was queued.
    if (path() == WizardPath::Vendor && !any_sla_selected && !existing_printers_kept() && queue.empty()) {
        message = _L("Tick at least one printer model to continue.");
        return pages_msla.empty() ? static_cast<ConfigWizardPage*>(page_path) : pages_msla.back();
    }
    if (path() == WizardPath::None && queue.empty() && !(vendor_queued && any_sla_selected) && !existing_printers_kept()) {
        message = _L("Choose how to add your printer to continue.");
        return page_path;
    }
    message.clear();
    return nullptr;
}

int ConfigWizard::priv::content_width() const
{
    const auto idx = wxDisplay::GetFromWindow(q);
    wxDisplay display(idx != wxNOT_FOUND ? idx : 0u);
    const int available = 9 * display.GetClientArea().width / 10;
    // minus the step list, its margin, the dialog margins and a scroll bar
    return std::max(0, available - index->GetMinSize().GetWidth() - INDEX_MARGIN - 2 * DIALOG_MARGIN - 2 * em());
}

void ConfigWizard::priv::init_dialog_size()
{
    // Open at a sensible size: wide enough for the step list plus the widest page, never larger than the screen.

    const auto idx = wxDisplay::GetFromWindow(q);
    wxDisplay display(idx != wxNOT_FOUND ? idx : 0u);

    const auto disp_rect = display.GetClientArea();
    const int content_em = 80;
    int width = index->GetMinSize().GetWidth() + content_em * em() + 6 * em();
    int height = 66 * em();
    width = std::min(width, 9 * disp_rect.width / 10);
    height = std::min(height, 9 * disp_rect.height / 10);
    wxRect window_rect(disp_rect.x + (disp_rect.width - width) / 2, disp_rect.y + (disp_rect.height - height) / 2, width, height);
    q->SetSize(window_rect);
}

void ConfigWizard::priv::load_vendors()
{
    bundles = BundleMap::load();

    // Load up the set of vendors / models / variants the user has had enabled up till now
    AppConfig *app_config = wxGetApp().app_config;
    if (! app_config->legacy_datadir()) {
        appconfig_new.set_vendors(*app_config);
    }
    // A legacy datadir carries no per-vendor selection worth guessing (the old guess worked from FFF printer ini names).

    for (const auto& printer : wxGetApp().preset_bundle->printers) {
        if (!printer.is_default && !printer.is_system && printer.is_visible) {
            custom_printer_in_bundle = true;
            break;
        }
    }

    // Initialize the is_visible flag in printer Presets
    for (auto &pair : bundles) {
        pair.second.preset_bundle->load_installed_printers(appconfig_new);
    }

    // Copy installed SLA material names from app_config to appconfig_new
    // while resolving current names of profiles, which were renamed in the meantime.
    {
    	const std::string &section_name = AppConfig::SECTION_MATERIALS;
		std::map<std::string, std::string> section_new;
		if (app_config->has_section(section_name)) {
			const std::map<std::string, std::string> &section_old = app_config->get_section(section_name);
            for (const auto& material_name_and_installed : section_old)
				if (material_name_and_installed.second == "1") {
					// Material is installed. Resolve it in bundles.
                    size_t num_found = 0;
					const std::string &material_name = material_name_and_installed.first;
				    for (auto &bundle : bundles) {
				    	const PresetCollection &materials = bundle.second.preset_bundle->sla_materials;
				    	const Preset           *preset    = materials.find_preset(material_name);
				    	if (preset == nullptr) {
				    		// Not found. Maybe the material preset is there, bu it was was renamed?
							const std::string *new_name = materials.get_preset_name_renamed(material_name);
							if (new_name != nullptr)
								preset = materials.find_preset(*new_name);
				    	}
                        if (preset != nullptr) {
                            // Materal preset was found, mark it as installed.
                            section_new[preset->name] = "1";
                            ++ num_found;
                        }
				    }
                    if (num_found == 0)
            	        BOOST_LOG_TRIVIAL(error) << boost::format("Profile %1% was not found in installed vendor Preset Bundles.") % material_name;
                    else if (num_found > 1)
            	        BOOST_LOG_TRIVIAL(error) << boost::format("Profile %1% was found in %2% vendor Preset Bundles.") % material_name % num_found;
                }
		}
        appconfig_new.set_section(section_name, section_new);
    }
}

void ConfigWizard::priv::add_page(ConfigWizardPage *page)
{
    const int proportion = (page == page_sla_materials);
    hscroll_sizer->Add(page, proportion, wxEXPAND);
    all_pages.push_back(page);
}

void ConfigWizard::priv::enable_next(bool enable)
{
    btn_next->Enable(enable);
    btn_finish->Enable(enable);
}

void ConfigWizard::priv::set_start_page(ConfigWizard::StartPage start_page)
{
    switch (start_page) {
        case ConfigWizard::SP_MATERIALS:
            // Materials belong to the vendor path; switch to it when the user asked for the materials step.
            if (page_path && page_sla_materials && !pages_msla.empty()) {
                page_path->set_path(WizardPath::Vendor, false);
                on_path_changed();
                index->go_to(page_sla_materials);
                btn_finish->SetFocus();
                break;
            }
            // fallthrough
        default:
            index->go_to(page_path);
            btn_next->SetFocus();
            break;
    }
}

ConfigWizard::priv::Repository* ConfigWizard::priv::get_repo(const std::string& repo_id)
{
    auto it = std::find(repositories.begin(), repositories.end(), repo_id);
    if (it == repositories.end())
        return nullptr;
    return &repositories[it - repositories.begin()];
}

void ConfigWizard::priv::create_vendor_printers_page(const std::string& repo_id, const VendorProfile* vendor, bool install/* = false*/, bool from_single_vendor_repo /*= false*/)
{
    if (is_prusa_vendor(vendor))
        return;

    // Only vendors that ship at least one SLA printer model get a printers page.
    const bool is_sla_technology = std::any_of(vendor->models.begin(), vendor->models.end(),
        [](const VendorProfile::PrinterModel& model) { return model.technology == ptSLA; });
    if (!is_sla_technology)
        return;

    const unsigned indent = 0;

    PagePrinters* pageSLA = new PagePrinters(q, from_u8(vendor->name) + " " + _L("printers"), from_u8(vendor->name), *vendor, indent, T_SLA);
    pageSLA->install = install;
    add_page(pageSLA);

    if (from_single_vendor_repo)
        // single vendor repository
        pages_msla.emplace_back(pageSLA);

    // multiple vendor repository
    auto repo = get_repo(repo_id);
    assert(repo);
    repo->printers_pages.insert({vendor->id, pageSLA});
}

void ConfigWizard::priv::set_run_reason(RunReason run_reason)
{
    this->run_reason = run_reason;
    for (auto &page : all_pages) {
        page->set_run_reason(run_reason);
    }
}

void ConfigWizard::priv::update_materials(Technology technology)
{
    auto add_material = [](Materials& materials, PresetAliases& aliases, const Preset& preset, const Preset* printer = nullptr) {
        if (!materials.containts(&preset)) {
            materials.push(&preset);
            if (!preset.alias.empty())
                aliases[preset.alias].emplace(&preset);
        }
        if (printer) {
            materials.add_printer(printer);
            materials.compatibility_counter[preset.alias].insert(printer);
        }
    };

    if ((any_sla_selected || custom_printer_selected) && (technology & T_SLA)) {
        sla_materials.clear();
        aliases_sla.clear();

        // Iterate SLA materials in all bundles
        for (const auto& [name, bundle] : bundles) {
            for (const auto &material : bundle.preset_bundle->sla_materials) {
                // Iterate printers in all bundles
				// For now, we only allow the profiles to be compatible with another profiles inside the same bundle.
                for (const auto& printer : bundle.preset_bundle->printers) {
                    if(!printer.is_visible || printer.printer_technology() != ptSLA)
                        continue;
                    // Filter out inapplicable printers
                    if (is_compatible_with_printer(PresetWithVendorProfile(material, nullptr), PresetWithVendorProfile(printer, nullptr)))
                        // Check if material is already added
                        add_material(sla_materials, aliases_sla, material, &printer);
                }
            }
        }
    }
}

void ConfigWizard::priv::on_printer_pick(PagePrinters *page, const PrinterPickerEvent &evt)
{
    // Nothing is written here: only the in-memory selection (appconfig_new) and the wizard's own bundles change.
    any_sla_selected = vendor_active() && check_sla_selected();
    if (vendor_queued && !vendors_changed())
        vendor_queued = false;
    for (ConfigWizardPage *p : all_pages)
        p->show_error(wxString());

    // Update the is_visible flag on relevant printer profiles
    for (auto &pair : bundles) {
        if (pair.first != evt.vendor_id) { continue; }

        for (auto &preset : pair.second.preset_bundle->printers) {
            if (preset.config.opt_string("printer_model") == evt.model_id
                && preset.config.opt_string("printer_variant") == evt.variant_name) {
                preset.is_visible = evt.enable;
            }
        }

        // When a printer model is picked, but there is no material installed compatible with this printer model,
        // install default materials for selected printer model silently.
		check_and_install_missing_materials(page->technology, evt.model_id);
    }

    if (page->technology & T_SLA) {
        page_sla_materials->clear();
        if (!any_sla_selected) {
            // clear all material's info, when no one printer is selected
            sla_materials.clear();
            aliases_sla.clear();
        }
    }
}

void ConfigWizard::priv::select_default_materials_for_printer_model(const VendorProfile::PrinterModel &printer_model, Technology technology)
{
    (void)technology;
    PageMaterials* page_materials = page_sla_materials;
    for (const std::string& material : printer_model.default_materials)
        appconfig_new.set(page_materials->materials->appconfig_section(), material, "1");
}

void ConfigWizard::priv::select_default_materials_for_printer_models(Technology technology, const std::set<const VendorProfile::PrinterModel*> &printer_models)
{
    PageMaterials     *page_materials    = page_sla_materials;
    const std::string &appconfig_section = page_materials->materials->appconfig_section();

    // Silently tick the default materials of every model; a model without any default is reported inline by the materials step.
    for (const VendorProfile::PrinterModel* printer_model : printer_models)
        for (const std::string& material : printer_model->default_materials)
            appconfig_new.set(appconfig_section, material, "1");

    update_materials(technology);
    page_sla_materials->reload_presets();
}

bool ConfigWizard::priv::can_finish()
{
    if (!index->active_page()) return false;
    // Printers already queued (library / custom, or new vendor models) are enough on their own.
    const bool queued = !queue.empty() || (vendor_queued && any_sla_selected);
    switch (path()) {
    case WizardPath::Library:
        return queued || (page_profile_printers && page_profile_printers->any_selected());
    case WizardPath::Custom:
        return queued || (page_custom && page_custom->is_valid_profile_name());
    case WizardPath::None:
        return queued || custom_printer_in_bundle;
    case WizardPath::Vendor:
    default:
        // there should be at least one printer, ticked now, queued or already existing
        return any_sla_selected || custom_printer_in_bundle || queued;
    }
}

bool ConfigWizard::priv::can_go_next()
{
    return index->active_page() != nullptr;
}

bool ConfigWizard::priv::can_show_next()
{
    if (!index->active_page()) return false;
    return !index->active_is_last();
}

bool ConfigWizard::priv::on_bnt_finish()
{
    wxBusyCursor wait;

    /* If some printers were added/deleted, but related MaterialPage wasn't activated,
     * than last changes wouldn't be updated for materials.
     */
    if (page_sla_materials && vendor_active())
        page_sla_materials->check_and_update_presets();
    return true;
}

// Walk over all ticked SLA Printer presets and verify whether there is an SLA material profile selected,
// which is compatible with it. In case only_for_model_id is set, the test is done for that particular printer model only.
std::set<const VendorProfile::PrinterModel*> ConfigWizard::priv::printer_models_missing_materials(const std::string &only_for_model_id)
{
    const std::string &section = AppConfig::SECTION_MATERIALS;
    const std::map<std::string, std::string> &appconfig_presets = appconfig_new.has_section(section) ? appconfig_new.get_section(section) : std::map<std::string, std::string>();
    std::set<const VendorProfile::PrinterModel*> printer_models_without_material;
    for (const auto &pair : bundles) {
        const PresetCollection &materials = pair.second.preset_bundle->sla_materials;
        for (const auto &printer : pair.second.preset_bundle->printers) {
            if (printer.is_visible && printer.printer_technology() == ptSLA) {
                const VendorProfile::PrinterModel *printer_model = PresetUtils::system_printer_model(printer);
                if (printer_model == nullptr)
                    continue;
                if ((only_for_model_id.empty() || only_for_model_id == printer_model->id) &&
                    printer_models_without_material.find(printer_model) == printer_models_without_material.end()) {
                    bool has_material = false;
                    for (const auto& preset : appconfig_presets) {
                        if (preset.second == "1") {
                            const Preset *material = materials.find_preset(preset.first, false);
                            if (material != nullptr && is_compatible_with_printer(PresetWithVendorProfile(*material, nullptr), PresetWithVendorProfile(printer, nullptr))) {
                                has_material = true;
                                break;
                            }
                        }
                    }
                    if (! has_material)
                        printer_models_without_material.insert(printer_model);
                }
            }
        }
    }
    return printer_models_without_material;
}

// Return true if the tested Printer Models already had materials selected. Models without one get their default
// materials ticked silently (no dialog); the materials step reports the models that have no default.
bool ConfigWizard::priv::check_and_install_missing_materials(Technology technology, const std::string &only_for_model_id)
{
    if (any_sla_selected && (technology & T_SLA)) {
        std::set<const VendorProfile::PrinterModel*> printer_models_without_material = printer_models_missing_materials(only_for_model_id);
        if (! printer_models_without_material.empty()) {
            if (only_for_model_id.empty())
                select_default_materials_for_printer_models(T_SLA, printer_models_without_material);
            else
                select_default_materials_for_printer_model(**printer_models_without_material.begin(), T_SLA);
            return false;
        }
    }

    return true;
}

static std::set<std::string> get_new_added_presets(const std::map<std::string, std::string>& old_data, const std::map<std::string, std::string>& new_data) 
{
    auto get_aliases = [](const std::map<std::string, std::string>& data) {
        std::set<std::string> old_aliases;
        for (auto item : data) {
            const std::string& name = item.first;
            size_t pos = name.find("@");
            old_aliases.emplace(pos == std::string::npos ? name : name.substr(0, pos-1));
        }
        return old_aliases;
    };

    std::set<std::string> old_aliases = get_aliases(old_data);
    std::set<std::string> new_aliases = get_aliases(new_data);
    std::set<std::string> diff;
    std::set_difference(new_aliases.begin(), new_aliases.end(), old_aliases.begin(), old_aliases.end(), std::inserter(diff, diff.begin()));

    return diff;
}

static std::string get_first_added_preset(const std::map<std::string, std::string>& old_data, const std::map<std::string, std::string>& new_data)
{
    std::set<std::string> diff = get_new_added_presets(old_data, new_data);
    if (diff.empty())
        return std::string();
    return *diff.begin();
}

bool ConfigWizard::priv::apply_config(AppConfig *app_config, PresetBundle *preset_bundle, const PresetUpdaterWrapper *updater, bool& apply_keeped_changes)
{
    // Recheck before any application/preset side effects, including programmatic
    // wizard application that did not navigate through the Build Volume page.
    if (path() == WizardPath::Custom && (!page_bvolume->validate().IsEmpty() || !page_custom->validate().IsEmpty()))
        return false;
    if (path() == WizardPath::Library && (page_profile_printers == nullptr || !page_profile_printers->validate().IsEmpty()))
        return false;
    // Everything this run adds: the queue first, then the printer being edited on the current path.
    std::vector<QueuedPrinter> to_install = queue;
    if (path() == WizardPath::Library && page_profile_printers->any_selected())
        to_install.push_back(make_library_entry());
    if (path() == WizardPath::Custom && page_custom->is_valid_profile_name())
        to_install.push_back(make_custom_entry(*preset_bundle));
    const bool any_library = std::any_of(to_install.begin(), to_install.end(), [](const QueuedPrinter &i) { return i.kind == QueuedPrinter::Kind::Library; });
    const bool any_custom  = std::any_of(to_install.begin(), to_install.end(), [](const QueuedPrinter &i) { return i.kind == QueuedPrinter::Kind::Custom; });
    if (!vendor_active()) {
        // Vendor/material ticks only count for the vendor path: the other paths leave installed vendor printers and materials as they are.
        appconfig_new.set_vendors(*app_config);
        if (app_config->has_section(AppConfig::SECTION_MATERIALS))
            appconfig_new.set_section(AppConfig::SECTION_MATERIALS, app_config->get_section(AppConfig::SECTION_MATERIALS));
    }
    wxString header, caption = _L("Configuration is edited in ConfigWizard");
    const auto enabled_vendors = appconfig_new.vendors();
    const auto enabled_vendors_old = app_config->vendors();

    bool show_info_msg = false;
    bool suppress_sla_printer = model_has_parameter_modifiers_in_objects(wxGetApp().model());
    PrinterTechnology preferred_pt = ptAny;
    auto get_preferred_printer_technology = [enabled_vendors, enabled_vendors_old, suppress_sla_printer, &show_info_msg](const std::string& bundle_name, const Bundle& bundle) {
        const auto config = enabled_vendors.find(bundle_name);
        PrinterTechnology pt = ptAny;
        if (config != enabled_vendors.end()) {
            for (const auto& model : bundle.vendor_profile->models) {
                if (const auto model_it = config->second.find(model.id);
                    model_it != config->second.end() && model_it->second.size() > 0) {
                    pt = model.technology;
                    const auto config_old = enabled_vendors_old.find(bundle_name);
                    if (config_old == enabled_vendors_old.end() || config_old->second.find(model.id) == config_old->second.end()) {
                        // if preferred printer model has SLA printer technology it's important to check the model for modifiers
                        if (pt == ptSLA && suppress_sla_printer) {
                            show_info_msg = true;
                            continue;
                        }
                        return pt;
                    }

                    if (const auto model_it_old = config_old->second.find(model.id);
                        model_it_old == config_old->second.end() || model_it_old->second != model_it->second) {
                        // if preferred printer model has SLA printer technology it's important to check the model for modifiers
                        if (pt == ptSLA && suppress_sla_printer) {
                            show_info_msg = true;
                            continue;
                        }
                        return pt;
                    }
                }
            }
        }
        return ptAny;
    };
    // Prusa printers are considered first, then 3rd party.
    if (auto prusa = bundles.find(PresetBundle::PRUSA_BUNDLE); prusa != bundles.end())
        preferred_pt = get_preferred_printer_technology(prusa->first, prusa->second);
    if (preferred_pt == ptAny || (preferred_pt == ptSLA && suppress_sla_printer)) {
        for (const auto& bundle : bundles) {
            if (bundle.second.is_prusa_bundle) { continue; }
            if (PrinterTechnology pt = get_preferred_printer_technology(bundle.first, bundle.second); pt == ptAny)
                continue;
            else if (preferred_pt == ptAny)
                preferred_pt = pt;
            if(!(preferred_pt == ptAny || (preferred_pt == ptSLA && suppress_sla_printer)))
                break;
        }
    }

    if (show_info_msg)
        show_info(nullptr, _L("It's impossible to print object(s) which contains parameter modifiers with SLA technology.\n\n"
                              "SLA-printer preset will not be selected"), caption);

    bool check_unsaved_preset_changes = false;
    int act_btns = ActionButtons::KEEP;
    if (!check_unsaved_preset_changes)
        act_btns |= ActionButtons::SAVE;

    // Install bundles from resources or cache / vendor if needed:
    std::vector<std::string> install_bundles;
    for (const auto &pair : bundles) {
        if (pair.second.location == BundleLocation::IN_VENDOR) { continue; }

        if (pair.second.is_prusa_bundle) {
            // Always install Prusa bundle, because it has a lot of materials
            // likely to be referenced by other profiles.
            install_bundles.emplace_back(pair.first);
            continue;
        }

        const auto vendor = enabled_vendors.find(pair.first);
        if (vendor == enabled_vendors.end()) {
            // vendor not found
            continue;
        }

        size_t size_sum = 0;
        for (const auto &model : vendor->second) { size_sum += model.second.size(); }

        if (size_sum > 0) {
            // This vendor needs to be installed
            install_bundles.emplace_back(pair.first);
        }
    }
    if (!check_unsaved_preset_changes)
        if ((check_unsaved_preset_changes = install_bundles.size() > 0))
            header = _L_PLURAL("A new vendor was installed and one of its printers will be activated", "New vendors were installed and one of theirs printers will be activated", install_bundles.size());


    // Decide whether to create snapshot based on run_reason and the reset profile checkbox
    bool snapshot = true;
    Snapshot::Reason snapshot_reason = Snapshot::SNAPSHOT_UPGRADE;
    switch (run_reason) {
        case ConfigWizard::RR_DATA_EMPTY:
            snapshot = false;
            break;
        case ConfigWizard::RR_DATA_LEGACY:
            snapshot = true;
            break;
        case ConfigWizard::RR_DATA_INCOMPAT:
            // In this case snapshot has already been taken by
            // PresetUpdater with the appropriate reason
            snapshot = false;
            break;
        case ConfigWizard::RR_USER:
            snapshot = false;
            snapshot_reason = Snapshot::SNAPSHOT_USER;
            break;
    }

    if (snapshot && ! take_config_snapshot_cancel_on_error(*app_config, snapshot_reason, "", _u8L("Do you want to continue changing the configuration?")))
        return false;

    if (check_unsaved_preset_changes &&
        !wxGetApp().check_and_keep_current_preset_changes(caption, header, act_btns, &apply_keeped_changes))
        return false;

    if (install_bundles.size() > 0) {
        // Install bundles from resources or cache / vendor.
        // Don't create snapshot - we've already done that above if applicable.
        bool install_result = updater->install_bundles_rsrc_or_cache_vendor(std::move(install_bundles), false);
        if (!install_result)
            return false;
    } else {
        BOOST_LOG_TRIVIAL(info) << "No bundles need to be installed from resources or cache / vendor";
    }

    std::string preferred_model;
    std::string preferred_variant;
    auto get_preferred_printer_model = [enabled_vendors, enabled_vendors_old, preferred_pt](const std::string& bundle_name, const Bundle& bundle, std::string& variant) {
        const auto config = enabled_vendors.find(bundle_name);
        if (config == enabled_vendors.end())
            return std::string();
        for (const auto& model : bundle.vendor_profile->models) {
            if (const auto model_it = config->second.find(model.id);
                model_it != config->second.end() && model_it->second.size() > 0 &&
                preferred_pt == model.technology) {
                variant = *model_it->second.begin();
                const auto config_old = enabled_vendors_old.find(bundle_name);
                if (config_old == enabled_vendors_old.end())
                    return model.id;
                const auto model_it_old = config_old->second.find(model.id);
                if (model_it_old == config_old->second.end())
                    return model.id;
                else if (model_it_old->second != model_it->second) {
                    for (const auto& var : model_it->second)
                        if (model_it_old->second.find(var) == model_it_old->second.end()) {
                            variant = var;
                            return model.id;
                        }
                }
            }
        }
        if (!variant.empty())
            variant.clear();
        return std::string();
    };
    // Prusa printers are considered first, then 3rd party.
    if (auto prusa = bundles.find(PresetBundle::PRUSA_BUNDLE); prusa != bundles.end())
        preferred_model = get_preferred_printer_model(prusa->first, prusa->second, preferred_variant);
    if (preferred_model.empty()) {
        for (const auto& bundle : bundles) {
            if (bundle.second.is_prusa_bundle) { continue; }
            if (preferred_model = get_preferred_printer_model(bundle.first, bundle.second, preferred_variant);
                !preferred_model.empty())
                    break;
        }
    }

    // if unsaved changes was not cheched till this moment
    if (!check_unsaved_preset_changes) {
        if ((check_unsaved_preset_changes = !preferred_model.empty())) {
            header = _L("A new Printer was installed and it will be activated.");
            if (!wxGetApp().check_and_keep_current_preset_changes(caption, header, act_btns, &apply_keeped_changes))
                return false;
        }
        else if ((check_unsaved_preset_changes = enabled_vendors_old != enabled_vendors)) {
            header = _L("Some Printers were uninstalled.");
            if (!wxGetApp().check_and_keep_current_preset_changes(caption, header, act_btns, &apply_keeped_changes))
                return false;
        }
    }

    std::string first_added_sla_material;
    auto get_first_added_material_preset = [this, app_config](const std::string& section_name, std::string& first_added_preset) {
        if (appconfig_new.has_section(section_name)) {
            // get first of new added preset names
            const std::map<std::string, std::string>& old_presets = app_config->has_section(section_name) ? app_config->get_section(section_name) : std::map<std::string, std::string>();
            first_added_preset = get_first_added_preset(old_presets, appconfig_new.get_section(section_name));
        }
    };
    get_first_added_material_preset(AppConfig::SECTION_MATERIALS, first_added_sla_material);

    // if unsaved changes was not cheched till this moment
    if (!check_unsaved_preset_changes) {
        if ((check_unsaved_preset_changes = !first_added_sla_material.empty())) {
            header = _L("A new SLA material was installed and it will be activated.");
            if (!wxGetApp().check_and_keep_current_preset_changes(caption, header, act_btns, &apply_keeped_changes))
                return false;
        }
        else {
            auto changed = [app_config, &appconfig_new = std::as_const(this->appconfig_new)](const std::string& section_name) {
                if (!appconfig_new.has_section(section_name))
                    return false;
                return (app_config->has_section(section_name) ? app_config->get_section(section_name) : std::map<std::string, std::string>()) != appconfig_new.get_section(section_name);
            };
            bool is_sla_materials_changed = changed(AppConfig::SECTION_MATERIALS);
            if ((check_unsaved_preset_changes = is_sla_materials_changed)) {
                header = _L("Some SLA materials were uninstalled.");
                if (!wxGetApp().check_and_keep_current_preset_changes(caption, header, act_btns, &apply_keeped_changes))
                    return false;
            }
        }
    }

    // Save used repo into manifest.
    std::vector<std::string> used_repo_ids;
    for (const auto& vendor : enabled_vendors) {
        // here vendor might be empty - it causes false has_installed_printers : 1 entries in manifest.
        if (vendor.second.empty()){
            continue;
        }
        bool not_empty = false;
        for (const auto& it : vendor.second) {
            if (!it.second.empty())  {
                not_empty = true;
                break;
            }
        }
        if (!not_empty) {
            continue;
        }

        const auto& it = bundles.find(vendor.first);
        // This is a last resort solution of missing secret repo in manifest while some of its printers are installed.
        if (it == bundles.end()) { 
            continue;
        }
        const std::string repo_id = it->second.vendor_profile->repo_id;
        if (std::find(used_repo_ids.begin(), used_repo_ids.end(), repo_id) == used_repo_ids.end()) {
            used_repo_ids.emplace_back(repo_id);
        }
    }
    wxGetApp().get_preset_updater_wrapper()->set_installed_printer_repositories(std::move(used_repo_ids));

    // apply materials in app_config
    if (appconfig_new.has_section(AppConfig::SECTION_MATERIALS))
        app_config->set_section(AppConfig::SECTION_MATERIALS, appconfig_new.get_section(AppConfig::SECTION_MATERIALS));

    app_config->set_vendors(appconfig_new);

    app_config->set("notify_release", "none");
    app_config->set("preset_update", "0");
    app_config->set("downloader_url_registered", "0");
    app_config->set("view_mode", "expert");
    if (page_preferences) {
        app_config->set("use_inches", page_preferences->use_inches() ? "1" : "0");
        app_config->set("export_sources_full_pathnames", page_preferences->full_pathnames() ? "1" : "0");
    }

#ifdef _WIN32
    app_config->set("associate_3mf", page_files_association->associate_3mf() ? "1" : "0");
    app_config->set("associate_stl", page_files_association->associate_stl() ? "1" : "0");

    if (wxGetApp().is_editor()) {
        if (page_files_association->associate_3mf())
            wxGetApp().associate_3mf_files();
        if (page_files_association->associate_stl())
            wxGetApp().associate_stl_files();
    }
#endif // _WIN32


    if (check_unsaved_preset_changes)
        preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem, 
                                    {preferred_model, preferred_variant, first_added_sla_material});

    // Library profiles and custom printers (queued ones first, the one being edited last) are saved now, and only now.
    // The last one saved stays the active printer.
    std::vector<std::string> custom_profile_names;
    if (any_library || any_custom) {
        if (!check_unsaved_preset_changes &&
            !wxGetApp().check_and_keep_current_preset_changes(caption,
                any_library ? _L("The selected printer profile will be saved and activated.") : _L("Custom printer was installed and it will be activated."),
                act_btns, &apply_keeped_changes))
            return false;
        check_unsaved_preset_changes = true;
        for (QueuedPrinter &item : to_install) {
            if (item.kind == QueuedPrinter::Kind::Library) {
                try {
                    install_library_profile(*preset_bundle, *app_config, item.library_path);
                } catch (const std::exception &error) {
                    wxMessageBox(from_u8(error.what()), _L("Cannot add printer"), wxOK | wxICON_ERROR);
                    return false;
                }
            } else {
                // The material named after the printer is created by load_config_from_wizard.
                DynamicPrintConfig custom_cfg = std::move(item.config);
                copy_bed_model_and_texture_if_needed(custom_cfg);
                preset_bundle->load_config_from_wizard(item.name, std::move(custom_cfg));
                custom_profile_names.push_back(item.name);
            }
        }
    }

    const auto apply_hop_and_tilt_safety = [](Preset &material, double hop_below_fallback, double hop_above_fallback) {
        auto *use_tilt_opt = material.config.option<ConfigOptionBools>("use_tilt");
        auto *hop_opt = material.config.option<ConfigOptionFloats>("tower_hop_height", true);
        if (use_tilt_opt == nullptr || hop_opt == nullptr)
            return;

        if (hop_opt->values.size() < 2)
            hop_opt->values.resize(2, 0.0);

        if (std::abs(hop_opt->get_at(0)) < EPSILON)
            hop_opt->values[0] = hop_below_fallback;
        if (std::abs(hop_opt->get_at(1)) < EPSILON)
            hop_opt->values[1] = hop_above_fallback;

        for (size_t i = 0; i < 2 && i < use_tilt_opt->values.size(); ++i)
            if (!use_tilt_opt->get_at(i) && std::abs(hop_opt->get_at(i)) < EPSILON)
                hop_opt->values[i] = (i == 0) ? hop_below_fallback : hop_above_fallback;
    };

    // Optional own material (path a and c); created only when its toggle is on.
    const bool custom_material_enabled = page_custom_material && page_custom_material->custom_material_profile_enabled() &&
                                         custom_material_step_active();
    if (custom_material_enabled || !custom_profile_names.empty()) {
        if (custom_material_enabled && custom_profile_names.empty() && !check_unsaved_preset_changes &&
            !wxGetApp().check_and_keep_current_preset_changes(caption, _L("A new SLA material was created and it will be activated."), act_btns, &apply_keeped_changes))
            return false;

        const std::string base_material_name = custom_material_enabled ? page_custom_material->base_sla_material_profile_name() : std::string();
        const Preset *base_material = nullptr;
        if (!base_material_name.empty())
            base_material = preset_bundle->sla_materials.find_preset(base_material_name, true);
        if (base_material == nullptr && !custom_profile_names.empty())
            base_material = preset_bundle->sla_materials.find_preset(custom_profile_names.front(), true);
        if (base_material == nullptr)
            base_material = &preset_bundle->sla_materials.default_preset();

        const double hop_below = custom_material_enabled ? std::max(0.0, page_custom_material->custom_material_hop_below()) : 5.0;
        const double hop_above = custom_material_enabled ? std::max(0.0, page_custom_material->custom_material_hop_above()) : 5.0;

        std::vector<std::string> material_names;
        std::string primary_name;
        if (custom_material_enabled) {
            primary_name = page_custom_material->custom_material_profile_name();
            boost::trim(primary_name);
            if (primary_name.empty() && !custom_profile_names.empty())
                primary_name = custom_profile_names.front();
            material_names.emplace_back(primary_name);
        }
        for (const std::string &printer_name : custom_profile_names)
            material_names.emplace_back(printer_name);

        std::set<std::string> unique_material_names;
        std::string primary_material; // first material actually created/updated: becomes the active one
        for (const std::string &material_name_raw : material_names) {
            std::string material_name = material_name_raw;
            boost::trim(material_name);
            if (material_name.empty() || !unique_material_names.insert(material_name).second)
                continue;

            // Exact lookup only: with first_visible_if_not_found=true a new name returned some other visible
            // material, which was then overwritten (exposure, cost, ...) instead of creating the named one.
            Preset *material_ptr = preset_bundle->sla_materials.find_preset(material_name, false);
            if (material_ptr == nullptr && base_material != nullptr) {
                DynamicPrintConfig material_cfg(base_material->config);
                if (auto *id_opt = material_cfg.option<ConfigOptionString>("sla_material_settings_id", true))
                    id_opt->value = material_name;
                material_ptr = &preset_bundle->sla_materials.load_preset(
                    preset_bundle->sla_materials.path_from_name(material_name), material_name, material_cfg, false);
            }

            if (material_ptr == nullptr)
                continue;

            if (auto *id_opt = material_ptr->config.option<ConfigOptionString>("sla_material_settings_id", true))
                id_opt->value = material_name;

            if (custom_material_enabled && material_name == primary_name) {
                material_ptr->config.set_key_value("material_vendor", new ConfigOptionString(page_custom_material->custom_material_vendor()));
                material_ptr->config.set_key_value("material_type", new ConfigOptionString(page_custom_material->custom_material_type()));
                material_ptr->config.set_key_value("material_notes", new ConfigOptionString(page_custom_material->custom_material_notes()));
                material_ptr->config.set_key_value("bottle_cost", new ConfigOptionFloat(std::max(0.0, page_custom_material->custom_material_cost())));
                material_ptr->config.set_key_value("exposure_time", new ConfigOptionFloat(std::max(0.1, page_custom_material->custom_material_exposure_time())));
                material_ptr->config.set_key_value("initial_exposure_time", new ConfigOptionFloat(std::max(0.1, page_custom_material->custom_material_initial_exposure_time())));
                material_ptr->config.set_key_value("tower_hop_height", new ConfigOptionFloats({ hop_below, hop_above }));
            }

            apply_hop_and_tilt_safety(*material_ptr, hop_below, hop_above);
            material_ptr->save();
            if (primary_material.empty())
                primary_material = material_name;
        }
        // load_config_from_wizard() selected a material named after the printer; activate the user's own
        // material instead, otherwise the typed exposure etc. would not be used for slicing.
        if (custom_material_enabled && !primary_material.empty())
            preset_bundle->sla_materials.select_preset_by_name(primary_material, true);
    }

    // Update the selections from the compatibilty.
    preset_bundle->export_selections(*app_config);

    return true;
}
void ConfigWizard::priv::update_presets_in_config(const std::string& section, const std::string& alias_key, bool add)
{
    const PresetAliases& aliases = aliases_sla;

    auto update = [this, add](const std::string& s, const std::string& key) {
    	assert(! s.empty());
        if (add)
            appconfig_new.set(s, key, "1");
        else
            appconfig_new.erase(s, key); 
    };

    // add or delete presets had a same alias 
    auto it = aliases.find(alias_key);
    if (it != aliases.end())
        for (const Preset* preset : it->second)
            update(section, preset->name);
}

bool ConfigWizard::priv::check_sla_selected()
{
    for (const auto page : pages_msla)
        if (page->any_selected())
            return true;

    for (const auto& repos : repositories)
        for (const auto& printers : repos.printers_pages)
            if (const auto page = printers.second;
                page && page->any_selected())
                return true;

    return false;
}

void ConfigWizard::priv::set_config_updated_from_archive()
{
    // Initialize the is_visible flag in printer Presets
    for (auto& pair : bundles)
        pair.second.preset_bundle->load_installed_printers(appconfig_new);

    load_pages_from_archive();
}

static void unselect(PagePrinters* page)
{
    const Slic3r::PresetUpdaterWrapper*    puw              = wxGetApp().get_preset_updater_wrapper();
    const SharedArchiveRepositoryVector&    archs           = puw->get_all_archive_repositories();

    bool unselect_all = true;

    for (const auto* archive : archs) {
        if (page->get_vendor_repo_id() == archive->get_manifest().id) {
            if (puw->is_selected_repository_by_uuid(archive->get_uuid()))
                unselect_all = false;
            //break; ! don't break here, because there can be several archives with same repo_id
        }
    }

    if (unselect_all)
        page->unselect_all_presets();
}

void ConfigWizard::priv::clear_printer_pages()
{
    auto delelete_page = [this](PagePrinters* page) {
        // unselect page to correct process those changes in app_config
        unselect(page);

        // remove page
        hscroll->RemoveChild(page);// Under OSX call of Reparent(nullptr) causes a crash, so as a workaround use RemoveChild() instead
        page->Destroy();
    };

    for (PagePrinters* page : pages_msla)
        delelete_page(page);
    pages_msla.clear();

    // Every printers page is also listed in pages_msla (and deleted above); only the registry is dropped here.
    repositories.clear();
}

void ConfigWizard::priv::load_pages_from_archive()
{
    if (!is_config_from_archive)
        return;

    wxBusyCursor wait; 
    wxWindowUpdateLocker freeze_guard(q);

    // clear vendors and printers pages if any exists
    clear_printer_pages();

    // One step per vendor that ships SLA printer models (Prusa-branded vendors stay hidden). The bundles were already
    // filtered by the selected profile repositories in BundleMap::load().
    std::vector<const VendorProfile*> vendors;
    for (const auto& [id, bundle] : bundles)
        if (bundle.vendor_profile != nullptr)
            vendors.emplace_back(bundle.vendor_profile);
    std::sort(vendors.begin(), vendors.end(), [](const VendorProfile *a, const VendorProfile *b) {
        return boost::algorithm::to_lower_copy(a->name) < boost::algorithm::to_lower_copy(b->name);
    });
    const auto &installed_vendors = appconfig_new.vendors();
    for (const VendorProfile *vendor : vendors) {
        if (!get_repo(vendor->repo_id))
            repositories.push_back({ vendor->repo_id });
        create_vendor_printers_page(vendor->repo_id, vendor, installed_vendors.find(vendor->id) != installed_vendors.end(), true);
    }

    if (!page_custom)
        add_page(page_custom = new PageCustom(q));
    if (!page_profile_printers)
        add_page(page_profile_printers = new PageProfilePrinters(q));
    if (!page_sla_materials)
        add_page(page_sla_materials = new PageMaterials(q, &sla_materials,
            _L("SLA materials"), _L("SLA materials"), _L("Type:")));
    if (!page_custom_material)
        add_page(page_custom_material = new PageCustomMaterial(q));
    if (!page_preferences)
        add_page(page_preferences = new PagePreferences(q));
    if (!page_summary)
        add_page(page_summary = new PageSummary(q));

    page_path->refresh_cards(!pages_msla.empty(), page_profile_printers->has_profiles());
    custom_printer_selected = path() == WizardPath::Custom;
    any_sla_selected = vendor_active() && check_sla_selected();

    check_and_install_missing_materials(T_ANY);
    update_materials(T_ANY);
    if (any_sla_selected)
        page_sla_materials->reload_presets();

    load_pages();
}


// Public

ConfigWizard::ConfigWizard(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _(name()), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , p(new priv(this))
{
#ifdef __APPLE__
    this->SetBackgroundColour(wxGetApp().get_window_default_clr());
#endif
    wxBusyCursor wait;

    this->SetFont(wxGetApp().normal_font());

    p->load_vendors();

    // Build custom-printer config from a complete SLA-capable config so all option groups have required keys.
    DynamicPrintConfig custom_template = build_custom_sla_template(*wxGetApp().preset_bundle);
    const Preset* sample_sla_printer = nullptr;

    const Preset& selected_printer = wxGetApp().preset_bundle->printers.get_selected_preset();
    if (selected_printer.printer_technology() == ptSLA)
        sample_sla_printer = &selected_printer;

    if (!sample_sla_printer) {
        for (const Preset& preset : wxGetApp().preset_bundle->printers)
            if (preset.printer_technology() == ptSLA && preset.is_system) {
                sample_sla_printer = &preset;
                break;
            }
    }

    if (!sample_sla_printer) {
        for (const auto& bundle : p->bundles) {
            for (const Preset& preset : bundle.second.preset_bundle->printers)
                if (preset.printer_technology() == ptSLA) {
                    sample_sla_printer = &preset;
                    break;
                }
            if (sample_sla_printer)
                break;
        }
    }

    if (sample_sla_printer)
        custom_template.apply(sample_sla_printer->config, true);

    custom_template.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    p->custom_config = std::make_unique<DynamicPrintConfig>(std::move(custom_template));

    p->index = new ConfigWizardIndex(this);

    auto *vsizer = new wxBoxSizer(wxVERTICAL);
    auto *topsizer = new wxBoxSizer(wxHORIZONTAL);
    auto* hline = new StaticLine(this);
    p->btnsizer = new wxBoxSizer(wxHORIZONTAL);

    // Initially we _do not_ SetScrollRate in order to figure out the overall width of the Wizard  without scrolling.
    // Later, we compare that to the size of the current screen and set minimum width based on that (see below).
    p->hscroll = new wxScrolledWindow(this);
    p->hscroll_sizer = new wxBoxSizer(wxHORIZONTAL);
    p->hscroll->SetSizer(p->hscroll_sizer);

    topsizer->Add(p->index, 0, wxEXPAND);
    topsizer->AddSpacer(INDEX_MARGIN);
    topsizer->Add(p->hscroll, 1, wxEXPAND);

    p->btn_prev = new wxButton(this, wxID_ANY, _L("< &Back"));
    p->btn_next = new wxButton(this, wxID_ANY, _L("&Next >"));
    p->btn_finish = new wxButton(this, wxID_APPLY, _L("&Finish"));
    p->btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));   // Note: The label needs to be present, otherwise we get accelerator bugs on Mac
    p->btnsizer->AddStretchSpacer();
    p->btnsizer->Add(p->btn_prev, 0, wxLEFT, BTN_SPACING);
    p->btnsizer->Add(p->btn_next, 0, wxLEFT, BTN_SPACING);
    p->btnsizer->Add(p->btn_finish, 0, wxLEFT, BTN_SPACING);
    p->btnsizer->Add(p->btn_cancel, 0, wxLEFT, BTN_SPACING);

    wxGetApp().UpdateDarkUI(p->btn_prev);
    wxGetApp().UpdateDarkUI(p->btn_next);
    wxGetApp().UpdateDarkUI(p->btn_finish);
    wxGetApp().UpdateDarkUI(p->btn_cancel);

    wxGetApp().SetWindowVariantForButton(p->btn_prev);
    wxGetApp().SetWindowVariantForButton(p->btn_next);
    wxGetApp().SetWindowVariantForButton(p->btn_finish);
    wxGetApp().SetWindowVariantForButton(p->btn_cancel);

    p->add_page(p->page_path = new PagePath(this));

    // other pages will be loaded later (run() -> load_pages_from_archive())

    p->load_pages();
    p->index->go_to(size_t{ 0 });
    
#ifdef _WIN32
    p->add_page(p->page_files_association = new PageFilesAssociation(this));
#endif // _WIN32
    p->add_page(p->page_bvolume  = new PageBuildVolume(this));

    vsizer->Add(topsizer, 1, wxEXPAND | wxALL, DIALOG_MARGIN);
    vsizer->Add(hline, 0, wxEXPAND | wxLEFT | wxRIGHT, VERTICAL_SPACING);
    vsizer->Add(p->btnsizer, 0, wxEXPAND | wxALL, DIALOG_MARGIN);

    SetSizer(vsizer);
    SetSizerAndFit(vsizer);

    // We can now enable scrolling on hscroll
    p->hscroll->SetScrollRate(30, 30);

    on_window_geometry(this, [this]() {
        p->init_dialog_size();
    });

    p->btn_prev->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &) { this->p->index->go_prev(); });

    p->btn_prev->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt)
    {
        // nothing to go back to on the first step
        evt.Enable(p->index->active_item() != ConfigWizardIndex::NO_ITEM && p->index->active_item() > 0);
    });

    p->btn_next->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &)
    {
        // The step is validated inline: its message appears on the page and the user stays on it.
        ConfigWizardPage* active_page = this->p->index->active_page();
        if (active_page != nullptr) {
            const wxString error = active_page->validate();
            active_page->show_error(error);
            if (!error.IsEmpty())
                return;
        }
        this->p->index->go_next();
    });

    p->btn_next->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt)
    {
        evt.Enable(p->can_go_next());
    });

    p->btn_finish->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &)
    {
        // Nothing has been written so far; check every step of the chosen path first and show the problem on its page.
        wxString error;
        if (ConfigWizardPage *bad = p->first_invalid_page(error)) {
            p->index->go_to(bad);
            bad->show_error(error);
            return;
        }
        if (p->on_bnt_finish())
            this->EndModal(wxID_OK);
    });

    p->btn_finish->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt)
    {
        evt.Enable(p->can_finish());
    });

    p->index->Bind(EVT_INDEX_PAGE, [this](const wxCommandEvent &)
    {
        p->btn_next->Show(p->can_show_next());

        if (p->index->active_is_last())
            p->btn_finish->SetFocus();
        Layout();
    });

    /* ysFIXME - delete after testing and release
    // it looks like this workaround is no need any more after update of the wxWidgets to 3.2.0
    if (wxLinux_gtk3)
        this->Bind(wxEVT_SHOW, [this, vsizer](const wxShowEvent& e) {
            ConfigWizardPage* active_page = p->index->active_page();
            if (!active_page)
                return;
            for (auto page : p->all_pages)
                if (page != active_page)
                    page->Hide();
            // update best size for the dialog after hiding of the non-active pages
            vsizer->SetSizeHints(this);
            // set initial dialog size
            p->init_dialog_size();
        });
    */
}

ConfigWizard::~ConfigWizard() {}

bool ConfigWizard::run(RunReason reason, StartPage start_page)
{
    BOOST_LOG_TRIVIAL(info) << boost::format("Running ConfigWizard, reason: %1%, start_page: %2%") % reason % start_page;

    GUI_App &app = wxGetApp();

    p->set_run_reason(reason);
    // Offline startup uses the bundled profile library for every run reason,
    // including a fresh datadir. Otherwise there are no pages and UI handlers
    // can mistake two null page pointers for an active profile-library page.
    p->is_config_from_archive = true;
    p->set_config_updated_from_archive();
    // Reloading pages resets the active page; choose the entry point afterwards.
    p->set_start_page(start_page);

    if (ShowModal() == wxID_OK) {
        bool apply_keeped_changes = false;
        if (! p->apply_config(app.app_config, app.preset_bundle, app.get_preset_updater_wrapper(), apply_keeped_changes))
            return false;

        if (apply_keeped_changes)
            app.apply_keeped_preset_modifications();

        app.app_config->set_legacy_datadir(false);
        app.update_mode();
        app.obj_manipul()->update_ui_from_settings();
        BOOST_LOG_TRIVIAL(info) << "ConfigWizard applied";
        return true;
    } else {
        BOOST_LOG_TRIVIAL(info) << "ConfigWizard cancelled";
        return false;
    }
}

const wxString& ConfigWizard::name(const bool from_menu/* = false*/)
{
    // A different naming convention is used for the Wizard on Windows & GTK vs. OSX.
    // Note: Don't call _() macro here.
    //       This function just return the current name according to the OS.
    //       Translation is implemented inside GUI_App::add_config_menu()
#if __APPLE__
    static const wxString config_wizard_name =  L("Configuration Assistant");
    static const wxString config_wizard_name_menu = L("Configuration &Assistant");
#else
    static const wxString config_wizard_name = L("Configuration Wizard");
    static const wxString config_wizard_name_menu = L("Configuration &Wizard");
#endif
    return from_menu ? config_wizard_name_menu : config_wizard_name;
}

void ConfigWizard::on_dpi_changed(const wxRect &suggested_rect)
{
    p->index->msw_rescale();

    const int em = em_unit();

    msw_buttons_rescale(this, em, { wxID_APPLY, 
                                    wxID_CANCEL,
                                    p->btn_next->GetId(),
                                    p->btn_prev->GetId() });

    for (auto page : p->pages_msla)
        for (auto printer_picker : page->printer_pickers)
            msw_buttons_rescale(this, em, printer_picker->get_button_indexes());

    p->init_dialog_size();

    Refresh();
}

void ConfigWizard::on_sys_color_changed()
{
    wxGetApp().UpdateDlgDarkUI(this);
    Refresh();
}

}
}
