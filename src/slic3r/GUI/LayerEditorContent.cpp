#include "LayerEditorContent.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
// Read-only drop-down for the editor controls. WHY not wxChoice: on GTK3 a wxChoice is a GtkComboBox whose list is a
// GtkMenu with a pointer grab. On Plasma/Wayland through XWayland (and in the private KWin test session, with a bare
// Gtk.ComboBoxText too) a click that has no pointer motion between press and release selects the entry under the pointer,
// i.e. the list _L("closes by itself") right after the click. The application's own ComboBox (DropDown popup) is used for
// every preset/settings drop-down and does not depend on GTK menu grabs.
static ComboBox *make_choice(wxWindow *parent)
{
    return create_readonly_combo(parent);
}


// Thin drag handle between two columns: drag, double-click to reset, Left/Right arrows when focused.
class LayerSash final : public wxPanel {
public:
    LayerSash(wxWindow *parent, const wxString &name, const wxString &tip, std::function<void()> begin,
              std::function<void(int)> move, std::function<void()> end, std::function<void(int)> step,
              std::function<void()> reset)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(5), -1), wxWANTS_CHARS),
          m_begin(std::move(begin)), m_move(std::move(move)), m_end(std::move(end)), m_step(std::move(step)), m_reset(std::move(reset))
    {
        SetMinSize(wxSize(FromDIP(5), -1));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_SIZEWE));
        SetName(name);
        SetToolTip(tip);
        set_accessible_name(this, name, tip);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            const bool active = m_hover || m_drag;
            const int width = active ? std::max(3, FromDIP(3)) : 1;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxSystemSettings::GetColour(active ? wxSYS_COLOUR_HIGHLIGHT : wxSYS_COLOUR_BTNSHADOW)));
            dc.DrawRectangle((GetClientSize().x - width) / 2, 0, width, GetClientSize().y);
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { m_hover = false; Refresh(); });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
            SetFocus();
            m_origin = ClientToScreen(e.GetPosition()).x;
            m_drag = true; m_begin();
            if (!HasCapture()) CaptureMouse();
            Refresh();
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent &e) {
            if (m_drag && e.LeftIsDown()) m_move(ClientToScreen(e.GetPosition()).x - m_origin);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { finish(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { m_drag = false; m_end(); Refresh(); });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent &) { finish(); m_reset(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &e) {
            const int step = e.ShiftDown() ? 48 : 16;
            if (e.GetKeyCode() == WXK_LEFT) m_step(-step);
            else if (e.GetKeyCode() == WXK_RIGHT) m_step(step);
            else e.Skip();
        });
    }
private:
    void finish() {
        if (!m_drag) return;
        m_drag = false;
        if (HasCapture()) ReleaseMouse();
        m_end(); Refresh();
    }
    bool m_hover = false, m_drag = false;
    int m_origin = 0;
    std::function<void()> m_begin, m_end, m_reset;
    std::function<void(int)> m_move, m_step;
};

LayerEditorContent::LayerEditorContent(wxWindow *parent, sla::LayerDocument document,
                  std::optional<sla::LayerExportProfile> profile, std::string project_path,
                  std::optional<sla::ProjectSLASettings> settings,
                  std::unique_ptr<sla::SlicedResultSession> session,
                  std::function<void()> close,
                  std::function<void(sla::SlicedResultSession &)> apply,
                  std::function<bool()> source_available): LayerEditorPanel(parent, wxID_ANY), m_session(std::move(session)),
      m_standalone_document(std::move(document)), m_document(m_session ? m_session->document() : m_standalone_document),
      m_print(m_session ? &m_session->context() : nullptr),
      m_close(std::move(close)), m_apply_source(std::move(apply)),
      m_source_available(std::move(source_available)), m_export_profile(profile),
      m_reopened_profile(profile), m_context_baseline(profile), m_project_settings(settings),
      m_settings_baseline(settings), m_layer_project_path(std::move(project_path))
{
    if (!m_document.layer_count()) throw RuntimeError("The layer document is empty");
    m_edit_projection=m_document.edit_value_projection();
    std::array<bool,256> levels{};
    for(const auto value:m_edit_projection)levels[value]=true;
    m_edit_level_count=unsigned(std::count(levels.begin(),levels.end(),true));
    m_document.mark_saved();
    m_model_project_revision = m_document.revision();
    if (m_print && m_print->is_raster_job() && !m_project_settings) {
        m_project_settings=sla::capture_project_settings(m_print->full_print_config());
        m_settings_baseline=m_project_settings;
    }
    if (m_print) m_printer_name = m_print->is_raster_job() ? m_project_settings->printer.name :
        wxGetApp().preset_bundle->printers.get_selected_preset_source_name("printer_settings_id");
    if (!m_layer_project_path.empty()) m_project_revision=m_document.revision();
    SetName("Sliced layer editor");
    {   // "Radius (pixels)" is 102 px wide at the 96 dpi design size. The English reference text is measured on purpose:
        // the factor reacts to the text SIZE (large fonts / DPI text scale), not to the length of a translation (the Polish
        // text is ~20% longer and would widen the side panels beyond the window).
        const double ratio = double(GetTextExtent(wxString::FromUTF8("Radius (pixels)")).x) / double(FromDIP(102));
        m_text_factor = ratio > 1.15 ? std::min(ratio, 2.0) : 1.;
    }
    SetScrollRate(16,16);
    // Three columns around the canvas, which takes every spare pixel: tools (left), canvas + layer actions + status bar
    // (centre), issues + export (right, collapsible). The columns are separated by drag handles (LayerSash) whose
    // widths are remembered in AppConfig. All controls stay children of this panel and are created
    // label-then-control in the historical order (the AT-SPI tree follows creation order).
    auto *root = new wxBoxSizer(wxVERTICAL);
    m_body = new wxBoxSizer(wxHORIZONTAL);
    m_left_col = new wxBoxSizer(wxVERTICAL);
    m_center_col = new wxBoxSizer(wxVERTICAL);
    m_right_col = new wxBoxSizer(wxVERTICAL);
    m_canvas_row = new wxBoxSizer(wxHORIZONTAL);
    m_info_bar = new wxBoxSizer(wxHORIZONTAL);
    m_hint_box = new wxBoxSizer(wxVERTICAL);
    m_ops_bar = new wxWrapSizer(wxHORIZONTAL);
    const auto stored_width = [](const char *key, int fallback) {
        const auto *config = wxGetApp().app_config;
        const int value = config ? std::atoi(config->get(key).c_str()) : 0;
        return value > 0 ? value : fallback;
    };
    m_left_dip = clamp_column_width(true, stored_width("layer_editor_left_width", sdip(kLeftColumnDip)));
    m_right_dip = clamp_column_width(false, stored_width("layer_editor_right_width", sdip(kRightColumnDip)));
    m_left_col->SetMinSize(FromDIP(m_left_dip), -1);
    m_right_col->SetMinSize(FromDIP(m_right_dip), -1);
    m_center_col->Add(m_canvas_row, 1, wxEXPAND);
    m_center_col->Add(m_hint_box, 0, wxEXPAND | wxTOP, 2);
    m_center_col->Add(m_ops_bar, 0, wxEXPAND | wxTOP, 6);
    m_center_col->Add(new wxStaticLine(this), 0, wxEXPAND | wxTOP, 6);
    m_center_col->Add(m_info_bar, 0, wxEXPAND | wxTOP | wxBOTTOM, 3);
    m_left_sash = new LayerSash(this, _L("Tools panel resizer"),
        _L("Drag to resize the tools panel. Double-click resets the width; Left/Right arrow keys resize it when focused."),
        [this] { m_sash_origin_dip = m_left_dip = std::max(m_left_dip, ToDIP(m_left_col->GetSize().x)); },
        [this](int dx) { set_column_width(true, m_sash_origin_dip + ToDIP(dx), false); },
        [this] { set_column_width(true, m_left_dip, true); },
        [this](int step) { set_column_width(true, m_left_dip + step, true); },
        [this] { set_column_width(true, sdip(kLeftColumnDip), true); });
    m_right_sash = new LayerSash(this, _L("Side panel resizer"),
        _L("Drag to resize the Issues and Export panel. Double-click resets the width; Left/Right arrow keys resize it when focused."),
        [this] { m_sash_origin_dip = (m_ops_panel ? m_ops_dip : m_right_dip) = std::max(right_dip(), ToDIP((m_ops_panel ? static_cast<wxWindow *>(m_ops_panel)->GetSize() : m_right_col->GetSize()).x)); },
        [this](int dx) { set_column_width(false, m_sash_origin_dip - ToDIP(dx), false); },
        [this] { set_column_width(false, right_dip(), true); },
        [this](int step) { set_column_width(false, right_dip() - step, true); },
        [this] { set_column_width(false, sdip(kRightColumnDip), true); });
    m_body->Add(m_left_col, 0, wxEXPAND | wxLEFT, 6);
    m_body->Add(m_left_sash, 0, wxEXPAND | wxLEFT, 2);
    m_body->Add(m_center_col, 1, wxEXPAND);
    m_body->Add(m_right_sash, 0, wxEXPAND);
    m_body->Add(m_right_col, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
    build_paint_controls(root);
    root->Add(m_body, 1, wxEXPAND | wxBOTTOM, 2);
    build_preview_canvas();
    build_range_controls(root);
    build_range_filters(root);
    build_issue_panel(root);
    build_action_buttons(root);
    // Widen the tools column when its controls need more than the stored/default width (never narrower than before).
    if (m_left_needed_dip > m_left_dip) {
        m_left_dip = std::min(sdip(kLeftMaxDip), m_left_needed_dip);
        m_left_col->SetMinSize(FromDIP(m_left_dip), -1);
    }
    SetSizer(root);
    bind_events();
    const bool side = !wxGetApp().app_config || wxGetApp().app_config->get("layer_editor_side_panel") != "0";
    set_side_panel_visible(side);
    wxGetApp().UpdateDarkUI(this);
    refresh();
    Scroll(0, 0);
    CallAfter([this] { Scroll(0, 0); });   // the first focus must not leave the header scrolled out of view
    m_source_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { update_source_status(); }, m_source_timer.GetId());
    m_source_timer.Start(250);
}

