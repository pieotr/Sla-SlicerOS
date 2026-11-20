#include "LayerEditorContent.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
void LayerEditorContent::update_source_status()
{
    if (!m_session_status) return;
    update_issue_state();
    const bool current = source_current();
    // Full sentence (tooltip + accessible name, the tests and screen readers rely on its prefix) and a short form for the
    // header line: at 1280x800 the document buttons leave only ~270 px, so the long sentence used to end in an ellipsis.
    wxString text, shorter;
    const auto say = [&](const wxString &full, const wxString &brief) { text = full; shorter = brief; };
    if (m_working) say(_L("Applying layer operation..."), _L("Applying operation..."));
    else if (!is_model_result()) {
        if (unsaved_changes()) say(_L("Layer document: modified. Save a layer project to keep masks and settings."), _L("Layer document: modified"));
        else if (has_changes()) say(_L("Generated layers: not saved or exported. Save a layer project or export a copy."), _L("Generated: not saved/exported"));
        else say(_L("Layer document: saved. Edits use this document's own settings."), _L("Layer document: saved"));
    } else if (!current) say(_L("Older slices: your edits are kept. Export a copy or load current slices. Apply is unavailable."), _L("Older slices: edits kept"));
    else if (unsaved_changes()) {
        if (m_document.dirty()) say(_L("Sliced layers: modified. Save the project or Apply to keep these edits in the model result."), _L("Sliced layers: modified"));
        else say(_L("Sliced layers: applied to the model. Save the 3D project to keep these edits after restart."), _L("Sliced layers: applied"));
    } else say(_L("Sliced layers: edits use the captured printer settings."), _L("Sliced layers: captured settings"));
    if (m_status_text != text) {   // one line in the header; the full text is the tooltip and the accessible name
        m_status_text = text;
        m_session_status->SetLabel(shorter); m_session_status->SetToolTip(text);
        set_accessible_name(m_session_status, text);
        if (wxGetApp().mainframe) wxGetApp().mainframe->update_title();
    }
    if (m_apply_button) m_apply_button->Enable(current && !m_working);
}

sla::LayerMaskPlane LayerEditorContent::selected_plane() const
{
    return m_plane && m_plane->GetSelection()==1 ? sla::LayerMaskPlane::Secondary : sla::LayerMaskPlane::Main;
}

void LayerEditorContent::perform(const std::function<void()> &action)
{
    if (m_working) return;
    m_preview_cancelled = false;
    m_op_preview.reset(); // any other action (layer change, brush, undo) ends an operation preview
    try { action(); refresh(); }
    catch (const std::exception &error) {
        wxMessageBox(wxString::FromUTF8(error.what()), _L("Layer editor"), wxOK | wxICON_ERROR, this);
    }
}

bool LayerEditorContent::handle_history_shortcut(const wxKeyEvent &event)
{
    if (!event.CmdDown() || event.AltDown()) return false;
    const int key = event.GetKeyCode();
    if (key != 'Z' && key != 'Y') return false;
    // All key routes discard the unfinished stroke before changing history.
    // In particular, a captured mouse may suppress the panel CHAR_HOOK.
    if (key == 'Y' || event.ShiftDown()) redo_change();
    else undo_change();
    return true;
}

void LayerEditorContent::update_brush_value_hint()
{
    if(!m_brush_value_info)return;
    const int requested=m_value->GetValue();
    const bool erasing=m_brush.GetSelection()==1;
    const wxString arrow=wxString::FromUTF8(" \xe2\x86\x92 ");
    wxString message;
    if(erasing)message=wxString::Format(_L("Eraser paints %u (requested %d kept)"),unsigned(m_edit_projection[0]),requested);
    else if(requested<0 || requested>255)
        message=_L("Grayscale must be 0–255; input kept");
    else message=wxString::Format(_L("Requested %d"),requested)+arrow+wxString::Format(_L("paints %u"),unsigned(m_edit_projection[size_t(requested)]));
    const int width=tool_text_width();
    if(message==m_brush_hint_text && width==m_brush_hint_width)return;
    m_brush_hint_text=message;m_brush_hint_width=width;
    m_brush_value_info->SetLabel(message);m_brush_value_info->Wrap(width);
    Layout();
}

void LayerEditorContent::update_paint_scope()
{
    if (m_paint_scope) {
        const bool range = m_paint_range->GetValue();
        m_paint_scope->SetLabel(range ? wxString::Format(_L("Painting layers %d–%d"), m_first->GetValue(), m_last->GetValue()) :
                                        wxString::Format(_L("Painting layer %d"), m_layer->GetValue()));
        m_paint_scope->Wrap(tool_text_width());
    }
}

