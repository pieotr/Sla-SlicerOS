#ifndef SLIC3R_GUI_LAYER_EDITOR_DIALOG_HPP
#define SLIC3R_GUI_LAYER_EDITOR_DIALOG_HPP
#include <wx/scrolwin.h>
#include <functional>
#include <cstdint>
#include <string>
namespace Slic3r { class SLAPrint; }
namespace Slic3r::sla { class SlicedResultSession; class ExportGenerationGuard; }
namespace Slic3r::GUI {
// Persistent editing surface for model results and standalone layer documents.
class LayerEditorPanel : public wxScrolledWindow {
public:
    using wxScrolledWindow::wxScrolledWindow;
    virtual bool has_changes() const = 0;
    virtual bool is_working() const = 0;
    virtual bool source_current() const = 0;
    virtual bool is_model_result() const = 0;
    virtual bool confirm_discard() = 0;
    virtual bool save_project(bool save_as) = 0;
    virtual void export_document() = 0;
    virtual void apply_changes() = 0;
    // A successful matching 3MF publication acknowledges this revision only.
    // Apply and native export do not constitute project persistence.
    // An applied snapshot may remain compatible after a metadata change has
    // retired the model source. Identity and revision must both match this
    // editor; a coincident revision from another session is not sufficient.
    virtual bool matches_project_snapshot(const sla::ExportGenerationGuard &document_source,
                                          uint64_t revision) const = 0;
    virtual void acknowledge_model_project_saved(uint64_t revision,
        const sla::ExportGenerationGuard *document_source = nullptr) = 0;
    virtual bool can_undo() const = 0;
    virtual bool can_redo() const = 0;
    virtual void undo_change() = 0;
    virtual void redo_change() = 0;
};
LayerEditorPanel *create_sliced_layer_editor(wxWindow *parent, SLAPrint &source,
    std::function<void(sla::SlicedResultSession &)> apply,
    std::function<bool()> source_available, std::function<void()> back);
// File selection and loading are separate so Cancel leaves the active panel intact.
std::string choose_layer_file(wxWindow *parent);
LayerEditorPanel *create_layer_file_editor(wxWindow *parent, const std::string &path,
    std::function<void()> back);
}
#endif
