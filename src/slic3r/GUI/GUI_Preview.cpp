///|/ Copyright (c) Prusa Research 2018 - 2023 Enrico Turri @enricoturri1966, Oleksandra Iushchenko @YuSanka, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Filip Sykala @Jony01, David Kocík @kocikdav, Tomáš Mészáros @tamasmeszaros, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2022 André Althaus
///|/ Copyright (c) 2019 John Drake @foxox
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
//#include "stdlib.h"
#include "libslic3r/libslic3r.h"
#include "GUI_Preview.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_Init.hpp"
#include "I18N.hpp"
#include "3DScene.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "OpenGLManager.hpp"
#include "GLCanvas3D.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "DoubleSliderForLayers.hpp"
#include "Plater.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"

#include <wx/listbook.h>
#include <wx/notebook.h>
#include <wx/glcanvas.h> // IWYU pragma: keep
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/combo.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>
#include <wx/colordlg.h>

// this include must follow the wxWidgets ones or it won't compile on Windows -> see http://trac.wxwidgets.org/ticket/2421
#include "libslic3r/SLAPrint.hpp"
#include "NotificationManager.hpp"
#include "libslic3r/MultipleBeds.hpp"

#ifdef _WIN32
#include "BitmapComboBox.hpp"
#endif

namespace Slic3r {
namespace GUI {

View3D::View3D(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
    : m_canvas_widget(nullptr)
    , m_canvas(nullptr)
{
    init(parent, bed, model, config, process);
}

View3D::~View3D()
{
    delete m_canvas;
    delete m_canvas_widget;
}

bool View3D::init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

    const GUI_InitParams* const init_params = wxGetApp().init_params;
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this, (init_params != nullptr) ? init_params->opengl_aa : false);
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));

    m_canvas->allow_multisample(OpenGLManager::can_multisample());

    m_canvas->enable_picking(true);
    m_canvas->get_selection().set_mode(Selection::Instance);
    m_canvas->enable_moving(true);
    // XXX: more config from 3D.pm
    m_canvas->set_model(model);
    m_canvas->set_process(process);
    m_canvas->set_config(config);
    m_canvas->enable_gizmos(true);
    m_canvas->enable_selection(true);
    m_canvas->enable_main_toolbar(true);
    m_canvas->enable_undoredo_toolbar(true);
    m_canvas->enable_labels(true);
    m_canvas->enable_slope(true);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    return true;
}

void View3D::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

void View3D::bed_shape_changed()
{
    if (m_canvas != nullptr)
        m_canvas->bed_shape_changed();
}

void View3D::select_view(const std::string& direction)
{
    if (m_canvas != nullptr)
        m_canvas->select_view(direction);
}

void View3D::select_all()
{
    if (m_canvas != nullptr)
        m_canvas->select_all();
}

void View3D::deselect_all()
{
    if (m_canvas != nullptr)
        m_canvas->deselect_all();
}

void View3D::delete_selected()
{
    if (m_canvas != nullptr)
        m_canvas->delete_selected();
}

void View3D::mirror_selection(Axis axis)
{
    if (m_canvas != nullptr)
        m_canvas->mirror_selection(axis);
}

bool View3D::is_dragging() const
{
    return (m_canvas != nullptr) ? m_canvas->is_dragging() : false;
}

bool View3D::is_reload_delayed() const
{
    return (m_canvas != nullptr) ? m_canvas->is_reload_delayed() : false;
}

void View3D::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas != nullptr)
        m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
}

void View3D::render()
{
    if (m_canvas != nullptr)
        //m_canvas->render();
        m_canvas->set_as_dirty();
}

Preview::Preview(
    wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config,
    BackgroundSlicingProcess* process, std::function<void()> schedule_background_process_func)
    : m_config(config)
    , m_process(process)
    , m_schedule_background_process(schedule_background_process_func)
{
    if (init(parent, bed, model))
        load_print();
}

void Preview::set_layers_slider_values_range(int bottom, int top)
{
    m_layers_slider->SetSelectionSpan(std::min(top, m_layers_slider->GetMaxPos()),
                                      std::max(bottom, m_layers_slider->GetMinPos()));
}


bool Preview::init(wxWindow* parent, Bed3D& bed, Model* model)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

    // to match the background of the sliders
#ifdef _WIN32 
    wxGetApp().UpdateDarkUI(this);
#else
    SetBackgroundColour(GetParent()->GetBackgroundColour());
#endif // _WIN32 

    const GUI_InitParams* const init_params = wxGetApp().init_params;
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this, (init_params != nullptr) ? init_params->opengl_aa : false);
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));
    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    m_canvas->set_config(m_config);
    m_canvas->set_model(model);
    m_canvas->set_process(m_process);
    m_canvas->show_legend(true);
    m_canvas->enable_dynamic_background(true);

    create_sliders();

    m_left_sizer = new wxBoxSizer(wxVERTICAL);
    m_left_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxHORIZONTAL);
    main_sizer->Add(m_left_sizer, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    bind_event_handlers();
    
    return true;
}

