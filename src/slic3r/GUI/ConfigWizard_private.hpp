///|/ Copyright (c) Prusa Research 2018 - 2023 David Kocík @kocikdav, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2022 Sebastian Nadorp @snadorp
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ConfigWizard_private_hpp_
#define slic3r_ConfigWizard_private_hpp_

#include "ConfigWizard.hpp"

#include <vector>
#include <set>
#include <unordered_map>
#include <functional>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/panel.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/listbox.h>
#include <wx/checklst.h>
#include <wx/radiobut.h>
#include <wx/html/htmlwin.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/PresetUpdaterWrapper.hpp"
#include "BedShapeDialog.hpp"
#include "GUI.hpp"
#include "SavePresetDialog.hpp"
#include "wxExtensions.hpp"
#include "Widgets/ComboBox.hpp"

#include "Widgets/SpinInput.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

enum {
    WRAP_WIDTH = 500,
    MODEL_MIN_WRAP = 150,

    DIALOG_MARGIN = 15,
    INDEX_MARGIN = 40,
    BTN_SPACING = 10,
    INDENT_SPACING = 30,
    VERTICAL_SPACING = 10,

    MAX_COLS = 4,
    ROW_SPACING = 75,
};



// Configuration data structures extensions needed for the wizard

enum Technology {
    // Bitflag equivalent of PrinterTechnology
    T_SLA = 0x1,
    T_ANY = ~0,
};

enum BundleLocation{
    IN_VENDOR,
    IN_ARCHIVE,
    IN_RESOURCES
};

struct Bundle
{
	std::unique_ptr<PresetBundle> preset_bundle;
	VendorProfile* vendor_profile{ nullptr };
	//bool is_in_resources{ false };
    BundleLocation location;
	bool is_prusa_bundle{ false };

	Bundle() = default;
	Bundle(Bundle&& other);

	// Returns false if not loaded. Reason for that is logged as boost::log error.
	bool load(fs::path source_path, BundleLocation location, bool is_prusa_bundle = false);

	const std::string& vendor_id() const { return vendor_profile->id; }
};

struct BundleMap : std::map<std::string /* = vendor ID */, Bundle>
{
	static BundleMap load();

	Bundle& prusa_bundle();
	const Bundle& prusa_bundle() const;
};

struct Materials;

struct PrinterPickerEvent;

// GUI elements

typedef std::function<bool(const VendorProfile::PrinterModel&)> ModelFilter;

struct PrinterPicker: wxPanel
{
    struct Checkbox : wxCheckBox
    {
        Checkbox(wxWindow *parent, const wxString &label, const std::string &model, const std::string &variant) :
            wxCheckBox(parent, wxID_ANY, label),
            model(model),
            variant(variant)
        {}

        std::string model;
        std::string variant;
    };

    const std::string vendor_id;
    const std::string vendor_repo_id;
    std::vector<Checkbox*> cboxes;
    std::vector<Checkbox*> cboxes_alt;

    PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig, const ModelFilter &filter);
    PrinterPicker(wxWindow *parent, const VendorProfile &vendor, wxString title, size_t max_cols, const AppConfig &appconfig);

    void select_all(bool select, bool alternates = false);
    void select_one(size_t i, bool select);
    // Untick the checkbox of this model/variant (notifies the wizard like a click). False when it is not ticked.
    bool untick(const std::string &model, const std::string &variant);
    bool any_selected() const;
    std::set<std::string> get_selected_models() const ;

    int get_width() const { return width; }
    const std::vector<int>& get_button_indexes() { return m_button_indexes; }

    static const std::string PRINTER_PLACEHOLDER;
private:
    int width;
    std::vector<int> m_button_indexes;

    void on_checkbox(const Checkbox *cbox, bool checked);
};

// A printer queued with "Add another printer" (library profiles and custom printers; vendor models stay ticked on their pages).
struct WizardQueuedPrinter {
    enum class Kind { Library, Custom };
    Kind               kind { Kind::Custom };
    std::string        name;          // display / preset name
    std::string        library_path;  // Library: profile file
    DynamicPrintConfig config;        // Custom: complete printer configuration (base + display + volume)
    std::string        base_name;     // Custom: profile it started from
    std::string        detail;        // Custom/Library: one-line description for the Summary (UTF-8)
};
struct WizardQueueItem { wxString label; std::function<void()> remove; };

