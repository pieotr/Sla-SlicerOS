#include "LayerEditorDialog.hpp"
#include "BackgroundTaskDialog.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/SL1.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include "libslic3r/SLAPrint.hpp"
#include <boost/filesystem.hpp>
#include <wx/utils.h>
#include "libslic3r/SLA/LayerDocument.hpp"
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/spinctrl.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/image.h>
#include <wx/choice.h>
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
#include <chrono>

namespace Slic3r::GUI {
namespace {
struct TaskLock {
    bool &flag;
    explicit TaskLock(bool &value) : flag(value) { flag = true; }
    ~TaskLock() { flag = false; }
};
class LayerEditorDialog final : public wxDialog {
    sla::LayerDocument m_document;
    SLAPrint *m_print = nullptr;
    bool m_generated_exported = false;
    bool m_working = false;
    wxChoice *m_format = nullptr;
    std::vector<const ArchiveEntry *> m_formats;
    std::optional<png::ImageGreyscale> m_stroke;
    wxPoint m_last_pixel;
    wxStaticBitmap *m_preview;
    wxSpinCtrl *m_layer, *m_radius, *m_value;
    wxSpinCtrl *m_first, *m_last;
    wxChoice *m_zoom, *m_operation;
    wxScrolledWindow *m_viewport;
    wxButton *m_undo, *m_redo;
    size_t m_cols = 0, m_rows = 0;
    void perform(const std::function<void()> &action) {
        if (m_working) return;
        try { action(); refresh(); }
        catch (const std::exception &error) {
            wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_ERROR, this);
        }
    }
    void refresh() {
        const auto loaded = m_stroke ? png::ImageGreyscale{} : m_document.image(size_t(m_layer->GetValue()-1));
        const auto &image = m_stroke ? *m_stroke : loaded;
        m_cols = image.cols; m_rows = image.rows;
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
        m_preview->SetBitmap(wxBitmap(preview));
        m_preview->SetMinSize(wxSize(width, height));
        m_preview->SetSize(width, height);
        m_viewport->FitInside();
        m_undo->Enable(m_document.can_undo()); m_redo->Enable(m_document.can_redo());
        SetTitle(m_document.dirty() || m_stroke ? "Layer editor *" : "Layer editor");
        Layout();
    }
    wxPoint pixel_at(const wxMouseEvent &event) const {
        const auto size = m_preview->GetClientSize();
        return {std::clamp(int(int64_t(event.GetX()) * m_cols / std::max(1, size.x)), 0, int(m_cols)-1),
                std::clamp(int(int64_t(event.GetY()) * m_rows / std::max(1, size.y)), 0, int(m_rows)-1)};
    }
    void finish_stroke(bool commit) {
        if (!m_stroke) return;
        if (commit) m_document.replace_image(size_t(m_layer->GetValue()-1), *m_stroke);
        m_stroke.reset();
        if (m_preview->HasCapture()) m_preview->ReleaseMouse();
        m_layer->Enable();
        refresh();
    }
public:
    LayerEditorDialog(wxWindow *parent, sla::LayerDocument document, SLAPrint *print = nullptr)
        : wxDialog(parent, wxID_ANY, "Layer editor", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_document(std::move(document)), m_print(print)
    {
        if (!m_document.layer_count()) throw RuntimeError("The layer document is empty");
        m_document.mark_saved();
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *controls = new wxWrapSizer(wxHORIZONTAL);
        auto spin = [&](const char *label, int low, int high, int value) {
            controls->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
            auto *ctrl = new wxSpinCtrl(this, wxID_ANY);
            ctrl->SetRange(low, high); ctrl->SetValue(value); controls->Add(ctrl, 0, wxALL, 5); return ctrl;
        };
        m_layer = spin("Layer", 1, int(m_document.layer_count()), 1);
        m_radius = spin("Radius (pixels)", 0, 4096, 3);
        m_value = spin("Grayscale / threshold", 0, 255, 255);
        controls->Add(new wxStaticText(this, wxID_ANY, "Zoom"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
        m_zoom = new wxChoice(this, wxID_ANY);
        for (const char *label : {"Fit", "25%", "50%", "100%", "200%", "400%"}) m_zoom->Append(label);
        m_zoom->SetSelection(0); m_zoom->SetName("Preview zoom");
        controls->Add(m_zoom, 0, wxALL, 5);
        root->Add(controls, 0, wxEXPAND);
        root->Add(new wxStaticText(this, wxID_ANY,
            "Drag to paint. Value 0 erases; 255 exposes. One stroke is one undo step."), 0, wxALL, 8);
        m_viewport = new wxScrolledWindow(this);
        m_viewport->SetScrollRate(16, 16);
        auto *preview_sizer = new wxBoxSizer(wxVERTICAL);
        m_preview = new wxStaticBitmap(m_viewport, wxID_ANY, wxNullBitmap);
        preview_sizer->Add(m_preview, 0, wxALL, 4);
        m_viewport->SetSizer(preview_sizer);
        root->Add(m_viewport, 1, wxEXPAND | wxALL, 4);
        auto *range_controls = new wxWrapSizer(wxHORIZONTAL);
        range_controls->Add(new wxStaticText(this, wxID_ANY, "Range: first / last layer"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
        m_first = new wxSpinCtrl(this, wxID_ANY); m_last = new wxSpinCtrl(this, wxID_ANY);
        m_first->SetRange(1, int(m_document.layer_count())); m_last->SetRange(1, int(m_document.layer_count()));
        m_first->SetValue(1); m_last->SetValue(int(m_document.layer_count()));
        m_first->SetName("First layer"); m_last->SetName("Last layer");
        range_controls->Add(m_first, 0, wxALL, 5); range_controls->Add(m_last, 0, wxALL, 5);
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
                    }, radius);
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
        m_undo = button("Undo", [this] { m_document.undo(); });
        m_redo = button("Redo", [this] { m_document.redo(); });
        button("Threshold layer", [this] { m_document.threshold(size_t(m_layer->GetValue()-1), uint8_t(m_value->GetValue())); });
        button("Invert layer", [this] { m_document.invert(size_t(m_layer->GetValue()-1)); });
        if (m_print) {
            m_format = new wxChoice(this, wxID_ANY);
            auto preferred = wxGetApp().app_config->sla_export_format(wxGetApp().preset_bundle->printers.get_selected_preset_name());
            if (preferred.empty() || preferred == "SL1SVG" || !archive_export_unavailable_reason(preferred.c_str()).empty())
                preferred = m_print->printer_config().sla_archive_format.value;
            if (m_print->is_raster_job() && (preferred == "SL1SVG" || !archive_export_unavailable_reason(preferred.c_str()).empty()))
                preferred = "uvj";
            for (const auto &entry : registered_sla_archives()) {
                if (!archive_export_unavailable_reason(entry.id).empty() || !std::strcmp(entry.id, "SL1SVG")) continue;
                m_formats.push_back(&entry);
                m_format->Append(wxString::FromUTF8(entry.desc) + " (." + wxString::FromUTF8(entry.ext) + ")");
                if (entry.id == preferred)
                    m_format->SetSelection(int(m_formats.size()-1));
            }
            if (m_format->GetSelection() == wxNOT_FOUND && !m_formats.empty()) m_format->SetSelection(0);
            root->Add(m_format, 0, wxEXPAND | wxALL, 5);
            auto *format_note = new wxStaticText(this, wxID_ANY, "");
            root->Add(format_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
            auto update_note = [this, format_note] {
                const int selection = m_format->GetSelection();
                const char *id = selection == wxNOT_FOUND ? "" : m_formats.at(size_t(selection))->id;
                const bool images_only = std::strcmp(id, "genericzip") == 0 || std::strcmp(id, "vda") == 0;
                const auto *legacy = anycubic_legacy_geometry(id);
                wxString note;
                if (images_only) note = "This ZIP format saves masks and geometry. Set exposure and motion on the receiving printer.";
                else if(std::strcmp(id,"cwsrgb")==0) note = "RGB.CWS packs three horizontal mask pixels into one PNG pixel. The LCD raster width after orientation must be divisible by three. Full LCD dimensions and all grayscale values are preserved.";
                else if((std::strcmp(id,"cxdlp")==0 || std::strcmp(id,"cxdlpv2")==0)) note = "CXDLP v2/v3 requires a CL/CT printer model, fixed layer height, integer speeds in mm/s, normal exposure in 0.1 s, integer bottom exposure and a shared wait of at least 1 s. Machine Z is not stored.";
                else if(std::strcmp(id,"cxdlpv4")==0) note = "CXDLPv4 stores 7-bit grayscale: values 0 and 1 are off; other values are quantized. Volume and contour metadata describe the encoded masks.";
                else if(std::strcmp(id,"cxdlpv1")==0) note = "CXDLPv1 requires fixed layer height, integer exposure/motion values and a shared wait of at least 1 second. This format does not store machine Z.";
                else if (legacy) note = wxString::Format(
                    "This legacy format requires an oriented screen of %.4g x %.4g mm and Z %.4g mm. Custom dimensions require a format with a machine table.",
                    legacy->width, legacy->height, legacy->machine_z);
                format_note->SetLabel(note);
                format_note->Wrap(650); format_note->Show(!note.empty()); Layout();
            };
            m_format->Bind(wxEVT_CHOICE, [update_note](wxCommandEvent &) { update_note(); });
            update_note();
            if (!m_print->is_raster_job()) button("Apply to sliced result", [this] {
                const bool changed = m_document.dirty();
                m_print->replace_layer_document(m_document);
                m_document.mark_saved();
                if (changed) if (auto *plater = wxGetApp().plater()) plater->on_sla_layer_document_changed();
            });
        } else if(m_document.archive_format()=="cxdlpv4") {
            auto *note=new wxStaticText(this,wxID_ANY,"CXDLPv4 edits use 7-bit grayscale. Values 0 and 1 are off; other values are quantized immediately so Undo and the preview match the saved file. Original per-layer settings and previews are preserved.");
            note->Wrap(650);root->Add(note,0,wxEXPAND|wxALL,5);
        } else if (m_document.archive_format() == "genericzip" || m_document.archive_format() == "vda") {
            auto *note = new wxStaticText(this, wxID_ANY, "This archive contains masks and geometry. Exposure and motion are configured on the receiving printer.");
            note->Wrap(650); root->Add(note, 0, wxEXPAND | wxALL, 5);
        }
        button(m_print ? (m_print->is_raster_job() ? "Export generated layers" : "Export edited layers") : (m_document.archive_format() == "png" ? "Save edited PNG" : "Save edited archive"), [this] {
            const bool png = !m_print && m_document.archive_format() == "png";
            const auto *entry = png ? nullptr : m_print ? m_formats.at(size_t(m_format->GetSelection())) : get_archive_entry(m_document.archive_format().c_str());
            if (!png && !entry) throw RuntimeError("Unknown layer container");
            auto filename = [&](const std::string &path) {
                return png ? boost::filesystem::path(path).replace_extension(".png").string() : sla_archive_filename(path, entry->ext);
            };
            const char *project = m_print && m_print->is_raster_job() ? "generated" : "edited";
            const auto name = filename(project);
            const auto filter = png ? wxString("PNG mask (*.png)|*.png") : wxString::FromUTF8(entry->desc) + " (*." + entry->ext + ")|*." + entry->ext;
            wxFileDialog save(this, "Save edited layers", "", wxString::FromUTF8(name), filter,
                              wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (save.ShowModal() == wxID_OK) {
                const auto path = filename(save.GetPath().ToUTF8().data());
                // The native dialog checks the typed path. A corrected suffix
                // can target a different existing file; confirm that one too.
                if (wxString::FromUTF8(path) != save.GetPath() && boost::filesystem::exists(path) &&
                    wxMessageBox("Replace the existing file?\n" + wxString::FromUTF8(path),
                        "Save edited layers", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
                    return;
                TaskLock working(m_working);
                if (m_print) {
                    run_background_write(this, "Export layers", [&] {
                        m_print->export_layer_document(m_document, entry->id, path, {}, project);
                    });
                    if (m_print->is_raster_job()) { m_generated_exported = true; m_document.mark_saved(); }
                    wxGetApp().app_config->set_sla_export_format(wxGetApp().preset_bundle->printers.get_selected_preset_name(), entry->id);
                } else run_background_write(this, "Save layer archive", [&] { m_document.save_archive(path); });
            }
        });
        auto *close_button = button("Close", [this] { Close(); });
        SetEscapeId(close_button->GetId());
        root->Add(buttons, 0, wxEXPAND);
        SetSizer(root);
        const wxAcceleratorEntry shortcuts[] = {
            wxAcceleratorEntry(wxACCEL_CTRL, 'Z', wxID_UNDO),
            wxAcceleratorEntry(wxACCEL_CTRL, 'Y', wxID_REDO)
        };
        SetAcceleratorTable(wxAcceleratorTable(2, shortcuts));
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { perform([&] { m_document.undo(); }); }, wxID_UNDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { perform([&] { m_document.redo(); }); }, wxID_REDO);
        m_layer->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { perform([] {}); });
        m_zoom->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
            try { refresh(); }
            catch (const std::exception &error) {
                m_zoom->SetSelection(0); refresh();
                wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_INFORMATION, this);
            }
        });
        m_preview->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
            perform([&] {
                m_stroke = m_document.image(size_t(m_layer->GetValue()-1));
                m_last_pixel = pixel_at(event);
                sla::LayerDocument::draw_line(*m_stroke, m_last_pixel.x, m_last_pixel.y,
                    m_last_pixel.x, m_last_pixel.y, unsigned(m_radius->GetValue()), uint8_t(m_value->GetValue()));
                m_layer->Disable();
                m_preview->CaptureMouse();
            });
        });
        m_preview->Bind(wxEVT_MOTION, [this](wxMouseEvent &event) {
            if (!m_stroke || !event.LeftIsDown()) return;
            perform([&] {
                const auto pixel = pixel_at(event);
                sla::LayerDocument::draw_line(*m_stroke, m_last_pixel.x, m_last_pixel.y,
                    pixel.x, pixel.y, unsigned(m_radius->GetValue()), uint8_t(m_value->GetValue()));
                m_last_pixel = pixel;
            });
        });
        m_preview->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { perform([&] { finish_stroke(true); }); });
        m_preview->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
            perform([&] { finish_stroke(false); });
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
            if (m_working) { if (event.CanVeto()) event.Veto(); return; }
            if ((m_document.dirty() || m_stroke || (m_print && m_print->is_raster_job() && !m_generated_exported)) && event.CanVeto() &&
                wxMessageBox(m_print && m_print->is_raster_job() ? "Discard generated layers that have not been exported?" :
                    m_print ? "Close without applying these edits to the sliced result? Exported files will be kept." :
                    "Discard unsaved layer edits?", "Layer editor", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
                event.Veto(); return;
            }
            EndModal(wxID_CLOSE);
        });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); }, wxID_CANCEL);
        refresh(); SetSize(840, 760); SetMinSize(wxSize(480, 400)); CentreOnParent();
    }
};
}
void show_layer_editor(wxWindow *parent, SLAPrint &print)
{
    try {
        if (!print.finished())
            throw RuntimeError("Slice the model before opening the layer editor");
        if (!print.layer_document())
            throw RuntimeError("This result contains vector layers. Choose a raster format and slice again to edit pixels.");
        LayerEditorDialog dialog(parent, *print.layer_document(), &print);
        dialog.ShowModal();
    } catch (const std::exception &error) {
        wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_ERROR, parent);
    }
}
void show_layer_editor(wxWindow *parent)
{
    wxFileDialog open(parent, "Open layer archive", "", "", "Layer archives and PNG|*.sl1;*.sl1s;*.uvj;*.cws;*.zip;*.cxdlp;*.cxdlpv4;*.CXDLP;*.CXDLPV4;*.png;*.PNG|All files|*",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (open.ShowModal() != wxID_OK) return;
    try {
        const std::string path = open.GetPath().ToUTF8().data();
        std::optional<sla::LayerDocument> document;
        run_background_write(parent, "Open layer archive", [&] {
            document.emplace(sla::LayerDocument::load_archive(path));
        }, "Opening layers…");
        LayerEditorDialog dialog(parent, std::move(*document));
        dialog.ShowModal();
    } catch (const std::exception &error) {
        wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_ERROR, parent);
    }
}
}