void LayerEditorContent::update_mask_plane_info()
{
    if (!m_plane_info) return;
    wxString message;
    if (m_paint_range->GetValue()) {
        // A range may mix shared and independent masks, and the current
        // layer may lie outside it. Describe the actual brush targets.
        const int first = m_first->GetValue(), last = m_last->GetValue();
        if (first < 1 || last < first || size_t(last) > m_document.layer_count()) {
            message = _L("Choose a valid layer range to see which masks will change.");
        } else {
            size_t shared = 0;
            for (size_t layer = size_t(first - 1); layer < size_t(last); ++layer)
                shared += m_document.mask_is_shared(layer) ? 1 : 0;
            const size_t independent = size_t(last - first + 1) - shared;
            message = wxString::Format(
                _L("Selected range: %zu shared %s, %zu independent %s. "
                "Shared layers update both masks; independent layers update only the selected mask."),
                shared, wxString(shared == 1 ? "layer" : "layers"),
                independent, wxString(independent == 1 ? "layer" : "layers"));
        }
    } else {
        message = m_document.mask_is_shared(size_t(m_layer->GetValue() - 1)) ?
            _L("Shared mask: main and SUBIMGS stay linked when edited.") :
            _L("Independent masks: edits affect only the selected image.");
    }
    const int width = hint_width();
    if (message == m_plane_hint_text && width == m_plane_hint_width) return;
    m_plane_hint_text = message; m_plane_hint_width = width;
    m_plane_info->SetLabel(message); m_plane_info->Wrap(width);
    Layout();
}

void LayerEditorContent::clear_preview(const wxString &message)
{
    m_preview_bitmap = wxBitmap();
    m_view_image = nullptr;
    m_cols = m_rows = 0;
    set_accessible_name(m_preview, "Layer mask preview", message.empty() ? wxString("No layer mask shown") : message, AccessibleRole::Image);
    m_preview->Refresh(false);
    m_paint_scope->SetLabel(message);
    m_paint_scope->Wrap(tool_text_width());
}

const png::ImageGreyscale *LayerEditorContent::prepared_preview()
{
    const bool range = m_paint_range->GetValue();
    const size_t first = size_t((range ? m_first : m_layer)->GetValue() - 1);
    const size_t last = size_t((range ? m_last : m_layer)->GetValue() - 1);
    const auto plane = selected_plane();
    if (const auto *image = m_preview_cache.find(m_document, first, last, plane)) {
        if (std::getenv("SLA_TRACE_LAYER_PREVIEW"))
            BOOST_LOG_TRIVIAL(info) << "Layer preview reused: first=" << first << "; last=" << last
                << "; plane=" << int(plane);
        return image;
    }
    // Nested refreshes after a cancelled stroke/operation must not open a
    // second progress window. A subsequent user action can retry explicitly.
    if (m_preview_cancelled) return nullptr;
    clear_preview(_L("Preparing layer preview…"));
    std::optional<sla::LayerPreviewCache::Prepared> prepared;
    std::atomic<bool> cancel_requested{false};
    bool loaded = false;
    try {
        {
            // The modal worker excludes edits and panel destruction while
            // it borrows PNG bytes. Join before publishing or touching wx.
            TaskLock working(m_working);
            loaded = run_background_layers(this, _L("Prepare layer preview"), [&](const auto &progress) {
                try {
                    prepared.emplace(sla::LayerPreviewCache::prepare(m_document, first, last, plane, progress));
                    return true;
                } catch (const sla::LayerOperationCancelled &) { return false; }
            }, [&] { cancel_requested.store(true, std::memory_order_relaxed); });
        }
        if (!loaded || cancel_requested.load(std::memory_order_relaxed)) {
            m_preview_cancelled = true;
            clear_preview(_L("Preview cancelled. Use Refresh preview or choose another layer or range."));
            return nullptr;
        }
        m_preview_cache.publish(std::move(*prepared), m_document, first, last, plane);
    } catch (...) {
        m_preview_cancelled = true;
        clear_preview(_L("Preview unavailable. Use Refresh preview or choose another layer or range."));
        throw;
    }
    if (std::getenv("SLA_TRACE_LAYER_PREVIEW"))
        BOOST_LOG_TRIVIAL(info) << "Layer preview prepared: first=" << first << "; last=" << last
            << "; plane=" << int(plane) << "; decoded=" << last-first+1;
    update_paint_scope();
    return m_preview_cache.find(m_document, first, last, plane);
}