int LayerEditorContent::spin_min_width(wxSpinCtrl *ctrl) const
{
    return std::max(FromDIP(64), ctrl->GetBestSize().x);
}

int LayerEditorContent::hint_width() const
{
    const int reserved = FromDIP(m_left_dip + (m_ops_panel ? m_ops_dip : m_side_visible ? m_right_dip : 0) + 150);
    return std::max(240, GetClientSize().x - reserved);
}

int LayerEditorContent::tool_text_width() const { return std::max(FromDIP(m_left_dip), m_left_col->GetSize().x) - 8; }

int LayerEditorContent::side_width() const { return std::max(FromDIP(m_right_dip), m_right_col->GetSize().x) - 8; }

int LayerEditorContent::clamp_column_width(bool left, int dip) const
{
    int high = sdip(left ? kLeftMaxDip : kRightMaxDip);
    const int client = ToDIP(GetClientSize().x);
    if (client > 0) {   // never squeeze the canvas below its minimum
        const int other = left ? (m_ops_panel ? m_ops_dip : m_side_visible ? m_right_dip : 0) : m_left_dip;
        high = std::min(high, std::max(sdip(left ? kLeftMinDip : kRightMinDip), client - other - kCanvasMinDip - 48));
    }
    const int low = left ? std::max(sdip(kLeftMinDip), m_left_needed_dip) : sdip(kRightMinDip);
    return std::clamp(dip, std::min(low, high), high);
}

void LayerEditorContent::set_column_width(bool left, int dip, bool persist)
{
    dip = clamp_column_width(left, dip);
    int &stored = left ? m_left_dip : (m_ops_panel ? m_ops_dip : m_right_dip);
    if (dip != stored) {
        stored = dip;
        if (left) m_left_col->SetMinSize(FromDIP(dip), -1);
        else if (m_ops_panel) m_ops_panel->SetMinSize(wxSize(FromDIP(dip), -1));
        else m_right_col->SetMinSize(FromDIP(dip), -1);
        m_brush_hint_width = -1; m_plane_hint_width = -1;
        update_brush_value_hint(); update_paint_scope(); update_mask_plane_info();
        Layout(); FitInside();
        schedule_fit_refresh();
    }
    if (persist && wxGetApp().app_config && (left || !m_ops_panel))
        wxGetApp().app_config->set(left ? "layer_editor_left_width" : "layer_editor_right_width", std::to_string(stored));
}

void LayerEditorContent::section_heading(wxSizer *column, const wxString &title, bool first)
{
    auto *label = new wxStaticText(this, wxID_ANY, title);
    label->SetFont(label->GetFont().Bold());
    column->Add(label, 0, wxTOP, first ? 2 : 10);
    column->Add(new wxStaticLine(this), 0, wxEXPAND | wxTOP | wxBOTTOM, 3);
}

void LayerEditorContent::set_side_panel_visible(bool visible)
{
    m_side_visible = visible;
    update_side_columns();
    if (visible && m_format_note) update_format_note();   // ShowItems must not resurrect an intentionally empty note
    if (m_side_toggle) {
        m_side_toggle->SetLabel(visible ? _L("Hide side panel") : _L("Show side panel"));
        m_side_toggle->SetToolTip(visible ? _L("Hide the Issues and Export column to give the mask more room.")
                                          : _L("Show the Issues and Export column again."));
    }
    if (wxGetApp().app_config) wxGetApp().app_config->set("layer_editor_side_panel", visible ? "1" : "0");
    update_source_status();
    Layout(); FitInside();
    schedule_fit_refresh();
}

