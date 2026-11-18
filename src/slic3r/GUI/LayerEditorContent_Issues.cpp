#include "LayerEditorContent.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
void LayerEditorContent::build_issue_panel(wxBoxSizer *root)
{
    const sla::IssueDetectionConfig defaults;
    auto *heading = new wxStaticText(this, wxID_ANY, "Issues — islands, overhangs, resin traps, suction cups, touching bounds, empty layers");
    heading->SetFont(heading->GetFont().Bold());
    root->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    auto *pane = new wxCollapsiblePane(this, wxID_ANY, "Detection settings");
    auto *grid = new wxFlexGridSizer(2, 4, 8);
    auto *inner = pane->GetPane();
    auto check = [&](int slot, const char *label, bool value) {
        m_issue_on[slot] = new wxCheckBox(inner, wxID_ANY, label);
        m_issue_on[slot]->SetValue(value);
        grid->Add(m_issue_on[slot], 0, wxALIGN_CENTER_VERTICAL);
    };
    auto spin = [&](const char *label, int low, int high, int value) {
        grid->Add(new wxStaticText(inner, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        auto *ctrl = new wxSpinCtrl(inner, wxID_ANY);
        ctrl->SetRange(low, high); ctrl->SetValue(value); grid->Add(ctrl, 0); return ctrl;
    };
    auto spin_d = [&](const char *label, double low, double high, double inc, double value) {
        grid->Add(new wxStaticText(inner, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        auto *ctrl = new wxSpinCtrlDouble(inner, wxID_ANY);
        ctrl->SetRange(low, high); ctrl->SetIncrement(inc); ctrl->SetValue(value); grid->Add(ctrl, 0); return ctrl;
    };
    check(0, "Islands", defaults.island.enabled);
    m_issue_island_area = spin("Island required area (px)", 0, 65535, defaults.island.required_area_to_process_check_px);
    m_issue_support_percent = spin_d("Island support (% of area above)", 0., 100., 1.,
                                     defaults.island.required_pixels_to_support_multiplier * 100.);
    grid->AddSpacer(0);
    check(1, "Overhangs", defaults.overhang.enabled);
    m_issue_overhang_erode = spin("Overhang erode (px)", 0, 255, defaults.overhang.erode_iterations);
    check(2, "Resin traps", defaults.resin_trap.enabled);
    m_issue_trap_area = spin("Resin trap required area (px)", 0, 255, defaults.resin_trap.required_area_to_process_check_px);
    check(3, "Suction cups", defaults.resin_trap.detect_suction_cups);
    m_issue_cup_height = spin_d("Suction cup height (mm)", 0., 100., .1, defaults.resin_trap.required_height_to_consider_suction_cup_mm);
    check(4, "Touching bounds", defaults.touching_bound.enabled);
    m_issue_margin = spin("Touching bound margin (px)", 0, 255, defaults.touching_bound.margin_left_px);
    check(5, "Empty layers", defaults.empty_layer.enabled);
    grid->AddSpacer(0); grid->AddSpacer(0);
    inner->SetSizer(grid);
    grid->SetSizeHints(inner);
    pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &) { Layout(); FitInside(); });
    root->Add(pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    auto *bar = new wxWrapSizer(wxHORIZONTAL);
    m_issue_detect = new wxButton(this, wxID_ANY, "Detect issues");
    m_issue_detect->SetToolTip("Scans every layer in the background (progress and Cancel are shown). Masks stay untouched; results are marked out of date after any edit.");
    m_issue_detect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { perform([this] { detect_issues(); }); }); });
    bar->Add(m_issue_detect, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    m_issue_status = new wxStaticText(this, wxID_ANY, "Issues have not been detected yet.");
    bar->Add(m_issue_status, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    root->Add(bar, 0, wxEXPAND);

    m_issue_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 140), wxDV_SINGLE | wxDV_VERT_RULES);
    m_issue_list->SetName("Detected layer issues");
    set_dataview_accessible_name(m_issue_list, "Detected layer issues");
    m_issue_list->AppendTextColumn("Type", wxDATAVIEW_CELL_INERT, 110, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_issue_list->AppendTextColumn("Layers", wxDATAVIEW_CELL_INERT, 100, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_issue_list->AppendTextColumn("Pixels", wxDATAVIEW_CELL_INERT, 90, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    m_issue_list->AppendTextColumn("Bounding box (x, y, w × h px)", wxDATAVIEW_CELL_INERT, 220, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_issue_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent &) {
        const int row = m_issue_list->GetSelectedRow();
        if (row != wxNOT_FOUND) { select_issue(size_t(row)); return; }
        m_issue_selected = size_t(-1); m_preview->Refresh(false);
    });
    root->Add(m_issue_list, 0, wxEXPAND | wxALL, 8);
}

sla::IssueDetectionConfig LayerEditorContent::read_issue_config() const
{
    sla::IssueDetectionConfig config;
    config.island.enabled = m_issue_on[0]->GetValue();
    config.island.required_area_to_process_check_px = uint16_t(m_issue_island_area->GetValue());
    config.island.required_pixels_to_support_multiplier = m_issue_support_percent->GetValue() / 100.;
    config.overhang.enabled = m_issue_on[1]->GetValue();
    config.overhang.erode_iterations = uint8_t(m_issue_overhang_erode->GetValue());
    config.resin_trap.enabled = m_issue_on[2]->GetValue();
    config.resin_trap.required_area_to_process_check_px = uint8_t(m_issue_trap_area->GetValue());
    config.resin_trap.detect_suction_cups = m_issue_on[3]->GetValue();
    config.resin_trap.required_height_to_consider_suction_cup_mm = m_issue_cup_height->GetValue();
    config.touching_bound.enabled = m_issue_on[4]->GetValue();
    const auto margin = uint8_t(m_issue_margin->GetValue());
    config.touching_bound.margin_left_px = config.touching_bound.margin_top_px =
        config.touching_bound.margin_right_px = config.touching_bound.margin_bottom_px = margin;
    config.empty_layer.enabled = m_issue_on[5]->GetValue();
    // Machine height is known only when a printer/profile is attached; 0 disables the check.
    config.print_height.machine_height_mm = m_export_profile ? m_export_profile->max_print_height_mm :
                                            m_print ? double(m_print->printer_config().max_print_height.value) : 0.;
    return config;
}

void LayerEditorContent::detect_issues()
{
    finish_stroke(true);
    const sla::IssueDetectionConfig config = read_issue_config();
    const uint64_t revision = m_document.revision();
    const std::vector<double> z = issue_layer_positions_z(m_document);
    // The detector decodes each mask on demand from its worker threads; the
    // document is only read while m_working blocks every edit.
    const sla::LayerMaskProvider provider = [this](size_t index) {
        auto image = m_document.image(index);
        sla::LayerMask mask;
        mask.width = image.cols; mask.height = image.rows; mask.data = std::move(image.buf);
        return mask;
    };
    std::vector<sla::LayerIssue> result;
    std::atomic<bool> cancel_requested{false};
    bool done = false;
    {
        TaskLock working(m_working);
        done = run_background_layers(this, "Detect layer issues", [&](const auto &progress) {
            try {
                result = sla::detect_layer_issues(provider, z, config,
                    [&](size_t layers_done, size_t layers_total) { progress(layers_done, layers_total); },
                    [&] { return cancel_requested.load(std::memory_order_relaxed); });
                return true;
            } catch (const CanceledException &) { return false; }
              catch (const sla::LayerOperationCancelled &) { return false; }
        }, [&] { cancel_requested.store(true, std::memory_order_relaxed); });
    }
    if (!done || cancel_requested.load(std::memory_order_relaxed)) {
        m_issue_status->SetLabel("Detection cancelled; previous results are unchanged.");
        Layout();
        return;
    }
    m_issues = std::move(result);
    m_issues_revision = revision;
    m_issues_ran = true;
    m_issue_selected = size_t(-1);
    fill_issue_list();
    m_issue_text.clear();
    update_issue_state();
}

void LayerEditorContent::fill_issue_list()
{
    m_issue_list->Freeze();
    m_issue_list->DeleteAllItems();
    wxVector<wxVariant> cells;
    for (size_t i = 0; i < m_issues.size(); ++i) {
        const auto &issue = m_issues[i];
        cells.clear();
        cells.push_back(wxVariant(issue_type_label(issue.type)));
        cells.push_back(wxVariant(issue.first_layer == issue.last_layer ?
            wxString::Format("%zu", issue.first_layer + 1) :
            wxString::Format("%zu–%zu", issue.first_layer + 1, issue.last_layer + 1))); // 1-based like the Layer spinner
        cells.push_back(wxVariant(wxString::Format("%zu", issue.pixel_count)));
        cells.push_back(wxVariant(issue.bbox.defined ? wxString::Format("%d, %d, %d × %d",
            issue.bbox.min.x(), issue.bbox.min.y(),
            issue.bbox.max.x() - issue.bbox.min.x() + 1, issue.bbox.max.y() - issue.bbox.min.y() + 1) : wxString("—")));
        m_issue_list->AppendItem(cells);
    }
    m_issue_list->Thaw();
}

void LayerEditorContent::update_issue_state()
{
    if (!m_issue_status || m_working) return;
    const bool stale = m_issues_ran && !issues_current();
    const wxString text = !m_issues_ran ? wxString("Issues have not been detected yet.") :
        stale ? wxString("Results are out of date: masks changed after detection. Run Detect issues again.") :
        m_issues.empty() ? wxString("No issues found.") :
        wxString::Format("%zu issue(s) found. Select a row to jump to it.", m_issues.size());
    if (text == m_issue_text) return; // called from a 250 ms timer: do nothing unless the state changed
    m_issue_text = text;
    m_issue_status->SetLabel(text);
    m_issue_list->Enable(!stale);
    if (stale) m_issue_selected = size_t(-1);
    Layout();
    m_preview->Refresh(false);
}

void LayerEditorContent::select_issue(size_t index)
{
    if (m_working || !issues_current() || index >= m_issues.size()) return;
    m_issue_selected = index;
    const size_t layer = m_issues[index].first_layer;
    m_layer->SetValue(int(layer + 1));
    m_paint_range->SetValue(false);
    CallAfter([this] { perform([] {}); });
}

void LayerEditorContent::paint_issue_overlay(wxAutoBufferedPaintDC &dc)
{
    if (m_issue_selected >= m_issues.size() || !issues_current() || !m_cols || !m_rows ||
        selected_plane() != sla::LayerMaskPlane::Main) return;
    const auto &issue = m_issues[m_issue_selected];
    const size_t layer = size_t(m_layer->GetValue() - 1);
    if (layer < issue.first_layer || layer > issue.last_layer) return;
    const sla::LayerIssuePart *part = nullptr;
    for (const auto &candidate : issue.parts) if (candidate.layer == layer) { part = &candidate; break; }
    const BoundingBox &box = part ? part->bbox : issue.bbox;
    const wxSize size = m_preview->GetClientSize();
    const double sx = double(size.x) / double(m_cols), sy = double(size.y) / double(m_rows);
    constexpr size_t max_runs = 20000;
    if (part && part->runs.size() <= max_runs) {
        wxGCDC gc(dc);
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(wxColour(255, 40, 40, 140)));
        for (const auto &run : part->runs)
            gc.DrawRectangle(int(run.x0 * sx), int(run.y * sy),
                             std::max(1, int(std::ceil((run.x1 - run.x0) * sx))), std::max(1, int(std::ceil(sy))));
    }
    if (box.defined) {
        dc.SetPen(wxPen(wxColour(255, 220, 0), 2));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(int(box.min.x() * sx) - 2, int(box.min.y() * sy) - 2,
                         int((box.max.x() - box.min.x() + 1) * sx) + 4, int((box.max.y() - box.min.y() + 1) * sy) + 4);
    }
}
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
