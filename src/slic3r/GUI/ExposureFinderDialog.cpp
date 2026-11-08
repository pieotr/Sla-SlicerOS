// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExposureFinderDialog.hpp"
#include "RasterGeneratorTask.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/ExposureFinderDraft.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/grid.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <sstream>

namespace Slic3r::GUI {
namespace {
std::string text(const wxString &s) { return std::string(s.ToUTF8().data()); }
std::string number(double value) {
    // Shortest locale-independent spelling that round-trips to the same double.
    // Keep defaults readable without rounding authored values to a UI precision.
    char buffer[128];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{})
        throw RuntimeError("Could not format an exposure finder number");
    return std::string(buffer, result.ptr);
}
wxString row_validity(const std::array<wxString,4> &values) {
    std::string csv;
    for(size_t col=0;col<values.size();++col){if(col)csv+=',';csv+=text(values[col]);}
    try {
        const auto requested=sla::parse_exposure_finder_manual_rows(csv);
        if(requested.size()==1 && requested.front().valid())
            return sla::normalize_exposure_finder_table(requested)==requested?"Valid":"Needs normalization";
    }catch(const std::exception &){}
    return "Invalid";
}
class FieldValidationError final : public RuntimeError {
public:
    const wxWeakRef<wxTextCtrl> field;
    FieldValidationError(wxTextCtrl *input, const std::string &detail)
        : RuntimeError(text(input->GetName())+": "+detail), field(input) {}
};
// Retain raw text until an explicit action. In particular no rounding to a
// spin control's display precision and no clearing inactive settings.
class ExposureFinderForm final : public wxDialog {
    std::map<std::string, wxTextCtrl*> m_text;
    std::map<std::string, wxCheckBox*> m_check;
    std::map<std::string, wxChoice*> m_choice;
    wxGrid *m_rows = nullptr;
    wxSpinCtrl *m_selected_row = nullptr;
    wxButton *m_edit_row = nullptr;
    wxTextCtrl *m_summary = nullptr;
    wxButton *m_generate = nullptr;
    wxNotebook *m_notebook = nullptr;
    bool m_populating = false;
    bool m_syncing_rows = false;
    std::optional<sla::ExposureFinderGenerationParameters> m_planned_request;
    std::optional<DynamicPrintConfig> m_planned_config;
    std::string m_planned_fields, m_generated_table_fields;