// Which way the user adds a printer; decides which steps the wizard index shows.
// None: the user pressed "Add another printer" and has not chosen how to add the next one yet.
enum class WizardPath { Vendor, Library, Custom, None };

// Step 1 of the wizard: WHAT the user wants to set up. It decides which branches (printer / material) the index shows.
enum class WizardGoal { Printer, Material, Both };

// How the user adds a material (material branch). None: "Add another material" was pressed and nothing is chosen yet.
enum class MaterialPath { Ready, Custom, None };

// A material profile of the user's own, queued with "Add another material" (created at Finish).
struct WizardQueuedMaterial {
    std::string name, vendor, type, notes, base_name;
    double cost { 0. }, exposure { 2. }, initial_exposure { 20. }, hop_below { 5. }, hop_above { 5. };
    std::vector<std::string> targets;   // names of the printers it is for (installed, or added in this run)
};

// A printer a new material can be made for: installed already, or added in this wizard run.
struct WizardMaterialTarget {
    std::string name;
    wxString    label;
    bool        is_new { false };
};

struct ConfigWizardPage: wxPanel
{
    ConfigWizard *parent;
    const wxString shortname;
    wxBoxSizer *content;
    const unsigned indent;
    // Shown as "optional" in the index; the wizard never forces the user to touch such a step.
    bool optional_step { false };

    ConfigWizardPage(ConfigWizard *parent, wxString title, wxString shortname, unsigned indent = 0);
    virtual ~ConfigWizardPage();

    template<class T>
    T* append(T *thing, int proportion = 0, int flag = wxEXPAND|wxTOP|wxBOTTOM, int border = 10)
    {
        content->Add(thing, proportion, flag, border);
        return thing;
    }

    wxStaticText* append_text(wxString text);
    // Short grey help paragraph / bold section heading (spacing follows the em unit).
    wxStaticText* append_help(const wxString &text);
    wxStaticText* append_heading(const wxString &text);
    void append_spacer(int space);

    ConfigWizard::priv *wizard_p() const { return parent->p.get(); }

    virtual void apply_custom_config(DynamicPrintConfig &config) {}
    virtual void set_run_reason(ConfigWizard::RunReason run_reason) {}
    virtual void on_activate() {}
    // Empty when the page is complete; otherwise the (inline) reason why the wizard cannot leave it.
    virtual wxString validate() { return wxString(); }

    void set_step_info(size_t step, size_t total);
    void show_error(const wxString &message);
    bool has_error() const;

protected:
    int page_em() const;
    // Scale (<= 1) for widths that were laid out for a 60 em wide page when the screen cannot offer that (large text).
    int fit(int em_count) const;
    // Wrap width of paragraphs, derived from the em unit (about 52 em).
    int wrap_width() const;

private:
    wxStaticText *m_step_label { nullptr };
    wxStaticText *m_error_label { nullptr };
};

struct PagePrinters: ConfigWizardPage
{
    std::vector<PrinterPicker *> printer_pickers;
    Technology technology;
    bool install;

    PagePrinters(ConfigWizard *parent,
        wxString title,
        wxString shortname,
        const VendorProfile &vendor,
        unsigned indent, Technology technology);

    void select_all(bool select, bool alternates = false);
    int get_width() const;
    bool any_selected() const;
    std::set<std::string> get_selected_models();

    std::string get_vendor_id() const { return printer_pickers.empty() ? "" : printer_pickers[0]->vendor_id; }
    std::string get_vendor_repo_id() const { return printer_pickers.empty() ? "" : printer_pickers[0]->vendor_repo_id; }

    // unselect all printers in appconfig_new and bundles
    void unselect_all_presets();

    virtual void set_run_reason(ConfigWizard::RunReason run_reason) override;
    virtual wxString validate() override;
    // Names of the models ticked on this page ("Photon Mono (default)" style).
    std::vector<std::string> selected_model_names();
    struct TickedModel { std::string label, vendor_id, model_id, variant; };
    std::vector<TickedModel> ticked_models() const;
    bool untick_model(const TickedModel &model);
    const VendorProfile *vendor_profile { nullptr };

    bool has_printers { false };
    bool is_primary_printer_page { false };
};