void LayerEditorContent::sync_layer_ranges()
{
    const int count = int(m_document.layer_count());
    if (count < 1 || m_layer->GetMax() == count) return;
    for (auto *spin : {m_layer, m_first, m_last}) {
        spin->SetRange(1, count);
        spin->SetValue(std::clamp(spin->GetValue(), 1, count));
    }
    if (m_last->GetValue() < m_first->GetValue()) m_last->SetValue(m_first->GetValue());
}

void LayerEditorContent::refresh()
{
    if (m_working) return;
    sync_layer_ranges();
    m_view_image = nullptr;
    update_brush_value_hint();
    update_mask_plane_info();
    if (!m_preview_cancelled) update_paint_scope();
    if (m_range_slider) {
        LayerRangeSelection selection(int(m_document.layer_count()));
        selection.set_current(m_layer->GetValue());
        selection.set_bounds(m_first->GetValue(), m_last->GetValue());
        selection.set_range_mode(m_paint_range->GetValue());
        m_range_slider->set_selection(selection);
        m_single_layer->SetValue(!selection.range_mode());
    }
    m_loaded_view.reset();
    const png::ImageGreyscale *selected = m_stroke ? &*m_stroke : m_op_preview ? &*m_op_preview : nullptr;
    if (!selected && m_reuse_preview) selected = prepared_preview();
    if (!selected && !m_reuse_preview) {
        m_loaded_view = m_paint_range->GetValue() ?
            m_document.overlay(size_t(m_first->GetValue()-1), size_t(m_last->GetValue()-1), selected_plane()) :
            m_document.image(size_t(m_layer->GetValue()-1), selected_plane());
        selected = &*m_loaded_view;
    }
    if (!selected) {
        // No drawing is possible until this exact selection has a preview.
        // Keep history controls accurate even if an edit's refresh is cancelled.
        m_undo->Enable(m_document.can_undo()); m_redo->Enable(m_document.can_redo());
        update_source_status(); Layout(); FitInside();
        return;
    }
    const auto &image = *selected;
    m_cols = image.cols; m_rows = image.rows;
    m_view_image = selected;
    m_view_geometry = m_document.raster_geometry();
    if (m_exposure_info) {
        const auto &fresh = *m_document.fresh_job();
        size_t physical_count = 0, selected_physical = 0;
        double previous_z = 0;
        const size_t selected = size_t(m_layer->GetValue()-1);
        for (size_t i = 0; i < fresh.rows.size(); ++i) {
            const double z = fresh.rows[i].at(sla::LayerParameter::PositionZ);
            if (z > previous_z) ++physical_count;
            if (i <= selected) selected_physical = physical_count;
            previous_z = z;
        }
        m_exposure_info->SetLabel(wxString::Format(_L("Exposure %zu/%zu; physical layer %zu/%zu; Z %.6g mm"),
            selected+1, fresh.rows.size(), selected_physical, physical_count,
            fresh.rows[selected].at(sla::LayerParameter::PositionZ)));
    }
    static constexpr double zooms[] = {0., .25, .5, 1., 2., 4.};
    const auto available = m_viewport->GetClientSize();
    m_fit_avail = available;
    const int zoom_choice = m_zoom->GetSelection();
    const double scale = m_zoom_scale > 0 ? m_zoom_scale : zoom_choice <= 0 ?
        // Fit shows the whole mask in the viewport: it shrinks big masks and (wave 35) also enlarges small ones, up to the
        // largest zoom. The scale may be fractional; picking and painting divide by the actual preview size, so every
        // screen pixel still maps to exactly one mask pixel. "1:1" stays exactly 100%.
        std::min({layer_zoom_max, double(std::max(320, available.x-2*kCanvasMargin-2)) / m_cols,
                  double(std::max(200, available.y-2*kCanvasMargin-2)) / m_rows}) : zooms[zoom_choice];
    const int width = std::max(1, int(m_cols*scale));
    const int height = std::max(1, int(m_rows*scale));
    if (uint64_t(width)*height > 32*1024*1024 || width > 16384 || height > 16384)
        throw RuntimeError("This zoom exceeds the preview memory budget. Choose a smaller zoom.");
    // Downsample directly from grayscale; don't allocate a full-size RGB copy.
    wxImage preview{width, height};
    auto *rgb = preview.GetData();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto gray = image.get(size_t(y)*m_rows/height, size_t(x)*m_cols/width);
            const size_t i = size_t(y)*width + x;
            rgb[3*i] = rgb[3*i+1] = rgb[3*i+2] = gray;
        }
    m_preview_bitmap = wxBitmap(preview);
    set_accessible_name(m_preview, "Layer mask preview", wxString::Format("Layer mask %d by %d pixels, shown at %d by %d", int(m_cols), int(m_rows), width, height), AccessibleRole::Image);
    m_preview->Refresh(false);
    m_preview->SetMinSize(wxSize(width, height));
    m_preview->SetSize(width, height);
    m_viewport->FitInside();
    m_viewport->Layout();
    m_viewport->Refresh(false);   // the surround and the frame around the mask follow the new size
    m_undo->Enable(m_document.can_undo()); m_redo->Enable(m_document.can_redo());
    update_source_status();
    update_view_info();
    if (wxGetApp().mainframe) wxGetApp().mainframe->update_title();
    Layout();
    FitInside();
}

