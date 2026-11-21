// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RepresentationAdjustment.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace Slic3r::sla {
namespace {
std::string number(double value)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(6);
    text << value;
    return text.str();
}
std::string exact(double value)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(12);
    text << value;
    return text.str();
}
double read(const DynamicPrintConfig &config, const char *key)
{
    const auto *option = config.option(key);
    if (!option) return 0.;
    return option->type() == coInt ? double(option->getInt()) : option->getFloat();
}

struct Rule {
    double step = 1;     // representable grid
    double minimum = 1;  // smallest storable value
    double maximum = 65535;
    bool floor = false;  // speeds never go faster than requested
    const char *minimum_text = "1";
    const char *grid_text = "whole numbers";
    bool float_safe = false; // the writer re-reads the value through a float: pick one that survives it (Klipper)
};
struct Rounded { double to; bool changed; std::string reason; bool overflow; };

Rounded round_to(double value, const Rule &rule, const std::string &unit, const std::string &name = "CXDLP")
{
    Rounded result{value, false, {}, false};
    if (!std::isfinite(value)) { result.overflow = true; return result; }
    double to = rule.floor ? std::floor(value / rule.step + 1e-9) * rule.step : std::round(value / rule.step) * rule.step;
    std::string reason = name + " stores " + rule.grid_text;
    if (value < rule.minimum - 1e-9 || to < rule.minimum) {
        to = rule.minimum;
        reason = name + " minimum is " + rule.minimum_text;
    }
    if (to > rule.maximum + 1e-9) { result.overflow = true; return result; }
    if (rule.float_safe && std::abs(double(float(to)) - to) >= 1e-6) {
        // Nearest grid value whose float image still matches within the writer's tolerance.
        bool found = false;
        for (int k = 1; k < 400 && !found; ++k)
            for (const int sign : {-1, 1}) {
                if (rule.floor && sign > 0) continue;
                const double candidate = std::round((to + sign * k * rule.step) / rule.step) * rule.step;
                if (candidate >= rule.minimum - 1e-9 && candidate <= rule.maximum && std::abs(double(float(candidate)) - candidate) < 1e-6) {
                    to = candidate; found = true; break;
                }
            }
    }
    result.to = to;
    result.changed = std::abs(to - value) > 1e-6;
    result.reason = reason;
    (void) unit;
    return result;
}

// Planner state: collects adjustments, merging identical normal/bottom entries.
struct Planner {
    const DynamicPrintConfig &config;
    RepresentationPlan plan;
    std::string name = "CXDLP"; // used in reasons and blocker texts
    std::map<std::string, double> final_; // value each adjusted key will have
    explicit Planner(const DynamicPrintConfig &c) : config(c) {}

