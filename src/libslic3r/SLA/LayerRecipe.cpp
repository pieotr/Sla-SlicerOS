// SPDX-License-Identifier: AGPL-3.0-or-later
// Layer recipe parser and runner. See LayerRecipe.hpp and doc/AUTOMATION.md.
#include "LayerRecipe.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "ExportRequest.hpp"
#include "LayerConversionPlan.hpp"
#include "LayerExports.hpp"
#include "LayerIssues.hpp"
#include "LayerListStream.hpp"
#include "LayerOperations.hpp"
#include "LayerSuggestions.hpp"
#include "ProjectArchive.hpp"

namespace Slic3r::sla::recipe {

namespace fs = boost::filesystem;
namespace lo = layerops;
namespace lx = layerexport;
using Json = nlohmann::json;

RecipeError::RecipeError(std::string loc, std::string msg)
    : std::runtime_error(loc.empty() ? msg : loc + ": " + msg), location(std::move(loc)), message(std::move(msg))
{}

namespace {

// Used by every operation that does not read a layer height from the document (clone gaps etc.).
constexpr double kFallbackLayerHeightMm = 0.05;

std::string fmt(double v)
{
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%g", v);
    return buffer;
}

// The validators of the operations end their messages with a newline (UVtools style); the step error does not.
void require_ok(const std::string &error)
{
    if (error.empty()) return;
    std::string text = error;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    throw RuntimeError(text);
}

// ---------------------------------------------------------------------------------------------------------
// Reader: typed access to one JSON object. Remembers every key that was asked for, so finish() can reject the
// keys nobody asked for (typos) with a message that lists the valid ones.
// ---------------------------------------------------------------------------------------------------------
class Reader
{
public:
    Reader(const Json &json, std::string location) : m_json(json), m_location(std::move(location))
    {
        if (!m_json.is_object()) throw RecipeError(m_location, "expected an object");
    }

    std::string at(const std::string &key) const { return m_location.empty() ? key : m_location + "." + key; }
    [[noreturn]] void fail(const std::string &key, const std::string &message) const { throw RecipeError(at(key), message); }

    const Json *find(const std::string &key)
    {
        m_known.push_back(key);
        const auto it = m_json.find(key);
        return it == m_json.end() ? nullptr : &*it;
    }

    bool boolean(const std::string &key, bool fallback)
    {
        const Json *v = find(key);
        if (!v) return fallback;
        if (!v->is_boolean()) fail(key, "expected true or false");
        return v->get<bool>();
    }

    std::optional<bool> opt_bool(const std::string &key)
    {
        const Json *v = find(key);
        if (!v) return std::nullopt;
        if (!v->is_boolean()) fail(key, "expected true or false");
        return v->get<bool>();
    }

    std::optional<double> opt_number(const std::string &key, double lo, double hi)
    {
        const Json *v = find(key);
        if (!v) return std::nullopt;
        if (!v->is_number()) fail(key, "expected a number");
        const double d = v->get<double>();
        if (!std::isfinite(d) || d < lo || d > hi)
            fail(key, "must be between " + fmt(lo) + " and " + fmt(hi) + " (got " + fmt(d) + ")");
        return d;
    }
    double number(const std::string &key, double fallback, double lo, double hi) { return opt_number(key, lo, hi).value_or(fallback); }

    std::optional<long long> opt_integer(const std::string &key, long long lo, long long hi)
    {
        const Json *v = find(key);
        if (!v) return std::nullopt;
        if (!v->is_number_integer()) fail(key, "expected an integer");
        const double d = v->get<double>(); // compared as double: no wrap-around for huge unsigned values
        if (d < double(lo) || d > double(hi))
            fail(key, "must be between " + std::to_string(lo) + " and " + std::to_string(hi) + " (got " + fmt(d) + ")");
        return v->get<long long>();
    }
    int      integer(const std::string &key, int fallback, int lo, int hi) { return int(opt_integer(key, lo, hi).value_or(fallback)); }
    unsigned uint(const std::string &key, unsigned fallback, unsigned hi) { return unsigned(opt_integer(key, 0, hi).value_or(fallback)); }
    // 0-based layer index
    size_t index(const std::string &key, size_t fallback) { return size_t(opt_integer(key, 0, 100000000).value_or(static_cast<long long>(fallback))); }

    std::string string(const std::string &key, const std::string &fallback = {})
    {
        const Json *v = find(key);
        if (!v) return fallback;
        if (!v->is_string()) fail(key, "expected a string");
        return v->get<std::string>();
    }
    std::string required_string(const std::string &key)
    {
        const Json *v = find(key);
        if (!v) fail(key, "is required");
        if (!v->is_string() || v->get<std::string>().empty()) fail(key, "expected a non-empty string");
        return v->get<std::string>();
    }

    template<class T> T choice(const std::string &key, T fallback, const std::vector<std::pair<std::string, T>> &table)
    {
        const Json *v = find(key);
        if (!v) return fallback;
        if (!v->is_string()) fail(key, "expected a string");
        const std::string text = v->get<std::string>();
        std::string allowed;
        for (const auto &[name, value] : table) {
            if (name == text) return value;
            allowed += (allowed.empty() ? "" : ", ") + name;
        }
        fail(key, "unknown value '" + text + "'; allowed: " + allowed);
    }

    std::optional<lo::Rect> rect(const std::string &key)
    {
        const Json *v = find(key);
        if (!v) return std::nullopt;
        Reader r(*v, at(key));
        lo::Rect out;
        out.x = r.integer("x", 0, 0, 1000000);
        out.y = r.integer("y", 0, 0, 1000000);
        out.w = r.integer("width", 1, 1, 1000000);
        out.h = r.integer("height", 1, 1, 1000000);
        if (!r.has("width")) r.fail("width", "is required");
        if (!r.has("height")) r.fail("height", "is required");
        r.finish();
        return out;
    }

    std::optional<Reader> object(const std::string &key)
    {
        const Json *v = find(key);
        if (!v) return std::nullopt;
        return Reader(*v, at(key));
    }

    bool has(const std::string &key) const { return m_json.contains(key); }

    // Rejects keys that were never asked for.
    void finish()
    {
        for (const auto &item : m_json.items()) {
            if (std::find(m_known.begin(), m_known.end(), item.key()) != m_known.end()) continue;
            std::string known;
            for (const auto &k : m_known) known += (known.empty() ? "" : ", ") + k;
            fail(item.key(), "unknown parameter; known parameters: " + known);
        }
    }

private:
    const Json              &m_json;
    std::string              m_location;
    std::vector<std::string> m_known;
};

// first_layer / last_layer (0-based, inclusive) and an optional ROI, like UVtools' OperationLayerRange.
lo::LayerSelection read_selection(Reader &r, bool with_roi = true)
{
    lo::LayerSelection s;
    s.first_layer = r.index("first_layer", 0);
    s.last_layer  = r.index("last_layer", lo::kLastLayer);
    if (s.last_layer != lo::kLastLayer && s.first_layer > s.last_layer) r.fail("first_layer", "must not be greater than last_layer");
    if (with_roi)
        if (const auto roi = r.rect("roi")) s.roi = *roi;
    return s;
}

lx::Orientation read_orientation(Reader &r)
{
    lx::Orientation o;
    if (auto sub = r.object("orientation")) {
        const std::vector<std::pair<std::string, std::optional<lo::FlipDirection>>> flips = {
            {"none", std::nullopt}, {"horizontal", lo::FlipDirection::Horizontal}, {"vertical", lo::FlipDirection::Vertical}, {"both", lo::FlipDirection::Both}};
        o.flip   = sub->choice<std::optional<lo::FlipDirection>>("flip", std::nullopt, flips);
        o.rotate = sub->choice("rotate", lo::RotateDirection::None,
                               std::vector<std::pair<std::string, lo::RotateDirection>>{{"none", lo::RotateDirection::None},
                                                                                      {"rotate_90_clockwise", lo::RotateDirection::Rotate90Clockwise},
                                                                                      {"rotate_180", lo::RotateDirection::Rotate180},
                                                                                      {"rotate_90_counter_clockwise", lo::RotateDirection::Rotate90CounterClockwise}});
        sub->finish();
    }
    return o;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Run time state shared by the steps of one run. Not in the anonymous namespace: the header forward declares
// StepCode, so it must be the same type.
// ---------------------------------------------------------------------------------------------------------
struct Staged
{
    fs::path final_path, temp_path;
    bool     directory = false;
};

struct Ctx
{
    std::optional<LayerDocument> doc;
    const RunOptions            *options = nullptr;
    const Step                  *step    = nullptr; // the step being executed
    fs::path                     base;
    std::vector<Staged>          staged;
    std::set<std::string>        claimed;
    LayerProgress                progress; // maps the step's own (done, total) onto the whole recipe
    std::string                  message;  // one line for the report
    bool                         dry = false;

    LayerDocument &document()
    {
        if (!doc) throw RuntimeError("no document is open (the recipe needs an 'open' step or an input file first)");
        return *doc;
    }

    fs::path resolve(const std::string &path) const
    {
        fs::path p(path);
        return p.is_absolute() ? p : base / p;
    }

    // The path a step writes: --output replaces the path of the final save/convert step.
    std::string output_path(const std::string &configured) const
    {
        return step && step->final_output && !options->output.empty() ? options->output : configured;
    }

    // Validates (and claims) an output target. Called from the step's check hook, before anything is written.
    void check_output(const std::string &path, bool directory)
    {
        if (path.empty()) throw RuntimeError("the output path is empty");
        const fs::path target = resolve(path);
        const fs::path parent = target.has_parent_path() ? target.parent_path() : fs::path(".");
        if (!fs::is_directory(parent)) throw RuntimeError("the folder '" + parent.string() + "' does not exist");
        if (directory) {
            if (fs::exists(target) && (!fs::is_directory(target) || !fs::is_empty(target)))
                throw RuntimeError("the output folder '" + target.string() + "' already exists and is not empty");
        } else if (fs::is_directory(target))
            throw RuntimeError("the output path '" + target.string() + "' is a folder");
        if (!claimed.insert(target.string()).second) throw RuntimeError("two steps write the same output '" + target.string() + "'");
    }

    // Returns the staging path a step must write to. The hidden name keeps the extension (formats are chosen by it).
    std::string stage(const std::string &path, bool directory)
    {
        Staged s;
        s.final_path = resolve(path);
        s.directory  = directory;
        const fs::path parent = s.final_path.has_parent_path() ? s.final_path.parent_path() : fs::path(".");
        s.temp_path  = parent / (".recipe-" + std::to_string(staged.size()) + "-" + s.final_path.filename().string());
        boost::system::error_code ec;
        fs::remove_all(s.temp_path, ec);
        staged.push_back(s);
        return s.temp_path.string();
    }

    void discard_staged() noexcept
    {
        boost::system::error_code ec;
        for (const auto &s : staged) fs::remove_all(s.temp_path, ec);
        staged.clear();
    }

    // Publishes every staged output. Renames within one folder are atomic per file; a failure here (full disk,
    // permissions) is reported and can leave earlier outputs of THIS recipe in place, nothing else.
    std::vector<std::string> publish_staged()
    {
        std::vector<std::string> published;
        for (const auto &s : staged) {
            boost::system::error_code ec;
            if (s.directory && fs::exists(s.final_path)) fs::remove(s.final_path, ec); // empty folder (checked earlier)
            fs::rename(s.temp_path, s.final_path, ec);
            if (ec) throw RuntimeError("cannot publish '" + s.final_path.string() + "': " + ec.message());
            published.push_back(s.final_path.string());
        }
        staged.clear();
        return published;
    }
};

// What a step is: a check (also used by the dry run) and an action.
struct StepCode
{
    std::function<void(Ctx &)> check; // throws RuntimeError when the step cannot run on the current state
    std::function<void(Ctx &)> run;   // may throw LayerOperationCancelled / RuntimeError
    bool changes_layer_list = false;  // dry run: later document dependent checks are no longer reliable
};

namespace {

std::vector<lo::Mask> decode_masks(LayerDocument &doc, const LayerProgress &progress)
{
    std::vector<lo::Mask> masks;
    masks.reserve(doc.layer_count());
    for (size_t i = 0; i < doc.layer_count(); ++i) {
        check_layer_progress(progress, i, doc.layer_count());
        auto image = doc.image(i);
        lo::Mask m(size_t(image.cols), size_t(image.rows));
        m.px = std::move(image.buf);
        masks.push_back(std::move(m));
    }
    return masks;
}

double layer_pitch_mm(const LayerDocument &doc)
{
    if (const auto job = doc.job_parameters(); job && job->layer_height > 0) return job->layer_height;
    return kFallbackLayerHeightMm;
}

// Z of the top of every layer, mm. Formats without per-layer Z fall back to a constant pitch.
std::vector<double> layer_z(const LayerDocument &doc)
{
    std::vector<double> z(doc.layer_count());
    const double pitch = layer_pitch_mm(doc);
    for (size_t i = 0; i < z.size(); ++i) {
        const auto parameters = doc.layer_parameters(i);
        const auto it = parameters.find(LayerParameter::PositionZ);
        z[i] = it != parameters.end() ? it->second : pitch * double(i + 1);
    }
    return z;
}

void check_range(const LayerDocument &doc, size_t first, size_t last)
{
    if (doc.layer_count() == 0) throw RuntimeError("the document has no layers");
    if (first >= doc.layer_count())
        throw RuntimeError("first_layer " + std::to_string(first) + " is outside the document (" + std::to_string(doc.layer_count()) + " layers)");
    if (last != lo::kLastLayer && last < first) throw RuntimeError("last_layer is before first_layer");
}

// Cancelled progress callbacks make the document APIs return false; turn that into the common exception.
void require_done(bool done)
{
    if (!done) throw LayerOperationCancelled{};
}

// ---- helpers building the common step shapes --------------------------------------------------------------
using MaskFn = std::function<void(std::vector<lo::Mask> &, const LayerProgress &)>;

// Operation expressed as a layerops kernel (per layer, or with a bounded window of neighbours): the document streams it
// (decode a few layers, apply, encode, store; history goes to disk) so the size of the document does not matter, and the
// result equals the batch function's, which runs the same kernel. `needs_z` hands the kernel the Z of every layer.
// `fallback` (optional) is the batch implementation used when the kernel reports NotStreamable.
using KernelFn = std::function<lo::StreamKernel(const lo::StreamLayout &)>;
StepCode kernel_step(size_t first, size_t last, KernelFn factory, bool needs_z = false, MaskFn fallback = {})
{
    StepCode c;
    c.check = [first, last](Ctx &x) { check_range(x.document(), first, last); };
    c.run   = [factory, needs_z, fallback](Ctx &x) {
        const std::vector<double> z = needs_z ? layer_z(x.document()) : std::vector<double>{};
        LayerDocument::MaskBatchOperation batch;
        if (fallback) batch = [&](std::vector<lo::Mask> &layers, const LayerProgress &p) { fallback(layers, p); };
        require_done(x.document().apply_mask_kernel(LayerMaskPlane::Main, z, factory, x.progress, batch));
    };
    return c;
}

StepCode list_step(size_t first, size_t last, std::function<void(std::vector<lo::Mask> &, std::vector<double> &, LayerDocument::LayerListChange &, const LayerProgress &)> apply,
                   bool changes_resolution = false)
{
    StepCode c;
    c.changes_layer_list = true;
    c.check = [first, last, changes_resolution](Ctx &x) {
        check_range(x.document(), first, last);
        require_ok(x.document().layer_list_refusal(changes_resolution));
    };
    c.run = [apply, changes_resolution](Ctx &x) {
        require_done(x.document().apply_layer_list_operation(
            [&](std::vector<lo::Mask> &layers, std::vector<double> &z, const LayerProgress &p) {
                LayerDocument::LayerListChange change;
                apply(layers, z, change, p);
                return change;
            },
            changes_resolution, layer_pitch_mm(x.document()), x.progress));
    };
    return c;
}

// Streamed layer-list operation (see LayerListStream.hpp): nothing is decoded that the operation does not need.
StepCode stream_step(size_t first, size_t last, std::function<sla::liststream::Operation(LayerDocument &)> make, bool changes_resolution = false)
{
    StepCode c;
    c.changes_layer_list = true;
    c.check = [first, last, changes_resolution](Ctx &x) {
        check_range(x.document(), first, last);
        require_ok(x.document().layer_list_refusal(changes_resolution));
    };
    c.run = [make, changes_resolution](Ctx &x) {
        LayerDocument &doc = x.document();
        require_done(doc.apply_layer_list_stream(make(doc), changes_resolution, layer_pitch_mm(doc), x.progress));
    };
    return c;
}

// ---------------------------------------------------------------------------------------------------------
// Operations: one compile function per op. Each reads (and validates) its parameters and returns the step code.
// Parameter names follow the UVtools property names in snake_case.
// ---------------------------------------------------------------------------------------------------------
using Choices = std::vector<std::pair<std::string, lo::Anchor>>;

const Choices &anchor_choices()
{
    static const Choices table = {{"top_left", lo::Anchor::TopLeft},       {"top_center", lo::Anchor::TopCenter},       {"top_right", lo::Anchor::TopRight},
                                  {"middle_left", lo::Anchor::MiddleLeft}, {"middle_center", lo::Anchor::MiddleCenter}, {"middle_right", lo::Anchor::MiddleRight},
                                  {"bottom_left", lo::Anchor::BottomLeft}, {"bottom_center", lo::Anchor::BottomCenter}, {"bottom_right", lo::Anchor::BottomRight},
                                  {"none", lo::Anchor::None}};
    return table;
}

const std::vector<std::pair<std::string, lo::ThresholdType>> &threshold_choices()
{
    static const std::vector<std::pair<std::string, lo::ThresholdType>> table = {
        {"binary", lo::ThresholdType::Binary}, {"binary_inv", lo::ThresholdType::BinaryInv}, {"trunc", lo::ThresholdType::Trunc},
        {"to_zero", lo::ThresholdType::ToZero}, {"to_zero_inv", lo::ThresholdType::ToZeroInv}, {"otsu", lo::ThresholdType::Otsu},
        {"triangle", lo::ThresholdType::Triangle}};
    return table;
}

StepCode compile_flip(Reader &r)
{
    lo::FlipParams p;
    p.selection = read_selection(r);
    p.direction = r.choice("direction", lo::FlipDirection::Horizontal,
                           std::vector<std::pair<std::string, lo::FlipDirection>>{{"horizontal", lo::FlipDirection::Horizontal},
                                                                                  {"vertical", lo::FlipDirection::Vertical},
                                                                                  {"both", lo::FlipDirection::Both}});
    p.make_copy = r.boolean("make_copy", false);
    r.finish();
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::flip_kernel(p, l); });
}

StepCode compile_rotate(Reader &r)
{
    lo::RotateParams p;
    p.selection     = read_selection(r);
    p.angle_degrees = r.number("angle_degrees", 90, -360, 360);
    r.finish();
    if (const auto e = lo::validate_rotate(p); !e.empty()) r.fail("angle_degrees", e);
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::rotate_kernel(p, l); });
}

