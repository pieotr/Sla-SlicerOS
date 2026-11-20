///|/ Copyright (c) Prusa Research 2020 - 2022 Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_DoubleSliderForLayers_hpp_
#define slic3r_GUI_DoubleSliderForLayers_hpp_

#include <vector>
#include <functional>
#include <string>

#include "ImGuiDoubleSlider.hpp"
#include "RulerForDoubleSlider.hpp"

struct ImVec2;

namespace Slic3r {
namespace GUI
{
class ImGuiWrapper;
}
}

namespace DoubleSlider {

enum FocusedItem {
    fiNone,
    fiOneLayerIcon,
    fiCogIcon,
};

enum LabelType
{
    ltHeightWithLayer,
    ltHeight,
    ltEstimatedTime,
};

// Vertical layers slider of the SLA/MSLA layer preview.
class DSForLayers : public Manager<double>
{
public:
    DSForLayers() : Manager<double>() {}
    DSForLayers(int lowerValue,
                int higherValue,
                int minValue,
                int maxValue,
                bool allow_editing);
    ~DSForLayers() {}

    void    ChangeOneLayerLock();

    void    SetLayersTimes(const std::vector<double>& layers_times, bool excludes_printer_setup = false);

    void    Render(const int canvas_width, const int canvas_height, float extra_scale = 1.f, float offset = 0.f) override;
    void    force_ruler_update();

    // jump to selected layer
    void    jump_to_value();

    void    set_imgui_wrapper(Slic3r::GUI::ImGuiWrapper* imgui) { m_imgui = imgui; }
    void    show_estimated_times(bool show)                     { m_show_estimated_times = show; }
    void    show_ruler(bool show, bool show_bg)                 { m_show_ruler = show; m_show_ruler_bg = show_bg; }

    void    set_callback_on_change_app_config (std::function<void(const std::string&, const std::string&)> cb)
            { m_cb_change_app_config = cb; }

private:

    bool        m_allow_editing         { true };
    bool        m_show_estimated_times  { true };
    bool        m_show_ruler            { false };
    bool        m_show_ruler_bg         { true };
    bool        m_show_cog_menu         { false };
    int         m_pos_on_move           { -1 };

    FocusedItem m_focus                 { fiNone };

    Ruler                       m_ruler;
    Slic3r::GUI::ImGuiWrapper*  m_imgui { nullptr };

    std::vector<double>         m_layers_times;
    bool                        m_time_excludes_printer_setup { false };

    std::string get_label(int tick, LabelType label_type, const std::string& fmt = "%1$.2f") const;

    std::string get_tooltip(int tick = -1);

    // functions for extend rendering of m_ctrl

    void        draw_ticks(const ImRect& slideable_region);
    void        draw_ruler(const ImRect& slideable_region);
    void        render_menu();
    void        render_cog_menu();
    bool        render_button(const wchar_t btn_icon, const wchar_t btn_icon_hovered, const std::string& label_id, const ImVec2& pos, FocusedItem focus, int tick = -1);

    void        process_jump_to_value();

    std::string get_label(int pos) const override { return get_label(pos, ltHeightWithLayer); }

    bool        m_show_get_jump_value           { false };

    double      m_jump_to_value                 { 0.0 };

    bool        render_jump_to_window(const ImVec2& pos, double* active_value, double min_z, double max_z);

    std::function<void(const std::string&, const std::string&)> m_cb_change_app_config  { nullptr };
};

} // DoubleSlider;

#endif // slic3r_GUI_DoubleSliderForLayers_hpp_
