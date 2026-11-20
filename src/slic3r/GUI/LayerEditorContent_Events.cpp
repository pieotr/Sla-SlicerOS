#include "LayerEditorContent.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
void LayerEditorContent::bind_events() {
    const wxAcceleratorEntry shortcuts[] = {
        wxAcceleratorEntry(wxACCEL_CTRL, 'Z', wxID_UNDO),
        wxAcceleratorEntry(wxACCEL_CTRL, 'Y', wxID_REDO)
    };
    SetAcceleratorTable(wxAcceleratorTable(2, shortcuts));
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { undo_change(); }, wxID_UNDO);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { redo_change(); }, wxID_REDO);
    if (m_plane) m_plane->Bind(wxEVT_COMBOBOX,[this](wxCommandEvent &){ perform([]{}); });
    m_value->Bind(wxEVT_SPINCTRL,[this](wxSpinEvent &){update_brush_value_hint();});
    m_value->Bind(wxEVT_TEXT,[this](wxCommandEvent &){update_brush_value_hint();});
    m_brush.on_change=[this]{update_brush_value_hint(); update_view_info();};
    m_tool.on_change=[this]{update_view_info();};
    m_shape.on_change=[this]{update_view_info();};
    Bind(wxEVT_SIZE,[this](wxSizeEvent &event){
        update_brush_value_hint(); update_mask_plane_info(); event.Skip();
    });
    // Fit follows the window: a new viewport size re-fits the mask (other zoom modes keep their scale).
    m_viewport->Bind(wxEVT_SIZE, [this](wxSizeEvent &event) { event.Skip(); schedule_fit_refresh(); });
    for (auto *spin : {m_radius, m_value}) {
        spin->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &event) { update_view_info(); event.Skip(); });
        spin->Bind(wxEVT_TEXT, [this](wxCommandEvent &event) { update_view_info(); event.Skip(); });
    }
    m_layer->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { perform([] {}); });
    m_zoom->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
        if (m_working) return;
        m_preview_cancelled = false;
        m_zoom_scale = 0;
        try { refresh(); }
        catch (const std::exception &error) {
            m_zoom->SetSelection(0); refresh();
            wxMessageBox(wxString::FromUTF8(error.what()), _L("Layer editor"), wxOK | wxICON_INFORMATION, this);
        }
    });
    m_preview->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) { on_preview_left_down(event); });
    m_preview->Bind(wxEVT_MOTION, [this](wxMouseEvent &event) { on_preview_motion(event); });
    m_preview->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) {
        if (!m_panning) update_pixel_info(nullptr);
    });
    m_preview->Bind(wxEVT_MIDDLE_DOWN, [this](wxMouseEvent &event) {
        if (m_working || m_stroke || !m_preview_bitmap.IsOk()) return;
        m_preview->SetFocus(); begin_pan(event);
    });
    m_preview->Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent &) { end_pan(); });
    m_preview->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent &event) { on_preview_wheel(event); });
    m_preview->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (m_panning) { end_pan(); return; }
        // A release without a stroke must not restart a cancelled preview.
        if (m_stroke) perform([&] { finish_stroke(true); });
    });
    m_preview->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
        m_panning = false; m_preview->SetCursor(wxCursor(wxCURSOR_CROSS));
        perform([&] { finish_stroke(false); });
    });
    m_preview->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &event) { on_preview_key_down(event); });
    m_preview->Bind(wxEVT_KEY_UP, [this](wxKeyEvent &event) {
        if (event.GetKeyCode() == WXK_SPACE) m_space_down = false;
        event.Skip();
    });
    m_preview->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) { m_space_down = false; event.Skip(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) { on_char_hook(event); });
}

void LayerEditorContent::on_preview_left_down(wxMouseEvent &event) {
    // A cancelled/unavailable canvas has no visible mask to paint.
    // Retry explicitly before accepting any brush coordinates.
    if (m_working || !m_preview_bitmap.IsOk()) return;
    m_preview->SetFocus();
    if (m_space_down && !m_stroke) { begin_pan(event); return; }
    perform([&] {
        const int requested=m_value->GetValue();
        const bool erasing=m_brush.GetSelection()==1;
        if(!erasing && (requested<0 || requested>255))
            throw RuntimeError("Brush grayscale must be between 0 and 255");
        if (m_reuse_preview) {
            const auto *preview = prepared_preview();
            if (!preview) return;
            m_cols = preview->cols; m_rows = preview->rows;
            m_stroke = *preview;
        } else {
            m_stroke = m_paint_range->GetValue() ?
                m_document.overlay(size_t(m_first->GetValue()-1), size_t(m_last->GetValue()-1), selected_plane()) :
                m_document.image(size_t(m_layer->GetValue()-1), selected_plane());
        }
        m_stroke_radius = unsigned(m_radius->GetValue());
        m_stroke_value = m_edit_projection[erasing ? 0 : size_t(requested)];
        m_last_pixel = pixel_at(event);
        m_stroke_start = m_last_pixel;
        m_stroke_tool = m_tool.GetSelection();
        m_stroke_shape = m_shape.GetSelection() == 1 ? sla::LayerDocument::BrushShape::Square
                                                      : sla::LayerDocument::BrushShape::Round;
        m_stroke_points = {{m_last_pixel.x, m_last_pixel.y}};
        if (m_stroke_tool != 0) m_stroke_base = *m_stroke;
        update_stroke_preview(m_last_pixel, m_last_pixel);
        m_layer->Disable();
        m_first->Disable(); m_last->Disable(); m_paint_range->Disable(); m_brush.Disable();
        m_tool.Disable(); m_shape.Disable();
        m_range_slider->Disable(); m_single_layer->Disable();
        m_radius->Disable(); m_value->Disable();
        if (m_plane) m_plane->Disable();
        // Keep stroke keys on the canvas instead of the last toolbar
        // control. wx may suppress CHAR_HOOK while the mouse is captured.
        m_preview->SetFocus();
        m_preview->CaptureMouse();
    });
}

