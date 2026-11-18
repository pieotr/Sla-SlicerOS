#include "LayerEditorContent.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
// Read-only drop-down for the editor controls. WHY not wxChoice: on GTK3 a wxChoice is a GtkComboBox whose list is a
// GtkMenu with a pointer grab. On Plasma/Wayland through XWayland (and in the private KWin test session, with a bare
// Gtk.ComboBoxText too) a click that has no pointer motion between press and release selects the entry under the pointer,
// i.e. the list "closes by itself" right after the click. The application's own ComboBox (DropDown popup) is used for
// every preset/settings drop-down and does not depend on GTK menu grabs.
static ComboBox *make_choice(wxWindow *parent)
{
    return create_readonly_combo(parent);
}

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
    SetScrollRate(16,16);
    auto *root = new wxBoxSizer(wxVERTICAL);
    build_paint_controls(root);
    build_preview_canvas();
    build_range_controls(root);
    build_range_filters(root);
    build_issue_panel(root);
    build_action_buttons(root);
    SetSizer(root);
    bind_events();
    refresh();
    m_source_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { update_source_status(); }, m_source_timer.GetId());
    m_source_timer.Start(250);
}

void LayerEditorContent::build_paint_controls(wxBoxSizer *root) {
    std::array<bool,256> levels{};
    for(const auto value:m_edit_projection)levels[value]=true;
    m_session_status = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_session_status, 0, wxEXPAND | wxALL, 8);
    auto *painting_heading = new wxStaticText(this, wxID_ANY, "Paint masks — brush, eraser and layer range");
    painting_heading->SetFont(painting_heading->GetFont().Bold());
    root->Add(painting_heading, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    auto *controls = new wxWrapSizer(wxHORIZONTAL);
    auto spin = [&](const char *label, int low, int high, int value) {
        controls->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
        auto *ctrl = new wxSpinCtrl(this, wxID_ANY);
        ctrl->SetRange(low, high); ctrl->SetValue(value); controls->Add(ctrl, 0, wxALL, 5); return ctrl;
    };
    m_layer = spin("Layer", 1, int(m_document.layer_count()), 1);
    if (m_document.mask_plane_count()>1) {
        controls->Add(new wxStaticText(this,wxID_ANY,"Mask plane"),0,wxALL|wxALIGN_CENTER_VERTICAL,5);
        m_plane=make_choice(this);m_plane->Append("Main mask");m_plane->Append("SUBIMGS mask");
        m_plane->SetSelection(0);m_plane->SetName("Mask plane");controls->Add(m_plane,0,wxALL,5);
        m_plane_info=new wxStaticText(this,wxID_ANY,"");
    }
    controls->Add(new wxStaticText(this, wxID_ANY, "Drawing tool"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    m_brush = make_choice(this);
    m_brush->Append("Brush"); m_brush->Append("Eraser"); m_brush->SetSelection(0);
    m_brush->SetName("Drawing tool"); controls->Add(m_brush, 0, wxALL, 5);
    m_brush->SetToolTip("Brush paints the selected grayscale value. Eraser clears pixels. Drag directly on the mask preview.");
    controls->Add(new wxStaticText(this, wxID_ANY, "Shape tool"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    m_tool = make_choice(this);
    for (const char *label : {"Freehand", "Line", "Rectangle", "Ellipse", "Flood fill"}) m_tool->Append(label);
    m_tool->SetSelection(0); m_tool->SetName("Shape tool"); controls->Add(m_tool, 0, wxALL, 5);
    m_tool->SetToolTip("Freehand follows the mouse. Line, Rectangle and Ellipse are drawn by dragging; Flood fill fills the connected region of identical grey on click. Each use is one Undo step for the whole layer range.");
    controls->Add(new wxStaticText(this, wxID_ANY, "Brush shape"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    m_shape = make_choice(this);
    m_shape->Append("Round"); m_shape->Append("Square"); m_shape->SetSelection(0);
    m_shape->SetName("Brush shape"); controls->Add(m_shape, 0, wxALL, 5);
    m_shape->SetToolTip("Stamp used by Freehand and Line. Size 2r+1 pixels across.");
    m_radius = spin("Radius (pixels)", 0, 4096, 3);
    m_radius->SetName("Brush radius (pixels)");
    m_value = spin("Grayscale / threshold", 0, 255, 255);
    controls->Add(new wxStaticText(this, wxID_ANY, "Zoom"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    m_zoom = make_choice(this);
    for (const char *label : {"Fit", "25%", "50%", "100%", "200%", "400%"}) m_zoom->Append(label);
    m_zoom->SetSelection(0); m_zoom->SetName("Preview zoom");
    m_zoom->SetToolTip("Ctrl+wheel zooms at the cursor, wheel scrolls, middle-drag or Space+drag pans, keys + - 0 zoom in/out/fit.");
    controls->Add(m_zoom, 0, wxALL, 5);
    root->Add(controls, 0, wxEXPAND);
    if (m_document.fresh_job() && m_document.fresh_job()->position_policy == sla::ExposurePositionPolicy::RepeatedExposureHeights) {
        m_exposure_info = new wxStaticText(this, wxID_ANY, "");
        root->Add(m_exposure_info, 0, wxALL, 8);
    }
    if (m_plane_info) root->Add(m_plane_info,0,wxALL,8);
    m_brush_value_info=new wxStaticText(this,wxID_ANY,"");
    m_brush_value_info->SetName("Brush grayscale mapping");
    wxString allowed="Allowed source paint values: ";
    if(m_edit_level_count==256)allowed+="0–255";
    else for(size_t value=0;value<levels.size();++value)if(levels[value]) {
        if(allowed.Last()!=' ')allowed+=", ";
        allowed+=wxString::Format("%u",unsigned(value));
    }
    m_brush_value_info->SetToolTip(allowed);
    m_value->SetToolTip("The requested value is kept. Brush and Fill follow the source palette; Threshold compares against this raw value.");
    root->Add(m_brush_value_info,0,wxEXPAND|wxLEFT|wxRIGHT|wxTOP,8);
    m_paint_scope = new wxStaticText(this, wxID_ANY, "");
    m_paint_scope->SetName("Painting scope");
    root->Add(m_paint_scope, 0, wxLEFT | wxRIGHT | wxTOP, 8);
}

void LayerEditorContent::build_preview_canvas() {
    m_viewport = new wxScrolledWindow(this);
    m_viewport->SetMinSize(wxSize(200,200));
    // 1 px units keep cursor-anchored zoom exact; wheel steps are handled below.
    m_viewport->SetScrollRate(1, 1);
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
    preview_sizer->Add(m_preview, 0, wxALL, 4);
    m_viewport->SetSizer(preview_sizer);
}

void LayerEditorContent::build_range_controls(wxBoxSizer *root) {
    auto *range_controls = new wxWrapSizer(wxHORIZONTAL);
    range_controls->Add(new wxStaticText(this, wxID_ANY, "Range: first / last layer"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    m_first = new wxSpinCtrl(this, wxID_ANY); m_last = new wxSpinCtrl(this, wxID_ANY);
    m_first->SetRange(1, int(m_document.layer_count())); m_last->SetRange(1, int(m_document.layer_count()));
    m_first->SetValue(1); m_last->SetValue(int(m_document.layer_count()));
    m_first->SetName("First layer"); m_last->SetName("Last layer");
    range_controls->Add(m_first, 0, wxALL, 5); range_controls->Add(m_last, 0, wxALL, 5);
    m_paint_range = new wxCheckBox(this, wxID_ANY, "Overlay and paint selected range");
    m_paint_range->SetName("Overlay and paint selected range");
    m_paint_range->SetToolTip("Maximum-intensity overlay for preview. A stroke edits every layer in the inclusive range, preserving pixels outside the stroke. One Undo restores the entire range.");
    range_controls->Add(m_paint_range, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    auto *reload_preview = new wxButton(this, wxID_ANY, "Refresh preview");
    reload_preview->SetToolTip("Prepare the selected layer or overlay again after cancelling. This does not change masks or Undo history.");
    range_controls->Add(reload_preview, 0, wxALL, 5);
    reload_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { perform([] {}); });
    m_paint_range->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { perform([]{}); });
    m_first->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { if (m_first->GetValue()>m_last->GetValue()) m_last->SetValue(m_first->GetValue()); perform([]{}); });
    m_last->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { if (m_last->GetValue()<m_first->GetValue()) m_first->SetValue(m_last->GetValue()); perform([]{}); });
    // Keep painting scope next to the brush, above the expanding canvas.
    // Range filters remain a separate action below the preview.
    root->Add(range_controls, 0, wxEXPAND);
    auto *image_and_range = new wxBoxSizer(wxHORIZONTAL);
    image_and_range->Add(m_viewport, 1, wxEXPAND | wxALL, 4);
    auto *vertical_range = new wxBoxSizer(wxVERTICAL);
    vertical_range->Add(new wxStaticText(this, wxID_ANY, "Layer range"), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 4);
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
    vertical_range->Add(m_range_slider, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);
    m_single_layer = new wxCheckBox(this, wxID_ANY, "Single layer");
    m_single_layer->SetName("Single layer");
    m_single_layer->SetValue(true);
    m_single_layer->SetToolTip("Checked: show and paint only the current layer. Unchecked: use both handles and overlay the selected range. The previous range is kept.");
    m_single_layer->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        m_paint_range->SetValue(!m_single_layer->GetValue());
        perform([] {});
    });
    vertical_range->Add(m_single_layer, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 4);
    image_and_range->Add(vertical_range, 0, wxEXPAND);
    root->Add(image_and_range, 1, wxEXPAND);
}

void LayerEditorContent::build_range_filters(wxBoxSizer *root) {
    m_pixel_info = new wxStaticText(this, wxID_ANY, "Pixel: move the cursor over the mask");
    m_pixel_info->SetName("Pixel inspector");
    root->Add(m_pixel_info, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    auto *range_controls = new wxWrapSizer(wxHORIZONTAL);
    range_controls->Add(new wxStaticText(this, wxID_ANY, "Range filters"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    m_operation = make_choice(this);
    for (const char *label : {"Threshold", "Invert", "Mirror horizontal", "Mirror vertical", "Rotate 180 degrees", "Fill with grayscale",
                              "Erode", "Dilate", "Morphological open", "Morphological close", "Gradient", "White top hat", "Black top hat", "Box blur", "Median blur"})
        m_operation->Append(label);
    m_operation->SetSelection(0); m_operation->SetName("Range operation");
    m_operation->SetToolTip("Filters use Radius (pixels) and an odd square kernel. Morphology and box blur reflect edges; median blur replicates edges and removes isolated noise. Grayscale is preserved without thresholding.");
    range_controls->Add(m_operation, 0, wxALL, 5);
    auto *apply_range = new wxButton(this, wxID_ANY, "Apply to range");
    apply_range->SetToolTip("Modify the inclusive range as one undo step. Cancellation preserves all layers.");
    range_controls->Add(apply_range, 0, wxALL, 5);
    root->Add(range_controls, 0, wxEXPAND);
    apply_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { perform([this] { apply_range_operation(); }); }); });
}

void LayerEditorContent::build_action_buttons(wxBoxSizer *root) {
    auto *buttons = new wxWrapSizer(wxHORIZONTAL);
    build_edit_buttons(buttons);
    build_format_section(root, buttons);
    build_project_buttons(buttons);
    m_export_action = [this] { run_export_action(); };
    add_action_button(buttons, m_print ? (m_print->is_raster_job() ? "Export generated layers" : "Export edited layers") : (sla::find_layer_image_format(m_document.archive_format()) ? "Save edited image" : "Save edited archive"), m_export_action);
    if(!m_print && !m_document.parameter_capabilities().empty())
        add_action_button(buttons, "Convert archive", [this] { convert_archive(); });
    add_action_button(buttons, "Export current mask", [this] { export_mask_images(false); });
    add_action_button(buttons, "Export mask range", [this] { export_mask_images(true); });
    add_action_button(buttons, "Back to 3D", [this] { if (m_close) m_close(); });
    root->Add(buttons, 0, wxEXPAND);
}

wxButton * LayerEditorContent::add_action_button(wxSizer *buttons, const char *text, std::function<void()> action) {
    auto *b = new wxButton(this, wxID_ANY, text); buttons->Add(b, 0, wxALL, 5);
    b->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent &) {
        CallAfter([this, action] { perform(action); });
    });
    return b;
}

void LayerEditorContent::build_edit_buttons(wxSizer *buttons) {
    if (!m_document.parameter_capabilities().empty())
        add_action_button(buttons, "Layer parameters",[this]{edit_layer_parameters();});
    if (m_document.fresh_job())
        add_action_button(buttons, "Range parameters",[this]{edit_layer_parameters(true);});
    add_action_button(buttons, "Operations...", [this] { open_operations_dialog(); });
    m_undo = add_action_button(buttons, "Undo", [this] { m_document.undo(); });
    m_redo = add_action_button(buttons, "Redo", [this] { m_document.redo(); });
    add_action_button(buttons, "Threshold layer", [this] { m_document.threshold(size_t(m_layer->GetValue()-1), uint8_t(m_value->GetValue()),selected_plane()); });
    add_action_button(buttons, "Invert layer", [this] { m_document.invert(size_t(m_layer->GetValue()-1),selected_plane()); });
}

void LayerEditorContent::build_format_section(wxBoxSizer *root, wxSizer *buttons) {
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
    root->Add(m_format, 0, wxEXPAND | wxALL, 5);
    if (m_print->is_raster_job()) m_format->Bind(wxEVT_COMBOBOX,[this](wxCommandEvent &event) {
        m_project_settings->printer.values["sla_archive_format"]=m_formats.at(size_t(m_format->GetSelection()))->id;
        refresh(); event.Skip();
    });
    m_format_note = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_format_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
    m_format->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &event) {
        update_format_note();
        // Continue to the project-settings handler registered above.
        // Updating the hint must not consume the selected format.
        event.Skip();
    });
    update_format_note();
    if (!m_print->is_raster_job()) m_apply_button = add_action_button(buttons, "Apply to sliced result", [this] { apply_changes(); });
}

void LayerEditorContent::build_profile_controls(wxBoxSizer *root) {
    auto *note = new wxStaticText(this, wxID_ANY, "Anycubic edits keep the original file version, per-layer parameters, previews and manufacturer metadata. Masks are quantized to the original grayscale or PWS AA levels. Volume and model bounds follow the edited masks when physical dimensions are known. Weight and cost scale existing estimates; zero values do not provide material properties. Changed masks append payloads; older unused payloads remain in the file.");
    note->Wrap(650); root->Add(note, 0, wxEXPAND|wxALL, 5);
    auto *use_profile = new wxCheckBox(this, wxID_ANY, m_reopened_profile ?
        "Use saved project context for resin estimates" : "Use selected printer and material for resin estimates");
    m_use_profile=use_profile;
    use_profile->SetValue(m_export_profile.has_value());
    use_profile->SetToolTip("Opt in to recalculate volume, weight and cost. The selected printer must match the mask resolution and known screen dimensions. Masks and exposure settings are preserved. Legacy PWC cannot store physical screen dimensions; keep its matching profile when reopening.");
    root->Add(use_profile, 0, wxALL, 5);
    m_profile_summary = new wxStaticText(this, wxID_ANY, "Resin estimates: original file values");
    root->Add(m_profile_summary, 0, wxEXPAND|wxALL, 5);
    m_update_profile_summary=[this]{show_profile_summary();};
    // Controls used by refresh are already constructed at this point.
    if (m_export_profile) {
        const auto &profile=*m_export_profile;
        m_profile_summary->SetLabel(wxString::Format("Saved project estimates: %.4g x %.4g mm; %.4g g/ml; %.4g per ml; max Z %.4g mm. Active presets are unchanged.",
            profile.screen_width_mm, profile.screen_height_mm, profile.density_g_ml, profile.cost_per_ml, profile.max_print_height_mm));
        m_profile_summary->Wrap(650);
    }
    // Leave the GTK toggle callback before opening a modal error dialog.
    m_use_profile->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { CallAfter([this] { on_use_profile_toggled(); }); });
}