wxPoint LayerEditorContent::pixel_at(const wxMouseEvent &event) const
{
    const auto size = m_preview->GetClientSize();
    return {layer_canvas_pixel(event.GetX(), size.x, int(m_cols)),
            layer_canvas_pixel(event.GetY(), size.y, int(m_rows))};
}

sla::LayerDocument::ShapeOp LayerEditorContent::current_shape_op(wxPoint end) const
{
    using Doc = sla::LayerDocument;
    Doc::ShapeOp op;
    op.tool = Doc::ShapeTool(std::max(0, m_stroke_tool-1));
    op.brush = m_stroke_shape; op.radius = m_stroke_radius; op.value = m_stroke_value;
    op.x0 = m_stroke_start.x; op.y0 = m_stroke_start.y; op.x1 = end.x; op.y1 = end.y;
    return op;
}

void LayerEditorContent::update_stroke_preview(wxPoint from, wxPoint to)
{
    using Doc = sla::LayerDocument;
    if (m_stroke_tool == 0) {
        if (m_stroke_shape == Doc::BrushShape::Round)
            Doc::draw_line(*m_stroke, from.x, from.y, to.x, to.y, m_stroke_radius, m_stroke_value);
        else
            Doc::draw_stroke(*m_stroke, from.x, from.y, to.x, to.y, m_stroke_radius, m_stroke_value, m_stroke_shape);
    } else if (m_stroke_base) {
        *m_stroke = *m_stroke_base;
        Doc::apply_shape(*m_stroke, current_shape_op(to));
    }
}

void LayerEditorContent::update_pixel_info(const wxMouseEvent *event)
{
    if (!m_pixel_info) return;
    wxString text = _L("Pixel: –");
    const auto size = m_preview->GetClientSize();
    if (event && m_view_image && m_cols && m_rows &&
        layer_canvas_contains(event->GetX(), size.x) && layer_canvas_contains(event->GetY(), size.y)) {
        const auto pixel = pixel_at(*event);
        const unsigned grey = m_view_image->get(size_t(pixel.y), size_t(pixel.x));
        text = wxString::Format(_L("Pixel x=%d y=%d"), pixel.x, pixel.y);
        if (m_view_geometry && m_view_geometry->width_mm > 0 && m_view_geometry->height_mm > 0)
            text += wxString::Format("  |  %.3f, %.3f mm",
                layer_pixel_to_mm(pixel.x, int(m_cols), m_view_geometry->width_mm),
                layer_pixel_to_mm(pixel.y, int(m_rows), m_view_geometry->height_mm));
        text += wxString::Format(_L("  |  grey %u%s"), grey, m_paint_range->GetValue() ? _L(" (range overlay)") : "");
    }
    if (text != m_pixel_info->GetLabel()) m_pixel_info->SetLabel(text);
}

double LayerEditorContent::effective_zoom() const
{
    return m_cols ? double(m_preview->GetClientSize().x) / double(m_cols) : 1.;
}

