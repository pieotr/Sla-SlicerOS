// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef slic3r_GUI_LayerRangeSlider_hpp_
#define slic3r_GUI_LayerRangeSlider_hpp_

#include "LayerRangeSelection.hpp"
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/timer.h>
#include <functional>
#include <cstdlib>
#include <optional>
#include <utility>

namespace Slic3r::GUI {

// Native wx canvas, independent of the GL/FFF preview sliders. Numeric fields
// in the editor remain the accessible alternative for every selected value.
class LayerRangeSlider final : public wxControl {
public:
    using Changed = std::function<void(const LayerRangeSelection &, bool committed)>;
    LayerRangeSlider(wxWindow *parent, int count, Changed changed)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS),
          m_selection(count), m_changed(std::move(changed)), m_key_timer(this)
    {
        SetName("Layer range slider");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(parent->GetBackgroundColour());
        SetMinSize({std::max(FromDIP(116), 2 * GetTextExtent(wxString::Format("%d", count)).x + FromDIP(48)), FromDIP(200)});
        SetToolTip("Drag a handle to choose a layer. Up/Down: one layer; Page Up/Down: ten; Home/End: limits. Left/Right chooses the first/last handle. Esc cancels a drag. The overlay updates when you release the handle.");
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) { commit_keys(); Refresh(false); event.Skip(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
            commit_keys();
            SetFocus();
            m_drag_start = m_selection;
            if (m_selection.range_mode()) {
                const int first = std::abs(event.GetY() - y(m_selection.first()));
                const int last = std::abs(event.GetY() - y(m_selection.last()));
                m_active = first == last ? (event.GetX() < GetClientSize().x / 2 ? LayerRangeHandle::First : LayerRangeHandle::Last) :
                    first < last ? LayerRangeHandle::First : LayerRangeHandle::Last;
            }
            CaptureMouse();
            move_pointer(event.GetY());
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent &event) {
            if (m_drag_start && event.LeftIsDown()) move_pointer(event.GetY());
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { end_drag(true); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { end_drag(false); });
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) {
            if (event.GetKeyCode() == WXK_ESCAPE && m_drag_start) end_drag(false);
            else event.Skip();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &event) { key_down(event); });
        Bind(wxEVT_KEY_UP, [this](wxKeyEvent &event) { commit_keys(); event.Skip(); });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent &event) {
            if (m_drag_start || event.GetWheelDelta() <= 0) return;
            const int64_t rotation = int64_t(m_wheel_remainder) + event.GetWheelRotation();
            const int steps = int(rotation / event.GetWheelDelta());
            m_wheel_remainder = int(rotation % event.GetWheelDelta());
            if (steps) { m_selection.step_handle(m_active, steps); defer_keys(); }
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { commit_keys(); }, m_key_timer.GetId());
    }
    ~LayerRangeSlider() override
    {
        m_key_timer.Stop();
        m_drag_start.reset();
        if (HasCapture()) ReleaseMouse();
    }
    void set_selection(const LayerRangeSelection &selection)
    {
        m_selection = selection;
        Refresh(false);
    }