// The right-hand slot holds either the Issues/Export column (when the user keeps it visible) or, while it is open,
// the docked Operations panel. Never both.
void LayerEditorContent::update_side_columns()
{
    m_body->Show(m_right_col, m_side_visible && !m_ops_panel, true);
    if (m_ops_panel) m_body->Show(m_ops_panel, true, true);
    m_body->Show(m_right_sash, m_side_visible || m_ops_panel);
}

void LayerEditorContent::set_document_actions(std::function<void()> load_slices, std::function<void()> open_file)
{
    m_load_slices_action = std::move(load_slices);
    m_open_file_action = std::move(open_file);
    for (auto *button : {m_load_slices_button, m_open_file_button}) if (button) button->Show();
    Layout(); FitInside();
}

void LayerEditorContent::set_document_actions_enabled(bool load_slices, bool open_file)
{
    if (m_load_slices_button && m_load_slices_button->IsEnabled() != load_slices) m_load_slices_button->Enable(load_slices);
    if (m_open_file_button && m_open_file_button->IsEnabled() != open_file) m_open_file_button->Enable(open_file);
}

void LayerEditorContent::schedule_fit_refresh()
{
    if (m_fit_pending || !m_cols || m_working || m_stroke || m_panning) return;
    if (m_zoom_scale > 0 || m_zoom->GetSelection() > 0) return;   // only the Fit mode follows the window
    m_fit_pending = true;
    CallAfter([this] {
        m_fit_pending = false;
        if (m_working || m_stroke || m_panning || !m_cols || m_viewport->GetClientSize() == m_fit_avail) return;
        try { refresh(); } catch (...) {}
    });
}

// One status bar under the canvas: layer, drawing tool, pixel inspector (updated on mouse move), zoom.
void LayerEditorContent::update_view_info()
{
    if (!m_view_info) return;
    const auto set = [](wxStaticText *label, const wxString &text) { if (text != label->GetLabel()) label->SetLabel(text); };
    set(m_status_layer, wxString::Format(_L("Layer %d/%zu"), m_layer->GetValue(), m_document.layer_count()));
    const wxString dot = wxString::FromUTF8(" \xc2\xb7 ");
    set(m_status_tool, m_brush.GetStringSelection() + dot + m_tool.GetStringSelection() + dot +
        wxString::Format("r%d", m_radius->GetValue()) + dot + wxString::Format("%d", m_value->GetValue()));
    m_status_tool->SetToolTip(wxString::Format(_L("%s, %s, %s tip, radius %d px, grayscale value %d"),
        m_brush.GetStringSelection(), m_tool.GetStringSelection(), m_shape.GetStringSelection().Lower(),
        m_radius->GetValue(), m_value->GetValue()));
    const bool fit = !(m_zoom_scale > 0 || m_zoom->GetSelection() > 0);
    wxString zoom = fit ? wxString(_L("Fit")) : wxString(_L("Zoom"));
    if (m_cols) zoom += wxString::Format(" %d%%", int(std::lround(effective_zoom() * 100.)));
    set(m_view_info, zoom);
}

namespace {
// Vector icon for a tool button, drawn in the theme's button text colour so it reads in the light and the dark theme.
wxBitmapBundle tool_icon(wxWindow *window, const char *shapes)
{
    const wxColour c = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
    const wxString hex = wxString::Format("#%02x%02x%02x", c.Red(), c.Green(), c.Blue());
    wxString body(shapes);
    body.Replace("@C@", hex);
    const wxString svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"" + hex +
        "\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">" + body + "</svg>";
    const wxScopedCharBuffer data = svg.utf8_str();
    return wxBitmapBundle::FromSVG(data.data(), window->FromDIP(wxSize(18, 18)));
}
}

// Brush / eraser, brush tip and shape tool as rows of icon toggle buttons (one row = one ToolGroup).
void LayerEditorContent::build_tool_toggles(wxSizer *column)
{
    const auto add = [&](ToolGroup &group, wxSizer *row, const wxString &label, const wxString &tip, const char *shapes) {
        auto *button = new wxBitmapToggleButton(this, wxID_ANY, tool_icon(this, shapes), wxDefaultPosition, FromDIP(wxSize(34, 30)));
        button->SetToolTip(wxString(label) + ": " + tip);
        button->SetName(label);
#ifdef __WXGTK__
        // A pressed tool must be obvious in the light theme too: accent border + tinted background (the app's orange).
        {
            GtkCssProvider *css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css,
                "button:checked { background-image: none; background-color: alpha(#ed6b21, 0.30); border-color: #ed6b21;"
                " box-shadow: inset 0 0 0 2px #ed6b21; }"
                "button:checked:hover { background-color: alpha(#ed6b21, 0.42); }", -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(button->GetHandle()), GTK_STYLE_PROVIDER(css),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
        }
#endif
        set_accessible_name(button, label, tip, AccessibleRole::ToggleButton);
        const size_t index = group.add(button, label);
        button->Bind(wxEVT_TOGGLEBUTTON, [this, &group, index](wxCommandEvent &) {
            group.SetSelection(int(index));
            if (group.on_change) group.on_change();
        });
        row->Add(button, 0, wxRIGHT, 3);
    };
    auto *tool_row = new wxBoxSizer(wxHORIZONTAL);
    add(m_brush, tool_row, _L("Brush"), _L("paints the selected grayscale value. Drag directly on the mask."),
        "<path d=\"M19 4l-8 8\"/><path d=\"M10.5 12.5c-2.6-.6-4.500 1-4.500 3.200 0 1.800-1 2.800-2.500 3.300 4.500 1.700 9.500.3 8.500-4.800z\" fill=\"@C@\" stroke-width=\"1.500\"/>");
    add(m_brush, tool_row, _L("Eraser"), _L("clears pixels (paints the palette's black level)."),
        "<path d=\"M4 15.500L13.500 6a2 2 0 0 1 2.800 0l3.700 3.700a2 2 0 0 1 0 2.800L13 19.500H7z\"/><path d=\"M9 10.500l6.500 6.500\"/><path d=\"M13 19.500h8\"/>");
    tool_row->AddSpacer(FromDIP(10));
    add(m_shape, tool_row, _L("Round brush"), _L("round stamp for Freehand and Line, 2r+1 pixels across."),
        "<circle cx=\"12\" cy=\"12\" r=\"6.500\" fill=\"@C@\"/>");
    add(m_shape, tool_row, _L("Square brush"), _L("square stamp for Freehand and Line, 2r+1 pixels across."),
        "<rect x=\"5.500\" y=\"5.500\" width=\"13\" height=\"13\" fill=\"@C@\"/>");
    column->Add(tool_row, 0, wxBOTTOM, 4);
    auto *shape_row = new wxBoxSizer(wxHORIZONTAL);
    add(m_tool, shape_row, "Freehand", "follows the mouse.", "<path d=\"M3 16c3-9 6 3 9-3s5-5 9-3\"/>");
    add(m_tool, shape_row, "Line", "drag from start to end; uses the brush tip and radius.", "<path d=\"M5 19L19 5\"/>");
    add(m_tool, shape_row, _L("Rectangle"), _L("drag a corner to the opposite corner; it becomes the region offered to Operations."), "<rect x=\"4\" y=\"6.500\" width=\"16\" height=\"11\"/>");
    add(m_tool, shape_row, _L("Ellipse"), _L("drag across the bounding box."), "<ellipse cx=\"12\" cy=\"12\" rx=\"8.500\" ry=\"6\"/>");
    add(m_tool, shape_row, _L("Flood fill"), _L("fills the connected region of identical grey on click. Each use is one Undo step for the whole layer range."),
        "<path d=\"M5 12l7-7 7 7-6 6a2 2 0 0 1-2.800 0z\"/><path d=\"M20.500 15c1.400 1.800 1.400 3.500 0 3.500s-1.400-1.700 0-3.500z\" fill=\"@C@\"/>");
    column->Add(shape_row, 0, wxBOTTOM, 6);
    m_brush.SetSelection(0); m_tool.SetSelection(0); m_shape.SetSelection(0);
}