Preview::~Preview()
{
    unbind_event_handlers();

    if (m_canvas != nullptr)
        delete m_canvas;

    if (m_canvas_widget != nullptr)
        delete m_canvas_widget;
}

void Preview::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

void Preview::bed_shape_changed()
{
    if (m_canvas != nullptr)
        m_canvas->bed_shape_changed();
}

void Preview::select_view(const std::string& direction)
{
    m_canvas->select_view(direction);
}

void Preview::set_drop_target(wxDropTarget* target)
{
    if (target != nullptr)
        SetDropTarget(target);
}

void Preview::load_print(bool keep_z_range)
{
    PrinterTechnology tech = m_process->current_printer_technology();
    if (tech == ptSLA)
        load_print_as_sla();

    Layout();
}

void Preview::reload_print()
{
    if (!IsShown())
        return;

    m_loaded = false;
    load_print();
}

void Preview::msw_rescale()
{
    m_layers_slider->SetEmUnit(wxGetApp().em_unit());
    // rescale warning legend on the canvas
    get_canvas3d()->msw_rescale();

    // rescale legend
    reload_print();
}

void Preview::render_sliders(GLCanvas3D& canvas)
{
    const Size  cnv_size        = canvas.get_canvas_size();
    const int   canvas_width    = cnv_size.get_width();
    const int   canvas_height   = cnv_size.get_height();
    const float extra_scale     = cnv_size.get_scale_factor();

    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    const bool  is_collapse_btn_shown = collapse_toolbar.is_enabled();

    if (m_layers_slider)
        m_layers_slider->Render(canvas_width, canvas_height, extra_scale, is_collapse_btn_shown ? collapse_toolbar.get_height() : 0.f);
}

float Preview::get_layers_slider_width(bool disregard_visibility) const
{
    if (m_layers_slider && (m_layers_slider->IsShown() || disregard_visibility))
        return m_layers_slider->GetWidth();
    return 0.0f;
}

void Preview::bind_event_handlers()
{
    Bind(wxEVT_SIZE, &Preview::on_size, this);
}

void Preview::unbind_event_handlers()
{
    Unbind(wxEVT_SIZE, &Preview::on_size, this);
}

void Preview::hide_layers_slider()
{
    m_layers_slider->Hide();
}

void Preview::on_size(wxSizeEvent& evt)
{
    evt.Skip();
    m_layers_slider->force_ruler_update();
    Refresh();
}

void Preview::create_sliders()
{
    // Layers Slider

    m_layers_slider = std::make_unique<DoubleSlider::DSForLayers>(0, 0, 0, 100, wxGetApp().is_editor());
    m_layers_slider->SetEmUnit(wxGetApp().em_unit());
    m_layers_slider->set_imgui_wrapper(wxGetApp().imgui());
    m_layers_slider->show_estimated_times(wxGetApp().app_config->get_bool("show_estimated_times_in_dbl_slider"));
    m_layers_slider->show_ruler(wxGetApp().app_config->get_bool("show_ruler_in_dbl_slider"), wxGetApp().app_config->get_bool("show_ruler_bg_in_dbl_slider"));

    m_layers_slider->set_callback_on_thumb_move( [this]() -> void { Preview::on_layers_slider_scroll_changed(); } );

    m_layers_slider->set_callback_on_change_app_config([](const std::string& key, const std::string& val) -> void {
        wxGetApp().app_config->set(key, val);
    });

    // m_canvas_widget
    m_canvas_widget->Bind(wxEVT_KEY_DOWN,                    &Preview::update_sliders_from_canvas, this);
    m_canvas_widget->Bind(EVT_GLCANVAS_SLIDERS_MANIPULATION, &Preview::update_sliders_from_canvas, this);

    // Hide sliders from the very begibing. Visibility will be set later
    m_layers_slider->Hide();
}

// Find an index of a value in a sorted vector, which is in <z-eps, z+eps>.
// Returns -1 if there is no such member.
static int find_close_layer_idx(const std::vector<double>& zs, double &z, double eps)
{
    if (zs.empty())
        return -1;
    auto it_h = std::lower_bound(zs.begin(), zs.end(), z);
    if (it_h == zs.end()) {
        auto it_l = it_h;
        -- it_l;
        if (z - *it_l < eps)
            return int(zs.size() - 1);
    } else if (it_h == zs.begin()) {
        if (*it_h - z < eps)
            return 0;
    } else {
        auto it_l = it_h;
        -- it_l;
        double dist_l = z - *it_l;
        double dist_h = *it_h - z;
        if (std::min(dist_l, dist_h) < eps) {
            return (dist_l < dist_h) ? int(it_l - zs.begin()) : int(it_h - zs.begin());
        }
    }
    return -1;
}

