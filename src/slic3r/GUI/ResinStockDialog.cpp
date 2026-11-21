// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ResinStockDialog.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SLA/ResinStock.hpp"
#include "libslic3r/SLA/StatisticsPresentation.hpp"
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <cmath>

namespace Slic3r::GUI {
namespace {

using sla::ResinMaterial;
using sla::ResinStock;
using sla::ResinStockError;

wxString num(double value, int decimals = 3)
{
    wxString text = wxString::FromCDouble(value, decimals);
    if (text.Contains('.')) { while (text.EndsWith("0")) text.RemoveLast(); if (text.EndsWith(".")) text.RemoveLast(); }
    return text;
}

// Locale independent: "1,5" and "1.5" both mean one and a half; anything else is rejected.
bool parse_number(const wxString &text, double &out)
{
    wxString t = text; t.Trim(true).Trim(false); t.Replace(",", ".");
    return !t.empty() && t.ToCDouble(&out) && std::isfinite(out);
}

wxTextCtrl *labelled(wxWindow *parent, wxFlexGridSizer *grid, const wxString &label, const wxString &value)
{
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto *field = new wxTextCtrl(parent, wxID_ANY, value, wxDefaultPosition, wxSize(220, -1));
    field->SetName(label);
    grid->Add(field, 1, wxEXPAND);
    return field;
}

bool load_stock(wxWindow *parent, ResinStock &stock, std::string &path)
{
    path = ResinStock::default_path();
    try { stock = ResinStock::load(path); return true; }
    catch (const ResinStockError &error) { show_error(parent, error.what()); return false; }
}

// Add / edit one material. Returns true when the user confirmed a valid material.
class MaterialEditor final : public wxDialog {
    wxTextCtrl *m_name, *m_volume, *m_cost, *m_density, *m_stock, *m_remaining;
public:
    ResinMaterial result;
    MaterialEditor(wxWindow *parent, const ResinMaterial &material, bool editing)
        : wxDialog(parent, wxID_ANY, editing ? _L("Edit resin") : _L("Add resin"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), result(material)
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 6, 8);
        grid->AddGrowableCol(1, 1);
        m_name = labelled(this, grid, _L("Resin name"), wxString::FromUTF8(material.name));
        if (editing) m_name->Disable();
        m_volume = labelled(this, grid, _L("Bottle volume (ml)"), num(material.bottle_volume_ml));
        m_cost = labelled(this, grid, _L("Bottle cost"), num(material.bottle_cost, 2));
        m_density = labelled(this, grid, _L("Density (g/ml)"), num(material.density));
        m_stock = labelled(this, grid, _L("Bottles in stock"), wxString::Format("%d", material.bottles_in_stock));
        m_remaining = labelled(this, grid, _L("Remaining in the open bottle (ml)"), num(material.bottle_remaining_ml));
        root->Add(grid, 0, wxEXPAND | wxALL, 12);
        root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
        SetSizerAndFit(root);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &event) { if (confirm()) event.Skip(); }, wxID_OK);
    }
private:
    void report(const wxString &message) { CallAfter([this, message] { show_error(this, message); }); }
    bool confirm()
    {
        ResinMaterial m = result;
        double stock = 0;
        m.name = into_u8(m_name->GetValue()); 
        const bool ok = parse_number(m_volume->GetValue(), m.bottle_volume_ml) && parse_number(m_cost->GetValue(), m.bottle_cost) &&
            parse_number(m_density->GetValue(), m.density) && parse_number(m_stock->GetValue(), stock) &&
            parse_number(m_remaining->GetValue(), m.bottle_remaining_ml) && stock == std::floor(stock) && std::fabs(stock) < 1e6;
        // Reported after returning from the button event: a modal opened inside an accessibility action would block the bridge.
        if (!ok) { report(_L("Every field must be a number; the number of bottles must be a whole number.")); return false; }
        m.bottles_in_stock = int(stock);
        if (const auto problem = sla::validate_resin_material(m); !problem.empty()) { report(from_u8(problem)); return false; }
        result = m;
        return true;
    }
};

