///|/ Copyright (c) Prusa Research 2018 - 2023 Enrico Turri @enricoturri1966, Filip Sykala @Jony01, Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Král @vojtechkral
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_Preview_hpp_
#define slic3r_GUI_Preview_hpp_

#include <wx/panel.h>

#include "libslic3r/Point.hpp"

#include <string>

class wxGLCanvas;
class wxBoxSizer;
class wxStaticText;
class wxComboBox;
class wxComboCtrl;
class wxCheckBox;

namespace DoubleSlider {
    class DSForLayers;
};

namespace Slic3r {

class DynamicPrintConfig;
class BackgroundSlicingProcess;
class Model;

namespace GUI {

class GLCanvas3D;
class GLToolbar;
class Bed3D;
struct Camera;
class Plater;
#ifdef _WIN32
class BitmapComboBox;
#endif

class View3D : public wxPanel
{
    wxGLCanvas* m_canvas_widget;
    GLCanvas3D* m_canvas;

public:
    View3D(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
    virtual ~View3D();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();
    void bed_shape_changed();

    void select_view(const std::string& direction);
    void select_all();
    void deselect_all();
    void delete_selected();
    void mirror_selection(Axis axis);

    bool is_dragging() const;
    bool is_reload_delayed() const;

    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    void render();

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
};

class Preview : public wxPanel
{
    wxGLCanvas* m_canvas_widget { nullptr };
    GLCanvas3D* m_canvas { nullptr };
    wxBoxSizer* m_left_sizer { nullptr };

    DynamicPrintConfig* m_config;
    BackgroundSlicingProcess* m_process;

    // Calling this function object forces Plater::schedule_background_process.
    std::function<void()> m_schedule_background_process;


    bool m_loaded { false };

    std::unique_ptr<DoubleSlider::DSForLayers>  m_layers_slider{ nullptr };

public:
    Preview(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process, 
        std::function<void()> schedule_background_process = []() {});
    virtual ~Preview();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();

    void bed_shape_changed();
    void select_view(const std::string& direction);
    void set_drop_target(wxDropTarget* target);

    void load_print(bool keep_z_range = false);
    void reload_print();

    void msw_rescale();

    void render_sliders(GLCanvas3D& canvas);
    float get_layers_slider_width(bool disregard_visibility = false) const;

    bool is_loaded() const { return m_loaded; }

    void hide_layers_slider();

    void set_layers_slider_values_range(int bottom, int top);

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model);

    void bind_event_handlers();
    void unbind_event_handlers();

    void on_size(wxSizeEvent& evt);

    // Create/Update/Reset double slider on 3dPreview
    void create_sliders();
    void reset_layers_slider();
    void update_layers_slider(const std::vector<double>& layers_z, bool keep_z_range = false);
    // update vertical DoubleSlider after keyDown in canvas
    void update_sliders_from_canvas(wxKeyEvent& event);

    void load_print_as_sla();

    void on_layers_slider_scroll_changed();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Preview_hpp_
