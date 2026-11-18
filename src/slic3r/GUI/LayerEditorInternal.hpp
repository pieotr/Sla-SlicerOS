#ifndef SLIC3R_GUI_LAYER_EDITOR_INTERNAL_HPP
#define SLIC3R_GUI_LAYER_EDITOR_INTERNAL_HPP
// Private header shared by the LayerEditorDialog translation units (see "Layer editor source map" in doc/DEVELOPER.md).
#include "libslic3r/SLA/ExposureFinderTable.hpp"
#include "libslic3r/SLA/LayerListStream.hpp"
#include "libslic3r/SLA/ExportPrinterValidation.hpp"
#include "libslic3r/SLA/ExportFormatPreference.hpp"
#include "LayerEditorDialog.hpp"
#include "Widgets/ComboBox.hpp"
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
#include "libslic3r/SLA/LayerMemoryBudget.hpp"
#include "libslic3r/SLA/LayerIssues.hpp"
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
#include <wx/listctrl.h>
#include <wx/dataview.h>
#include <wx/collpane.h>
#include <wx/dcgraph.h>
#include <wx/accel.h>
#include <wx/treectrl.h>
#include <wx/settings.h>
#include <map>
#include <numeric>
#include <cmath>
#include <stdexcept>
#include "libslic3r/SLA/LayerOperations.hpp"
#include "libslic3r/SLA/LayerExports.hpp"
#include <optional>
#include <algorithm>
#include <functional>
#include <future>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#if defined(__WXGTK__)
    #include <gtk/gtk.h>
#endif