// Here we extend wxListBox and wxCheckListBox
// to make the client data API much easier to use.
template<class T, class D> struct DataList : public T
{
    DataList(wxWindow *parent) : T(parent, wxID_ANY) {}
	DataList(wxWindow* parent, int style) : T(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, NULL, style) {}

    // Note: We're _not_ using wxLB_SORT here because it doesn't do the right thing,
    // eg. "ABS" is sorted before "(All)"

    int append(const std::string &label, const D *data) {
        void *ptr = reinterpret_cast<void*>(const_cast<D*>(data));
        return this->Append(from_u8(label), ptr);
    }

    int append(const wxString &label, const D *data) {
        void *ptr = reinterpret_cast<void*>(const_cast<D*>(data));
        return this->Append(label, ptr);
    }

    const D& get_data(int n) {
        return *reinterpret_cast<const D*>(this->GetClientData(n));
    }

    int find(const D &data) {
        for (unsigned i = 0; i < this->GetCount(); i++) {
            if (get_data(i) == data) { return i; }
        }

        return wxNOT_FOUND;
    }

    int size() { return this->GetCount(); }

    void on_mouse_move(const wxPoint& position) {
        int item = T::HitTest(position);
       
        if(item == wxHitTest::wxHT_WINDOW_INSIDE)
            BOOST_LOG_TRIVIAL(error) << "hit test wxHT_WINDOW_INSIDE";
        else if (item == wxHitTest::wxHT_WINDOW_OUTSIDE)
            BOOST_LOG_TRIVIAL(error) << "hit test wxHT_WINDOW_OUTSIDE";
        else if(item == wxHitTest::wxHT_WINDOW_CORNER)
            BOOST_LOG_TRIVIAL(error) << "hit test wxHT_WINDOW_CORNER";
        else if (item == wxHitTest::wxHT_WINDOW_VERT_SCROLLBAR)
            BOOST_LOG_TRIVIAL(error) << "hit test wxHT_WINDOW_VERT_SCROLLBAR";
       else if (item == wxHitTest::wxHT_NOWHERE)
            BOOST_LOG_TRIVIAL(error) << "hit test wxHT_NOWHERE";
       else if (item == wxHitTest::wxHT_MAX)
            BOOST_LOG_TRIVIAL(error) << "hit test wxHT_MAX";
       else
            BOOST_LOG_TRIVIAL(error) << "hit test: " << item;
    }
};

typedef DataList<wxListBox, std::string> StringList;
typedef DataList<wxCheckListBox, std::string> PresetList;

struct ProfilePrintData
{
    std::reference_wrapper<const std::string> name;
    bool omnipresent;
    bool checked;
    ProfilePrintData(const std::string& n, bool o, bool c) : name(n), omnipresent(o), checked(c) {}
};

struct PageMaterials: ConfigWizardPage
{
    Materials *materials;
    StringList *list_printer, *list_type, *list_vendor;
    PresetList *list_profile;
    wxArrayInt sel_printers_prev;
    int sel_type_prev, sel_vendor_prev;
    bool presets_loaded;

    wxFlexGridSizer *grid;
    wxHtmlWindow* html_window;

    int compatible_printers_width = { 100 };
    std::string empty_printers_label;
    bool first_paint = { false };
    static const std::string EMPTY;
    int last_hovered_item = { -1 } ;

    PageMaterials(ConfigWizard *parent, Materials *materials, wxString title, wxString shortname, wxString list1name);

    void check_and_update_presets(bool force_reload_presets = false);
    void reload_presets();
	void update_lists(int sel_type, int sel_vendor, int last_selected_printer = -1);
	void on_material_highlighted(int sel_material);
    void on_material_hovered(int sel_material);
    void select_material(int i);
    void select_all(bool select);
    void clear();
    void set_compatible_printers_html_window(const std::vector<std::string>& printer_names, bool all_printers = false);
    void clear_compatible_printers_label();

    void sort_list_data(StringList* list, bool add_All_item, bool material_type_ordering);
    void sort_list_data(PresetList* list, const std::vector<ProfilePrintData>& data);

    void on_paint();
    void on_mouse_move_on_profiles(wxMouseEvent& evt);
    void on_mouse_enter_profiles(wxMouseEvent& evt);
    void on_mouse_leave_profiles(wxMouseEvent& evt);
    virtual void on_activate() override;
    virtual wxString validate() override;
};

struct Materials
{
    Technology technology;
    // use vector for the presets to purpose of save of presets sorting in the bundle
    std::vector<const Preset*> presets;
    // String is alias of material, set is set of compatible printers
    std::map<std::string, std::set<const Preset*>> compatibility_counter;
    std::set<std::string> types;
    std::set<const Preset*> printers;

    Materials(Technology technology) : technology(technology) {}