class ResinStockManager final : public wxDialog {
    ResinStock m_stock;
    std::string m_path;
    wxDataViewListCtrl *m_list = nullptr;
    wxButton *m_edit = nullptr, *m_remove = nullptr;

    void refresh()
    {
        m_list->DeleteAllItems();
        for (const auto &m : m_stock.materials) {
            wxVector<wxVariant> cells;
            cells.push_back(wxVariant(wxString::FromUTF8(m.name)));
            cells.push_back(wxVariant(num(m.bottle_volume_ml)));
            cells.push_back(wxVariant(num(m.bottle_cost, 2)));
            cells.push_back(wxVariant(wxString::Format("%d", m.bottles_in_stock)));
            cells.push_back(wxVariant(num(m.bottle_remaining_ml)));
            cells.push_back(wxVariant(num(m.consumed_ml)));
            cells.push_back(wxVariant(wxString::Format("%d", m.print_count)));
            cells.push_back(wxVariant(num(m.print_time_h, 2)));
            m_list->AppendItem(cells);
        }
        const bool any = m_list->GetItemCount() > 0;
        m_edit->Enable(any && m_list->GetSelectedRow() != wxNOT_FOUND);
        m_remove->Enable(any && m_list->GetSelectedRow() != wxNOT_FOUND);
    }
    // Saves first, adopts the new inventory only when the file was written.
    bool commit(ResinStock next)
    {
        try { next.save(m_path); }
        catch (const ResinStockError &error) { show_error(this, error.what()); return false; }
        m_stock = std::move(next);
        refresh();
        return true;
    }
public:
    explicit ResinStockManager(wxWindow *parent, ResinStock stock, std::string path)
        : wxDialog(parent, wxID_ANY, _L("Resin stock"), wxDefaultPosition, wxSize(720, 380), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_stock(std::move(stock)), m_path(std::move(path))
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(this, wxID_ANY, _L("Bottles of resin you own. \"I printed this file\" books consumption here.")), 0, wxALL, 10);
        m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(600, 200), wxDV_ROW_LINES | wxDV_SINGLE);
        m_list->SetName("Resin stock list");
        const wxString columns[] = {_L("Name"), _L("Bottle (ml)"), _L("Cost"), _L("Bottles"), _L("Open bottle (ml)"), _L("Consumed (ml)"), _L("Prints"), _L("Print time (h)")};
        for (int i = 0; i < 8; ++i) m_list->AppendTextColumn(columns[i], wxDATAVIEW_CELL_INERT, i == 0 ? 170 : 90);
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto *add = new wxButton(this, wxID_ANY, _L("Add resin..."));
        add->SetName("Add resin...");
        m_edit = new wxButton(this, wxID_ANY, _L("Edit resin..."));
        m_edit->SetName("Edit resin...");
        m_remove = new wxButton(this, wxID_ANY, _L("Remove resin"));
        m_remove->SetName("Remove resin");
        auto *close = new wxButton(this, wxID_CANCEL, _L("Close"));
        close->SetName("Close");
        buttons->Add(add, 0, wxRIGHT, 6); buttons->Add(m_edit, 0, wxRIGHT, 6); buttons->Add(m_remove, 0);
        buttons->AddStretchSpacer(); buttons->Add(close, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, 10);
        SetSizer(root);
        SetMinSize(wxSize(560, 300));
        refresh();
        m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent &) { refresh_buttons(); });
        add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { add_material(); }); });
        m_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { edit_material(); }); });
        m_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { remove_material(); }); });
    }
