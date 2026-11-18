#include "LayerEditorInternal.hpp"

namespace Slic3r::GUI {
namespace layer_editor_detail {
std::vector<OpDef> make_layer_operation_defs(const OpContext &initial)
{
    using namespace ops_detail;
    std::vector<OpDef> defs;
    auto add = [&](OpGroup g, const char *label, const char *description, std::vector<OpField> fields,
                   std::function<BuiltOp(const OpValues &, const OpContext &)> build) -> OpDef & {
        defs.push_back({g, label, description, std::move(fields), true, true, true, false, std::move(build)});
        return defs.back();
    };
    const int max_px = 32768;

    // ---- Transform ----
    add(OpGroup::Transform, "Move", "Moves the model (or the region of interest) to an anchor with margins.",
        {pick("anchor", "Anchor", anchor_names(), int(layerops::Anchor::MiddleCenter)),
         num("ml", "Margin left (px)", -max_px, max_px, 0), num("mt", "Margin top (px)", -max_px, max_px, 0),
         num("mr", "Margin right (px)", -max_px, max_px, 0), num("mb", "Margin bottom (px)", -max_px, max_px, 0),
         flag("cut", "Cut (unchecked = copy)", true)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::MoveParams p;
            p.selection = c.selection(); p.anchor = layerops::Anchor(v.i("anchor"));
            p.margin_left = v.i("ml"); p.margin_top = v.i("mt"); p.margin_right = v.i("mr"); p.margin_bottom = v.i("mb");
            p.is_cut_move = v.b("cut");
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                require_valid(layerops::validate_move(q, layers));
                layerops::move_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::move_kernel(p, l); };
            return op;
        });
    add(OpGroup::Transform, "Resize", "Scales the layers around the image centre (bilinear).",
        {dbl("x", "X scale (%)", 1, 1000, 100, 2), dbl("y", "Y scale (%)", 1, 1000, 100, 2),
         flag("constrain", "Constrain proportions (Y follows X)", true),
         flag("fade", "Fade the scale back to 100 % across the range", false)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::ResizeParams p;
            p.selection = c.selection(); p.x_percent = v.d("x"); p.y_percent = v.b("constrain") ? v.d("x") : v.d("y");
            p.constrain_xy = v.b("constrain"); p.is_fade = v.b("fade");
            require_valid(layerops::validate_resize(p));
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                layerops::resize_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::resize_kernel(p, l); };
            return op;
        });
    add(OpGroup::Transform, "Rotate", "Rotates the layers around the image centre. Positive angles are clockwise.",
        {dbl("angle", "Angle (degrees)", -360, 360, 90, 2)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::RotateParams p;
            p.selection = c.selection(); p.angle_degrees = v.d("angle");
            require_valid(layerops::validate_rotate(p));
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                layerops::rotate_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::rotate_kernel(p, l); };
            return op;
        });
    add(OpGroup::Transform, "Flip", "Mirrors the layers horizontally, vertically or both.",
        {pick("dir", "Direction", {"Horizontal", "Vertical", "Both"}, 0),
         flag("copy", "Keep the original and blend the flipped copy over it", false)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::FlipParams p;
            p.selection = c.selection(); p.direction = layerops::FlipDirection(v.i("dir")); p.make_copy = v.b("copy");
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                layerops::flip_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::flip_kernel(p, l); };
            return op;
        });
    add(OpGroup::Transform, "Pattern", "Duplicates the model (or the region of interest) into a grid of copies.",
        {pick("anchor", "Anchor", with_none(anchor_names()), int(layerops::Anchor::None)),
         num("cols", "Columns", 1, 1000, 1), num("rows", "Rows", 1, 1000, 1),
         num("cs", "Column spacing (px)", 0, max_px, 0), num("rs", "Row spacing (px)", 0, max_px, 0)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::PatternParams p;
            p.selection = c.selection(); p.anchor = layerops::Anchor(v.i("anchor"));
            p.cols = v.u("cols"); p.rows = v.u("rows"); p.col_spacing = v.u("cs"); p.row_spacing = v.u("rs");
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                require_valid(layerops::validate_pattern(q, layers));
                layerops::pattern_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::pattern_kernel(p, l); };
            return op;
        });
    {
        auto &def = add(OpGroup::Transform, "Change resolution",
            "Copies the model bounds to the centre of a new canvas size (crop or pad). Only for documents that "
            "store their own resolution (not sliced documents).",
            {num("w", "New width (px)", 1, max_px, double(initial.width)), num("h", "New height (px)", 1, max_px, double(initial.height)),
             flag("fix", "Fix pixel ratio", false, "Scale the model by the pixel pitch ratio old/new."),
             dbl("rx", "Pixel ratio X (old/new)", 0.001, 1000, 1, 3), dbl("ry", "Pixel ratio Y (old/new)", 0.001, 1000, 1, 3)},
            [](const OpValues &v, const OpContext &) {
                layerops::ChangeResolutionParams p;
                p.new_width = v.u("w"); p.new_height = v.u("h"); p.fix_ratio = v.b("fix");
                p.ratio_x = v.d("rx"); p.ratio_y = v.d("ry");
                BuiltOp op;
                op.changes_resolution = true;
                op.stream = sla::liststream::change_resolution(p);
                return op;
            });
        def.uses_range = false; def.uses_roi = false; def.previewable = false;
    }

    // ---- Pixels ----
    add(OpGroup::Pixels, "Threshold", "Applies an OpenCV-style threshold to the pixel values.",
        {num("t", "Threshold", 0, 255, 127), num("max", "Maximum", 0, 255, 255), pick("type", "Type", threshold_names(), 0)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::ThresholdParams p;
            p.selection = c.selection(); p.threshold = uint8_t(v.i("t")); p.maximum = uint8_t(v.i("max"));
            p.type = layerops::ThresholdType(v.i("type"));
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                layerops::threshold_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::threshold_kernel(p, l); };
            return op;
        });
    add(OpGroup::Pixels, "Solidify", "Fills holes inside the model.",
        {num("area", "Minimum hole area (px^2, 0 = off)", 0, 100000000, 1),
         pick("check", "Fill holes", {"larger than or equal to the area", "smaller than or equal to the area"}, 0)},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::SolidifyParams p;
            p.selection = c.selection(); p.minimum_area = v.u("area"); p.area_check = layerops::AreaCheck(v.i("check"));
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                layerops::solidify_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::solidify_kernel(p, l); };
            return op;
        });
    {
        const auto chess = layerops::generate_dimming_pattern("Chessboard", 127);
        auto &def = add(OpGroup::Pixels, "Pixel dimming", "Dims or lightens pixels of the model's walls or inside with a tiled pattern.",
            {flag("light", "Lighten instead of dim", false), num("ws", "Wall thickness start (px)", 1, 4096, 10),
             num("we", "Wall thickness end (px)", 1, 4096, 10), flag("walls", "Dim only the walls", false),
             flag("chamfer", "Fade the wall thickness start -> end over the range", false),
             txt("pattern", "Pattern (rows of values 0..255)", chess.pattern), txt("alternate", "Alternate pattern", chess.alternate),
             num("per", "Alternate every N layers", 1, 10000, 1)},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::PixelDimmingParams p;
                p.selection = c.selection(); p.lightening_pixels = v.b("light");
                p.wall_thickness_start = v.u("ws"); p.wall_thickness_end = v.u("we");
                p.walls_only = v.b("walls"); p.chamfer = v.b("chamfer");
                p.pattern_text = v.s("pattern"); p.alternate_pattern_text = v.s("alternate");
                p.alternate_pattern_per_layers = v.u("per");
                require_valid(layerops::validate_pixel_dimming(p));
                BuiltOp op;
                op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                    auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                    layerops::pixel_dimming_layers(layers, q, pr);
                };
                op.kernel = [p](const layerops::StreamLayout &l) { return layerops::pixel_dimming_kernel(p, l); };
                return op;
            });
        def.pattern_helper = true;
    }
    {
        auto &def = add(OpGroup::Pixels, "Infill", "Replaces the model interior with a hollow infill pattern (needs neighbouring layers).",
            {pick("type", "Infill type", {"Pillars", "Concentric", "Waves", "Cubic cross", "Cubic cross alternating", "Cubic star", "Honeycomb", "Gyroid"},
                  int(layerops::InfillAlgorithm::CubicCrossAlternating)),
             dbl("fc", "Floor / ceiling thickness (mm)", 0, 100, 3.0, 3), num("wall", "Wall thickness (px)", 1, 4096, 64),
             num("thick", "Infill thickness (px)", 1, 4096, 45), num("space", "Infill spacing (px)", 1, 8192, 300),
             num("bright", "Infill brightness", 0, 255, 255), flag("reinforce", "Reinforce infill", false)},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::InfillParams p;
                p.selection = c.selection(); p.type = layerops::InfillAlgorithm(v.i("type"));
                p.floor_ceil_thickness_mm = v.d("fc"); p.wall_thickness = v.u("wall"); p.infill_thickness = v.u("thick");
                p.infill_spacing = v.u("space"); p.infill_brightness = uint8_t(v.i("bright")); p.reinforce_infill = v.b("reinforce");
                BuiltOp op;
                op.mask = [p](auto &layers, const auto &z, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                    auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                    require_valid(layerops::validate_infill(q, layers, z));
                    layerops::infill_layers(layers, z, q, pr);
                };
                op.kernel = [p](const layerops::StreamLayout &l) { return layerops::infill_kernel(p, l); };
                op.kernel_needs_z = true;
                return op;
            });
        def.previewable = false;
    }
    {
        auto &def = add(OpGroup::Pixels, "Pixel arithmetic", "Combines every pixel with a value or pattern (set, add, multiply, bitwise, keep region, ...).",
            {pick("op", "Operator", {"Set", "Add", "Subtract", "Multiply", "Divide", "Minimum", "Maximum", "Bitwise NOT", "Bitwise AND",
                                     "Bitwise OR", "Bitwise XOR", "Absolute difference", "Threshold", "Keep region (needs ROI)", "Discard region (needs ROI)"}, 0),
             pick("apply", "Apply to", {"All pixels", "Model", "Model surface", "Model surface and inset", "Model inner", "Model walls"},
                  int(layerops::ArithmeticApply::Model)),
             num("ws", "Wall thickness start (px)", 1, 4096, 20), num("we", "Wall thickness end (px)", 1, 4096, 20),
             flag("chamfer", "Fade the wall thickness over the range", false),
             num("value", "Value (threshold level for Threshold)", 0, 255, 255), dbl("step", "Value change per layer", -255, 255, 0, 3),
             pick("tt", "Threshold type", threshold_names(), 0), num("tmax", "Threshold maximum", 0, 255, 255),
             flag("usepat", "Use pattern instead of the value", false), flag("inv", "Invert the pattern", false),
             num("per", "Alternate every N layers", 1, 10000, 1),
             txt("pattern", "Pattern", ""), txt("alternate", "Alternate pattern", "")},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::PixelArithmeticParams p;
                p.selection = c.selection(); p.op = layerops::ArithmeticOperator(v.i("op"));
                p.apply = layerops::ArithmeticApply(v.i("apply"));
                p.wall_thickness_start = v.u("ws"); p.wall_thickness_end = v.u("we"); p.wall_chamfer = v.b("chamfer");
                p.value = uint8_t(v.i("value")); p.value_step = float(v.d("step")); p.use_pattern = v.b("usepat");
                p.threshold_type = layerops::ThresholdType(v.i("tt")); p.threshold_max = uint8_t(v.i("tmax"));
                p.pattern_alternate_per_layers = v.u("per"); p.pattern_invert = v.b("inv");
                p.pattern_text = v.s("pattern"); p.pattern_text_alternate = v.s("alternate");
                require_valid(layerops::validate_pixel_arithmetic(p));
                BuiltOp op;
                op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                    auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer;
                    layerops::pixel_arithmetic_layers(layers, q, pr);
                };
                op.kernel = [p](const layerops::StreamLayout &l) { return layerops::pixel_arithmetic_kernel(p, l); };
                return op;
            });
        def.pattern_helper = true;
    }
    {
        auto &def = add(OpGroup::Pixels, "Raft relief", "Relieves the raft with holes, dimming, decimation or tabs. Works on the whole document; "
                        "the optional region of interest limits it.",
            {pick("type", "Relief type", {"Relief (holes)", "Dimming", "Decimate", "Tabs"}, 0),
             num("mask", "First layer with supports (0 = auto-detect)", 0, 1000000, 0), num("ign", "Ignore first layers", 0, 1000000, 0),
             num("low", "Low brightness", 0, 255, 0), num("dil", "Dilate iterations", 0, 1000, 15), num("margin", "Wall margin (px)", 0, 4096, 40),
             num("hole", "Hole diameter (px)", 1, 4096, 80), num("spacing", "Hole spacing (px)", 0, 4096, 40),
             num("high", "Tab brightness", 0, 255, 255), num("tbase", "Tab triangle base (px)", 1, 4096, 200),
             num("theight", "Tab triangle height (px)", 1, 4096, 250)},
            [](const OpValues &v, const OpContext &c) {
                static constexpr layerops::RaftReliefType kTypes[] = {layerops::RaftReliefType::Relief, layerops::RaftReliefType::Dimming,
                                                                       layerops::RaftReliefType::Decimate, layerops::RaftReliefType::Tabs};
                layerops::RaftReliefParams p;
                p.roi = c.roi; p.type = kTypes[std::clamp(v.i("type"), 0, 3)];
                p.mask_layer_index = size_t(v.i("mask")); p.ignore_first_layers = v.u("ign"); p.low_brightness = uint8_t(v.i("low"));
                p.dilate_iterations = v.u("dil"); p.wall_margin = v.u("margin"); p.hole_diameter = v.u("hole");
                p.hole_spacing = v.u("spacing"); p.high_brightness = uint8_t(v.i("high"));
                p.tab_triangle_base = v.u("tbase"); p.tab_triangle_height = v.u("theight");
                require_valid(layerops::validate_raft_relief(p));
                BuiltOp op;
                op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &, const sla::LayerProgress &pr) {
                    if (!layerops::raft_relief_layers(layers, p, pr))
                        invalid("No raft or support layers were found, so nothing was changed.");
                };
                op.kernel = [p](const layerops::StreamLayout &l) {
                    auto kernel = layerops::raft_relief_kernel(p, l);
                    if (!kernel) invalid("No raft or support layers were found, so nothing was changed.");
                    return *kernel;
                };
                return op;
            });
        def.uses_range = false; def.previewable = false;
    }

    // ---- Layers ----
    {
        auto &def = add(OpGroup::Layers, "Clone layers", "Repeats every layer of the range N more times (later layers move up).",
            {num("n", "Clones per layer", 1, 1000, 1), flag("same", "Keep the same Z (repeated exposure)", false)},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::CloneParams p;
                p.first_layer = c.first; p.last_layer = c.last; p.clones = v.u("n"); p.keep_same_z = v.b("same");
                p.default_layer_height_mm = nominal_layer_height_mm(c.z_mm);
                require_valid(layerops::validate_clone(p));
                BuiltOp op;
                op.stream = sla::liststream::clone(p);
                return op;
            });
        def.previewable = false;
    }
    {
        auto &def = add(OpGroup::Layers, "Remove layers", "Removes the layers of the range (optionally only nearly empty ones).",
            {flag("use", "Only remove layers with few pixels", false), num("px", "Remove layers with at most N lit pixels", 0, 100000000, 0)},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::RemoveParams p;
                p.first_layer = c.first; p.last_layer = c.last; p.use_threshold = v.b("use"); p.pixel_threshold = v.u("px");
                BuiltOp op;
                op.stream = sla::liststream::remove(p);
                return op;
            });
        def.previewable = false;
    }
    {
        const auto presets = layerops::reheight_presets(initial.layer_count, nominal_layer_height_mm(initial.z_mm));
        auto &def = add(OpGroup::Layers, "Change layer height", "Splits or merges layers to a new height, or shifts the Z position of a range.",
            {pick("method", "Method", {"Re-height (split / merge layers)", "Offset Z position of the range"}, 0),
             pick("preset", "New layer height", reheight_labels(presets), 0),
             pick("aa", "Anti-aliasing when merging", {"None", "Difference", "Average"}, 0),
             dbl("off", "Z offset (mm)", -10, 10, 0.01, 3)},
            [presets](const OpValues &v, const OpContext &c) {
                layerops::ReHeightParams p;
                p.method = layerops::ReHeightMethod(v.i("method"));
                if (p.method == layerops::ReHeightMethod::ReHeight) {
                    if (presets.empty()) invalid("This layer height cannot be split or merged exactly.");
                    p.preset = presets.at(size_t(std::clamp(v.i("preset"), 0, int(presets.size()) - 1)));
                }
                p.anti_aliasing = layerops::ReHeightAntiAliasing(v.i("aa"));
                p.position_z_offset_mm = v.d("off");
                p.first_layer = c.first; p.last_layer = c.last;
                BuiltOp op;
                op.stream = sla::liststream::reheight(p);
                return op;
            });
        def.previewable = false;
    }
    {
        auto &def = add(OpGroup::Layers, "Layer arithmetic", "Computes layers from other layers, e.g. \"10,11 = 11+12-10*5; 3:7 = 3|4\" "
                        "(operators + - * / & | ^ $ evaluated left to right). Layer numbers in the sentence are 0-based, like UVtools.",
            {txt("sentence", "Sentence", "")},
            [](const OpValues &v, const OpContext &c) {
                layerops::LayerArithmeticParams p;
                p.sentence = v.s("sentence"); p.roi = c.roi;
                require_valid(layerops::validate_layer_arithmetic(p, c.layer_count));
                BuiltOp op;
                op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &, const sla::LayerProgress &pr) {
                    layerops::layer_arithmetic(layers, p, pr);
                };
                op.kernel = [p](const layerops::StreamLayout &l) { return layerops::layer_arithmetic_kernel(p, l); };
                return op;
            });
        def.uses_range = false; def.previewable = false;
    }
    {
        auto &def = add(OpGroup::Layers, "Double exposure", "Prints every layer of the range twice (second pass uses an eroded copy). Needs a document with exposure data.",
            {num("bottom", "Bottom layer count", 0, 100000, double(initial.bottom_layers)),
             dbl("fbe", "First pass bottom exposure (s)", 0, 1000, 0, 2), dbl("fne", "First pass normal exposure (s)", 0, 1000, 0, 2),
             dbl("sbe", "Second pass bottom exposure (s)", 0, 1000, 0, 2), dbl("sne", "Second pass normal exposure (s)", 0, 1000, 0, 2),
             num("fbr", "First pass bottom erode (px)", 0, 100, 4), num("sbr", "Second pass bottom erode (px)", 0, 100, 0),
             num("fnr", "First pass normal erode (px)", 0, 100, 1), num("snr", "Second pass normal erode (px)", 0, 100, 0),
             flag("diff", "Second layer = difference to the first", true), num("ov", "Difference overlap erode (px)", 0, 100, 10),
             flag("lift", "Lift before the second pass", true), dbl("liftmm", "Second pass lift (mm)", 0, 100, 0.1, 3),
             flag("wait", "Wait before the second pass", true), dbl("waits", "Second pass wait before cure (s)", 0, 1000, 0, 2)},
            [](const OpValues &v, const OpContext &c) {
                require_range(c);
                layerops::DoubleExposureParams p;
                p.first_layer = c.first; p.last_layer = c.last; p.bottom_layer_count = size_t(v.i("bottom"));
                p.first_bottom_exposure_s = v.d("fbe"); p.first_normal_exposure_s = v.d("fne");
                p.second_bottom_exposure_s = v.d("sbe"); p.second_normal_exposure_s = v.d("sne");
                p.first_bottom_erode = v.u("fbr"); p.second_bottom_erode = v.u("sbr");
                p.first_normal_erode = v.u("fnr"); p.second_normal_erode = v.u("snr");
                p.second_layer_difference = v.b("diff"); p.second_layer_difference_overlap_erode = v.u("ov");
                p.different_settings_for_second_layer = true;
                p.second_layer_lift_enabled = v.b("lift"); p.second_layer_lift_mm = v.d("liftmm");
                p.second_layer_wait_enabled = v.b("wait"); p.second_layer_wait_s = v.d("waits");
                require_valid(layerops::validate_double_exposure(p, c.z_mm));
                BuiltOp op;
                op.stream = sla::liststream::double_exposure(p);
                return op;
            });
        def.previewable = false;
    }
    append_batch2_defs(defs, initial);
    return defs;
}

// ---- 2b. Batch 2: per-layer parameter operations and exports ---------------------------------------------
} // namespace layer_editor_detail
} // namespace Slic3r::GUI