    // Value a key has once every adjustment planned so far is applied.
    double val(const std::string &key) const
    {
        const auto found = final_.find(key);
        return found != final_.end() ? found->second : read(config, key.c_str());
    }
    void add(std::vector<std::string> keys, const std::string &label, const std::string &unit, const std::string &from,
             const std::string &to, const std::string &reason, const std::string &value)
    {
        const double numeric = std::atof(value.c_str());
        for (const auto &key : keys) final_[key] = numeric;
        plan.adjustments.push_back({std::move(keys), label, from, to, unit, reason, value});
    }
    void blocker(const std::string &text) { plan.blockers.push_back(text); }
    void overflow(const std::string &label, double value, const std::string &unit, const Rule &rule)
    {
        blocker(label + " " + number(value) + " " + unit + " is outside what " + name + " can store (up to " +
                number(rule.maximum) + " " + unit + "); change it in the material settings.");
    }
    // Independent normal and bottom fields (or one only). `display_scale` converts a stored key value to
    // the displayed one (1 for everything here).
    void pair(const std::string &normal, const std::string &bottom, const std::string &label, const std::string &unit, const Rule &rule,
              bool integer_key = false)
    {
        const double vn = val(normal), vb = val(bottom);
        const auto rn = round_to(vn, rule, unit, name), rb = round_to(vb, rule, unit, name);
        if (rn.overflow) { overflow(label, vn, unit, rule); }
        if (rb.overflow && bottom != normal) { overflow("Bottom " + label, vb, unit, rule); }
        if (rn.overflow || rb.overflow) return;
        auto text = [&](double v) { return integer_key ? std::to_string(long(std::llround(v))) : exact(v); };
        if (rn.changed && rb.changed && std::abs(rn.to - rb.to) < 1e-9 && std::abs(vn - vb) < 1e-9) {
            add({normal, bottom}, label, unit, number(vn), number(rn.to), rn.reason, text(rn.to));
            return;
        }
        if (rn.changed) add({normal}, label + " (normal layers)", unit, number(vn), number(rn.to), rn.reason, text(rn.to));
        if (rb.changed) add({bottom}, label + " (bottom layers)", unit, number(vb), number(rb.to), rb.reason, text(rb.to));
    }
    // One stored value for normal and bottom layers.
    void shared(const std::string &normal, const std::string &bottom, const std::string &label, const std::string &unit, const Rule &rule)
    {
        const double vn = val(normal), vb = val(bottom);
        const auto rn = round_to(vn, rule, unit, name), rb = round_to(vb, rule, unit, name);
        if (rn.overflow) overflow(label, vn, unit, rule);
        if (rb.overflow) overflow("Bottom " + label, vb, unit, rule);
        if (rn.overflow || rb.overflow) return;
        if (std::abs(rn.to - rb.to) > 1e-9) {
            blocker(name + " stores one " + label + " for the whole job, but normal layers use " + number(vn) + " " + unit +
                    " and bottom layers " + number(vb) + " " + unit + "; set the same value in the material settings.");
            return;
        }
        if (!rn.changed && !rb.changed) return;
        const std::string from = std::abs(vn - vb) < 1e-9 ? number(vn) : number(vn) + " / " + number(vb);
        add({normal, bottom}, label, unit, from, number(rn.to), rn.changed ? rn.reason : rb.reason, exact(rn.to));
    }
    void single(const std::string &key, const std::string &label, const std::string &unit, const Rule &rule, bool integer_key = false)
    {
        pair(key, key, label, unit, rule, integer_key);
    }
    // The container has no field for this value and always means `target` (UVtools reads that constant).
    void fixed(const std::string &normal, const std::string &bottom, const std::string &label, const std::string &unit,
               double target, const std::string &why, bool integer_key = false)
    {
        const double vn = val(normal), vb = val(bottom);
        auto text = [&](double v) { return integer_key ? std::to_string(long(std::llround(v))) : exact(v); };
        const bool cn = std::abs(vn - target) > 1e-6, cb = std::abs(vb - target) > 1e-6;
        if (normal == bottom) {
            if (cn) add({normal}, label, unit, number(vn), number(target), why, text(target));
            return;
        }
        if (cn && cb && std::abs(vn - vb) < 1e-9) add({normal, bottom}, label, unit, number(vn), number(target), why, text(target));
        else {
            if (cn) add({normal}, label + " (normal layers)", unit, number(vn), number(target), why, text(target));
            if (cb) add({bottom}, label + " (bottom layers)", unit, number(vb), number(target), why, text(target));
        }
    }
    // A value no adjustment may change (second stage, rests, transitions): reported as a blocker.
    void must_be_zero(const std::string &key, const std::string &label)
    {
        const double v = val(key);
        if (std::abs(v) > 1e-9)
            blocker(name + " cannot store the " + label + " (" + number(v) + "); set it to 0 in the material settings.");
    }
    // UV power clamped to the storable range.
    void clamp_pwm(const std::string &key, const std::string &label, double lo, double hi)
    {
        const double v = val(key);
        const double to = std::min(hi, std::max(lo, std::round(v)));
        if (std::abs(to - v) > 1e-9)
            add({key}, label, "", number(v), number(to), "UV power is a value from " + number(lo) + " to " + number(hi), std::to_string(long(to)));
    }
    // UV power the container cannot store at all: it always writes the full value.
    void pwm_full(const std::string &key, const std::string &label)
    {
        const double v = val(key);
        if (std::abs(v - 255) > 1e-9)
            blocker(name + " cannot store a custom UV power (" + label + " is " + number(v) + "); set 255 in the material settings.");
    }
};
} // namespace

std::string RepresentationAdjustment::describe() const
{
    const std::string u = unit.empty() ? std::string() : " " + unit;
    return label + " " + from + u + " -> " + to + u + " (" + reason + ")";
}

std::string describe_adjustments(const RepresentationApproval &adjustments)
{
    std::string text;
    for (const auto &adjustment : adjustments) text += (text.empty() ? "" : "; ") + adjustment.describe();
    return text;
}

std::string RepresentationPlan::strict_message(const std::string &format_name) const
{
    std::string text = format_name + " cannot store this job as it is:";
    for (const auto &blocker : blockers) text += "\n - " + blocker;
    for (const auto &adjustment : adjustments) text += "\n - " + adjustment.describe();
    if (!adjustments.empty())
        text += blockers.empty() ? "\nChange these in the material/printer settings, export from the application window (it offers to write the adjusted values), or pass --sla-accept-adjustments on the command line."
                                 : "\nThe adjustable items can be written with adjusted values (application window or --sla-accept-adjustments); the others must be changed in the settings.";
    return text;
}

bool has_representation_rules(const std::string &format)
{
    static const char *const ids[] = {"cxdlp", "cxdlpv2", "cxdlpv1", "mdlp", "gr1", "n4", "n7", "qdt", "svgx", "jxs", "zcode",
                                      "zcodex", "vdt", "klipper", "klipper_rgb", "ctb_v3"};
    return std::find(std::begin(ids), std::end(ids), format) != std::end(ids);
}

