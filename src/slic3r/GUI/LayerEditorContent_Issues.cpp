#include "LayerEditorContent.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
void LayerEditorContent::build_issue_panel(wxBoxSizer *root)
{
    const sla::IssueDetectionConfig defaults;
    // Right column: issues first (they drive the canvas overlay), export below.
    auto *heading = new wxStaticText(this, wxID_ANY, _L("Issues"));
    heading->SetFont(heading->GetFont().Bold());
    heading->SetToolTip(_L("Islands, overhangs, resin traps, suction cups, touching bounds and empty layers."));
    m_right_col->Add(heading, 0, wxTOP, 2);
    m_right_col->Add(new wxStaticLine(this), 0, wxEXPAND | wxTOP | wxBOTTOM, 3);

    auto *pane = new wxCollapsiblePane(this, wxID_ANY, _L("Detection settings"));
    auto *grid = new wxFlexGridSizer(2, 4, 8);
    grid->AddGrowableCol(0);
    auto *inner = pane->GetPane();
    auto check = [&](int slot, const wxString &label, bool value) {
        m_issue_on[slot] = new wxCheckBox(inner, wxID_ANY, label);
        m_issue_on[slot]->SetValue(value);
        grid->Add(m_issue_on[slot], 0, wxALIGN_CENTER_VERTICAL | wxTOP, slot ? 6 : 0);
        grid->AddSpacer(0);
    };
    auto spin = [&](const wxString &label, int low, int high, int value) {
        grid->Add(new wxStaticText(inner, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 20);
        auto *ctrl = new wxSpinCtrl(inner, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1));
        ctrl->SetRange(low, high); ctrl->SetValue(value); grid->Add(ctrl, 0); return ctrl;
    };
    auto spin_d = [&](const wxString &label, double low, double high, double inc, double value) {
        grid->Add(new wxStaticText(inner, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 20);
        auto *ctrl = new wxSpinCtrlDouble(inner, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1));
        ctrl->SetRange(low, high); ctrl->SetIncrement(inc); ctrl->SetValue(value); grid->Add(ctrl, 0); return ctrl;
    };
    check(0, _L("Islands"), defaults.island.enabled);
    m_issue_island_area = spin("Required area (px)", 0, 65535, defaults.island.required_area_to_process_check_px);
    m_issue_support_percent = spin_d(_L("Support (% of area above)"), 0., 100., 1.,
                                     defaults.island.required_pixels_to_support_multiplier * 100.);
    check(1, _L("Overhangs"), defaults.overhang.enabled);
    m_issue_overhang_erode = spin(_L("Erode (px)"), 0, 255, defaults.overhang.erode_iterations);
    check(2, _L("Resin traps"), defaults.resin_trap.enabled);
    m_issue_trap_area = spin("Required area (px)", 0, 255, defaults.resin_trap.required_area_to_process_check_px);
    check(3, _L("Suction cups"), defaults.resin_trap.detect_suction_cups);
    m_issue_cup_height = spin_d("Cup height (mm)", 0., 100., .1, defaults.resin_trap.required_height_to_consider_suction_cup_mm);
    check(4, _L("Touching bounds"), defaults.touching_bound.enabled);
    m_issue_margin = spin(_L("Margin (px)"), 0, 255, defaults.touching_bound.margin_left_px);
    check(5, _L("Empty layers"), defaults.empty_layer.enabled);
    inner->SetSizer(grid);
    grid->SetSizeHints(inner);
    pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &) { Layout(); FitInside(); });
    m_right_col->Add(pane, 0, wxEXPAND);

    m_issue_detect = new wxButton(this, wxID_ANY, _L("Detect issues"));
    m_issue_detect->SetToolTip(_L("Scans every layer in the background (progress and Cancel are shown). Masks stay untouched; results are marked out of date after any edit."));
    m_issue_detect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { perform([this] { detect_issues(); }); }); });
    m_right_col->Add(m_issue_detect, 0, wxEXPAND | wxTOP, 6);
    m_issue_status = new wxStaticText(this, wxID_ANY, _L("Issues have not been detected yet."));
    m_right_col->Add(m_issue_status, 0, wxEXPAND | wxTOP, 4);

    m_issue_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(100), FromDIP(110)), wxDV_SINGLE | wxDV_VERT_RULES);
    m_issue_list->SetName("Detected layer issues");
    set_dataview_accessible_name(m_issue_list, "Detected layer issues");
    // Column widths follow the header text (plus room for the sort arrow), so no header is ever clipped, in any language
    // or text size. The list scrolls horizontally when the columns together are wider than the right column.
    const auto column = [&](const wxString &title, int minimum_dip, wxAlignment align) {
        const int width = std::max(FromDIP(minimum_dip), m_issue_list->GetTextExtent(title).x + FromDIP(30));
        m_issue_list->AppendTextColumn(title, wxDATAVIEW_CELL_INERT, width, align, wxDATAVIEW_COL_RESIZABLE);
    };
    column(_L("Type"), 66, wxALIGN_LEFT);
    column(_L("Layers"), 56, wxALIGN_LEFT);
    column(_L("Pixels"), 62, wxALIGN_RIGHT);
    column(_L("Bounding box (x, y, w × h px)"), 150, wxALIGN_LEFT);
    m_issue_list->SetMinSize(wxSize(FromDIP(120), -1));   // the columns scroll sideways; they must not widen the column
    m_issue_list->SetToolTip(_L("Bounding box: x, y, width × height in mask pixels. Scroll the list sideways when a column is cut off."));
    m_issue_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent &) {
        const int row = m_issue_list->GetSelectedRow();
        if (row != wxNOT_FOUND) { select_issue(size_t(row)); return; }
        m_issue_selected = size_t(-1); m_preview->Refresh(false);
    });
    // The list takes the free height of the column; the export group keeps its natural size below it.
    m_right_col->Add(m_issue_list, 1, wxEXPAND | wxTOP, 6);
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
        done = run_background_layers(this, _L("Detect layer issues"), [&](const auto &progress) {
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
        m_issue_status->SetLabel(_L("Detection cancelled; previous results are unchanged."));
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
    const wxString text = !m_issues_ran ? wxString(_L("Issues have not been detected yet.")) :
        stale ? wxString(_L("Results are out of date: masks changed after detection. Run Detect issues again.")) :
        m_issues.empty() ? wxString(_L("No issues found.")) :
        wxString::Format(_L("%zu issue(s) found. Select a row to jump to it."), m_issues.size());
    if (text == m_issue_text) return; // called from a 250 ms timer: do nothing unless the state changed
    m_issue_text = text;
    m_issue_status->SetLabel(text); m_issue_status->Wrap(side_width());
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