void LayerEditorContent::build_paint_controls(wxBoxSizer *root) {
    std::array<bool,256> levels{};
    for(const auto value:m_edit_projection)levels[value]=true;
    // Header: what is open and its state, the document commands, history, side panel and the way back.
    auto *header = new wxBoxSizer(wxHORIZONTAL);
    auto *painting_heading = new wxStaticText(this, wxID_ANY, _L("Layer editor"));
    painting_heading->SetFont(painting_heading->GetFont().Bold());
    header->Add(painting_heading, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
    m_session_status = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_session_status->SetMinSize(wxSize(FromDIP(60), -1));
    header->Add(m_session_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
    m_header_tools = new wxBoxSizer(wxHORIZONTAL);
    if (m_text_factor > 1.25) {
        // Large text: the document buttons no longer fit next to the title, so they get their own row under it.
        auto *rows = new wxBoxSizer(wxVERTICAL);
        rows->Add(header, 0, wxEXPAND);
        rows->Add(m_header_tools, 0, wxALIGN_RIGHT | wxRIGHT, 4);
        root->Add(rows, 0, wxEXPAND | wxTOP | wxBOTTOM, 3);
    } else {
        header->Add(m_header_tools, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        root->Add(header, 0, wxEXPAND | wxTOP | wxBOTTOM, 3);
    }
    root->Add(new wxStaticLine(this), 0, wxEXPAND);

    auto *left = m_left_col;
    // One row per setting: caption on the left, control stretched to a common width on the right.
    auto make_grid = [&] {
        auto *grid = new wxFlexGridSizer(2, 5, 8);
        grid->AddGrowableCol(1);
        left->Add(grid, 0, wxEXPAND);
        return grid;
    };
    auto caption = [&](wxFlexGridSizer *grid, const wxString &label) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    };
    auto spin = [&](wxFlexGridSizer *grid, const wxString &label, int low, int high, int value) {
        // A caption too long for "caption + spin button" in the default column goes above its control instead of
        // widening the column (the canvas keeps its width).
        const bool stacked = GetTextExtent(label).x + 8 + FromDIP(96) + FromDIP(20) > FromDIP(sdip(kLeftColumnDip));
        if (stacked) left->Add(new wxStaticText(this, wxID_ANY, label), 0, wxTOP, 5);
        else caption(grid, label);
        auto *ctrl = new wxSpinCtrl(this, wxID_ANY);
        ctrl->SetRange(low, high); ctrl->SetValue(value);
        // The native spin button needs room for its entry plus the "-" and "+" buttons; a narrower slot clips the "+".
        const int spin_min = spin_min_width(ctrl);
        ctrl->SetMinSize(wxSize(spin_min, -1));
        if (stacked) { left->Add(ctrl, 0, wxEXPAND | wxTOP, 3); return ctrl; }
        m_left_needed_dip = std::max(m_left_needed_dip, ToDIP(GetTextExtent(label).x + 8 + spin_min) + 20);
        grid->Add(ctrl, 1, wxEXPAND); return ctrl;
    };
    section_heading(left, _L("Navigate"), true);
    auto *nav = make_grid();
    m_layer = spin(nav, _L("Layer"), 1, int(m_document.layer_count()), 1);
    m_layer->SetToolTip(_L("Layer shown and painted. PageUp / PageDown on the mask moves one layer."));
    if (m_document.mask_plane_count()>1) {
        caption(nav, _L("Mask plane"));
        m_plane=make_choice(this);m_plane->Append(_L("Main mask"));m_plane->Append(_L("SUBIMGS mask"));
        m_plane->SetSelection(0);m_plane->SetName("Mask plane");nav->Add(m_plane,1,wxEXPAND);
        m_plane_info=new wxStaticText(this,wxID_ANY,"");
    }
    section_heading(left, _L("Draw"));
    build_tool_toggles(left);
    auto *tools = make_grid();
    m_radius = spin(tools, _L("Radius (pixels)"), 0, 4096, 3);
    m_radius->SetName("Brush radius (pixels)");
    m_value = spin(tools, _L("Grayscale / threshold"), 0, 255, 255);
    // View bar above the canvas: zoom and fit controls next to the thing they act on.
    m_view_bar = new wxBoxSizer(wxHORIZONTAL);
    m_center_col->Insert(0, m_view_bar, 0, wxEXPAND | wxBOTTOM, 4);
    m_view_bar->Add(new wxStaticText(this, wxID_ANY, _L("Zoom")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
    m_zoom = make_choice(this);
    for (const wxString label : std::initializer_list<wxString>{_L("Fit"), "25%", "50%", "100%", "200%", "400%"}) m_zoom->Append(label);
    m_zoom->SetSelection(0); m_zoom->SetName("Preview zoom");
    m_zoom->SetToolTip(_L("Ctrl+wheel zooms at the cursor, wheel scrolls, middle-drag or Space+drag pans, keys + - zoom in/out, 0 fits the window, 1 shows 100%."));
    m_view_bar->Add(m_zoom, 0, wxALIGN_CENTER_VERTICAL);
    auto *fit = new wxButton(this, wxID_ANY, _L("Fit view"));
    fit->SetToolTip("Fit the whole mask into the window (key 0). Follows the window size.");
    auto *actual = new wxButton(this, wxID_ANY, "1:1");
    actual->SetToolTip("Show the mask at 100% (key 1): one mask pixel per screen pixel.");
    m_view_bar->Add(fit, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);
    m_view_bar->Add(actual, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 4);
    fit->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { zoom_to(1., viewport_centre(), true); });
    actual->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { zoom_to(1., viewport_centre()); });

    if (m_document.fresh_job() && m_document.fresh_job()->position_policy == sla::ExposurePositionPolicy::RepeatedExposureHeights) {
        m_exposure_info = new wxStaticText(this, wxID_ANY, "");
        m_hint_box->Add(m_exposure_info, 0, wxLEFT | wxRIGHT | wxTOP, 4);
    }
    if (m_plane_info) m_hint_box->Add(m_plane_info,0,wxLEFT | wxRIGHT | wxTOP,4);
    // One short line (full explanation in the tooltip) instead of the former wrapped paragraph.
    m_brush_value_info=new wxStaticText(this,wxID_ANY,"");
    m_brush_value_info->SetName("Brush grayscale mapping");
    wxString allowed=_L("Brush source palette: ")+wxString::Format("%u",m_edit_level_count)+_L(" levels. Allowed source paint values: ");
    if(m_edit_level_count==256)allowed+="0–255";
    else for(size_t value=0;value<levels.size();++value)if(levels[value]) {
        if(allowed.Last()!=' ')allowed+=", ";
        allowed+=wxString::Format("%u",unsigned(value));
    }
    allowed+=". The requested value is kept; Threshold compares against the raw value.";
    m_brush_value_info->SetToolTip(allowed);
    m_value->SetToolTip(_L("The requested value is kept. Brush and Fill follow the source palette; Threshold compares against this raw value."));
    left->Add(m_brush_value_info, 0, wxEXPAND | wxTOP, 3);
    m_paint_scope = new wxStaticText(this, wxID_ANY, "");
    m_paint_scope->SetName("Painting scope");
    m_paint_scope->SetToolTip(_L("A stroke edits the current layer only, or every layer of the selected range when "
                              "'Overlay and paint selected range' is on. One Undo restores the whole stroke."));
}

wxPoint LayerEditorContent::viewport_centre() const
{
    const auto size = m_viewport->GetClientSize();
    return wxPoint(size.x / 2, size.y / 2);
}

// Neutral dark surround: a subtle checker, a hairline frame around the mask. Identical in the light and dark theme
// (the mask's black and white edges must stay readable on it).
void LayerEditorContent::paint_viewport_background(wxDC &dc)
{
    const wxPoint origin = m_viewport->GetViewStart();   // scroll rate 1: units are pixels
    const wxSize client = m_viewport->GetClientSize();
    const int tile = std::max(8, FromDIP(14));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(40, 42, 46)));
    dc.DrawRectangle(origin.x, origin.y, client.x, client.y);
    dc.SetBrush(wxBrush(wxColour(46, 48, 53)));
    for (int y = origin.y - origin.y % tile; y < origin.y + client.y; y += tile)
        for (int x = origin.x - origin.x % tile; x < origin.x + client.x; x += tile)
            if (((x / tile) + (y / tile)) % 2) dc.DrawRectangle(x, y, tile, tile);
    wxRect mask = m_preview->GetRect();
    if (mask.width < 2 || mask.height < 2) return;
    mask.Offset(origin);
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(wxColour(24, 25, 28), 1));
    dc.DrawRectangle(mask.Inflate(2));
    dc.SetPen(wxPen(wxColour(150, 154, 162), 1));
    dc.DrawRectangle(mask.Deflate(1));
}