StepCode compile_resize(Reader &r)
{
    lo::ResizeParams p;
    p.selection    = read_selection(r);
    p.x_percent    = r.number("x_percent", 100, 0.01, 10000);
    p.y_percent    = r.number("y_percent", 100, 0.01, 10000);
    p.constrain_xy = r.boolean("constrain_xy", false);
    p.is_fade      = r.boolean("is_fade", false);
    r.finish();
    if (const auto e = lo::validate_resize(p); !e.empty()) r.fail("x_percent", e);
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::resize_kernel(p, l); });
}

StepCode compile_move(Reader &r)
{
    lo::MoveParams p;
    p.selection     = read_selection(r);
    p.anchor        = r.choice("anchor", lo::Anchor::MiddleCenter, anchor_choices());
    p.margin_left   = r.integer("margin_left", 0, 0, 1000000);
    p.margin_top    = r.integer("margin_top", 0, 0, 1000000);
    p.margin_right  = r.integer("margin_right", 0, 0, 1000000);
    p.margin_bottom = r.integer("margin_bottom", 0, 0, 1000000);
    p.is_cut_move   = r.boolean("is_cut_move", true);
    r.finish();
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::move_kernel(p, l); });
}

StepCode compile_threshold(Reader &r)
{
    lo::ThresholdParams p;
    p.selection = read_selection(r);
    p.threshold = uint8_t(r.uint("threshold", 127, 255));
    p.maximum   = uint8_t(r.uint("maximum", 255, 255));
    p.type      = r.choice("type", lo::ThresholdType::Binary, threshold_choices());
    r.finish();
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::threshold_kernel(p, l); });
}

StepCode compile_solidify(Reader &r)
{
    lo::SolidifyParams p;
    p.selection    = read_selection(r);
    p.minimum_area = r.uint("minimum_area", 1, 100000000);
    p.area_check   = r.choice("area_check", lo::AreaCheck::More,
                              std::vector<std::pair<std::string, lo::AreaCheck>>{{"more", lo::AreaCheck::More}, {"less", lo::AreaCheck::Less}});
    r.finish();
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::solidify_kernel(p, l); });
}

StepCode compile_pattern(Reader &r)
{
    lo::PatternParams p;
    p.selection   = read_selection(r);
    p.anchor      = r.choice("anchor", lo::Anchor::None, anchor_choices());
    p.cols        = r.uint("cols", 1, 10000);
    p.rows        = r.uint("rows", 1, 10000);
    p.col_spacing = r.uint("col_spacing", 0, 1000000);
    p.row_spacing = r.uint("row_spacing", 0, 1000000);
    r.finish();
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::pattern_kernel(p, l); });
}

StepCode compile_pixel_dimming(Reader &r)
{
    lo::PixelDimmingParams p;
    p.selection            = read_selection(r);
    p.lightening_pixels    = r.boolean("lightening_pixels", false);
    p.wall_thickness_start = r.uint("wall_thickness_start", 10, 100000);
    p.wall_thickness_end   = r.uint("wall_thickness_end", 10, 100000);
    p.walls_only           = r.boolean("walls_only", false);
    p.chamfer              = r.boolean("chamfer", false);
    p.pattern_text         = r.string("pattern");
    p.alternate_pattern_text = r.string("alternate_pattern");
    p.alternate_pattern_per_layers = r.uint("alternate_pattern_per_layers", 1, 100000);
    r.finish();
    if (const auto e = lo::validate_pixel_dimming(p); !e.empty()) r.fail("pattern", e);
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::pixel_dimming_kernel(p, l); });
}

StepCode compile_pixel_arithmetic(Reader &r)
{
    lo::PixelArithmeticParams p;
    p.selection = read_selection(r);
    p.op        = r.choice("operator", lo::ArithmeticOperator::Set,
                           std::vector<std::pair<std::string, lo::ArithmeticOperator>>{
                               {"set", lo::ArithmeticOperator::Set}, {"add", lo::ArithmeticOperator::Add}, {"subtract", lo::ArithmeticOperator::Subtract},
                               {"multiply", lo::ArithmeticOperator::Multiply}, {"divide", lo::ArithmeticOperator::Divide},
                               {"minimum", lo::ArithmeticOperator::Minimum}, {"maximum", lo::ArithmeticOperator::Maximum},
                               {"bitwise_not", lo::ArithmeticOperator::BitwiseNot}, {"bitwise_and", lo::ArithmeticOperator::BitwiseAnd},
                               {"bitwise_or", lo::ArithmeticOperator::BitwiseOr}, {"bitwise_xor", lo::ArithmeticOperator::BitwiseXor},
                               {"abs_diff", lo::ArithmeticOperator::AbsDiff}, {"threshold", lo::ArithmeticOperator::Threshold},
                               {"keep_region", lo::ArithmeticOperator::KeepRegion}, {"discard_region", lo::ArithmeticOperator::DiscardRegion}});
    p.apply = r.choice("apply", lo::ArithmeticApply::Model,
                       std::vector<std::pair<std::string, lo::ArithmeticApply>>{
                           {"all", lo::ArithmeticApply::All}, {"model", lo::ArithmeticApply::Model}, {"model_surface", lo::ArithmeticApply::ModelSurface},
                           {"model_surface_and_inset", lo::ArithmeticApply::ModelSurfaceAndInset}, {"model_inner", lo::ArithmeticApply::ModelInner},
                           {"model_walls", lo::ArithmeticApply::ModelWalls}});
    p.wall_thickness_start = r.uint("wall_thickness_start", 20, 100000);
    p.wall_thickness_end   = r.uint("wall_thickness_end", 20, 100000);
    p.wall_chamfer         = r.boolean("wall_chamfer", false);
    p.value                = uint8_t(r.uint("value", 255, 255));
    p.value_step           = float(r.number("value_step", 0, -255, 255));
    p.threshold_type       = r.choice("threshold_type", lo::ThresholdType::Binary, threshold_choices());
    p.threshold_max        = uint8_t(r.uint("threshold_max", 255, 255));
    r.finish();
    if (const auto e = lo::validate_pixel_arithmetic(p); !e.empty()) r.fail("operator", e);
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::pixel_arithmetic_kernel(p, l); });
}

StepCode compile_light_bleed(Reader &r)
{
    lo::LightBleedParams p;
    p.selection = read_selection(r);
    p.subject   = r.choice("subject", lo::LightBleedSubject::Similarities,
                           std::vector<std::pair<std::string, lo::LightBleedSubject>>{{"similarities", lo::LightBleedSubject::Similarities},
                                                                                       {"bridges", lo::LightBleedSubject::Bridges},
                                                                                       {"both", lo::LightBleedSubject::Both}});
    p.lookup    = r.choice("lookup", lo::LightBleedLookup::Next,
                           std::vector<std::pair<std::string, lo::LightBleedLookup>>{{"previous", lo::LightBleedLookup::Previous},
                                                                                      {"next", lo::LightBleedLookup::Next},
                                                                                      {"both", lo::LightBleedLookup::Both}});
    p.dim_by    = r.string("dim_by", p.dim_by);
    r.finish();
    if (const auto e = lo::validate_light_bleed(p); !e.empty()) r.fail("dim_by", e);
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::light_bleed_kernel(p, l); });
}

StepCode compile_clone(Reader &r)
{
    lo::CloneParams p;
    p.first_layer = r.index("first_layer", 0);
    p.last_layer  = r.index("last_layer", lo::kLastLayer);
    p.clones      = r.uint("clones", 1, 1000);
    p.keep_same_z = r.boolean("keep_same_z", false);
    r.finish();
    if (p.last_layer != lo::kLastLayer && p.first_layer > p.last_layer) r.fail("first_layer", "must not be greater than last_layer");
    if (const auto e = lo::validate_clone(p); !e.empty()) r.fail("clones", e);
    return stream_step(p.first_layer, p.last_layer, [p](LayerDocument &) { return sla::liststream::clone(p); });
}