    void push(const Preset* preset);
    void add_printer(const Preset* preset);
    void clear();
    bool containts(const Preset* preset) const {
        //return std::find(presets.begin(), presets.end(), preset) != presets.end(); 
        return std::find_if(presets.begin(), presets.end(),
            [preset](const Preset* element) { return element == preset; }) != presets.end();

    }

    bool get_omnipresent(const Preset* preset) {
        return get_printer_counter(preset) == printers.size();
    }

    const std::vector<const Preset*> get_presets_by_alias(const std::string name) {
        std::vector<const Preset*> ret_vec;
        for (auto it = presets.begin(); it != presets.end(); ++it) {
            if ((*it)->alias == name)
                ret_vec.push_back((*it));
        }
        return ret_vec;
    }



    size_t get_printer_counter(const Preset* preset) {
        for (auto it : compatibility_counter) {
            if (it.first == preset->alias)
                return it.second.size();
        }
        return 0;
    }

    const std::string& appconfig_section() const;
    const std::string& get_type(const Preset* preset) const;
    const std::string& get_vendor(const Preset* preset) const;

    template<class F> void filter_presets(const Preset* printer, const std::string& printer_name, const std::string& type, const std::string& vendor, F cb) {
        for (auto preset : presets) {
            const Preset& prst = *(preset);
            const Preset& prntr = *printer;
            if (((printer == nullptr && printer_name == PageMaterials::EMPTY) || (printer != nullptr && is_compatible_with_printer(PresetWithVendorProfile(prst, prst.vendor), PresetWithVendorProfile(prntr, prntr.vendor)))) &&
                (type.empty() || get_type(preset) == type) &&
                (vendor.empty() || get_vendor(preset) == vendor) &&
                prst.vendor) {

                cb(preset);
            }
        }
    }

    static const std::string UNKNOWN;
    static const std::string& get_material_type(const Preset* preset);
    static const std::string& get_material_vendor(const Preset* preset);
};

// Step 1: one question, three large choice cards. The radio buttons are siblings of one panel (one native radio
// group); the card frames are painted by the panel behind them.
struct PathChoice : wxPanel
{
    PathChoice(wxWindow *parent);

    size_t add_card(const wxString &title, const wxString &description, const std::string &icon_name);
    void   set_selected(size_t index);
    size_t selected() const { return m_selected; }
    void   set_available(size_t index, bool available, const wxString &description_when_unavailable = wxString());
    // Text wrap width of the card descriptions in pixels (narrow screens / large text).
    void   set_wrap_width(int pixels);
    // Called when the user picks a card.
    std::function<void(size_t)> on_pick;

private:
    struct Card {
        wxRadioButton  *radio { nullptr };
        wxStaticBitmap *icon { nullptr };
        wxStaticText   *description { nullptr };
        wxBoxSizer     *row { nullptr };
        wxString        text;
        bool            available { true };
    };
    std::vector<Card> m_cards;
    size_t            m_selected { 0 };
    wxRadioButton    *m_none { nullptr };
    int               m_wrap { 0 };
    wxBoxSizer       *m_sizer { nullptr };
    void              pick(size_t index);
    void              on_paint(wxPaintEvent &);
};

// Step 1: "What would you like to set up?" - a printer, a material or both.
struct PageGoal: ConfigWizardPage
{
    PageGoal(ConfigWizard *parent);

    WizardGoal goal() const { return m_goal; }
    void set_goal(WizardGoal goal, bool notify = true);
    // "A material" needs an installed printer: without one the card is disabled and says why.
    void refresh_cards(bool printers_installed);
    virtual void set_run_reason(ConfigWizard::RunReason run_reason) override;

private:
    PathChoice   *m_choice { nullptr };
    WizardGoal    m_goal { WizardGoal::Printer };
    wxStaticText *m_welcome { nullptr };
};

struct PagePath: ConfigWizardPage
{
    PagePath(ConfigWizard *parent);

    WizardPath path() const { return m_path; }
    void set_path(WizardPath path, bool notify = true);
    // Which cards can be used (vendor profiles / library profiles present) and the first-run default.
    void refresh_cards(bool vendors_available, bool library_available);
    // Back to the first usable card (after the last queued printer was removed).
    void restore_default();
    virtual void on_activate() override { refresh_queue(); }
    // The compact "Queued: ..." line (one removable chip per queued printer); hidden while nothing is queued.
    void refresh_queue();

private:
    PathChoice   *m_choice { nullptr };
    wxPanel      *m_queue { nullptr };
    WizardPath    m_path { WizardPath::Vendor };
    bool          m_initialised { false };
    bool          m_vendors_available { false };
    bool          m_library_available { false };
};