std::string representation_format_label(const std::string &format)
{
    if (format == "cxdlp" || format == "cxdlpv2") return "CXDLP";
    if (format == "cxdlpv1") return "CXDLP v1";
    if (format == "mdlp") return "Makerbase MDLP";
    if (format == "gr1") return "GR1 Workshop";
    if (format == "n4") return "Anet N4";
    if (format == "n7") return "Anet N7";
    if (format == "qdt") return "Emake3D Galaxy (QDT)";
    if (format == "svgx") return "Flashforge SVGX";
    if (format == "jxs") return "Uniformation GKone (JXS)";
    if (format == "zcode") return "UnizMaker IBEE (ZCode)";
    if (format == "zcodex") return "Z-Suite ZCodex";
    if (format == "vdt") return "Voxeldance Tango (VDT)";
    if (format == "klipper" || format == "klipper_rgb") return "Klipper MSLA";
    if (format == "ctb_v3") return "Chitubox CTB v3";
    return format;
}

std::string creality_model_for_resolution(unsigned columns, unsigned rows)
{
    // Order and resolutions follow UVtools' machine list (first match wins) and the bundled Creality profiles.
    struct Machine { const char *model; unsigned x, y; };
    static const Machine machines[] = {
        {"CT-005", 3840, 2400}, {"CL-89L", 3840, 2400}, {"CL-133", 3840, 2160}, {"CL-60", 1620, 2560},
        {"CL-79", 4320, 2560}, {"CL-70", 2560, 2400}, {"CL-89", 3840, 2400}, {"CL-92", 5760, 3600},
        {"CL925", 5760, 3600}, {"CL-103L", 7680, 4320}, {"CL-103", 7680, 4320}};
    for (const auto &machine : machines)
        if (machine.x == columns && machine.y == rows) return machine.model;
    return {};
}