StepCode compile_remove(Reader &r)
{
    lo::RemoveParams p;
    p.first_layer      = r.index("first_layer", 0);
    p.last_layer       = r.index("last_layer", lo::kLastLayer);
    p.use_threshold    = r.boolean("use_threshold", false);
    p.pixel_threshold  = r.uint("pixel_threshold", 0, 1000000000);
    r.finish();
    if (p.last_layer != lo::kLastLayer && p.first_layer > p.last_layer) r.fail("first_layer", "must not be greater than last_layer");
    return stream_step(p.first_layer, p.last_layer, [p](LayerDocument &) { return sla::liststream::remove(p); });
}

lo::LayerParamEdit read_param_edit(Reader &r)
{
    lo::LayerParamEdit e;
    e.exposure_s         = r.opt_number("exposure_s", 0, 100000);
    e.wait_before_cure_s = r.opt_number("wait_before_cure_s", 0, 100000);
    e.wait_after_cure_s  = r.opt_number("wait_after_cure_s", 0, 100000);
    e.wait_after_lift_s  = r.opt_number("wait_after_lift_s", 0, 100000);
    e.lift_height_mm     = r.opt_number("lift_height_mm", 0, 1000);
    e.lift_speed         = r.opt_number("lift_speed", 0, 100000);
    e.lift_height2_mm    = r.opt_number("lift_height2_mm", 0, 1000);
    e.lift_speed2        = r.opt_number("lift_speed2", 0, 100000);
    e.retract_speed      = r.opt_number("retract_speed", 0, 100000);
    e.retract_height2_mm = r.opt_number("retract_height2_mm", 0, 1000);
    e.retract_speed2     = r.opt_number("retract_speed2", 0, 100000);
    if (const auto pwm = r.opt_integer("light_pwm", 0, 255)) e.light_pwm = uint8_t(*pwm);
    r.finish();
    return e;
}

// Global mode: "values" are written to the normal layers and "bottom" to the bottom layers.
// per_layer_override: "values" are written to every selected layer (set_layers on, skip_layers off, repeating).
StepCode compile_edit_parameters(Reader &r)
{
    lo::EditParametersParams p;
    p.first_layer         = r.index("first_layer", 0);
    p.last_layer          = r.index("last_layer", lo::kLastLayer);
    p.per_layer_override  = r.boolean("per_layer_override", false);
    p.set_layers          = r.uint("set_layers", 1, 1000000);
    p.skip_layers         = r.uint("skip_layers", 0, 1000000);
    if (auto v = r.object("values")) p.edit = read_param_edit(*v);
    if (auto b = r.object("bottom")) p.bottom = read_param_edit(*b);
    r.finish();
    if (p.last_layer != lo::kLastLayer && p.first_layer > p.last_layer) r.fail("first_layer", "must not be greater than last_layer");
    if (const auto e = lo::validate_edit_parameters(p); !e.empty()) r.fail("values", e);
    StepCode c;
    c.check = [p](Ctx &x) {
        check_range(x.document(), p.first_layer, p.last_layer);
        require_ok(x.document().parameter_operation_refusal());
    };
    c.run = [p](Ctx &x) {
        require_done(x.document().apply_parameter_operation(
            [&](std::vector<lo::Mask> &, std::vector<double> &z, std::vector<lo::LayerParams> &table, const LayerProgress &pr) {
                lo::edit_parameters(z, table, p, pr);
                return LayerDocument::LayerListChange{};
            },
            false, kFallbackLayerHeightMm, x.progress));
    };
    return c;
}

// ---- shared shapes of the remaining operations ------------------------------------------------------------
using Change = LayerDocument::LayerListChange;

size_t count_bottom(const std::vector<lo::LayerParams> &table)
{
    return size_t(std::count_if(table.begin(), table.end(), [](const lo::LayerParams &p) { return p.is_bottom; }));
}

// The first bottom / first normal layer of the table: what the GUI forms use as defaults.
const lo::LayerParams &first_bottom_of(const std::vector<lo::LayerParams> &table)
{
    static const lo::LayerParams empty;
    return table.empty() ? empty : table.front();
}
const lo::LayerParams &first_normal_of(const std::vector<lo::LayerParams> &table)
{
    for (const auto &p : table)
        if (!p.is_bottom) return p;
    return first_bottom_of(table);
}

// Same nominal layer height the layer editor uses for the re-height presets.
double nominal_layer_height(const std::vector<double> &z)
{
    if (z.size() < 2) return kFallbackLayerHeightMm;
    return std::max(0.001, z[z.size() / 2] - z[z.size() / 2 - 1]);
}

std::vector<size_t> identity_sources(size_t count, size_t extra_from_last = 0)
{
    std::vector<size_t> source(count);
    for (size_t i = 0; i < count; ++i) source[i] = i;
    for (size_t i = 0; i < extra_from_last; ++i) source.push_back(count - 1);
    return source;
}

// Whole-document pixel operation that also needs the Z of every layer (infill, repair without empty layer removal).
StepCode mask_step_z(size_t first, size_t last,
                     std::function<void(std::vector<lo::Mask> &, const std::vector<double> &, const LayerProgress &)> apply)
{
    StepCode c;
    c.check = [first, last](Ctx &x) { check_range(x.document(), first, last); };
    c.run   = [apply](Ctx &x) {
        const std::vector<double> z = layer_z(x.document());
        require_done(x.document().apply_mask_batch(
            LayerMaskPlane::Main, [&](std::vector<lo::Mask> &layers, const LayerProgress &p) { apply(layers, z, p); }, x.progress));
    };
    return c;
}

// Per-layer parameter operation (UVtools batch 2). `validate` (optional) is the operation's own validator evaluated
// against the current document by the check hook, so a dry run reports the same text a real run would.
using ParamValidate = std::function<std::string(const std::vector<double> &z, const std::vector<lo::LayerParams> &table, const lo::PrinterContext &)>;
using ParamFn       = std::function<Change(std::vector<lo::Mask> &, std::vector<double> &, std::vector<lo::LayerParams> &, const lo::PrinterContext &, const LayerProgress &)>;

StepCode parameter_step(size_t first, size_t last, bool needs_masks, bool changes_list, ParamValidate validate, ParamFn fn)
{
    StepCode c;
    c.changes_layer_list = changes_list;
    c.check = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        check_range(doc, first, last);
        if (changes_list) {
            require_ok(doc.layer_list_refusal(false));
            if (!doc.fresh_job())
                throw RuntimeError("layer-list parameter operations need a sliced or generated document: files opened from a container keep their layer count and heights in native headers");
        }
        require_ok(doc.parameter_operation_refusal());
        if (validate) require_ok(validate(layer_z(doc), doc.parameter_table(), doc.parameter_printer_context(layer_pitch_mm(doc))));
    };
    c.run = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        const double pitch = layer_pitch_mm(doc);
        const lo::PrinterContext context = doc.parameter_printer_context(pitch);
        const LayerDocument::ParameterOperation op = [&](std::vector<lo::Mask> &masks, std::vector<double> &z, std::vector<lo::LayerParams> &table, const LayerProgress &p) {
            return fn(masks, z, table, context, p);
        };
        require_done(changes_list ? doc.apply_parameter_list_operation(op, pitch, x.progress)
                                  : doc.apply_parameter_operation(op, needs_masks, pitch, x.progress));
    };
    return c;
}

// Streamed layer-list parameter operation (phased exposure, timelapse virtual layers, raise on finish): same checks as
// parameter_step; `make` builds the streamed operation from the current heights, table and printer context.
using ParamStreamFn = std::function<sla::liststream::Operation(const std::vector<double> &, const std::vector<lo::LayerParams> &, const lo::PrinterContext &)>;
StepCode parameter_stream_step(size_t first, size_t last, ParamValidate validate, ParamStreamFn make)
{
    StepCode c;
    c.changes_layer_list = true;
    c.check = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        check_range(doc, first, last);
        require_ok(doc.layer_list_refusal(false));
        if (!doc.fresh_job())
            throw RuntimeError("layer-list parameter operations need a sliced or generated document: files opened from a container keep their layer count and heights in native headers");
        require_ok(doc.parameter_operation_refusal());
        if (validate) require_ok(validate(layer_z(doc), doc.parameter_table(), doc.parameter_printer_context(layer_pitch_mm(doc))));
    };
    c.run = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        const double pitch = layer_pitch_mm(doc);
        const lo::PrinterContext context = doc.parameter_printer_context(pitch);
        require_done(doc.apply_parameter_list_stream(make(layer_z(doc), doc.parameter_table(), context), pitch, x.progress));
    };
    return c;
}

const std::vector<std::pair<std::string, lo::RotateDirection>> &rotate_choices()
{
    static const std::vector<std::pair<std::string, lo::RotateDirection>> table = {
        {"none", lo::RotateDirection::None}, {"rotate_90_clockwise", lo::RotateDirection::Rotate90Clockwise},
        {"rotate_180", lo::RotateDirection::Rotate180}, {"rotate_90_counter_clockwise", lo::RotateDirection::Rotate90CounterClockwise}};
    return table;
}

// "first_layer" / "last_layer" without a ROI (operations that have their own range semantics).
void read_range(Reader &r, size_t &first, size_t &last)
{
    first = r.index("first_layer", 0);
    last  = r.index("last_layer", lo::kLastLayer);
    if (last != lo::kLastLayer && first > last) r.fail("first_layer", "must not be greater than last_layer");
}

// ---- pixel operations: infill, raft relief, repair, layer arithmetic -------------------------------------
StepCode compile_infill(Reader &r)
{
    lo::InfillParams p;
    p.selection = read_selection(r);
    p.type      = r.choice("type", lo::InfillAlgorithm::CubicCrossAlternating,
                           std::vector<std::pair<std::string, lo::InfillAlgorithm>>{
                               {"pillars", lo::InfillAlgorithm::Pillars}, {"concentric", lo::InfillAlgorithm::Concentric}, {"waves", lo::InfillAlgorithm::Waves},
                               {"cubic_cross", lo::InfillAlgorithm::CubicCross}, {"cubic_cross_alternating", lo::InfillAlgorithm::CubicCrossAlternating},
                               {"cubic_star", lo::InfillAlgorithm::CubicStar}, {"honeycomb", lo::InfillAlgorithm::Honeycomb}, {"gyroid", lo::InfillAlgorithm::Gyroid}});
    p.floor_ceil_thickness_mm = r.number("floor_ceil_thickness_mm", p.floor_ceil_thickness_mm, 0, 100);
    p.wall_thickness     = r.uint("wall_thickness", p.wall_thickness, 100000);
    p.infill_thickness   = r.uint("infill_thickness", p.infill_thickness, 100000);
    p.infill_spacing     = r.uint("infill_spacing", p.infill_spacing, 100000);
    p.infill_brightness  = uint8_t(r.uint("infill_brightness", p.infill_brightness, 255));
    p.reinforce_infill   = r.boolean("reinforce_infill", false);
    r.finish();
    if (p.infill_thickness == 0) r.fail("infill_thickness", "must be greater than 0");
    if (p.infill_spacing == 0) r.fail("infill_spacing", "must be greater than 0");
    return kernel_step(p.selection.first_layer, p.selection.last_layer, [p](const lo::StreamLayout &l) { return lo::infill_kernel(p, l); }, true);
}

StepCode compile_raft_relief(Reader &r)
{
    lo::RaftReliefParams p;
    if (const auto roi = r.rect("roi")) p.roi = *roi;
    p.type = r.choice("type", lo::RaftReliefType::Relief,
                      std::vector<std::pair<std::string, lo::RaftReliefType>>{{"relief", lo::RaftReliefType::Relief}, {"dimming", lo::RaftReliefType::Dimming},
                                                                              {"decimate", lo::RaftReliefType::Decimate}, {"tabs", lo::RaftReliefType::Tabs}});
    p.mask_layer_index    = r.index("mask_layer_index", 0);
    p.ignore_first_layers = r.uint("ignore_first_layers", 0, 1000000);
    p.low_brightness      = uint8_t(r.uint("low_brightness", 0, 255));
    p.dilate_iterations   = r.uint("dilate_iterations", p.dilate_iterations, 1000);
    p.wall_margin         = r.uint("wall_margin", p.wall_margin, 100000);
    p.hole_diameter       = r.uint("hole_diameter", p.hole_diameter, 100000);
    p.hole_spacing        = r.uint("hole_spacing", p.hole_spacing, 100000);
    p.high_brightness     = uint8_t(r.uint("high_brightness", 255, 255));
    p.tab_triangle_base   = r.uint("tab_triangle_base", p.tab_triangle_base, 100000);
    p.tab_triangle_height = r.uint("tab_triangle_height", p.tab_triangle_height, 100000);
    r.finish();
    if (const auto e = lo::validate_raft_relief(p); !e.empty()) r.fail("type", e);
    return kernel_step(0, lo::kLastLayer, [p](const lo::StreamLayout &l) {
        // Nothing found = nothing changed; the edit is atomic, so the error leaves the document untouched.
        auto kernel = lo::raft_relief_kernel(p, l);
        if (!kernel) throw RuntimeError("no raft or support layers were found, nothing was changed");
        return *kernel;
    });
}

StepCode compile_layer_arithmetic(Reader &r)
{
    lo::LayerArithmeticParams p;
    p.sentence = r.required_string("sentence");
    if (const auto roi = r.rect("roi")) p.roi = *roi;
    r.finish();
    // Independent groups stream layer by layer; groups that feed each other need the whole document (batch).
    StepCode c = kernel_step(0, lo::kLastLayer, [p](const lo::StreamLayout &l) { return lo::layer_arithmetic_kernel(p, l); }, false,
        [p](auto &layers, const LayerProgress &pr) {
            require_ok(lo::validate_layer_arithmetic(p, layers.size()));
            lo::layer_arithmetic(layers, p, pr);
        });
    c.check = [p](Ctx &x) {
        check_range(x.document(), 0, lo::kLastLayer);
        require_ok(lo::validate_layer_arithmetic(p, x.document().layer_count())); // layer numbers are checked against the document
    };
    return c;
}