namespace Slic3r::GUI {
namespace layer_editor_detail {
struct TaskLock {
    bool &flag;
    explicit TaskLock(bool &value) : flag(value) { flag = true; }
    ~TaskLock() { flag = false; }
};
// ---------------------------------------------------------------------------------------------------
// Layer operations dialog (goal G6): native UVtools operations (SLA/LayerOperations.hpp) for the
// Sliced editor. Three clearly separated parts:
//   1. OpDef table   - which operations exist, their parameter forms and UVtools defaults.
//   2. build_*       - turn the form values into a runnable BuiltOp (validation messages included).
//   3. LayerOperationsDialog - widgets, Preview (current layer, not committed) and Apply.
// The dialog never touches the document itself; the editor panel supplies a LayerOpsHost.
// ---------------------------------------------------------------------------------------------------
namespace layerops = sla::layerops;
namespace layerexport = sla::layerexport;

// What the dialog needs from the editor. All functions run on the UI thread.
struct LayerOpsJob {
    wxString title;
    sla::LayerDocument::MaskBatchOperation mask_op;  // set for pixel operations
    // Streaming form of `mask_op` (same result, a few layers decoded at a time); mask_op is only its fallback
    // when the kernel reports NotStreamable. `kernel_z` = top Z of every layer for kernels that need it.
    sla::LayerDocument::MaskKernelFactory kernel;
    std::vector<double> kernel_z;
    sla::LayerDocument::LayerListOperation list_op;  // whole-vector layer-list operation (tests / fallback)
    // Streamed layer-list operation (Clone/Remove/ReHeight/DoubleExposure/ChangeResolution, and with `stream_params`
    // Phased exposure / Timelapse / Raise on finish): nothing is decoded that the operation does not need.
    sla::LayerDocument::ListStreamOperation stream;
    bool stream_params = false;
    // `stream` only edits pixels (Repair layers without removing layers): applied with apply_pixel_list_stream, works on
    // every document and keeps the layer list.
    bool stream_pixels = false;
    // Parameter-only operation that streams by itself (Dynamic lifts: counts the lit pixels layer by layer, then edits
    // the parameter rows). Gets the document, the fallback layer height and the progress; false = cancelled.
    std::function<bool(sla::LayerDocument &, double, const sla::LayerProgress &)> document_op;
    bool changes_resolution = false;
    // Per-layer parameter operations (batch 2). `param_changes_list`: layers are added/removed (generated
    // documents only); otherwise only the parameter table changes and the whole edit is one undo step.
    sla::LayerDocument::ParameterOperation param_op;
    bool param_needs_masks = false, param_changes_list = false;
    // Lithophane builds a brand-new stack of layers: applied in place when the document can take a
    // different layer list, otherwise opened as a new generated document.
    std::function<void(std::vector<layerops::Mask> &, std::vector<double> &, std::vector<layerops::LayerParams> &,
                       const sla::LayerProgress &)> lithophane;
    // Streamed in-place form (given the document's first bottom / normal layer parameters).
    std::function<sla::LayerDocument::ListStreamOperation(const layerops::LayerParams &, const layerops::LayerParams &)> lithophane_stream;
};
// Everything an export needs. The layers are NOT decoded up front: `layers.fetch(i)` decodes one on demand (from the
// background worker's threads) and `layers.scan` visits all of them, so exporting a document of any size costs a few
// layers of memory plus the result (see LayerExports.hpp).
struct ExportData {
    layerops::StreamLayout layers;
    std::vector<double> z_mm;
    std::vector<layerops::LayerParams> table; // empty when the document stores no parameters
};
struct LayerExportJob {
    wxString title;
    std::function<void(const ExportData &, const sla::LayerProgress &)> run;
};
// Committed = the open document changed; Elsewhere = the result went to a new document / file.
enum class RunOutcome { Cancelled, Committed, Elsewhere };
struct LayerOpsHost {
    std::function<size_t()> layer_count;
    std::function<size_t()> current_layer;                         // 0-based
    std::function<std::pair<size_t, size_t>()> selected_range;     // 0-based, inclusive
    std::function<std::optional<layerops::Rect>()> last_rectangle; // last rectangle drawn in the editor (px)
    std::function<std::pair<size_t, size_t>()> mask_size;          // width, height in px
    std::function<std::vector<double>()> layer_z_mm;               // top Z of every layer (mm)
    std::function<size_t()> bottom_layer_count;
    std::function<std::string(bool)> list_refusal;                 // empty = layer-list edits possible
    std::function<layerops::Mask()> read_current_layer;
    std::function<void(const layerops::Mask *)> show_preview;      // nullptr clears the canvas preview
    std::function<RunOutcome(const LayerOpsJob &)> run;            // background run
    std::function<bool(const LayerExportJob &)> run_export;        // background export; true = finished
    std::function<std::string()> parameter_refusal;                // empty = the document stores per-layer parameters
    std::function<std::vector<layerops::LayerParams>()> parameter_table;
    std::function<layerops::PrinterContext()> printer_context;
    std::function<std::optional<std::pair<double, double>>()> display_size_mm; // LCD width, height
    std::function<void()> on_layer_list_changed;                   // editor reopens the dialog with new data
};

// ---- 1. Operation table ------------------------------------------------------------------------------
enum class OpGroup { Transform, Pixels, Layers, Parameters, Export };

struct OpField {
    // Opt: single-line text, empty = "leave unchanged". File: single-line path with a Browse button.
    enum Kind { Int, Dbl, Bool, Choice, Text, Opt, File } kind = Int;
    std::string key;
    wxString label, tip;
    double lo = 0, hi = 0, def = 0; // Int/Dbl range and default; Bool/Choice default index
    int digits = 2;                 // Dbl precision
    std::vector<wxString> choices;
    std::string text;               // Text default
};

// Form values by key. Choices are stored as their index, booleans as 0/1.
struct OpValues {
    std::map<std::string, double> number;
    std::map<std::string, std::string> text;
    double d(const std::string &k) const { return number.at(k); }
    int i(const std::string &k) const { return int(std::lround(number.at(k))); }
    unsigned u(const std::string &k) const { return unsigned(std::max(0, i(k))); }
    bool b(const std::string &k) const { return number.at(k) != 0; }
    const std::string &s(const std::string &k) const { return text.at(k); }
    // Optional number typed into an Opt field; empty = not engaged.
    std::optional<double> opt(const std::string &k) const {
        const std::string &t = text.at(k);
        const auto b = t.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::nullopt;
        const std::string trimmed = t.substr(b, t.find_last_not_of(" \t\r\n") - b + 1);
        char *end = nullptr;
        const double value = std::strtod(trimmed.c_str(), &end);
        if (end == trimmed.c_str() || *end != '\0' || !std::isfinite(value))
            throw std::invalid_argument("'" + trimmed + "' is not a number (field " + k + ").");
        return value;
    }
};

// Everything an operation needs besides its form (captured when Preview/Apply is pressed).
struct OpContext {
    size_t layer_count = 0;
    std::vector<double> z_mm;
    size_t first = 0, last = 0;       // inclusive, 0-based
    layerops::Rect roi;               // empty = whole image
    size_t width = 0, height = 0;     // px
    size_t bottom_layers = 0;
    // Per-layer parameters (empty when the document stores none) and what the format can store.
    std::vector<layerops::LayerParams> table;
    layerops::PrinterContext printer;
    std::string parameter_refusal;
    std::optional<std::pair<double, double>> display_mm;
    bool list_edits_possible = false; // the open document can take a different layer list
    layerops::LayerSelection selection() const { return {first, last, roi}; }
};

using MaskRunner = std::function<void(std::vector<layerops::Mask> &, const std::vector<double> &z_mm,
                                      const layerops::LayerSelection &, const sla::LayerProgress &)>;
using ListRunner = std::function<sla::LayerDocument::LayerListChange(std::vector<layerops::Mask> &,
                                      std::vector<double> &z_mm, const sla::LayerProgress &)>;
// A validated, ready-to-run operation. Exactly one of `mask` / `list` / `param` / `exporter` / `lithophane` is set.
// A file or folder export: the runner gets the decoded document and the chosen destination.
struct ExportPlan {
    bool folder = false;
    wxString wildcard, default_name;
    std::function<void(const ExportData &, const std::string &path, const sla::LayerProgress &)> run;
};
struct BuiltOp {
    MaskRunner mask;                                  // whole-vector form: Preview (one layer) and batch fallback
    sla::LayerDocument::MaskKernelFactory kernel;     // streaming form used by Apply
    bool kernel_needs_z = false;
    ListRunner list;
    sla::LayerDocument::ListStreamOperation stream;   // streamed layer-list form used by Apply (replaces `list`)
    bool stream_params = false;
    bool stream_pixels = false;
    std::function<bool(sla::LayerDocument &, double, const sla::LayerProgress &)> document_op;
    bool changes_resolution = false;
    sla::LayerDocument::ParameterOperation param;
    bool param_needs_masks = false, param_changes_list = false;
    std::optional<ExportPlan> exporter;
    decltype(LayerOpsJob::lithophane) lithophane;
    decltype(LayerOpsJob::lithophane_stream) lithophane_stream;
};

struct OpDef {
    OpGroup group;
    wxString label, description;
    std::vector<OpField> fields;
    bool uses_range = true;    // layer range spinners apply
    bool uses_roi = true;      // optional region of interest applies
    bool previewable = true;   // can be shown on the current layer alone
    bool pattern_helper = false; // form gets the dimming-pattern generator (keys "pattern", "alternate")
    std::function<BuiltOp(const OpValues &, const OpContext &)> build;
};

namespace ops_detail {
inline OpField num(const char *key, const char *label, double lo, double hi, double def, const char *tip = "")
{ OpField f; f.kind = OpField::Int; f.key = key; f.label = label; f.lo = lo; f.hi = hi; f.def = def; f.tip = tip; return f; }
inline OpField dbl(const char *key, const char *label, double lo, double hi, double def, int digits, const char *tip = "")
{ OpField f; f.kind = OpField::Dbl; f.key = key; f.label = label; f.lo = lo; f.hi = hi; f.def = def; f.digits = digits; f.tip = tip; return f; }
inline OpField flag(const char *key, const char *label, bool def, const char *tip = "")
{ OpField f; f.kind = OpField::Bool; f.key = key; f.label = label; f.def = def; f.tip = tip; return f; }
inline OpField pick(const char *key, const char *label, std::vector<wxString> choices, int def = 0, const char *tip = "")
{ OpField f; f.kind = OpField::Choice; f.key = key; f.label = label; f.choices = std::move(choices); f.def = def; f.tip = tip; return f; }
inline OpField txt(const char *key, const char *label, const std::string &def, const char *tip = "")
{ OpField f; f.kind = OpField::Text; f.key = key; f.label = label; f.text = def; f.tip = tip; return f; }

inline const std::vector<wxString> &anchor_names()
{
    static const std::vector<wxString> names{"Top left", "Top center", "Top right", "Middle left", "Middle center",
        "Middle right", "Bottom left", "Bottom center", "Bottom right"};
    return names;
}
inline std::vector<wxString> with_none(std::vector<wxString> names) { names.push_back("None (keep centred)"); return names; }
inline const std::vector<wxString> &threshold_names()
{
    static const std::vector<wxString> n{"Binary", "Binary inverted", "Trunc", "To zero", "To zero inverted", "Otsu (auto)", "Triangle (auto)"};
    return n;
}

// ReHeight offers only presets that divide/multiply the current layer height exactly (UVtools rule).
inline std::vector<wxString> reheight_labels(const std::vector<layerops::ReHeightPreset> &presets)
{
    std::vector<wxString> labels;
    for (const auto &p : presets)
        labels.push_back(wxString::Format("%s x%u -> %.3f mm, %zu layers", p.is_multiply ? "Merge" : "Split",
                                          unsigned(p.modifier), p.layer_height_mm, p.layer_count));
    return labels;
}
inline double nominal_layer_height_mm(const std::vector<double> &z)
{
    if (z.size() < 2) return 0.05;
    return std::max(0.001, z[z.size() / 2] - z[z.size() / 2 - 1]);
}
[[noreturn]] inline void invalid(const std::string &message) { throw std::invalid_argument(message); }
// Pre-flight check shown inline in the dialog before any work starts.
inline void require_valid(const std::string &message) { if (!message.empty()) invalid(message); }
// The selection's range is validated against the layer count here; the backend clamps otherwise.
inline void require_range(const OpContext &c)
{
    if (c.first > c.last || c.last >= c.layer_count) invalid("The layer range is outside of the document.");
}
} // namespace ops_detail

// Defined in LayerEditorCommon.cpp
void set_dataview_accessible_name(wxWindow *view, const char *name);
const char *issue_type_label(sla::LayerIssue::Type type);
std::vector<double> issue_layer_positions_z(const sla::LayerDocument &document);

// LayerOperationCatalog.cpp / LayerOperationCatalog_Batch2.cpp
void append_batch2_defs(std::vector<OpDef> &defs, const OpContext &initial);
std::vector<OpDef> make_layer_operation_defs(const OpContext &initial);

namespace batch2_detail {
using namespace ops_detail;

inline OpField opt_field(const std::string &key, const std::string &label, const std::string &def = "", const char *tip = "")
{ OpField f; f.kind = OpField::Opt; f.key = key; f.label = wxString::FromUTF8(label); f.text = def; f.tip = tip; return f; }
inline OpField file_field(const char *key, const char *label, const char *tip = "")
{ OpField f; f.kind = OpField::File; f.key = key; f.label = label; f.tip = tip; return f; }

inline const layerops::LayerParams &first_bottom(const std::vector<layerops::LayerParams> &t)
{
    static const layerops::LayerParams empty;
    return t.empty() ? empty : t.front();
}
inline const layerops::LayerParams &first_normal(const std::vector<layerops::LayerParams> &t)
{
    for (const auto &p : t) if (!p.is_bottom) return p;
    return first_bottom(t);
}
// Parameter operations are refused up front (inline in the dialog) when the document stores no table.
inline void require_parameters(const OpContext &c) { if (!c.parameter_refusal.empty()) invalid(c.parameter_refusal); }

// Orientation of exported pictures: flip first, rotate afterwards (UVtools order).
inline std::vector<OpField> orientation_fields()
{
    return {pick("flip", "Flip", {"None", "Horizontal", "Vertical", "Both"}, 0, "Mirror the picture the way the LCD shows it."),
            pick("rot", "Rotate", {"None", "90 clockwise", "180", "90 counter-clockwise"}, 0)};
}
inline layerexport::Orientation read_orientation(const OpValues &v)
{
    layerexport::Orientation o;
    if (v.i("flip") > 0) o.flip = layerops::FlipDirection(v.i("flip") - 1);
    o.rotate = layerops::RotateDirection(std::clamp(v.i("rot"), 0, 3));
    return o;
}

// The editable fields of LayerParamEdit, shared by the "normal" and "bottom" sets of Edit parameters.
struct EditFieldSpec { const char *suffix, *label; std::optional<double> layerops::LayerParamEdit::*member; };
inline const std::vector<EditFieldSpec> &edit_field_specs()
{
    using E = layerops::LayerParamEdit;
    static const std::vector<EditFieldSpec> specs{
        {"exp", "Exposure time (s)", &E::exposure_s}, {"wbc", "Wait before cure (s)", &E::wait_before_cure_s},
        {"wac", "Wait after cure (s)", &E::wait_after_cure_s}, {"wal", "Wait after lift (s)", &E::wait_after_lift_s},
        {"lh", "Lift height (mm)", &E::lift_height_mm}, {"ls", "Lift speed (mm/min)", &E::lift_speed},
        {"lh2", "Lift height 2 (mm)", &E::lift_height2_mm}, {"ls2", "Lift speed 2 (mm/min)", &E::lift_speed2},
        {"rs", "Retract speed (mm/min)", &E::retract_speed}, {"rh2", "Retract height 2 (mm)", &E::retract_height2_mm},
        {"rs2", "Retract speed 2 (mm/min)", &E::retract_speed2}};
    return specs;
}
inline layerops::LayerParamEdit read_edit(const OpValues &v, const std::string &prefix, bool with_z)
{
    layerops::LayerParamEdit edit;
    for (const auto &spec : edit_field_specs()) edit.*spec.member = v.opt(prefix + spec.suffix);
    if (const auto pwm = v.opt(prefix + "pwm")) edit.light_pwm = uint8_t(std::clamp(std::lround(*pwm), 0L, 255L));
    if (with_z) edit.position_z_mm = v.opt(prefix + "z");
    return edit;
}

// Identity source map for operations that keep the layer list (plus `extra` copies of the last layer).
inline std::vector<size_t> identity_sources(size_t count, size_t extra_from_last = 0)
{
    std::vector<size_t> source(count);
    std::iota(source.begin(), source.end(), size_t(0));
    for (size_t i = 0; i < extra_from_last; ++i) source.push_back(count - 1);
    return source;
}

inline layerops::Mask load_grey_image(const std::string &path)
{
    if (path.empty()) invalid("Choose an image file first.");
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(path))) invalid("The image could not be loaded: " + path);
    layerops::Mask mask(size_t(image.GetWidth()), size_t(image.GetHeight()));
    const unsigned char *rgb = image.GetData();
    for (size_t i = 0; i < mask.px.size(); ++i) // Rec.601 luma, like an 8-bit grey conversion
        mask.px[i] = uint8_t((299 * rgb[3 * i] + 587 * rgb[3 * i + 1] + 114 * rgb[3 * i + 2]) / 1000);
    return mask;
}

inline std::string format_duration(double seconds)
{
    const long total = std::lround(seconds);
    return wxString::Format("%ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60).ToStdString();
}
} // namespace batch2_detail

// LayerCalculatorDialog.cpp: modal UVtools-style calculator.
void show_layer_calculator(wxWindow *parent, size_t res_x, size_t res_y, std::optional<std::pair<double, double>> display_mm,
                           std::vector<layerops::LayerParams> table, double layer_height_mm);
// LayerOperationsDialog.cpp: modeless operations dialog (a LayerOperationsDialog).
wxDialog *create_layer_operations_dialog(wxWindow *parent, LayerOpsHost host);
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
namespace Slic3r::GUI { using namespace layer_editor_detail; }
#endif