void LayerEditorContent::build_preview_canvas() {
    m_viewport = new wxScrolledWindow(this);
    m_viewport->SetMinSize(wxSize(FromDIP(kCanvasMinDip), FromDIP(240)));
    m_viewport->SetBackgroundColour(wxColour(40, 42, 46));
    m_viewport->SetBackgroundStyle(wxBG_STYLE_PAINT);
    // 1 px units keep cursor-anchored zoom exact; wheel steps are handled below.
    m_viewport->SetScrollRate(1, 1);
    m_viewport->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(m_viewport);
        m_viewport->DoPrepareDC(dc);
        paint_viewport_background(dc);
    });
    auto *preview_sizer = new wxBoxSizer(wxVERTICAL);
    // A real drawing window keeps a stable mouse event/capture target.
    // GTK's static-image control is unsuitable as an interactive canvas.
    m_preview = new wxPanel(m_viewport, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS);
    m_preview->SetName("Layer mask preview");
    m_preview->SetCursor(wxCursor(wxCURSOR_CROSS));
    m_preview->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_preview->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(m_preview);
        dc.Clear();
        if (m_preview_bitmap.IsOk()) dc.DrawBitmap(m_preview_bitmap, 0, 0, false);
        paint_issue_overlay(dc);
    });
    // The mask is centred in the viewport (the sizer is laid out over the whole virtual area).
    preview_sizer->AddStretchSpacer(1);
    preview_sizer->Add(m_preview, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, kCanvasMargin);
    preview_sizer->AddStretchSpacer(1);
    m_viewport->SetSizer(preview_sizer);
}