StepCode compile_repair_layers(Reader &r)
{
    lo::RepairLayersParams p;
    read_range(r, p.first_layer, p.last_layer);
    p.repair_islands       = r.boolean("repair_islands", true);
    p.repair_resin_traps   = r.boolean("repair_resin_traps", true);
    p.repair_suction_cups  = r.boolean("repair_suction_cups", false);
    // UVtools defaults this to true; here it is false because removing layers needs a document that can change its
    // layer list (sliced or generated), and a recipe should work on opened files too.
    p.remove_empty_layers  = r.boolean("remove_empty_layers", false);
    p.remove_islands_below_equal_pixels  = r.uint("remove_islands_below_equal_pixels", p.remove_islands_below_equal_pixels, 100000000);
    p.remove_islands_recursive_iterations = r.uint("remove_islands_recursive_iterations", p.remove_islands_recursive_iterations, 1000);
    p.attach_islands_below_layers = r.uint("attach_islands_below_layers", p.attach_islands_below_layers, 1000);
    p.resin_traps_overlap_by      = r.uint("resin_traps_overlap_by", p.resin_traps_overlap_by, 1000);
    p.suction_cups_vent_hole      = r.uint("suction_cups_vent_hole", p.suction_cups_vent_hole, 1000);
    p.gap_closing_iterations      = r.uint("gap_closing_iterations", p.gap_closing_iterations, 1000);
    p.noise_removal_iterations    = r.uint("noise_removal_iterations", p.noise_removal_iterations, 1000);
    r.finish();
    if (const auto e = lo::validate_repair_layers(p); !e.empty()) r.fail("repair_islands", e);
    if (p.remove_empty_layers)
        return stream_step(p.first_layer, p.last_layer, [p](LayerDocument &) { return sla::liststream::repair_layers(p); });
    // No layer is removed: a pixel operation that works on every document, streamed through the encoded layers.
    StepCode c;
    c.check = [p](Ctx &x) { check_range(x.document(), p.first_layer, p.last_layer); };
    c.run = [p](Ctx &x) {
        LayerDocument &doc = x.document();
        require_done(doc.apply_pixel_list_stream(sla::liststream::repair_layers(p), layer_pitch_mm(doc), x.progress));
    };
    return c;
}

// ---- layer-list operations: reheight, double exposure, change resolution ----------------------------------
StepCode compile_reheight(Reader &r)
{
    const auto method = r.choice("method", lo::ReHeightMethod::ReHeight,
                                 std::vector<std::pair<std::string, lo::ReHeightMethod>>{{"reheight", lo::ReHeightMethod::ReHeight},
                                                                                         {"offset_position_z", lo::ReHeightMethod::OffsetPositionZ}});
    const auto height = r.opt_number("layer_height_mm", 0.001, 1);
    const auto aa     = r.choice("anti_aliasing", lo::ReHeightAntiAliasing::None,
                                 std::vector<std::pair<std::string, lo::ReHeightAntiAliasing>>{{"none", lo::ReHeightAntiAliasing::None},
                                                                                               {"difference", lo::ReHeightAntiAliasing::Difference},
                                                                                               {"average", lo::ReHeightAntiAliasing::Average}});
    const double offset = r.number("position_z_offset_mm", 0.01, -1000, 1000);
    size_t first, last;
    read_range(r, first, last);
    r.finish();
    if (method == lo::ReHeightMethod::ReHeight && !height) r.fail("layer_height_mm", "is required for method 'reheight' (the new layer height in mm)");

    // The presets (exact split / merge factors of the current layer count and height) are only known at run time.
    auto resolve = [=](size_t count, const std::vector<double> &z) {
        lo::ReHeightParams q;
        q.method = method; q.anti_aliasing = aa; q.position_z_offset_mm = offset; q.first_layer = first; q.last_layer = last;
        if (method != lo::ReHeightMethod::ReHeight) return q;
        const auto presets = lo::reheight_presets(count, nominal_layer_height(z));
        std::string available;
        for (const auto &s : presets) {
            if (std::abs(s.layer_height_mm - *height) < 0.0005) { q.preset = s; return q; }
            available += (available.empty() ? "" : ", ") + fmt(s.layer_height_mm) + (s.is_multiply ? " (merge x" : " (split x") + std::to_string(unsigned(s.modifier)) + ")";
        }
        throw RuntimeError("the layer height " + fmt(*height) + " mm cannot be reached exactly from " + std::to_string(count) + " layers of " +
                           fmt(nominal_layer_height(z)) + " mm; available: " + (available.empty() ? "none" : available));
    };
    StepCode c = stream_step(first, last, [resolve](LayerDocument &doc) {
        return sla::liststream::reheight(resolve(doc.layer_count(), layer_z(doc)));
    });
    const auto base_check = c.check;
    c.check = [=](Ctx &x) {
        base_check(x);
        const std::vector<double> z = layer_z(x.document());
        const std::vector<lo::Mask> stub(x.document().layer_count()); // the validator only looks at the layer count
        require_ok(lo::validate_reheight(resolve(stub.size(), z), stub, z));
    };
    return c;
}

StepCode compile_double_exposure(Reader &r)
{
    lo::DoubleExposureParams p;
    read_range(r, p.first_layer, p.last_layer);
    const auto bottom_count = r.opt_integer("bottom_layer_count", 0, 100000000);
    const auto first_bottom = r.opt_number("first_bottom_exposure_s", 0, 100000), first_normal = r.opt_number("first_normal_exposure_s", 0, 100000);
    const auto second_bottom = r.opt_number("second_bottom_exposure_s", 0, 100000), second_normal = r.opt_number("second_normal_exposure_s", 0, 100000);
    p.first_bottom_erode  = r.uint("first_bottom_erode", p.first_bottom_erode, 1000);
    p.second_bottom_erode = r.uint("second_bottom_erode", p.second_bottom_erode, 1000);
    p.first_normal_erode  = r.uint("first_normal_erode", p.first_normal_erode, 1000);
    p.second_normal_erode = r.uint("second_normal_erode", p.second_normal_erode, 1000);
    p.second_layer_difference = r.boolean("second_layer_difference", true);
    p.second_layer_difference_overlap_erode = r.uint("second_layer_difference_overlap_erode", p.second_layer_difference_overlap_erode, 1000);
    p.different_settings_for_second_layer = r.boolean("different_settings_for_second_layer", true);
    p.second_layer_lift_enabled = r.boolean("second_layer_lift_enabled", true);
    p.second_layer_lift_mm      = r.number("second_layer_lift_mm", p.second_layer_lift_mm, 0, 1000);
    p.second_layer_wait_enabled = r.boolean("second_layer_wait_enabled", true);
    p.second_layer_wait_s       = r.number("second_layer_wait_s", p.second_layer_wait_s, 0, 100000);
    r.finish();

    // Omitted bottom count / exposures: the document's own (UVtools InitWithSlicerFile). Documents without a
    // parameter table have no such values, so there they must be given.
    auto resolve = [=](LayerDocument &doc) {
        lo::DoubleExposureParams q = p;
        const auto table = doc.parameter_table();
        if (!table.empty()) {
            q.bottom_layer_count = bottom_count ? size_t(*bottom_count) : count_bottom(table);
            q.first_bottom_exposure_s  = first_bottom.value_or(first_bottom_of(table).exposure_s);
            q.first_normal_exposure_s  = first_normal.value_or(first_normal_of(table).exposure_s);
            q.second_bottom_exposure_s = second_bottom.value_or(q.first_bottom_exposure_s);
            q.second_normal_exposure_s = second_normal.value_or(q.first_normal_exposure_s);
        } else {
            if (!first_bottom || !first_normal)
                throw RuntimeError("this document stores no exposure values: give first_bottom_exposure_s and first_normal_exposure_s");
            q.bottom_layer_count = bottom_count ? size_t(*bottom_count) : 0;
            q.first_bottom_exposure_s = *first_bottom; q.first_normal_exposure_s = *first_normal;
            q.second_bottom_exposure_s = second_bottom.value_or(*first_bottom);
            q.second_normal_exposure_s = second_normal.value_or(*first_normal);
        }
        return q;
    };
    StepCode c;
    c.changes_layer_list = true;
    c.check = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        check_range(doc, p.first_layer, p.last_layer);
        require_ok(doc.layer_list_refusal(false));
        require_ok(lo::validate_double_exposure(resolve(doc), layer_z(doc)));
    };
    c.run = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        const lo::DoubleExposureParams q = resolve(doc);
        require_done(doc.apply_layer_list_stream(sla::liststream::double_exposure(q), false, layer_pitch_mm(doc), x.progress));
    };
    return c;
}

StepCode compile_change_resolution(Reader &r)
{
    lo::ChangeResolutionParams p;
    p.new_width  = r.uint("new_width", 0, 100000);
    p.new_height = r.uint("new_height", 0, 100000);
    if (!r.has("new_width")) r.fail("new_width", "is required");
    if (!r.has("new_height")) r.fail("new_height", "is required");
    p.fix_ratio = r.boolean("fix_ratio", false);
    p.ratio_x   = r.number("ratio_x", 1.0, 0.001, 1000);
    p.ratio_y   = r.number("ratio_y", 1.0, 0.001, 1000);
    r.finish();
    if (p.new_width == 0) r.fail("new_width", "must be greater than 0");
    if (p.new_height == 0) r.fail("new_height", "must be greater than 0");
    StepCode c = stream_step(0, lo::kLastLayer, [p](LayerDocument &) { return sla::liststream::change_resolution(p); }, true);
    return c;
}

// ---- parameter operations --------------------------------------------------------------------------------
StepCode compile_dynamic_lifts(Reader &r)
{
    lo::DynamicLiftsParams p;
    read_range(r, p.first_layer, p.last_layer);
    p.method = r.choice("method", lo::DynamicLiftsMethod::Traditional,
                        std::vector<std::pair<std::string, lo::DynamicLiftsMethod>>{{"traditional", lo::DynamicLiftsMethod::Traditional},
                                                                                    {"full_range", lo::DynamicLiftsMethod::FullRange}});
    // 0 / omitted = the value of the first bottom / normal layer (UVtools InitWithSlicerFile).
    p.smallest_bottom_lift_height = r.number("smallest_bottom_lift_height", 0, 0, 1000);
    p.largest_bottom_lift_height  = r.number("largest_bottom_lift_height", 0, 0, 1000);
    p.smallest_lift_height        = r.number("smallest_lift_height", 0, 0, 1000);
    p.largest_lift_height         = r.number("largest_lift_height", 0, 0, 1000);
    p.slowest_bottom_lift_speed   = r.number("slowest_bottom_lift_speed", 0, 0, 100000);
    p.fastest_bottom_lift_speed   = r.number("fastest_bottom_lift_speed", 0, 0, 100000);
    p.slowest_lift_speed          = r.number("slowest_lift_speed", 0, 0, 100000);
    p.fastest_lift_speed          = r.number("fastest_lift_speed", 0, 0, 100000);
    r.finish();
    auto ordered = [&](double smallest, double largest, const char *key, const char *what) {
        if (smallest > 0 && largest > 0 && smallest > largest) r.fail(key, std::string("must not be lower than the ") + what);
    };
    ordered(p.smallest_bottom_lift_height, p.largest_bottom_lift_height, "largest_bottom_lift_height", "smallest bottom lift height");
    ordered(p.smallest_lift_height, p.largest_lift_height, "largest_lift_height", "smallest lift height");
    ordered(p.slowest_bottom_lift_speed, p.fastest_bottom_lift_speed, "fastest_bottom_lift_speed", "slowest bottom lift speed");
    ordered(p.slowest_lift_speed, p.fastest_lift_speed, "fastest_lift_speed", "slowest lift speed");
    // The lifts only need the number of lit pixels of every layer: counted with a scan (one layer decoded at a time),
    // never the whole document decoded. Parameter-only change, so it works on every document.
    StepCode c = parameter_step(
        p.first_layer, p.last_layer, false, false,
        [p](const auto &, const auto &table, const auto &) { return lo::validate_dynamic_lifts(lo::dynamic_lifts_defaults(table, p)); },
        {});
    c.run = [p](Ctx &x) {
        LayerDocument &doc = x.document();
        require_done(sla::liststream::apply_dynamic_lifts(doc, p, layer_pitch_mm(doc), x.progress));
    };
    return c;
}

StepCode compile_fade_exposure(Reader &r)
{
    lo::FadeExposureParams p;
    p.first_layer = r.index("first_layer", 0);
    p.layer_count = r.index("layer_count", p.layer_count);
    const auto from = r.opt_number("from_exposure_s", 0, 100000), to = r.opt_number("to_exposure_s", 0, 100000);
    p.disable_firmware_transition_layers = r.boolean("disable_firmware_transition_layers", true);
    r.finish();
    if (p.layer_count == 0) r.fail("layer_count", "must be greater than 0");
    auto resolve = [=](const std::vector<lo::LayerParams> &table) {
        lo::FadeExposureParams q = p;
        q.from_exposure_s = from.value_or(first_bottom_of(table).exposure_s); // UVtools: the bottom exposure fades into the normal one
        q.to_exposure_s   = to.value_or(first_normal_of(table).exposure_s);
        q.bottom_layer_count = count_bottom(table);
        return q;
    };
    // The transition layer count that UVtools may update is a global header value; the document does not expose it
    // to parameter edits, so only the per-layer exposures are written.
    return parameter_step(
        p.first_layer, lo::kLastLayer, false, false,
        [resolve](const auto &, const auto &table, const auto &) { return lo::validate_fade_exposure(resolve(table), table.size()); },
        [resolve](auto &, auto &, auto &table, const auto &, const LayerProgress &pr) {
            lo::fade_exposure_time(table, resolve(table), pr);
            return Change{};
        });
}