private:
    void refresh_buttons()
    {
        const bool selected = m_list->GetSelectedRow() != wxNOT_FOUND;
        m_edit->Enable(selected); m_remove->Enable(selected);
    }
    void add_material()
    {
        ResinMaterial m;
        m.name = into_u8(wxGetApp().preset_bundle->sla_materials.get_selected_preset_name());
        MaterialEditor editor(this, m, false);
        if (editor.ShowModal() != wxID_OK) return;
        ResinStock next = m_stock;
        try { next.add(editor.result); } catch (const ResinStockError &error) { show_error(this, error.what()); return; }
        commit(std::move(next));
    }
    void edit_material()
    {
        const int row = m_list->GetSelectedRow();
        if (row == wxNOT_FOUND || row >= int(m_stock.materials.size())) return;
        MaterialEditor editor(this, m_stock.materials[size_t(row)], true);
        if (editor.ShowModal() != wxID_OK) return;
        ResinStock next = m_stock;
        // Consumption history is not editable here: only the bottle data are.
        auto &target = next.materials[size_t(row)];
        target.bottle_volume_ml = editor.result.bottle_volume_ml; target.bottle_cost = editor.result.bottle_cost;
        target.density = editor.result.density; target.bottles_in_stock = editor.result.bottles_in_stock;
        target.bottle_remaining_ml = editor.result.bottle_remaining_ml;
        commit(std::move(next));
    }
    void remove_material()
    {
        const int row = m_list->GetSelectedRow();
        if (row == wxNOT_FOUND || row >= int(m_stock.materials.size())) return;
        const auto name = m_stock.materials[size_t(row)].name;
        if (wxMessageBox(from_u8("\"" + name + "\"") + " " + _L("will be removed from the resin stock, including its consumption history. Continue?"),
                         _L("Remove resin"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        ResinStock next = m_stock;
        next.remove(name);
        commit(std::move(next));
    }
};

class PrintedDialog final : public wxDialog {
    ResinStock m_stock;
    std::string m_path;
    wxChoice *m_material = nullptr;
    wxSpinCtrl *m_copies = nullptr;
    wxTextCtrl *m_volume = nullptr, *m_time = nullptr;
    wxStaticText *m_summary = nullptr;
    wxButton *m_ok = nullptr;
    std::vector<std::string> m_names;
public:
    PrintedDialog(wxWindow *parent, ResinStock stock, std::string path, double volume_ml, double seconds, const std::string &preferred,
                  const ResinMaterial &preset_defaults)
        : wxDialog(parent, wxID_ANY, _L("I printed this file"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_stock(std::move(stock)), m_path(std::move(path))
    {
        // The current material preset is always selectable; it is stored in the stock only when the booking is confirmed.
        if (!preferred.empty() && !m_stock.find(preferred)) {
            ResinMaterial m = preset_defaults; m.name = preferred;
            m_stock.materials.push_back(m);
        }
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 6, 8);
        grid->AddGrowableCol(1, 1);
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Resin")), 0, wxALIGN_CENTER_VERTICAL);
        m_material = new wxChoice(this, wxID_ANY);
        m_material->SetName("Resin");
        int selection = 0;
        for (const auto &m : m_stock.materials) {
            if (m.name == preferred) selection = int(m_names.size());
            m_names.push_back(m.name);
            m_material->Append(wxString::FromUTF8(m.name));
        }
        if (!m_names.empty()) m_material->SetSelection(selection);
        grid->Add(m_material, 1, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Copies printed")), 0, wxALIGN_CENTER_VERTICAL);
        m_copies = new wxSpinCtrl(this, wxID_ANY, "1", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 10000, 1);
        m_copies->SetName("Copies printed");
        grid->Add(m_copies, 0);
        m_volume = labelled(this, grid, _L("Resin used per copy (ml)"), num(volume_ml, 3));
        m_time = labelled(this, grid, _L("Print time per copy (s)"), num(seconds, 0));
        root->Add(grid, 0, wxEXPAND | wxALL, 12);
        m_summary = new wxStaticText(this, wxID_ANY, wxString(), wxDefaultPosition, wxSize(420, 70));
        m_summary->SetName("Resin booking summary");
        root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        auto *buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
        m_ok = static_cast<wxButton*>(FindWindow(wxID_OK));
        m_ok->SetLabel(_L("Update stock"));
        m_ok->SetName("Update stock");
        if (auto *cancel = FindWindow(wxID_CANCEL)) cancel->SetName("Cancel");
        root->Add(buttons, 0, wxEXPAND | wxALL, 8);
        SetSizerAndFit(root);
        auto update = [this](wxCommandEvent &) { refresh(); };
        m_material->Bind(wxEVT_CHOICE, update);
        m_copies->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { refresh(); });
        m_copies->Bind(wxEVT_TEXT, update);
        m_volume->Bind(wxEVT_TEXT, update);
        m_time->Bind(wxEVT_TEXT, update);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &event) { if (book()) event.Skip(); }, wxID_OK);
        refresh();
    }
private:
    bool read(double &ml, double &seconds, int &copies) const
    {
        double v = 0, t = 0;
        if (!parse_number(m_volume->GetValue(), v) || !parse_number(m_time->GetValue(), t) || v <= 0. || t < 0.) return false;
        copies = m_copies->GetValue();
        ml = v * copies; seconds = t * copies;
        return m_material->GetSelection() != wxNOT_FOUND && std::isfinite(ml) && std::isfinite(seconds);
    }
    void refresh()
    {
        double ml, seconds; int copies;
        if (!read(ml, seconds, copies)) {
            m_summary->SetLabel(_L("Enter a resin used per copy greater than zero and a non-negative print time."));
            m_ok->Enable(false);
            return;
        }
        const auto &m = m_stock.materials[size_t(m_material->GetSelection())];
        ResinMaterial after = m; after.consume(ml, seconds);
        const double cost = m.bottle_volume_ml > 0 ? ml / m.bottle_volume_ml * m.bottle_cost : 0.;
        m_summary->SetLabel(wxString::Format(_L("Total resin: %s ml, cost: %s.\nBottles in stock: %d -> %d; open bottle: %s ml -> %s ml."),
            num(ml), num(cost, 2), m.bottles_in_stock, after.bottles_in_stock, num(m.bottle_remaining_ml), num(after.bottle_remaining_ml)));
        m_ok->Enable(true);
    }
    bool book()
    {
        double ml, seconds; int copies;
        if (!read(ml, seconds, copies)) return false;
        ResinStock next = m_stock;
        next.materials[size_t(m_material->GetSelection())].consume(ml, seconds);
        try { next.save(m_path); }
        catch (const ResinStockError &error) { const std::string message = error.what(); CallAfter([this, message] { show_error(this, message); }); return false; }
        return true;
    }
};

} // namespace

