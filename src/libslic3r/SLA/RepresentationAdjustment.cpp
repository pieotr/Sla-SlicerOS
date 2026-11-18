// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RepresentationAdjustment.hpp"
#include <algorithm>
#include <cmath>
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
};
struct Rounded { double to; bool changed; std::string reason; bool overflow; };

Rounded round_to(double value, const Rule &rule, const std::string &unit)
{
    Rounded result{value, false, {}, false};
    if (!std::isfinite(value)) { result.overflow = true; return result; }
    double to = rule.floor ? std::floor(value / rule.step + 1e-9) * rule.step : std::round(value / rule.step) * rule.step;
    std::string reason = std::string("CXDLP stores ") + rule.grid_text;
    if (value < rule.minimum - 1e-9 || to < rule.minimum) {
        to = rule.minimum;
        reason = std::string("CXDLP minimum is ") + rule.minimum_text;
    }
    if (to > rule.maximum + 1e-9) { result.overflow = true; return result; }
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
    explicit Planner(const DynamicPrintConfig &c) : config(c) {}

    void add(std::vector<std::string> keys, const std::string &label, const std::string &unit, const std::string &from,
             const std::string &to, const std::string &reason, const std::string &value)
    {
        plan.adjustments.push_back({std::move(keys), label, from, to, unit, reason, value});
    }
    void overflow(const std::string &label, double value, const std::string &unit, const Rule &rule)
    {
        plan.blockers.push_back(label + " " + number(value) + " " + unit + " is outside what CXDLP can store (up to " +
                                number(rule.maximum) + " " + unit + "); change it in the material settings.");
    }
    // Independent normal and bottom fields (or one only). `display_scale` converts a stored key value to
    // the displayed one (1 for everything here).
    void pair(const char *normal, const char *bottom, const std::string &label, const std::string &unit, const Rule &rule,
              bool integer_key = false)
    {
        const double vn = read(config, normal), vb = read(config, bottom);
        const auto rn = round_to(vn, rule, unit), rb = round_to(vb, rule, unit);
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
    void shared(const char *normal, const char *bottom, const std::string &label, const std::string &unit, const Rule &rule)
    {
        const double vn = read(config, normal), vb = read(config, bottom);
        const auto rn = round_to(vn, rule, unit), rb = round_to(vb, rule, unit);
        if (rn.overflow) overflow(label, vn, unit, rule);
        if (rb.overflow) overflow("Bottom " + label, vb, unit, rule);
        if (rn.overflow || rb.overflow) return;
        if (std::abs(rn.to - rb.to) > 1e-9) {
            plan.blockers.push_back("CXDLP stores one " + label + " for the whole job, but normal layers use " + number(vn) + " " + unit +
                                    " and bottom layers " + number(vb) + " " + unit + "; set the same value in the material settings.");
            return;
        }
        if (!rn.changed && !rb.changed) return;
        const std::string from = std::abs(vn - vb) < 1e-9 ? number(vn) : number(vn) + " / " + number(vb);
        add({normal, bottom}, label, unit, from, number(rn.to), rn.changed ? rn.reason : rb.reason, exact(rn.to));
    }
    void single(const char *key, const std::string &label, const std::string &unit, const Rule &rule, bool integer_key = false)
    {
        pair(key, key, label, unit, rule, integer_key);
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
        text += blockers.empty() ? "\nChange these in the material/printer settings, or export from the application window, which offers to write the adjusted values."
                                 : "\nThe adjustable items can be written with adjusted values from the application window; the others must be changed in the settings.";
    return text;
}

bool has_representation_rules(const std::string &format)
{
    return format == "cxdlp" || format == "cxdlpv2" || format == "cxdlpv1";
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

RepresentationPlan plan_representation(const std::string &format, const DynamicPrintConfig &c)
{
    if (!has_representation_rules(format)) return {};
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

void apply_representation_adjustments(ConfigBase &target, const RepresentationApproval &adjustments)
{
    for (const auto &adjustment : adjustments)
        for (const auto &key : adjustment.keys)
            if (auto *option = target.option(key)) option->deserialize(adjustment.value);
}
} // namespace Slic3r::sla