    void update_enabled() {
        auto on=[&](const char *key){const auto found=m_check.find(key);return found!=m_check.end() && found->second->GetValue();};
        auto enable=[&](const char *key,bool value){const auto field=m_text.find(key);if(field!=m_text.end())field->second->Enable(value);const auto check=m_check.find(key);if(check!=m_check.end())check->second->Enable(value);};
        enable("maximum_layer_height",on("multiple_layer_heights"));enable("layer_height_step",on("multiple_layer_heights"));
        enable("brightness",on("multiple_brightness"));
        const auto base=m_choice.find("base_mode");enable("custom_base_exposure",base!=m_choice.end() && base->second->GetSelection()==4);
        enable("repeated_lift_enabled",on("repeated_settings"));enable("repeated_wait_enabled",on("repeated_settings"));
        enable("repeated_lift_height",on("repeated_settings") && on("repeated_lift_enabled"));
        enable("repeated_wait_before",on("repeated_settings") && on("repeated_wait_enabled"));
    }
    std::string table_fingerprint() const {
        std::ostringstream out;
        for(const char *key:{"layer_height","maximum_layer_height","layer_height_step","bottom_exposure","normal_exposure","bottom_step","normal_step","tests"}){const auto value=text(m_text.at(key)->GetValue());out << value.size() << ':' << value;}
        out << checked("multiple_layer_heights") << checked("include_base") << m_choice.at("series")->GetSelection();return out.str();
    }
    void invalidate() {
        update_enabled();
        m_planned_request.reset(); m_planned_config.reset();
        if (m_generate) m_generate->Disable();
        if (m_summary) m_summary->ChangeValue("Settings changed. Preview plan before generating.");
    }
    wxFlexGridSizer *page(const char *name, wxBoxSizer **outer = nullptr) {
        auto *panel = new wxScrolledWindow(m_notebook);
        panel->SetScrollRate(12,12);
        auto *layout = new wxBoxSizer(wxVERTICAL);
        auto *fields = new wxFlexGridSizer(2,6,10); fields->AddGrowableCol(1);
        layout->Add(fields,0,wxEXPAND|wxALL,10); panel->SetSizer(layout);
        m_notebook->AddPage(panel, name); if (outer) *outer=layout;
        return fields;
    }
    wxWindow *owner(wxSizer *sizer) { return sizer->GetContainingWindow(); }
    void entry(wxFlexGridSizer *sizer, const char *key, const char *label, const std::string &value) {
        auto *parent=owner(sizer);
        sizer->Add(new wxStaticText(parent,wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
        auto *field=new wxTextCtrl(parent,wxID_ANY,wxString::FromUTF8(value));
        field->SetName(label); field->SetToolTip(label); m_text[key]=field;
        sizer->Add(field,1,wxEXPAND);
        field->Bind(wxEVT_TEXT,[this](wxCommandEvent &){ if(!m_populating) invalidate(); });
    }
    void numeric(wxFlexGridSizer *sizer,const char *key,const char *label,double value) { entry(sizer,key,label,number(value)); }
    void boolean(wxFlexGridSizer *sizer,const char *key,const char *label,bool value) {
        sizer->Add(new wxStaticText(owner(sizer),wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
        auto *field=new wxCheckBox(owner(sizer),wxID_ANY,label);field->SetName(label);field->SetValue(value);
        m_check[key]=field;sizer->Add(field,1,wxEXPAND);
        field->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent &){invalidate();});
    }
    void choice(wxFlexGridSizer *sizer,const char *key,const char *label,std::initializer_list<const char*> values,int selected) {
        sizer->Add(new wxStaticText(owner(sizer),wxID_ANY,label),0,wxALIGN_CENTER_VERTICAL);
        auto *field=new wxChoice(owner(sizer),wxID_ANY);for(const auto *value:values)field->Append(value);
        field->SetSelection(selected);field->SetName(label);m_choice[key]=field;sizer->Add(field,1,wxEXPAND);
        field->Bind(wxEVT_CHOICE,[this](wxCommandEvent &){invalidate();});
    }
    double real(const char *key) {
        auto *field=m_text.at(key);std::istringstream input(text(field->GetValue()));input.imbue(std::locale::classic());
        double value;input >> value;
        if(!input || !std::isfinite(value)) throw FieldValidationError(field,"enter a finite number");
        input >> std::ws;if(!input.eof())throw FieldValidationError(field,"unexpected characters");
        return value;
    }
    unsigned integer(const char *key) {
        const double value=real(key);
        if(value<0 || value>std::numeric_limits<unsigned>::max() || std::floor(value)!=value)
            throw FieldValidationError(m_text.at(key),"enter a nonnegative whole number");
        return unsigned(value);
    }
    int signed_integer(const char *key) {
        const double value=real(key);
        if(value<std::numeric_limits<int>::min() || value>std::numeric_limits<int>::max() || std::floor(value)!=value)
            throw FieldValidationError(m_text.at(key),"enter a whole number in the supported range");
        return int(value);
    }
    bool checked(const char *key) const {return m_check.at(key)->GetValue();}
    void commit_grid_editor() { if(m_rows->IsCellEditControlEnabled()) {m_rows->SaveEditControlValue();m_rows->DisableCellEditControl();} }
    std::string fingerprint() const {
        std::ostringstream out;
        auto append=[&](const std::string &s){out << s.size() << ':' << s;};
        for(const auto &[key,field]:m_text){append(key);append(text(field->GetValue()));}
        for(const auto &[key,field]:m_check){append(key);out << field->GetValue();}
        for(const auto &[key,field]:m_choice){append(key);out << field->GetSelection() << ';';}
        for(int row=0;row<m_rows->GetNumberRows();++row)for(int col=0;col<4;++col)append(text(m_rows->GetCellValue(row,col)));
        return out.str();
    }
    sla::ExposureFinderTableParameters table_parameters() {
        sla::ExposureFinderTableParameters p;
#define EF_REAL(name) p.name=real(#name)
        EF_REAL(layer_height);EF_REAL(maximum_layer_height);EF_REAL(layer_height_step);
        EF_REAL(bottom_exposure);EF_REAL(normal_exposure);EF_REAL(bottom_step);EF_REAL(normal_step);
#undef EF_REAL
        p.tests=integer("tests");p.multiple_layer_heights=checked("multiple_layer_heights");p.include_base=checked("include_base");
        p.series=sla::ExposureFinderSeries(m_choice.at("series")->GetSelection());return p;
    }
    std::vector<sla::ExposureFinderTableRow> read_rows() {
        std::string csv;
        for(int row=0;row<m_rows->GetNumberRows();++row) {
            for(int col=0;col<4;++col){if(col)csv+=',';csv+=text(m_rows->GetCellValue(row,col));}csv+='\n';
        }
        return sla::parse_exposure_finder_manual_rows(csv);
    }
    void populate_rows(const std::vector<sla::ExposureFinderTableRow> &rows) {
        m_populating=true;
        if(m_rows->GetNumberRows())m_rows->DeleteRows(0,m_rows->GetNumberRows());
        m_rows->AppendRows(int(rows.size()));
        for(size_t i=0;i<rows.size();++i) {
            const auto &r=rows[i];const std::string values[]={number(r.layer_height),number(r.bottom_exposure),number(r.normal_exposure),std::to_string(r.brightness)};
            for(int col=0;col<4;++col)m_rows->SetCellValue(int(i),col,wxString::FromUTF8(values[col]));
        }
        for(int row=0;row<m_rows->GetNumberRows();++row)m_rows->SetReadOnly(row,4);
        m_populating=false;update_row_validity();sync_row_selection();invalidate();
    }
    void mark_manual() {m_choice.at("origin")->SetSelection(1);invalidate();}
    void generate_table() {
        commit_grid_editor();
        if(m_choice.at("origin")->GetSelection()==1 && m_rows->GetNumberRows() &&
            wxMessageBox("Replace the manual table? Row order, duplicates and manual values will be replaced.","Generate exposure table",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
        const auto parameters=table_parameters();
        auto rows=run_raster_task<std::vector<sla::ExposureFinderTableRow>>(this,"Generate exposure table",[parameters](const sla::LayerProgress &progress){return sla::generate_exposure_finder_table(parameters,progress);});
        if(!rows)return;
        populate_rows(*rows);m_choice.at("origin")->SetSelection(0);m_generated_table_fields=table_fingerprint();
    }
    sla::ExposureFinderGenerationParameters request() {
        if(m_choice.at("origin")->GetSelection()==0 && (m_generated_table_fields.empty() || m_generated_table_fields!=table_fingerprint()))
            throw RuntimeError("Table settings changed. Use Generate table before Preview plan, or choose Manual to preserve the current rows.");
        sla::ExposureFinderGenerationParameters result;
        result.recipe.table_parameters=table_parameters();result.recipe.rows=read_rows();
        result.recipe.table_origin=sla::ExposureFinderTableOrigin(m_choice.at("origin")->GetSelection());
        sla::ExposureFinderScheduleParameters p;
#define EF_REAL(name) p.name=real(#name)
        EF_REAL(base_height);EF_REAL(features_height);EF_REAL(custom_base_exposure);EF_REAL(repeated_lift_height);EF_REAL(repeated_wait_before);
#undef EF_REAL
        p.bottom_layers=integer("bottom_layers");p.multiple_exposures=checked("multiple_exposures");p.multiple_brightness=checked("multiple_brightness");
        p.brightness=sla::parse_exposure_finder_brightness(text(m_text.at("brightness")->GetValue()));
        p.base_mode=sla::ExposureFinderBaseMode(m_choice.at("base_mode")->GetSelection());
        p.brightness_exclusion=sla::ExposureFinderBrightnessExclusion(m_choice.at("brightness_exclusion")->GetSelection());
        p.repeated_settings=checked("repeated_settings");p.repeated_lift_enabled=checked("repeated_lift_enabled");p.repeated_wait_enabled=checked("repeated_wait_enabled");
        result.recipe.schedule=p;
        auto &pattern=result.pattern;
        pattern.holes=sla::parse_exposure_finder_features(text(m_text.at("holes")->GetValue()));
        pattern.bars=sla::parse_exposure_finder_features(text(m_text.at("bars")->GetValue()));
        pattern.bullseyes=sla::parse_exposure_finder_rings(text(m_text.at("bullseyes")->GetValue()));
#define EF_REAL(name) pattern.name=real(#name)
        EF_REAL(feature_margin_mm);EF_REAL(bar_length_mm);EF_REAL(bar_spacing_mm);EF_REAL(text_scale);
#undef EF_REAL
#define EF_INT(name) pattern.name=signed_integer(#name)
        EF_INT(bar_splitter);EF_INT(bar_fence);EF_INT(bar_fence_offset);EF_INT(bullseye_fence);EF_INT(bullseye_fence_offset);EF_INT(triangle_tip_offset);EF_INT(text_thickness);
#undef EF_INT
#define EF_BOOL(name) pattern.name=checked(#name)
        EF_BOOL(square_holes);EF_BOOL(invert_bullseye_quadrants);EF_BOOL(counter_triangles);EF_BOOL(triangle_fence);EF_BOOL(antialias);
#undef EF_BOOL
        pattern.staircase_thickness=integer("staircase_thickness");pattern.text=text(m_text.at("text")->GetValue());
        pattern.font_face=m_choice.at("font_face")->GetSelection() | (checked("italic")?16:0);
        auto &layout=result.layout;layout.cell_columns=integer("cell_columns");layout.cell_rows=integer("cell_rows");
#define EF_REAL(name) layout.name=real(#name)
        EF_REAL(margin_x_mm);EF_REAL(margin_y_mm);EF_REAL(gap_x_mm);EF_REAL(gap_y_mm);
#undef EF_REAL
        layout.center=checked("center");layout.mirror_x=checked("mirror_x");layout.mirror_y=checked("mirror_y");
        result.bottom_erosion=integer("bottom_erosion");result.chamfer_layers=integer("chamfer_layers");
        return result;
    }
    template<class Action> void safely(Action action) {
        try {action();}catch(const std::exception &error){
            std::string message=error.what();
            if(message.find("table is not normalized")!=std::string::npos)
                message+="\nUse Normalize rows on the Table tab to approve rounding, then Preview plan again.";
            const auto *validation=dynamic_cast<const FieldValidationError*>(&error);
            const wxWeakRef<wxTextCtrl> field(validation?validation->field.get():nullptr);
            const wxWeakRef<wxWindow> parent(this);
            const wxWeakRef<wxNotebook> notebook(m_notebook);
            wxMessageBox(wxString::FromUTF8(message),"Exposure finder",wxOK|wxICON_ERROR,this);
            if(parent && field) {
                // Restore focus after the error modal has finished, without
                // keeping a destroyed/cancelled form alive. Selecting its page
                // first lets wxScrolledWindow reveal the focused child natively.
                wxGetApp().CallAfter([parent,notebook,field] {
                    if(!parent || !notebook || !field || !parent->IsShownOnScreen())return;
                    wxWindow *page=field.get();
                    while(page && page->GetParent()!=notebook.get())page=page->GetParent();
                    if(!page)return;
                    for(size_t index=0;index<notebook->GetPageCount();++index)
                        if(notebook->GetPage(index)==page){notebook->SetSelection(index);break;}
                    if(field->IsEnabled()){field->SetFocus();field->SetSelection(-1,-1);}
                });
            }
        }
    }
    void update_row_validity(int changed_row = -1) {
        const int first=changed_row<0?0:changed_row;
        const int end=changed_row<0?m_rows->GetNumberRows():changed_row+1;
        for(int row=first;row<end;++row) {
            std::array<wxString,4> values;
            for(int col=0;col<4;++col)values[col]=m_rows->GetCellValue(row,col);
            m_rows->SetCellValue(row,4,row_validity(values));
        }
    }
    void sync_row_selection(int row = -1) {
        if(!m_rows || !m_selected_row)return;
        m_syncing_rows=true;
        const int count=m_rows->GetNumberRows();
        if(m_selected_row->GetMin()!=1 || m_selected_row->GetMax()!=std::max(1,count))
            m_selected_row->SetRange(1,std::max(1,count));
        m_selected_row->Enable(count>0);m_edit_row->Enable(count>0);
        if(count) {
            row=std::clamp(row<0?m_rows->GetGridCursorRow():row,0,count-1);
            if(m_selected_row->GetValue()!=row+1)m_selected_row->SetValue(row+1);
        }else m_selected_row->SetValue(1);
        m_syncing_rows=false;
    }
    void select_row(int selected) {
        if(m_syncing_rows || !m_rows || !m_rows->GetNumberRows())return;
        commit_grid_editor();
        const int row=std::clamp(selected-1,0,m_rows->GetNumberRows()-1);
        m_rows->SetGridCursor(row,0);m_rows->MakeCellVisible(row,0);sync_row_selection(row);
    }
    void edit_selected_row() {
        commit_grid_editor();
        select_row(m_selected_row->GetValue());
        const int row=m_rows->GetGridCursorRow();
        if(row<0 || row>=m_rows->GetNumberRows())return;
        std::array<wxString,4> original;
        for(int col=0;col<4;++col)original[col]=m_rows->GetCellValue(row,col);
        wxDialog dialog(this,wxID_ANY,wxString::Format("Edit exposure row %d",row+1),wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto *layout=new wxBoxSizer(wxVERTICAL);
        auto *fields=new wxFlexGridSizer(2,8,12);fields->AddGrowableCol(1);
        const char *labels[]={"Layer height (mm)","Bottom exposure (s)","Normal exposure (s)","Brightness (0–255)"};
        std::array<wxTextCtrl*,4> edits;
        for(int col=0;col<4;++col) {
            fields->Add(new wxStaticText(&dialog,wxID_ANY,labels[col]),0,wxALIGN_CENTER_VERTICAL);
            edits[col]=new wxTextCtrl(&dialog,wxID_ANY,original[col]);edits[col]->SetName(labels[col]);
            fields->Add(edits[col],1,wxEXPAND);
        }
        fields->Add(new wxStaticText(&dialog,wxID_ANY,"Row validity"),0,wxALIGN_CENTER_VERTICAL);
        auto *validity=new wxTextCtrl(&dialog,wxID_ANY,row_validity(original),wxDefaultPosition,wxSize(320,64),wxTE_READONLY|wxTE_MULTILINE);
        validity->SetName("Row validity");fields->Add(validity,1,wxEXPAND);
        layout->Add(fields,0,wxEXPAND|wxALL,12);
        layout->Add(new wxStaticText(&dialog,wxID_ANY,"Apply retains exact text, including invalid rows.\nUse Normalize rows to approve rounding separately."),0,wxLEFT|wxRIGHT|wxBOTTOM,12);
        layout->Add(dialog.CreateSeparatedButtonSizer(wxAPPLY|wxCANCEL),0,wxEXPAND|wxALL,10);
        for(auto *edit:edits)edit->Bind(wxEVT_TEXT,[&](wxCommandEvent &){
            std::array<wxString,4> values;for(int col=0;col<4;++col)values[col]=edits[col]->GetValue();
            validity->ChangeValue(row_validity(values));
        });
        dialog.Bind(wxEVT_BUTTON,[&](wxCommandEvent &){
            try {
                std::string csv;
                for(int col=0;col<4;++col){if(col)csv+=',';csv+=text(edits[col]->GetValue());}
                if(sla::parse_exposure_finder_manual_rows(csv).size()!=1)
                    throw RuntimeError("Enter exactly one row of numeric values");
                // Parsed finite invalid rows remain useful draft entries.
                // Syntax/range errors keep this modal and its raw text open.
                dialog.EndModal(wxID_APPLY);
            }catch(const std::exception &error){
                const auto message="Invalid: "+wxString::FromUTF8(error.what());
                validity->ChangeValue(message);validity->SetToolTip(message);
            }
        },wxID_APPLY);
        dialog.SetSizerAndFit(layout);dialog.SetMinSize(dialog.GetSize());dialog.CentreOnScreen();edits[0]->SetFocus();
        if(dialog.ShowModal()!=wxID_APPLY)return;
        std::array<wxString,4> values;for(int col=0;col<4;++col)values[col]=edits[col]->GetValue();
        if(values==original)return;
        // Commit this row only: formatting any other raw strings would silently
        // change the draft. Selection and Cancel never invalidate the plan.
        for(int col=0;col<4;++col)m_rows->SetCellValue(row,col,values[col]);
        mark_manual();update_row_validity(row);sync_row_selection(row);
    }
public:
    explicit ExposureFinderForm(wxWindow *parent):wxDialog(parent,wxID_ANY,"Exposure finder",wxDefaultPosition,wxSize(800,700),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER) {
        auto *root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(this,wxID_ANY,"ExposureFinder: edit the table, schedule and pattern, then Preview plan before Generate.\nBrightness controls individual mask parts; it does not replace the whole-layer PWM."),0,wxEXPAND|wxALL,10);
        m_notebook=new wxNotebook(this,wxID_ANY);m_notebook->SetName("Exposure finder sections");
        root->Add(m_notebook,1,wxEXPAND|wxLEFT|wxRIGHT,8);
        wxBoxSizer *table_layout=nullptr;auto *table=page("Table",&table_layout);
        numeric(table,"layer_height","Table layer height (mm)",.05);
        numeric(table,"maximum_layer_height","Maximum table layer height (mm)",.1);
        numeric(table,"layer_height_step","Table layer height step (mm)",.01);
        numeric(table,"bottom_exposure","Bottom exposure (s)",15);
        numeric(table,"normal_exposure","Normal exposure (s)",2);
        numeric(table,"bottom_step","Bottom exposure step (s or multiplier)",0);
        numeric(table,"normal_step","Normal exposure step (s or multiplier)",.2);
        numeric(table,"tests","Tests",4);
        boolean(table,"multiple_layer_heights","Multiple table layer heights",false);
        boolean(table,"include_base","Include base table row",true);
        choice(table,"series","Table series",{"Linear","Multiplier"},0);
        choice(table,"origin","Table source",{"Generated","Manual"},0);
        auto *table_actions=new wxWrapSizer(wxHORIZONTAL);
        auto table_button=[&](const char *label,auto action){auto *b=new wxButton(owner(table),wxID_ANY,label);b->SetName(label);table_actions->Add(b,0,wxALL,4);b->Bind(wxEVT_BUTTON,[this,action](wxCommandEvent &){CallAfter([this,action]{safely(action);});});};
        table_button("Generate table",[this]{generate_table();});
        table_button("Normalize rows",[this]{
            commit_grid_editor();
            if(wxMessageBox("Round table heights to 0.001 mm and exposures to 0.01 s? Row order, duplicates and invalid rows are preserved.","Normalize exposure rows",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
            const auto requested=read_rows();
            auto normalized=run_raster_task<std::vector<sla::ExposureFinderTableRow>>(this,"Normalize exposure rows",[requested](const sla::LayerProgress &progress){return sla::normalize_exposure_finder_table(requested,progress);});
            if(!normalized)return;
            populate_rows(*normalized);mark_manual();
        });
        table_button("Add row",[this]{commit_grid_editor();m_rows->AppendRows();const int row=m_rows->GetNumberRows()-1;for(int col=0;col<4;++col)m_rows->SetCellValue(row,col,col==3?"255":"0");m_rows->SetReadOnly(row,4);m_rows->SetGridCursor(row,0);mark_manual();update_row_validity();sync_row_selection(row);});
        table_button("Remove row",[this]{commit_grid_editor();if(m_rows->GetNumberRows()){const int row=m_rows->GetGridCursorRow();m_rows->DeleteRows(row);if(m_rows->GetNumberRows())m_rows->SetGridCursor(std::min(row,m_rows->GetNumberRows()-1),0);mark_manual();update_row_validity();sync_row_selection();}});
        auto move_row=[this](int delta){commit_grid_editor();const int row=m_rows->GetGridCursorRow(),to=row+delta;if(row<0 || to<0 || to>=m_rows->GetNumberRows())return;for(int col=0;col<4;++col){const auto saved=m_rows->GetCellValue(row,col);m_rows->SetCellValue(row,col,m_rows->GetCellValue(to,col));m_rows->SetCellValue(to,col,saved);}m_rows->SetGridCursor(to,0);mark_manual();update_row_validity();sync_row_selection(to);};
        table_button("Move up",[move_row]{move_row(-1);});table_button("Move down",[move_row]{move_row(1);});
        table_layout->Add(table_actions,0,wxEXPAND|wxLEFT|wxRIGHT,6);
        auto *row_actions=new wxBoxSizer(wxHORIZONTAL);
        row_actions->Add(new wxStaticText(owner(table),wxID_ANY,"Selected row"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,8);
        m_selected_row=new wxSpinCtrl(owner(table),wxID_ANY,"1",wxDefaultPosition,wxSize(100,-1),wxSP_ARROW_KEYS,1,1,1);
        m_selected_row->SetName("Selected row");row_actions->Add(m_selected_row,0,wxRIGHT,8);
        m_edit_row=new wxButton(owner(table),wxID_ANY,"Edit selected row...");m_edit_row->SetName("Edit selected row...");
        row_actions->Add(m_edit_row,0);table_layout->Add(row_actions,0,wxEXPAND|wxLEFT|wxRIGHT|wxTOP,10);
        m_selected_row->Bind(wxEVT_SPINCTRL,[this](wxSpinEvent &event){select_row(event.GetValue());});
        m_selected_row->Bind(wxEVT_TEXT,[this](wxCommandEvent &){select_row(m_selected_row->GetValue());});
        m_edit_row->Bind(wxEVT_BUTTON,[this](wxCommandEvent &){CallAfter([this]{safely([this]{edit_selected_row();});});});
        table_layout->Add(new wxStaticText(owner(table),wxID_ANY,"Edit rows directly. Edits switch the table to Manual. Invalid rows and duplicates remain visible.\nGenerate table is explicit and asks before replacing a manual table."),0,wxALL,10);
        m_rows=new wxGrid(owner(table),wxID_ANY);m_rows->CreateGrid(0,5);m_rows->SetName("Exposure table rows");
        const char *columns[]={"Height (mm)","Bottom (s)","Normal (s)","Brightness (0–255)","Validity"};
        for(int col=0;col<5;++col){m_rows->SetColLabelValue(col,columns[col]);m_rows->SetColSize(col,col==3?150:col==4?180:100);}
        m_rows->SetMinSize(wxSize(300,200));table_layout->Add(m_rows,0,wxEXPAND|wxALL,8);
        m_rows->Bind(wxEVT_GRID_CELL_CHANGED,[this](wxGridEvent &event){if(!m_populating && event.GetCol()<4){mark_manual();update_row_validity(event.GetRow());}event.Skip();});
        m_rows->Bind(wxEVT_GRID_SELECT_CELL,[this](wxGridEvent &event){sync_row_selection(event.GetRow());event.Skip();});
        sync_row_selection();
        m_choice.at("origin")->Bind(wxEVT_CHOICE,[this](wxCommandEvent &){
            invalidate();
            if(m_choice.at("origin")->GetSelection()==0){m_choice.at("origin")->SetSelection(1);CallAfter([this]{safely([this]{generate_table();});});}
        });

        auto *schedule=page("Schedule");
        numeric(schedule,"base_height","Base height (mm)",.1);numeric(schedule,"features_height","Features height (mm)",.1);
        numeric(schedule,"custom_base_exposure","Custom base exposure (s)",2);
        numeric(schedule,"bottom_layers","Bottom physical layers",2);
        boolean(schedule,"multiple_exposures","Multiple exposures",true);boolean(schedule,"multiple_brightness","Multiple brightness values",false);
        entry(schedule,"brightness","Brightness list (0–255; ordered)","255");
        choice(schedule,"base_mode","Base exposure mode",{"Iterative","First","Middle","Last","Custom"},0);
        choice(schedule,"brightness_exclusion","Brightness exclusion",{"None","Bottom","Bottom and base"},2);
        boolean(schedule,"repeated_settings","Override repeated exposures",false);
        boolean(schedule,"repeated_lift_enabled","Override repeated lift",true);
        boolean(schedule,"repeated_wait_enabled","Override repeated wait",true);
        numeric(schedule,"repeated_lift_height","Repeated lift height (mm)",0);numeric(schedule,"repeated_wait_before","Repeated wait before exposure (s)",0);

        auto *pattern=page("Pattern");
        entry(pattern,"holes","Holes (px; comma-separated)","");boolean(pattern,"square_holes","Square holes",true);
        entry(pattern,"bars","Bars (px; comma-separated)","4, 6, 8, 60");
        entry(pattern,"bullseyes","Bullseyes (diameter:thickness px)","26:5, 60:10, 116:15, 190:20");
        numeric(pattern,"feature_margin_mm","Feature margin (mm)",2);numeric(pattern,"bar_length_mm","Bar length (mm)",4);numeric(pattern,"bar_spacing_mm","Bar spacing (mm)",1.5);
        numeric(pattern,"bar_splitter","Bar splitter (px)",0);numeric(pattern,"bar_fence","Bar fence (px)",10);numeric(pattern,"bar_fence_offset","Bar fence offset (px; signed)",4);
        numeric(pattern,"bullseye_fence","Bullseye fence (px)",10);numeric(pattern,"bullseye_fence_offset","Bullseye fence offset (px; signed)",0);
        boolean(pattern,"invert_bullseye_quadrants","Invert bullseye quadrants",true);boolean(pattern,"counter_triangles","Counter triangles",true);boolean(pattern,"triangle_fence","Triangle fence",false);
        numeric(pattern,"triangle_tip_offset","Triangle tip offset (px; signed)",3);numeric(pattern,"staircase_thickness","Staircase thickness (px)",40);boolean(pattern,"antialias","Antialias",false);
        entry(pattern,"text","Pattern text (UTF-8)","ABHJQRWZ%&#");
        choice(pattern,"font_face","Font face",{"Simplex","Plain","Duplex","Complex","Triplex","Complex small","Script simplex","Script complex"},2);
        boolean(pattern,"italic","Italic",false);numeric(pattern,"text_thickness","Text thickness (px)",2);numeric(pattern,"text_scale","Text scale",1);

        auto *layout=page("Layout");
        numeric(layout,"cell_columns","Cell width (px; Auto=0)",0);numeric(layout,"cell_rows","Cell height (px; Auto=0)",0);
        numeric(layout,"margin_x_mm","LCD margin X (mm)",0);numeric(layout,"margin_y_mm","LCD margin Y (mm)",0);numeric(layout,"gap_x_mm","Cell gap X (mm)",0);numeric(layout,"gap_y_mm","Cell gap Y (mm)",0);
        boolean(layout,"center","Center layout",true);boolean(layout,"mirror_x","Additional mirror X",false);boolean(layout,"mirror_y","Additional mirror Y",false);
        numeric(layout,"bottom_erosion","Bottom erosion (px)",0);numeric(layout,"chamfer_layers","Chamfer layers",0);

        m_summary=new wxTextCtrl(this,wxID_ANY,"Generate or edit the table, then Preview plan.",wxDefaultPosition,wxSize(-1,120),wxTE_MULTILINE|wxTE_READONLY);
        m_summary->SetName("Exposure plan summary");root->Add(m_summary,0,wxEXPAND|wxALL,8);
        auto *actions=new wxBoxSizer(wxHORIZONTAL);auto *preview=new wxButton(this,wxID_ANY,"Preview plan");preview->SetName("Preview plan");
        m_generate=new wxButton(this,wxID_OK,"Generate");m_generate->SetName("Generate");m_generate->Disable();
        auto *cancel=new wxButton(this,wxID_CANCEL,"Cancel");actions->Add(preview,0,wxALL,5);actions->AddStretchSpacer();actions->Add(m_generate,0,wxALL,5);actions->Add(cancel,0,wxALL,5);root->Add(actions,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,6);
        SetSizer(root);SetMinSize(wxSize(560,480));
        // Keep actions outside scrolling pages and fit the actual display work area.
        const auto work=wxGetClientDisplayRect();SetSize(std::min(800,work.width),std::min(700,work.height));CentreOnScreen();update_enabled();
        preview->Bind(wxEVT_BUTTON,[this](wxCommandEvent &){CallAfter([this]{safely([this]{preview_plan();});});});
        Bind(wxEVT_BUTTON,[this](wxCommandEvent &){CallAfter([this]{safely([this]{generate();});});},wxID_OK);
    }
    void preview_plan();
    void generate();
};

void ExposureFinderForm::preview_plan() {
    commit_grid_editor();
    const auto parameters=request();
    auto configuration=wxGetApp().preset_bundle->full_config();
    const auto fields=fingerprint();
    invalidate();
    const auto plan=run_raster_task<sla::ExposureFinderGenerationPlan>(this,"Preview exposure plan",
        [configuration,parameters](const sla::LayerProgress &progress){return sla::plan_exposure_finder_generation(configuration,parameters,progress);});
    if(!plan)return;
    // The task dialog is modal; keep an explicit identity check as well so a
    // deferred event cannot publish a plan for a different control snapshot.
    if(fields!=fingerprint())throw RuntimeError("Settings changed while planning. Preview plan again.");
    m_planned_request=parameters;m_planned_config=std::move(configuration);m_planned_fields=fields;
    m_summary->ChangeValue(wxString::FromUTF8(sla::describe_exposure_finder_generation_plan(*plan)));
    m_generate->Enable();
}
void ExposureFinderForm::generate() {
    commit_grid_editor();
    if(!m_planned_request || !m_planned_config || m_planned_fields!=fingerprint() ||
       !m_planned_config->equals(wxGetApp().preset_bundle->full_config())) {
        invalidate();throw RuntimeError("Preview plan for the current table, settings and printer before Generate.");
    }
    const auto parameters=*m_planned_request;const auto configuration=*m_planned_config;
    auto job=run_raster_task<std::unique_ptr<SLAPrint>>(this,"Generate exposure finder",
        [configuration,parameters](const sla::LayerProgress &progress){return sla::create_exposure_finder_calibration(configuration,parameters,progress);});
    if(!job)return;
    auto *plater=wxGetApp().plater();if(!plater)throw RuntimeError("The Sliced workspace is unavailable");
    // Publication is a separate user decision. Refusing replacement retains
    // this entire draft and the old Sliced document, not a partial new job.
    if(!plater->open_generated_layers(**job))return;
    EndModal(wxID_OK);
}
} // namespace
void show_exposure_finder(wxWindow *parent) {ExposureFinderForm form(parent);form.ShowModal();}
} // namespace Slic3r::GUI