void show_resin_printed_dialog(wxWindow *parent)
{
    ResinStock stock; std::string path;
    if (!load_stock(parent, stock, path)) return;
    double ml = 0., seconds = 0.;
    ResinMaterial defaults;
    std::string preferred;
    if (auto *plater = wxGetApp().plater()) {
        auto &print = plater->active_sla_print();
        const SLAPrintStatistics &ps = print.print_statistics();
        ml = (ps.objects_used_material + ps.support_used_material) / 1000.;
        seconds = std::isfinite(ps.estimated_print_time) && ps.estimated_print_time > 0 ? ps.estimated_print_time : 0.;
        const auto &material = print.material_config();
        if (material.bottle_volume.value > 0) defaults.bottle_volume_ml = defaults.bottle_remaining_ml = material.bottle_volume.value;
        if (material.bottle_cost.value >= 0) defaults.bottle_cost = material.bottle_cost.value;
        if (material.material_density.value >= 0) defaults.density = material.material_density.value;
        defaults.bottles_in_stock = 1;
    }
    if (!std::isfinite(ml) || ml < 0) ml = 0;
    preferred = wxGetApp().preset_bundle->sla_materials.get_selected_preset_name();
    PrintedDialog dialog(parent, std::move(stock), path, ml, seconds, preferred, defaults);
    dialog.ShowModal();
}

void show_resin_stock_manager(wxWindow *parent)
{
    ResinStock stock; std::string path;
    if (!load_stock(parent, stock, path)) return;
    ResinStockManager dialog(parent, std::move(stock), path);
    dialog.ShowModal();
}

} // namespace Slic3r::GUI