namespace {
RepresentationPlan plan_cxdlp(const std::string &format, const DynamicPrintConfig &c)
{
    const int version = format == "cxdlpv1" ? 1 : format == "cxdlpv2" ? 2 : 3;
    const std::string name = version == 1 ? "CXDLP v1" : "CXDLP v2/v3";
    Planner p(c);
    Rule whole;                              // 1..65535 whole numbers
    whole.grid_text = "whole numbers";
    auto seconds = [&](double minimum, double step, const char *grid) {
        Rule r; r.step = step; r.minimum = minimum; r.minimum_text = minimum >= 1 ? "1 s" : "0.1 s"; r.grid_text = grid; return r;
    };
    // Exposure: tenths of a second for v2/v3 normal exposure, whole seconds elsewhere.
    if (version == 1) p.single("exposure_time", "Exposure", "s", seconds(1, 1, "whole seconds"));
    else {
        Rule tenths = seconds(.1, .1, "tenths of a second"); tenths.maximum = 6553.5;
        p.single("exposure_time", "Exposure", "s", tenths);
    }
    p.single("initial_exposure_time", "Bottom exposure", "s", seconds(1, 1, "whole seconds"));
    // Wait before exposure: UVtools raises 0 to 1 and the firmware refuses a job without it.
    p.shared("sla_wait_before_cure", "sla_bottom_wait_before_cure", "Wait before exposure", "s", seconds(1, 1, "whole seconds"));
    Rule height; height.minimum_text = "1 mm"; height.grid_text = "whole millimetres";
    p.single("sla_bottom_lift_height", "Bottom lift height", "mm", height);
    p.single("sla_lift_height", "Lift height", "mm", height);
    Rule speed; speed.floor = true;
    if (version == 1) {
        speed.minimum_text = "1 mm/min"; speed.grid_text = "whole mm/min";
    } else {
        speed.step = 60; speed.minimum = 60; speed.maximum = 65535. * 60; speed.minimum_text = "60 mm/min (1 mm/s)"; speed.grid_text = "whole mm/s";
    }
    p.single("sla_bottom_lift_speed", "Bottom lift speed", "mm/min", speed);
    p.single("sla_lift_speed", "Lift speed", "mm/min", speed);
    p.shared("sla_retract_speed", "sla_bottom_retract_speed", "Retract speed", "mm/min", speed);
    // UV power: a byte.
    for (const auto &[key, label] : {std::pair<const char *, const char *>{"sla_bottom_light_pwm", "Bottom UV power"}, {"sla_light_pwm", "UV power"}}) {
        const double v = read(c, key);
        const double to = std::min(255., std::max(0., std::round(v)));
        if (std::abs(to - v) > 1e-9)
            p.add({key}, label, "", number(v), number(to), "UV power is a value from 0 to 255", std::to_string(long(to)));
    }
    // Printer identity (v2/v3 header only).
    if (version != 1) {
        const std::string model = c.opt_string("printer_model");
        const bool valid = model.size() <= 255 && model.find('\0') == std::string::npos &&
            (model.compare(0, 2, "CL") == 0 || model.compare(0, 2, "CT") == 0);
        if (!valid) {
            size_t columns = size_t(c.opt_int("display_pixels_x")), rows = size_t(c.opt_int("display_pixels_y"));
            if (c.option("display_orientation") && c.option("display_orientation")->getInt() == sladoPortrait) std::swap(columns, rows);
            const std::string match = creality_model_for_resolution(unsigned(columns), unsigned(rows));
            if (match.empty())
                p.plan.blockers.push_back("CXDLP needs a Creality printer model (starting with CL or CT) but \"" + model +
                    "\" is set and no Creality machine has the resolution " + std::to_string(columns) + "x" + std::to_string(rows) +
                    ". Set Printer > General > Output > Printer model (firmware) to your Creality model, e.g. CL-60.");
            else
                p.add({"printer_model"}, "Printer model", "", model.empty() ? "(empty)" : model, match,
                      "matched by " + std::to_string(columns) + "x" + std::to_string(rows) + " resolution", match);
        }
    }
    // Things no adjustment can change.
    {
        double width = c.opt_float("display_width"), height = c.opt_float("display_height");
        size_t columns = size_t(c.opt_int("display_pixels_x")), rows = size_t(c.opt_int("display_pixels_y"));
        if (c.option("display_orientation") && c.option("display_orientation")->getInt() == sladoPortrait) {
            std::swap(width, height); std::swap(columns, rows);
        }
        if (!columns || !rows || columns > 16384 || rows > 8192)
            p.plan.blockers.push_back(name + " stores a resolution up to 16384x8192, but the printer has " + std::to_string(columns) + "x" + std::to_string(rows) + ".");
        if (std::abs(width * 100 - std::round(width * 100)) > 1e-6 || std::abs(height * 1000 - std::round(height * 1000)) > 1e-6)
            p.plan.blockers.push_back(name + " stores the display width with 2 decimals and the height with 3, but the printer has " + number(width) + " x " + number(height) + " mm.");
    }
    if (c.opt_int("sla_transition_layers") != 0)
        p.plan.blockers.push_back(name + " cannot store transition exposures; set Transition layers to 0 in the material settings.");
    for (const bool bottom : {false, true})
        for (const auto &[suffix, label] : {std::pair<const char *, const char *>{"lift_height2", "second lift height"},
                {"retract_height2", "second retract height"}, {"wait_after_cure", "wait after exposure"}, {"wait_after_lift", "wait after lift"}}) {
            const std::string key = std::string(bottom ? "sla_bottom_" : "sla_") + suffix;
            if (read(c, key.c_str()) != 0)
                p.plan.blockers.push_back(name + " cannot store the " + (bottom ? "bottom " : "normal ") + label + " (" +
                                          number(read(c, key.c_str())) + "); set it to 0 in the material settings.");
        }
    return std::move(p.plan);
}


Rule grid(double step, double minimum, double maximum, bool floor, const char *grid_text, const char *minimum_text)
{
    Rule r; r.step = step; r.minimum = minimum; r.maximum = maximum; r.floor = floor; r.grid_text = grid_text; r.minimum_text = minimum_text;
    return r;
}
std::string key_n(const char *suffix) { return std::string("sla_") + suffix; }
std::string key_b(const char *suffix) { return std::string("sla_bottom_") + suffix; }
std::string cat(const char *a, const std::string &b) { return std::string(a) + b; }

// Second stage, rests the container has no field for, and a transition count: no safe replacement, refused together.
void no_second_stage(Planner &p, bool waits_before = false)
{
    p.must_be_zero("sla_transition_layers", "transition layers");
    for (const bool bottom : {false, true}) {
        const char *who = bottom ? "bottom " : "normal ";
        for (const auto &[suffix, label] : {std::pair<const char *, const char *>{"lift_height2", "second lift height"},
                {"retract_height2", "second retract height"}, {"wait_after_cure", "wait after exposure"}, {"wait_after_lift", "wait after lift"}})
            p.must_be_zero(bottom ? key_b(suffix) : key_n(suffix), cat(who, label));
        if (waits_before) p.must_be_zero(bottom ? key_b("wait_before_cure") : key_n("wait_before_cure"), cat(who, "wait before exposure"));
    }
}
void layer_height_rules(Planner &p, double step_text_scale, bool same_initial)
{
    const double lh = read(p.config, "layer_height");
    if (lh > 0 && std::abs(lh * step_text_scale - std::round(lh * step_text_scale)) > 1e-5)
        p.blocker(p.name + " stores the layer height with a precision of " + number(1. / step_text_scale) + " mm, but it is " + number(lh) +
                  " mm; change the layer height in the print settings.");
    const double initial = read(p.config, "initial_layer_height");
    if (same_initial && p.config.option("initial_layer_height") && std::abs(initial - lh) > 1e-7)
        p.blocker(p.name + " stores one layer height, but the first layer is " + number(initial) + " mm and the others " + number(lh) +
                  " mm; set the same value in the print/material settings.");
}
// All of the motion fields on a decimal grid (normal and bottom), with the cross-field checks that follow.
void decimal_motion(Planner &p, double max_value, double speed_step, bool float_safe = false, bool floor_speeds = true)
{
    Rule dist = grid(.01, 0, max_value, false, "at most two decimals", "0");
    Rule wait = grid(.01, 0, max_value, false, "at most two decimals", "0");
    Rule speed = grid(speed_step, 0, max_value, floor_speeds, speed_step >= 1 ? "whole numbers" : speed_step >= .1 ? "one decimal" : "at most two decimals", "0");
    if (float_safe) { dist.float_safe = wait.float_safe = speed.float_safe = true; }
    for (const auto &[suffix, label] : {std::pair<const char *, const char *>{"lift_height", "Lift height"}, {"lift_height2", "Second lift height"}, {"retract_height2", "Second retract height"}})
        p.pair(key_n(suffix), key_b(suffix), label, "mm", dist);
    for (const auto &[suffix, label] : {std::pair<const char *, const char *>{"lift_speed", "Lift speed"}, {"lift_speed2", "Second lift speed"},
            {"retract_speed", "Retract speed"}, {"retract_speed2", "Second retract speed"}})
        p.pair(key_n(suffix), key_b(suffix), label, "mm/min", speed);
    for (const auto &[suffix, label] : {std::pair<const char *, const char *>{"wait_before_cure", "Wait before exposure"},
            {"wait_after_cure", "Wait after exposure"}, {"wait_after_lift", "Wait after lift"}})
        p.pair(key_n(suffix), key_b(suffix), label, "s", wait);
}
// Active moves need a speed and the secondary retract cannot exceed the total lift.
void move_checks(Planner &p, bool klipper)
{
    for (const bool bottom : {false, true}) {
        auto v = [&](const char *suffix) { return p.val(bottom ? key_b(suffix) : key_n(suffix)); };
        const std::string who = bottom ? "bottom " : "normal ";
        const double lift = v("lift_height"), lift2 = v("lift_height2"), retract2 = v("retract_height2");
        if (retract2 > lift + lift2 + 1e-9)
            p.blocker(p.name + " cannot store a " + who + "secondary retract (" + number(retract2) + " mm) longer than the total lift (" + number(lift + lift2) + " mm).");
        if (klipper) {
            if (lift <= 0 || v("lift_speed") <= 0 || v("retract_speed") <= 0 || (lift2 > 0 && v("lift_speed2") <= 0) ||
                (retract2 > 0 && v("retract_speed2") <= 0) || retract2 >= lift + lift2)
                p.blocker(p.name + " needs a positive first lift and retract stage with positive speeds for every enabled stage (" + who + "layers).");
            continue;
        }
        const std::pair<double, const char *> stages[] = {{lift, "lift_speed"}, {lift2, "lift_speed2"}, {lift + lift2 - retract2, "retract_speed"}, {retract2, "retract_speed2"}};
        for (const auto &[distance, speed_key] : stages)
            if (distance > 0 && v(speed_key) <= 0)
                p.blocker(p.name + " needs a positive " + who + std::string(speed_key) + " because that move is enabled.");
    }
}
void pwm_rules(Planner &p, bool positive_only)
{
    for (const auto &[key, label] : {std::pair<const char *, const char *>{"sla_bottom_light_pwm", "Bottom UV power"}, {"sla_light_pwm", "UV power"}}) {
        if (positive_only && p.val(key) < 0.5) {
            p.blocker(p.name + " cannot keep a timed exposure with UV power 0; set " + label + " to 1..255 in the material settings.");
            continue;
        }
        p.clamp_pwm(key, label, positive_only ? 1 : 0, 255);
    }
}
void count_rules(Planner &p)
{
    const double bottom = read(p.config, "sla_bottom_layers"), transition = read(p.config, "sla_transition_layers");
    if (bottom < 0 || bottom > 65535)
        p.blocker(p.name + " stores the bottom layer count as 0..65535, but it is " + number(bottom) + ".");
    if (transition < 0)
        p.blocker(p.name + " cannot store a negative transition layer count.");
}
bool portrait_of(const DynamicPrintConfig &c) { return c.option("display_orientation") && c.option("display_orientation")->getInt() == sladoPortrait; }

// Geometry the container cannot describe differently: refused (together with everything else), never adjusted.
void resolution_rule(Planner &p, double minimum_side, double maximum_side)
{
    const double x = read(p.config, "display_pixels_x"), y = read(p.config, "display_pixels_y");
    if (x < minimum_side || y < minimum_side || x > maximum_side || y > maximum_side || x * y > double(128ull << 20))
        p.blocker(p.name + " stores a resolution of " + number(minimum_side) + ".." + number(maximum_side) + " pixels per side and at most 128 megapixels, but the printer has " +
                  number(x) + "x" + number(y) + ".");
}
void mirror_rule(Planner &p, bool mirror_x, bool mirror_y)
{
    if (p.config.opt_bool("display_mirror_x") != mirror_x || p.config.opt_bool("display_mirror_y") != mirror_y)
        p.blocker(p.name + " requires " + (mirror_x ? "horizontal" : "no horizontal") + " and " + (mirror_y ? "vertical" : "no vertical") +
                  " display mirroring; change the printer display mirroring.");
}
// width/height are the image axes (swapped for portrait); z < 0 = not fixed.
void fixed_geometry(Planner &p, double columns, double rows, double width, double height, double z, const std::string &what)
{
    double cx = read(p.config, "display_pixels_x"), cy = read(p.config, "display_pixels_y");
    double w = p.config.opt_float("display_width"), h = p.config.opt_float("display_height");
    if (portrait_of(p.config)) { std::swap(cx, cy); std::swap(w, h); }
    if (cx != columns || cy != rows || std::abs(w - width) > 1e-6 || std::abs(h - height) > 1e-6 ||
        (z >= 0 && std::abs(p.config.opt_float("max_print_height") - z) > 1e-6))
        p.blocker(p.name + " requires " + what + ", but the printer is " + number(cx) + "x" + number(cy) + " px, " + number(w) + " x " + number(h) +
                  " mm, height " + number(p.config.opt_float("max_print_height")) + " mm.");
}

RepresentationPlan plan_makerbase(bool mdlp, const DynamicPrintConfig &c)
{
    Planner p(c);
    p.name = mdlp ? "Makerbase MDLP" : "GR1 Workshop";
    const Rule whole = grid(1, 1, 65535, false, "whole numbers", "1");
    p.single("exposure_time", "Exposure", "s", whole);
    p.single("initial_exposure_time", "Bottom exposure", "s", whole);
    no_second_stage(p);
    if (mdlp) {
        const std::string why = "MDLP has no motion fields; UVtools reads";
        p.fixed(key_n("lift_height"), key_b("lift_height"), "Lift height", "mm", 6, why + " 6 mm");
        p.fixed(key_n("lift_speed"), key_b("lift_speed"), "Lift speed", "mm/min", 100, why + " 100 mm/min");
        p.fixed(key_n("retract_speed"), key_b("retract_speed"), "Retract speed", "mm/min", 100, why + " 100 mm/min");
        p.pwm_full("sla_light_pwm", "UV power");
        p.pwm_full("sla_bottom_light_pwm", "Bottom UV power");
    } else {
        const Rule dist = grid(1, 0, 65535, false, "whole millimetres", "0");
        const Rule speed = grid(1, 1, 65535, true, "whole mm/min", "1");
        p.pair(key_n("lift_height"), key_b("lift_height"), "Lift height", "mm", dist);
        p.pair(key_n("lift_speed"), key_b("lift_speed"), "Lift speed", "mm/min", speed);
        p.shared(key_n("retract_speed"), key_b("retract_speed"), "Retract speed", "mm/min", speed);
    }
    // The light-off delay (movement + wait before exposure) is one whole number of seconds for all layers.
    auto light_off = [&](bool bottom) {
        auto v = [&](const char *suffix) { return double(float(p.val(bottom ? key_b(suffix) : key_n(suffix)))); };
        const double h = v("lift_height"), s = v("lift_speed"), r = v("retract_speed");
        const double move = h > 0 && s > 0 && r > 0 ? h * 60. / s + h * 60. / r : 0.;
        return std::make_pair(move, move + v("wait_before_cure"));
    };
    const auto [move_n, total_n] = light_off(false);
    const auto [move_b, total_b] = light_off(true);
    if (std::abs(total_n - std::round(total_n)) > 1e-5 || std::abs(total_b - std::round(total_b)) > 1e-5 || std::abs(total_n - total_b) > 1e-5) {
        const double target = std::max(std::ceil(total_n - 1e-5), std::ceil(total_b - 1e-5));
        const std::string reason = p.name + " stores the light-off delay (lift/retract movement plus wait) as whole seconds shared by all layers";
        const std::pair<bool, double> layers[] = {{false, move_n}, {true, move_b}};
        const double totals[] = {total_n, total_b};
        for (int i = 0; i < 2; ++i)
            if (std::abs(totals[i] - target) > 1e-5) {
                const std::string key = layers[i].first ? key_b("wait_before_cure") : key_n("wait_before_cure");
                const double from = p.val(key), to = std::round((target - layers[i].second) * 1e4) / 1e4;
                p.add({key}, std::string("Wait before exposure") + (layers[i].first ? " (bottom layers)" : " (normal layers)"),
                      "s", number(from), number(to), reason, exact(to));
            }
    }
    resolution_rule(p, 1, 32768);
    mirror_rule(p, false, false);
    {
        double w = c.opt_float("display_width"), h = c.opt_float("display_height");
        if (portrait_of(c)) std::swap(w, h);
        if (w <= 0 || h <= 0 || std::abs(w - std::round(w * 100) / 100) > 1e-6 || std::abs(h - std::round(h * 1000) / 1000) > 1e-6)
            p.blocker(p.name + " stores the display width with 2 decimals and the height with 3, but the printer has " + number(w) + " x " + number(h) + " mm.");
    }
    return std::move(p.plan);
}

RepresentationPlan plan_anet(bool n7, const DynamicPrintConfig &c)
{
    Planner p(c);
    p.name = "Anet";
    const Rule whole = grid(1, 1, 4294967295., false, "whole seconds", "1");
    p.single("exposure_time", "Exposure", "s", whole);
    p.single("initial_exposure_time", "Bottom exposure", "s", whole);
    no_second_stage(p, true);
    p.pwm_full("sla_light_pwm", "UV power");
    p.pwm_full("sla_bottom_light_pwm", "Bottom UV power");
    p.shared(key_n("lift_height"), key_b("lift_height"), "Lift height", "mm", grid(1, 0, 4294967295., false, "whole millimetres", "0"));
    p.shared(key_n("lift_speed"), key_b("lift_speed"), "Lift speed", "mm/min", grid(60, 60, 4294967295. * 60, true, "whole mm/s", "60 mm/min (1 mm/s)"));
    // One common lift/retract speed: the retract speed follows the lift speed.
    const double common = p.val(key_n("lift_speed"));
    p.fixed(key_n("retract_speed"), key_b("retract_speed"), "Retract speed", "mm/min", common, "Anet uses one common lift/retract speed");
    if (n7) fixed_geometry(p, 2560, 1600, 192., 120., 300., "the Anet N7 screen (2560x1600 px, 192 x 120 mm, height 300 mm)");
    else    fixed_geometry(p, 1440, 2560, 68.04, 120.96, 135., "the Anet N4 screen (1440x2560 px, 68.04 x 120.96 mm, height 135 mm)");
    return std::move(p.plan);
}

RepresentationPlan plan_qdt(const DynamicPrintConfig &c)
{
    Planner p(c);
    p.name = "QDT";
    const std::string why = "QDT has no exposure/motion fields; UVtools reads";
    p.fixed("exposure_time", "exposure_time", "Exposure", "s", 3, why + " 3 s");
    p.fixed("initial_exposure_time", "initial_exposure_time", "Bottom exposure", "s", 30, why + " 30 s");
    p.fixed("sla_bottom_layers", "sla_bottom_layers", "Bottom layers", "", 4, why + " 4 bottom layers", true);
    p.fixed(key_n("lift_height"), key_b("lift_height"), "Lift height", "mm", 6, why + " 6 mm");
    p.fixed(key_n("lift_speed"), key_b("lift_speed"), "Lift speed", "mm/min", 100, why + " 100 mm/min");
    p.fixed(key_n("retract_speed"), key_b("retract_speed"), "Retract speed", "mm/min", 100, why + " 100 mm/min");
    p.fixed(key_n("wait_before_cure"), key_b("wait_before_cure"), "Wait before exposure", "s", 0, "QDT has no rest fields; UVtools reads no explicit rest");
    no_second_stage(p);
    p.pwm_full("sla_light_pwm", "UV power");
    p.pwm_full("sla_bottom_light_pwm", "Bottom UV power");
    double width = c.opt_float("display_width"), height = c.opt_float("display_height");
    if (portrait_of(c)) std::swap(width, height);
    if (width != 400 || height != 200 || c.opt_float("max_print_height") != 400)
        p.blocker("QDT has no physical geometry fields: Emake3D Galaxy 1 requires a 400x200 mm display and a 400 mm height, but the printer has " +
                  number(width) + "x" + number(height) + " mm, height " + number(c.opt_float("max_print_height")) + " mm.");
    if (!c.opt_bool("display_mirror_x") || c.opt_bool("display_mirror_y"))
        p.blocker("QDT requires horizontal mirroring and no vertical mirroring; change the printer display mirroring.");
    const double um = read(c, "layer_height") * 1000;
    if (um < 1 || um > 65535)
        p.blocker("QDT requires the layer height in 0.001..65.535 mm; it is " + number(read(c, "layer_height")) + " mm.");
    layer_height_rules(p, 1000, true);
    resolution_rule(p, 6, 32768);
    return std::move(p.plan);
}

RepresentationPlan plan_svgx(const DynamicPrintConfig &c)
{
    Planner p(c);
    p.name = "SVGX";
    const Rule seconds = grid(.01, .01, 1e6, false, "at most two decimals", "0.01 s");
    p.single("exposure_time", "Exposure", "s", seconds);
    p.single("initial_exposure_time", "Bottom exposure", "s", seconds);
    const std::string why = "SVGX has no motion fields; UVtools reads";
    p.fixed(key_n("lift_height"), key_b("lift_height"), "Lift height", "mm", 6, why + " 6 mm");
    p.fixed(key_n("lift_speed"), key_b("lift_speed"), "Lift speed", "mm/min", 100, why + " 100 mm/min");
    p.fixed(key_n("retract_speed"), key_b("retract_speed"), "Retract speed", "mm/min", 100, why + " 100 mm/min");
    p.fixed(key_n("wait_before_cure"), key_b("wait_before_cure"), "Wait before exposure", "s", 0, "SVGX cannot store a wait before exposure");
    no_second_stage(p);
    p.pwm_full("sla_light_pwm", "UV power");
    p.pwm_full("sla_bottom_light_pwm", "Bottom UV power");
    count_rules(p);
    layer_height_rules(p, 1000, true);
    resolution_rule(p, 1, 1e9);
    mirror_rule(p, false, true);
    return std::move(p.plan);
}

RepresentationPlan plan_decimal_family(const std::string &format, const DynamicPrintConfig &c)
{
    Planner p(c);
    const bool jxs = format == "jxs", zcode = format == "zcode", zcodex = format == "zcodex", vdt = format == "vdt";
    p.name = jxs ? "JXS" : zcode ? "ZCode" : zcodex ? "ZCodex" : vdt ? "VDT" : "Klipper";
    const bool klipper = !jxs && !zcode && !zcodex && !vdt;
    const Rule seconds = grid(.01, .01, klipper ? 4294967. : 1e6, false, "at most two decimals", "0.01 s");
    p.single("exposure_time", "Exposure", "s", seconds);
    p.single("initial_exposure_time", "Bottom exposure", "s", seconds);
    count_rules(p);
    resolution_rule(p, 1, 32768);
    if (jxs) { mirror_rule(p, false, true); fixed_geometry(p, 4920, 2880, 221.4, 129.6, 245., "the Uniformation GKone screen (4920x2880 px, 221.4 x 129.6 mm, height 245 mm)"); }
    if (zcodex) { mirror_rule(p, true, false); fixed_geometry(p, 1440, 2560, 74.67, 132.88, -1, "the Inkspire screen (1440x2560 px, 74.67 x 132.88 mm)"); }
    if (zcodex) {
        no_second_stage(p);
        const Rule dist = grid(.01, 0, 1e6, false, "at most two decimals", "0");
        const Rule speed = grid(1, 1, 1e6, true, "whole mm/min", "1");
        p.shared(key_n("lift_height"), key_b("lift_height"), "Lift height", "mm", dist);
        p.shared(key_n("lift_speed"), key_b("lift_speed"), "Lift speed", "mm/min", speed);
        p.shared(key_n("retract_speed"), key_b("retract_speed"), "Retract speed", "mm/min", speed);
        p.shared(key_n("wait_before_cure"), key_b("wait_before_cure"), "Wait before exposure", "s", dist);
        pwm_rules(p, true);
        layer_height_rules(p, 1000, false);
        return std::move(p.plan);
    }
    decimal_motion(p, klipper ? 4294967. : 1e6, zcode ? .1 : .01, klipper);
    move_checks(p, klipper);
    pwm_rules(p, zcode || klipper);
    layer_height_rules(p, 1000, false);
    if (klipper) {
        double width = c.opt_float("display_width"), height = c.opt_float("display_height"), z = c.opt_float("max_print_height");
        for (const double value : {width, height, z})
            if (value > 0 && std::abs(value * 100 - std::round(value * 100)) > 1e-6)
                p.blocker("Klipper stores screen geometry at 0.01 mm precision, but the printer has " + number(width) + " x " + number(height) +
                          " mm, height " + number(z) + " mm.");
    }
    if (zcode && (c.opt_bool("display_mirror_x") || !c.opt_bool("display_mirror_y")))
        p.blocker("ZCode requires vertical mirroring without horizontal mirroring; change the printer display mirroring.");
    return std::move(p.plan);
}

RepresentationPlan plan_ctb_v3(const DynamicPrintConfig &c)
{
    Planner p(c);
    p.name = "CTB v3";
    for (const bool bottom : {false, true})
        for (const auto &[suffix, label] : {std::pair<const char *, const char *>{"lift_height2", "second lift height"},
                {"retract_height2", "second retract height"}, {"wait_after_cure", "wait after exposure"}, {"wait_after_lift", "wait after lift"}})
            p.must_be_zero(bottom ? key_b(suffix) : key_n(suffix), cat(bottom ? "bottom " : "normal ", label));
    return std::move(p.plan);
}
} // namespace

RepresentationPlan plan_representation(const std::string &format, const DynamicPrintConfig &c)
{
    if (!has_representation_rules(format)) return {};
    if (format == "cxdlp" || format == "cxdlpv2" || format == "cxdlpv1") return plan_cxdlp(format, c);
    if (format == "mdlp" || format == "gr1") return plan_makerbase(format == "mdlp", c);
    if (format == "n4" || format == "n7") return plan_anet(format == "n7", c);
    if (format == "qdt") return plan_qdt(c);
    if (format == "svgx") return plan_svgx(c);
    if (format == "ctb_v3") return plan_ctb_v3(c);
    return plan_decimal_family(format, c);
}

void apply_representation_adjustments(ConfigBase &target, const RepresentationApproval &adjustments)
{
    for (const auto &adjustment : adjustments)
        for (const auto &key : adjustment.keys)
            if (auto *option = target.option(key)) option->deserialize(adjustment.value);
}
} // namespace Slic3r::sla
