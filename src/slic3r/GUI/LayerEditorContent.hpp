#ifndef SLIC3R_GUI_LAYER_EDITOR_CONTENT_HPP
#define SLIC3R_GUI_LAYER_EDITOR_CONTENT_HPP
// Private class of the layer editor (Sliced workspace panel); members are defined in LayerEditorContent*.cpp.
#include "LayerEditorInternal.hpp"
#include "Widgets/ComboBox.hpp"   // owner-drawn drop-down (DropDown popup): see make_choice() in LayerEditorContent.cpp

namespace Slic3r::GUI {
namespace layer_editor_detail {
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
    void update_source_status();
    bool m_generated_exported = false;
    bool m_working = false;
    ComboBox *m_format = nullptr;
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
    ComboBox *m_brush = nullptr;
    // Pixel-editor state. Tool indices: 0 freehand, 1 line, 2 rectangle, 3 ellipse, 4 flood fill.
    ComboBox *m_tool = nullptr, *m_shape = nullptr;
    wxStaticText *m_pixel_info = nullptr;
    int m_stroke_tool = 0;
    sla::LayerDocument::BrushShape m_stroke_shape = sla::LayerDocument::BrushShape::Round;
    wxPoint m_stroke_start;
    // Shape tools redraw from this untouched copy while the cursor moves.
    std::optional<png::ImageGreyscale> m_stroke_base;
    // Backing image of the canvas, for the pixel inspector. Owned by m_stroke,
    // m_loaded_view or the preview cache; cleared whenever those may change.
    std::optional<png::ImageGreyscale> m_loaded_view;
    const png::ImageGreyscale *m_view_image = nullptr;
    std::optional<sla::LayerRasterGeometry> m_view_geometry;
    // Free zoom from wheel/keys (canvas px per mask px); 0 means the zoom choice decides.
    double m_zoom_scale = 0;
    bool m_panning = false, m_space_down = false;
    wxPoint m_pan_origin_screen, m_pan_start_scroll;
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
    bool context_changed() const;
    bool unsaved_changes() const;
    wxPoint m_last_pixel;
    // Layer operations (goal G6): uncommitted preview shown instead of the real layer, the last
    // rectangle drawn (offered as ROI) and the non-modal Operations dialog.
    std::optional<png::ImageGreyscale> m_op_preview;
    std::optional<layerops::Rect> m_last_rectangle;
    wxDialog *m_ops_dialog = nullptr;
    wxPanel *m_preview;
    wxBitmap m_preview_bitmap;
    wxSpinCtrl *m_layer, *m_radius, *m_value;
    wxSpinCtrl *m_first, *m_last;
    ComboBox *m_zoom = nullptr, *m_operation = nullptr;
    ComboBox *m_plane = nullptr;
    wxStaticText *m_plane_info = nullptr;
    wxString m_plane_hint_text;
    int m_plane_hint_width = -1;
    wxStaticText *m_exposure_info = nullptr;
    sla::LayerMaskPlane selected_plane() const;
    wxScrolledWindow *m_viewport;
    wxButton *m_undo, *m_redo;
    size_t m_cols = 0, m_rows = 0;
    void perform(const std::function<void()> &action);
    bool handle_history_shortcut(const wxKeyEvent &event);
    void update_brush_value_hint();
    void update_paint_scope();
    void update_mask_plane_info();
    void clear_preview(const wxString &message);
    const png::ImageGreyscale *prepared_preview();
    // Undo/Redo or a layer-list operation can change the layer count: keep every layer spinner valid.
    void sync_layer_ranges();
    void refresh();
    wxPoint pixel_at(const wxMouseEvent &event) const;
    sla::LayerDocument::ShapeOp current_shape_op(wxPoint end) const;
    // Freehand stamps go straight into the preview; shape tools redraw from the base copy.
    void update_stroke_preview(wxPoint from, wxPoint to);
    void update_pixel_info(const wxMouseEvent *event);
    double effective_zoom() const;
    // Zoom so the mask point under `viewport_point` (viewport client px) stays put.
    void zoom_to(double new_scale, wxPoint viewport_point, bool fit = false);
    wxPoint viewport_point(const wxMouseEvent &event) const;
    void begin_pan(const wxMouseEvent &event);
    void end_pan();
    void edit_layer_parameters(bool range = false);
    void finish_stroke(bool commit);

    // ---- Layer operations (dialog host) -------------------------------------------------------------
    // Layer height used for documents that store no Z rows (plain rasters); only affects Z-dependent
    // layer-list operations on such documents.
    static constexpr double kFallbackLayerHeightMm = 0.05;
    layerops::Mask read_layer_mask(size_t layer) const;
    std::string operation_list_refusal(bool changes_resolution) const;
    // Top Z of every layer: the stored Z rows when the document has them, otherwise a nominal pitch.
    std::vector<double> operation_layer_z() const;
    // Lithophane that the open document cannot take: save the result as a generated layer project and open it.
    // Opening replaces this editor, so it is deferred to the application event loop.
    RunOutcome open_lithophane_as_new_document(const LayerOpsJob &job);
    // Runs a job in the background (progress + Cancel); the document commits one undo step.
    RunOutcome run_operation_job(const LayerOpsJob &job);
    // Export: decode the whole document in the worker, then hand it to the writer (which stages + renames).
    bool run_export_job(const LayerExportJob &job);
    LayerOpsHost make_operations_host();
    void open_operations_dialog();