// Path (b): community library of printer profiles. Selecting only MARKS a profile; it is saved at Finish.
// Saves a library profile file as a new (uniquely named) printer preset and selects it. Throws on failure.
// Returns the name the profile was saved under.
std::string install_library_profile(PresetBundle &preset_bundle, AppConfig &app_config, const std::string &profile_path);

struct PageProfilePrinters: ConfigWizardPage
{
    PageProfilePrinters(ConfigWizard *parent);

    bool any_selected() const;
    std::string get_selected_profile_path() const;
    std::string get_selected_profile_name() const;
    DynamicPrintConfig get_selected_config() const;
    // Save the marked profile as a new printer preset (unique name) and select it. Throws on failure.
    void install_selected_profile(PresetBundle &preset_bundle, AppConfig &app_config) const;
    virtual wxString validate() override;
    bool has_profiles() const { return !profiles.empty(); }
    // Forget the marked profile (after it was moved to the queue of printers).
    void clear_selection();

private:
    wxListBox *profile_list = nullptr;
    wxTextCtrl *filter = nullptr;
    wxStaticText *preview_title = nullptr;
    wxStaticText *preview_body = nullptr;
    wxStaticText *preview_note = nullptr;
    std::vector<std::pair<std::string, std::string>> profiles; // {name, full path}
    std::vector<size_t> shown;                                 // indices into profiles for the visible list rows
    std::string profiles_dir;
    std::string selected_path;                                  // kept across filtering

    void load_profiles();
    void fill_list();
    void on_profile_selected(wxCommandEvent &event);
    void update_preview();
};

// Path (c), step 1: name and base profile of the custom printer.
struct PageCustom: ConfigWizardPage
{
    PageCustom(ConfigWizard *parent);
    ~PageCustom() {
        if (profile_name_editor) 
            delete profile_name_editor;
    }

    bool        is_valid_profile_name() const { return profile_name_editor->is_valid();}
    std::string profile_name()          const { return profile_name_editor->preset_name(); }
    std::string sample_sla_profile_name() const;
    virtual wxString validate() override;
    // After this printer was queued: a fresh unique name for the next one (the base profile is kept).
    void prepare_next();

private:
    static const char* default_profile_name;

    wxListBox               *sample_sla_choice {nullptr};
    std::vector<std::string> sample_sla_profile_names;
    SavePresetDialog::Item  *profile_name_editor {nullptr};
};

// Material branch, first step: ready-made vendor profiles or an own material (guided, three steps).
struct PageMaterialPath: ConfigWizardPage
{
    PageMaterialPath(ConfigWizard *parent);

    MaterialPath path() const { return m_path; }
    void set_path(MaterialPath path, bool notify = true);
    // Ready-made profiles exist for vendor printers (installed or added in this run) only.
    void refresh_cards(bool ready_available);
    bool ready_available() const { return m_ready_available; }
    virtual void on_activate() override;
    // The "Queued:" line: one removable chip per material queued with "Add another material".
    void refresh_queue();

private:
    PathChoice   *m_choice { nullptr };
    wxPanel      *m_queue { nullptr };
    MaterialPath  m_path { MaterialPath::Ready };
    bool          m_ready_available { false };
    bool          m_user_chose { false };
};

// Own material, step 1: name, brand, type, bottle cost, notes.
struct PageMaterialBasics: ConfigWizardPage
{
    PageMaterialBasics(ConfigWizard *parent);

    std::string name() const;
    std::string vendor() const;
    std::string type() const;
    std::string notes() const;
    double      cost() const;
    // Brand, type, cost and notes follow the base profile unless the user typed them.
    void        load_defaults(const Preset &base);
    // After the material was queued: a fresh unique name for the next one.
    void        prepare_next();
    virtual wxString validate() override;
    virtual void on_activate() override;
    void        refresh_inline_error();

private:
    wxTextCtrl *name_input { nullptr };
    wxTextCtrl *vendor_input { nullptr };
    wxTextCtrl *type_input { nullptr };
    wxTextCtrl *cost_input { nullptr };
    wxTextCtrl *notes_input { nullptr };
    bool        m_touched[4] { false, false, false, false };   // brand, type, cost, notes
};

// Own material, step 2: the printers it is for and the profile it starts from.
struct PageMaterialBase: ConfigWizardPage
{
    PageMaterialBase(ConfigWizard *parent);

