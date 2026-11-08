#include "libslic3r/SLA/ExposureFinderTable.hpp"
#include "libslic3r/SLA/ExportPrinterValidation.hpp"
#include "libslic3r/SLA/ExportFormatPreference.hpp"
#include "LayerEditorDialog.hpp"
#include "LayerCanvasGeometry.hpp"
#include "LayerRangeSlider.hpp"
#include "BackgroundTaskDialog.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/SL1.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/Format/ChituboxFormat.hpp"
#include "libslic3r/SLAPrint.hpp"
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <wx/utils.h>
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerPreviewCache.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/SlicedResultSession.hpp"
#include "libslic3r/SLA/LayerImageFile.hpp"
#include "libslic3r/SLA/LayerExportProfile.hpp"
#include "libslic3r/SLA/LayerPaletteProjection.hpp"
#include "libslic3r/SLA/CWSFreshExportPlan.hpp"
#include "libslic3r/SLA/SL1FreshExportPlan.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/cursor.h>
#include <wx/stattext.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/image.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/wrapsizer.h>
#include <wx/scrolwin.h>
#include <wx/gauge.h>
#include <wx/timer.h>
#include <wx/accel.h>
#include <optional>
#include <algorithm>
#include <functional>
#include <future>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace Slic3r::GUI {
namespace {
struct TaskLock {
    bool &flag;
    explicit TaskLock(bool &value) : flag(value) { flag = true; }
    ~TaskLock() { flag = false; }
};
class LayerEditorContent final : public LayerEditorPanel {
    std::unique_ptr<sla::SlicedResultSession> m_session;
    sla::LayerDocument m_standalone_document;
    sla::LayerDocument &m_document;
    const SLAPrint *m_print = nullptr;
    std::string m_printer_name;
    std::function<void()> m_close;
    std::function<void()> m_export_action;
    std::function<void(sla::SlicedResultSession &)> m_apply_source;
    std::function<bool()> m_source_available;
    wxStaticText *m_session_status = nullptr;
    wxString m_status_text;
    int m_status_width = 0;
    wxButton *m_apply_button = nullptr;
    wxTimer m_source_timer;
    void update_source_status() {
        if (!m_session_status) return;
        const bool current = source_current();
        const wxString text = m_working ? "Applying layer operation..." : !is_model_result() ?
            (unsaved_changes() ? "Layer document: modified. Save a layer project to keep masks and settings." :
             has_changes() ? "Generated layers: not saved or exported. Save a layer project or export a copy." :
             "Layer document: saved. Edits use this document's own settings.") : !current ?
            "Older slices: your edits are kept. Export a copy or load current slices. Apply is unavailable." :
            unsaved_changes() ? (m_document.dirty() ?
                "Sliced layers: modified. Save the project or Apply to keep these edits in the model result." :
                "Sliced layers: applied to the model. Save the 3D project to keep these edits after restart.") :
            "Sliced layers: edits use the captured printer settings.";
        const int width = std::max(200, GetClientSize().x-24);
        if (m_status_text != text || m_status_width != width) {
            m_status_text = text; m_status_width = width;
            m_session_status->SetLabel(text); m_session_status->Wrap(width);
            Layout(); FitInside();
            if (wxGetApp().mainframe) wxGetApp().mainframe->update_title();
        }
        if (m_apply_button) m_apply_button->Enable(current && !m_working);
    }
    bool m_generated_exported = false;
    bool m_working = false;
    wxChoice *m_format = nullptr;
    std::vector<const ArchiveEntry *> m_formats;
    std::optional<png::ImageGreyscale> m_stroke;
    sla::LayerPreviewCache m_preview_cache;
    // Reuse one validated preview by default; explicit 0 retains the diagnostic fallback.
    const bool m_reuse_preview = [] {
        const char *flag = std::getenv("SLA_EXPERIMENT_LAYER_PREVIEW_CACHE");
        return flag ? std::strcmp(flag, "1") == 0 : true;
    }();
    bool m_preview_cancelled = false;
    std::vector<sla::LayerDocument::BrushPoint> m_stroke_points;
    wxChoice *m_brush = nullptr;
    wxCheckBox *m_paint_range = nullptr;
    wxCheckBox *m_single_layer = nullptr;
    LayerRangeSlider *m_range_slider = nullptr;
    wxStaticText *m_paint_scope = nullptr;
    wxStaticText *m_brush_value_info = nullptr;
    std::array<uint8_t,256> m_edit_projection{};
    unsigned m_edit_level_count = 0;
    wxString m_brush_hint_text;
    int m_brush_hint_width = -1;
    unsigned m_stroke_radius = 0;
    uint8_t m_stroke_value = 255;
    std::optional<sla::LayerExportProfile> m_export_profile;
    std::optional<sla::LayerExportProfile> m_reopened_profile;
    std::optional<sla::LayerExportProfile> m_context_baseline;
    std::function<void()> m_update_profile_summary;
    wxCheckBox *m_use_profile = nullptr;
    std::optional<sla::ProjectSLASettings> m_project_settings;
    std::optional<sla::ProjectSLASettings> m_settings_baseline;
    bool m_settings_require_project = false;
    std::optional<uint64_t> m_project_revision;
    // LayerDocument's saved baseline tracks Apply for model sessions. Keep a
    // separate project baseline so later scene changes cannot silently discard
    // applied masks that have never been written to a matching 3MF.
    uint64_t m_model_project_revision = 0;
    std::string m_layer_project_path;
    bool context_changed() const {
        if (m_export_profile.has_value() != m_context_baseline.has_value()) return true;
        if (!m_export_profile) return false;
        const auto &a=*m_export_profile, &b=*m_context_baseline;
        return a.columns!=b.columns || a.rows!=b.rows ||
            a.screen_width_mm!=b.screen_width_mm || a.screen_height_mm!=b.screen_height_mm ||
            a.density_g_ml!=b.density_g_ml || a.cost_per_ml!=b.cost_per_ml ||
            a.max_print_height_mm!=b.max_print_height_mm;
    }
    bool unsaved_changes() const {
        // Native export and project persistence have independent baselines.
        // Undo can return to the saved project even after exporting another revision.
        return m_stroke || m_settings_require_project || context_changed() || !(m_project_settings==m_settings_baseline) ||
            (is_model_result() && m_document.revision()!=m_model_project_revision) || (m_project_revision ?
            m_document.revision()!=*m_project_revision : m_document.dirty());
    }
    wxPoint m_last_pixel;
    wxPanel *m_preview;
    wxBitmap m_preview_bitmap;
    wxSpinCtrl *m_layer, *m_radius, *m_value;
    wxSpinCtrl *m_first, *m_last;
    wxChoice *m_zoom, *m_operation;
    wxChoice *m_plane = nullptr;
    wxStaticText *m_plane_info = nullptr;
    wxString m_plane_hint_text;
    int m_plane_hint_width = -1;
    wxStaticText *m_exposure_info = nullptr;
    sla::LayerMaskPlane selected_plane() const {
        return m_plane && m_plane->GetSelection()==1 ? sla::LayerMaskPlane::Secondary : sla::LayerMaskPlane::Main;
    }
    wxScrolledWindow *m_viewport;
    wxButton *m_undo, *m_redo;
    size_t m_cols = 0, m_rows = 0;
    void perform(const std::function<void()> &action) {
        if (m_working) return;
        m_preview_cancelled = false;
        try { action(); refresh(); }
        catch (const std::exception &error) {
            wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_ERROR, this);
        }
    }
    bool handle_history_shortcut(const wxKeyEvent &event) {
        if (!event.CmdDown() || event.AltDown()) return false;
        const int key = event.GetKeyCode();
        if (key != 'Z' && key != 'Y') return false;
        // All key routes discard the unfinished stroke before changing history.
        // In particular, a captured mouse may suppress the panel CHAR_HOOK.
        if (key == 'Y' || event.ShiftDown()) redo_change();
        else undo_change();
        return true;
    }
    void update_brush_value_hint() {
        if(!m_brush_value_info)return;
        const int requested=m_value->GetValue();
        const bool erasing=m_brush->GetSelection()==1;
        wxString message;
        if(erasing)message=wxString::Format(
            "Eraser paints %u; brush request %d is kept. Source palette: %u levels.",unsigned(m_edit_projection[0]),requested,m_edit_level_count);
        else if(requested<0 || requested>255)
            message="Brush grayscale must be between 0 and 255. The requested input is kept.";
        else message=wxString::Format("Brush source palette: %u levels. Requested %d -> paints %u. Threshold uses the requested value.",
                                     m_edit_level_count,requested,unsigned(m_edit_projection[size_t(requested)]));
        const int width=std::max(240,GetClientSize().x-24);
        if(message==m_brush_hint_text && width==m_brush_hint_width)return;
        m_brush_hint_text=message;m_brush_hint_width=width;
        m_brush_value_info->SetLabel(message);m_brush_value_info->Wrap(width);
        Layout();
    }
    void update_paint_scope() {
        if (m_paint_scope) {
            const bool range = m_paint_range->GetValue();
            m_paint_scope->SetLabel(range ? wxString::Format(
                "Painting layers %d–%d — drag on the overlay; one Undo restores the whole stroke.",
                m_first->GetValue(), m_last->GetValue()) : wxString::Format(
                "Painting layer %d only — enable the range below to paint several layers.", m_layer->GetValue()));
            m_paint_scope->Wrap(std::max(240, GetClientSize().x - 24));
        }
    }
    void update_mask_plane_info() {
        if (!m_plane_info) return;
        wxString message;
        if (m_paint_range->GetValue()) {
            // A range may mix shared and independent masks, and the current
            // layer may lie outside it. Describe the actual brush targets.
            const int first = m_first->GetValue(), last = m_last->GetValue();
            if (first < 1 || last < first || size_t(last) > m_document.layer_count()) {
                message = "Choose a valid layer range to see which masks will change.";
            } else {
                size_t shared = 0;
                for (size_t layer = size_t(first - 1); layer < size_t(last); ++layer)
                    shared += m_document.mask_is_shared(layer) ? 1 : 0;
                const size_t independent = size_t(last - first + 1) - shared;
                message = wxString::Format(
                    "Selected range: %zu shared %s, %zu independent %s. "
                    "Shared layers update both masks; independent layers update only the selected mask.",
                    shared, wxString(shared == 1 ? "layer" : "layers"),
                    independent, wxString(independent == 1 ? "layer" : "layers"));
            }
        } else {
            message = m_document.mask_is_shared(size_t(m_layer->GetValue() - 1)) ?
                "Shared mask: main and SUBIMGS stay linked when edited." :
                "Independent masks: edits affect only the selected image.";
        }
        const int width = std::max(240, GetClientSize().x - 24);
        if (message == m_plane_hint_text && width == m_plane_hint_width) return;
        m_plane_hint_text = message; m_plane_hint_width = width;
        m_plane_info->SetLabel(message); m_plane_info->Wrap(width);
        Layout();
    }
    void clear_preview(const wxString &message) {
        m_preview_bitmap = wxBitmap();
        m_cols = m_rows = 0;
        m_preview->Refresh(false);
        m_paint_scope->SetLabel(message);
        m_paint_scope->Wrap(std::max(240, GetClientSize().x - 24));
    }
    const png::ImageGreyscale *prepared_preview() {
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
        clear_preview("Preparing layer preview…");
        std::optional<sla::LayerPreviewCache::Prepared> prepared;
        std::atomic<bool> cancel_requested{false};
        bool loaded = false;
        try {
            {
                // The modal worker excludes edits and panel destruction while
                // it borrows PNG bytes. Join before publishing or touching wx.
                TaskLock working(m_working);
                loaded = run_background_layers(this, "Prepare layer preview", [&](const auto &progress) {
                    try {
                        prepared.emplace(sla::LayerPreviewCache::prepare(m_document, first, last, plane, progress));
                        return true;
                    } catch (const sla::LayerOperationCancelled &) { return false; }
                }, [&] { cancel_requested.store(true, std::memory_order_relaxed); });
            }
            if (!loaded || cancel_requested.load(std::memory_order_relaxed)) {
                m_preview_cancelled = true;
                clear_preview("Preview cancelled. Use Refresh preview or choose another layer or range.");
                return nullptr;
            }
            m_preview_cache.publish(std::move(*prepared), m_document, first, last, plane);
        } catch (...) {
            m_preview_cancelled = true;
            clear_preview("Preview unavailable. Use Refresh preview or choose another layer or range.");
            throw;
        }
        if (std::getenv("SLA_TRACE_LAYER_PREVIEW"))
            BOOST_LOG_TRIVIAL(info) << "Layer preview prepared: first=" << first << "; last=" << last
                << "; plane=" << int(plane) << "; decoded=" << last-first+1;
        update_paint_scope();
        return m_preview_cache.find(m_document, first, last, plane);
    }
    void refresh() {
        if (m_working) return;
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
        std::optional<png::ImageGreyscale> loaded;
        const png::ImageGreyscale *selected = m_stroke ? &*m_stroke : nullptr;
        if (!selected && m_reuse_preview) selected = prepared_preview();
        if (!selected && !m_reuse_preview) {
            loaded = m_paint_range->GetValue() ?
                m_document.overlay(size_t(m_first->GetValue()-1), size_t(m_last->GetValue()-1), selected_plane()) :
                m_document.image(size_t(m_layer->GetValue()-1), selected_plane());
            selected = &*loaded;
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
            m_exposure_info->SetLabel(wxString::Format("Exposure %zu/%zu; physical layer %zu/%zu; Z %.6g mm",
                selected+1, fresh.rows.size(), selected_physical, physical_count,
                fresh.rows[selected].at(sla::LayerParameter::PositionZ)));
        }
        static constexpr double zooms[] = {0., .25, .5, 1., 2., 4.};
        const auto available = m_viewport->GetClientSize();
        const double scale = m_zoom->GetSelection() == 0 ?
            std::min({1., double(std::max(320, available.x-12)) / m_cols,
                          double(std::max(200, available.y-12)) / m_rows}) : zooms[m_zoom->GetSelection()];
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
        m_preview->Refresh(false);
        m_preview->SetMinSize(wxSize(width, height));
        m_preview->SetSize(width, height);
        m_viewport->FitInside();
        m_undo->Enable(m_document.can_undo()); m_redo->Enable(m_document.can_redo());
        update_source_status();
        if (wxGetApp().mainframe) wxGetApp().mainframe->update_title();
        Layout();
        FitInside();
    }
    wxPoint pixel_at(const wxMouseEvent &event) const {
        const auto size = m_preview->GetClientSize();
        return {layer_canvas_pixel(event.GetX(), size.x, int(m_cols)),
                layer_canvas_pixel(event.GetY(), size.y, int(m_rows))};
    }
    void edit_layer_parameters(bool range = false) {
        finish_stroke(true);
        const auto layer=size_t((range ? m_first->GetValue() : m_layer->GetValue())-1);
        const auto last=range ? size_t(m_last->GetValue()-1) : layer;
        if (layer > last || last >= m_document.layer_count()) throw RuntimeError("Invalid parameter range");
        const auto values=m_document.layer_parameters(layer);
        wxDialog editor(this,wxID_ANY,range ? wxString::Format("Layer parameters — range %zu–%zu",layer+1,last+1) :
            wxString::Format("Layer parameters — layer %zu",layer+1));
        auto *root=new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(&editor,wxID_ANY,
            m_document.fresh_job() ?
            "Exposure and movement apply to this record. Z comes from generated layers and is read-only here.\nChanges keep the current layer schedule independent of preset updates." :
            "Parameters apply to this print layer and its mask planes. Read-only fields cannot be changed in this format."),0,wxALL,10);
        if (range) root->Add(new wxStaticText(&editor,wxID_ANY,
            "Values shown come from the first selected record. Only fields you change are applied to the range.\nOther settings and masks are preserved. The whole range is one Undo step."),0,wxALL,10);
        auto *grid=new wxFlexGridSizer(2,8,8);
        std::map<sla::LayerParameter,std::pair<wxTextCtrl *,wxString>> fields;
        for (const auto &capability:m_document.parameter_capabilities()) {
            grid->Add(new wxStaticText(&editor,wxID_ANY,wxString::FromUTF8(capability.label)+
                " ("+wxString::FromUTF8(capability.unit)+")"),0,wxALIGN_CENTER_VERTICAL);
            const auto text=wxString::Format("%.9g",values.at(capability.field));
            auto *input=new wxTextCtrl(&editor,wxID_ANY,text,wxDefaultPosition,wxDefaultSize,
                capability.editable ? 0 : wxTE_READONLY);
            input->SetName(wxString::FromUTF8(capability.label));grid->Add(input,1,wxEXPAND);
            if (capability.editable) fields.emplace(capability.field,std::make_pair(input,text));
        }
        grid->AddGrowableCol(1);root->Add(grid,1,wxEXPAND|wxALL,10);
        root->Add(editor.CreateButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,10);
        static_cast<wxButton *>(editor.FindWindow(wxID_OK))->SetLabel("Apply parameters");
        editor.SetSizerAndFit(root);editor.CentreOnParent();
        while (editor.ShowModal()==wxID_OK) {
            try {
                sla::LayerParameters patch;
                for (const auto &[field,input]:fields) if (input.first->GetValue()!=input.second) {
                    double value=0;
                    if (!input.first->GetValue().ToDouble(&value)) throw RuntimeError("Enter a numeric layer parameter");
                    patch.emplace(field,value);
                }
                if (range && !patch.empty()) {
                    auto table=m_document.fresh_job()->rows;
                    for (size_t i=layer; i<=last; ++i)
                        for (const auto &[field,value]:patch) table[i][field]=value;
                    m_document.replace_layer_parameter_table(table);
                } else if (!range) m_document.set_layer_parameters(layer,patch);
                return;
            } catch (const std::exception &error) {
                wxMessageBox(wxString::FromUTF8(error.what()),"Layer parameters",wxOK|wxICON_ERROR,&editor);
            }
        }
    }
    void finish_stroke(bool commit) {
        if (!m_stroke) return;
        const bool range = m_paint_range->GetValue();
        const auto first = size_t((range ? m_first : m_layer)->GetValue()-1);
        const auto last = size_t((range ? m_last : m_layer)->GetValue()-1);
        auto points = std::move(m_stroke_points);
        // Release capture before the modal progress window so Cancel receives
        // input. Also restore controls on a worker error or lost capture.
        m_stroke.reset();
        if (m_preview->HasCapture()) m_preview->ReleaseMouse();
        m_layer->Enable();
        m_first->Enable(); m_last->Enable(); m_paint_range->Enable(); m_brush->Enable();
        m_range_slider->Enable(); m_single_layer->Enable();
        m_radius->Enable(); m_value->Enable();
        if (m_plane) m_plane->Enable();
        if (commit) {
            TaskLock working(m_working);
            run_background_layers(this, "Paint layers", [&](const auto &progress) {
                return m_document.paint_stroke(first, last, points,
                    m_stroke_radius, m_stroke_value, selected_plane(), progress);
            });
        }
        refresh();
    }

    bool save_layer_project_impl(bool save_as) {
        if (is_model_result()) throw RuntimeError("Save model layers with the 3D project");
        finish_stroke(true);
        const boost::filesystem::path previous(m_layer_project_path);
        std::string path = m_layer_project_path;
        if (save_as || path.empty()) {
            wxFileDialog save(this, "Save layer project", wxString::FromUTF8(previous.parent_path().string()),
                previous.empty() ? "layers.sla-project" : wxString::FromUTF8(previous.filename().string()),
                "SLA layer project (*.sla-project)|*.sla-project", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (save.ShowModal() != wxID_OK) return false;
            path = boost::filesystem::path(into_u8(save.GetPath())).replace_extension(".sla-project").string();
            if (wxString::FromUTF8(path) != save.GetPath() && boost::filesystem::exists(path) &&
                wxMessageBox("Replace the existing layer project?\n" + wxString::FromUTF8(path),
                    "Save layer project", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
                return false;
        }
        auto state = m_print ? sla::capture_generated_project(*m_print, m_document) :
            sla::ProjectRasterState{m_document, {}, "", m_export_profile, sla::ProjectLayerOrigin::ImportedArchive, m_project_settings};
        state.settings = m_project_settings;
        TaskLock working(m_working);
        const bool saved = run_background_layers(this, "Save layer project", [&](const sla::LayerProgress &progress) {
            try { sla::save_layer_project(path, state, progress); return true; }
            catch (const sla::LayerOperationCancelled &) { return false; }
        });
        // Advance both the remembered path and the baseline only after publish.
        if (saved) {
            m_layer_project_path = path;
            m_project_revision = m_document.revision();
            m_context_baseline = m_export_profile;
            m_settings_baseline = m_project_settings;
            m_settings_require_project = false;
            m_document.mark_saved();
            if (auto *frame = wxGetApp().mainframe) frame->remember_layer_document(path);
        }
        return saved;
    }

public:
    LayerEditorContent(wxWindow *parent, sla::LayerDocument document,
                      std::optional<sla::LayerExportProfile> profile = {}, std::string project_path = {},
                      std::optional<sla::ProjectSLASettings> settings = {},
                      std::unique_ptr<sla::SlicedResultSession> session = {},
                      std::function<void()> close = {},
                      std::function<void(sla::SlicedResultSession &)> apply = {},
                      std::function<bool()> source_available = {})
        : LayerEditorPanel(parent, wxID_ANY), m_session(std::move(session)),
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
            m_plane=new wxChoice(this,wxID_ANY);m_plane->Append("Main mask");m_plane->Append("SUBIMGS mask");
            m_plane->SetSelection(0);m_plane->SetName("Mask plane");controls->Add(m_plane,0,wxALL,5);
            m_plane_info=new wxStaticText(this,wxID_ANY,"");
        }
        controls->Add(new wxStaticText(this, wxID_ANY, "Drawing tool"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
        m_brush = new wxChoice(this, wxID_ANY);
        m_brush->Append("Brush"); m_brush->Append("Eraser"); m_brush->SetSelection(0);
        m_brush->SetName("Drawing tool"); controls->Add(m_brush, 0, wxALL, 5);
        m_brush->SetToolTip("Brush paints the selected grayscale value. Eraser clears pixels. Drag directly on the mask preview.");
        m_radius = spin("Radius (pixels)", 0, 4096, 3);
        m_radius->SetName("Brush radius (pixels)");
        m_value = spin("Grayscale / threshold", 0, 255, 255);
        controls->Add(new wxStaticText(this, wxID_ANY, "Zoom"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
        m_zoom = new wxChoice(this, wxID_ANY);
        for (const char *label : {"Fit", "25%", "50%", "100%", "200%", "400%"}) m_zoom->Append(label);
        m_zoom->SetSelection(0); m_zoom->SetName("Preview zoom");
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
        m_viewport = new wxScrolledWindow(this);
        m_viewport->SetMinSize(wxSize(200,200));
        m_viewport->SetScrollRate(16, 16);
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
        });
        preview_sizer->Add(m_preview, 0, wxALL, 4);
        m_viewport->SetSizer(preview_sizer);
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
        range_controls = new wxWrapSizer(wxHORIZONTAL);
        range_controls->Add(new wxStaticText(this, wxID_ANY, "Range filters"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
        m_operation = new wxChoice(this, wxID_ANY);
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
        apply_range->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CallAfter([this] { perform([&] {
            if (m_first->GetValue() > m_last->GetValue()) throw RuntimeError("First layer must not exceed last layer");
            TaskLock working(m_working);
            wxDialog progress(this, wxID_ANY, "Edit layer range");
            auto *progress_sizer = new wxBoxSizer(wxVERTICAL);
            auto *status = new wxStaticText(&progress, wxID_ANY, "Preparing masks");
            auto *gauge = new wxGauge(&progress, wxID_ANY, 1000, wxDefaultPosition, wxSize(360, -1));
            auto *abort = new wxButton(&progress, wxID_CANCEL, "Cancel");
            progress_sizer->Add(status, 0, wxALL, 12);
            progress_sizer->Add(gauge, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
            progress_sizer->Add(abort, 0, wxALIGN_RIGHT | wxALL, 12);
            progress.SetSizerAndFit(progress_sizer); progress.CentreOnParent();
            const auto first = size_t(m_first->GetValue()-1), last = size_t(m_last->GetValue()-1);
            const auto operation = static_cast<sla::LayerDocument::MaskOperation>(m_operation->GetSelection());
            const auto value = uint8_t(m_value->GetValue());
            const auto radius = unsigned(m_radius->GetValue());
            const auto plane=selected_plane();
            std::atomic<size_t> completed{0};
            std::atomic<bool> cancel{false};
            // Only the worker accesses the document until get() completes. The
            // application-modal progress window disables other editing controls.
            // The callback exchanges plain values; wxWidgets stays on its UI thread.
            auto task = std::async(std::launch::async, [&] {
                return m_document.transform_layers(first, last, operation, value,
                    [&](size_t done, size_t) {
                        completed.store(done, std::memory_order_relaxed);
                        return !cancel.load(std::memory_order_relaxed);
                    }, radius,plane);
            });
            auto request_cancel = [&] {
                cancel.store(true, std::memory_order_relaxed);
                abort->Disable(); status->SetLabel("Cancelling…");
            };
            progress.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { request_cancel(); }, wxID_CANCEL);
            progress.Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent &event) {
                request_cancel(); if (event.CanVeto()) event.Veto();
            });
            // A normal modal event loop also services GTK accessibility and
            // native input while the worker runs. No synchronous Yield loop.
            wxTimer timer(&progress);
            progress.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
                if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    timer.Stop(); progress.EndModal(wxID_OK); return;
                }
                const size_t done = completed.load(std::memory_order_relaxed);
                gauge->SetValue(int(std::min(size_t(999), done * 1000 / (last-first+1))));
                if (!cancel.load(std::memory_order_relaxed))
                    status->SetLabel(wxString::Format("Layers: %zu / %zu", done, last-first+1));
            });
            timer.Start(30); progress.ShowModal(); timer.Stop();
            task.get();
        }); }); });
        auto *buttons = new wxWrapSizer(wxHORIZONTAL);
        auto button = [&](const char *text, std::function<void()> action) {
            auto *b = new wxButton(this, wxID_ANY, text); buttons->Add(b, 0, wxALL, 5);
            b->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent &) {
                CallAfter([this, action] { perform(action); });
            }); return b;
        };
        if (!m_document.parameter_capabilities().empty())
            button("Layer parameters",[this]{edit_layer_parameters();});
        if (m_document.fresh_job())
            button("Range parameters",[this]{edit_layer_parameters(true);});
        m_undo = button("Undo", [this] { m_document.undo(); });
        m_redo = button("Redo", [this] { m_document.redo(); });
        button("Threshold layer", [this] { m_document.threshold(size_t(m_layer->GetValue()-1), uint8_t(m_value->GetValue()),selected_plane()); });
        button("Invert layer", [this] { m_document.invert(size_t(m_layer->GetValue()-1),selected_plane()); });
        if (m_print) {
            m_format = new wxChoice(this, wxID_ANY);
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
            if (m_print->is_raster_job()) m_format->Bind(wxEVT_CHOICE,[this](wxCommandEvent &event) {
                m_project_settings->printer.values["sla_archive_format"]=m_formats.at(size_t(m_format->GetSelection()))->id;
                refresh(); event.Skip();
            });
            auto *format_note = new wxStaticText(this, wxID_ANY, "");
            root->Add(format_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
            auto update_note = [this, format_note] {
                const int selection = m_format->GetSelection();
                const char *id = selection == wxNOT_FOUND ? "" : m_formats.at(size_t(selection))->id;
                const bool images_only = std::strcmp(id, "genericzip") == 0 || std::strcmp(id, "vda") == 0;
                const auto &versions = anycubic_available_versions(id);
                uint16_t version = 0;
                wxString version_error;
                if (!versions.empty()) {
                    // Read the default from the same registered factory as
                    // export, rather than maintaining a second version map.
                    try {
                        const auto writer = get_archive_entry(id)->wrfactoryfn(m_print->printer_config());
                        const auto *anycubic = dynamic_cast<const AnycubicSLAArchive*>(writer.get());
                        if (!anycubic) throw std::logic_error("Invalid Anycubic archive registration");
                        version = anycubic_resolve_version(id, m_print->printer_config().sla_anycubic_version.value, anycubic->default_version());
                    }
                    catch (const std::exception &error) { version_error = wxString::FromUTF8(error.what()); }
                }
                const bool machine_table = version >= 516;
                const auto *legacy = machine_table ? nullptr : anycubic_legacy_geometry(id);
                wxString note;
                if (!version_error.empty()) note = version_error;
                else if (images_only) note = "This ZIP format saves masks and geometry. Set exposure and motion on the receiving printer.";
                else if(std::strcmp(id,"klipper")==0 || std::strcmp(id,"klipper_rgb")==0)
                    note="Klipper ZIP requires matching MSLA firmware macros for image display, timed UV exposure and layer changes. Configure two-stage lift/retract, PWM and waits in Material Settings > LCD exposure and motion. A positive first lift and retract stage and PWM 1–255 are required by this encoder. RGB uses full-width PNG24 masks.";
                else if(std::strcmp(id,"cwsrgb")==0) note = "RGB.CWS packs three horizontal mask pixels into one PNG pixel. The LCD raster width after orientation must be divisible by three. Full LCD dimensions and all grayscale values are preserved.";
                else if(std::strcmp(id,"cbddlp")==0 || std::strcmp(id,"photon")==0)
                    note=wxString::Format("CBDDLP/Photon v2 uses %d AA exposure planes from the printer preset. Counts other than powers of two decode maximum white below 255 and may change pixels on re-encoding. Configure AA in Printer Settings > General > Output.", m_print->printer_config().sla_cbddlp_aa.value);
                else if(std::strcmp(id,"cbddlp_v1")==0 || std::strcmp(id,"photon_v1")==0)
                    note="CBDDLP/Photon v1 uses one binary plane: only input value 255 exposes. It cannot store custom lift/retract settings; the decoder assumes normal lift 0.05 mm, bottom lift 6 mm and speeds 100 mm/min. Verify firmware motion separately.";
                else if((std::strcmp(id,"cxdlp")==0 || std::strcmp(id,"cxdlpv2")==0)) note = "CXDLP v2/v3 requires a CL/CT printer model, fixed layer height, integer speeds in mm/s, normal exposure in 0.1 s, integer bottom exposure and a shared wait of at least 1 s. Machine Z is not stored.";
                else if(std::strcmp(id,"cxdlpv4")==0) note = "CXDLPv4 stores 7-bit grayscale: values 0 and 1 are off; other values are quantized. Volume and contour metadata describe the encoded masks.";
                else if(std::strcmp(id,"cxdlpv1")==0) note = "CXDLPv1 requires fixed layer height, integer exposure/motion values and a shared wait of at least 1 second. This format does not store machine Z.";
                else if (version == 518)
                    note="Anycubic 518 supports two-stage lift and separate bottom/normal retract speeds. Configure them in Material Settings > LCD exposure and motion. Wait before exposure is shared; custom PWM, after-waits and second retract height are unavailable.";
                else if (machine_table) note = "Anycubic 516/517 supports two-stage lift and separate bottom/normal retract speeds. Configure them in Material Settings > LCD exposure and motion. Screen dimensions and machine Z come from the printer preset. Wait before exposure is shared.";
                else if (legacy) note = wxString::Format(
                    "This legacy format requires an oriented screen of %.4g x %.4g mm and Z %.4g mm. Custom dimensions require a format with a machine table.",
                    legacy->width, legacy->height, legacy->machine_z);
                else if (!versions.empty() && version < 516)
                    note = "PWC 1/515 saves masks, pixel pitch, exposure and single-stage motion, but has no machine table or printer identity. UVtools reports screen dimensions and machine Z as zero. Select PWC 516 or newer in Printer Settings > General > Output to preserve physical dimensions.";
                format_note->SetLabel(note);
                format_note->Wrap(650); format_note->Show(!note.empty()); Layout();
            };
            m_format->Bind(wxEVT_CHOICE, [update_note](wxCommandEvent &event) {
                update_note();
                // Continue to the project-settings handler registered above.
                // Updating the hint must not consume the selected format.
                event.Skip();
            });
            update_note();
            if (!m_print->is_raster_job()) m_apply_button = button("Apply to sliced result", [this] { apply_changes(); });
        } else if (!anycubic_available_versions(m_document.archive_format()).empty()) {
            auto *note = new wxStaticText(this, wxID_ANY, "Anycubic edits keep the original file version, per-layer parameters, previews and manufacturer metadata. Masks are quantized to the original grayscale or PWS AA levels. Volume and model bounds follow the edited masks when physical dimensions are known. Weight and cost scale existing estimates; zero values do not provide material properties. Changed masks append payloads; older unused payloads remain in the file.");
            note->Wrap(650); root->Add(note, 0, wxEXPAND|wxALL, 5);
            auto *use_profile = new wxCheckBox(this, wxID_ANY, m_reopened_profile ?
                "Use saved project context for resin estimates" : "Use selected printer and material for resin estimates");
            m_use_profile=use_profile;
            use_profile->SetValue(m_export_profile.has_value());
            use_profile->SetToolTip("Opt in to recalculate volume, weight and cost. The selected printer must match the mask resolution and known screen dimensions. Masks and exposure settings are preserved. Legacy PWC cannot store physical screen dimensions; keep its matching profile when reopening.");
            root->Add(use_profile, 0, wxALL, 5);
            auto *summary = new wxStaticText(this, wxID_ANY, "Resin estimates: original file values");
            root->Add(summary, 0, wxEXPAND|wxALL, 5);
            auto show_profile = [this, summary] {
                if (!m_export_profile) summary->SetLabel("Resin estimates: original file values");
                else {
                    const auto &profile=*m_export_profile;
                    summary->SetLabel(wxString::Format(m_reopened_profile ?
                        "Saved project estimates: %.4g x %.4g mm; %.4g g/ml; %.4g per ml; max Z %.4g mm. Active presets are unchanged." :
                        "Resin estimates: %.4g x %.4g mm; %.4g g/ml; %.4g per ml; max Z %.4g mm. Screen dimensions and layer heights are checked on save.",
                        profile.screen_width_mm, profile.screen_height_mm, profile.density_g_ml, profile.cost_per_ml, profile.max_print_height_mm));
                }
                summary->Wrap(650); refresh();
            };
            m_update_profile_summary=show_profile;
            // Controls used by refresh are already constructed at this point.
            if (m_export_profile) {
                const auto &profile=*m_export_profile;
                summary->SetLabel(wxString::Format("Saved project estimates: %.4g x %.4g mm; %.4g g/ml; %.4g per ml; max Z %.4g mm. Active presets are unchanged.",
                    profile.screen_width_mm, profile.screen_height_mm, profile.density_g_ml, profile.cost_per_ml, profile.max_print_height_mm));
                summary->Wrap(650);
            }
            use_profile->Bind(wxEVT_CHECKBOX, [this, use_profile, summary, show_profile](wxCommandEvent &) {
                // Leave the GTK toggle callback before opening a modal error dialog.
                CallAfter([this, use_profile, summary, show_profile] {
                if (m_working) { use_profile->SetValue(m_export_profile.has_value()); return; }
                m_export_profile.reset();
                summary->SetLabel("Resin estimates: original file values");
                if (!use_profile->GetValue()) { show_profile(); return; }
                try {
                    if (m_reopened_profile) {
                        m_export_profile=m_reopened_profile;
                        show_profile(); return;
                    }
                    const auto cfg = wxGetApp().preset_bundle->full_config();
                    if (cfg.option("printer_technology")->getInt() != ptSLA)
                        throw RuntimeError("Select an SLA printer and material before using their resin estimates");
                    sla::LayerExportProfile profile;
                    profile.columns = cfg.option("display_pixels_x")->getInt();
                    profile.rows = cfg.option("display_pixels_y")->getInt();
                    profile.screen_width_mm = cfg.option("display_width")->getFloat();
                    profile.screen_height_mm = cfg.option("display_height")->getFloat();
                    if (cfg.option("display_orientation")->getInt() == sladoPortrait) {
                        std::swap(profile.columns, profile.rows);
                        std::swap(profile.screen_width_mm, profile.screen_height_mm);
                    }
                    profile.max_print_height_mm = cfg.option("max_print_height")->getFloat();
                    if (!std::isfinite(profile.max_print_height_mm) || profile.max_print_height_mm<=0)
                        throw RuntimeError("Selected printer requires a positive finite maximum print height");
                    profile.density_g_ml = cfg.option("material_density")->getFloat();
                    const double bottle_volume = cfg.option("bottle_volume")->getFloat();
                    if (!std::isfinite(bottle_volume) || bottle_volume <= 0)
                        throw RuntimeError("Selected material requires a positive finite bottle volume");
                    profile.cost_per_ml = cfg.option("bottle_cost")->getFloat()/bottle_volume;
                    const auto mask = m_document.image(0);
                    profile.validate(mask.cols, mask.rows);
                    const auto settings=sla::capture_project_settings(cfg);
                    m_export_profile = profile;
                    m_project_settings=settings;
                    show_profile();
                } catch (const std::exception &error) {
                    use_profile->SetValue(false);
                    show_profile();
                    wxMessageBox(wxString::FromUTF8(error.what()), "Layer export profile", wxOK|wxICON_ERROR, this);
                }
                });
            });
        } else if (sla::is_ctb_mask_format(m_document.archive_format())) {
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
        if (!m_print || m_print->is_raster_job()) {
            if (m_print && m_print->calibration_recipe()) button("Calibration recipe", [this] {
                const auto &recipe=*m_print->calibration_recipe();const auto &p=recipe.parameters;
                const char *patterns[]={"XYZ blocks","Grayscale patches","Lift area","ExposureFinder"};
                wxString summary="Original generation recipe. Current masks may contain later edits.\n\n";
                summary+=wxString::FromUTF8(patterns[int(p.pattern)]);
                summary+=wxString::Format("\nPattern X: %.17g mm\nPattern Y: %.17g mm\nHeight: %.17g mm\n",p.size_x,p.size_y,p.height);
                if(!(recipe.exposure_finder && recipe.exposure_finder->generation))
                    summary+=wxString::Format("Spacing / margin: %.17g mm\n",p.margin);
                if (p.pattern == sla::CalibrationPattern::LiftArea)
                    summary += wxString::Format("\nLift size step: %u%% (0 = disabled)\nMinimum lift size: %u%%\nPercentages apply to both X and Y dimensions; decreasing layers are included in total height.\n",
                        p.lift_taper_step,p.lift_taper_min);
                if (recipe.exposure_finder) {
                    const auto &table=*recipe.exposure_finder;
                    summary+=wxString::Format("\nTest table: %s\nRows: %u\nHeight (mm) | Bottom (s) | Normal (s) | Requested brightness\n",
                        table.table_origin==sla::ExposureFinderTableOrigin::Generated ? "Generated" : "Manual",unsigned(table.rows.size()));
                    for(size_t i=0;i<std::min(size_t(100),table.rows.size());++i) {
                        const auto &row=table.rows[i];
                        summary+=wxString::Format("%.3f | %.2f | %.2f | %u%s\n",row.layer_height,row.bottom_exposure,
                            row.normal_exposure,unsigned(row.brightness),row.valid()?"":" (invalid)");
                    }
                    if(table.rows.size()>100)summary+="Showing first 100 rows. The project retains the complete table.\n";
                    if(table.schedule) {
                        const auto &p=*table.schedule;
                        const char *modes[]={"Iterative","First table row","Middle table row","Last table row","Custom"};
                        const char *exclusions[]={"None","Bottom","Bottom and base"};
                        summary+=wxString::Format("\nExposure schedule\nBase height: %.9g mm\nFeature height: %.9g mm\nBottom layers per test: %u\nBase exposure mode: %s\nCustom base exposure: %.9g s (cure grid 0.01 s)\n",
                            p.base_height,p.features_height,p.bottom_layers,modes[int(p.base_mode)],p.custom_base_exposure);
                        summary+=wxString::Format("Multiple exposures: %s\nMultiple brightness: %s\nStored brightness: ",
                            p.multiple_exposures?"Yes":"No",p.multiple_brightness?"Yes":"No");
                        for(size_t i=0;i<p.brightness.size();++i) {
                            if(i)summary+=",";
                            summary+=wxString::Format("%u",unsigned(p.brightness[i]));
                        }
                        summary+=wxString::Format("\nBrightness exclusion: %s\nRepeated-Z settings: %s\nTotal lift override: %.9g mm (%s)\nWait before override: %.9g s (%s)\n",
                            exclusions[int(p.brightness_exclusion)],p.repeated_settings?"Enabled":"Disabled",
                            p.repeated_lift_height,p.repeated_lift_enabled?"Enabled":"Disabled",
                            p.repeated_wait_before,p.repeated_wait_enabled?"Enabled":"Disabled");
                    }
                }
                if(recipe.exposure_finder && recipe.exposure_finder->generation) {
                    const auto &g=*recipe.exposure_finder->generation;const auto &p=g.pattern;const auto &l=g.layout;
                    summary+="\nPattern geometry\nHoles (px): ";
                    for(auto v:p.holes)summary+=wxString::Format("%u ",v);
                    if(p.holes.empty())summary+="None";
                    summary+=wxString::Format("\nHole shape: %s\nBars (px): ",p.square_holes?"Square":"Circle");
                    for(auto v:p.bars)summary+=wxString::Format("%u ",v);
                    if(p.bars.empty())summary+="None";
                    summary+="\nBullseyes diameter:thickness (px): ";
                    for(const auto &ring:p.bullseyes)summary+=wxString::Format("%u:%u ",ring[0],ring[1]);
                    if(p.bullseyes.empty())summary+="None";
                    summary+=wxString::Format("\nFeature margin: %.9g mm\nBar length: %.9g mm\nBar spacing: %.9g mm\nBar splitter: %d px\nBar fence: %d px\nBar fence offset: %d px\n",
                        p.feature_margin_mm,p.bar_length_mm,p.bar_spacing_mm,p.bar_splitter,p.bar_fence,p.bar_fence_offset);
                    summary+=wxString::Format("Bullseye fence: %d px\nBullseye fence offset: %d px\nInvert bullseye quadrants: %s\nCounter triangles: %s\nTriangle fence: %s\nTriangle tip offset: %d px\nStaircase thickness: %u px\nAnti-aliasing: %s\n",
                        p.bullseye_fence,p.bullseye_fence_offset,p.invert_bullseye_quadrants?"Yes":"No",p.counter_triangles?"Yes":"No",p.triangle_fence?"Yes":"No",p.triangle_tip_offset,p.staircase_thickness,p.antialias?"Yes":"No");
                    const char *fonts[]={"Simplex","Plain","Duplex","Complex","Triplex","Complex small","Script simplex","Script complex"};
                    summary+="Text: "+wxString::FromUTF8(p.text)+"\n";
                    summary+=wxString::Format("Font: %s%s\nText scale: %.9g (rounded to 0.01)\nText thickness: %d px\n",
                        fonts[p.font_face&15],(p.font_face&16)?" italic":"",p.text_scale,p.text_thickness);
                    summary+="\nLCD layout\nCell columns: "+(l.cell_columns?wxString::Format("%u px",unsigned(l.cell_columns)):wxString("Auto"));
                    summary+="\nCell rows: "+(l.cell_rows?wxString::Format("%u px",unsigned(l.cell_rows)):wxString("Auto"));
                    summary+=wxString::Format("\nMargin X: %.9g mm\nMargin Y: %.9g mm\nGap X: %.9g mm\nGap Y: %.9g mm\nCenter cells: %s\nExtra mirror X: %s\nExtra mirror Y: %s\nBottom erosion: %u px\nChamfer layers: %u\n",
                        l.margin_x_mm,l.margin_y_mm,l.gap_x_mm,l.gap_y_mm,l.center?"Yes":"No",l.mirror_x?"Yes":"No",l.mirror_y?"Yes":"No",g.bottom_erosion,g.chamfer_layers);
                }
                if (!recipe.requested.empty()) {
                    summary+="\nRequested -> generated grayscale (0-255):\n";
                    for(size_t i=0;i<recipe.requested.size();++i)
                        summary+=wxString::Format("%u -> %u\n",unsigned(recipe.requested[i]),unsigned(recipe.effective[i]));
                }
                wxDialog details(this,wxID_ANY,"Calibration recipe",wxDefaultPosition,wxSize(560,440),wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
                auto *layout=new wxBoxSizer(wxVERTICAL);
                auto *text=new wxTextCtrl(&details,wxID_ANY,summary,wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY);
                text->SetName("Original calibration recipe");layout->Add(text,1,wxEXPAND|wxALL,10);
                layout->Add(details.CreateSeparatedButtonSizer(wxOK),0,wxEXPAND|wxALL,10);
                details.SetSizer(layout);details.CentreOnParent();details.ShowModal();
            });
            if (!m_print) button("Capture selected SLA settings", [this] {
                const auto config=wxGetApp().preset_bundle->full_config();
                auto settings=sla::capture_project_settings(config);
                size_t columns=config.option("display_pixels_x")->getInt(), rows=config.option("display_pixels_y")->getInt();
                if (config.option("display_orientation")->getInt()==sladoPortrait) std::swap(columns,rows);
                const auto image=m_document.image(0);
                if (columns!=image.cols || rows!=image.rows)
                    throw RuntimeError("Selected printer resolution differs from the imported layer raster");
                if (m_export_profile) {
                    auto profile=*m_export_profile;
                    profile.columns=columns;profile.rows=rows;
                    profile.screen_width_mm=config.option("display_width")->getFloat();
                    profile.screen_height_mm=config.option("display_height")->getFloat();
                    if (config.option("display_orientation")->getInt()==sladoPortrait)
                        std::swap(profile.screen_width_mm,profile.screen_height_mm);
                    profile.density_g_ml=config.option("material_density")->getFloat();
                    profile.cost_per_ml=config.option("bottle_cost")->getFloat()/config.option("bottle_volume")->getFloat();
                    profile.max_print_height_mm=config.option("max_print_height")->getFloat();
                    profile.validate(image.cols,image.rows);
                    m_export_profile=profile;
                    m_reopened_profile.reset();
                    if (m_use_profile) m_use_profile->SetLabel("Use selected printer and material for resin estimates");
                }
                m_project_settings=std::move(settings);
                m_settings_require_project=!m_project_revision.has_value();
                if (m_update_profile_summary) m_update_profile_summary();
            });
            button("Project SLA settings", [this] {
                if (!m_project_settings) throw RuntimeError("Capture selected SLA settings before reviewing them");
                wxDialog details(this,wxID_ANY,"Project SLA settings",wxDefaultPosition,wxSize(700,600),
                    wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
                auto *layout=new wxBoxSizer(wxVERTICAL);
                layout->Add(new wxStaticText(&details,wxID_ANY,
                    "Saved project settings. Active presets are unchanged. Imported native layer parameters take precedence over their project defaults."),0,wxEXPAND|wxALL,10);
                wxString text;
                auto role=[&](const char *label,const sla::ProjectPresetSettings &preset) {
                    text+=wxString::FromUTF8(label)+": "+wxString::FromUTF8(preset.name)+"\n";
                    for (const auto &[key,value]:preset.values)
                        text+=wxString::FromUTF8(key)+" = "+wxString::FromUTF8(value)+"\n";
                    if(preset.metadata) {
                        text+="Profile relationships (resolved values work without the referenced base):\n";
                        for(const auto &[key,value]:*preset.metadata)
                            text+=wxString::FromUTF8(key)+" = "+wxString::FromUTF8(value)+"\n";
                    } else text+="Profile relationships were not stored in this legacy project.\n";
                    text+="\n";
                };
                role("Printer",m_project_settings->printer);role("Material",m_project_settings->material);role("Process",m_project_settings->process);
                auto *values=new wxTextCtrl(&details,wxID_ANY,text,wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY);
                values->SetName("Resolved SLA project settings");layout->Add(values,1,wxEXPAND|wxALL,10);
                layout->Add(details.CreateButtonSizer(wxOK),0,wxEXPAND|wxALL,10);
                details.SetSizer(layout);details.CentreOnParent();details.ShowModal();
            });
        }
        if (!m_print || m_print->is_raster_job())
            button("Save layer project", [this] { save_layer_project_impl(true); });
        m_export_action = [this] {
            const auto *image_format = !m_print ? sla::find_layer_image_format(m_document.archive_format()) : nullptr;
            const bool png = image_format != nullptr;
            const auto *entry = png ? nullptr : m_print ? m_formats.at(size_t(m_format->GetSelection())) : get_archive_entry(m_document.archive_format().c_str());
            if (!png && !entry) throw RuntimeError("Unknown layer container");
            if (m_print && std::string(entry->id)=="cxdlpv4") {
                const auto error=sla::cxdlpv4_printer_identifier_error(m_print->printer_config().printer_model.value,
                    bool(m_document.fresh_job()));
                if (!error.empty()) throw RuntimeError(error+
                    "\n\nSet Printer > General > Output > Printer model (firmware), then save the printer preset and try again.");
            }
            auto filename = [&](const std::string &path) {
                return png ? boost::filesystem::path(path).replace_extension(std::string(".")+image_format->id).string() : sla_archive_filename(path, entry->ext);
            };
            const char *project = m_print && m_print->is_raster_job() ? "generated" : "edited";
            const auto name = filename(project);
            const auto filter = png ? wxString::FromUTF8(image_format->description) + " (*." + image_format->id + ")|*." + image_format->id : wxString::FromUTF8(entry->desc) + " (*." + entry->ext + ")|*." + entry->ext;
            wxFileDialog save(this, "Save edited layers", "", wxString::FromUTF8(name), filter,
                              wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (save.ShowModal() == wxID_OK) {
                if (image_format && image_format->lossy && !image_format->binary &&
                    wxMessageBox("Saving JPEG may change exposure pixels. Continue?", "Save edited image",
                        wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
                const auto path = filename(save.GetPath().ToUTF8().data());
                // The native dialog checks the typed path. A corrected suffix
                // can target a different existing file; confirm that one too.
                if (wxString::FromUTF8(path) != save.GetPath() && boost::filesystem::exists(path) &&
                    wxMessageBox("Replace the existing file?\n" + wxString::FromUTF8(path),
                        "Save edited layers", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
                    return;
                TaskLock working(m_working);
                if (m_print) {
                    bool times_projected=false;
                    if((std::string(entry->id)=="SL1" || std::string(entry->id)=="SL1S") && m_document.fresh_job()) {
                        std::optional<sla::SL1ExposureTransitionPreview> transition_plan;
                        const bool analyzed = run_background_layers(this, "Check exposure transition",
                            [&](const sla::LayerProgress &progress) {
                                try {
                                    transition_plan = sla::sl1_exposure_transition_preview(m_document, progress);
                                    return true;
                                } catch (const sla::LayerOperationCancelled &) { return false; }
                            });
                        if (!analyzed) return;
                        times_projected=transition_plan->projected_transition_exposures!=0;
                        if(times_projected && wxMessageBox(wxString::Format(
                            "SL1 uses a global exposure transition. Exporting will project %llu transition exposures; the largest change is %.6g seconds.\n\nContinue? The original project parameters will be kept.",
                            static_cast<unsigned long long>(transition_plan->projected_transition_exposures),transition_plan->maximum_exposure_change),
                            "Confirm exposure transition",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
                    }
                    if(std::string(entry->id)=="cws" && m_document.fresh_job()) {
                        std::optional<sla::CWSFreshExportPlan> time_plan;
                        run_background_write(this,"Check export times",[&] {time_plan=sla::cws_fresh_export_plan(m_document);});
                        times_projected=time_plan->projected_time_fields!=0;
                        if(time_plan->projected_time_fields && wxMessageBox(wxString::Format(
                            "CWS uses whole milliseconds. Exporting will round %llu time values; the largest change is %.6g seconds.\n\nContinue? The original project parameters will be kept.",
                            static_cast<unsigned long long>(time_plan->projected_time_fields),time_plan->maximum_time_change),
                            "Confirm export times",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
                    }
                    std::optional<sla::LayerDocument> projected_copy;
                    const auto &anycubic_versions=anycubic_available_versions(entry->id);
                    const bool cxdlpv4=std::string(entry->id)=="cxdlpv4";
                    const auto precision=cxdlpv4?sla::LayerMaskPrecision::NativeGray7:sla::LayerMaskPrecision::NativeGray4;
                    if(m_document.fresh_job() && (cxdlpv4 || std::any_of(anycubic_versions.begin(),anycubic_versions.end(),
                        [](uint16_t version){return version>=516 && version<=518;}))) {
                        sla::LayerPaletteLoss loss;
                        const bool analyzed=run_background_layers(this,"Check export palette",[&](const sla::LayerProgress &progress) {
                            try {loss=sla::layer_palette_loss(m_document,precision,progress);return true;}
                            catch(const sla::LayerOperationCancelled &){return false;}
                        });
                        if(!analyzed)return;
                        if(loss.changed_pixels) {
                            const auto family=cxdlpv4?wxString("CXDLPv4"):std::string(entry->id)=="pwc"?wxString("PWC"):wxString("Anycubic");
                            const auto message=family+wxString::Format(" uses %u grayscale levels. Exporting will change %llu pixels; the largest change is %u of 255.\n\nExport a converted copy? Your original layers and Undo history will be kept.",
                                cxdlpv4?128u:16u,static_cast<unsigned long long>(loss.changed_pixels),loss.maximum_change);
                            if(wxMessageBox(message,"Convert export palette",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
                            const bool converted=run_background_layers(this,"Convert export palette",[&](const sla::LayerProgress &progress) {
                                try {projected_copy=sla::project_layer_palette_copy(m_document,precision,progress);return true;}
                                catch(const sla::LayerOperationCancelled &){return false;}
                            });
                            if(!converted)return;
                        }
                    }
                    // The session owns a const context. TaskLock + this modal
                    // keep the edited document stable until the worker joins.
                    // Every attempt has a fresh cancellation flag, while the
                    // context's commit mutex protects the existing destination.
                    BackgroundRequestOwner<sla::ExportGenerationGuard> request_owner;
                    const bool saved=run_background_layers(this, "Export layers", [&](const sla::LayerProgress &progress) {
                        try {
                            const auto request=std::make_shared<const sla::ExportGenerationGuard>(
                                m_print->export_generation_guard().with_cancellation());
                            if (!request_owner.bind(request)) return false;
                            const auto &document = projected_copy ? *projected_copy : m_document;
                            const auto check = [&] { sla::check_layer_progress(progress); };
                            const char *snapshot_flag = std::getenv("SLA_EXPERIMENT_SL1_EDITOR_SNAPSHOT");
                            // The private SL1 copy permits verified PNG reuse even
                            // before Apply. Other writers keep their existing path
                            // until their capture cost has been measured separately.
                            const bool sl1 = std::string(entry->id) == "SL1" || std::string(entry->id) == "SL1S";
                            if (sl1 && (!snapshot_flag || std::string(snapshot_flag) == "1"))
                                m_print->export_layer_document_snapshot(document, entry->id,
                                    path, {}, project, check, request.get());
                            else
                                m_print->export_layer_document(document, entry->id,
                                    path, {}, project, check, request.get());
                            return true; // Publication won; a later Cancel cannot undo it.
                        } catch(const sla::LayerOperationCancelled &) { return false; }
                        catch(const CanceledException &) { return false; }
                    }, [&] { request_owner.cancel(); });
                    if (!saved) return;
                    if (m_print->is_raster_job() && !projected_copy && !times_projected) {
                        m_generated_exported = true; m_document.mark_saved();
                        if (!m_project_revision) m_settings_baseline=m_project_settings;
                    }
                    if (!m_printer_name.empty()) wxGetApp().app_config->set_sla_export_format(m_printer_name, entry->id);
                } else {
                    BackgroundRequestOwner<sla::NativeArchiveExportRequest> request_owner;
                    const bool saved=run_background_layers(this,"Save layer archive",[&](const sla::LayerProgress &progress) {
                        try {
                            const auto request=std::make_shared<const sla::NativeArchiveExportRequest>(m_document,path,m_export_profile,progress);
                            if (!request_owner.bind(request)) return false;
                            request->write(progress);
                            return true;
                        } catch(const sla::LayerOperationCancelled &) { return false; }
                        catch(const CanceledException &) { return false; }
                    }, [&] { request_owner.cancel(); });
                    const auto request=request_owner.request();
                    const bool acknowledged = saved && request && request->acknowledge_saved(m_document);
                    if (acknowledged && !m_project_revision) {
                        m_context_baseline=m_export_profile;
                        // Native containers cannot carry every process/preset option.
                        if (!m_settings_require_project) m_settings_baseline=m_project_settings;
                    }
                }
            }
        };
        button(m_print ? (m_print->is_raster_job() ? "Export generated layers" : "Export edited layers") : (sla::find_layer_image_format(m_document.archive_format()) ? "Save edited image" : "Save edited archive"), m_export_action);
        if(!m_print && !m_document.parameter_capabilities().empty())
            button("Convert archive",[this] {
                if(!m_project_settings)
                    throw RuntimeError("Capture selected SLA settings before converting to another printer format");
                const auto configuration=sla::restore_project_settings(*m_project_settings);
                wxDialog conversion(this,wxID_ANY,"Convert layer archive",wxDefaultPosition,wxSize(620,260),
                    wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
                auto *layout=new wxBoxSizer(wxVERTICAL);
                auto *description=new wxStaticText(&conversion,wxID_ANY,
                    "Destination printer: "+wxString::FromUTF8(m_project_settings->printer.name)+
                    "\nPer-layer parameters must be representable by the destination printer format.");
                description->Wrap(570);layout->Add(description,0,wxEXPAND|wxALL,10);
                auto *choice=new wxChoice(&conversion,wxID_ANY);
                std::vector<const ArchiveEntry *> formats;
                const auto source_fields=m_document.parameter_capabilities();
                for(const auto &entry:registered_sla_archives()) {
                    const auto schema=sla::parameter_conversion_schema(entry.id);
                    if(schema.empty() || !archive_export_unavailable_reason(entry.id).empty())continue;
                    if(!std::all_of(source_fields.begin(),source_fields.end(),[&](const auto &source) {
                        return std::any_of(schema.begin(),schema.end(),[&](const auto &target){return source.field==target.field;});
                    }))continue;
                    formats.push_back(&entry);
                    choice->Append(wxString::FromUTF8(entry.desc)+" (."+wxString::FromUTF8(entry.ext)+")");
                }
                if(formats.empty())throw RuntimeError("No implemented destination can preserve this source parameter table");
                choice->SetSelection(0);
                const auto preferred=wxGetApp().app_config->sla_export_format(m_project_settings->printer.name);
                for(size_t i=0;i<formats.size();++i)if(preferred==formats[i]->id)choice->SetSelection(int(i));
                layout->Add(choice,0,wxEXPAND|wxALL,10);
                auto *actions=conversion.CreateButtonSizer(wxOK|wxCANCEL);
                auto *confirm=dynamic_cast<wxButton *>(conversion.FindWindow(wxID_OK));
                if(confirm)confirm->SetLabel("Convert");
                layout->Add(actions,0,wxEXPAND|wxALL,10);conversion.SetSizer(layout);
                if(conversion.ShowModal()!=wxID_OK)return;
                const auto *entry=formats.at(size_t(choice->GetSelection()));
                const auto default_name=sla_archive_filename("converted",entry->ext);
                wxFileDialog save(this,"Save converted layers","",wxString::FromUTF8(default_name),
                    wxString::FromUTF8(entry->desc)+" (*."+wxString::FromUTF8(entry->ext)+")|*."+wxString::FromUTF8(entry->ext),
                    wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
                if(save.ShowModal()!=wxID_OK)return;
                const auto typed=into_u8(save.GetPath());const auto path=sla_archive_filename(typed,entry->ext);
                if(path!=typed && boost::filesystem::exists(path) &&
                    wxMessageBox("Replace the existing file?\n"+wxString::FromUTF8(path),"Convert archive",
                        wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
                TaskLock working(m_working);
                BackgroundRequestOwner<sla::NativeArchiveConversionRequest> request_owner;
                const bool converted=run_background_layers(this,"Convert layers",[&](const sla::LayerProgress &progress) {
                    try {
                        const auto request=std::make_shared<const sla::NativeArchiveConversionRequest>(m_document,configuration,entry->id,path,progress);
                        if (!request_owner.bind(request)) return false;
                        request->write(progress);
                        return true;
                    } catch(const sla::LayerOperationCancelled &) { return false; }
                    catch(const CanceledException &) { return false; }
                }, [&] { request_owner.cancel(); });
                if (!converted) return;
                if(!m_project_settings->printer.name.empty())
                    wxGetApp().app_config->set_sla_export_format(m_project_settings->printer.name,entry->id);
                // Conversion is an output of this source, not a save/apply of
                // its project or native container. Preserve revision/history.
            });
        auto export_images = [this](bool range) {
            finish_stroke(true);
            const auto first = size_t((range ? m_first : m_layer)->GetValue()-1);
            const auto last = size_t((range ? m_last : m_layer)->GetValue()-1);
            if (first > last) throw RuntimeError("First layer must not exceed last layer");
            wxString filters;
            for (const auto &format : sla::layer_image_formats()) {
                if (!filters.empty()) filters += '|';
                filters += wxString::FromUTF8(format.description) + " (*." + format.id + ")|*." + format.id;
            }
            wxFileDialog save(this, first == last ? "Export selected mask" : "Export range to a new mask folder",
                "", "mask.png", filters, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            // Update only after a filter change; repeatedly rewriting the
            // filename while the dropdown is open can dismiss GTK's popup.
            int image_filter = 0;
            wxTimer suffix_timer(&save);
            save.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
                const auto selected = save.GetFilterIndex();
                if (selected == image_filter || selected < 0 || size_t(selected) >= sla::layer_image_formats().size()) return;
                image_filter = selected;
                auto filename = boost::filesystem::path(into_u8(save.GetFilename()));
                filename.replace_extension(std::string(".")+sla::layer_image_formats()[size_t(selected)].id);
                save.SetFilename(wxString::FromUTF8(filename.string()));
            });
            suffix_timer.Start(100);
            const auto result = save.ShowModal();
            suffix_timer.Stop();
            if (result != wxID_OK) return;
            const auto &format = sla::layer_image_formats().at(size_t(save.GetFilterIndex()));
            auto output = boost::filesystem::path(into_u8(save.GetPath()));
            output.replace_extension(std::string(".")+format.id);
            if (format.lossy && wxMessageBox(format.binary ?
                "PBM keeps zero black and converts every nonzero value to white, like UVtools. Continue?" :
                "JPEG is lossy and may change exposure pixels. Continue?",
                "Export masks", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
            if (first != last) {
                output = output.parent_path() / (output.stem().string()+"_layers");
                if (boost::filesystem::exists(output)) throw RuntimeError("Choose a new folder name; the mask export folder already exists");
                if (wxMessageBox(wxString::Format("Export layers %zu–%zu as separate images into a new folder?\n", first+1,last+1) +
                    wxString::FromUTF8(output.string()), "Export mask range",
                    wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
            } else if (wxString::FromUTF8(output.string()) != save.GetPath() && boost::filesystem::exists(output) &&
                wxMessageBox("Replace the existing mask?\n"+wxString::FromUTF8(output.string()),
                    "Export masks", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
            const auto plane=selected_plane();
            TaskLock working(m_working);
            run_background_layers(this, "Export masks", [&](const auto &progress) {
                return m_document.export_images(output.string(), format.id, first,last,progress,plane);
            });
        };
        button("Export current mask", [export_images] { export_images(false); });
        button("Export mask range", [export_images] { export_images(true); });
        button("Back to 3D", [this] { if (m_close) m_close(); });
        root->Add(buttons, 0, wxEXPAND);
        SetSizer(root);
        const wxAcceleratorEntry shortcuts[] = {
            wxAcceleratorEntry(wxACCEL_CTRL, 'Z', wxID_UNDO),
            wxAcceleratorEntry(wxACCEL_CTRL, 'Y', wxID_REDO)
        };
        SetAcceleratorTable(wxAcceleratorTable(2, shortcuts));
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { undo_change(); }, wxID_UNDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { redo_change(); }, wxID_REDO);
        if (m_plane) m_plane->Bind(wxEVT_CHOICE,[this](wxCommandEvent &){ perform([]{}); });
        m_value->Bind(wxEVT_SPINCTRL,[this](wxSpinEvent &){update_brush_value_hint();});
        m_value->Bind(wxEVT_TEXT,[this](wxCommandEvent &){update_brush_value_hint();});
        m_brush->Bind(wxEVT_CHOICE,[this](wxCommandEvent &){update_brush_value_hint();});
        Bind(wxEVT_SIZE,[this](wxSizeEvent &event){
            update_brush_value_hint(); update_mask_plane_info(); event.Skip();
        });
        m_layer->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { perform([] {}); });
        m_zoom->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
            if (m_working) return;
            m_preview_cancelled = false;
            try { refresh(); }
            catch (const std::exception &error) {
                m_zoom->SetSelection(0); refresh();
                wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_INFORMATION, this);
            }
        });
        m_preview->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
            // A cancelled/unavailable canvas has no visible mask to paint.
            // Retry explicitly before accepting any brush coordinates.
            if (m_working || !m_preview_bitmap.IsOk()) return;
            perform([&] {
                const int requested=m_value->GetValue();
                const bool erasing=m_brush->GetSelection()==1;
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
                m_stroke_points = {{m_last_pixel.x, m_last_pixel.y}};
                sla::LayerDocument::draw_line(*m_stroke, m_last_pixel.x, m_last_pixel.y,
                    m_last_pixel.x, m_last_pixel.y, m_stroke_radius, m_stroke_value);
                m_layer->Disable();
                m_first->Disable(); m_last->Disable(); m_paint_range->Disable(); m_brush->Disable();
                m_range_slider->Disable(); m_single_layer->Disable();
                m_radius->Disable(); m_value->Disable();
                if (m_plane) m_plane->Disable();
                // Keep stroke keys on the canvas instead of the last toolbar
                // control. wx may suppress CHAR_HOOK while the mouse is captured.
                m_preview->SetFocus();
                m_preview->CaptureMouse();
            });
        });
        m_preview->Bind(wxEVT_MOTION, [this](wxMouseEvent &event) {
            if (!m_stroke || !event.LeftIsDown()) return;
            perform([&] {
                const auto pixel = pixel_at(event);
                sla::LayerDocument::draw_line(*m_stroke, m_last_pixel.x, m_last_pixel.y,
                    pixel.x, pixel.y, m_stroke_radius, m_stroke_value);
                m_stroke_points.push_back({pixel.x, pixel.y});
                m_last_pixel = pixel;
            });
        });
        m_preview->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
            // A release without a stroke must not restart a cancelled preview.
            if (m_stroke) perform([&] { finish_stroke(true); });
        });
        m_preview->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
            perform([&] { finish_stroke(false); });
        });
        m_preview->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &event) {
            if (handle_history_shortcut(event)) return;
            if (event.GetKeyCode() == WXK_ESCAPE && m_stroke) {
                perform([this] { finish_stroke(false); });
                return;
            }
            event.Skip();
        });
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) {
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
        });
        refresh();
        m_source_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { update_source_status(); }, m_source_timer.GetId());
        m_source_timer.Start(250);
    }
    bool save_project(bool save_as) override {
        bool saved = false;
        perform([&] { saved = save_layer_project_impl(save_as); });
        return saved;
    }
    void export_document() override { perform(m_export_action); }
    bool has_changes() const override {
        return m_working || unsaved_changes() ||
            (m_print && m_print->is_raster_job() && !m_generated_exported && !m_project_revision);
    }
    bool is_model_result() const override { return m_print && !m_print->is_raster_job(); }
    bool is_working() const override { return m_working; }
    bool source_current() const override {
        return is_model_result() && m_session && m_session->source_current() && (!m_source_available || m_source_available());
    }
    bool can_undo() const override { return !m_working && m_document.can_undo(); }
    bool can_redo() const override { return !m_working && m_document.can_redo(); }
    void undo_change() override { perform([this] { finish_stroke(false); m_document.undo(); }); }
    void redo_change() override { perform([this] { finish_stroke(false); m_document.redo(); }); }
    void apply_changes() override {
        if (m_working) throw RuntimeError("Wait for the layer operation to finish");
        finish_stroke(true);
        if (!m_document.dirty()) return;
        if (!source_current())
            throw RuntimeError("The sliced result changed. Load its current layers before applying edits.");
        if (!m_apply_source) throw RuntimeError("The layer editor has no model Apply target");
        m_apply_source(*m_session);
        if (auto *plater = wxGetApp().plater()) plater->on_sla_layer_document_changed();
        refresh();
    }
    bool matches_project_snapshot(const sla::ExportGenerationGuard &document_source,
                                  uint64_t revision) const override {
        // Use document identity plus its content revision, not current() on
        // the model source: metadata invalidation must not lose applied PNGs.
        // Undo/Redo can return to the exact applied revision; newer edits and
        // replacement sessions cannot reuse that receipt.
        return is_model_result() && !m_working && !m_stroke && !m_document.dirty() &&
            !m_settings_require_project && !context_changed() &&
            m_project_settings == m_settings_baseline && m_document.revision() == revision &&
            m_document.export_input_guard().same_source(document_source);
    }
    void acknowledge_model_project_saved(uint64_t revision,
        const sla::ExportGenerationGuard *document_source) override {
        const bool matching_snapshot = document_source && matches_project_snapshot(*document_source, revision);
        if (!is_model_result() || (!source_current() && !matching_snapshot) || m_working || m_stroke ||
            m_document.revision() != revision) return;
        m_model_project_revision = revision;
        update_source_status();
    }
    bool confirm_discard() override {
        if (m_working) return false;
        if (!unsaved_changes() && !(m_print && m_print->is_raster_job() && !m_generated_exported && !m_project_revision)) return true;
        if (m_session && !m_print->is_raster_job() && !source_current())
            return wxMessageBox("Discard the older sliced edits? They are separate from the current 3D project. Exported files will be kept.",
                "Layer editor", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
        return wxMessageBox(m_project_revision ? "Close without saving changes to the layer project? Exported files will be kept." :
            m_print && m_print->is_raster_job() ? "Discard generated layers that have not been saved or exported?" :
            m_print ? "Close without saving these layer edits in the 3D project? Exported files will be kept." :
            "Discard unsaved layer edits or export context?", "Layer editor", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
    }
};

}
LayerEditorPanel *create_sliced_layer_editor(wxWindow *parent, SLAPrint &source,
    std::function<void(sla::SlicedResultSession &)> apply,
    std::function<bool()> source_available, std::function<void()> back)
{
    std::unique_ptr<sla::SlicedResultSession> session;
    const bool captured = run_background_layers(parent, "Open sliced layers", [&](const sla::LayerProgress &progress) {
        try { session = sla::SlicedResultSession::capture(source, progress); return true; }
        catch (const sla::LayerOperationCancelled &) { return false; }
        catch (const CanceledException &) { return false; }
    });
    if (!captured) return nullptr;
    return new LayerEditorContent(parent, {}, {}, {}, {}, std::move(session),
        std::move(back), std::move(apply), std::move(source_available));
}
std::string choose_layer_file(wxWindow *parent)
{
    wxString archive_filter = "Layer archives, projects and images|*.sla-project;*.SLA-PROJECT;*.sl1;*.sl1s;*.uvj;*.cws;*.zip;*.cxdlp;*.cxdlpv4;*.ctb;*.cbddlp;*.photon;*.CTB;*.CBDDLP;*.PHOTON;*.CXDLP;*.CXDLPV4;*.png;*.PNG;*.jpg;*.jpeg;*.jp2;*.tif;*.tiff;*.bmp;*.pbm;*.pgm;*.sr;*.ras;*.JPG;*.JPEG;*.JP2;*.TIF;*.TIFF;*.BMP;*.PBM;*.PGM;*.SR;*.RAS|All files|*";
    wxString anycubic_masks;
    for (const auto &entry : registered_sla_archives()) if (!anycubic_available_versions(entry.id).empty()) {
        const auto extension = wxString::FromUTF8(entry.ext);
        anycubic_masks += ";*." + extension + ";*." + extension.Upper();
    }
    archive_filter.Replace("|All files|*", anycubic_masks + "|All files|*");
    wxFileDialog open(parent, "Open layer archive", "", "", archive_filter,
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (open.ShowModal() != wxID_OK) return {};
    return into_u8(open.GetPath());
}
namespace {
struct OpenedLayerFile {
    sla::LayerDocument document;
    std::optional<sla::LayerExportProfile> profile;
    std::optional<sla::ProjectSLASettings> settings;
    std::string project_path;
    std::unique_ptr<sla::SlicedResultSession> session;
};
// The temporary generated job is destroyed after capture; only its owned
// context and masks travel to the workspace. Imported documents own their reader.
std::optional<OpenedLayerFile> load_layer_file(wxWindow *parent, const std::string &path)
{
    OpenedLayerFile loaded;
    const bool is_project = boost::algorithm::to_lower_copy(
        boost::filesystem::path(path).extension().string()) == ".sla-project";
    const bool opened = run_background_layers(parent, "Open layer archive", [&](const sla::LayerProgress &progress) {
        try {
            if (is_project) {
                auto project = sla::read_project_layers(path, progress);
                if (!project || project->origin == sla::ProjectLayerOrigin::ModelSlice)
                    throw RuntimeError("Open model projects in the 3D workspace.");
                if (project->origin == sla::ProjectLayerOrigin::GeneratedRaster) {
                    auto job = sla::restore_generated_project(*project);
                    loaded.session = sla::SlicedResultSession::capture(*job, progress);
                } else loaded.document = std::move(project->document);
                loaded.profile = project->export_profile;
                loaded.settings = project->settings;
                loaded.project_path = path;
            } else loaded.document = sla::LayerDocument::load_archive(path, progress);
            return true;
        } catch (const sla::LayerOperationCancelled &) { return false; }
          catch (const CanceledException &) { return false; }
    });
    if (!opened) return std::nullopt;
    return loaded;
}
}
LayerEditorPanel *create_layer_file_editor(wxWindow *parent, const std::string &path,
    std::function<void()> back)
{
    auto loaded = load_layer_file(parent, path);
    if (!loaded) return nullptr;
    return new LayerEditorContent(parent, std::move(loaded->document),
        loaded->profile, std::move(loaded->project_path), loaded->settings,
        std::move(loaded->session), std::move(back));
}
}
