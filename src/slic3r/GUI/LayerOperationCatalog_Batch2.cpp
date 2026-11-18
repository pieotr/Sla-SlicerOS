#include "LayerEditorInternal.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
void append_batch2_defs(std::vector<OpDef> &defs, const OpContext &initial)
{
    using namespace ops_detail;
    using namespace batch2_detail;
    using Change = sla::LayerDocument::LayerListChange;
    const auto &table = initial.table;
    const auto &printer = initial.printer;
    auto add = [&](OpGroup g, const char *label, const char *description, std::vector<OpField> fields,
                   std::function<BuiltOp(const OpValues &, const OpContext &)> build) -> OpDef & {
        defs.push_back({g, label, description, std::move(fields), true, false, false, false, std::move(build)});
        return defs.back();
    };

    // ---- Parameters ----
    {
        const auto d = table.empty() ? layerops::DynamicLiftsParams{} : layerops::dynamic_lifts_defaults(table);
        add(OpGroup::Parameters, "Dynamic lifts",
            "Sets lift height and speed of every layer from its lit area: big layers lift higher and slower. Reads the pixels, "
            "writes only per-layer lift parameters.",
            {pick("method", "Method", {"Traditional", "Full range"}, 0),
             dbl("sbl", "Smallest bottom lift height (mm)", 0, 1000, d.smallest_bottom_lift_height, 2),
             dbl("lbl", "Largest bottom lift height (mm)", 0, 1000, d.largest_bottom_lift_height, 2),
             dbl("sl", "Smallest lift height (mm)", 0, 1000, d.smallest_lift_height, 2),
             dbl("ll", "Largest lift height (mm)", 0, 1000, d.largest_lift_height, 2),
             dbl("sbs", "Slowest bottom lift speed (mm/min)", 0, 100000, d.slowest_bottom_lift_speed, 2),
             dbl("fbs", "Fastest bottom lift speed (mm/min)", 0, 100000, d.fastest_bottom_lift_speed, 2),
             dbl("ss", "Slowest lift speed (mm/min)", 0, 100000, d.slowest_lift_speed, 2),
             dbl("fs", "Fastest lift speed (mm/min)", 0, 100000, d.fastest_lift_speed, 2)},
            [](const OpValues &v, const OpContext &c) {
                require_parameters(c); require_range(c);
                layerops::DynamicLiftsParams p;
                p.first_layer = c.first; p.last_layer = c.last;
                p.method = layerops::DynamicLiftsMethod(v.i("method"));
                p.smallest_bottom_lift_height = v.d("sbl"); p.largest_bottom_lift_height = v.d("lbl");
                p.smallest_lift_height = v.d("sl"); p.largest_lift_height = v.d("ll");
                p.slowest_bottom_lift_speed = v.d("sbs"); p.fastest_bottom_lift_speed = v.d("fbs");
                p.slowest_lift_speed = v.d("ss"); p.fastest_lift_speed = v.d("fs");
                require_valid(layerops::validate_dynamic_lifts(p));
                BuiltOp op;
                // Counts the lit pixels one layer at a time and edits only the parameter rows (no whole-document decode).
                op.document_op = [p](sla::LayerDocument &doc, double fallback, const sla::LayerProgress &pr) {
                    return sla::liststream::apply_dynamic_lifts(doc, p, fallback, pr);
                };
                return op;
            });
    }
    {
        const auto d = table.empty() ? layerops::FadeExposureParams{} : layerops::fade_exposure_defaults(table);
        add(OpGroup::Parameters, "Fade exposure time",
            "Changes the exposure time step by step across N layers, starting at the first layer of the range.",
            {num("count", "Number of layers", 1, 1000000, double(std::max<size_t>(1, d.layer_count))),
             dbl("from", "From exposure (s)", 0, 1000, d.from_exposure_s, 2), dbl("to", "To exposure (s)", 0, 1000, d.to_exposure_s, 2),
             flag("fw", "Disable firmware transition layers", d.disable_firmware_transition_layers)},
            [](const OpValues &v, const OpContext &c) {
                require_parameters(c); require_range(c);
                layerops::FadeExposureParams p;
                p.first_layer = c.first; p.layer_count = v.u("count");
                p.from_exposure_s = v.d("from"); p.to_exposure_s = v.d("to");
                p.disable_firmware_transition_layers = v.b("fw");
                p.bottom_layer_count = c.bottom_layers;
                require_valid(layerops::validate_fade_exposure(p, c.table.size()));
                BuiltOp op;
                op.param = [p](std::vector<layerops::Mask> &, std::vector<double> &, std::vector<layerops::LayerParams> &t,
                               const sla::LayerProgress &pr) {
                    layerops::fade_exposure_time(t, p, pr);
                    return Change{};
                };
                return op;
            });
    }
    {
        const auto d = layerops::phased_exposure_defaults(first_bottom(table), first_normal(table), printer.is_gcode);
        std::vector<OpField> fields;
        for (int n = 1; n <= 3; ++n) {
            const auto &lvl = d.phases[std::min<size_t>(size_t(n - 1), d.phases.size() - 1)];
            const std::string k = "p" + std::to_string(n), t = "Phase " + std::to_string(n) + " ";
            if (n == 3) fields.push_back(flag("third", "Use a third phase", false));
            fields.push_back(dbl((k + "be").c_str(), (t + "bottom exposure (s)").c_str(), 0, 1000, lvl.bottom_exposure_s, 2));
            fields.push_back(num((k + "bi").c_str(), (t + "bottom iterations (px)").c_str(), 0, 1000, lvl.bottom_iterations));
            fields.push_back(dbl((k + "e").c_str(), (t + "exposure (s)").c_str(), 0, 1000, lvl.exposure_s, 2));
            fields.push_back(num((k + "i").c_str(), (t + "iterations (px)").c_str(), 0, 1000, lvl.iterations));
        }
        fields.push_back(flag("diff", "Later phases print only what earlier ones did not cure", d.difference_only));
        fields.push_back(num("ov", "Difference overlap (px)", 0, 1000, d.difference_overlap_iterations));
        fields.push_back(flag("seq", "Different settings for the sequential layers", d.different_settings_for_sequential_layers));
        fields.push_back(flag("lift", "Lift before sequential layers", d.sequential_lift_enabled));
        fields.push_back(dbl("liftmm", "Sequential lift (mm)", 0, 100, d.sequential_lift_mm, 3));
        fields.push_back(flag("wait", "Wait before cure of sequential layers", d.sequential_wait_before_cure_enabled));
        fields.push_back(dbl("waits", "Sequential wait before cure (s)", 0, 1000, d.sequential_wait_before_cure_s, 2));
        auto &def = add(OpGroup::Parameters, "Phased exposure",
            "Cures every layer of the range in several phases (eroded copies). Adds layers, so it needs a sliced or generated document; "
            "the result is one Undo step.",
            std::move(fields),
            [printer](const OpValues &v, const OpContext &c) {
                require_parameters(c); require_range(c);
                layerops::PhasedExposureParams p;
                p.first_layer = c.first; p.last_layer = c.last;
                p.phases.clear();
                for (int n = 1; n <= (v.b("third") ? 3 : 2); ++n) {
                    const std::string k = "p" + std::to_string(n);
                    p.phases.push_back({v.d(k + "be"), v.u(k + "bi"), v.d(k + "e"), v.u(k + "i")});
                }
                p.difference_only = v.b("diff"); p.difference_overlap_iterations = v.u("ov");
                p.different_settings_for_sequential_layers = v.b("seq");
                p.sequential_lift_enabled = v.b("lift"); p.sequential_lift_mm = v.d("liftmm");
                p.sequential_wait_before_cure_enabled = v.b("wait"); p.sequential_wait_before_cure_s = v.d("waits");
                require_valid(layerops::validate_phased_exposure(p, c.z_mm));
                const auto ctx = c.printer;
                BuiltOp op;
                op.param_changes_list = true;
                op.stream = sla::liststream::phased_exposure(p, ctx); op.stream_params = true;
                return op;
            });
        (void) def;
    }
    add(OpGroup::Parameters, "Raise on print finish",
        "Appends a dummy layer that lifts the platform to a height and waits there when the print ends. Adds a layer: sliced or "
        "generated documents only.",
        {dbl("z", "Platform position Z (mm, 0 = machine Z)", 0, 10000, 0, 2), dbl("wait", "Wait time (s)", 0, 100000, 1200, 1),
         flag("dummy", "Output a dummy pixel", true)},
        [](const OpValues &v, const OpContext &c) {
            require_parameters(c);
            layerops::RaiseOnPrintFinishParams p;
            p.position_z_mm = v.d("z"); p.wait_time_s = v.d("wait"); p.output_dummy_pixel = v.b("dummy");
            require_valid(layerops::validate_raise_on_print_finish(p, c.z_mm, c.printer));
            const auto ctx = c.printer;
            BuiltOp op;
            op.param_changes_list = true;
            op.stream = sla::liststream::raise_on_print_finish(p, ctx); op.stream_params = true;
            return op;
        }).uses_range = false;
    {
        std::vector<OpField> fields{
            pick("mode", "Edit mode", {"Per layer (every N layers)", "Global (bottom and normal layers)"}, 1),
            num("set", "Per layer: edit N layers", 1, 1000000, 1), num("skip", "Per layer: then skip M layers", 0, 1000000, 0)};
        fields.push_back(opt_field("e_z", "Layer Z position (mm, per layer mode)", "", "Most generated documents keep Z read-only."));
        for (const auto &spec : edit_field_specs()) fields.push_back(opt_field(std::string("e_") + spec.suffix, spec.label));
        fields.push_back(opt_field("e_pwm", "Light PWM (0-255)"));
        for (const auto &spec : edit_field_specs()) fields.push_back(opt_field(std::string("b_") + spec.suffix, std::string("Bottom: ") + spec.label,
                                                                           "", "Global mode only: value for the bottom layers."));
        fields.push_back(opt_field("b_pwm", "Bottom: Light PWM (0-255)"));
        add(OpGroup::Parameters, "Edit parameters",
            "Sets exposure, lift, wait and speed values. Leave a field empty to keep the stored value. Global mode writes the bottom "
            "fields to the bottom layers and the other fields to all others. Fields the file format cannot store are refused.",
            std::move(fields),
            [](const OpValues &v, const OpContext &c) {
                require_parameters(c); require_range(c);
                layerops::EditParametersParams p;
                p.first_layer = c.first; p.last_layer = c.last;
                p.per_layer_override = v.i("mode") == 0;
                p.set_layers = v.u("set"); p.skip_layers = v.u("skip");
                p.edit = read_edit(v, "e_", p.per_layer_override);
                p.bottom = read_edit(v, "b_", false);
                require_valid(layerops::validate_edit_parameters(p));
                BuiltOp op;
                op.param = [p](std::vector<layerops::Mask> &, std::vector<double> &z, std::vector<layerops::LayerParams> &t,
                               const sla::LayerProgress &pr) {
                    layerops::edit_parameters(z, t, p, pr);
                    return Change{};
                };
                return op;
            });
    }
    {
        const auto &n = first_normal(table);
        add(OpGroup::Parameters, "Timelapse",
            "Raises the platform every N mm of height so a camera can take a picture. Lift height mode only changes lift values; "
            "Virtual layer mode inserts extra layers (sliced or generated documents only).",
            {pick("mode", "Mode", {"Lift height", "Virtual layer"}, 0), dbl("z", "Raise position Z (mm, 0 = machine Z)", 0, 10000, 0, 2),
             flag("dummy", "Output a dummy pixel (virtual layer)", true), dbl("nth", "Raise every N mm of height", 0.01, 1000, 1, 2),
             dbl("wal", "Wait after lift (s)", 0, 100000, 1, 2), dbl("exp", "Photo layer exposure (s, 0 = default)", 0, 1000, 0, 2),
             flag("last", "Ensure the last layer is a timelapse layer", true), flag("custom", "Use custom lift", false),
             dbl("slh", "Slow lift height (mm)", 0, 1000, 3, 2), dbl("ls", "Lift speed (mm/min)", 0, 100000, n.lift_speed, 2),
             dbl("ls2", "Lift speed 2 (mm/min)", 0, 100000, n.lift_speed2, 2), dbl("srh", "Slow retract height (mm)", 0, 1000, 3, 2),
             dbl("rs", "Retract speed (mm/min)", 0, 100000, n.retract_speed, 2), dbl("rs2", "Retract speed 2 (mm/min)", 0, 100000, n.retract_speed2, 2)},
            [](const OpValues &v, const OpContext &c) {
                require_parameters(c); require_range(c);
                layerops::TimelapseParams p;
                p.first_layer = c.first; p.last_layer = c.last;
                p.mode = layerops::TimelapseMode(v.i("mode"));
                p.raise_position_z_mm = v.d("z"); p.output_dummy_pixel = v.b("dummy");
                p.raise_each_nth_height_mm = v.d("nth"); p.wait_after_lift_s = v.d("wal"); p.exposure_s = v.d("exp");
                p.ensure_last_layer = v.b("last"); p.use_custom_lift = v.b("custom");
                p.slow_lift_height_mm = v.d("slh"); p.lift_speed = v.d("ls"); p.lift_speed2 = v.d("ls2");
                p.slow_retract_height_mm = v.d("srh"); p.retract_speed = v.d("rs"); p.retract_speed2 = v.d("rs2");
                require_valid(layerops::validate_timelapse(p, c.z_mm, c.printer));
                const auto ctx = c.printer;
                BuiltOp op;
                op.param_changes_list = p.mode == layerops::TimelapseMode::VirtualLayer;
                if (op.param_changes_list) {
                    op.stream = sla::liststream::timelapse(p, ctx); op.stream_params = true;
                } else {
                    // Lift height mode only edits the parameter table: no layer is decoded.
                    op.param = [p, ctx](std::vector<layerops::Mask> &, std::vector<double> &z, std::vector<layerops::LayerParams> &t,
                                        const sla::LayerProgress &pr) {
                        layerops::TimelapseVirtualLayer unused;
                        layerops::timelapse_plan(z.size(), 0, 0, z, t, p, ctx, [](int &, int &) {}, unused, pr);
                        return Change{};
                    };
                }
                return op;
            });
    }
    {
        layerops::DynamicLayerHeightParams d;
        if (!table.empty()) d = layerops::dynamic_layer_height_defaults(table, printer);
        add(OpGroup::Parameters, "Dynamic layer height",
            "Stacks identical-looking layers into thicker ones (within the minimum and maximum height) and sets the exposure for each "
            "new height. Sliced or generated documents only.",
            {dbl("min", "Minimum layer height (mm)", 0.001, 10, d.minimum_layer_height_mm, 3),
             dbl("max", "Maximum layer height (mm)", 0.001, 10, d.maximum_layer_height_mm, 3),
             flag("strip", "Strip anti-aliasing before comparing", d.strip_anti_aliasing),
             flag("rebuild", "Reconstruct anti-aliasing afterwards", d.reconstruct_anti_aliasing),
             num("erodes", "Maximum erodes", 0, 1000, d.maximum_erodes),
             pick("type", "Exposure for the new heights", {"Linear", "Multiplier"}, int(d.exposure_type == layerops::ExposureSetType::Multiplier)),
             flag("iter", "Iterate the bottom exposure too", d.iterate_bottom_exposure),
             dbl("be", "Bottom exposure (s)", 0, 1000, d.bottom_exposure_s, 2), dbl("ne", "Normal exposure (s)", 0, 1000, d.exposure_s, 2),
             dbl("bs", "Bottom exposure step (s)", 0, 1000, d.bottom_exposure_step_s, 2), dbl("ns", "Normal exposure step (s)", 0, 1000, d.exposure_step_s, 2)},
            [](const OpValues &v, const OpContext &c) {
                require_parameters(c); require_range(c);
                layerops::DynamicLayerHeightParams p;
                p.first_layer = c.first; p.last_layer = c.last;
                p.minimum_layer_height_mm = v.d("min"); p.maximum_layer_height_mm = v.d("max");
                p.strip_anti_aliasing = v.b("strip"); p.reconstruct_anti_aliasing = v.b("rebuild");
                p.maximum_erodes = v.u("erodes");
                p.exposure_type = v.i("type") == 1 ? layerops::ExposureSetType::Multiplier : layerops::ExposureSetType::Linear;
                p.iterate_bottom_exposure = v.b("iter");
                p.bottom_exposure_s = v.d("be"); p.exposure_s = v.d("ne");
                p.bottom_exposure_step_s = v.d("bs"); p.exposure_step_s = v.d("ns");
                require_valid(layerops::validate_dynamic_layer_height(p, c.z_mm, c.printer));
                const auto ctx = c.printer;
                BuiltOp op;
                op.param_changes_list = true;
                // Stacks are built layer by layer from the encoded document; unchanged layers keep their payload.
                op.stream = sla::liststream::dynamic_layer_height(p, ctx); op.stream_params = true;
                return op;
            });
    }

    // ---- Pixels / Layers (batch 2, no parameter table involved) ----
    {
        auto &def = add(OpGroup::Pixels, "Light bleed compensation",
            "Dims pixels that are lit in the neighbouring layers to compensate light bleeding through thin layers.",
            {pick("subject", "Dim", {"Similarities", "Bridges", "Both"}, 0), pick("lookup", "Compare with", {"Previous layer", "Next layer", "Both"}, 1),
             txt("dim", "Dim by (comma separated brightness steps)", "25,15,10,5")},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::LightBleedParams p;
                p.selection = c.selection();
                p.subject = layerops::LightBleedSubject(v.i("subject")); p.lookup = layerops::LightBleedLookup(v.i("lookup"));
                p.dim_by = v.s("dim");
                require_valid(layerops::validate_light_bleed(p));
                BuiltOp op;
                op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                    auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                    layerops::light_bleed_compensation_layers(layers, q, pr);
                };
                op.kernel = [p](const layerops::StreamLayout &l) { return layerops::light_bleed_kernel(p, l); };
                return op;
            });
        def.uses_roi = true;
    }
    {
        auto &def = add(OpGroup::Layers, "Repair layers",
            "Removes islands, fills resin traps, drills suction cups, closes gaps and removes noise. Removing empty layers changes the "
            "layer list (sliced or generated documents only), so it is off by default on a sliced model result.",
            {flag("islands", "Repair islands", true), flag("traps", "Repair resin traps", true), flag("cups", "Repair suction cups", false),
             flag("empty", "Remove empty layers", initial.list_edits_possible),
             num("islpx", "Remove islands with at most N pixels", 0, 100000000, 5), num("isliter", "Island removal passes (0 = until done)", 0, 1000, 4),
             num("attach", "Attach islands to support below (layers)", 0, 1000, 2), num("overlap", "Resin trap overlap (px)", 0, 1000, 5),
             num("vent", "Suction cup vent hole diameter (px)", 1, 1000, 16), num("gap", "Gap closing iterations", 0, 1000, 1),
             num("noise", "Noise removal iterations", 0, 1000, 0)},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::RepairLayersParams p;
                p.first_layer = c.first; p.last_layer = c.last;
                p.repair_islands = v.b("islands"); p.repair_resin_traps = v.b("traps"); p.repair_suction_cups = v.b("cups");
                p.remove_empty_layers = v.b("empty");
                p.remove_islands_below_equal_pixels = v.u("islpx"); p.remove_islands_recursive_iterations = v.u("isliter");
                p.attach_islands_below_layers = v.u("attach"); p.resin_traps_overlap_by = v.u("overlap");
                p.suction_cups_vent_hole = v.u("vent"); p.gap_closing_iterations = v.u("gap"); p.noise_removal_iterations = v.u("noise");
                require_valid(layerops::validate_repair_layers(p));
                BuiltOp op;
                op.stream = sla::liststream::repair_layers(p);
                op.stream_pixels = !p.remove_empty_layers; // no layer removed: pixels only, every document, one Undo step
                if (p.remove_empty_layers) {
                    op.list = [p](auto &layers, auto &z, const sla::LayerProgress &pr) {
                        auto result = layerops::repair_layers(layers, z, p, pr);
                        Change change;
                        change.list = std::move(result.list);
                        return change;
                    };
                } else {
                    // No layer is removed: a plain pixel operation, available on every document and one Undo step.
                    op.mask = [p](auto &layers, const std::vector<double> &z, const layerops::LayerSelection &, const sla::LayerProgress &pr) {
                        auto heights = z;
                        layerops::repair_layers(layers, heights, p, pr);
                    };
                }
                return op;
            });
        def.uses_roi = false;
    }
    {
        auto &def = add(OpGroup::Layers, "Lithophane",
            "Builds a lithophane from a picture: darker pixels become taller columns. REPLACES all layers. Applied to the open document "
            "when it can take a different layer list, otherwise saved as a new generated layer project and opened.",
            {file_field("image", "Picture"),
             dbl("lh", "Layer height (mm)", 0.001, 1, printer.layer_height_mm > 0 ? printer.layer_height_mm : 0.05, 3),
             num("bottom", "Bottom layer count", 0, 1000, double(initial.bottom_layers ? initial.bottom_layers : 4)),
             dbl("bexp", "Bottom exposure (s)", 0, 1000, first_bottom(table).exposure_s > 0 ? first_bottom(table).exposure_s : 30, 2),
             dbl("nexp", "Normal exposure (s)", 0, 1000, first_normal(table).exposure_s > 0 ? first_normal(table).exposure_s : 2, 2),
             pick("rot", "Rotate", {"None", "90 clockwise", "180", "90 counter-clockwise"}, 0),
             flag("mirror", "Mirror", false), flag("invert", "Invert colours", false),
             dbl("resize", "Resize (%)", 1, 1000, 100, 1), flag("contrast", "Enhance contrast", false),
             num("gain", "Brightness gain (-127..127)", -127, 127, 0), num("gap", "Gap closing iterations", 0, 1000, 0),
             num("noise", "Noise removal iterations", 0, 1000, 0), num("blur", "Gaussian blur (px)", 0, 1000, 0),
             flag("sep", "Separate grey levels into layers", true), num("t0", "Start threshold", 0, 255, 1), num("t1", "End threshold", 0, 255, 255),
             dbl("base", "Base thickness (mm)", 0, 100, 2, 2), pick("basetype", "Base", {"None", "Square", "Model outline"}, 1),
             num("margin", "Base margin (px)", 0, 10000, 80), dbl("height", "Lithophane height (mm)", 0.01, 100, 3, 2),
             flag("onelayer", "One layer per threshold", false), flag("aa", "Anti-aliasing", true)},
            [](const OpValues &v, const OpContext &c) {
                require_parameters(c);
                layerops::LithophaneParams p;
                p.layer_height_mm = v.d("lh"); p.bottom_layer_count = size_t(v.i("bottom"));
                p.bottom_exposure_s = v.d("bexp"); p.normal_exposure_s = v.d("nexp");
                p.rotate = layerops::RotateDirection(std::clamp(v.i("rot"), 0, 3));
                p.mirror = v.b("mirror"); p.invert_color = v.b("invert"); p.resize_factor_percent = v.d("resize");
                p.enhance_contrast = v.b("contrast"); p.brightness_gain = v.i("gain");
                p.gap_closing_iterations = v.u("gap"); p.remove_noise_iterations = v.u("noise"); p.gaussian_blur = v.u("blur");
                p.separate_grayscale_pixels = v.b("sep"); p.start_threshold = v.u("t0"); p.end_threshold = v.u("t1");
                p.base_thickness_mm = v.d("base"); p.base_type = layerops::LithophaneBase(std::clamp(v.i("basetype"), 0, 2));
                p.base_margin_px = v.u("margin"); p.lithophane_height_mm = v.d("height");
                p.one_layer_per_threshold = v.b("onelayer"); p.anti_aliasing = v.b("aa");
                const auto image = load_grey_image(v.s("image"));
                require_valid(layerops::validate_lithophane(p, image, c.width, c.height));
                const auto ctx = c.printer;
                const size_t width = c.width, height = c.height;
                BuiltOp op;
                op.lithophane = [p, image, ctx, width, height](std::vector<layerops::Mask> &layers, std::vector<double> &z,
                                                               std::vector<layerops::LayerParams> &t, const sla::LayerProgress &pr) {
                    layers.clear(); z.clear(); t.clear();
                    layerops::lithophane_layers(image, width, height, p, ctx, layers, z, t, pr);
                };
                op.lithophane_stream = [p, image, ctx, width, height](const layerops::LayerParams &bottom, const layerops::LayerParams &normal) {
                    return sla::liststream::lithophane(image, width, height, p, ctx, bottom, normal);
                };
                return op;
            });
        def.uses_range = false;
    }

    // ---- Export ----
    auto add_export = [&](const char *label, const char *description, std::vector<OpField> fields,
                          std::function<BuiltOp(const OpValues &, const OpContext &)> build) -> OpDef & {
        defs.push_back({OpGroup::Export, label, description, std::move(fields), true, true, false, false, std::move(build)});
        return defs.back();
    };
    {
        auto fields = std::vector<OpField>{opt_field("prefix", "File name prefix", "layer"),
            pick("type", "Format", {"PNG", "PGM (8-bit)"}, 0), flag("pad", "Pad the layer number with zeros", true),
            flag("crop", "Crop to the region of interest", true)};
        for (auto &f : orientation_fields()) fields.push_back(std::move(f));
        add_export("Layer images", "Writes the selected layers as numbered PNG (or PGM) pictures into a folder. Files are staged and renamed, "
                   "so Cancel leaves the folder untouched.", std::move(fields),
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerexport::ExportImageParams p;
                p.selection = c.selection(); p.filename_prefix = v.s("prefix").empty() ? "layer" : v.s("prefix");
                p.type = v.i("type") == 1 ? layerexport::ImageType::Pgm : layerexport::ImageType::Png;
                p.pad_layer_index = v.b("pad"); p.crop_by_roi = v.b("crop"); p.orientation = read_orientation(v);
                BuiltOp op;
                ExportPlan plan;
                plan.folder = true;
                plan.run = [p](const ExportData &d, const std::string &folder, const sla::LayerProgress &pr) {
                    layerexport::export_layer_images(d.layers, p, folder, pr);
                };
                op.exporter = std::move(plan);
                return op;
            });
    }
    {
        auto fields = std::vector<OpField>{num("fps", "Frames per second", 1, 100, 30), num("rep", "Loop count (0 = forever)", 0, 65535, 0),
            num("skip", "Layers skipped between frames", 0, 100000, 0), dbl("scale", "Scale (%)", 1, 100, 50, 1),
            flag("clip", "Crop to the model bounds", false)};
        for (auto &f : orientation_fields()) fields.push_back(std::move(f));
        add_export("Animated GIF", "Renders the layers as an animated GIF (256 grey levels).", std::move(fields),
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerexport::ExportGifParams p;
                p.selection = c.selection(); p.fps = v.u("fps"); p.repeats = v.u("rep"); p.skip = v.u("skip");
                p.scale_percent = v.d("scale"); p.clip_by_volume_bounds = v.b("clip"); p.orientation = read_orientation(v);
                require_valid(layerexport::validate_export_gif(p, c.layer_count));
                BuiltOp op;
                ExportPlan plan;
                plan.wildcard = "GIF animation (*.gif)|*.gif"; plan.default_name = "layers.gif";
                plan.run = [p](const ExportData &d, const std::string &file, const sla::LayerProgress &pr) {
                    layerexport::export_layer_gif(d.layers, p, file, pr);
                };
                op.exporter = std::move(plan);
                return op;
            });
    }
    {
        auto fields = std::vector<OpField>{flag("merge", "Count layers with the same Z once", true), flag("crop", "Crop to the region of interest", true)};
        for (auto &f : orientation_fields()) fields.push_back(std::move(f));
        add_export("Heat map", "Adds up the layers into one picture: bright = exposed in many layers.", std::move(fields),
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerexport::ExportHeatMapParams p;
                p.selection = c.selection(); p.merge_same_positioned_layers = v.b("merge"); p.crop_by_roi = v.b("crop");
                p.orientation = read_orientation(v);
                require_valid(layerexport::validate_export_heat_map(p, c.layer_count));
                BuiltOp op;
                ExportPlan plan;
                plan.wildcard = "PNG picture (*.png)|*.png"; plan.default_name = "heatmap.png";
                plan.run = [p](const ExportData &d, const std::string &file, const sla::LayerProgress &pr) {
                    layerexport::export_heat_map(d.layers, d.z_mm, p, file, pr);
                };
                op.exporter = std::move(plan);
                return op;
            });
    }
    add_export("Skeleton", "Morphological skeleton of every layer, summed into one picture.", {flag("crop", "Crop to the region of interest", true)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerexport::ExportSkeletonParams p;
            p.selection = c.selection(); p.crop_by_roi = v.b("crop");
            BuiltOp op;
            ExportPlan plan;
            plan.wildcard = "PNG picture (*.png)|*.png"; plan.default_name = "skeleton.png";
            plan.run = [p](const ExportData &d, const std::string &file, const sla::LayerProgress &pr) {
                layerexport::export_skeleton(d.layers, p, file, pr);
            };
            op.exporter = std::move(plan);
            return op;
        });
    {
        auto fields = std::vector<OpField>{opt_field("title", "Report title", "Layer export"), flag("props", "File information", true),
            flag("settings", "Per-layer settings table", true), flag("preview", "Layer preview with slider", true),
            num("step", "Preview every Nth layer", 1, 100000, 1), dbl("scale", "Preview scale (%)", 1, 100, 25, 1)};
        for (auto &f : orientation_fields()) fields.push_back(std::move(f));
        add_export("HTML report", "One self-contained HTML file with file information, per-layer settings and a layer preview (no external resources).",
            std::move(fields),
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerexport::ExportHtmlParams p;
                p.title = v.s("title").empty() ? "Layer export" : v.s("title");
                p.export_properties = v.b("props"); p.export_layer_settings = v.b("settings") && !c.table.empty();
                p.export_layer_preview = v.b("preview"); p.export_gcode = false;
                p.preview_first = c.first; p.preview_last = c.last; p.preview_step = std::max<size_t>(1, v.u("step"));
                p.preview_scale_percent = v.d("scale"); p.orientation = read_orientation(v);
                BuiltOp op;
                ExportPlan plan;
                plan.wildcard = "HTML report (*.html)|*.html"; plan.default_name = "report.html";
                plan.run = [p](const ExportData &d, const std::string &file, const sla::LayerProgress &pr) {
                    auto q = p;
                    q.properties.emplace_back("Layers", std::to_string(d.layers.layer_count));
                    if (d.layers.layer_count)
                        q.properties.emplace_back("Resolution", std::to_string(d.layers.width) + " x " + std::to_string(d.layers.height) + " px");
                    if (!d.z_mm.empty()) q.properties.emplace_back("Total height", wxString::Format("%.3f mm", d.z_mm.back()).ToStdString());
                    if (!d.table.empty()) q.properties.emplace_back("Estimated print time", format_duration(layerops::estimate_print_time_s(d.table)));
                    layerexport::export_html(d.layers, d.z_mm, d.table, q, file, pr);
                };
                op.exporter = std::move(plan);
                return op;
            });
    }
    {
        // Pixel size in mm: LCD size divided by the resolution when both are known (UVtools default 0.035 otherwise).
        const double px_w = initial.display_mm && initial.width ? initial.display_mm->first / double(initial.width) : 0.035;
        const double px_h = initial.display_mm && initial.height ? initial.display_mm->second / double(initial.height) : 0.035;
        auto fields = std::vector<OpField>{dbl("pw", "Pixel width (mm)", 0.0001, 10, px_w, 4), dbl("ph", "Pixel height (mm)", 0.0001, 10, px_h, 4),
            pick("quality", "Quality", {"1 - accurate", "2 - average", "3 - quick", "6 - dirty", "8 - blocky"}, 0),
            flag("merge", "Merge coplanar faces", true)};
        for (auto &f : orientation_fields()) fields.push_back(std::move(f));
        auto &def = add_export("STL mesh", "Builds a voxel surface of the layers (binary STL). Quality > 1 downsamples the pixels first.",
            std::move(fields),
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                static constexpr unsigned kQuality[] = {1, 2, 3, 6, 8};
                layerexport::ExportMeshParams p;
                p.selection = c.selection(); p.pixel_width_mm = v.d("pw"); p.pixel_height_mm = v.d("ph");
                p.quality = kQuality[std::clamp(v.i("quality"), 0, 4)]; p.merge_faces = v.b("merge");
                p.orientation = read_orientation(v);
                require_valid(layerexport::validate_export_mesh(p, c.layer_count));
                BuiltOp op;
                ExportPlan plan;
                plan.wildcard = "STL mesh (*.stl)|*.stl"; plan.default_name = "layers.stl";
                plan.run = [p](const ExportData &d, const std::string &file, const sla::LayerProgress &pr) {
                    layerexport::export_mesh(d.layers, d.z_mm, p, file, pr);
                };
                op.exporter = std::move(plan);
                return op;
            });
        def.uses_roi = false; // the mesh always uses the model bounds
    }
}

} // namespace layer_editor_detail
} // namespace Slic3r::GUI