void LayerEditorContent::build_range_controls(wxBoxSizer *root) {
    auto *left = m_left_col;
    section_heading(left, _L("Selection range"));
    left->Add(new wxStaticText(this, wxID_ANY, _L("Range: first / last layer")), 0, wxBOTTOM, 3);
    auto *first_last = new wxBoxSizer(wxHORIZONTAL);
    m_first = new wxSpinCtrl(this, wxID_ANY); m_last = new wxSpinCtrl(this, wxID_ANY);
    m_first->SetRange(1, int(m_document.layer_count())); m_last->SetRange(1, int(m_document.layer_count()));
    m_first->SetValue(1); m_last->SetValue(int(m_document.layer_count()));
    m_first->SetName("First layer"); m_last->SetName("Last layer");
    m_first->SetMinSize(wxSize(spin_min_width(m_first), -1)); m_last->SetMinSize(wxSize(spin_min_width(m_last), -1));
    m_left_needed_dip = std::max(m_left_needed_dip, ToDIP(m_first->GetMinSize().x + m_last->GetMinSize().x + 6) + 20);
    first_last->Add(m_first, 1, wxRIGHT, 6); first_last->Add(m_last, 1);
    left->Add(first_last, 0, wxEXPAND);
    m_paint_range = new wxCheckBox(this, wxID_ANY, _L("Overlay and paint selected range"));
    m_paint_range->SetName("Overlay and paint selected range");
    m_paint_range->SetToolTip(_L("Maximum-intensity overlay for preview. A stroke edits every layer in the inclusive range, preserving pixels outside the stroke. One Undo restores the entire range."));
    left->Add(m_paint_range, 0, wxTOP, 6);
    left->Add(m_paint_scope, 0, wxEXPAND | wxTOP | wxBOTTOM, 3);   // says what the current stroke will touch
    auto *reload_preview = new wxButton(this, wxID_ANY, _L("Refresh preview"));
    reload_preview->SetToolTip(_L("Prepare the selected layer or overlay again after cancelling. This does not change masks or Undo history."));
    set_button_icon(reload_preview, "refresh");
    left->Add(reload_preview, 0, wxEXPAND | wxTOP, 6);
    reload_preview->SetMinSize(wxSize(-1, FromDIP(kRowDip)));
    reload_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { perform([] {}); });
    m_paint_range->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { perform([]{}); });
    m_first->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { if (m_first->GetValue()>m_last->GetValue()) m_last->SetValue(m_first->GetValue()); perform([]{}); });
    m_last->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { if (m_last->GetValue()<m_first->GetValue()) m_first->SetValue(m_last->GetValue()); perform([]{}); });
    // Canvas with the vertical layer slider on its right edge.
    m_canvas_row->Add(m_viewport, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);
    auto *vertical_range = new wxBoxSizer(wxVERTICAL);
    m_range_slider = new LayerRangeSlider(this, int(m_document.layer_count()),
        [this](const LayerRangeSelection &selection, bool committed) {
            m_layer->SetValue(selection.current());
            m_first->SetValue(selection.first());
            m_last->SetValue(selection.last());
            m_paint_range->SetValue(selection.range_mode());
            m_single_layer->SetValue(!selection.range_mode());
            // A drag changes only selection labels. Decode/composite the
            // selected masks once when released, not for every mouse move.
            if (committed) perform([] {});
            else update_paint_scope();
        });
    vertical_range->Add(m_range_slider, 1, wxEXPAND | wxLEFT | wxRIGHT, 2);
    m_single_layer = new wxCheckBox(this, wxID_ANY, _L("Single layer"));
    m_single_layer->SetName("Single layer");
    m_single_layer->SetValue(true);
    m_single_layer->SetToolTip(_L("Checked: show and paint only the current layer. Unchecked: use both handles and overlay the selected range. The previous range is kept."));
    m_single_layer->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        m_paint_range->SetValue(!m_single_layer->GetValue());
        perform([] {});
    });
    // The checkbox sits at the right end of the view bar above the canvas: a column of its own would cost ~100 px of width.
    m_view_bar->AddStretchSpacer(1);
    m_view_bar->Add(m_single_layer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
    m_canvas_row->Add(vertical_range, 0, wxEXPAND);
}

