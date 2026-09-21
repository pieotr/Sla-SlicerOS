#include "LayerEditorDialog.hpp"
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
#include <algorithm>
#include <functional>

namespace Slic3r::GUI {
namespace {
class LayerEditorDialog final : public wxDialog {
    sla::LayerDocument m_document;
    wxStaticBitmap *m_preview;
    wxSpinCtrl *m_layer, *m_radius, *m_value;
    wxButton *m_undo, *m_redo;
    size_t m_cols = 0, m_rows = 0;
    void perform(const std::function<void()> &action) {
        try { action(); refresh(); }
        catch (const std::exception &error) {
            wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_ERROR, this);
        }
    }
    void refresh() {
        const auto image = m_document.image(size_t(m_layer->GetValue()-1));
        m_cols = image.cols; m_rows = image.rows;
        const double scale = std::min({1., 760. / m_cols, 560. / m_rows});
        const int width = std::max(1, int(m_cols*scale));
        const int height = std::max(1, int(m_rows*scale));
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
        m_undo->Enable(m_document.can_undo()); m_redo->Enable(m_document.can_redo());
        SetTitle(m_document.dirty() ? "SL1 layer editor *" : "SL1 layer editor");
        Layout(); Fit();
    }
public:
    LayerEditorDialog(wxWindow *parent, sla::LayerDocument document)
        : wxDialog(parent, wxID_ANY, "SL1 layer editor"), m_document(std::move(document))
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *controls = new wxBoxSizer(wxHORIZONTAL);
        auto spin = [&](const char *label, int low, int high, int value) {
            controls->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
            auto *ctrl = new wxSpinCtrl(this, wxID_ANY);
            ctrl->SetRange(low, high); ctrl->SetValue(value); controls->Add(ctrl, 0, wxALL, 5); return ctrl;
        };
        m_layer = spin("Layer", 1, int(m_document.layer_count()), 1);
        m_radius = spin("Radius (pixels)", 0, 4096, 3);
        m_value = spin("Grayscale / threshold", 0, 255, 255);
        root->Add(controls, 0, wxEXPAND);
        root->Add(new wxStaticText(this, wxID_ANY,
            "Click to paint a circular stamp. Value 0 erases; 255 exposes. Coordinates follow the stored PNG."), 0, wxALL, 8);
        m_preview = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
        root->Add(m_preview, 0, wxALIGN_CENTER | wxALL, 8);
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto button = [&](const char *text, std::function<void()> action) {
            auto *b = new wxButton(this, wxID_ANY, text); buttons->Add(b, 0, wxALL, 5);
            b->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent &) { perform(action); }); return b;
        };
        m_undo = button("Undo", [this] { m_document.undo(); });
        m_redo = button("Redo", [this] { m_document.redo(); });
        button("Threshold layer", [this] { m_document.threshold(size_t(m_layer->GetValue()-1), uint8_t(m_value->GetValue())); });
        button("Save as SL1", [this] {
            wxFileDialog save(this, "Save edited layers", "", "edited.sl1", "SL1 archive (*.sl1)|*.sl1",
                              wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (save.ShowModal() == wxID_OK) m_document.save_sl1(save.GetPath().ToUTF8().data());
        });
        button("Close", [this] { Close(); });
        root->Add(buttons, 0, wxALIGN_CENTER);
        SetSizer(root);
        m_layer->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { perform([] {}); });
        m_preview->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
            const auto size = m_preview->GetClientSize();
            if (size.x <= 0 || size.y <= 0) return;
            const int x = int(int64_t(event.GetX()) * m_cols / size.x);
            const int y = int(int64_t(event.GetY()) * m_rows / size.y);
            perform([&] { m_document.paint(size_t(m_layer->GetValue()-1), x, y,
                                           unsigned(m_radius->GetValue()), uint8_t(m_value->GetValue())); });
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
            if (m_document.dirty() && event.CanVeto() &&
                wxMessageBox("Discard unsaved layer edits?", "Layer editor", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
                event.Veto(); return;
            }
            EndModal(wxID_CLOSE);
        });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); }, wxID_CANCEL);
        refresh(); CentreOnParent();
    }
};
}
void show_layer_editor(wxWindow *parent)
{
    wxFileDialog open(parent, "Open SL1 layers", "", "", "SL1 archives (*.sl1;*.sl1s)|*.sl1;*.sl1s",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (open.ShowModal() != wxID_OK) return;
    try {
        const std::string path = open.GetPath().ToUTF8().data();
        LayerEditorDialog dialog(parent, sla::LayerDocument::load_sl1(path));
        dialog.ShowModal();
    } catch (const std::exception &error) {
        wxMessageBox(wxString::FromUTF8(error.what()), "Layer editor", wxOK | wxICON_ERROR, parent);
    }
}
}