StepCode compile_phased_exposure(Reader &r)
{
    lo::PhasedExposureParams p;
    read_range(r, p.first_layer, p.last_layer);
    std::vector<lo::PhasedExposureLevel> phases; // empty = the UVtools defaults of the document
    if (const Json *list = r.find("phases")) {
        if (!list->is_array() || list->size() < 2 || list->size() > 3) r.fail("phases", "expected a list of 2 or 3 phases");
        for (size_t i = 0; i < list->size(); ++i) {
            Reader item((*list)[i], r.at("phases") + "[" + std::to_string(i) + "]");
            lo::PhasedExposureLevel level;
            level.bottom_exposure_s = item.number("bottom_exposure_s", 0, 0, 100000);
            level.bottom_iterations = item.uint("bottom_iterations", 0, 1000);
            level.exposure_s        = item.number("exposure_s", 0, 0, 100000);
            level.iterations        = item.uint("iterations", 0, 1000);
            if (!item.has("exposure_s")) item.fail("exposure_s", "is required");
            if (!item.has("bottom_exposure_s")) item.fail("bottom_exposure_s", "is required");
            item.finish();
            phases.push_back(level);
        }
    }
    const auto difference_only = r.opt_bool("difference_only");
    const auto overlap = r.opt_integer("difference_overlap_iterations", 0, 1000);
    const auto sequential = r.opt_bool("different_settings_for_sequential_layers");
    const auto lift_enabled = r.opt_bool("sequential_lift_enabled");
    const auto lift_mm = r.opt_number("sequential_lift_mm", 0, 1000);
    const auto wait_enabled = r.opt_bool("sequential_wait_before_cure_enabled");
    const auto wait_s = r.opt_number("sequential_wait_before_cure_s", 0, 100000);
    r.finish();
    // Omitted values take the same defaults as the layer editor form (they depend on the first bottom / normal layer
    // and on whether the printer takes G-code).
    auto resolve = [=](const std::vector<lo::LayerParams> &table, const lo::PrinterContext &context) {
        lo::PhasedExposureParams q = lo::phased_exposure_defaults(first_bottom_of(table), first_normal_of(table), context.is_gcode);
        q.first_layer = p.first_layer; q.last_layer = p.last_layer;
        if (!phases.empty()) q.phases = phases;
        if (difference_only) q.difference_only = *difference_only;
        if (overlap) q.difference_overlap_iterations = unsigned(*overlap);
        if (sequential) q.different_settings_for_sequential_layers = *sequential;
        if (lift_enabled) q.sequential_lift_enabled = *lift_enabled;
        if (lift_mm) q.sequential_lift_mm = *lift_mm;
        if (wait_enabled) q.sequential_wait_before_cure_enabled = *wait_enabled;
        if (wait_s) q.sequential_wait_before_cure_s = *wait_s;
        return q;
    };
    return parameter_stream_step(
        p.first_layer, p.last_layer,
        [resolve](const auto &z, const auto &table, const auto &context) { return lo::validate_phased_exposure(resolve(table, context), z); },
        [resolve](const auto &, const auto &table, const auto &context) { return sla::liststream::phased_exposure(resolve(table, context), context); });
}

StepCode compile_raise_on_print_finish(Reader &r)
{
    lo::RaiseOnPrintFinishParams p;
    p.position_z_mm      = r.number("position_z_mm", 0, 0, 10000);
    p.wait_time_s        = r.number("wait_time_s", p.wait_time_s, 0, 100000);
    p.output_dummy_pixel = r.boolean("output_dummy_pixel", true);
    r.finish();
    return parameter_stream_step(
        0, lo::kLastLayer,
        [p](const auto &z, const auto &, const auto &context) { return lo::validate_raise_on_print_finish(p, z, context); },
        [p](const auto &, const auto &, const auto &context) { return sla::liststream::raise_on_print_finish(p, context); });
}

StepCode compile_timelapse(Reader &r)
{
    lo::TimelapseParams p;
    read_range(r, p.first_layer, p.last_layer);
    p.mode = r.choice("mode", lo::TimelapseMode::LiftHeight,
                      std::vector<std::pair<std::string, lo::TimelapseMode>>{{"lift_height", lo::TimelapseMode::LiftHeight},
                                                                             {"virtual_layer", lo::TimelapseMode::VirtualLayer}});
    p.raise_position_z_mm      = r.number("raise_position_z_mm", 0, 0, 10000);
    p.output_dummy_pixel       = r.boolean("output_dummy_pixel", true);
    p.raise_each_nth_height_mm = r.number("raise_each_nth_height_mm", p.raise_each_nth_height_mm, 0.01, 1000);
    p.wait_after_lift_s        = r.number("wait_after_lift_s", p.wait_after_lift_s, 0, 100000);
    p.exposure_s               = r.number("exposure_s", 0, 0, 100000);
    p.ensure_last_layer        = r.boolean("ensure_last_layer", true);
    p.use_custom_lift          = r.boolean("use_custom_lift", false);
    p.slow_lift_height_mm      = r.number("slow_lift_height_mm", p.slow_lift_height_mm, 0, 1000);
    const auto lift_speed = r.opt_number("lift_speed", 0, 100000), lift_speed2 = r.opt_number("lift_speed2", 0, 100000);
    p.slow_retract_height_mm   = r.number("slow_retract_height_mm", p.slow_retract_height_mm, 0, 1000);
    const auto retract_speed = r.opt_number("retract_speed", 0, 100000), retract_speed2 = r.opt_number("retract_speed2", 0, 100000);
    r.finish();
    // Omitted speeds: those of the first normal layer (only used with use_custom_lift).
    auto resolve = [=](const std::vector<lo::LayerParams> &table) {
        lo::TimelapseParams q = p;
        const lo::LayerParams &n = first_normal_of(table);
        q.lift_speed     = lift_speed.value_or(n.lift_speed);
        q.lift_speed2    = lift_speed2.value_or(n.lift_speed2);
        q.retract_speed  = retract_speed.value_or(n.retract_speed);
        q.retract_speed2 = retract_speed2.value_or(n.retract_speed2);
        return q;
    };
    const bool changes_list = p.mode == lo::TimelapseMode::VirtualLayer;
    // A document does not know the machine height, so "0 = machine Z" would silently raise nothing.
    auto validate = [resolve](const std::vector<double> &z, const std::vector<lo::LayerParams> &table, const lo::PrinterContext &context) -> std::string {
        const lo::TimelapseParams q = resolve(table);
        if (q.raise_position_z_mm <= 0 && context.machine_z_mm <= 0)
            return "the machine height of this document is unknown: give raise_position_z_mm";
        return lo::validate_timelapse(q, z, context);
    };
    if (changes_list)
        return parameter_stream_step(
            p.first_layer, p.last_layer, validate,
            [resolve](const auto &, const auto &table, const auto &context) { return sla::liststream::timelapse(resolve(table), context); });
    // Lift height mode only edits the parameter table: no layer is decoded.
    return parameter_step(
        p.first_layer, p.last_layer, false, false, validate,
        [resolve, validate](auto &, auto &z, auto &table, const auto &context, const LayerProgress &pr) {
            require_ok(validate(z, table, context));
            lo::TimelapseVirtualLayer unused;
            lo::timelapse_plan(z.size(), 0, 0, z, table, resolve(table), context, [](int &, int &) {}, unused, pr);
            return Change{};
        });
}

StepCode compile_dynamic_layer_height(Reader &r)
{
    lo::DynamicLayerHeightParams p;
    read_range(r, p.first_layer, p.last_layer);
    const auto minimum = r.opt_number("minimum_layer_height_mm", 0.001, 10), maximum = r.opt_number("maximum_layer_height_mm", 0.001, 10);
    const auto strip = r.opt_bool("strip_anti_aliasing"), reconstruct = r.opt_bool("reconstruct_anti_aliasing");
    const auto erodes = r.opt_integer("maximum_erodes", 0, 1000);
    p.exposure_type = r.choice("exposure_type", lo::ExposureSetType::Linear,
                               std::vector<std::pair<std::string, lo::ExposureSetType>>{{"linear", lo::ExposureSetType::Linear},
                                                                                        {"multiplier", lo::ExposureSetType::Multiplier},
                                                                                        {"manual", lo::ExposureSetType::Manual}});
    p.iterate_bottom_exposure = r.boolean("iterate_bottom_exposure", false);
    const auto bottom_exposure = r.opt_number("bottom_exposure_s", 0, 100000), exposure = r.opt_number("exposure_s", 0, 100000);
    const auto bottom_step = r.opt_number("bottom_exposure_step_s", 0, 100000), step = r.opt_number("exposure_step_s", 0, 100000);
    if (const Json *list = r.find("manual_table")) {
        if (!list->is_array()) r.fail("manual_table", "expected a list of {layer_height_mm, bottom_exposure_s, exposure_s}");
        for (size_t i = 0; i < list->size(); ++i) {
            Reader item((*list)[i], r.at("manual_table") + "[" + std::to_string(i) + "]");
            lo::ExposureItem e;
            e.layer_height_mm   = item.number("layer_height_mm", 0, 0.001, 10);
            e.bottom_exposure_s = item.number("bottom_exposure_s", 0, 0, 100000);
            e.exposure_s        = item.number("exposure_s", 0, 0, 100000);
            for (const char *key : {"layer_height_mm", "bottom_exposure_s", "exposure_s"})
                if (!item.has(key)) item.fail(key, "is required");
            item.finish();
            p.manual_table.push_back(e);
        }
    }
    r.finish();
    if (p.exposure_type == lo::ExposureSetType::Manual && p.manual_table.empty()) r.fail("manual_table", "is required for exposure_type 'manual'");
    // Omitted values: the defaults of the layer editor form (UVtools InitWithSlicerFile).
    auto resolve = [=](const std::vector<lo::LayerParams> &table, const lo::PrinterContext &context) {
        lo::DynamicLayerHeightParams q = lo::dynamic_layer_height_defaults(table, context);
        q.first_layer = p.first_layer; q.last_layer = p.last_layer;
        q.exposure_type = p.exposure_type; q.iterate_bottom_exposure = p.iterate_bottom_exposure; q.manual_table = p.manual_table;
        if (minimum) q.minimum_layer_height_mm = *minimum;
        if (maximum) q.maximum_layer_height_mm = *maximum;
        if (strip) q.strip_anti_aliasing = *strip;
        if (reconstruct) q.reconstruct_anti_aliasing = *reconstruct;
        if (erodes) q.maximum_erodes = unsigned(*erodes);
        if (bottom_exposure) q.bottom_exposure_s = *bottom_exposure;
        if (exposure) q.exposure_s = *exposure;
        if (bottom_step) q.bottom_exposure_step_s = *bottom_step;
        if (step) q.exposure_step_s = *step;
        return q;
    };
    // Streamed: stacks are built layer by layer from the encoded document, unchanged layers keep their payload.
    return parameter_stream_step(
        p.first_layer, p.last_layer,
        [resolve](const auto &z, const auto &table, const auto &context) { return lo::validate_dynamic_layer_height(resolve(table, context), z, context); },
        [resolve](const std::vector<double> &, const std::vector<lo::LayerParams> &table, const lo::PrinterContext &context) {
            return sla::liststream::dynamic_layer_height(resolve(table, context), context);
        });
}

// A picture for the lithophane: 8-bit grey PNG, or a true colour PNG converted to grey.
lo::Mask read_grey_png(const fs::path &file)
{
    if (!fs::is_regular_file(file)) throw RuntimeError("the image '" + file.string() + "' does not exist");
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const png::ReadBuf buffer{bytes.data(), bytes.size()};
    if (bytes.empty() || !png::is_png(buffer)) throw RuntimeError("the image '" + file.string() + "' is not a PNG file (only PNG pictures are supported)");
    png::ImageGreyscale image;
    if (!png::decode_png(buffer, image) && !png::decode_color_png(buffer, image))
        throw RuntimeError("the image '" + file.string() + "' cannot be decoded (8-bit grey or true colour PNG expected)");
    lo::Mask mask(image.cols, image.rows);
    mask.px = std::move(image.buf);
    return mask;
}