    bool save_layer_project_impl(bool save_as);

    // ---- Issues section (detection results, UI state) ----
    sla::IssueDetectionConfig m_issue_config;
    std::vector<sla::LayerIssue> m_issues;
    uint64_t m_issues_revision = 0;       // document revision the results describe
    bool m_issues_ran = false;
    wxString m_issue_text;                // last status text; detects state changes
    size_t m_issue_selected = size_t(-1); // index into m_issues drawn on the canvas
    wxDataViewListCtrl *m_issue_list = nullptr; // GtkTreeView on GTK: rows/cells are exposed through AT-SPI
    wxStaticText *m_issue_status = nullptr;
    wxButton *m_issue_detect = nullptr;
    wxCheckBox *m_issue_on[6] = {};       // island, overhang, resin trap, suction cup, touching bound, empty layer
    wxSpinCtrl *m_issue_island_area = nullptr, *m_issue_overhang_erode = nullptr,
               *m_issue_trap_area = nullptr, *m_issue_margin = nullptr;
    wxSpinCtrlDouble *m_issue_support_percent = nullptr, *m_issue_cup_height = nullptr;

    bool issues_current() const { return m_issues_ran && m_issues_revision == m_document.revision(); }

    // Builds the Issues section. Defaults come from the backend's config struct,
    // which mirrors UVtools' defaults.
    void build_issue_panel(wxBoxSizer *root);

    sla::IssueDetectionConfig read_issue_config() const;

    // Runs detect_layer_issues on a worker (modal progress + Cancel, edits locked
    // by m_working) and publishes the list only for the revision that was scanned.
    void detect_issues();

    void fill_issue_list();

    // Marks results stale (list disabled, overlay hidden) whenever the document
    // revision differs from the scanned one; Undo back to that revision revives them.
    void update_issue_state();

    // Jump to the first layer of the issue; the overlay is drawn by the paint handler.
    void select_issue(size_t index);

    // Cheap overlay: bbox outline plus translucent pixel runs of the selected
    // issue on the displayed layer (runs are skipped beyond a cap).
    void paint_issue_overlay(wxAutoBufferedPaintDC &dc);

    // ---- UI construction (LayerEditorContent.cpp); called in this order by the constructor ----
    wxStaticText *m_format_note = nullptr;     // explanation of the selected export format
    wxStaticText *m_profile_summary = nullptr; // resin-estimate summary of native archives
    void build_paint_controls(wxBoxSizer *root);
    void build_preview_canvas();
    void build_range_controls(wxBoxSizer *root);
    void build_range_filters(wxBoxSizer *root);
    void build_action_buttons(wxBoxSizer *root);
    wxButton *add_action_button(wxSizer *buttons, const char *text, std::function<void()> action);
    void build_edit_buttons(wxSizer *buttons);
    void build_format_section(wxBoxSizer *root, wxSizer *buttons);
    void build_export_format_controls(wxBoxSizer *root, wxSizer *buttons);
    void build_profile_controls(wxBoxSizer *root);
    void build_archive_note(wxBoxSizer *root);
    void build_project_buttons(wxSizer *buttons);

    // ---- Event binding and canvas handlers (LayerEditorContent_Events.cpp) ----
    void bind_events();
    void on_preview_left_down(wxMouseEvent &event);
    void on_preview_motion(wxMouseEvent &event);
    void on_preview_wheel(wxMouseEvent &event);
    void on_preview_key_down(wxKeyEvent &event);
    void on_char_hook(wxKeyEvent &event);

    // ---- Export and conversion (LayerEditorContent_Export.cpp) ----
    void update_format_note();
    void run_export_action();
    void export_through_print(const ArchiveEntry *entry, const std::string &path, const char *project);
    void export_native_archive(const std::string &path);
    void convert_archive();
    void export_mask_images(bool range);

    // ---- Dialogs and range edit (LayerEditorContent_Dialogs.cpp) ----
    void apply_range_operation();
    void show_calibration_recipe();
    void capture_selected_settings();
    void show_project_settings();
    void show_profile_summary();
    void on_use_profile_toggled();

public:
    LayerEditorContent(wxWindow *parent, sla::LayerDocument document,
                      std::optional<sla::LayerExportProfile> profile = {}, std::string project_path = {},
                      std::optional<sla::ProjectSLASettings> settings = {},
                      std::unique_ptr<sla::SlicedResultSession> session = {},
                      std::function<void()> close = {},
                      std::function<void(sla::SlicedResultSession &)> apply = {},
                      std::function<bool()> source_available = {});
    bool save_project(bool save_as) override;
    void export_document() override { perform(m_export_action); }
    bool has_changes() const override;
    bool is_model_result() const override { return m_print && !m_print->is_raster_job(); }
    bool is_working() const override { return m_working; }
    bool source_current() const override;
    bool can_undo() const override { return !m_working && m_document.can_undo(); }
    bool can_redo() const override { return !m_working && m_document.can_redo(); }
    void undo_change() override { perform([this] { finish_stroke(false); m_document.undo(); }); }
    void redo_change() override { perform([this] { finish_stroke(false); m_document.redo(); }); }
    void apply_changes() override;
    bool matches_project_snapshot(const sla::ExportGenerationGuard &document_source,
                                  uint64_t revision) const override;
    void acknowledge_model_project_saved(uint64_t revision,
        const sla::ExportGenerationGuard *document_source) override;
    bool confirm_discard() override;
};
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
#endif