void Preview::update_layers_slider(const std::vector<double>& layers_z, bool keep_z_range)
{
    // Save the initial slider span.
    double z_low = m_layers_slider->GetLowerValue();
    double z_high = m_layers_slider->GetHigherValue();
    bool   was_empty = m_layers_slider->GetMaxPos() == 0;

    bool force_sliders_full_range = was_empty;
    if (!keep_z_range) {
        bool span_changed = layers_z.empty() || std::abs(layers_z.back() - m_layers_slider->GetMaxValue()) > 1e-6;
        force_sliders_full_range |= span_changed;
    }
    bool   snap_to_min = force_sliders_full_range || m_layers_slider->IsLowerAtMin();
    bool   snap_to_max = force_sliders_full_range || m_layers_slider->IsHigherAtMax();

    Plater* plater = wxGetApp().plater();

    m_layers_slider->SetSliderValues(layers_z);
    m_layers_slider->force_ruler_update();
    assert(m_layers_slider->GetMinPos() == 0);

    m_layers_slider->Freeze();

    m_layers_slider->SetMaxPos(layers_z.empty() ? 0 : layers_z.size() - 1);

    int idx_low = 0;
    int idx_high = m_layers_slider->GetMaxPos();
    if (!layers_z.empty()) {
        if (!snap_to_min) {
            int idx_new = find_close_layer_idx(layers_z, z_low, 1e-6);
            if (idx_new != -1)
                idx_low = idx_new;
        }
        if (!snap_to_max) {
            int idx_new = find_close_layer_idx(layers_z, z_high, 1e-6);
            if (idx_new != -1)
                idx_high = idx_new;
        }
    }
    m_layers_slider->SetSelectionSpan(idx_low, idx_high);
    m_layers_slider->SetLayersTimes(plater->active_sla_print().print_statistics().layers_times_running_total,
        plater->active_sla_print().print_statistics().time_basis == SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);

    m_layers_slider->Thaw();

    m_layers_slider->Show();
}

void Preview::reset_layers_slider()
{
    m_layers_slider->SetSelectionSpan(0, 0);
}

void Preview::update_sliders_from_canvas(wxKeyEvent& event)
{
    const auto key = event.GetKeyCode();

    if (key == 'G' || key == 'g')
        m_layers_slider->jump_to_value();
    else if (key == WXK_LEFT || key == WXK_RIGHT || key == WXK_UP || key == WXK_DOWN) {
        int delta = 1;
        // accelerators
        int accelerator = 0;
        if (wxGetKeyState(WXK_SHIFT))
            accelerator += 5;
        if (wxGetKeyState(WXK_CONTROL))
            accelerator += 5;
        if (accelerator > 0)
            delta *= accelerator;

        if (key == WXK_UP || key == WXK_DOWN)
            m_layers_slider->move_current_thumb(delta * (key == WXK_DOWN ? 1 : -1));
    }

    else if (event.HasModifiers()) {
        event.Skip();
        return;
    }

    else if (key == 'S' || key == 'W') {
        const int new_pos = key == 'W' ? m_layers_slider->GetHigherPos() + 1 : m_layers_slider->GetHigherPos() - 1;
        m_layers_slider->SetHigherPos(new_pos);
    }
    else if (key == 'X')
        m_layers_slider->ChangeOneLayerLock();
    else
        event.Skip();
}




void Preview::load_print_as_sla()
{
    // The bound SLAPrint exists during construction, but notification helpers
    // also need the completed MainFrame/Plater published by GUI_App.
    if (wxGetApp().mainframe == nullptr || wxGetApp().is_recreating_gui() ||
        wxGetApp().plater() != GetParent())
        return;

    if (m_loaded || (m_process->current_printer_technology() != ptSLA))
        return;

    unsigned int n_layers = 0;
    const SLAPrint* print = m_process->sla_print();

    std::vector<double> zs;
    double initial_layer_height = print->material_config().initial_layer_height.value;
    for (const SLAPrintObject* obj : print->objects())
        if (obj->is_step_done(slaposSliceSupports) && !obj->get_slice_index().empty()) {
            auto low_coord = obj->get_slice_index().front().print_level();
            for (auto& rec : obj->get_slice_index())
                zs.emplace_back(initial_layer_height + (rec.print_level() - low_coord) * SCALING_FACTOR);
        }
    sort_remove_duplicates(zs);

    m_canvas->reset_clipping_planes_cache();
    m_canvas->set_use_clipping_planes(true);

    n_layers = (unsigned int)zs.size();
    if (n_layers == 0) {
        hide_layers_slider();
        m_canvas_widget->Refresh();
    }

    if (IsShown()) {
        m_canvas->load_sla_preview();

        if (n_layers > 0)
            update_layers_slider(zs);

        m_loaded = true;
    }
}

void Preview::on_layers_slider_scroll_changed()
{
    if (!IsShown())
        return;

    const PrinterTechnology tech = m_process->current_printer_technology();
    if (tech == ptSLA) {
        m_canvas->set_clipping_plane(0, ClippingPlane(Vec3d::UnitZ(), -m_layers_slider->GetLowerValue()));
        m_canvas->set_clipping_plane(1, ClippingPlane(-Vec3d::UnitZ(), m_layers_slider->GetHigherValue()));
        m_canvas->set_layer_slider_index(m_layers_slider->GetHigherPos());
        m_canvas->render();
    }
}


} // namespace GUI
} // namespace Slic3r