    std::string              base_name() const;
    // Printers ticked here that still exist in this run (installed, or added and not removed).
    std::vector<std::string> selected_targets() const;
    // Forget what was ticked (after the material was queued).
    void                     reset_targets();
    virtual wxString validate() override;
    virtual void on_activate() override { rebuild(); }
    void rebuild();
    // The first visit of any own-material step fills in the form from the default base profile.
    void ensure_built() { if (!m_built) rebuild(); }

private:
    bool                     m_built { false };
    wxPanel                 *m_targets { nullptr };
    std::map<std::string, bool> m_state;
    wxListBox               *base_choice { nullptr };
    std::vector<std::string> base_names;
    bool                     is_checked(const WizardMaterialTarget &target, const std::vector<WizardMaterialTarget> &all) const;
};

// Own material, step 3: exposure, initial exposure and hop heights.
struct PageMaterialExposure: ConfigWizardPage
{
    PageMaterialExposure(ConfigWizard *parent);

    double exposure() const;
    double initial_exposure() const;
    double hop_below() const;
    double hop_above() const;
    void   load_defaults(const Preset &base);
    virtual wxString validate() override;
    virtual void on_activate() override;
    void   refresh_inline_error();

private:
    wxTextCtrl *exposure_input { nullptr };
    wxTextCtrl *initial_exposure_input { nullptr };
    wxTextCtrl *hop_below_input { nullptr };
    wxTextCtrl *hop_above_input { nullptr };
};

// Optional step: a few application preferences. The wizard keeps the app offline (no update / downloader options).
struct PagePreferences: ConfigWizardPage
{
    PagePreferences(ConfigWizard *parent);
    bool use_inches() const { return check_inch->GetValue(); }
    bool full_pathnames() const { return check_pathnames->GetValue(); }
private:
    wxCheckBox *check_inch { nullptr };
    wxCheckBox *check_pathnames { nullptr };
};

// Last step: exactly what Finish will do. Nothing is written before it.
struct PageSummary: ConfigWizardPage
{
    PageSummary(ConfigWizard *parent);
    virtual void on_activate() override;
    void rebuild();
private:
    wxPanel *holder { nullptr };
};

#ifdef _WIN32
struct PageFilesAssociation : ConfigWizardPage
{
private:
    wxCheckBox* cb_3mf{ nullptr };
    wxCheckBox* cb_stl{ nullptr };

public:
    PageFilesAssociation(ConfigWizard* parent);

    bool associate_3mf() const { return cb_3mf->IsChecked(); }
    bool associate_stl() const { return cb_stl->IsChecked(); }
};
#endif // _WIN32

// Path (c), step 2: display / build volume of the custom printer (validated inline while typing).
struct PageBuildVolume : ConfigWizardPage
{
    wxTextCtrl* build_volume;
    wxTextCtrl* display_width;
    wxTextCtrl* display_height;
    wxTextCtrl* display_pixels_x;
    wxTextCtrl* display_pixels_y;
    ComboBox*   display_orientation;
    wxCheckBox* display_mirror_x;
    wxCheckBox* display_mirror_y;
    wxCheckBox* use_tilt;

    PageBuildVolume(ConfigWizard* parent);
    void load_config(const DynamicPrintConfig &config);
    virtual void apply_custom_config(DynamicPrintConfig& config);
    virtual wxString validate() override;
    virtual void on_activate() override;
    void refresh_inline_error();
};

typedef std::map<std::string /* = vendor ID */, PagePrinters* /* = SLA printers page */> Pages3rdparty;


class ConfigWizardIndex: public wxPanel
{
public:
    ConfigWizardIndex(wxWindow *parent);

    void add_page(ConfigWizardPage *page);

    size_t active_item() const { return item_active; }
    ConfigWizardPage* active_page() const;
    bool active_is_last() const { return item_active < items.size() && item_active == last_page; }
    size_t pages_cnt()    const { return items.size(); }

    void go_prev();
    void go_next();
    void go_to(size_t i);
    void go_to(const ConfigWizardPage *page);

    void clear();
    void msw_rescale();

    int em() const { return em_w; }
    size_t pages_total() const { return items.size(); }
    void update_steps();
    std::vector<ConfigWizardPage*> pages() const;

    static const size_t NO_ITEM = size_t(-1);
private:
    struct Item
    {
        wxString label;
        unsigned indent;
        bool optional;
        ConfigWizardPage *page;