void LayerEditorContent::zoom_to(double new_scale, wxPoint viewport_point, bool fit)
{
    if (m_working || m_stroke || m_panning || !m_preview_bitmap.IsOk() || !m_cols) return;
    const double old_scale = effective_zoom();
    const wxPoint preview_at = m_preview->GetPosition(); // viewport-client coordinates
    const wxPoint view_start = m_viewport->GetViewStart(); // scroll rate is 1: units are px
    const wxPoint canvas_cursor = viewport_point - preview_at;
    const double target = layer_zoom_clamp(new_scale);
    const double previous_scale = m_zoom_scale;
    const int previous_choice = m_zoom->GetSelection();
    const auto restore = [&] {
        m_zoom_scale = previous_scale; m_zoom->SetSelection(previous_choice);
    };
    if (fit) { m_zoom_scale = 0; m_zoom->SetSelection(0); }
    else {
        static constexpr double zooms[] = {0., .25, .5, 1., 2., 4.};
        m_zoom_scale = target; m_zoom->SetSelection(wxNOT_FOUND);
        for (int i = 1; i < 6; ++i)
            if (std::abs(zooms[i]-target) < 1e-9) { m_zoom->SetSelection(i); m_zoom_scale = 0; }
    }
    m_preview_cancelled = false;
    try { refresh(); }
    catch (const std::exception &error) {
        restore();
        try { refresh(); } catch (...) {}
        wxMessageBox(wxString::FromUTF8(error.what()), _L("Layer editor"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (fit) { m_viewport->Scroll(0, 0); return; }
    const wxPoint origin = preview_at + view_start; // canvas origin in scrolled content
    m_viewport->Scroll(
        int(layer_zoom_anchor_scroll(canvas_cursor.x, viewport_point.x, old_scale, effective_zoom(), origin.x)),
        int(layer_zoom_anchor_scroll(canvas_cursor.y, viewport_point.y, old_scale, effective_zoom(), origin.y)));
}

wxPoint LayerEditorContent::viewport_point(const wxMouseEvent &event) const
{
    return m_viewport->ScreenToClient(m_preview->ClientToScreen(event.GetPosition()));
}

void LayerEditorContent::begin_pan(const wxMouseEvent &event)
{
    m_panning = true;
    m_pan_origin_screen = m_preview->ClientToScreen(event.GetPosition());
    m_pan_start_scroll = m_viewport->GetViewStart();
    m_preview->SetCursor(wxCursor(wxCURSOR_HAND));
    if (!m_preview->HasCapture()) m_preview->CaptureMouse();
}

void LayerEditorContent::end_pan()
{
    if (!m_panning) return;
    m_panning = false;
    if (m_preview->HasCapture()) m_preview->ReleaseMouse();
    m_preview->SetCursor(wxCursor(wxCURSOR_CROSS));
}

void LayerEditorContent::finish_stroke(bool commit)
{
    if (!m_stroke) return;
    const bool range = m_paint_range->GetValue();
    const auto first = size_t((range ? m_first : m_layer)->GetValue()-1);
    const auto last = size_t((range ? m_last : m_layer)->GetValue()-1);
    auto points = std::move(m_stroke_points);
    const int tool = m_stroke_tool;
    const auto shape_op = current_shape_op(m_last_pixel);
    if (commit && tool == 2 && m_cols && m_rows) { // remember the rectangle as a ROI candidate
        const int x0 = std::clamp(std::min(shape_op.x0, shape_op.x1), 0, int(m_cols) - 1);
        const int y0 = std::clamp(std::min(shape_op.y0, shape_op.y1), 0, int(m_rows) - 1);
        const int x1 = std::clamp(std::max(shape_op.x0, shape_op.x1), 0, int(m_cols) - 1);
        const int y1 = std::clamp(std::max(shape_op.y0, shape_op.y1), 0, int(m_rows) - 1);
        m_last_rectangle = layerops::Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }
    m_view_image = nullptr; m_stroke_base.reset();
    // Release capture before the modal progress window so Cancel receives
    // input. Also restore controls on a worker error or lost capture.
    m_stroke.reset();
    if (m_preview->HasCapture()) m_preview->ReleaseMouse();
    m_layer->Enable();
    m_first->Enable(); m_last->Enable(); m_paint_range->Enable(); m_brush.Enable();
    m_tool.Enable(); m_shape.Enable();
    m_range_slider->Enable(); m_single_layer->Enable();
    m_radius->Enable(); m_value->Enable();
    if (m_plane) m_plane->Enable();
    if (commit) {
        TaskLock working(m_working);
        run_background_layers(this, _L("Paint layers"), [&](const auto &progress) {
            if (tool != 0) return m_document.paint_shape(first, last, shape_op, selected_plane(), progress);
            return m_document.paint_stroke(first, last, points,
                m_stroke_radius, m_stroke_value, selected_plane(), progress, m_stroke_shape);
        });
    }
    refresh();
}

bool LayerEditorContent::has_changes() const
{
    return m_working || unsaved_changes() ||
        (m_print && m_print->is_raster_job() && !m_generated_exported && !m_project_revision);
}

bool LayerEditorContent::source_current() const
{
    return is_model_result() && m_session && m_session->source_current() && (!m_source_available || m_source_available());
}
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