StepCode compile_lithophane(Reader &r)
{
    const std::string image_path = r.required_string("image");
    lo::LithophaneParams p;
    const auto layer_height = r.opt_number("layer_height_mm", 0.001, 1);
    const auto bottom_layers = r.opt_integer("bottom_layer_count", 0, 1000000);
    const auto bottom_exposure = r.opt_number("bottom_exposure_s", 0, 100000), normal_exposure = r.opt_number("normal_exposure_s", 0, 100000);
    p.rotate                = r.choice("rotate", lo::RotateDirection::None, rotate_choices());
    p.mirror                = r.boolean("mirror", false);
    p.invert_color          = r.boolean("invert_color", false);
    p.resize_factor_percent = r.number("resize_factor_percent", 100, 1, 1000);
    p.enhance_contrast      = r.boolean("enhance_contrast", false);
    p.brightness_gain       = r.integer("brightness_gain", 0, -127, 127);
    p.gap_closing_iterations   = r.uint("gap_closing_iterations", 0, 1000);
    p.remove_noise_iterations  = r.uint("remove_noise_iterations", 0, 1000);
    p.gaussian_blur            = r.uint("gaussian_blur", 0, 1000);
    p.separate_grayscale_pixels = r.boolean("separate_grayscale_pixels", true);
    p.start_threshold = r.uint("start_threshold", p.start_threshold, 255);
    p.end_threshold   = r.uint("end_threshold", p.end_threshold, 255);
    p.base_thickness_mm = r.number("base_thickness_mm", p.base_thickness_mm, 0, 100);
    p.base_type = r.choice("base_type", lo::LithophaneBase::Square,
                           std::vector<std::pair<std::string, lo::LithophaneBase>>{{"none", lo::LithophaneBase::None}, {"square", lo::LithophaneBase::Square},
                                                                                   {"model", lo::LithophaneBase::Model}});
    p.base_margin_px          = r.uint("base_margin_px", p.base_margin_px, 10000);
    p.lithophane_height_mm    = r.number("lithophane_height_mm", p.lithophane_height_mm, 0.01, 100);
    p.one_layer_per_threshold = r.boolean("one_layer_per_threshold", false);
    p.anti_aliasing           = r.boolean("anti_aliasing", true);
    r.finish();

    // Omitted layer height / bottom count / exposures: the document's own (as the layer editor form does).
    auto resolve = [=](const std::vector<lo::LayerParams> &table, const lo::PrinterContext &context) {
        lo::LithophaneParams q = p;
        q.layer_height_mm    = layer_height.value_or(context.layer_height_mm > 0 ? context.layer_height_mm : 0.05);
        const size_t bottom  = count_bottom(table);
        q.bottom_layer_count = bottom_layers ? size_t(*bottom_layers) : bottom ? bottom : 4;
        q.bottom_exposure_s  = bottom_exposure.value_or(first_bottom_of(table).exposure_s > 0 ? first_bottom_of(table).exposure_s : 30);
        q.normal_exposure_s  = normal_exposure.value_or(first_normal_of(table).exposure_s > 0 ? first_normal_of(table).exposure_s : 2);
        return q;
    };
    StepCode c;
    c.changes_layer_list = true;
    c.check = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        require_ok(doc.layer_list_refusal(false)); // the lithophane REPLACES the layer list
        if (!doc.fresh_job())
            throw RuntimeError("the lithophane needs a sliced or generated document (printer geometry); files opened from a container keep their layer list in native headers");
        require_ok(doc.parameter_operation_refusal());
        const lo::Mask image = read_grey_png(x.resolve(image_path));
        const auto first = doc.image(0);
        require_ok(lo::validate_lithophane(resolve(doc.parameter_table(), doc.parameter_printer_context(layer_pitch_mm(doc))), image, size_t(first.cols), size_t(first.rows)));
    };
    c.run = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        const lo::Mask image = read_grey_png(x.resolve(image_path));
        const auto first = doc.image(0);
        const size_t width = size_t(first.cols), height = size_t(first.rows);
        const double pitch = layer_pitch_mm(doc);
        const lo::PrinterContext context = doc.parameter_printer_context(pitch);
        const lo::LithophaneParams q = resolve(doc.parameter_table(), context);
        require_ok(lo::validate_lithophane(q, image, width, height));
        const auto table = doc.parameter_table();
        require_done(doc.apply_parameter_list_stream(
            sla::liststream::lithophane(image, width, height, q, context, first_bottom_of(table), first_normal_of(table)), pitch, x.progress));
        x.message = "replaced the layers by a lithophane of " + std::to_string(doc.layer_count()) + " layers";
    };
    return c;
}

// ---- issue detection and suggestions ----------------------------------------------------------------------
const char *issue_type_name(LayerIssue::Type type)
{
    switch (type) {
    case LayerIssue::Type::Island:        return "island";
    case LayerIssue::Type::Overhang:      return "overhang";
    case LayerIssue::Type::ResinTrap:     return "resin_trap";
    case LayerIssue::Type::SuctionCup:    return "suction_cup";
    case LayerIssue::Type::TouchingBound: return "touching_bound";
    case LayerIssue::Type::PrintHeight:   return "print_height";
    case LayerIssue::Type::EmptyLayer:    return "empty_layer";
    }
    return "unknown";
}

Json bbox_json(const BoundingBox &box)
{
    if (!box.defined) return nullptr;
    return {{"x", box.min.x()}, {"y", box.min.y()}, {"width", box.max.x() - box.min.x() + 1}, {"height", box.max.y() - box.min.y() + 1}};
}

void write_text(Ctx &x, const std::string &path, const std::string &text)
{
    const std::string staged = x.stage(path, false);
    boost::nowide::ofstream file(staged, std::ios::binary);
    file << text;
    file.close();
    if (!file) throw RuntimeError("cannot write " + path);
}

StepCode compile_detect_issues(Reader &r)
{
    IssueDetectionConfig config;
    const std::string output = r.string("output");
    const bool fail_on_issues = r.boolean("fail_on_issues", false);
    std::set<std::string> disabled;
    if (const Json *list = r.find("disable")) {
        if (!list->is_array()) r.fail("disable", "expected a list of issue names");
        const std::set<std::string> known = {"island", "overhang", "resin_trap", "suction_cup", "touching_bound", "print_height", "empty_layer"};
        for (const auto &item : *list) {
            if (!item.is_string() || !known.count(item.get<std::string>()))
                r.fail("disable", "allowed names: island, overhang, resin_trap, suction_cup, touching_bound, print_height, empty_layer");
            disabled.insert(item.get<std::string>());
        }
    }
    config.island.required_area_to_process_check_px = uint16_t(r.uint("island_required_area", config.island.required_area_to_process_check_px, 65535));
    config.island.required_pixels_to_support_multiplier = r.number("island_support_percent", config.island.required_pixels_to_support_multiplier * 100., 0, 1000) / 100.;
    config.overhang.erode_iterations = uint8_t(r.uint("overhang_erode", config.overhang.erode_iterations, 255));
    config.resin_trap.required_area_to_process_check_px = uint8_t(r.uint("resin_trap_required_area", config.resin_trap.required_area_to_process_check_px, 255));
    config.resin_trap.required_height_to_consider_suction_cup_mm = r.number("suction_cup_height_mm", config.resin_trap.required_height_to_consider_suction_cup_mm, 0, 1000);
    const auto margin = uint8_t(r.uint("touching_margin", config.touching_bound.margin_left_px, 255));
    config.touching_bound.margin_left_px = config.touching_bound.margin_top_px = config.touching_bound.margin_right_px = config.touching_bound.margin_bottom_px = margin;
    config.print_height.machine_height_mm = r.number("machine_height_mm", 0, 0, 100000);
    r.finish();
    config.island.enabled                 = !disabled.count("island");
    config.overhang.enabled               = !disabled.count("overhang");
    config.resin_trap.enabled             = !disabled.count("resin_trap");
    config.resin_trap.detect_suction_cups = !disabled.count("suction_cup");
    config.touching_bound.enabled         = !disabled.count("touching_bound");
    config.print_height.enabled           = !disabled.count("print_height");
    config.empty_layer.enabled            = !disabled.count("empty_layer");

    StepCode c;
    c.check = [output](Ctx &x) {
        if (x.document().layer_count() == 0) throw RuntimeError("the document has no layers");
        if (!output.empty()) x.check_output(x.output_path(output), false);
    };
    c.run = [config, output, fail_on_issues](Ctx &x) {
        LayerDocument &doc = x.document();
        const LayerMaskProvider provider = [&doc](size_t index) {
            auto image = doc.image(index);
            LayerMask mask;
            mask.width = size_t(image.cols); mask.height = size_t(image.rows); mask.data = std::move(image.buf);
            return mask;
        };
        std::atomic<bool> stop{false};
        const auto issues = detect_layer_issues(
            provider, layer_z(doc), config,
            [&](size_t done, size_t total) { if (x.progress && !x.progress(done, total)) stop = true; },
            [&] { return stop.load(); });
        if (stop) throw LayerOperationCancelled{};
        Json report = {{"layer_count", doc.layer_count()}, {"issue_count", issues.size()}, {"issues", Json::array()}};
        for (const auto &issue : issues) {
            Json parts = Json::array();
            for (const auto &part : issue.parts) parts.push_back({{"layer", part.layer}, {"pixel_count", part.pixel_count}, {"bbox", bbox_json(part.bbox)}});
            report["issues"].push_back({{"type", issue_type_name(issue.type)}, {"first_layer", issue.first_layer}, {"last_layer", issue.last_layer},
                                        {"pixel_count", issue.pixel_count}, {"bbox", bbox_json(issue.bbox)}, {"parts", parts}});
        }
        if (!output.empty()) write_text(x, x.output_path(output), report.dump(2) + "\n");
        x.message = std::to_string(issues.size()) + " issue(s) in " + std::to_string(doc.layer_count()) + " layers";
        if (fail_on_issues && !issues.empty()) throw RuntimeError(x.message + " (fail_on_issues is set)");
    };
    return c;
}

// The job description the suggestion evaluators need, from what the document stores. Values the container does
// not hold stay absent (the evaluator then reports "unavailable") instead of being invented.
SuggestionJob build_suggestion_job(LayerDocument &doc)
{
    SuggestionJob job;
    const size_t count = doc.layer_count();
    job.z_mm = layer_z(doc);
    const auto job_parameters = doc.job_parameters();
    const auto geometry       = doc.raster_geometry();
    job.layer_height_mm        = layer_pitch_mm(doc);
    job.bottom_layer_count     = job_parameters ? job_parameters->bottom_layers : 0;
    job.transition_layer_count = job_parameters ? job_parameters->transition_layers : 0;
    // Sliced / generated documents keep these globals in their layer job, not in a native header.
    if (const auto &fresh = doc.fresh_job()) {
        job.bottom_layer_count     = fresh->globals.bottom_layers;
        job.transition_layer_count = fresh->globals.transition_layers;
    }
    // The bottom layer count is a global of sliced / generated documents, written through a layer-list edit (which keeps
    // layers and Z as they are). Opened files keep it in their native header, which a recipe cannot rewrite.
    job.can_set_bottom_layer_count = doc.fresh_job().has_value() && doc.layer_list_refusal(false).empty();

    const auto capabilities = doc.parameter_capabilities();
    auto editable = [&](LayerParameter field) {
        return std::any_of(capabilities.begin(), capabilities.end(), [&](const auto &c) { return c.field == field && c.editable; });
    };
    std::vector<LayerParameters> rows(count);
    bool has_exposure = count > 0, has_before = count > 0, has_after = count > 0;
    for (size_t i = 0; i < count; ++i) {
        rows[i] = doc.layer_parameters(i);
        has_exposure &= rows[i].count(LayerParameter::Exposure) > 0;
        has_before   &= rows[i].count(LayerParameter::WaitBefore) > 0;
        has_after    &= rows[i].count(LayerParameter::WaitAfter) > 0;
    }
    auto column = [&](LayerParameter field) {
        std::vector<double> values(count);
        for (size_t i = 0; i < count; ++i) values[i] = rows[i].at(field);
        return values;
    };
    if (has_exposure) job.exposure_s = column(LayerParameter::Exposure);
    job.can_set_exposure = has_exposure && editable(LayerParameter::Exposure);
    if (has_before && editable(LayerParameter::WaitBefore)) job.wait_before_s = column(LayerParameter::WaitBefore);
    if (has_after && editable(LayerParameter::WaitAfter))   job.wait_after_s  = column(LayerParameter::WaitAfter);
    auto exposure_of = [](const LayerParameters &m) { const auto it = m.find(LayerParameter::Exposure); return it == m.end() ? 0. : it->second; };
    job.bottom_exposure_s = job_parameters ? exposure_of(job_parameters->bottom) : 0.;
    job.normal_exposure_s = job_parameters ? exposure_of(job_parameters->normal) : 0.;
    if (!job.exposure_s.empty()) {
        if (job.bottom_exposure_s <= 0) job.bottom_exposure_s = job.exposure_s.front();
        if (job.normal_exposure_s <= 0) job.normal_exposure_s = job.exposure_s.back();
    }
    if (count > 0) {
        const auto first = doc.image(0);
        job.resolution_x = size_t(first.cols);
        job.resolution_y = size_t(first.rows);
    }
    if (geometry && geometry->columns && geometry->rows) {
        job.pixel_width_mm  = geometry->width_mm / double(geometry->columns);
        job.pixel_height_mm = geometry->height_mm / double(geometry->rows);
    }
    job.stats = layer_stats_from_masks([&doc](size_t index) {
        auto image = doc.image(index);
        LayerMask mask;
        mask.width = size_t(image.cols); mask.height = size_t(image.rows); mask.data = std::move(image.buf);
        return mask;
    });
    return job;
}

