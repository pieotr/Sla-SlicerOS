// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LayerSuggestions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <tbb/parallel_for.h>

#include "libslic3r/PrintBase.hpp" // CanceledException

namespace Slic3r { namespace sla {

namespace {

constexpr size_t k_stats_block_layers = 8; // layers decoded per parallel block / cancel check
// UVtools GlobalAppliedMessage / GlobalNotAppliedMessage prefixes.
const char *const k_ok = "\xE2\x9C\x93 ";   // check mark
const char *const k_warn = "\xE2\x9A\xA0 "; // warning sign
const char *const k_arrow = " \xC2\xBB ";    // right guillemet, UVtools' "before » after" separator

// ------------------------------------------------------------- numbers

// Looks like C# float/decimal ToString(): shortest form, no trailing zeros.
std::string num(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(6);
    out << (std::fabs(value) < 1e-9 ? 0. : value);
    return out.str();
}

// UVtools Layer.RoundHeight: heights carry 3 decimals.
double round3(double v) { return std::round(v * 1000.) / 1000.; }

// Comparisons of heights/times given in mm/s that must not trip over binary
// floating point noise (C# uses decimal there).
double r4(double v) { return std::round(v * 1e4) / 1e4; }

// C# Math.Round(decimal, 2) rounds half to even.
double round2_even(double v)
{
    const double scaled = v * 100.;
    double floor_v = std::floor(scaled);
    const double diff = scaled - floor_v;
    double rounded;
    if (std::fabs(diff - 0.5) < 1e-9) rounded = std::fmod(floor_v, 2.) == 0. ? floor_v : floor_v + 1.;
    else rounded = std::round(scaled);
    return rounded / 100.;
}

// Number of decimals of the shortest representation that reads back as the
// same float (UVtools float.DecimalDigits()). Layer heights are floats there,
// so 0.05 has 2 digits although the double 0.05 is not exactly representable.
unsigned decimal_digits(double value)
{
    const float f = float(value);
    char buffer[48];
    for (int precision = 1; precision <= 9; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*f", precision, double(f)); // fixed notation: digits after the point
        if (std::strtof(buffer, nullptr) == f) {
            std::string text = buffer;
            while (text.size() > 1 && text.back() == '0') text.pop_back(); // "0.50" -> "0.5"
            const size_t dot = text.find('.');
            return dot == std::string::npos || dot + 1 == text.size() ? 0u : unsigned(text.size() - dot - 1);
        }
    }
    return 9;
}

// ------------------------------------------------------- evaluation context

// Shared lazy state of one evaluation: mask statistics are decoded once.
class Context
{
public:
    Context(const SuggestionJob &job, std::function<bool()> cancel) : job(job), cancel(std::move(cancel)) {}

    const SuggestionJob &job;
    size_t count() const { return job.z_mm.size(); }

    bool is_bottom(size_t i) const { return i < job.bottom_layer_count; }

    // Mask statistics of every layer, gathered in parallel on first use.
    const std::vector<SuggestionLayerStats> &stats()
    {
        if (m_loaded) return m_stats;
        m_stats.assign(count(), {});
        if (job.stats && count() > 0) {
            const size_t blocks = (count() + k_stats_block_layers - 1) / k_stats_block_layers;
            tbb::parallel_for(size_t(0), blocks, [this](size_t block) {
                if (cancel && cancel()) return; // remaining blocks are skipped, reported below
                const size_t end = std::min(count(), (block + 1) * k_stats_block_layers);
                for (size_t i = block * k_stats_block_layers; i < end; ++i) m_stats[i] = job.stats(i);
            });
            if (cancel && cancel()) throw CanceledException();
        }
        m_loaded = true;
        return m_stats;
    }

    // UVtools Layer.IsDummy: at most one lit pixel or (almost) no exposure.
    bool is_dummy(size_t i)
    {
        const double exposure = i < job.exposure_s.size() ? job.exposure_s[i] : job.normal_exposure_s;
        // Without masks the pixel rule cannot be evaluated; only the exposure rule applies.
        const bool few_pixels = job.stats && stats()[i].nonzero_pixels <= 1;
        return few_pixels || exposure <= 0.01;
    }

    // UVtools Layer.LayerHeight: distance to the closest lower layer with a different Z.
    double layer_height(size_t i) const
    {
        if (i == 0) return job.z_mm[0];
        for (size_t j = i; j-- > 0;) {
            const double h = round3(job.z_mm[i] - job.z_mm[j]);
            if (h == 0.) continue;
            if (h < 0.) break;
            return h;
        }
        return job.layer_height_mm;
    }

    double pixel_area_mm2() const { return job.pixel_width_mm * job.pixel_height_mm; }

    // Previous / next layer carrying at least `pixels` lit pixels (UVtools Get*LayerWithAtLeastPixelCountOf).
    std::optional<size_t> previous_with_pixels(size_t i, size_t pixels)
    {
        for (size_t j = i; j-- > 0;)
            if (stats()[j].nonzero_pixels >= pixels) return j;
        return std::nullopt;
    }
    std::optional<size_t> next_with_pixels(size_t i, size_t pixels)
    {
        for (size_t j = i + 1; j < count(); ++j)
            if (stats()[j].nonzero_pixels >= pixels) return j;
        return std::nullopt;
    }