void LayerEditorContent::build_range_filters(wxBoxSizer *root) {
    // Filter of the selected layer range: next to the range it acts on (left panel).
    auto *left = m_left_col;
    left->AddSpacer(FromDIP(8));
    auto *filter = new wxFlexGridSizer(2, 5, 8);
    filter->AddGrowableCol(1);
    filter->Add(new wxStaticText(this, wxID_ANY, _L("Filter")), 0, wxALIGN_CENTER_VERTICAL);
    m_operation = make_choice(this);
    for (const wxString label : std::initializer_list<wxString>{_L("Threshold"), _L("Invert"), _L("Mirror horizontal"), _L("Mirror vertical"), _L("Rotate 180 degrees"), _L("Fill with grayscale"),
                              _L("Erode"), _L("Dilate"), _L("Morphological open"), _L("Morphological close"), _L("Gradient"), _L("White top hat"), _L("Black top hat"), _L("Box blur"), _L("Median blur")})
        m_operation->Append(label);
    m_operation->SetSelection(0); m_operation->SetName("Range operation");
    m_operation->SetToolTip(_L("Filters use Radius (pixels) and an odd square kernel. Morphology and box blur reflect edges; median blur replicates edges and removes isolated noise. Grayscale is preserved without thresholding."));
    m_operation->SetMinSize(wxSize(FromDIP(60), -1));
    filter->Add(m_operation, 1, wxEXPAND);
    left->Add(filter, 0, wxEXPAND);
    auto *apply_range = new wxButton(this, wxID_ANY, _L("Apply to range"));
    apply_range->SetToolTip(_L("Modify the inclusive range as one undo step. Cancellation preserves all layers."));
    apply_range->SetMinSize(wxSize(-1, FromDIP(kRowDip)));
    left->Add(apply_range, 0, wxEXPAND | wxTOP, 5);
    apply_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { perform([this] { apply_range_operation(); }); }); });

    // Status bar under the canvas: layer | tool | pixel inspector | zoom. Ellipsized with a small minimum: a long
    // reading must never widen the canvas column.
    const auto separator = [this] {
        m_info_bar->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    };
    m_status_layer = new wxStaticText(this, wxID_ANY, "Layer 1");
    m_status_layer->SetName("Layer status");
    m_status_layer->SetMinSize(wxSize(m_text_factor > 1.25 ? m_status_layer->GetTextExtent("Layer 999/999").x : m_status_layer->GetTextExtent("Layer 99999/99999").x, -1));
    m_info_bar->Add(m_status_layer, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 6);
    separator();
    m_status_tool = new wxStaticText(this, wxID_ANY, _L("Brush"), wxDefaultPosition, wxDefaultSize, m_text_factor > 1.25 ? wxST_ELLIPSIZE_END : 0);
    m_status_tool->SetName("Tool status");
    // Large text: let the status items ellipsize (the tooltip has the full text) so the centre column can shrink.
    m_status_tool->SetMinSize(wxSize(m_text_factor > 1.25 ? FromDIP(110) :
        m_status_tool->GetTextExtent(wxString::FromUTF8("Eraser \xc2\xb7 Flood fill \xc2\xb7 r4096 \xc2\xb7 255")).x, -1));
    m_info_bar->Add(m_status_tool, 0, wxALIGN_CENTER_VERTICAL);
    separator();
    // Short idle text so that it fits next to the other status fields even with the Operations panel open; the whole
    // sentence is the tooltip.
    m_pixel_info = new wxStaticText(this, wxID_ANY, _L("Pixel: –"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_pixel_info->SetToolTip(_L("Pixel: move the cursor over the mask"));
    m_pixel_info->SetMinSize(wxSize(FromDIP(80), -1));
    m_pixel_info->SetName("Pixel inspector");
    m_info_bar->Add(m_pixel_info, 1, wxALIGN_CENTER_VERTICAL);
    separator();
    m_view_info = new wxStaticText(this, wxID_ANY, _L("Fit"), wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
    m_view_info->SetName("Zoom status");
    m_view_info->SetMinSize(wxSize(m_view_info->GetTextExtent("Zoom 1600%").x, -1));
    m_info_bar->Add(m_view_info, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
}

void LayerEditorContent::set_button_icon(wxButton *button, const char *icon)
{
    if (auto *bundle = get_bmp_bundle(icon, 16)) {
        button->SetBitmap(*bundle);
        button->SetBitmapPosition(wxLEFT);
        button->SetBitmapMargins(FromDIP(4), 0);
    }
}

void LayerEditorContent::build_action_buttons(wxBoxSizer *root) {
    // Header and layer-action toolbar first (they were created in this order historically).
    build_edit_buttons(m_ops_bar);
    auto *out = m_right_col;
    build_format_section(out, out);
    m_export_action = [this] { run_export_action(); };
    // Output group: the primary action first (bold, taller, icon), then secondary exports and the project commands.
    auto *export_button = add_action_button(out, m_print ? (m_print->is_raster_job() ? _L("Export generated layers") : _L("Export edited layers")) : (sla::find_layer_image_format(m_document.archive_format()) ? _L("Save edited image") : _L("Save edited archive")), m_export_action, wxEXPAND | wxTOP, 3, "export_plater");
    export_button->SetFont(export_button->GetFont().Bold());
    export_button->SetMinSize(wxSize(-1, FromDIP(kPrimaryRowDip)));
    if(!m_print && !m_document.parameter_capabilities().empty())
        add_action_button(out, _L("Convert archive"), [this] { convert_archive(); }, wxEXPAND | wxTOP, 5);
    out->AddSpacer(FromDIP(10));
    add_action_button(out, _L("Export current mask"), [this] { export_mask_images(false); }, wxEXPAND | wxTOP, 3);
    add_action_button(out, _L("Export mask range"), [this] { export_mask_images(true); }, wxEXPAND | wxTOP, 3);
    out->AddSpacer(FromDIP(10));
    build_project_buttons(out);
    build_project_settings_buttons(out);
    // The way back, set apart from the editing commands: leaving may ask to discard.
    m_header_tools->AddSpacer(FromDIP(14));
    add_action_button(m_header_tools, _L("Back to 3D"), [this] { if (m_close) m_close(); }, wxALL, 3);
}

wxButton * LayerEditorContent::add_action_button(wxSizer *buttons, const wxString &text, std::function<void()> action,
                                                 int flags, int border, const char *icon) {
    auto *b = new wxButton(this, wxID_ANY, text); buttons->Add(b, 0, flags, border);
    b->SetMinSize(wxSize(-1, FromDIP(kRowDip)));
    if (icon) set_button_icon(b, icon);
    b->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent &) {
        CallAfter([this, action] { perform(action); });
    });
    return b;
}

void LayerEditorContent::build_edit_buttons(wxSizer *buttons) {
    // Header: [Load current slices] [Open layer file] | [Undo] [Redo] | [Hide side panel] ... [Back to 3D].
    const auto document_button = [this](const wxString &label, const wxString &tip, const char *icon, std::function<void()> &action) {
        auto *b = new wxButton(this, wxID_ANY, label);
        b->SetToolTip(tip);
        b->SetMinSize(wxSize(-1, FromDIP(kRowDip)));
        set_button_icon(b, icon);
        b->Hide();   // shown once the Plater supplies the action (set_document_actions)
        b->Bind(wxEVT_BUTTON, [&action](wxCommandEvent &) { if (action) action(); });
        m_header_tools->Add(b, 0, wxALL, 3);
        return b;
    };
    m_load_slices_button = document_button(_L("Load current slices"),
        _L("Replace the editing session with the current sliced result. Unsaved edits are confirmed first."), "refresh", m_load_slices_action);
    m_open_file_button = document_button(_L("Open layer file"),
        _L("Open an archive, image or layer project as an independent editing document."), "open", m_open_file_action);
    const auto separator = [this] {
        m_header_tools->AddSpacer(FromDIP(3));
        m_header_tools->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL), 0, wxEXPAND | wxTOP | wxBOTTOM, 4);
        m_header_tools->AddSpacer(FromDIP(3));
    };
    separator();
    m_undo = add_action_button(m_header_tools, _L("Undo"), [this] { m_document.undo(); }, wxALL, 3, "undo_toolbar");
    m_redo = add_action_button(m_header_tools, _L("Redo"), [this] { m_document.redo(); }, wxALL, 3, "redo_toolbar");
    separator();
    m_side_toggle = new wxButton(this, wxID_ANY, _L("Hide side panel"));
    m_side_toggle->SetMinSize(wxSize(-1, FromDIP(kRowDip)));
    m_header_tools->Add(m_side_toggle, 0, wxALL, 3);
    m_side_toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { set_side_panel_visible(!m_side_visible); });
    // Layer actions (one compact row, wraps on a narrow canvas) sit directly under the canvas they act on.
    if (!m_document.parameter_capabilities().empty())
        add_action_button(buttons, _L("Layer parameters"),[this]{edit_layer_parameters();}, wxRIGHT | wxBOTTOM, 4);
    if (m_document.fresh_job())
        add_action_button(buttons, _L("Range parameters"),[this]{edit_layer_parameters(true);}, wxRIGHT | wxBOTTOM, 4);
    add_action_button(buttons, _L("Operations..."), [this] { open_operations_panel(); }, wxRIGHT | wxBOTTOM, 4, "cog");
    buttons->AddSpacer(FromDIP(10));
    add_action_button(buttons, _L("Threshold layer"), [this] { m_document.threshold(size_t(m_layer->GetValue()-1), uint8_t(m_value->GetValue()),selected_plane()); }, wxRIGHT | wxBOTTOM, 4);
    add_action_button(buttons, _L("Invert layer"), [this] { m_document.invert(size_t(m_layer->GetValue()-1),selected_plane()); }, wxRIGHT | wxBOTTOM, 4);
}

void LayerEditorContent::build_format_section(wxBoxSizer *root, wxSizer *buttons) {
    section_heading(root, _L("Export and project"), false);
    if (m_print) build_export_format_controls(root, buttons);
    else if (!anycubic_available_versions(m_document.archive_format()).empty()) build_profile_controls(root);
    else build_archive_note(root);
}

