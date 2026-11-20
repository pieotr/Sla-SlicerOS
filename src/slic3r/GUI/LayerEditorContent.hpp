#ifndef SLIC3R_GUI_LAYER_EDITOR_CONTENT_HPP
#define SLIC3R_GUI_LAYER_EDITOR_CONTENT_HPP
// Private class of the layer editor (Sliced workspace panel); members are defined in LayerEditorContent*.cpp.
#include "LayerEditorInternal.hpp"
#include "Widgets/ComboBox.hpp"   // owner-drawn drop-down (DropDown popup): see make_choice() in LayerEditorContent.cpp
#include <wx/tglbtn.h>

namespace Slic3r::GUI {
namespace layer_editor_detail {
// A row of mutually exclusive icon toggle buttons. It replaces the drop-downs of the drawing tools (wave 33) and keeps
// their index semantics: GetSelection() is the index of the pressed button in the order the buttons were added.
class ToolGroup {
public:
    size_t add(wxToggleButton *button, const wxString &label) { m_buttons.push_back(button); m_labels.push_back(label); return m_buttons.size() - 1; }
    int GetSelection() const { return m_selection; }
    wxString GetStringSelection() const { return m_selection >= 0 ? m_labels[size_t(m_selection)] : wxString(); }
    void SetSelection(int index) {
        m_selection = index;
        for (size_t i = 0; i < m_buttons.size(); ++i) m_buttons[i]->SetValue(int(i) == index);
    }
    void Enable(bool enable = true) { for (auto *button : m_buttons) button->Enable(enable); }
    void Disable() { Enable(false); }
    std::function<void()> on_change;   // called after the user picked another button
private:
    std::vector<wxToggleButton *> m_buttons;
    std::vector<wxString> m_labels;
    int m_selection = -1;
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
    // Pixel-editor state. Tool indices: 0 freehand, 1 line, 2 rectangle, 3 ellipse, 4 flood fill.
    // m_brush: 0 brush, 1 eraser; m_tool: shape tool; m_shape: 0 round, 1 square brush tip.
    ToolGroup m_brush, m_tool, m_shape;
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
    // The Operations panel is docked in the editor layout: while it is open it takes the place of the right column
    // (Issues / Export) so the mask is never covered (wave 35).
    wxPanel *m_ops_panel = nullptr;
    int m_ops_dip = 380;
    // Text scale relative to the 96 dpi design (GDK_DPI_SCALE / large system fonts): column widths are given in DIP, which
    // follow GDK_SCALE but not the font size, so they are multiplied by this factor (>= 1) to keep captions unclipped.
    double m_text_factor = 1.;
    int sdip(int dip) const { return int(std::lround(dip * m_text_factor)); }
    int right_dip() const { return m_ops_panel ? m_ops_dip : m_right_dip; }
    void close_operations_panel();
    void update_side_columns();
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
    void open_operations_panel();

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
    void build_tool_toggles(wxSizer *column);
    void build_preview_canvas();
    void build_range_controls(wxBoxSizer *root);
    void build_range_filters(wxBoxSizer *root);
    void build_action_buttons(wxBoxSizer *root);
    wxButton *add_action_button(wxSizer *buttons, const wxString &text, std::function<void()> action,
                                int flags = wxALL, int border = 5, const char *icon = nullptr);
    void set_button_icon(wxButton *button, const char *icon);
    void add_format_note(wxBoxSizer *root, const wxString &text);
    void build_project_settings_buttons(wxSizer *buttons);

    // ---- Layout (LayerEditorContent.cpp) ----
    // Column widths in DIP; the canvas gets everything else.
    static constexpr int kLeftColumnDip = 220, kRightColumnDip = 250, kOpsColumnDip = 380;
    static constexpr int kLeftMinDip = 200, kLeftMaxDip = 420, kRightMinDip = 200, kRightMaxDip = 480, kCanvasMinDip = 320, kCanvasMargin = 3, kRowDip = 30, kPrimaryRowDip = 36;
    wxBoxSizer *m_body = nullptr, *m_left_col = nullptr, *m_center_col = nullptr, *m_right_col = nullptr;
    wxBoxSizer *m_view_bar = nullptr, *m_canvas_row = nullptr, *m_info_bar = nullptr, *m_hint_box = nullptr, *m_header_tools = nullptr;
    wxWrapSizer *m_ops_bar = nullptr;
    // Status bar under the canvas: layer, tool, pixel inspector (m_pixel_info), zoom.
    wxStaticText *m_view_info = nullptr, *m_status_layer = nullptr, *m_status_tool = nullptr;
    wxButton *m_side_toggle = nullptr;
    // Resizable columns (wave 33): drag handles between the columns, widths in DIP kept in AppConfig.
    wxWindow *m_left_sash = nullptr, *m_right_sash = nullptr;
    int m_left_dip = 0, m_right_dip = 0, m_sash_origin_dip = 0;
    // Width (DIP) the tools column needs so that every caption + spin control fits without clipping.
    int m_left_needed_dip = 0;
    void set_column_width(bool left, int dip, bool persist);
    int clamp_column_width(bool left, int dip) const;
    // "Load current slices" / "Open layer file" live in the header; the Plater supplies the actions.
    wxButton *m_load_slices_button = nullptr, *m_open_file_button = nullptr;
    std::function<void()> m_load_slices_action, m_open_file_action;
    void paint_viewport_background(wxDC &dc);
    bool m_side_visible = true;
    // Fit follows the window: the viewport size the current preview was fitted to.
    wxSize m_fit_avail;
    bool m_fit_pending = false;
    int spin_min_width(wxSpinCtrl *ctrl) const;      // narrowest slot that shows the entry and both +/- buttons
    int hint_width() const;      // wrap width of the hint texts under the canvas
    int tool_text_width() const; // wrap width of the hints in the left column
    int side_width() const;      // wrap width of texts in the right column
    int status_width() const;    // wrap width of the document status in the header
    void section_heading(wxSizer *column, const wxString &title, bool first = false);
    void set_side_panel_visible(bool visible);
    void schedule_fit_refresh();
    void update_view_info();
    wxPoint viewport_centre() const;
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
    void set_document_actions(std::function<void()> load_slices, std::function<void()> open_file) override;
    void set_document_actions_enabled(bool load_slices, bool open_file) override;
};
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
#endif