// Evaluates the UVtools suggestions with their default limits. With "apply": true the per-layer changes
// (exposure / wait before / wait after cure) are written as ONE edit. Suggestions that need the global header
// (bottom / transition layer count), a blank first layer or a model move are reported but never applied.
StepCode compile_suggestions(Reader &r)
{
    static const std::vector<std::string> ids = {"bottom_layer_count", "layer_height", "transition_layer_count",
                                                 "wait_time_before_cure", "wait_time_after_cure", "model_position"};
    std::set<std::string> only, disabled;
    auto read_list = [&r](const char *key, std::set<std::string> &target) {
        const Json *list = r.find(key);
        if (!list) return;
        if (!list->is_array()) r.fail(key, "expected a list of suggestion ids");
        for (const auto &item : *list) {
            if (!item.is_string() || std::find(ids.begin(), ids.end(), item.get<std::string>()) == ids.end()) {
                std::string allowed;
                for (const auto &id : ids) allowed += (allowed.empty() ? "" : ", ") + id;
                r.fail(key, "allowed ids: " + allowed);
            }
            target.insert(item.get<std::string>());
        }
    };
    read_list("only", only);
    read_list("disable", disabled);
    const bool apply = r.boolean("apply", false);
    const std::string output = r.string("output");
    r.finish();

    StepCode c;
    c.check = [output](Ctx &x) {
        if (x.document().layer_count() == 0) throw RuntimeError("the document has no layers");
        if (!output.empty()) x.check_output(x.output_path(output), false);
    };
    c.run = [only, disabled, apply, output](Ctx &x) {
        LayerDocument &doc = x.document();
        SuggestionConfig config;
        auto on = [&](const char *id) { return (only.empty() || only.count(id)) && !disabled.count(id); };
        config.bottom_layer_count.enabled     = on("bottom_layer_count");
        config.layer_height.enabled           = on("layer_height");
        config.transition_layer_count.enabled = on("transition_layer_count");
        config.wait_time_before_cure.enabled  = on("wait_time_before_cure");
        config.wait_time_after_cure.enabled   = on("wait_time_after_cure");
        config.model_position.enabled         = on("model_position");
        const SuggestionJob job = build_suggestion_job(doc);
        const auto results = evaluate_suggestions(job, config, [&] { return x.progress && !x.progress(0, 0); });

        std::vector<LayerParameters> table, patch;
        size_t changed_layers = 0, not_applicable = 0;
        std::optional<size_t> new_bottom_count;
        if (apply) {
            table.resize(doc.layer_count());
            patch.resize(doc.layer_count());
            for (size_t i = 0; i < table.size(); ++i) table[i] = doc.layer_parameters(i);
        }
        Json report = {{"layer_count", doc.layer_count()}, {"suggestions", Json::array()}};
        for (const auto &s : results) {
            const char *status = s.status == SuggestionStatus::Applied ? "applied" : s.status == SuggestionStatus::NotApplied ? "not_applied" : "unavailable";
            Json item = {{"title", s.title}, {"status", status}, {"message", s.message}, {"layer_changes", s.layer_changes.size()}};
            if (apply && s.status == SuggestionStatus::NotApplied) {
                for (const auto &change : s.layer_changes) {
                    if (change.layer < table.size()) { table[change.layer][change.field] = change.after; patch[change.layer][change.field] = change.after; ++changed_layers; }
                }
                const bool bottom_applied = s.bottom_layer_count && job.can_set_bottom_layer_count && s.bottom_layer_count->second <= doc.layer_count();
                if (bottom_applied) new_bottom_count = s.bottom_layer_count->second;
                // Not applicable: the transition layer count (a global the document does not let an edit rewrite), a
                // bottom count of a file format with a native header, a blank first layer, a model move, and advice
                // that carries no change at all.
                if ((s.bottom_layer_count && !bottom_applied) || s.transition_layer_count || s.model_move || s.prepend_blank_first_layer ||
                    (s.layer_changes.empty() && !s.bottom_layer_count))
                    ++not_applicable;
            }
            report["suggestions"].push_back(std::move(item));
        }
        if (new_bottom_count && *new_bottom_count != job.bottom_layer_count) {
            // One layer-list edit (identity list): the per-layer values and the new bottom layer count together.
            const size_t count = doc.layer_count(), bottom = *new_bottom_count;
            require_done(doc.apply_layer_list_operation(
                [&](std::vector<lo::Mask> &, std::vector<double> &, const LayerProgress &) {
                    Change change;
                    change.list.source_index = identity_sources(count);
                    change.row_patch         = patch;
                    change.bottom_layer_count = bottom;
                    return change;
                },
                false, layer_pitch_mm(doc), x.progress));
        } else if (apply && changed_layers)
            doc.replace_layer_parameter_table(table); // one edit; throws with the document untouched
        if (!output.empty()) write_text(x, x.output_path(output), report.dump(2) + "\n");
        x.message = std::to_string(results.size()) + " suggestion(s)";
        if (apply) {
            x.message += ", " + std::to_string(changed_layers) + " layer value(s) changed";
            if (new_bottom_count && *new_bottom_count != job.bottom_layer_count)
                x.message += ", bottom layer count " + std::to_string(job.bottom_layer_count) + " -> " + std::to_string(*new_bottom_count);
            x.message += ", " + std::to_string(not_applicable) + " not applicable by a recipe";
        }
    };
    return c;
}

// ---- open / save / convert / exports ----------------------------------------------------------------------
StepCode compile_open(Reader &r)
{
    const std::string path = r.string("path"); // may be empty: then --input must supply it
    r.finish();
    StepCode c;
    auto open = [path](Ctx &x) {
        const std::string file = !x.options->input.empty() ? x.options->input : path;
        if (x.options->document) { x.doc = *x.options->document; x.message = "using the supplied document"; return; }
        if (file.empty()) throw RuntimeError("no input: give 'path' in the open step or --input");
        const fs::path resolved = !x.options->input.empty() ? fs::path(file) : x.resolve(file);
        if (!fs::is_regular_file(resolved)) throw RuntimeError("input file '" + resolved.string() + "' does not exist");
        if (boost::algorithm::iends_with(resolved.string(), ".sla-project")) {
            auto project = read_project_layers(resolved.string(), x.progress);
            if (!project) throw RuntimeError("not a readable layer project: " + resolved.string());
            x.doc = std::move(project->document);
        } else
            x.doc = LayerDocument::load_archive(resolved.string(), x.progress);
        x.message = std::to_string(x.doc->layer_count()) + " layers, format '" + x.doc->archive_format() + "'";
    };
    // Opening is read only, so the dry run performs it too: it proves the input is readable.
    c.check = open;
    c.run   = [](Ctx &) {}; // the check already opened it; the runner treats open specially
    return c;
}

StepCode compile_save(Reader &r)
{
    const std::string path = r.required_string("path");
    r.finish();
    StepCode c;
    c.check = [path](Ctx &x) {
        if (x.document().layer_count() == 0) throw RuntimeError("the document has no layers");
        x.check_output(x.output_path(path), false);
    };
    c.run = [path](Ctx &x) {
        const std::string target = x.stage(x.output_path(path), false);
        x.document().save_archive(target, x.progress);
        x.message = "native container '" + x.document().archive_format() + "' -> " + x.output_path(path);
    };
    return c;
}

DynamicPrintConfig load_printer_config(Ctx &x, const std::string &file)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    if (file.empty()) return config;
    const fs::path resolved = x.resolve(file);
    if (!fs::is_regular_file(resolved)) throw RuntimeError("printer config '" + resolved.string() + "' does not exist");
    config.load(resolved.string(), ForwardCompatibilitySubstitutionRule::EnableSilent);
    return config;
}

StepCode compile_convert(Reader &r)
{
    const std::string format = r.required_string("format");
    const std::string path   = r.required_string("path");
    const std::string printer_config = r.string("printer_config");
    r.finish();
    const ArchiveEntry *entry = get_archive_entry(format.c_str());
    if (!entry) {
        std::string ids;
        for (const auto &e : registered_sla_archives()) ids += (ids.empty() ? "" : ", ") + std::string(e.id);
        r.fail("format", "unknown format id '" + format + "'; known ids: " + ids);
    }
    if (const auto reason = archive_export_unavailable_reason(format.c_str()); !reason.empty()) r.fail("format", "cannot be written: " + reason);

    StepCode c;
    c.check = [=](Ctx &x) {
        LayerDocument &doc = x.document();
        if (doc.layer_count() == 0) throw RuntimeError("the document has no layers");
        x.check_output(sla_archive_filename(x.output_path(path), entry->ext), false);
        (void) plan_layer_conversion(doc, format); // throws naming the field the destination cannot preserve
    };
    c.run = [=](Ctx &x) {
        const std::string final_name = sla_archive_filename(x.output_path(path), entry->ext);
        const DynamicPrintConfig config = load_printer_config(x, printer_config);
        const std::string target = x.stage(final_name, false);
        NativeArchiveConversionRequest(x.document(), config, entry->id, target, x.progress).write(x.progress);
        x.message = "converted to '" + format + "' -> " + final_name;
    };
    return c;
}

StepCode export_step(const std::string &path, bool directory, std::function<void(Ctx &, const std::string &target)> write,
                     std::function<std::string(const LayerDocument &)> validate = {})
{
    StepCode c;
    c.check = [path, directory, validate](Ctx &x) {
        if (x.document().layer_count() == 0) throw RuntimeError("the document has no layers");
        if (validate) require_ok(validate(x.document()));
        x.check_output(path, directory);
    };
    c.run = [path, directory, write](Ctx &x) {
        write(x, x.stage(path, directory));
        x.message += (x.message.empty() ? "" : " ") + std::string("-> ") + path;
    };
    return c;
}

StepCode compile_export_images(Reader &r)
{
    lx::ExportImageParams p;
    p.selection       = read_selection(r);
    const std::string folder = r.required_string("folder");
    p.filename_prefix = r.string("filename_prefix", p.filename_prefix);
    p.type            = r.choice("type", lx::ImageType::Png, std::vector<std::pair<std::string, lx::ImageType>>{{"png", lx::ImageType::Png}, {"pgm", lx::ImageType::Pgm}});
    p.orientation     = read_orientation(r);
    p.pad_layer_index = r.boolean("pad_layer_index", true);
    p.crop_by_roi     = r.boolean("crop_by_roi", true);
    r.finish();
    return export_step(folder, true, [p](Ctx &x, const std::string &target) {
        const auto masks = decode_masks(x.document(), x.progress);
        const size_t n = lx::export_layer_images(masks, p, target, x.progress);
        x.message = std::to_string(n) + " image(s)";
    });
}

StepCode compile_export_gif(Reader &r)
{
    lx::ExportGifParams p;
    p.selection = read_selection(r);
    const std::string file = r.required_string("file");
    p.clip_by_volume_bounds = r.boolean("clip_by_volume_bounds", false);
    p.fps           = r.uint("fps", p.fps, 100);
    p.repeats       = r.uint("repeats", 0, 65535);
    p.skip          = r.uint("skip", 0, 100000);
    p.scale_percent = r.number("scale_percent", p.scale_percent, 1, 1000);
    p.orientation   = read_orientation(r);
    r.finish();
    return export_step(file, false,
        [p](Ctx &x, const std::string &target) { lx::export_layer_gif(decode_masks(x.document(), x.progress), p, target, x.progress); },
        [p](const LayerDocument &d) { return lx::validate_export_gif(p, d.layer_count()); });
}

StepCode compile_export_heat_map(Reader &r)
{
    lx::ExportHeatMapParams p;
    p.selection   = read_selection(r);
    const std::string file = r.required_string("file");
    p.orientation = read_orientation(r);
    p.merge_same_positioned_layers = r.boolean("merge_same_positioned_layers", true);
    p.crop_by_roi = r.boolean("crop_by_roi", true);
    r.finish();
    return export_step(file, false,
        [p](Ctx &x, const std::string &target) { lx::export_heat_map(decode_masks(x.document(), x.progress), layer_z(x.document()), p, target, x.progress); },
        [p](const LayerDocument &d) { return lx::validate_export_heat_map(p, d.layer_count()); });
}

StepCode compile_export_skeleton(Reader &r)
{
    lx::ExportSkeletonParams p;
    p.selection = read_selection(r);
    const std::string file = r.required_string("file");
    p.crop_by_roi = r.boolean("crop_by_roi", true);
    r.finish();
    return export_step(file, false, [p](Ctx &x, const std::string &target) { lx::export_skeleton(decode_masks(x.document(), x.progress), p, target, x.progress); });
}

StepCode compile_export_mesh(Reader &r)
{
    lx::ExportMeshParams p;
    p.selection       = read_selection(r, false);
    const std::string file = r.required_string("file");
    p.pixel_width_mm  = r.number("pixel_width_mm", p.pixel_width_mm, 0, 10);
    p.pixel_height_mm = r.number("pixel_height_mm", p.pixel_height_mm, 0, 10);
    p.quality         = r.uint("quality", 1, 8);
    p.orientation     = read_orientation(r);
    p.merge_faces     = r.boolean("merge_faces", true);
    r.finish();
    return export_step(file, false,
        [p](Ctx &x, const std::string &target) { lx::export_mesh(decode_masks(x.document(), x.progress), layer_z(x.document()), p, target, x.progress); },
        [p](const LayerDocument &d) { return lx::validate_export_mesh(p, d.layer_count()); });
}

StepCode compile_export_html(Reader &r)
{
    lx::ExportHtmlParams p;
    const std::string file = r.required_string("file");
    p.title                 = r.string("title", p.title);
    p.export_layer_settings = r.boolean("export_layer_settings", true);
    p.export_layer_preview  = r.boolean("export_layer_preview", true);
    p.preview_first         = r.index("preview_first", 0);
    p.preview_last          = r.index("preview_last", lo::kLastLayer);
    p.preview_step          = std::max<size_t>(1, r.index("preview_step", 1));
    p.preview_scale_percent = r.number("preview_scale_percent", p.preview_scale_percent, 1, 1000);
    p.orientation           = read_orientation(r);
    r.finish();
    return export_step(file, false, [p](Ctx &x, const std::string &target) {
        LayerDocument &doc = x.document();
        std::vector<lo::LayerParams> params;
        if (p.export_layer_settings) params = doc.parameter_table(); // empty when the format has no table: the table is left out
        lx::export_html(decode_masks(doc, x.progress), layer_z(doc), params, p, target, x.progress);
    });
}

struct OperationEntry
{
    const char *name;
    StepCode  (*compile)(Reader &);
    const char *summary;
};