void LayerEditorContent::build_archive_note(wxBoxSizer *root) {
    if (sla::is_ctb_mask_format(m_document.archive_format())) {
        auto *note=new wxStaticText(this,wxID_ANY,"Chitubox edits are quantized immediately to the original grayscale or AA levels. Undo and preview match saved pixels. Unchanged layers, original parameters and previews are preserved. Edited masks append new payloads; original unused payloads remain in the file.");
        note->Wrap(650);root->Add(note,0,wxEXPAND|wxALL,5);
    } else if(m_document.archive_format()=="cxdlpv4") {
        auto *note=new wxStaticText(this,wxID_ANY,"CXDLPv4 edits use 7-bit grayscale. Values 0 and 1 are off; other values are quantized immediately so Undo and the preview match the saved file. Original per-layer settings and previews are preserved.");
        note->Wrap(650);root->Add(note,0,wxEXPAND|wxALL,5);
    } else if (const auto *image_format = sla::find_layer_image_format(m_document.archive_format())) {
        auto *note = new wxStaticText(this,wxID_ANY,image_format->binary ?
            "PBM has only black and white pixels. Zero stays black; nonzero values become white so the preview and Undo match the saved mask." :
            image_format->lossy ? "JPEG is lossy. Edits remain lossless in this session; saving JPEG may change pixel values. Export a PNG mask to preserve them exactly." :
            "This file contains one mask. It has no physical printer dimensions, exposure settings or layer height.");
        note->Wrap(650); root->Add(note,0,wxEXPAND|wxALL,5);
    } else if (m_document.archive_format() == "genericzip" || m_document.archive_format() == "vda") {
        auto *note = new wxStaticText(this, wxID_ANY, "This archive contains masks and geometry. Exposure and motion are configured on the receiving printer.");
        note->Wrap(650); root->Add(note, 0, wxEXPAND | wxALL, 5);
    }
}

void LayerEditorContent::build_project_buttons(wxSizer *buttons) {
    if (!m_print || m_print->is_raster_job()) {
        if (m_print && m_print->calibration_recipe())
            add_action_button(buttons, "Calibration recipe", [this] { show_calibration_recipe(); });
        if (!m_print)
            add_action_button(buttons, "Capture selected SLA settings", [this] { capture_selected_settings(); });
        add_action_button(buttons, "Project SLA settings", [this] { show_project_settings(); });
    }
    if (!m_print || m_print->is_raster_job())
        add_action_button(buttons, "Save layer project", [this] { save_layer_project_impl(true); });
}

} // namespace layer_editor_detail
} // namespace Slic3r::GUI
