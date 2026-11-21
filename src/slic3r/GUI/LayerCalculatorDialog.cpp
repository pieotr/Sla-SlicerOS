#include "LayerEditorInternal.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
class LayerCalculatorDialog final : public wxDialog {
    wxSpinCtrlDouble *m_res_x = nullptr, *m_res_y = nullptr, *m_width = nullptr, *m_height = nullptr, *m_mm = nullptr,
                     *m_lift = nullptr, *m_lift_speed = nullptr, *m_retract_speed = nullptr, *m_wait = nullptr, *m_layer = nullptr;
    wxStaticText *m_ppm = nullptr, *m_pixels = nullptr, *m_delay = nullptr, *m_xy = nullptr, *m_tilt = nullptr, *m_time = nullptr;
    std::vector<layerops::LayerParams> m_table;

    wxSpinCtrlDouble *row(wxWindow *box, wxFlexGridSizer *grid, const wxString &label, double lo, double hi, double value, int digits) {
        grid->Add(new wxStaticText(box, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        auto *spin = new wxSpinCtrlDouble(box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
                                          lo, hi, value, std::pow(10., -digits));
        spin->SetDigits(unsigned(digits));
        spin->SetName(label);
        grid->Add(spin, 1, wxEXPAND);
        spin->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &) { update(); });
        spin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { update(); });
        return spin;
    }
    wxStaticText *result(wxWindow *box, wxFlexGridSizer *grid, const wxString &label) {
        grid->Add(new wxStaticText(box, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        auto *text = new wxStaticText(box, wxID_ANY, "-");
        text->SetFont(text->GetFont().Bold());
        set_accessible_name(text, wxString(), label, AccessibleRole::Label); // name stays the value; the description says what it is
        grid->Add(text, 1, wxEXPAND);
        return text;
    }
    void update() {
        if (!m_time) return; // controls are still being built (m_time is created last)
        const size_t rx = size_t(m_res_x->GetValue()), ry = size_t(m_res_y->GetValue());
        const double w = m_width->GetValue(), h = m_height->GetValue();
        const auto ppm = layerops::pixels_per_millimeter(rx, ry, w, h);
        const auto px = layerops::mm_to_pixels(rx, ry, w, h, m_mm->GetValue());
        m_ppm->SetLabel(wxString::Format("%.3f x %.3f px/mm", ppm.x, ppm.y));
        m_pixels->SetLabel(wxString::Format("%.2f px (X)   %.2f px (Y)", px.x, px.y));
        m_delay->SetLabel(wxString::Format("%.2f s", layerops::light_off_delay_seconds(
            m_lift->GetValue(), m_lift_speed->GetValue(), m_retract_speed->GetValue(), m_wait->GetValue())));
        m_xy->SetLabel(wxString::Format("%.4f mm", layerops::xy_resolution_mm(rx, ry, w, h)));
        m_tilt->SetLabel(wxString::Format("%.3f degrees", layerops::optimal_model_tilt_degrees(rx, ry, w, h, m_layer->GetValue())));
        m_time->SetLabel(m_table.empty() ? wxString(_L("No per-layer parameters in this document")) :
            wxString::FromUTF8(batch2_detail::format_duration(layerops::estimate_print_time_s(m_table))));
        Layout();
    }

public:
    LayerCalculatorDialog(wxWindow *parent, size_t res_x, size_t res_y, std::optional<std::pair<double, double>> display_mm,
                          std::vector<layerops::LayerParams> table, double layer_height_mm)
        : wxDialog(parent, wxID_ANY, _L("Calculator"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_table(std::move(table))
    {
        SetName("Layer calculator");
        const auto &sample = batch2_detail::first_normal(m_table);
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto section = [&](const wxString &title) {
            auto *box = new wxStaticBoxSizer(wxVERTICAL, this, title);
            auto *grid = new wxFlexGridSizer(2, 4, 8);
            grid->AddGrowableCol(1);
            box->Add(grid, 1, wxEXPAND | wxALL, 4);
            root->Add(box, 0, wxEXPAND | wxALL, 6);
            return std::make_pair(box->GetStaticBox(), grid);
        };
        auto [b1, g1] = section(_L("Millimeters to pixels"));
        m_res_x = row(b1, g1, _L("Resolution X (px)"), 1, 100000, double(res_x), 0);
        m_res_y = row(b1, g1, _L("Resolution Y (px)"), 1, 100000, double(res_y), 0);
        m_width = row(b1, g1, _L("Display width (mm)"), 0, 10000, display_mm ? display_mm->first : 0, 3);
        m_height = row(b1, g1, _L("Display height (mm)"), 0, 10000, display_mm ? display_mm->second : 0, 3);
        m_mm = row(b1, g1, _L("Length (mm)"), 0, 10000, 1, 3);
        m_ppm = result(b1, g1, _L("Pixels per millimeter"));
        m_pixels = result(b1, g1, _L("Length in pixels"));
        auto [b2, g2] = section(_L("Light-off delay"));
        m_lift = row(b2, g2, _L("Lift height (mm)"), 0, 1000, sample.lift_height_mm > 0 ? sample.lift_height_mm : 5, 2);
        m_lift_speed = row(b2, g2, _L("Lift speed (mm/min)"), 0, 100000, sample.lift_speed > 0 ? sample.lift_speed : 100, 2);
        m_retract_speed = row(b2, g2, _L("Retract speed (mm/min)"), 0, 100000, sample.retract_speed > 0 ? sample.retract_speed : 150, 2);
        m_wait = row(b2, g2, _L("Extra wait (s)"), 0, 1000, 0, 2);
        m_delay = result(b2, g2, _L("Light-off delay"));
        auto [b3, g3] = section(_L("Optimal model tilt"));
        m_layer = row(b3, g3, _L("Layer height (mm)"), 0.001, 10, layer_height_mm > 0 ? layer_height_mm : 0.05, 3);
        m_xy = result(b3, g3, _L("XY resolution"));
        m_tilt = result(b3, g3, _L("Optimal tilt angle"));
        auto [b4, g4] = section(_L("Print time"));
        m_time = result(b4, g4, _L("Estimated time (h:mm:ss)"));
        auto *close = new wxButton(this, wxID_CLOSE, _L("Close"));
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); });
        root->Add(close, 0, wxALIGN_RIGHT | wxALL, 8);
        SetSizerAndFit(root);
        update();
    }
};


void show_layer_calculator(wxWindow *parent, size_t res_x, size_t res_y, std::optional<std::pair<double, double>> display_mm,
                           std::vector<layerops::LayerParams> table, double layer_height_mm)
{
    LayerCalculatorDialog dialog(parent, res_x, res_y, display_mm, std::move(table), layer_height_mm);
    dialog.ShowModal();
}
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
