#include "LayerEditorInternal.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
class OpTreeData final : public wxClientData {
public:
    explicit OpTreeData(int op) : index(op) {}
    int index;
};

class LayerOperationsDialog final : public wxDialog {
    LayerOpsHost m_host;
    std::vector<OpDef> m_defs;
    std::vector<std::vector<wxWindow *>> m_controls; // per operation, one control per field
    std::vector<wxScrolledWindow *> m_forms;
    wxDataViewTreeCtrl *m_tree = nullptr; // GtkTreeView on GTK: rows are exposed through AT-SPI
    wxStaticText *m_description = nullptr, *m_message = nullptr, *m_range_note = nullptr;
    wxSpinCtrl *m_first = nullptr, *m_last = nullptr;
    wxCheckBox *m_use_roi = nullptr;
    wxSpinCtrl *m_roi[4] = {nullptr, nullptr, nullptr, nullptr};
    wxButton *m_last_rect = nullptr, *m_preview_button = nullptr, *m_apply_button = nullptr;
    wxBoxSizer *m_form_sizer = nullptr;
    wxPanel *m_range_row = nullptr, *m_roi_row = nullptr;
    int m_selected = -1;
    bool m_previewing = false;

    OpContext initial_context() const {
        OpContext c;
        c.layer_count = m_host.layer_count();
        c.z_mm = m_host.layer_z_mm();
        const auto size = m_host.mask_size();
        c.width = size.first; c.height = size.second;
        c.bottom_layers = m_host.bottom_layer_count();
        c.parameter_refusal = m_host.parameter_refusal();
        if (c.parameter_refusal.empty()) {
            c.table = m_host.parameter_table();
            c.printer = m_host.printer_context();
        }
        c.display_mm = m_host.display_size_mm();
        c.list_edits_possible = m_host.list_refusal(false).empty();
        return c;
    }
    void build_form(size_t op_index, wxWindow *parent) {
        auto &def = m_defs[op_index];
        // Scrolled so that long parameter forms (Edit parameters has 23 fields) do not outgrow the screen.
        auto *panel = new wxScrolledWindow(parent);
        auto *sizer = new wxFlexGridSizer(2, 4, 8);
        sizer->AddGrowableCol(1);
        auto &controls = m_controls[op_index];
        for (const auto &f : def.fields) {
            wxWindow *ctrl = nullptr;
            if (f.kind == OpField::Bool) {
                auto *box = new wxCheckBox(panel, wxID_ANY, f.label);
                box->SetValue(f.def != 0);
                sizer->AddSpacer(0); sizer->Add(box, 0, wxALIGN_CENTER_VERTICAL);
                ctrl = box;
            } else {
                sizer->Add(new wxStaticText(panel, wxID_ANY, f.label), 0, wxALIGN_CENTER_VERTICAL);
                if (f.kind == OpField::Int) {
                    auto *spin = new wxSpinCtrl(panel, wxID_ANY);
                    spin->SetRange(int(f.lo), int(f.hi)); spin->SetValue(int(f.def));
                    ctrl = spin;
                } else if (f.kind == OpField::Dbl) {
                    auto *spin = new wxSpinCtrlDouble(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                                      wxSP_ARROW_KEYS, f.lo, f.hi, f.def, std::pow(10., -f.digits));
                    spin->SetDigits(unsigned(f.digits));
                    ctrl = spin;
                } else if (f.kind == OpField::Choice) {
                    auto *choice = create_readonly_combo(panel);
                    for (const auto &c : f.choices) choice->Append(c);
                    if (!f.choices.empty()) choice->SetSelection(std::clamp(int(f.def), 0, int(f.choices.size()) - 1));
                    ctrl = choice;
                } else if (f.kind == OpField::Opt) {
                    auto *text = new wxTextCtrl(panel, wxID_ANY, wxString::FromUTF8(f.text));
                    text->SetHint("unchanged");
                    ctrl = text;
                } else if (f.kind == OpField::File) {
                    auto *text = new wxTextCtrl(panel, wxID_ANY, wxString::FromUTF8(f.text), wxDefaultPosition, wxSize(220, -1));
                    auto *browse = new wxButton(panel, wxID_ANY, "Browse...");
                    browse->Bind(wxEVT_BUTTON, [text](wxCommandEvent &) {
                        wxFileDialog open(text, "Choose an image", "", "",
                            "Images|*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff|All files|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
                        if (open.ShowModal() == wxID_OK) text->SetValue(open.GetPath());
                    });
                    auto *row = new wxBoxSizer(wxHORIZONTAL);
                    row->Add(text, 1, wxEXPAND | wxRIGHT, 4);
                    row->Add(browse, 0);
                    sizer->Add(row, 1, wxEXPAND);
                    ctrl = text;
                } else {
                    auto *text = new wxTextCtrl(panel, wxID_ANY, wxString::FromUTF8(f.text), wxDefaultPosition, wxSize(260, 60), wxTE_MULTILINE);
                    ctrl = text;
                }
                if (f.kind != OpField::File) sizer->Add(ctrl, 1, wxEXPAND);
            }
            if (!f.tip.empty()) ctrl->SetToolTip(f.tip);
            ctrl->SetName(f.label);
            controls.push_back(ctrl);
        }
        auto *outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(sizer, 0, wxEXPAND | wxALL, 4);
        if (def.pattern_helper) add_pattern_helper(op_index, panel, outer);
        panel->SetSizer(outer);
        const wxSize best = outer->CalcMin();
        panel->SetScrollRate(0, 12);
        panel->SetMinSize(wxSize(best.x + 24, std::min(best.y, 340)));
        m_forms[op_index] = panel;
    }
    // Fills the "pattern"/"alternate" text boxes from the library's pattern generator.
    void add_pattern_helper(size_t op_index, wxPanel *panel, wxBoxSizer *outer) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        auto *names = create_readonly_combo(panel, "Pattern preset");
        for (const char *n : {"Chessboard", "Sparse", "Crosses", "Strips", "Pyramid", "Rhombus", "Hearts", "Slashes", "Waves", "Solid"})
            names->Append(n);
        names->SetSelection(0);
        auto *brightness = new wxSpinCtrl(panel, wxID_ANY);
        brightness->SetRange(0, 255); brightness->SetValue(127);
        auto *generate = new wxButton(panel, wxID_ANY, "Generate pattern");
        row->Add(new wxStaticText(panel, wxID_ANY, "Preset"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(names, 0, wxRIGHT, 8);
        row->Add(new wxStaticText(panel, wxID_ANY, "Brightness"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(brightness, 0, wxRIGHT, 8);
        row->Add(generate, 0);
        outer->Add(row, 0, wxALL, 4);
        generate->Bind(wxEVT_BUTTON, [this, op_index, names, brightness](wxCommandEvent &) {
            const auto texts = layerops::generate_dimming_pattern(names->GetStringSelection().ToStdString(), uint8_t(brightness->GetValue()));
            auto set_text = [&](const char *key, const std::string &text) {
                for (size_t i = 0; i < m_defs[op_index].fields.size(); ++i)
                    if (m_defs[op_index].fields[i].key == key)
                        static_cast<wxTextCtrl *>(m_controls[op_index][i])->SetValue(wxString::FromUTF8(text));
            };
            set_text("pattern", texts.pattern); set_text("alternate", texts.alternate);
        });
    }
    OpValues read_values(size_t op_index) const {
        OpValues v;
        for (size_t i = 0; i < m_defs[op_index].fields.size(); ++i) {
            const auto &f = m_defs[op_index].fields[i];
            auto *ctrl = m_controls[op_index][i];
            switch (f.kind) {
            case OpField::Int:    v.number[f.key] = static_cast<wxSpinCtrl *>(ctrl)->GetValue(); break;
            case OpField::Dbl:    v.number[f.key] = static_cast<wxSpinCtrlDouble *>(ctrl)->GetValue(); break;
            case OpField::Bool:   v.number[f.key] = static_cast<wxCheckBox *>(ctrl)->GetValue() ? 1 : 0; break;
            case OpField::Choice: v.number[f.key] = static_cast<ComboBox *>(ctrl)->GetSelection(); break;
            case OpField::Text:
            case OpField::Opt:
            case OpField::File:   v.text[f.key] = static_cast<wxTextCtrl *>(ctrl)->GetValue().utf8_string(); break;
            }
        }
        return v;
    }
    OpContext read_context() const {
        auto c = initial_context();
        c.first = size_t(std::max(1, m_first->GetValue()) - 1);
        c.last = size_t(std::max(1, m_last->GetValue()) - 1);
        if (m_use_roi->GetValue() && m_defs[size_t(m_selected)].uses_roi)
            c.roi = {m_roi[0]->GetValue(), m_roi[1]->GetValue(), m_roi[2]->GetValue(), m_roi[3]->GetValue()};
        return c;
    }
    void set_message(const wxString &text, bool error) {
        m_message->SetForegroundColour(error ? wxColour(190, 40, 40) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        m_message->SetLabel(text);
        m_message->Wrap(std::max(260, GetClientSize().x - 40));
        Layout();
    }
    void select_operation(int index) {
        if (index < 0 || size_t(index) >= m_defs.size()) return;
        clear_preview();
        m_selected = index;
        for (size_t i = 0; i < m_forms.size(); ++i) m_forms[i]->Show(int(i) == index);
        const auto &def = m_defs[size_t(index)];
        m_description->SetLabel(def.description);
        m_description->Wrap(std::max(260, GetClientSize().x / 2));
        m_range_row->Show(def.uses_range);
        m_roi_row->Show(def.uses_roi);
        m_apply_button->SetLabel(def.group == OpGroup::Export ? "Export..." : "Apply");
        m_preview_button->Enable(def.previewable);
        m_preview_button->SetToolTip(def.previewable ? "Show the result on the current layer only. Nothing is changed."
                                                     : "This operation needs several layers and has no single-layer preview.");
        set_message("", false);
        Layout(); Fit();
    }
    void clear_preview() {
        if (!m_previewing) return;
        m_previewing = false;
        m_host.show_preview(nullptr);
    }
    // Builds the operation from the form; validation failures are shown inline, never thrown further.
    std::optional<BuiltOp> build_selected(OpContext &context) {
        if (m_selected < 0) { set_message("Choose an operation first.", true); return std::nullopt; }
        context = read_context();
        try { return m_defs[size_t(m_selected)].build(read_values(size_t(m_selected)), context); }
        catch (const std::invalid_argument &error) { set_message(wxString::FromUTF8(error.what()), true); }
        catch (const std::exception &error) { set_message(wxString::FromUTF8(error.what()), true); }
        return std::nullopt;
    }
    // Preview: run on a copy of the current layer only; the document is never touched.
    void on_preview() {
        OpContext context;
        auto built = build_selected(context);
        if (!built || !built->mask) return;
        try {
            std::vector<layerops::Mask> layers{m_host.read_current_layer()};
            std::vector<double> z{context.z_mm.empty() ? 0.05 : context.z_mm[std::min(context.z_mm.size() - 1, m_host.current_layer())]};
            built->mask(layers, z, layerops::LayerSelection{0, 0, context.roi}, {});
            m_previewing = true;
            m_host.show_preview(&layers.front());
            set_message("Preview of the current layer only. Press Apply to change the selected layers.", false);
        } catch (const std::exception &error) { set_message(wxString::FromUTF8(error.what()), true); }
    }
    // Exports ask for the destination first, then decode the document in the background worker.
    void run_export(const ExportPlan &plan, const wxString &title) {
        std::string path;
        if (plan.folder) {
            wxDirDialog dir(this, "Choose the export folder", "", wxDD_DEFAULT_STYLE | wxDD_NEW_DIR_BUTTON);
            if (dir.ShowModal() != wxID_OK) return;
            path = dir.GetPath().utf8_string();
        } else {
            wxFileDialog save(this, title, "", plan.default_name, plan.wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (save.ShowModal() != wxID_OK) return;
            path = save.GetPath().utf8_string();
        }
        LayerExportJob job;
        job.title = title;
        const auto run = plan.run;
        job.run = [run, path](const ExportData &data, const sla::LayerProgress &progress) { run(data, path, progress); };
        if (m_host.run_export(job)) set_message("Exported to " + wxString::FromUTF8(path), false);
        else set_message("Cancelled. Nothing was written.", false);
    }
    // Apply: background run with progress + Cancel; the document commits ONE undo step.
    void on_apply() {
        OpContext context;
        auto built = build_selected(context);
        if (!built) return;
        clear_preview();
        const wxString title = m_defs[size_t(m_selected)].label;
        try {
            if (built->exporter) { run_export(*built->exporter, "Export " + title); return; }
            LayerOpsJob job;
            job.title = "Apply " + title;
            job.changes_resolution = built->changes_resolution;
            if (built->document_op) {
                job.document_op = built->document_op;
            } else if (built->stream) {
                if (!built->stream_pixels)
                    if (const auto reason = m_host.list_refusal(built->changes_resolution); !reason.empty()) { set_message(wxString::FromUTF8(reason), true); return; }
                job.stream = built->stream; job.stream_params = built->stream_params; job.stream_pixels = built->stream_pixels;
            } else if (built->param) {
                // Parameter operations that add layers need a document that can take a different layer list.
                if (built->param_changes_list)
                    if (const auto reason = m_host.list_refusal(false); !reason.empty()) { set_message(wxString::FromUTF8(reason), true); return; }
                job.param_op = built->param; job.param_needs_masks = built->param_needs_masks;
                job.param_changes_list = built->param_changes_list;
            } else if (built->lithophane) {
                job.lithophane = built->lithophane;
                job.lithophane_stream = built->lithophane_stream;
            } else if (built->list) {
                if (const auto reason = m_host.list_refusal(built->changes_resolution); !reason.empty()) {
                    set_message(wxString::FromUTF8(reason), true);
                    return;
                }
                auto list = built->list;
                job.list_op = [list](std::vector<layerops::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &pr) { return list(layers, z, pr); };
            } else {
                auto mask = built->mask;
                const auto z = context.z_mm;
                const auto selection = context.selection();
                job.mask_op = [mask, z, selection](std::vector<layerops::Mask> &layers, const sla::LayerProgress &pr) { mask(layers, z, selection, pr); };
                if (built->kernel) {
                    job.kernel = built->kernel;
                    if (built->kernel_needs_z) job.kernel_z = z;
                }
            }
            switch (m_host.run(job)) {
            case RunOutcome::Committed:
                // The layer list or the parameter table changed: the editor rebuilds this dialog with fresh defaults.
                if (job.list_op || job.param_op || job.lithophane || job.document_op || (job.stream && !job.stream_pixels)) { m_host.on_layer_list_changed(); return; }
                set_message("Applied. Use Undo in the editor to revert (one step).", false);
                break;
            case RunOutcome::Elsewhere: set_message("Done. The result was opened as a new layer project.", false); break;
            case RunOutcome::Cancelled: set_message("Cancelled. The layers were not changed.", false); break;
            }
        } catch (const std::invalid_argument &error) { set_message(wxString::FromUTF8(error.what()), true); }
        catch (const std::exception &error) { set_message(wxString::FromUTF8(error.what()), true); }
    }
    // Small calculator (UVtools Calculator tool): everything is computed live from the form.
    void open_calculator();

public:
    LayerOperationsDialog(wxWindow *parent, LayerOpsHost host)
        : wxDialog(parent, wxID_ANY, "Layer operations", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_host(std::move(host))
    {
        SetName("Layer operations");
        m_defs = make_layer_operation_defs(initial_context());
        m_controls.resize(m_defs.size());
        m_forms.resize(m_defs.size(), nullptr);
        auto *root = new wxBoxSizer(wxHORIZONTAL);
        m_tree = new wxDataViewTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(190, 380), wxDV_SINGLE | wxDV_NO_HEADER);
        m_tree->SetName("Operation list");
        set_dataview_accessible_name(m_tree, "Operation list");
        const wxDataViewItem tree_root;
        const wxDataViewItem groups[5] = {m_tree->AppendContainer(tree_root, "Transform"), m_tree->AppendContainer(tree_root, "Pixels"),
                                          m_tree->AppendContainer(tree_root, "Layers"), m_tree->AppendContainer(tree_root, "Parameters"),
                                          m_tree->AppendContainer(tree_root, "Export...")};
        for (size_t i = 0; i < m_defs.size(); ++i)
            m_tree->AppendItem(groups[int(m_defs[i].group)], m_defs[i].label, -1, new OpTreeData(int(i)));
        for (const auto &g : groups) m_tree->Expand(g);
        root->Add(m_tree, 0, wxEXPAND | wxALL, 8);

        auto *right = new wxBoxSizer(wxVERTICAL);
        m_description = new wxStaticText(this, wxID_ANY, "Choose an operation on the left.");
        right->Add(m_description, 0, wxALL, 6);
        m_form_sizer = new wxBoxSizer(wxVERTICAL);
        for (size_t i = 0; i < m_defs.size(); ++i) {
            build_form(i, this);
            m_forms[i]->Hide();
            m_form_sizer->Add(m_forms[i], 0, wxEXPAND);
        }
        right->Add(m_form_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);

        // Layer range (default: the editor's current selection).
        const size_t count = std::max<size_t>(1, m_host.layer_count());
        const auto range = m_host.selected_range();
        m_range_row = new wxPanel(this);
        auto *range_sizer = new wxBoxSizer(wxHORIZONTAL);
        range_sizer->Add(new wxStaticText(m_range_row, wxID_ANY, "Layers (1-based) from"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        m_first = new wxSpinCtrl(m_range_row, wxID_ANY); m_first->SetRange(1, int(count)); m_first->SetValue(int(range.first + 1));
        m_last = new wxSpinCtrl(m_range_row, wxID_ANY); m_last->SetRange(1, int(count)); m_last->SetValue(int(range.second + 1));
        m_first->SetName("Operation first layer"); m_last->SetName("Operation last layer");
        range_sizer->Add(m_first, 0, wxRIGHT, 4);
        range_sizer->Add(new wxStaticText(m_range_row, wxID_ANY, "to"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        range_sizer->Add(m_last, 0);
        m_range_row->SetSizer(range_sizer);
        right->Add(m_range_row, 0, wxALL, 6);
        m_first->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { if (m_first->GetValue() > m_last->GetValue()) m_last->SetValue(m_first->GetValue()); });
        m_last->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { if (m_last->GetValue() < m_first->GetValue()) m_first->SetValue(m_last->GetValue()); });

        // Optional region of interest in pixels.
        m_roi_row = new wxPanel(this);
        auto *roi_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_use_roi = new wxCheckBox(m_roi_row, wxID_ANY, "Region of interest (px)");
        m_use_roi->SetToolTip("Limit the operation to a rectangle: x, y, width, height. Unchecked = whole image (or model bounds).");
        roi_sizer->Add(m_use_roi, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        const auto size = m_host.mask_size();
        const char *roi_names[4] = {"ROI x", "ROI y", "ROI width", "ROI height"};
        for (int i = 0; i < 4; ++i) {
            m_roi[i] = new wxSpinCtrl(m_roi_row, wxID_ANY, "", wxDefaultPosition, wxSize(78, -1));
            m_roi[i]->SetRange(0, int(i % 2 ? size.second : size.first));
            m_roi[i]->SetValue(i >= 2 ? int(i == 2 ? size.first : size.second) : 0);
            m_roi[i]->SetName(roi_names[i]);
            roi_sizer->Add(m_roi[i], 0, wxRIGHT, 3);
        }
        m_last_rect = new wxButton(m_roi_row, wxID_ANY, "Use last rectangle");
        m_last_rect->SetToolTip("Takes the last rectangle drawn with the Rectangle tool in the editor.");
        roi_sizer->Add(m_last_rect, 0, wxLEFT, 4);
        m_last_rect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            const auto rect = m_host.last_rectangle();
            if (!rect) { set_message("Draw a rectangle with the Rectangle tool in the editor first.", true); return; }
            const int v[4] = {rect->x, rect->y, rect->w, rect->h};
            for (int i = 0; i < 4; ++i) m_roi[i]->SetValue(v[i]);
            m_use_roi->SetValue(true);
        });
        m_roi_row->SetSizer(roi_sizer);
        right->Add(m_roi_row, 0, wxALL, 6);

        m_message = new wxStaticText(this, wxID_ANY, "");
        m_message->SetName("Operation message");
        right->Add(m_message, 0, wxEXPAND | wxALL, 6);

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_preview_button = new wxButton(this, wxID_ANY, "Preview");
        m_apply_button = new wxButton(this, wxID_ANY, "Apply");
        m_apply_button->SetToolTip("Runs in the background with progress and Cancel. The whole operation is one Undo step.");
        auto *close = new wxButton(this, wxID_CLOSE, "Close");
        auto *calculator = new wxButton(this, wxID_ANY, "Calculator...");
        calculator->SetToolTip("Pixel size, light-off delay, optimal model tilt and print time estimate.");
        calculator->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { open_calculator(); });
        buttons->Add(calculator, 0, wxRIGHT, 24);
        buttons->Add(m_preview_button, 0, wxRIGHT, 6); buttons->Add(m_apply_button, 0, wxRIGHT, 6); buttons->Add(close, 0);
        right->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 8);
        root->Add(right, 1, wxEXPAND);
        SetSizerAndFit(root);

        m_tree->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent &event) {
            if (!event.GetItem().IsOk()) return;
            if (auto *data = dynamic_cast<OpTreeData *>(m_tree->GetItemData(event.GetItem()))) select_operation(data->index);
        });
        m_preview_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_preview(); });
        m_apply_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { on_apply(); }); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); });
        // Closing hides nothing permanent: the editor destroys this dialog when it is reopened.
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { clear_preview(); Hide(); });
        m_last_rect->Enable(true);
    }
};

void LayerOperationsDialog::open_calculator()
{
    const auto size = m_host.mask_size();
    const auto context = initial_context();
    show_layer_calculator(this, size.first, size.second, context.display_mm, context.table,
                          context.printer.layer_height_mm);
}


wxDialog *create_layer_operations_dialog(wxWindow *parent, LayerOpsHost host)
{
    return new LayerOperationsDialog(parent, std::move(host));
}
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