void LayerEditorContent::on_preview_motion(wxMouseEvent &event) {
    if (m_panning) {
        const wxPoint moved = m_preview->ClientToScreen(event.GetPosition()) - m_pan_origin_screen;
        m_viewport->Scroll(int(layer_pan_scroll(m_pan_start_scroll.x, moved.x)),
                           int(layer_pan_scroll(m_pan_start_scroll.y, moved.y)));
        return;
    }
    if (!m_working) update_pixel_info(&event);
    if (!m_stroke || !event.LeftIsDown()) return;
    const auto pixel = pixel_at(event);
    if (pixel == m_last_pixel || m_stroke_tool == 4) return; // fill is decided by the click alone
    perform([&] {
        update_stroke_preview(m_last_pixel, pixel);
        if (m_stroke_tool == 0) m_stroke_points.push_back({pixel.x, pixel.y});
        m_last_pixel = pixel;
    });
    update_pixel_info(&event);
}

void LayerEditorContent::on_preview_wheel(wxMouseEvent &event) {
    const int delta = std::max(1, event.GetWheelDelta());
    const int notches = event.GetWheelRotation() / delta;
    if (!notches || m_stroke || m_panning) return;
    if (event.ControlDown()) {
        zoom_to(layer_zoom_after_steps(effective_zoom(), notches), viewport_point(event));
        return;
    }
    // 3 lines of 16 px per notch, like a normal scrolled window.
    const wxPoint start = m_viewport->GetViewStart();
    const int step = -notches * 48;
    if (event.ShiftDown() || event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL)
        m_viewport->Scroll(std::max(0, start.x + step), start.y);
    else
        m_viewport->Scroll(start.x, std::max(0, start.y + step));
}

void LayerEditorContent::on_preview_key_down(wxKeyEvent &event) {
    if (handle_history_shortcut(event)) return;
    if (event.GetKeyCode() == WXK_ESCAPE && m_stroke) {
        perform([this] { finish_stroke(false); });
        return;
    }
    const int key = event.GetKeyCode();
    if (key == WXK_SPACE) { m_space_down = true; return; } // Space+drag pans
    if (!event.AltDown() && !m_stroke && !m_panning) {
        const auto centre = [this] { const auto size = m_viewport->GetClientSize(); return wxPoint(size.x/2, size.y/2); };
        if (key == '+' || key == '=' || key == WXK_ADD || key == WXK_NUMPAD_ADD) {
            zoom_to(layer_zoom_after_steps(effective_zoom(), 1), centre()); return;
        }
        if (key == '-' || key == WXK_SUBTRACT || key == WXK_NUMPAD_SUBTRACT) {
            zoom_to(layer_zoom_after_steps(effective_zoom(), -1), centre()); return;
        }
        if (key == '0' || key == WXK_NUMPAD0) { zoom_to(1., centre(), true); return; }
        if (key == '1' || key == WXK_NUMPAD1) { zoom_to(1., centre()); return; }
        if (key == WXK_PAGEUP || key == WXK_PAGEDOWN) {
            const int layer = std::clamp(m_layer->GetValue() + (key == WXK_PAGEUP ? 1 : -1), 1, int(m_document.layer_count()));
            if (layer != m_layer->GetValue()) { m_layer->SetValue(layer); perform([] {}); }
            return;
        }
    }
    event.Skip();
}

void LayerEditorContent::on_char_hook(wxKeyEvent &event) {
    if (handle_history_shortcut(event)) return;
    if (event.GetKeyCode() == WXK_ESCAPE) {
        // Cancel only the uncommitted stroke. A later button release
        // sees no stroke, so it cannot publish pixels or an Undo entry.
        if (m_stroke) perform([this] { finish_stroke(false); });
        else if (m_close) m_close();
        return;
    }
    if (event.GetKeyCode() == WXK_DELETE || event.GetKeyCode() == WXK_BACK) {
        auto *focus = wxWindow::FindFocus();
        if (!dynamic_cast<wxTextCtrl*>(focus) && !dynamic_cast<wxSpinCtrl*>(focus) &&
            !(focus && dynamic_cast<wxSpinCtrl*>(focus->GetParent()))) return;
    }
    event.Skip();
}

} // namespace layer_editor_detail
} // namespace Slic3r::GUI