const std::vector<OperationEntry> &operations()
{
    static const std::vector<OperationEntry> table = {
        {"open", compile_open, "Open an archive or .sla-project (first step only; path may come from --input)."},
        {"flip", compile_flip, "Mirror layers horizontally / vertically / both (UVtools Flip)."},
        {"rotate", compile_rotate, "Rotate layers by an angle (UVtools Rotate)."},
        {"resize", compile_resize, "Scale layers by a percentage (UVtools Resize)."},
        {"move", compile_move, "Move or copy the model to an anchor (UVtools Move)."},
        {"threshold", compile_threshold, "Threshold the masks (UVtools Threshold)."},
        {"solidify", compile_solidify, "Fill holes (UVtools Solidify)."},
        {"pattern", compile_pattern, "Repeat the model in a grid (UVtools Pattern)."},
        {"pixel_dimming", compile_pixel_dimming, "Dim wall / inner pixels (UVtools Pixel dimming)."},
        {"pixel_arithmetic", compile_pixel_arithmetic, "Per-pixel arithmetic (UVtools Pixel arithmetic)."},
        {"light_bleed", compile_light_bleed, "Compensate light bleed (UVtools Light bleed compensation)."},
        {"clone", compile_clone, "Repeat layers (UVtools Clone; sliced or generated documents only)."},
        {"remove", compile_remove, "Remove layers (UVtools Remove; sliced or generated documents only)."},
        {"edit_parameters", compile_edit_parameters, "Edit exposure / lift / wait values (UVtools Edit print parameters)."},
        {"repair_layers", compile_repair_layers, "Remove islands, fill resin traps, drill suction cups ... (UVtools Repair layers)."},
        {"reheight", compile_reheight, "Split / merge layers to a new height or offset Z (UVtools Layer re-height; sliced or generated documents only)."},
        {"double_exposure", compile_double_exposure, "Print every layer twice (UVtools Double exposure; sliced or generated documents only)."},
        {"phased_exposure", compile_phased_exposure, "Cure layers in several phases (UVtools Phased exposure; sliced or generated documents only)."},
        {"dynamic_lifts", compile_dynamic_lifts, "Lift height / speed from the layer area (UVtools Dynamic lifts)."},
        {"fade_exposure", compile_fade_exposure, "Fade the exposure time over N layers (UVtools Fade exposure time)."},
        {"timelapse", compile_timelapse, "Raise the platform for camera shots (UVtools Timelapse; virtual layer mode: sliced or generated documents only)."},
        {"dynamic_layer_height", compile_dynamic_layer_height, "Stack identical layers into thicker ones (UVtools Dynamic layer height; sliced or generated documents only)."},
        {"raise_on_print_finish", compile_raise_on_print_finish, "Append a layer that raises the platform at the end (UVtools Raise platform on print finish; sliced or generated documents only)."},
        {"change_resolution", compile_change_resolution, "Crop / pad the layers to a new resolution (UVtools Change resolution; not for sliced or generated documents)."},
        {"infill", compile_infill, "Replace the model interior by an infill pattern (UVtools Infill)."},
        {"raft_relief", compile_raft_relief, "Relieve the raft with holes, dimming, decimation or tabs (UVtools Raft relief)."},
        {"layer_arithmetic", compile_layer_arithmetic, "Compute layers from other layers (UVtools Layer arithmetic)."},
        {"lithophane", compile_lithophane, "Replace all layers by a lithophane of a PNG picture (UVtools Lithophane; sliced or generated documents only)."},
        {"detect_issues", compile_detect_issues, "Detect islands, overhangs, resin traps ...; optional JSON report."},
        {"suggestions", compile_suggestions, "Evaluate UVtools suggestions; optionally apply the per-layer ones."},
        {"save", compile_save, "Save the document in its own container format."},
        {"convert", compile_convert, "Convert to another registered format id."},
        {"export_images", compile_export_images, "Export layer images to a new folder."},
        {"export_gif", compile_export_gif, "Export an animated GIF."},
        {"export_heat_map", compile_export_heat_map, "Export the heat map image."},
        {"export_skeleton", compile_export_skeleton, "Export the skeleton image."},
        {"export_mesh", compile_export_mesh, "Export a voxel mesh (STL)."},
        {"export_html", compile_export_html, "Export a self-contained HTML report."},
    };
    return table;
}

} // namespace

std::vector<OperationInfo> operation_catalogue()
{
    std::vector<OperationInfo> out;
    for (const auto &e : operations()) out.push_back({e.name, e.summary});
    return out;
}

// ---------------------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------------------
Recipe parse_recipe(const std::string &text)
{
    Json json;
    try {
        json = Json::parse(text);
    } catch (const Json::parse_error &error) {
        // Turn the byte offset into line and column; the library message repeats it, but not as a location.
        const size_t pos = error.byte ? std::min(size_t(error.byte) - 1, text.size()) : 0;
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos; ++i) {
            if (text[i] == '\n') { ++line; col = 1; }
            else ++col;
        }
        throw RecipeError("line " + std::to_string(line) + ", column " + std::to_string(col), std::string("invalid JSON: ") + error.what());
    }
    Reader top(json, "");
    Recipe recipe;
    const auto version = top.opt_integer("version", 0, 1000000);
    if (!version) top.fail("version", "is required");
    if (*version != kRecipeVersion) top.fail("version", "unsupported version " + std::to_string(*version) + "; this program reads version " + std::to_string(kRecipeVersion));
    recipe.version     = int(*version);
    recipe.description = top.string("description");
    const Json *steps  = top.find("steps");
    if (!steps) top.fail("steps", "is required");
    if (!steps->is_array() || steps->empty()) top.fail("steps", "expected a non-empty list of steps");
    top.finish();

    std::map<std::string, const OperationEntry *> by_name;
    for (const auto &e : operations()) by_name[e.name] = &e;
    for (size_t i = 0; i < steps->size(); ++i) {
        const std::string location = "steps[" + std::to_string(i) + "]";
        Reader r((*steps)[i], location);
        Step step;
        step.index    = i;
        step.location = location;
        step.op       = r.required_string("op");
        step.label    = r.string("name");
        const auto it = by_name.find(step.op);
        if (it == by_name.end()) {
            std::string names;
            for (const auto &e : operations()) names += (names.empty() ? "" : ", ") + std::string(e.name);
            r.fail("op", "unknown operation '" + step.op + "'; known operations: " + names);
        }
        if (step.op == "open" && i != 0) r.fail("op", "'open' must be the first step");
        // "op" and "name" are consumed above; the compile function consumes the rest and rejects unknown keys.
        step.code = std::make_shared<const StepCode>(it->second->compile(r));
        recipe.steps.push_back(std::move(step));
    }
    for (auto it = recipe.steps.rbegin(); it != recipe.steps.rend(); ++it)
        if (it->op == "save" || it->op == "convert") { it->final_output = true; break; }
    return recipe;
}

Recipe load_recipe(const std::string &path)
{
    boost::nowide::ifstream file(path, std::ios::binary);
    if (!file) throw RecipeError("", "cannot read recipe file '" + path + "'");
    std::stringstream buffer;
    buffer << file.rdbuf();
    Recipe recipe = parse_recipe(buffer.str());
    recipe.base_dir = fs::absolute(fs::path(path)).parent_path().string();
    return recipe;
}

// ---------------------------------------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------------------------------------
RunResult run_recipe(const Recipe &recipe, RunOptions options)
{
    RunResult result;
    result.dry_run = options.dry_run;
    for (const auto &s : recipe.steps) {
        StepResult sr;
        sr.index = s.index; sr.op = s.op; sr.label = s.label;
        result.steps.push_back(sr);
    }

    Ctx ctx;
    ctx.options = &options;
    ctx.dry     = options.dry_run;
    const std::string base = !options.base_dir.empty() ? options.base_dir : recipe.base_dir;
    ctx.base = base.empty() ? fs::current_path() : fs::absolute(fs::path(base));

    const size_t total_steps = recipe.steps.size() + 1; // + the final publication
    bool cancelled = false;
    size_t current = 0;
    size_t reported = 0; // highest value reported so far
    auto report = [&](size_t done, size_t total) {
        if (!options.progress) return true;
        // Capped below 100 so a step that is still running never looks like the next step starting.
        const size_t pct = total ? std::min<size_t>(99, done * 100 / total) : 0;
        // A step may run several sub-tasks (decode, then encode/export), each restarting its own
        // (done, total) at zero, and (0, 0) is used as a pure cancellation poll. Never let the
        // overall bar move backwards because of that.
        reported = std::max(reported, current * 100 + pct);
        if (!options.progress(reported, total_steps * 100)) cancelled = true;
        return !cancelled;
    };
    ctx.progress = report;

    auto fail = [&](size_t index, const std::string &why) {
        result.steps[index].status  = StepResult::Status::Failed;
        result.steps[index].message = why;
        result.failed_step = index;
        result.error = recipe.steps[index].location + " (" + recipe.steps[index].op + "): " + why;
    };

    // An input without an "open" step (the recipe is generic and --input / a document is supplied).
    const bool has_open = !recipe.steps.empty() && recipe.steps.front().op == "open";
    if (!has_open) {
        if (options.document) ctx.doc = *options.document;
        else if (!options.input.empty()) {
            try {
                ctx.doc = LayerDocument::load_archive(options.input, ctx.progress);
            } catch (const LayerOperationCancelled &) {
                result.cancelled = true; result.error = "cancelled";
                return result;
            } catch (const std::exception &e) {
                result.error = "input '" + options.input + "': " + e.what();
                return result;
            }
        }
    }

    // An --output without a save / convert step still has to be written: append an implicit native save.
    Recipe implicit_holder;
    const Recipe *active = &recipe;
    const bool has_final_output = std::any_of(recipe.steps.begin(), recipe.steps.end(), [](const Step &s) { return s.final_output; });
    if (!options.output.empty() && !has_final_output) {
        implicit_holder = recipe;
        Step save;
        save.index = implicit_holder.steps.size();
        save.op = "save"; save.label = "implicit save (--output)"; save.location = "steps[" + std::to_string(save.index) + "]";
        save.final_output = true;
        StepCode code;
        const std::string out = options.output;
        code.check = [out](Ctx &x) { x.check_output(out, false); };
        code.run   = [out](Ctx &x) {
            x.document().save_archive(x.stage(out, false), x.progress);
            x.message = "native container -> " + out;
        };
        save.code = std::make_shared<const StepCode>(std::move(code));
        implicit_holder.steps.push_back(save);
        active = &implicit_holder;
        StepResult sr;
        sr.index = save.index; sr.op = save.op; sr.label = save.label;
        result.steps.push_back(sr);
    }

    bool stale = false; // dry run: a layer-list change was only simulated by skipping, so checks would be wrong
    try {
        for (const Step &step : active->steps) {
            current = step.index;
            ctx.step = &step;
            ctx.message.clear();
            StepResult &sr = result.steps[step.index];
            if (!report(0, 1)) { cancelled = true; sr.status = StepResult::Status::Cancelled; break; }
            try {
                if (ctx.dry) {
                    if (step.op == "open") step.code->check(ctx);
                    else if (stale) { sr.message = "not checked: depends on the layer list after an earlier layer-list step"; sr.status = StepResult::Status::Planned; continue; }
                    else step.code->check(ctx);
                    stale = stale || step.code->changes_layer_list;
                    sr.status  = StepResult::Status::Planned;
                    sr.message = ctx.message.empty() ? "checked" : ctx.message;
                } else {
                    if (step.op == "open") step.code->check(ctx); // opening is the whole step
                    else {
                        step.code->check(ctx);
                        step.code->run(ctx);
                    }
                    if (cancelled) throw LayerOperationCancelled{};
                    sr.status  = StepResult::Status::Done;
                    sr.message = ctx.message;
                }
            } catch (const LayerOperationCancelled &) {
                cancelled = true;
                sr.status = StepResult::Status::Cancelled;
                break;
            } catch (const CanceledException &) {
                cancelled = true;
                sr.status = StepResult::Status::Cancelled;
                break;
            } catch (const std::exception &e) {
                fail(step.index, e.what());
                break;
            }
        }
    } catch (...) {
        ctx.discard_staged();
        throw;
    }

    if (cancelled) {
        ctx.discard_staged();
        result.cancelled = true;
        result.error = "cancelled";
        return result;
    }
    if (!result.error.empty()) {
        ctx.discard_staged();
        return result;
    }
    if (ctx.dry) {
        result.ok = true;
        return result;
    }
    // Last chance to cancel; after this point the outputs are published.
    current = active->steps.size();
    if (!report(0, 1)) {
        ctx.discard_staged();
        result.cancelled = true;
        result.error = "cancelled";
        return result;
    }
    try {
        result.outputs = ctx.publish_staged();
    } catch (const std::exception &e) {
        ctx.discard_staged();
        result.error = std::string("publishing the outputs: ") + e.what();
        return result;
    }
    result.ok = true;
    result.document = std::move(ctx.doc);
    return result;
}

std::string run_result_to_json(const RunResult &result)
{
    auto status_name = [](StepResult::Status s) {
        switch (s) {
        case StepResult::Status::Done:      return "done";
        case StepResult::Status::Planned:   return "planned";
        case StepResult::Status::Failed:    return "failed";
        case StepResult::Status::Cancelled: return "cancelled";
        case StepResult::Status::NotRun:    return "not_run";
        }
        return "unknown";
    };
    Json json = {{"ok", result.ok}, {"cancelled", result.cancelled}, {"dry_run", result.dry_run}, {"error", result.error},
                 {"steps", Json::array()}, {"outputs", result.outputs}};
    for (const auto &s : result.steps)
        json["steps"].push_back({{"index", s.index}, {"op", s.op}, {"name", s.label}, {"status", status_name(s.status)}, {"message", s.message}});
    return json.dump(2) + "\n";
}

} // namespace Slic3r::sla::recipe