void LayerEditorContent::build_export_format_controls(wxBoxSizer *root, wxSizer *buttons) {
    m_format = make_choice(this);
    m_format->SetName("Layer export format");
    auto preferred = m_print->is_raster_job() ?
        m_project_settings->printer.values.at("sla_archive_format") :
        sla::preferred_export_format(
            wxGetApp().preset_bundle->printers.get_selected_preset().config.opt_string("sla_archive_format"),
            wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_string("sla_archive_format"),
            wxGetApp().app_config->sla_export_format(wxGetApp().preset_bundle->printers.get_selected_preset_source_name("printer_settings_id")),
            wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_string("sla_anycubic_version"));
    if (preferred.empty() || preferred == "SL1SVG" || !archive_export_unavailable_reason(preferred.c_str()).empty())
        preferred = m_print->printer_config().sla_archive_format.value;
    if (m_print->is_raster_job() && (preferred == "SL1SVG" || !archive_export_unavailable_reason(preferred.c_str()).empty()))
        preferred = "uvj";
    for (const auto &entry : registered_sla_archives()) {
        if (!archive_export_unavailable_reason(entry.id).empty() || !std::strcmp(entry.id, "SL1SVG")) continue;
        if (m_document.fresh_job() && m_document.fresh_job()->position_policy == sla::ExposurePositionPolicy::RepeatedExposureHeights &&
            std::strcmp(entry.id, "uvj") && std::strcmp(entry.id, "cxdlpv4")) continue;
        m_formats.push_back(&entry);
        m_format->Append(wxString::FromUTF8(entry.desc) + " (." + wxString::FromUTF8(entry.ext) + ")");
        if (entry.id == preferred)
            m_format->SetSelection(int(m_formats.size()-1));
    }
    if (m_format->GetSelection() == wxNOT_FOUND && !m_formats.empty()) m_format->SetSelection(0);
    m_format->SetMinSize(wxSize(FromDIP(100), -1));   // long format names are elided, they must not widen the column
    root->Add(m_format, 0, wxEXPAND | wxBOTTOM, 4);
    if (m_print->is_raster_job()) m_format->Bind(wxEVT_COMBOBOX,[this](wxCommandEvent &event) {
        m_project_settings->printer.values["sla_archive_format"]=m_formats.at(size_t(m_format->GetSelection()))->id;
        refresh(); event.Skip();
    });
    m_format_note = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_format_note, 0, wxEXPAND | wxBOTTOM, 4);
    m_format->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &event) {
        update_format_note();
        // Continue to the project-settings handler registered above.
        // Updating the hint must not consume the selected format.
        event.Skip();
    });
    update_format_note();
    if (!m_print->is_raster_job()) {
        m_apply_button = add_action_button(buttons, _L("Apply to sliced result"), [this] { apply_changes(); }, wxEXPAND | wxBOTTOM, 6);
        m_apply_button->SetFont(m_apply_button->GetFont().Bold());
        m_apply_button->SetMinSize(wxSize(-1, FromDIP(kPrimaryRowDip)));
    }
}

// Long format explanations live in a collapsed pane: the text stays reachable (and accessible)
// without pushing the export buttons down. `text` is not wrapped by the caller.
void LayerEditorContent::add_format_note(wxBoxSizer *root, const wxString &text)
{
    auto *pane = new wxCollapsiblePane(this, wxID_ANY, _L("About this format"));
    auto *inner = pane->GetPane();
    auto *note = new wxStaticText(inner, wxID_ANY, text);
    note->Wrap(side_width());
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(note, 0, wxEXPAND | wxALL, 4);
    inner->SetSizer(sizer);
    pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent &) { Layout(); FitInside(); });
    root->Add(pane, 0, wxEXPAND | wxBOTTOM, 4);
}

void LayerEditorContent::build_profile_controls(wxBoxSizer *root) {
    add_format_note(root, _L("Anycubic edits keep the original file version, per-layer parameters, previews and manufacturer metadata. Masks are quantized to the original grayscale or PWS AA levels. Volume and model bounds follow the edited masks when physical dimensions are known. Weight and cost scale existing estimates; zero values do not provide material properties. Changed masks append payloads; older unused payloads remain in the file."));
    auto *use_profile = new wxCheckBox(this, wxID_ANY, m_reopened_profile ?
        _L("Use saved project context for resin estimates") : _L("Use selected printer and material for resin estimates"));
    m_use_profile=use_profile;
    use_profile->SetValue(m_export_profile.has_value());
    use_profile->SetToolTip(_L("Opt in to recalculate volume, weight and cost. The selected printer must match the mask resolution and known screen dimensions. Masks and exposure settings are preserved. Legacy PWC cannot store physical screen dimensions; keep its matching profile when reopening."));
    root->Add(use_profile, 0, wxBOTTOM, 4);
    m_profile_summary = new wxStaticText(this, wxID_ANY, _L("Resin estimates: original file values"));
    root->Add(m_profile_summary, 0, wxEXPAND | wxBOTTOM, 4);
    m_update_profile_summary=[this]{show_profile_summary();};
    // Controls used by refresh are already constructed at this point.
    if (m_export_profile) {
        const auto &profile=*m_export_profile;
        m_profile_summary->SetLabel(wxString::Format(_L("Saved project estimates: %.4g x %.4g mm; %.4g g/ml; %.4g per ml; max Z %.4g mm. Active presets are unchanged."),
            profile.screen_width_mm, profile.screen_height_mm, profile.density_g_ml, profile.cost_per_ml, profile.max_print_height_mm));
        m_profile_summary->Wrap(side_width());
    }
    // Leave the GTK toggle callback before opening a modal error dialog.
    m_use_profile->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { CallAfter([this] { on_use_profile_toggled(); }); });
}

void LayerEditorContent::build_archive_note(wxBoxSizer *root) {
    if (sla::is_ctb_mask_format(m_document.archive_format())) {
        add_format_note(root, _L("Chitubox edits are quantized immediately to the original grayscale or AA levels. Undo and preview match saved pixels. Unchanged layers, original parameters and previews are preserved. Edited masks append new payloads; original unused payloads remain in the file."));
    } else if(m_document.archive_format()=="cxdlpv4") {
        add_format_note(root, _L("CXDLPv4 edits use 7-bit grayscale. Values 0 and 1 are off; other values are quantized immediately so Undo and the preview match the saved file. Original per-layer settings and previews are preserved."));
    } else if (const auto *image_format = sla::find_layer_image_format(m_document.archive_format())) {
        add_format_note(root, image_format->binary ?
            _L("PBM has only black and white pixels. Zero stays black; nonzero values become white so the preview and Undo match the saved mask.") :
            image_format->lossy ? _L("JPEG is lossy. Edits remain lossless in this session; saving JPEG may change pixel values. Export a PNG mask to preserve them exactly.") :
            _L("This file contains one mask. It has no physical printer dimensions, exposure settings or layer height."));
    } else if (m_document.archive_format() == "genericzip" || m_document.archive_format() == "vda") {
        add_format_note(root, _L("This archive contains masks and geometry. Exposure and motion are configured on the receiving printer."));
    }
}

void LayerEditorContent::build_project_buttons(wxSizer *buttons) {
    if (!m_print || m_print->is_raster_job())
        add_action_button(buttons, _L("Save layer project"), [this] { save_layer_project_impl(true); }, wxEXPAND | wxTOP, 3, "save");
}

void LayerEditorContent::build_project_settings_buttons(wxSizer *buttons) {
    if (!m_print || m_print->is_raster_job()) {
        if (m_print && m_print->calibration_recipe())
            add_action_button(buttons, _L("Calibration recipe"), [this] { show_calibration_recipe(); }, wxEXPAND | wxTOP, 3);
        if (!m_print)
            add_action_button(buttons, _L("Capture selected SLA settings"), [this] { capture_selected_settings(); }, wxEXPAND | wxTOP, 3);
        add_action_button(buttons, _L("Project SLA settings"), [this] { show_project_settings(); }, wxEXPAND | wxTOP, 3, "settings");
    }
}

} // namespace layer_editor_detail
} // namespace Slic3r::GUI