        bool operator==(ConfigWizardPage *page) const { return this->page == page; }
    };

    int em_w;
    int em_h;
    ScalableBitmap bg;

    std::vector<Item> items;
    size_t item_active;
    ssize_t item_hover;
    size_t last_page;

    int item_height() const { return em_h * 5 / 2; }

    void on_paint(wxPaintEvent &evt);
    void on_mouse_move(wxMouseEvent &evt);
    void on_mouse_left_up(wxMouseEvent &evt);
    void on_mouse_leave(wxMouseEvent &evt);
};

wxDEFINE_EVENT(EVT_INDEX_PAGE, wxCommandEvent);



// ConfigWizard private data

typedef std::map<std::string, std::set<const Preset*>> PresetAliases;

struct ConfigWizard::priv
{
    ConfigWizard *q;
    ConfigWizard::RunReason run_reason = RR_USER;
    AppConfig appconfig_new;      // Backing for vendor/model/variant and material selections in the GUI
    BundleMap bundles;            // Holds all loaded config bundles, the key is the vendor names.
                                  // Materials refers to Presets in those bundles by pointers.
                                  // Also we update the is_visible flag in printer Presets according to the
                                  // PrinterPickers state.
    Materials sla_materials;      // Ditto for SLA materials
    PresetAliases aliases_sla;    // Map of alias to material presets
    std::unique_ptr<DynamicPrintConfig> custom_config;           // Backing for custom printer definition
    bool any_sla_selected { false };        // A vendor printer model is ticked (only meaningful while vendor printers count)
    // Printers queued with "Add another printer" (library profiles and custom printers). Vendor models stay ticked on
    // their pages; vendor_queued says that those ticks count even when another path is being edited.
    using QueuedPrinter = WizardQueuedPrinter;
    using QueueItem = WizardQueueItem;
    std::vector<QueuedPrinter> queue;
    // Own materials queued with "Add another material" (created at Finish).
    std::vector<WizardQueuedMaterial> material_queue;
    bool vendor_queued { false };
    bool custom_printer_selected { false }; // The custom path is chosen
    bool custom_printer_in_bundle { false }; // Older custom printer already exists when wizard starts

    wxScrolledWindow *hscroll = nullptr;
    wxBoxSizer *hscroll_sizer = nullptr;
    wxBoxSizer *btnsizer = nullptr;
    ConfigWizardPage *page_current = nullptr;
    ConfigWizardIndex *index = nullptr;
    wxButton *btn_prev = nullptr;
    wxButton *btn_next = nullptr;
    wxButton *btn_finish = nullptr;
    wxButton *btn_cancel = nullptr;

    PageMaterials    *page_sla_materials = nullptr;
    PageProfilePrinters *page_profile_printers = nullptr;
    PageCustom       *page_custom = nullptr;
    PagePath         *page_path = nullptr;
    PageGoal         *page_goal = nullptr;
    PageMaterialPath *page_material_path = nullptr;
    PageMaterialBasics *page_material_basics = nullptr;
    PageMaterialBase *page_material_base = nullptr;
    PageMaterialExposure *page_material_exposure = nullptr;
    PagePreferences  *page_preferences = nullptr;
    PageSummary      *page_summary = nullptr;
#ifdef _WIN32
    PageFilesAssociation* page_files_association = nullptr;
#endif // _WIN32

    // Custom setup pages
    PageBuildVolume* page_bvolume = nullptr;

    std::vector<PagePrinters*>  pages_msla;

    struct Repository {
        bool operator==(const std::string& other_id_name) const { return other_id_name == this->id_name; }

        std::string     id_name;
        Pages3rdparty   printers_pages;
    };
    std::vector<Repository>     repositories;


    bool                        is_config_from_archive{ false };

    // Pointers to all pages (regardless or whether currently part of the ConfigWizardIndex)
    std::vector<ConfigWizardPage*> all_pages;

    priv(ConfigWizard *q)
        : q(q)
        , appconfig_new(AppConfig::EAppMode::Editor)
        , sla_materials(T_SLA)
    {}

    void load_pages();
    void init_dialog_size();
    // Width in pixels the page area of the wizard can use on the screen it opens on (the window is capped at 90% of it).
    int content_width() const;

    void load_vendors();
    void add_page(ConfigWizardPage *page);
    void enable_next(bool enable);
    void set_start_page(ConfigWizard::StartPage start_page);
    void create_vendor_printers_page(const std::string& repo_id, const VendorProfile* vendor, bool install = false, bool from_single_vendor_repo = false);
    void set_run_reason(RunReason run_reason);
    void update_materials(Technology technology);