private:
    LayerRangeSelection m_selection;
    Changed m_changed;
    LayerRangeHandle m_active = LayerRangeHandle::Last;
    std::optional<LayerRangeSelection> m_drag_start;
    wxTimer m_key_timer;
    bool m_keys_pending = false;
    int m_wheel_remainder = 0;

    int margin() const { return FromDIP(22); }
    int travel() const { return std::max(1, GetClientSize().y - 2 * margin()); }
    int y(int layer) const { return margin() + layer_range_offset(layer, m_selection.count(), travel()); }
    void notify(bool committed)
    {
        Refresh(false);
        if (m_changed) m_changed(m_selection, committed);
    }
    void move_pointer(int position)
    {
        m_selection.move_handle(m_active, layer_range_value(position - margin(), m_selection.count(), travel()));
        notify(false);
    }
    void end_drag(bool commit)
    {
        if (!m_drag_start) return;
        if (!commit) m_selection = *m_drag_start;
        m_drag_start.reset();
        if (HasCapture()) ReleaseMouse();
        notify(true);
    }
    void defer_keys()
    {
        m_keys_pending = true;
        notify(false);
        // Coalesce key repeat/wheel input instead of decoding an entire mask
        // range on every input event. Key release commits immediately.
        m_key_timer.StartOnce(120);
    }
    void commit_keys()
    {
        m_key_timer.Stop();
        if (!m_keys_pending) return;
        m_keys_pending = false;
        notify(true);
    }
    void key_down(wxKeyEvent &event)
    {
        if (m_drag_start) {
            // wxGTK can deliver key-down directly while the mouse is captured,
            // without the usual char-hook route. Both routes must roll back.
            if (event.GetKeyCode() == WXK_ESCAPE) end_drag(false);
            else event.Skip();
            return;
        }
        switch (event.GetKeyCode()) {
        case WXK_LEFT: m_active = LayerRangeHandle::First; Refresh(false); return;
        case WXK_RIGHT: m_active = LayerRangeHandle::Last; Refresh(false); return;
        case WXK_UP: m_selection.step_handle(m_active, 1); break;
        case WXK_DOWN: m_selection.step_handle(m_active, -1); break;
        case WXK_PAGEUP: m_selection.step_handle(m_active, 10); break;
        case WXK_PAGEDOWN: m_selection.step_handle(m_active, -10); break;
        case WXK_HOME: m_selection.move_handle(m_active, 1); break;
        case WXK_END: m_selection.move_handle(m_active, m_selection.count()); break;
        default: event.Skip(); return;
        }
        defer_keys();
    }
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        const int center = GetClientSize().x / 2;
        const auto highlight = wxSystemSettings::GetColour(IsEnabled() ? wxSYS_COLOUR_HIGHLIGHT : wxSYS_COLOUR_GRAYTEXT);
        dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT), FromDIP(2)));
        dc.DrawLine(center, margin(), center, margin() + travel());
        dc.SetPen(wxPen(highlight, FromDIP(4)));
        if (m_selection.range_mode()) dc.DrawLine(center, y(m_selection.last()), center, y(m_selection.first()));
        dc.SetTextForeground(IsEnabled() ? GetForegroundColour() : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        auto handle = [&](int layer, LayerRangeHandle which, bool single) {
            const int at = y(layer);
            const bool first = which == LayerRangeHandle::First;
            dc.SetPen(wxPen(highlight, FromDIP(2)));
            dc.SetBrush(wxBrush(single || which == m_active ? highlight : GetBackgroundColour()));
            if (single) dc.DrawCircle(center, at, FromDIP(7));
            else {
                const int side = first ? -1 : 1;
                const wxPoint points[] = {{center + side * FromDIP(2), at},
                    {center + side * FromDIP(14), at - FromDIP(7)}, {center + side * FromDIP(14), at + FromDIP(7)}};
                dc.DrawPolygon(3, points);
            }
            const wxString label = wxString::Format("%d", layer);
            const wxSize size = dc.GetTextExtent(label);
            const int left = first ? center - FromDIP(18) - size.x : center + FromDIP(18);
            dc.DrawText(label, std::clamp(left, 0, std::max(0, GetClientSize().x - size.x)), at - size.y / 2);
        };
        if (m_selection.range_mode()) {
            handle(m_selection.first(), LayerRangeHandle::First, false);
            handle(m_selection.last(), LayerRangeHandle::Last, false);
        } else handle(m_selection.current(), LayerRangeHandle::Last, true);
        if (HasFocus()) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(highlight, 1, wxPENSTYLE_DOT));
            wxRect bounds = GetClientRect(); bounds.Deflate(1); dc.DrawRectangle(bounds);
        }
    }
};

} // namespace Slic3r::GUI
#endif