    std::function<bool()> cancel;

private:
    std::vector<SuggestionLayerStats> m_stats;
    bool                              m_loaded = false;
};

SuggestionResult make_result(const char *id, const char *title, const char *description)
{
    SuggestionResult r;
    r.id = id;
    r.title = title;
    r.description = description;
    return r;
}

SuggestionResult unavailable(SuggestionResult r, const std::string &reason)
{
    r.status = SuggestionStatus::Unavailable;
    r.unavailable_reason = reason;
    r.message = std::string(k_warn) + r.title + ": " + reason;
    return r;
}

std::string with_validation(const std::string &error)
{
    return error.empty() ? error : "invalid settings: " + error;
}

void append_error(std::string &errors, const std::string &line)
{
    if (!errors.empty()) errors += "; ";
    errors += line;
}

// ------------------------------------------------------------ BottomLayerCount

// UVtools SuggestionBottomLayerCount.BottomLayerCountValue; float math like the C# original.
size_t recommended_bottom_layer_count(const SuggestionJob &job, const BottomLayerCountSuggestionConfig &c)
{
    const float needed = std::ceil(float(c.target_bottom_height_mm) / float(job.layer_height_mm));
    const float clamped = std::clamp(needed, float(c.minimum_bottom_layer_count), float(c.maximum_bottom_layer_count));
    return size_t(clamped);
}

} // namespace

// ---------------------------------------------------------------- validation

std::string validate_suggestion_config(const BottomLayerCountSuggestionConfig &c)
{
    std::string e;
    if (c.target_bottom_height_mm < 0) append_error(e, "Bottom height must be a positive value");
    if (c.minimum_bottom_height_mm < 0) append_error(e, "Minimum limit (mm) must be a positive value");
    if (c.maximum_bottom_height_mm < 0) append_error(e, "Maximum limit (mm) must be a positive value");
    if (c.minimum_bottom_height_mm > c.maximum_bottom_height_mm) append_error(e, "Minimum limit (mm) can't be higher than maximum limit (mm)");
    if (c.minimum_bottom_layer_count > c.maximum_bottom_layer_count) append_error(e, "Minimum limit (layers) can't be higher than maximum limit (layers)");
    return e;
}

std::string validate_suggestion_config(const LayerHeightSuggestionConfig &c)
{
    std::string e;
    if (c.maximum_decimal_digits < 2 || c.maximum_decimal_digits > 10) append_error(e, "Layer height digits must be between 2 and 10");
    if (c.minimum_layer_height_mm <= 0) append_error(e, "Minimum layer height must be higher than 0mm");
    if (c.maximum_layer_height_mm <= 0) append_error(e, "Maximum layer height must be higher than 0mm");
    if (c.minimum_layer_height_mm > c.maximum_layer_height_mm) append_error(e, "Minimum layer height can't be higher than maximum layer height");
    return e;
}

std::string validate_suggestion_config(const TransitionLayerCountSuggestionConfig &c)
{
    std::string e;
    if (c.transition_step_time_s < 0) append_error(e, "Decrement time (s) must be zero or a positive value");
    if (c.minimum_transition_layer_count > c.maximum_transition_layer_count) append_error(e, "Minimum limit (layers) can't be higher than maximum limit (layers)");
    return e;
}

std::string validate_suggestion_config(const WaitTimeBeforeCureSuggestionConfig &c)
{
    using T = WaitTimeBeforeCureSuggestionConfig::SetType;
    std::string e;
    if (c.minimum_bottom_s > c.maximum_bottom_s) append_error(e, "Minimum bottom limit can't be higher than maximum bottom limit");
    if (c.minimum_normal_s > c.maximum_normal_s) append_error(e, "Minimum normal limit can't be higher than maximum normal limit");
    if (c.set_type != T::Fixed && c.proportional_normal_s <= 0) append_error(e, "The proportional wait time must be higher than 0s");
    if (c.set_type == T::ProportionalLayerPixels && (c.proportional_bottom_layer_pixels == 0 || c.proportional_layer_pixels == 0))
        append_error(e, "The proportional pixels must be higher than 0");
    if (c.set_type == T::ProportionalLayerArea && (c.proportional_bottom_layer_area_mm2 == 0 || c.proportional_layer_area_mm2 == 0))
        append_error(e, "The proportional layer area must be higher than 0mm2");
    return e;
}

std::string validate_suggestion_config(const WaitTimeAfterCureSuggestionConfig &c)
{
    using T = WaitTimeAfterCureSuggestionConfig::SetType;
    std::string e;
    if (c.minimum_bottom_s > c.maximum_bottom_s) append_error(e, "Minimum bottom limit can't be higher than maximum bottom limit");
    if (c.minimum_normal_s > c.maximum_normal_s) append_error(e, "Minimum normal limit can't be higher than maximum normal limit");
    if (c.set_type == T::ProportionalExposure) {
        if (c.proportional_wait_s <= 0) append_error(e, "The proportional wait time must be higher than 0s");
        if (c.proportional_exposure_s <= 0) append_error(e, "The proportional exposure time must be higher than 0s");
    }
    return e;
}

std::string validate_suggestion_config(const ModelPositionSuggestionConfig &c)
{
    std::string e;
    if (c.minimum_top_bottom_margin_px > c.maximum_top_bottom_margin_px) append_error(e, "Minimum top/bottom margin limit (px) can't be higher than maximum limit (px)");
    if (c.minimum_left_right_margin_px > c.maximum_left_right_margin_px) append_error(e, "Minimum left/right margin limit (px) can't be higher than maximum limit (px)");
    return e;
}

// ------------------------------------------------------------------ stats

SuggestionLayerStatsProvider layer_stats_from_masks(LayerMaskProvider masks)
{
    return [masks = std::move(masks)](size_t layer) {
        SuggestionLayerStats stats;
        const LayerMask mask = masks(layer);
        if (mask.data.empty() || mask.width == 0) return stats; // blank layer
        int x0 = int(mask.width), y0 = int(mask.height), x1 = -1, y1 = -1;
        for (size_t y = 0; y < mask.height; ++y) {
            const uint8_t *row = mask.data.data() + y * mask.width;
            for (size_t x = 0; x < mask.width; ++x) {
                if (!row[x]) continue;
                ++stats.nonzero_pixels;
                x0 = std::min(x0, int(x)); x1 = std::max(x1, int(x));
                y0 = std::min(y0, int(y)); y1 = std::max(y1, int(y));
            }
        }
        if (stats.nonzero_pixels) stats.bbox = BoundingBox(Point(x0, y0), Point(x1, y1));
        return stats;
    };
}

namespace {

// ------------------------------------------------------------ BottomLayerCount impl

SuggestionResult run_bottom_layer_count(Context &ctx, const BottomLayerCountSuggestionConfig &c)
{
    const SuggestionJob &job = ctx.job;
    SuggestionResult r = make_result("BottomLayerCount", "Bottom layers",
        "Bottom layers should be kept to a minimum, usually from 2 to 3, it function is to provide a good adhesion to the first layer on the build plate, using a high count have disadvantages.");
    r.information_url = "https://ameralabs.com/blog/default-3d-printing-raft-settings";
    if (!job.can_set_bottom_layer_count) return unavailable(r, "the file format cannot store a bottom layer count");
    if (ctx.count() == 0) return unavailable(r, "the file has no layers");
    if (job.layer_height_mm <= 0) return unavailable(r, "the layer height is unknown");
    if (const auto error = validate_suggestion_config(c); !error.empty()) return unavailable(r, with_validation(error));

    const size_t count = job.bottom_layer_count;
    // UVtools BottomLayersHeight: Z of the last bottom layer.
    const double bottom_height = count == 0 ? 0. : job.z_mm[std::min(count, ctx.count()) - 1];
    const double print_height = job.z_mm.back();
    const size_t recommended = recommended_bottom_layer_count(job, c);

    bool applied = false;
    if (c.apply_when == SuggestionApplyWhen::OutsideLimits)
        applied = r4(bottom_height) >= r4(std::min(print_height, c.minimum_bottom_height_mm)) &&
                  r4(bottom_height) <= r4(c.maximum_bottom_height_mm) &&
                  count >= std::min<size_t>(ctx.count(), c.minimum_bottom_layer_count) &&
                  count <= c.maximum_bottom_layer_count;
    else
        applied = r4(bottom_height) == r4(std::min(print_height, c.target_bottom_height_mm));

    r.status = applied ? SuggestionStatus::Applied : SuggestionStatus::NotApplied;
    if (applied) {
        r.message = std::string(k_ok) + r.title + ": " + num(double(count)) + " / " + num(bottom_height) + "mm";
        return r;
    }
    r.message = std::string(k_warn) + r.title + " (" + num(double(count)) + ") is out of the recommended " + num(double(recommended)) + " layers";
    r.confirmation = r.title + ": " + num(double(count)) + k_arrow + num(double(recommended));
    r.bottom_layer_count = std::make_pair(count, recommended);
    return r;
}

// --------------------------------------------------------------- LayerHeight impl

SuggestionResult run_layer_height(Context &ctx, const LayerHeightSuggestionConfig &c)
{
    const SuggestionJob &job = ctx.job;
    SuggestionResult r = make_result("LayerHeight", "Layer height",
        "Using the right layer height is important to get successful prints:\n"
        "Thin layers may cause problems on adhesion, lamination, will print much slower and have no real visual benefits.\n"
        "Thick layers may not fully cure no matter the exposure time you use, causing lamination and other hazards. Read your resin datasheet to know the limits.\n"
        "Using layer height with too many decimal digits may produce a wrong positioning due stepper step loss and/or Z axis quality.\n"
        "Solution: Re-slice the model with proper layer height.");
    r.informational_only = true; // nothing to apply: the model has to be re-sliced
    if (ctx.count() == 0) return unavailable(r, "the file has no layers");
    if (const auto error = validate_suggestion_config(c); !error.empty()) return unavailable(r, with_validation(error));

    auto outside = [&](double height) {
        return r4(height) < r4(c.minimum_layer_height_mm) || r4(height) > r4(c.maximum_layer_height_mm) ||
               decimal_digits(height) > c.maximum_decimal_digits;
    };
    bool applied = !outside(job.layer_height_mm);
    for (size_t i = 0; applied && i < ctx.count(); ++i) applied = !outside(ctx.layer_height(i));

    r.status = applied ? SuggestionStatus::Applied : SuggestionStatus::NotApplied;
    r.message = applied
        ? std::string(k_ok) + r.title + ": " + num(job.layer_height_mm) + "mm"
        : std::string(k_warn) + r.title + " is out of the recommended " + num(c.minimum_layer_height_mm) + "mm" + k_arrow +
              num(c.maximum_layer_height_mm) + "mm, up to " + std::to_string(c.maximum_decimal_digits) + " decimal digit(s)";
    if (!applied) r.confirmation = r.title + ": Re-slice the model with proper layer height";
    return r;
}

// --------------------------------------------------------- TransitionLayerCount impl

struct TransitionMath
{
    const SuggestionJob &job;
    size_t count() const { return job.z_mm.size(); }
    double exposure(size_t i) const { return job.exposure_s[i]; }