    WizardGoal goal() const { return page_goal ? page_goal->goal() : WizardGoal::Printer; }
    bool goal_printer() const { return goal() != WizardGoal::Material; }
    bool goal_material() const { return goal() != WizardGoal::Printer; }
    // The way the printer is added; None while the printer branch is not part of the wizard.
    WizardPath path() const { return goal_printer() && page_path ? page_path->path() : WizardPath::None; }
    MaterialPath material_path() const { return goal_material() && page_material_path ? page_material_path->path() : MaterialPath::None; }
    // Printers queued for this run (nothing while the wizard only sets up a material).
    const std::vector<QueuedPrinter>& printer_queue() const;
    // Vendor ticks are installed at Finish (path Vendor, or ticks queued by "Add another printer").
    bool vendor_active() const { return path() == WizardPath::Vendor || (goal_printer() && vendor_queued); }
    bool queue_has(QueuedPrinter::Kind kind) const;
    // A vendor printer (bundle id + preset) that is installed or will be installed by this run.
    bool vendor_printer_counts(const std::string &vendor_id, const Preset &printer) const;
    bool ready_made_available() const;
    bool has_installed_printers() const;
    // Printers a new material can be made for: installed, ticked vendor models, queued and current printers.
    std::vector<WizardMaterialTarget> material_targets() const;
    std::set<std::string> queued_material_names() const;
    const Preset* find_material_base(const std::string &name) const;
    void load_material_defaults(const std::string &base_name);
    WizardQueuedMaterial make_material_entry() const;
    std::vector<QueueItem> material_queue_items();
    void add_another_material();
    void material_queue_changed();
    void on_goal_changed();
    void on_material_path_changed();
    // Rebuild the list of ready-made material profiles after the printers in play changed.
    void invalidate_materials();
    // any_sla_selected: a vendor printer counts (ticked, or installed while only a material is set up).
    void refresh_sla_selected();
    // Materials ticked in this run that are not installed yet; with only_in_play, only those that fit a printer that counts.
    std::vector<std::string> new_ticked_materials(bool only_in_play) const;
    // Vendor models that are new (not installed yet) and queued, followed by queued library / custom printers.
    std::vector<QueueItem> queue_items();
    // Complete configuration of the custom printer typed on the Custom printer / Display steps.
    QueuedPrinter make_custom_entry(PresetBundle &preset_bundle) const;
    QueuedPrinter make_library_entry() const;
    std::set<std::string> queued_custom_names() const;
    // Summary action: keep everything chosen so far queued and go back to "Your printer".
    void add_another_printer();
    void queue_changed();
    // True when the vendor ticks differ from what is installed now.
    bool vendors_changed() const;
    void on_path_changed();
    // Whole-wizard validation of the chosen path; returns the failing page (and its message) or nullptr.
    ConfigWizardPage* first_invalid_page(wxString &message);
    // Models of installed/ticked vendor printers that lack a compatible selected material.
    std::set<const VendorProfile::PrinterModel*> printer_models_missing_materials(const std::string &only_for_model_id = std::string());
    bool existing_printers_kept() const;
    void on_printer_pick(PagePrinters *page, const PrinterPickerEvent &evt);
    void select_default_materials_for_printer_model(const VendorProfile::PrinterModel &printer_model, Technology technology);
    void select_default_materials_for_printer_models(Technology technology, const std::set<const VendorProfile::PrinterModel*> &printer_models);

    bool can_finish();
    bool can_go_next();
    bool can_show_next();
    bool on_bnt_finish();
    bool check_and_install_missing_materials(Technology technology, const std::string &only_for_model_id = std::string());
    bool apply_config(AppConfig *app_config, PresetBundle *preset_bundle, const PresetUpdaterWrapper *updater, bool& apply_keeped_changes);
    // #ys_FIXME_alise
    void update_presets_in_config(const std::string& section, const std::string& alias_key, bool add);
//#ifdef __linux__
//    void perform_desktop_integration() const;
//#endif
    bool check_sla_selected();        // Used to decide whether to display SLA Materials page

    int em() const { return index->em(); }
    void set_config_updated_from_archive();

    Repository* get_repo(const std::string& repo_id);

    void clear_printer_pages();
    void load_pages_from_archive();
};

}
}

#endif