    // UVtools MaximumPossibleTransitionLayerCount.
    size_t maximum_possible() const
    {
        if (job.bottom_layer_count == 0) return 0;
        const long layers = long(count()) - long(job.bottom_layer_count) - 1;
        return layers <= 0 ? 0 : size_t(layers);
    }
    // UVtools LastBottomLayer exposure, else the global bottom exposure.
    double last_bottom_exposure() const
    {
        if (job.bottom_layer_count == 0) return job.bottom_exposure_s;
        return exposure(std::min(job.bottom_layer_count, count()) - 1);
    }
    // Exposure of the first layer after the file's own transition, as used by the FromLayers helpers.
    double first_normal_exposure() const
    {
        const size_t tlc = job.transition_layer_count;
        if (tlc > 0 && job.bottom_layer_count + tlc <= count()) return exposure(job.bottom_layer_count + tlc - 1);
        return job.bottom_layer_count < count() ? exposure(job.bottom_layer_count) : job.normal_exposure_s;
    }
    static double step_time(double long_s, double short_s, size_t layers)
    {
        if (layers == 0 || long_s == short_s) return 0;
        return std::round((long_s - short_s) / double(layers + 1) * 100.) / 100.;
    }
    // UVtools ParseTransitionLayerCountFromLayers.
    size_t parse_count() const
    {
        size_t n = 0;
        for (size_t i = job.bottom_layer_count; i + 1 < count(); ++i) {
            if (exposure(i) < exposure(i + 1)) break;               // only decreasing exposure is a transition
            if (std::fabs(exposure(i) - exposure(i + 1)) < 0.009) break; // first equal layer ends it
            ++n;
        }
        return n > 0 ? n : job.transition_layer_count;
    }
    // UVtools ParseTransitionStepTimeFromLayers.
    double parse_step_time() const
    {
        if (count() < 3 || parse_count() == 0 || job.bottom_layer_count + 1 >= count()) return 0;
        return std::round((exposure(job.bottom_layer_count) - exposure(job.bottom_layer_count + 1)) * 100.) / 100.;
    }
    // UVtools GetTransitionLayerCountFromLayers(step, constrain: false), then the suggestion's clamp.
    size_t recommended(const TransitionLayerCountSuggestionConfig &c) const
    {
        size_t from_layers = 0;
        if (count() >= 3 && job.bottom_layer_count <= count() && c.transition_step_time_s != 0) {
            const double raw = std::round((last_bottom_exposure() - first_normal_exposure()) / c.transition_step_time_s - 1.);
            from_layers = raw <= 0 ? 0 : size_t(raw);
        }
        const size_t clamped = std::clamp<size_t>(from_layers, c.minimum_transition_layer_count, c.maximum_transition_layer_count);
        return std::min(clamped, maximum_possible());
    }
    double suggested_step_time(size_t layers) const
    {
        if (count() < 3 || job.bottom_layer_count > count()) return 0;
        return step_time(last_bottom_exposure(), first_normal_exposure(), layers);
    }
};

// UVtools SetTransitionLayers(count, resetExposureTimes: true) as exposure patches.
std::vector<SuggestedLayerChange> transition_exposure_changes(const SuggestionJob &job, size_t transition_layers)
{
    const TransitionMath m{job};
    std::vector<double> exposure = job.exposure_s;
    const double bottom_exposure = m.last_bottom_exposure();
    double normal_exposure = job.normal_exposure_s;
    std::vector<size_t> to_reset;
    for (size_t i = job.bottom_layer_count; i + 1 < exposure.size(); ++i) {
        if (std::fabs(exposure[i] - exposure[i + 1]) < 0.009) { normal_exposure = exposure[i]; break; }
        to_reset.push_back(i);
    }
    for (size_t i : to_reset) exposure[i] = normal_exposure; // old transition is replaced entirely

    const double decrement = TransitionMath::step_time(bottom_exposure, normal_exposure, transition_layers);
    if (transition_layers > 0 && decrement > 0) {
        size_t applied = 0;
        for (size_t i = job.bottom_layer_count; applied < transition_layers && i < exposure.size(); ++i) {
            ++applied;
            exposure[i] = std::clamp(bottom_exposure - decrement * double(applied), normal_exposure, bottom_exposure);
        }
    }
    std::vector<SuggestedLayerChange> changes;
    for (size_t i = 0; i < exposure.size(); ++i)
        if (std::fabs(exposure[i] - job.exposure_s[i]) > 1e-9)
            changes.push_back({i, LayerParameter::Exposure, job.exposure_s[i], exposure[i]});
    return changes;
}

SuggestionResult run_transition_layer_count(Context &ctx, const TransitionLayerCountSuggestionConfig &c)
{
    const SuggestionJob &job = ctx.job;
    SuggestionResult r = make_result("TransitionLayerCount", "Transition layers",
        "If you are printing flat on the build plate your model will print better when using a smooth transition exposure time instead of a harsh variation, resulting in reduced layer line effect and avoid possible problems due the large exposure difference.\n"
        "This is not so important when your model print raised under a raft/supports unaffected by the bottom exposure, in that case, it's fine to ignore this.");
    r.information_url = "https://ameralabs.com/blog/9-settings-to-change-for-faster-resin-3d-printing";
    if (!job.can_set_exposure || job.exposure_s.size() != ctx.count()) return unavailable(r, "the file format cannot store an exposure time per layer");
    if (ctx.count() == 0) return unavailable(r, "the file has no layers");
    if (const auto error = validate_suggestion_config(c); !error.empty()) return unavailable(r, with_validation(error));

    const TransitionMath m{job};
    const size_t suggested = m.recommended(c);
    const size_t actual = job.transition_in_firmware ? job.transition_layer_count : m.parse_count();

    // UVtools: a job that cannot hold a transition counts as applied.
    bool applied;
    if (ctx.count() < 3 || job.bottom_layer_count == 0 || job.bottom_exposure_s <= 0 || job.normal_exposure_s <= 0 ||
        job.bottom_exposure_s <= job.normal_exposure_s || job.bottom_layer_count + c.minimum_transition_layer_count > ctx.count() ||
        m.maximum_possible() < c.minimum_transition_layer_count)
        applied = true;
    else if (actual == suggested)
        applied = true;
    else if (c.apply_when == SuggestionApplyWhen::OutsideLimits)
        applied = actual >= c.minimum_transition_layer_count && actual <= c.maximum_transition_layer_count;
    else
        applied = false; // Different and not equal

    const double actual_step = job.transition_in_firmware
        ? TransitionMath::step_time(job.bottom_exposure_s, job.normal_exposure_s, job.transition_layer_count)
        : m.parse_step_time();
    const double suggested_step = m.suggested_step_time(suggested);
    const double bottom_exposure = m.last_bottom_exposure();
    double exposure = job.normal_exposure_s;
    const size_t layer_index = job.transition_layer_count > 0 ? job.bottom_layer_count + job.transition_layer_count : job.bottom_layer_count;
    if (layer_index < ctx.count()) exposure = m.exposure(layer_index);

    auto ramp = [&](double step, size_t layers) {
        return step <= 0 || layers == 0 ? std::string() : "[-" + num(step) + "s/" + num(double(layers)) + " layers]" + k_arrow;
    };
    const std::string before = num(bottom_exposure) + "s" + k_arrow + ramp(actual_step, actual) + num(exposure) + "s";
    const std::string after = num(bottom_exposure) + "s" + k_arrow + ramp(suggested_step, suggested) + num(exposure) + "s";

    r.status = applied ? SuggestionStatus::Applied : SuggestionStatus::NotApplied;
    if (applied) {
        r.message = std::string(k_ok) + r.title + ": " + before;
        return r;
    }
    r.message = std::string(k_warn) + r.title + " " + before + " is out of the recommended " + after;
    r.confirmation = r.title + ": (" + before + ")" + k_arrow + "(" + after + ")";
    r.transition_layer_count = std::make_pair(actual, suggested);
    if (!job.transition_in_firmware) r.layer_changes = transition_exposure_changes(job, suggested);
    return r;
}

// -------------------------------------------------------- WaitTimeAfterCure impl

double wait_after_value(const WaitTimeAfterCureSuggestionConfig &c, bool bottom, double exposure_s)
{
    if (c.set_type == WaitTimeAfterCureSuggestionConfig::SetType::Fixed) return bottom ? c.fixed_bottom_s : c.fixed_normal_s;
    return std::clamp(round2_even(exposure_s * c.proportional_wait_s / c.proportional_exposure_s),
                      bottom ? c.minimum_bottom_s : c.minimum_normal_s, bottom ? c.maximum_bottom_s : c.maximum_normal_s);
}

SuggestionResult run_wait_after_cure(Context &ctx, const WaitTimeAfterCureSuggestionConfig &c)
{
    const SuggestionJob &job = ctx.job;
    SuggestionResult r = make_result("WaitTimeAfterCure", "Wait time after cure",
        "Rest some time after curing the layer and before the lift sequence may allow the resin reaction to settle, harden and cool down a bit to detach more easily from the FEP, then yields less tension on the model, better print quality and easier lift's.");
    if (job.wait_after_s.size() != ctx.count() || ctx.count() == 0) return unavailable(r, "the file format cannot store a wait time after cure per layer");
    if (job.exposure_s.size() != ctx.count()) return unavailable(r, "the exposure time per layer is unknown");
    if (const auto error = validate_suggestion_config(c); !error.empty()) return unavailable(r, with_validation(error));

    const double first_bottom = wait_after_value(c, true, job.bottom_exposure_s);
    const double first_normal = wait_after_value(c, false, job.normal_exposure_s);
    bool applied = true;
    std::vector<SuggestedLayerChange> changes;
    for (size_t i = 0; i < ctx.count(); ++i) {
        const bool dummy = ctx.is_dummy(i);
        const double wanted = dummy ? 0. : wait_after_value(c, ctx.is_bottom(i), job.exposure_s[i]);
        const double have = job.wait_after_s[i];
        bool layer_ok;
        if (dummy) layer_ok = have <= 0.1;
        else if (c.apply_when == SuggestionApplyWhen::OutsideLimits)
            layer_ok = r4(have) >= r4(c.minimum_normal_s) && r4(have) <= r4(c.maximum_normal_s); // UVtools checks the normal limits for every layer
        else
            layer_ok = std::fabs(have - wanted) <= 0.1;
        if (!layer_ok) applied = false;
        if (std::fabs(have - wanted) > 1e-9) changes.push_back({i, LayerParameter::WaitAfter, have, wanted});
    }
    r.status = applied ? SuggestionStatus::Applied : SuggestionStatus::NotApplied;
    if (applied) {
        r.message = std::string(k_ok) + r.title + ": " + num(job.wait_after_s.front()) + "/" + num(job.wait_after_s.back()) + "s";
        return r;
    }
    r.message = std::string(k_warn) + r.title + " of " + num(job.wait_after_s.front()) + "/" + num(job.wait_after_s.back()) +
                "s is out of the recommended " + num(first_bottom) + "/" + num(first_normal) + "s";
    r.confirmation = r.title + ": " + num(job.wait_after_s.front()) + "/" + num(job.wait_after_s.back()) + "s" + k_arrow + num(first_bottom) + "/" + num(first_normal) + "s";
    r.layer_changes = std::move(changes);
    return r;
}

// ------------------------------------------------------- WaitTimeBeforeCure impl

// Port of SuggestionWaitTimeBeforeCure.CalculateWaitTime(layer). `current` holds the wait of
// every layer at this moment: UVtools applies layers in order, so the "previous layer" limit
// of the proportional modes sees already-updated values when a proposal is built.
double wait_before_value(Context &ctx, const WaitTimeBeforeCureSuggestionConfig &c, size_t i,
                         const std::vector<double> &current)
{
    using T = WaitTimeBeforeCureSuggestionConfig::SetType;
    using M = WaitTimeBeforeCureSuggestionConfig::MassFrom;
    const SuggestionJob &job = ctx.job;
    if (ctx.is_dummy(i)) return 0.; // not exposed: no need to wait

    const bool bottom = ctx.is_bottom(i) || (c.bottom_height_mm > 0 && r4(job.z_mm[i]) <= r4(c.bottom_height_mm));
    const double fixed = bottom ? c.fixed_bottom_s : c.fixed_normal_s;

    if (c.set_type == T::Fixed) {
        if (bottom || c.transition_layer_count == 0) return fixed;
        // Ramp from the bottom wait down to the normal wait over the layers after the bottom height.
        size_t first_normal = ctx.count();
        for (size_t k = 0; k < ctx.count(); ++k)
            if (r4(job.z_mm[k]) > r4(c.bottom_height_mm)) { first_normal = k; break; }
        if (first_normal < ctx.count() && i >= first_normal && i <= first_normal + c.transition_layer_count) {
            const double decrement = round2_even((c.fixed_bottom_s - c.fixed_normal_s) / double(c.transition_layer_count + 1));
            return round2_even(std::max(c.fixed_bottom_s - decrement * double(i - first_normal + 1), c.minimum_normal_s));
        }
        return fixed;
    }

    const auto &stats = ctx.stats();
    auto mass_of = [&](size_t layer) {
        return c.set_type == T::ProportionalLayerPixels ? double(stats[layer].nonzero_pixels)
                                                         : ctx.pixel_area_mm2() * double(stats[layer].nonzero_pixels);
    };

    double mass = 0;
    if (i > 0 && c.mass_from != M::Previous && c.mass_relative_height_mm > 0) {
        size_t samples = 0;
        for (size_t k = i; k-- > 0 && job.z_mm[i] - job.z_mm[k] <= c.mass_relative_height_mm + 1e-6;) {
            if (ctx.is_dummy(k)) continue;
            ++samples;
            if (c.mass_from == M::Average) mass += mass_of(k);
            else mass = std::max(mass_of(k), mass);
        }
        if (c.mass_from == M::Average && mass > 0 && samples > 0) mass /= double(samples);
    }
    if (mass <= 0) {
        auto source = ctx.previous_with_pixels(i, 2);
        if (!source) source = ctx.next_with_pixels(i, 2);
        if (!source) return fixed; // nothing to measure
        mass = mass_of(*source);
    }

    double value;
    if (c.set_type == T::ProportionalLayerPixels)
        value = mass * (bottom ? c.proportional_bottom_s / c.proportional_bottom_layer_pixels : c.proportional_normal_s / c.proportional_layer_pixels);
    else
        value = mass * (bottom ? c.proportional_bottom_s / c.proportional_bottom_layer_area_mm2 : c.proportional_normal_s / c.proportional_layer_area_mm2);

    const double max_difference = bottom ? c.proportional_bottom_max_difference_s : c.proportional_normal_max_difference_s;
    if (max_difference > 0)
        if (const auto previous = ctx.previous_with_pixels(i, 2))
            value = std::clamp(value, std::max(0., current[*previous] - max_difference), current[*previous] + max_difference);

    return std::clamp(round2_even(value), bottom ? c.minimum_bottom_s : c.minimum_normal_s,
                      bottom ? c.maximum_bottom_s : c.maximum_normal_s);
}

SuggestionResult run_wait_before_cure(Context &ctx, const WaitTimeBeforeCureSuggestionConfig &c)
{
    using T = WaitTimeBeforeCureSuggestionConfig::SetType;
    const SuggestionJob &job = ctx.job;
    SuggestionResult r = make_result("WaitTimeBeforeCure", "Wait time before cure",
        "Rest some time before cure the layer is crucial to let the resin settle after the lift sequence and allow some time for the arm settle at the correct Z position as the resin will offer some resistance and push the structure.\n"
        "This lead to better quality with more successful prints, less lamination problems, better first layers with more success of stick to the build plate and less elephant foot effect.");
    r.information_url = "https://blog.honzamrazek.cz/2022/01/prints-not-sticking-to-the-build-plate-layer-separation-rough-surface-on-a-resin-printer-resin-viscosity-the-common-denominator";
    if (job.wait_before_s.size() != ctx.count() || ctx.count() == 0) return unavailable(r, "the file format cannot store a wait time before cure per layer");
    if (job.exposure_s.size() != ctx.count()) return unavailable(r, "the exposure time per layer is unknown");
    if (c.set_type != T::Fixed && !job.stats) return unavailable(r, "the layer masks are needed for the proportional modes");
    if (c.set_type == T::ProportionalLayerArea && ctx.pixel_area_mm2() <= 0) return unavailable(r, "the display pixel size is unknown");
    if (const auto error = validate_suggestion_config(c); !error.empty()) return unavailable(r, with_validation(error));

    // IsApplied: compare against what the rule gives for the CURRENT neighbour values.
    bool applied = true;
    for (size_t i = 0; i < ctx.count() && applied; ++i) {
        const double have = job.wait_before_s[i];
        if (ctx.is_dummy(i)) { applied = have <= 0.1; continue; }
        if (c.apply_when == SuggestionApplyWhen::OutsideLimits)
            applied = r4(have) >= r4(c.minimum_normal_s) && r4(have) <= r4(c.maximum_normal_s); // normal limits for every layer, as UVtools
        else
            applied = std::fabs(have - wait_before_value(ctx, c, i, job.wait_before_s)) <= 0.1;
    }

    // Proposal: apply layer after layer so each one sees the already updated previous layer.
    std::vector<double> proposed = job.wait_before_s;
    for (size_t i = 0; i < ctx.count(); ++i) proposed[i] = wait_before_value(ctx, c, i, proposed);
    const double bottom_wait = proposed.front(), normal_wait = proposed.back();

    r.status = applied ? SuggestionStatus::Applied : SuggestionStatus::NotApplied;
    if (applied) {
        r.message = std::string(k_ok) + r.title + ": " + num(job.wait_before_s.front()) + "/" + num(job.wait_before_s.back()) + "s";
        return r;
    }
    r.message = std::string(k_warn) + r.title + " of " + num(job.wait_before_s.front()) + "/" + num(job.wait_before_s.back()) +
                "s is out of the recommended " + num(bottom_wait) + "/" + num(normal_wait) + "s";
    r.confirmation = r.title + ": " + num(job.wait_before_s.front()) + "/" + num(job.wait_before_s.back()) + "s" + k_arrow + num(bottom_wait) + "/" + num(normal_wait) + "s";
    for (size_t i = 0; i < ctx.count(); ++i)
        if (std::fabs(proposed[i] - job.wait_before_s[i]) > 1e-9)
            r.layer_changes.push_back({i, LayerParameter::WaitBefore, job.wait_before_s[i], proposed[i]});
    if (c.create_empty_first_layer && job.can_prepend_blank_layer && !ctx.is_dummy(0)) {
        r.prepend_blank_first_layer = true;
        if (job.bottom_layer_count > 0 && job.bottom_layer_count <= 7) // the dummy layer counts as a bottom layer
            r.bottom_layer_count = std::make_pair(job.bottom_layer_count, job.bottom_layer_count + 1);
    }
    return r;
}

// ------------------------------------------------------------ ModelPosition impl

struct Rect
{
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    BoundingBox to_bbox() const { return w > 0 && h > 0 ? BoundingBox(Point(x, y), Point(x + w - 1, y + h - 1)) : BoundingBox(); }
};

std::string rect_text(const Rect &r)
{
    return "{X=" + std::to_string(r.x) + ", Y=" + std::to_string(r.y) + ", Width=" + std::to_string(r.w) + ", Height=" + std::to_string(r.h) + "}";
}
std::string point_text(int x, int y) { return "{X=" + std::to_string(x) + ",Y=" + std::to_string(y) + "}"; }

// UVtools TopBottomMargin / LeftRightMargin: the target clamped to the limits, or half of
// the free space when the model is too large for two margins.
int effective_margin(int resolution, int model_size, int target, int minimum, int maximum)
{
    const int margin = std::clamp(target, minimum, maximum);
    if (resolution - model_size < margin * 2)
        return int(std::round(double(std::max(0, resolution - model_size)) / 2.));
    return margin;
}

SuggestionResult run_model_position(Context &ctx, const ModelPositionSuggestionConfig &c)
{
    using A = ModelPositionSuggestionConfig::Anchor;
    const SuggestionJob &job = ctx.job;
    SuggestionResult r = make_result("ModelPosition", "Model position",
        "Printing on a corner will reduce the FEP stretch forces when detaching from the model during a lift sequence, benefits are: Reduced lift height and faster printing, less stretch, less FEP marks, better FEP lifespan, easier to peel, less prone to failure and use the screen pixels more evenly.\n"
        "If the model is too large to fit within the margin(s) on the screen, it will attempt to center it on that same axis to avoid touching on screen edge(s) and to give a sane margin from it.");
    r.information_url = "https://ameralabs.com/blog/9-settings-to-change-for-faster-resin-3d-printing";
    if (!job.stats) return unavailable(r, "the layer masks are needed to find the model");
    if (job.resolution_x == 0 || job.resolution_y == 0) return unavailable(r, "the screen resolution is unknown");
    if (ctx.count() == 0) return unavailable(r, "the file has no layers");
    if (const auto error = validate_suggestion_config(c); !error.empty()) return unavailable(r, with_validation(error));

    // UVtools FileFormat.BoundingRectangle: union of the layers' bounding rectangles.
    BoundingBox model;
    for (const SuggestionLayerStats &s : ctx.stats()) if (s.bbox.defined) model.merge(s.bbox);
    if (!model.defined) return unavailable(r, "the layers contain no model");
    const Rect box{int(model.min.x()), int(model.min.y()), int(model.max.x() - model.min.x() + 1), int(model.max.y() - model.min.y() + 1)};
    const int res_x = int(job.resolution_x), res_y = int(job.resolution_y);
    if (box.w > res_x || box.h > res_y) return unavailable(r, "the model is larger than the screen");

    const int lr = effective_margin(res_x, box.w, c.target_left_right_margin_px, c.minimum_left_right_margin_px, c.maximum_left_right_margin_px);
    const int tb = effective_margin(res_y, box.h, c.target_top_bottom_margin_px, c.minimum_top_bottom_margin_px, c.maximum_top_bottom_margin_px);

    bool applied;
    if (box.w == res_x && box.h == res_y) {
        applied = true; // fills the whole screen: there is nowhere to move it
    } else {
        SuggestionApplyWhen apply_when = c.apply_when;
        if (res_x - box.w < c.minimum_left_right_margin_px * 2 || res_y - box.h < c.minimum_top_bottom_margin_px * 2)
            apply_when = SuggestionApplyWhen::Different; // too large for the limits: only the exact corner counts
        const int min_lr = c.minimum_left_right_margin_px, max_lr = c.maximum_left_right_margin_px;
        const int min_tb = c.minimum_top_bottom_margin_px, max_tb = c.maximum_top_bottom_margin_px;
        if (apply_when == SuggestionApplyWhen::OutsideLimits) {
            const bool left = box.x >= min_lr && box.x <= max_lr, top = box.y >= min_tb && box.y <= max_tb;
            const bool right = box.right() <= res_x - min_lr && box.right() >= res_x - max_lr;
            const bool bottom = box.bottom() <= res_y - min_tb && box.bottom() >= res_y - max_tb;
            applied = (left && top) || (right && top) || (right && bottom) || (left && bottom);
        } else {
            const bool left = box.x == lr, top = box.y == tb;
            const bool right = box.right() == res_x - lr, bottom = box.bottom() == res_y - tb;
            applied = (left && top) || (right && top) || (right && bottom) || (left && bottom);
        }
    }

    r.status = applied ? SuggestionStatus::Applied : SuggestionStatus::NotApplied;
    if (applied) {
        r.message = std::string(k_ok) + r.title + ": " + rect_text(box);
        return r;
    }

    // UVtools picks a random corner for Random; a deterministic proposal is more useful here.
    A anchor = c.anchor;
    if (anchor == A::Random) {
        const bool left = box.x + box.w / 2 < res_x / 2, top = box.y + box.h / 2 < res_y / 2;
        anchor = top ? (left ? A::TopLeft : A::TopRight) : (left ? A::BottomLeft : A::BottomRight);
    }
    Rect target = box;
    const char *anchor_name = "top_left";
    switch (anchor) {
    case A::TopRight:    target.x = res_x - lr - box.w; target.y = tb; anchor_name = "top_right"; break;
    case A::BottomRight: target.x = res_x - lr - box.w; target.y = res_y - tb - box.h; anchor_name = "bottom_right"; break;
    case A::BottomLeft:  target.x = lr; target.y = res_y - tb - box.h; anchor_name = "bottom_left"; break;
    default:             target.x = lr; target.y = tb; break;
    }
    // Position text uses the anchor point as UVtools does (the corner the model's own corner goes to).
    int px = lr, py = tb;
    if (anchor == A::TopRight || anchor == A::BottomRight) px = res_x - lr;
    if (anchor == A::BottomLeft || anchor == A::BottomRight) py = res_y - tb;

    r.message = std::string(k_warn) + r.title + ": " + point_text(box.x, box.y) + " is out of the recommended " + point_text(px, py);
    r.confirmation = r.title + ": " + point_text(box.x, box.y) + k_arrow + point_text(px, py) + " (" + anchor_name + ")";
    SuggestedModelMove move;
    move.dx_px = target.x - box.x;
    move.dy_px = target.y - box.y;
    move.anchor = anchor_name;
    move.before = box.to_bbox();
    move.after = target.to_bbox();
    r.model_move = std::move(move);
    return r;
}

} // namespace

// ---------------------------------------------------------------- public API

SuggestionResult suggest_bottom_layer_count(const SuggestionJob &job, const BottomLayerCountSuggestionConfig &config)
{
    Context ctx(job, {});
    return run_bottom_layer_count(ctx, config);
}

SuggestionResult suggest_layer_height(const SuggestionJob &job, const LayerHeightSuggestionConfig &config)
{
    Context ctx(job, {});
    return run_layer_height(ctx, config);
}

SuggestionResult suggest_transition_layer_count(const SuggestionJob &job, const TransitionLayerCountSuggestionConfig &config)
{
    Context ctx(job, {});
    return run_transition_layer_count(ctx, config);
}

SuggestionResult suggest_wait_time_before_cure(const SuggestionJob &job, const WaitTimeBeforeCureSuggestionConfig &config,
                                               const std::function<bool()> &cancel)
{
    Context ctx(job, cancel);
    return run_wait_before_cure(ctx, config);
}

SuggestionResult suggest_wait_time_after_cure(const SuggestionJob &job, const WaitTimeAfterCureSuggestionConfig &config)
{
    // The dummy-layer rule uses mask statistics when a provider exists.
    Context ctx(job, {});
    return run_wait_after_cure(ctx, config);
}

SuggestionResult suggest_model_position(const SuggestionJob &job, const ModelPositionSuggestionConfig &config,
                                        const std::function<bool()> &cancel)
{
    Context ctx(job, cancel);
    return run_model_position(ctx, config);
}

std::vector<SuggestionResult> evaluate_suggestions(const SuggestionJob &job, const SuggestionConfig &config,
                                                   const std::function<bool()> &cancel)
{
    // The masks are decoded once; the evaluators then read the same cached statistics.
    Context ctx(job, cancel);
    std::vector<SuggestionResult> results;
    if (job.stats && (config.wait_time_before_cure.enabled || config.wait_time_after_cure.enabled || config.model_position.enabled))
        ctx.stats();

    if (config.bottom_layer_count.enabled) results.push_back(run_bottom_layer_count(ctx, config.bottom_layer_count));
    if (config.layer_height.enabled) results.push_back(run_layer_height(ctx, config.layer_height));
    if (config.transition_layer_count.enabled) results.push_back(run_transition_layer_count(ctx, config.transition_layer_count));
    if (config.wait_time_before_cure.enabled) results.push_back(run_wait_before_cure(ctx, config.wait_time_before_cure));
    if (config.wait_time_after_cure.enabled) results.push_back(run_wait_after_cure(ctx, config.wait_time_after_cure));
    if (config.model_position.enabled) results.push_back(run_model_position(ctx, config.model_position));
    return results;
}

}} // namespace Slic3r::sla
