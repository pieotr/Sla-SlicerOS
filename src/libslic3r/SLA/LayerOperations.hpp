// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Native port of UVtools layer operations (batch 1, goal G6). Pure backend: operates on plain
// 8-bit masks so it can be applied through LayerDocument (Undo/Redo) by the GUI layer later.
// No OpenCV / Emgu / .NET at runtime; everything is plain C++ pixel code.
//
// Operations matrix (UVtools op -> our function -> notes / deviations)
// ---------------------------------------------------------------------------------------------
// OperationMove          -> move_layers (+ compute_move_destination)   exact port; ROI empty = model bounds.
// OperationResize        -> resize_layers                              bilinear warp; OpenCV fixed-point
//                                                                      rounding may differ by +-1 level.
// OperationRotate        -> rotate_layers                              same rotation matrix as OpenCV
//                                                                      (angle > 0 = clockwise), bilinear.
// OperationFlip          -> flip_layers                                exact (H / V / both, blend = saturating add).
// OperationPattern       -> pattern_layers (+ compute_pattern_limits)  exact; anchor reuses move_layers.
// OperationSolidify      -> solidify_layers                            hole area = pixel count of the hole
//                                                                      (UVtools: polygon area of its contour).
// OperationThreshold     -> threshold_layers                           Binary/BinaryInv/Trunc/ToZero/ToZeroInv
//                                                                      + Otsu/Triangle auto threshold. No "Mask" type.
// OperationPixelDimming  -> pixel_dimming_layers (+ pattern text       exact; only the default 3x3 rectangle
//                           parser / generators)                       kernel (no custom kernels).
// OperationInfill        -> infill_layers                              all 8 algorithms. Thick strokes use a
//                                                                      distance-to-segment brush, so edges may
//                                                                      differ by 1 px from OpenCV.
// OperationPixelArithmetic -> pixel_arithmetic_layers                  Set..Threshold, Keep/Discard region,
//                                                                      all apply methods, patterns, value step.
//                                                                      NOT ported: Corrode (noise), ignore-area.
// OperationRaftRelief    -> raft_relief_layers                         Relief, Dimming, Decimate, Tabs. Support
//                                                                      layer auto-detect counts blobs instead of
//                                                                      Hough circles. NOT ported: LinkedLines.
// OperationLayerClone    -> clone_layers                               exact incl. z shifting.
// OperationLayerRemove   -> remove_layers                              exact incl. z shifting.
// OperationLayerReHeight -> reheight_layers (+ reheight_presets)       multiply / divide / z offset. Pyramid
//                                                                      smoothing of AA modes is NOT applied.
// OperationLayerArithmetic -> layer_arithmetic (+ parse)               exact sentence grammar.
// OperationDoubleExposure -> double_exposure_layers                    mask part exact; exposure/lift data are
//                                                                      returned as per-layer vectors.
// OperationChangeResolution -> change_resolution_layers                crop/pad around model bounds (+ ratio fix).
//
// ---- batch 2 (per-layer parameter table + layer list operations + calculator) -----------------------
// OperationDynamicLifts  -> dynamic_lifts                              exact (Traditional / FullRange); float rounding
//                                                                      uses ties-to-even like MathF.Round.
// OperationFadeExposureTime -> fade_exposure_time                      exact incl. step rounding; the transition layer
//                                                                      count is returned instead of written to a file.
// OperationLightBleedCompensation -> light_bleed_compensation_layers   Similarities / Bridges / Both, Previous / Next /
//                                                                      Both. Neighbours are read from the ORIGINAL layers
//                                                                      (UVtools races with its own parallel writes). No
//                                                                      image-mask feature (use the ROI).
// OperationPhasedExposure -> phased_exposure_layers                    exact incl. the "difference only" overlap quirk;
//                                                                      empty layers are kept (UVtools drops them) and the
//                                                                      trailing layers are placed correctly for any
//                                                                      phase count (UVtools is only right for 2).
// OperationRaiseOnPrintFinish -> raise_on_print_finish                 exact.
// OperationEditParameters -> edit_parameters                           per-layer (set/skip pattern) and global bottom /
//                                                                      normal edits on the parameter table.
// OperationTimelapse     -> timelapse_layers                           Lift height and Virtual layer modes, custom lift.
// OperationDynamicLayerHeight -> dynamic_layer_height_layers           exact stacking algorithm; returns the head layer
//                                                                      of every stack as source index. Cache RAM size is
//                                                                      irrelevant natively. AA strip / reconstruct done.
// OperationLithophane    -> lithophane_layers                          takes an already decoded 8-bit image (no image
//                                                                      codec in this module); full pipeline incl. base,
//                                                                      packing and duplication of threshold layers.
//                                                                      Thumbnail text overlay NOT ported.
// OperationRepairLayers  -> repair_layers                              islands (recursive removal, attach below), resin
//                                                                      traps (fill + overlap), suction cups (drill), gap
//                                                                      closing, noise removal, empty layers. Always
//                                                                      re-detects (no issue session). Resin trap fill uses
//                                                                      the pixel region + box dilation instead of a
//                                                                      stroked contour. The last layer of the range IS
//                                                                      processed (UVtools' Parallel.For skips it).
// OperationCalculator    -> mm_to_pixels / light_off_delay_* /         pure functions; print time estimator is a
//                           optimal_model_tilt / estimate_print_time  simplification of Layer.CalculatePrintTime.
// Layer exports live in LayerExports.hpp.
//
// Not covered by this module (UVtools "Mask" image operation): the image-mask feature; use a ROI instead.
//
// Conventions: pixel coordinates, origin top-left, +y down. Layer ranges are inclusive. All multi-layer
// functions compute results off to the side and commit at the end, so a cancelled run (progress callback
// returns false -> LayerOperationCancelled) leaves the input untouched. Validation errors throw
// std::invalid_argument with the UVtools-style message; `validate_*` return the same text without throwing
// (empty string = valid).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

#include "LayerIssues.hpp"
#include "LayerProgress.hpp"

namespace Slic3r::sla::layerops {

// 8-bit grayscale layer image, row-major, 255 = full exposure.
struct Mask {
    size_t               width  = 0;
    size_t               height = 0;
    std::vector<uint8_t> px;

    Mask() = default;
    Mask(size_t w, size_t h, uint8_t fill = 0) : width(w), height(h), px(w * h, fill) {}
    uint8_t &at(size_t x, size_t y) { return px[y * width + x]; }
    uint8_t  at(size_t x, size_t y) const { return px[y * width + x]; }
    size_t   nonzero_count() const;
    bool     operator==(const Mask &o) const { return width == o.width && height == o.height && px == o.px; }
    bool     operator!=(const Mask &o) const { return !(*this == o); }
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    int  right() const { return x + w; }
    int  bottom() const { return y + h; }
    bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect &o) const { return !(*this == o); }
};

// Same nine anchors as UVtools; None is only valid where documented.
enum class Anchor { TopLeft, TopCenter, TopRight, MiddleLeft, MiddleCenter, MiddleRight, BottomLeft, BottomCenter, BottomRight, None };

constexpr size_t kLastLayer = std::numeric_limits<size_t>::max();

// Inclusive layer range + optional region of interest (empty ROI = whole image).
struct LayerSelection {
    size_t first_layer = 0;
    size_t last_layer  = kLastLayer; // clamped to the last existing layer
    Rect   roi;
};

// ---- Generic helpers (public: GUI preview and tests use them) ------------------------------------
Rect bounding_rect(const Mask &m);                              // non-zero pixel bounds, empty if blank
Rect bounding_rect(const std::vector<Mask> &layers);            // union over all layers
Rect union_bounds(const Rect &a, const Rect &b);                // union of two bounds (empty ones ignored)
// UVtools FileFormat.MutateGetIterationChamfer: wall thickness (px) for a layer when fading start -> end.
int  iteration_chamfer(size_t layer, size_t first, size_t last, int start, int end, bool fade);

// A tiled brightness matrix used by pixel dimming / arithmetic (rows x cols, values 0..255).
struct PixelPattern {
    size_t               rows = 0, cols = 0;
    std::vector<uint8_t> v;
    bool                 empty() const { return v.empty(); }
};
// Parses UVtools pattern text ("255 127\n127 255"). Empty/blank text -> empty pattern and no error.
PixelPattern parse_pattern_text(const std::string &text, std::string *error);
struct PatternTexts { std::string pattern, alternate; };
// Names: Chessboard, Sparse, Crosses, Strips, Pyramid, Rhombus, Hearts, Slashes, Waves, Solid.
PatternTexts generate_dimming_pattern(const std::string &name, uint8_t brightness);
// Names: Rectilinear, "Square grid", Waves, Lattice.
PatternTexts generate_infill_pattern(const std::string &name, unsigned thickness, unsigned spacing);

// ---- Move -----------------------------------------------------------------------------------------
struct MoveParams {
    LayerSelection selection;           // roi = SOURCE rectangle (empty = bounds of the whole model)
    Anchor         anchor        = Anchor::MiddleCenter;
    int            margin_left   = 0, margin_top = 0, margin_right = 0, margin_bottom = 0; // px
    bool           is_cut_move   = true; // false = copy
};
Rect        compute_move_destination(const Rect &src, size_t image_w, size_t image_h, Anchor, int ml, int mt, int mr, int mb);
std::string validate_move(const MoveParams &, const std::vector<Mask> &layers);
void        move_layers(std::vector<Mask> &layers, const MoveParams &, const LayerProgress &progress = {});

// ---- Resize ---------------------------------------------------------------------------------------
struct ResizeParams {
    LayerSelection selection;
    double x_percent = 100, y_percent = 100;
    bool   constrain_xy = false; // true: y follows x
    bool   is_fade      = false; // linearly fade the scale back to 100 % across the range
};
std::string validate_resize(const ResizeParams &);
void        resize_layers(std::vector<Mask> &layers, const ResizeParams &, const LayerProgress &progress = {});

// ---- Rotate ---------------------------------------------------------------------------------------
struct RotateParams {
    LayerSelection selection;
    double angle_degrees = 90; // positive = clockwise
};
std::string validate_rotate(const RotateParams &);
void        rotate_layers(std::vector<Mask> &layers, const RotateParams &, const LayerProgress &progress = {});

// ---- Flip -----------------------------------------------------------------------------------------
enum class FlipDirection { Horizontal, Vertical, Both };
struct FlipParams {
    LayerSelection selection;
    FlipDirection  direction = FlipDirection::Horizontal;
    bool           make_copy = false; // blend: original + flipped (saturating add)
};
void flip_layers(std::vector<Mask> &layers, const FlipParams &, const LayerProgress &progress = {});

// ---- Pattern --------------------------------------------------------------------------------------
struct PatternParams {
    LayerSelection selection;       // roi = rectangle that is duplicated (empty = model bounds)
    Anchor         anchor = Anchor::None; // None = keep the centred pattern; otherwise move it afterwards
    unsigned       cols = 1, rows = 1;
    unsigned       col_spacing = 0, row_spacing = 0; // px between copies
};
struct PatternLimits { unsigned max_cols, max_rows, max_col_spacing, max_row_spacing; };
PatternLimits compute_pattern_limits(size_t image_w, size_t image_h, const Rect &src);
std::string   validate_pattern(const PatternParams &, const std::vector<Mask> &layers);
void          pattern_layers(std::vector<Mask> &layers, const PatternParams &, const LayerProgress &progress = {});

// ---- Solidify -------------------------------------------------------------------------------------
enum class AreaCheck { More, Less };
struct SolidifyParams {
    LayerSelection selection;
    unsigned       minimum_area = 1;       // px^2; 0 disables the area test
    AreaCheck      area_check   = AreaCheck::More; // More: fill holes >= area. Less: fill holes <= area
};
void solidify_layers(std::vector<Mask> &layers, const SolidifyParams &, const LayerProgress &progress = {});

// ---- Threshold ------------------------------------------------------------------------------------
enum class ThresholdType { Binary, BinaryInv, Trunc, ToZero, ToZeroInv, Otsu, Triangle };
struct ThresholdParams {
    LayerSelection selection;
    uint8_t        threshold = 127;
    uint8_t        maximum   = 255;
    ThresholdType  type      = ThresholdType::Binary; // Otsu/Triangle = Binary with auto threshold
};
// Applies the threshold to one image in place (exposed for pixel arithmetic and tests).
void apply_threshold(Mask &m, uint8_t threshold, uint8_t maximum, ThresholdType type);
void threshold_layers(std::vector<Mask> &layers, const ThresholdParams &, const LayerProgress &progress = {});

// ---- Pixel dimming --------------------------------------------------------------------------------
struct PixelDimmingParams {
    LayerSelection selection;
    bool           lightening_pixels = false; // add instead of subtract
    unsigned       wall_thickness_start = 10, wall_thickness_end = 10; // px
    bool           walls_only = false;        // dim only the walls, keep the inside
    bool           chamfer    = false;        // fade wall thickness start -> end over the range
    std::string    pattern_text, alternate_pattern_text;
    unsigned       alternate_pattern_per_layers = 1;
};
std::string validate_pixel_dimming(const PixelDimmingParams &);
void        pixel_dimming_layers(std::vector<Mask> &layers, const PixelDimmingParams &, const LayerProgress &progress = {});

// ---- Infill ---------------------------------------------------------------------------------------
enum class InfillAlgorithm { Pillars, Concentric, Waves, CubicCross, CubicCrossAlternating, CubicStar, Honeycomb, Gyroid };
struct InfillParams {
    LayerSelection  selection;      // roi empty = model bounds
    InfillAlgorithm type = InfillAlgorithm::CubicCrossAlternating;
    double          floor_ceil_thickness_mm = 3.0;
    unsigned        wall_thickness = 64, infill_thickness = 45, infill_spacing = 300; // px
    uint8_t         infill_brightness = 255;
    bool            reinforce_infill  = false;
};
// z_mm = top Z of every layer (same length as layers); used for floor/ceiling thickness and layer height.
std::string validate_infill(const InfillParams &, const std::vector<Mask> &layers, const std::vector<double> &z_mm);
void        infill_layers(std::vector<Mask> &layers, const std::vector<double> &z_mm, const InfillParams &,
                          const LayerProgress &progress = {});

// ---- Pixel arithmetic -----------------------------------------------------------------------------
enum class ArithmeticOperator { Set, Add, Subtract, Multiply, Divide, Minimum, Maximum, BitwiseNot, BitwiseAnd, BitwiseOr,
                                BitwiseXor, AbsDiff, Threshold, KeepRegion, DiscardRegion };
enum class ArithmeticApply { All, Model, ModelSurface, ModelSurfaceAndInset, ModelInner, ModelWalls };
struct PixelArithmeticParams {
    LayerSelection     selection; // KeepRegion / DiscardRegion require a ROI
    ArithmeticOperator op     = ArithmeticOperator::Set;
    ArithmeticApply    apply  = ArithmeticApply::Model;
    unsigned           wall_thickness_start = 20, wall_thickness_end = 20; // px
    bool               wall_chamfer = false;
    uint8_t            value        = 255;
    float              value_step   = 0; // per-layer change of value
    bool               use_pattern  = false;
    ThresholdType      threshold_type = ThresholdType::Binary; // Threshold operator; `value` is the threshold
    uint8_t            threshold_max  = 255;
    unsigned           pattern_alternate_per_layers = 1;
    bool               pattern_invert = false;
    std::string        pattern_text, pattern_text_alternate;
};
std::string validate_pixel_arithmetic(const PixelArithmeticParams &);
void        pixel_arithmetic_layers(std::vector<Mask> &layers, const PixelArithmeticParams &, const LayerProgress &progress = {});

// ---- Raft relief ----------------------------------------------------------------------------------
enum class RaftReliefType { Relief, LinkedLines /* not ported */, Dimming, Decimate, Tabs };
struct RaftReliefParams {
    Rect           roi;
    RaftReliefType type = RaftReliefType::Relief;
    size_t         mask_layer_index   = 0;   // first layer WITH supports; 0 = auto-detect
    unsigned       ignore_first_layers = 0;
    uint8_t        low_brightness   = 0;
    unsigned       dilate_iterations = 15, wall_margin = 40, hole_diameter = 80, hole_spacing = 40; // px
    uint8_t        high_brightness  = 255;   // tab triangle brightness
    unsigned       tab_triangle_base = 200, tab_triangle_height = 250; // px
};
std::string validate_raft_relief(const RaftReliefParams &);
// Returns false when no raft/support layer was found (nothing changed).
bool        raft_relief_layers(std::vector<Mask> &layers, const RaftReliefParams &, const LayerProgress &progress = {});

// ---- Layer list operations (masks + z in mm) -------------------------------------------------------
// source_index[i] = index of the input layer that produced output layer i (for copying exposure data).
struct LayerListResult { std::vector<size_t> source_index; };

struct CloneParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    unsigned clones = 1;
    bool     keep_same_z = false;
    double   default_layer_height_mm = 0.05; // used when a layer has zero relative height
};
std::string     validate_clone(const CloneParams &);
// Pure list part of Clone (no pixels): output i is a copy of input source_index[i]; `z_mm` becomes the output Z.
LayerListResult clone_plan(size_t layer_count, std::vector<double> &z_mm, const CloneParams &, const LayerProgress &progress = {});
LayerListResult clone_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const CloneParams &,
                             const LayerProgress &progress = {});

struct RemoveParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    bool     use_threshold   = false;
    unsigned pixel_threshold = 0; // remove only layers with <= this many non-zero pixels
};
std::string     validate_remove(const RemoveParams &, const std::vector<Mask> &layers);
// List part of Remove. `nonzero(i)` is only called (for layers of the range) when use_threshold is set.
LayerListResult remove_plan(size_t layer_count, std::vector<double> &z_mm, const RemoveParams &,
                            const std::function<size_t(size_t)> &nonzero, const LayerProgress &progress = {});
LayerListResult remove_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const RemoveParams &,
                              const LayerProgress &progress = {});

enum class ReHeightMethod { ReHeight, OffsetPositionZ };
enum class ReHeightAntiAliasing { None, Difference, Average };
struct ReHeightPreset { bool is_multiply; uint8_t modifier; double layer_height_mm; size_t layer_count; };
std::vector<ReHeightPreset> reheight_presets(size_t layer_count, double layer_height_mm);
struct ReHeightParams {
    ReHeightMethod       method = ReHeightMethod::ReHeight;
    ReHeightPreset       preset{false, 2, 0.025, 0}; // used by ReHeight (pick from reheight_presets)
    ReHeightAntiAliasing anti_aliasing = ReHeightAntiAliasing::None; // only for multiply presets
    double               position_z_offset_mm = 0.01;                // OffsetPositionZ
    size_t first_layer = 0, last_layer = kLastLayer;                 // OffsetPositionZ range
};
std::string     validate_reheight(const ReHeightParams &, const std::vector<Mask> &layers, const std::vector<double> &z_mm);
// List part of ReHeight: source_index[k] is the (first) input layer of output k; for a multiply preset output k
// merges inputs k*modifier .. k*modifier+modifier-1 with reheight_merge.
LayerListResult reheight_plan(size_t layer_count, std::vector<double> &z_mm, const ReHeightParams &, const LayerProgress &progress = {});
Mask            reheight_merge(const std::vector<const Mask *> &group, const ReHeightParams &);
LayerListResult reheight_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const ReHeightParams &,
                                const LayerProgress &progress = {});

// Layer arithmetic: "10,11,12 = 11+12-10*5; 3:7 = 3|4". Operators + - * / & | ^ $ (absdiff), sequential.
struct LayerArithmeticOp { size_t layer; char op; }; // op is the operator following this layer ('\0' = none)
struct LayerArithmeticGroup {
    std::vector<size_t>            set_layers;
    std::vector<LayerArithmeticOp> operations;
    bool is_valid() const;
};
// Returns false (and sets error) for a malformed sentence.
bool        parse_layer_arithmetic(const std::string &sentence, size_t layer_count, std::vector<LayerArithmeticGroup> &out,
                                   std::string *error);
struct LayerArithmeticParams { std::string sentence; Rect roi; };
std::string validate_layer_arithmetic(const LayerArithmeticParams &, size_t layer_count);
void        layer_arithmetic(std::vector<Mask> &layers, const LayerArithmeticParams &, const LayerProgress &progress = {});

// Double exposure: every layer of the range is printed twice (same z). Output carries per-layer data.
struct DoubleExposureParams {
    size_t   first_layer = 0, last_layer = kLastLayer;
    size_t   bottom_layer_count = 0; // layers with index < this are "bottom" layers
    double   first_bottom_exposure_s = 0, first_normal_exposure_s = 0;
    double   second_bottom_exposure_s = 0, second_normal_exposure_s = 0;
    unsigned first_bottom_erode = 4, second_bottom_erode = 0, first_normal_erode = 1, second_normal_erode = 0; // px
    bool     second_layer_difference = true;
    unsigned second_layer_difference_overlap_erode = 10; // px
    bool     different_settings_for_second_layer = false;
    bool     second_layer_lift_enabled = true;      double second_layer_lift_mm = 0.1;
    bool     second_layer_wait_enabled = true;      double second_layer_wait_s  = 0;
};
struct DoubleExposureResult {
    LayerListResult       list;
    std::vector<double>   exposure_s;          // per output layer, < 0 = untouched layer (keep source value)
    std::vector<bool>     is_second;           // true for the cloned (second) pass
    std::vector<double>   lift_mm;             // per output layer, < 0 = keep source value
    std::vector<double>   wait_before_cure_s;  // per output layer, < 0 = keep source value
    size_t                bottom_layer_count = 0; // new count (cloned bottoms also count)
};
std::string          validate_double_exposure(const DoubleExposureParams &, const std::vector<double> &z_mm);
// Per-layer pixel work of Double exposure: the first and the second pass of layer `i`.
void                 double_exposure_pair(const Mask &layer, size_t i, const DoubleExposureParams &, Mask &first, Mask &second);
struct DoubleExposurePiece { enum class Role { Copy, First, Second } role; size_t source; };
// List part: output Z and, per output layer, which pass of which input layer it is.
DoubleExposureResult double_exposure_plan(const std::vector<double> &z_mm, const DoubleExposureParams &, std::vector<double> &out_z,
                                          std::vector<DoubleExposurePiece> &pieces);
DoubleExposureResult double_exposure_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const DoubleExposureParams &,
                                            const LayerProgress &progress = {});

// Change resolution: model bounds are copied to the centre of the new canvas (or cropped around it).
struct ChangeResolutionParams {
    size_t new_width = 0, new_height = 0;
    bool   fix_ratio = false;
    double ratio_x = 1.0, ratio_y = 1.0; // pixel pitch ratio old/new (see pixel_pitch_ratio)
};
// UVtools NewFixedRatio: old pixel pitch / new pixel pitch, rounded to 3 decimals. Returns 1 for bad input.
double      pixel_pitch_ratio(double old_display_mm, size_t old_res, double new_display_mm, size_t new_res);
std::string validate_change_resolution(const ChangeResolutionParams &, const std::vector<Mask> &layers);
// Same validation from the layout alone: the union of the model bounds of every layer.
std::string validate_change_resolution(const ChangeResolutionParams &, size_t layer_count, size_t width, size_t height, const Rect &bounds);
// One output layer; `region` is the model bounds over ALL layers (bounding_rect).
Mask        change_resolution_layer(const Mask &in, const Rect &region, const ChangeResolutionParams &);
void        change_resolution_layers(std::vector<Mask> &layers, const ChangeResolutionParams &, const LayerProgress &progress = {});

// =====================================================================================================
// Batch 2: per-layer print parameter table
// =====================================================================================================

// Print parameters of one layer. Units: seconds, mm, mm/min. Setters of UVtools round to 2 decimals;
// operations below do the same through round2().
struct LayerParams {
    bool    is_bottom = false;
    double  exposure_s = 0, wait_before_cure_s = 0, wait_after_cure_s = 0, wait_after_lift_s = 0;
    double  lift_height_mm = 0, lift_speed = 0, lift_height2_mm = 0, lift_speed2 = 0;
    double  retract_speed = 0, retract_height2_mm = 0, retract_speed2 = 0;
    uint8_t light_pwm = 255;

    double lift_height_total() const;                 // lift 1 + lift 2
    void   set_lift_height_total(double mm);          // UVtools: lift 1 = mm, lift 2 = 0
    double retract_height_total() const { return lift_height_total(); }
    bool   operator==(const LayerParams &o) const;
    bool   operator!=(const LayerParams &o) const { return !(*this == o); }
};

// What the target printer / file format can do plus global values the operations need.
struct PrinterContext {
    double layer_height_mm = 0.05;
    double machine_z_mm    = 0;        // 0 = unknown
    bool   is_gcode        = false;    // UVtools SupportGCode
    double maximum_speed   = 200;      // mm/min, UVtools SlicerFile.MaximumSpeed
    double min_normal_speed = 5;       // mm/min, UVtools SlicerFile.MinimumNormalSpeed
    double global_lift_height_mm = 5;  // fallback "slow lift" height of the timelapse virtual layer
    bool   can_use_lift_height2 = true, can_use_lift_speed2 = true;
    bool   can_use_retract_height2 = true, can_use_retract_speed2 = true;
    bool   can_use_wait_after_lift = true, can_use_wait_before_cure = true;
    std::optional<FlipDirection> display_mirror; // Lithophane "mirror": flips the way the display is mirrored (default horizontal)
};

// Mutable view of the model every parameter operation works on. Lists must have the same size.
// `params[i].is_bottom` is the bottom layer flag (UVtools IsBottomLayer).
inline double round2(double v) { return std::nearbyint(v * 100.0) / 100.0; }

// Per-layer table of non-zero pixel counts (UVtools Layer.NonZeroPixelCount), parallel.
std::vector<size_t> layer_pixel_counts(const std::vector<Mask> &layers);

// ---- Dynamic lifts ----------------------------------------------------------------------------------
enum class DynamicLiftsMethod { Traditional, FullRange };
struct DynamicLiftsParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    DynamicLiftsMethod method = DynamicLiftsMethod::Traditional;
    double smallest_bottom_lift_height = 0, largest_bottom_lift_height = 0;
    double smallest_lift_height = 0, largest_lift_height = 0;
    double slowest_bottom_lift_speed = 0, fastest_bottom_lift_speed = 0;
    double slowest_lift_speed = 0, fastest_lift_speed = 0;
};
// UVtools InitWithSlicerFile: every unset (<= 0) limit takes the table's first bottom / first normal layer value.
DynamicLiftsParams dynamic_lifts_defaults(const std::vector<LayerParams> &table, DynamicLiftsParams p = {});
std::string        validate_dynamic_lifts(const DynamicLiftsParams &);
void               dynamic_lifts(const std::vector<Mask> &layers, std::vector<LayerParams> &table, const DynamicLiftsParams &,
                                 const LayerProgress &progress = {});
// The lifts only depend on the number of lit pixels of every layer: `counts[i]` = nonzero pixels of layer i. This is
// what dynamic_lifts runs after counting; LayerDocument callers count with scan_masks (one layer decoded at a time).
void               dynamic_lifts_from_counts(const std::vector<size_t> &counts, std::vector<LayerParams> &table, const DynamicLiftsParams &,
                                             const LayerProgress &progress = {});

// ---- Fade exposure time -----------------------------------------------------------------------------
struct FadeExposureParams {
    size_t first_layer = 0;
    size_t layer_count = 10;               // the range is first_layer .. first_layer + layer_count - 1
    double from_exposure_s = 0, to_exposure_s = 0;
    bool   disable_firmware_transition_layers = true;
    bool   transition_type_firmware = false; // file format uses firmware transition layers
    size_t bottom_layer_count = 0;           // table value, decides whether the transition layer count is updated
};
struct FadeExposureResult {
    double increment_s = 0;
    bool   transition_layer_count_changed = false;
    size_t transition_layer_count = 0;     // valid when ..._changed
};
FadeExposureParams fade_exposure_defaults(const std::vector<LayerParams> &table, FadeExposureParams p = {});
// UVtools FileFormat.GetTransitionStepTime: (long - short) / (count + 1) rounded to 2 decimals (away from zero).
double             transition_step_time(double long_exposure_s, double short_exposure_s, size_t layer_count);
std::string        validate_fade_exposure(const FadeExposureParams &, size_t table_size);
FadeExposureResult fade_exposure_time(std::vector<LayerParams> &table, const FadeExposureParams &, const LayerProgress &progress = {});

// ---- Light bleed compensation -----------------------------------------------------------------------
enum class LightBleedSubject { Similarities, Bridges, Both };
enum class LightBleedLookup { Previous, Next, Both };
struct LightBleedParams {
    LayerSelection    selection;
    LightBleedSubject subject = LightBleedSubject::Similarities;
    LightBleedLookup  lookup  = LightBleedLookup::Next;
    std::string       dim_by  = "25,15,10,5"; // comma separated; entries 0 and 255 and garbage are ignored
};
std::vector<uint8_t> parse_dim_by(const std::string &);
std::string          validate_light_bleed(const LightBleedParams &);
void                 light_bleed_compensation_layers(std::vector<Mask> &layers, const LightBleedParams &, const LayerProgress &progress = {});

// ---- Phased exposure --------------------------------------------------------------------------------
struct PhasedExposureLevel { double bottom_exposure_s = 0; unsigned bottom_iterations = 0; double exposure_s = 0; unsigned iterations = 0; };
struct PhasedExposureParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    std::vector<PhasedExposureLevel> phases;      // at least two
    bool     difference_only = true;              // later phases print only what earlier phases did not cure
    unsigned difference_overlap_iterations = 10;  // px
    bool     different_settings_for_sequential_layers = true;
    bool     sequential_lift_enabled = true;      double sequential_lift_mm = 0.1;
    bool     sequential_wait_before_cure_enabled = true; double sequential_wait_before_cure_s = 0;
};
// UVtools defaults: {bottom, 10px, normal, 4px} and {normal, 0, normal, 0}; sequential lift 0.1 / wait 0
// (gcode printers: 0 / 2).
PhasedExposureParams phased_exposure_defaults(const LayerParams &first_bottom, const LayerParams &first_normal, bool is_gcode);
std::string          validate_phased_exposure(const PhasedExposureParams &, const std::vector<double> &z_mm);
struct PhasedExposureResult { LayerListResult list; size_t bottom_layer_count = 0; };
// Per-layer part of Phased exposure: the pieces layer `li` becomes. `original` = the piece's pixels are the input
// layer itself (`mask` is then left empty).
struct PhasedExposurePiece { Mask mask; bool original; double z; LayerParams params; size_t source; };
std::vector<PhasedExposurePiece> phased_exposure_layer(const Mask &layer, size_t li, double z, const LayerParams &layer_params,
                                                       const PhasedExposureParams &, const PrinterContext &);
PhasedExposureResult phased_exposure_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                            const PhasedExposureParams &, const PrinterContext &,
                                            const LayerProgress &progress = {});

// ---- Raise platform on print finish -----------------------------------------------------------------
struct RaiseOnPrintFinishParams {
    double position_z_mm = 0;      // 0 -> machine Z of the context
    double wait_time_s   = 1200;
    bool   output_dummy_pixel = true;
};
std::string validate_raise_on_print_finish(const RaiseOnPrintFinishParams &, const std::vector<double> &z_mm, const PrinterContext &);
// The appended dummy layer: blank, or with one dim pixel at (x, y).
struct RaiseDummyLayer { size_t width = 0, height = 0; bool dummy_pixel = false; int x = -1, y = -1; uint8_t brightness = 128; Mask make() const; };
// List/parameter part (no pixels): appends Z and parameter row. `last_layer_first_pixel` reports the first non-zero
// pixel of the last layer, `model_center_of` the centre of the model bounds; both are only called when needed.
RaiseDummyLayer raise_on_print_finish_plan(size_t layer_count, size_t width, size_t height, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                           const RaiseOnPrintFinishParams &, const PrinterContext &,
                                           const std::function<void(size_t &, size_t &, bool &)> &last_layer_first_pixel,
                                           const std::function<void(int &, int &)> &model_center_of);
// Appends the dummy layer. Returns nothing: the new layer is the last element of every list.
void        raise_on_print_finish(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                  const RaiseOnPrintFinishParams &, const PrinterContext &);

// ---- Edit parameters --------------------------------------------------------------------------------
// Every engaged field is written; disengaged fields are left alone.
struct LayerParamEdit {
    std::optional<double>  position_z_mm;
    std::optional<double>  exposure_s, wait_before_cure_s, wait_after_cure_s, wait_after_lift_s;
    std::optional<double>  lift_height_mm, lift_speed, lift_height2_mm, lift_speed2;
    std::optional<double>  retract_speed, retract_height2_mm, retract_speed2;
    std::optional<uint8_t> light_pwm;
    bool any() const;
};
struct EditParametersParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    bool   per_layer_override = false;   // false: global edit, bottom layers use `bottom`, all others `edit`
    unsigned set_layers = 1, skip_layers = 0; // per layer mode: edit N layers, skip M, repeat
    LayerParamEdit edit;                 // per layer mode value set / global normal values
    LayerParamEdit bottom;               // global mode bottom values
};
std::string validate_edit_parameters(const EditParametersParams &);
void        edit_parameters(std::vector<double> &z_mm, std::vector<LayerParams> &table, const EditParametersParams &,
                            const LayerProgress &progress = {});

// ---- Timelapse --------------------------------------------------------------------------------------
enum class TimelapseMode { LiftHeight, VirtualLayer };
struct TimelapseParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    TimelapseMode mode = TimelapseMode::LiftHeight;
    double raise_position_z_mm = 0;   // 0 -> machine Z
    bool   output_dummy_pixel = true;
    double raise_each_nth_height_mm = 1;
    double wait_after_lift_s = 1;
    double exposure_s = 0;            // photo layer exposure (virtual layer); 0 -> 0.05 (0 for gcode printers)
    bool   ensure_last_layer = true;
    bool   use_custom_lift = false;
    double slow_lift_height_mm = 3, lift_speed = 0, lift_speed2 = 0;
    double slow_retract_height_mm = 3, retract_speed = 0, retract_speed2 = 0;
};
// Number of raises the settings produce (UVtools NumberOfLifts).
size_t      timelapse_lift_count(const TimelapseParams &, const std::vector<double> &z_mm, const PrinterContext &);
std::string validate_timelapse(const TimelapseParams &, const std::vector<double> &z_mm, const PrinterContext &);
struct TimelapseResult { LayerListResult list; std::vector<bool> inserted; }; // inserted[i]: virtual layer
// The blank (or one-dim-pixel) layer of every inserted virtual layer.
struct TimelapseVirtualLayer { size_t width = 0, height = 0; bool dummy_pixel = false; int x = 0, y = 0; uint8_t brightness = 128; Mask make() const; };
// List/parameter part (no pixels). `model_center_of` is called only when a dummy pixel is output.
TimelapseResult timelapse_plan(size_t layer_count, size_t width, size_t height, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                               const TimelapseParams &, const PrinterContext &, const std::function<void(int &, int &)> &model_center_of,
                               TimelapseVirtualLayer &virtual_layer, const LayerProgress &progress = {});
TimelapseResult timelapse_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                 const TimelapseParams &, const PrinterContext &, const LayerProgress &progress = {});

// ---- Dynamic layer height ---------------------------------------------------------------------------
enum class ExposureSetType { Linear, Multiplier, Manual };
struct ExposureItem { double layer_height_mm = 0, bottom_exposure_s = 0, exposure_s = 0; };
struct DynamicLayerHeightParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    double minimum_layer_height_mm = 0.03, maximum_layer_height_mm = 0.10;
    bool   strip_anti_aliasing = false, reconstruct_anti_aliasing = false;
    unsigned maximum_erodes = 10;
    ExposureSetType exposure_type = ExposureSetType::Linear;
    bool   iterate_bottom_exposure = false;
    double bottom_exposure_s = 0, exposure_s = 0;
    double bottom_exposure_step_s = 0.5, exposure_step_s = 0.2;
    std::vector<ExposureItem> manual_table;
};
// UVtools InitWithSlicerFile: min >= file layer height, max >= 2 * layer height (capped at 0.2), unset exposures from the table.
DynamicLayerHeightParams dynamic_layer_height_defaults(const std::vector<LayerParams> &table, const PrinterContext &, DynamicLayerHeightParams p = {});
// Exposure per layer height for Linear / Multiplier (UVtools RebuildAutoExposureTable); Manual returns the manual table.
std::vector<ExposureItem> dynamic_layer_height_exposure_table(const DynamicLayerHeightParams &, double file_layer_height_mm);
struct DynamicLayerHeightReport {
    size_t old_layer_count = 0, new_layer_count = 0, stacked_layers = 0;
    double maximum_layer_height_mm = 0, old_print_time_s = 0, new_print_time_s = 0;
    size_t reused_layers() const { return old_layer_count - stacked_layers; }
    double compression_ratio_percent() const { return new_layer_count ? std::round(double(old_layer_count) / double(new_layer_count) * 10000.0) / 100.0 : 0; }
};
std::string validate_dynamic_layer_height(const DynamicLayerHeightParams &, const std::vector<double> &z_mm, const PrinterContext &);
struct DynamicLayerHeightResult { LayerListResult list; DynamicLayerHeightReport report; };
DynamicLayerHeightResult dynamic_layer_height_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                                     const DynamicLayerHeightParams &, const PrinterContext &,
                                                     const LayerProgress &progress = {});
// Same algorithm over a layer source: `load(i)` returns original layer i (called about once per layer, in ascending
// order), `emit(mask, head, pristine)` receives every OUTPUT layer in order; pristine = the mask is exactly layer
// `head` unchanged (the caller may then reference its payload instead of storing a new one). z_mm / table are updated
// to the output list. dynamic_layer_height_layers is a wrapper around this.
using DynamicLayerHeightEmit = std::function<void(Mask &&, size_t head, bool pristine)>;
DynamicLayerHeightResult dynamic_layer_height_plan(size_t layer_count, std::vector<double> &z_mm, std::vector<LayerParams> &table,
                                                   const DynamicLayerHeightParams &, const PrinterContext &,
                                                   const std::function<Mask(size_t)> &load, const DynamicLayerHeightEmit &emit,
                                                   const LayerProgress &progress = {});

// ---- Lithophane -------------------------------------------------------------------------------------
enum class LithophaneBase { None, Square, Model };
enum class RotateDirection { None, Rotate90Clockwise, Rotate180, Rotate90CounterClockwise };
struct LithophaneParams {
    double   layer_height_mm = 0.05;
    size_t   bottom_layer_count = 4;
    double   bottom_exposure_s = 30, normal_exposure_s = 2;
    RotateDirection rotate = RotateDirection::None;
    bool     mirror = false, invert_color = false;
    double   resize_factor_percent = 100;
    bool     enhance_contrast = false;
    int      brightness_gain = 0;          // -127 .. 127
    unsigned gap_closing_iterations = 0, remove_noise_iterations = 0, gaussian_blur = 0;
    bool     separate_grayscale_pixels = true;
    unsigned start_threshold = 1, end_threshold = 255;
    double   base_thickness_mm = 2;
    LithophaneBase base_type = LithophaneBase::Square;
    unsigned base_margin_px = 80;
    double   lithophane_height_mm = 3;
    bool     one_layer_per_threshold = false;
    bool     anti_aliasing = true;
};
std::string validate_lithophane(const LithophaneParams &, const Mask &image, size_t resolution_x, size_t resolution_y);
// The picture after crop / resize / equalise / blur / morphology / rotate / mirror (UVtools GetTargetMat).
Mask        lithophane_target_image(const Mask &image, const LithophaneParams &, const PrinterContext &);
// Replaces the document content with the lithophane stack on a resolution_x * resolution_y canvas. The table gets
// one entry per layer (bottom / normal exposure); z is layer_height * (index + 1).
// Same result as lithophane_layers without repeating identical layers: layer i is distinct[sequence[i]].
void        lithophane_stack(const Mask &image, size_t resolution_x, size_t resolution_y, const LithophaneParams &,
                             const PrinterContext &, std::vector<Mask> &distinct, std::vector<size_t> &sequence,
                             std::vector<double> &z_mm, std::vector<LayerParams> &table, const LayerProgress &progress = {});
void        lithophane_layers(const Mask &image, size_t resolution_x, size_t resolution_y, const LithophaneParams &,
                              const PrinterContext &, std::vector<Mask> &layers, std::vector<double> &z_mm,
                              std::vector<LayerParams> &table, const LayerProgress &progress = {});

// ---- Repair layers ----------------------------------------------------------------------------------
struct RepairLayersParams {
    size_t first_layer = 0, last_layer = kLastLayer;
    bool   repair_islands = true, repair_resin_traps = true, repair_suction_cups = false, remove_empty_layers = true;
    unsigned remove_islands_below_equal_pixels = 5;
    unsigned remove_islands_recursive_iterations = 4; // 0 = until nothing is left, 1 = single pass
    unsigned attach_islands_below_layers = 2;
    unsigned resin_traps_overlap_by = 5;
    unsigned suction_cups_vent_hole = 16;             // diameter px
    unsigned gap_closing_iterations = 1, noise_removal_iterations = 0;
    // Island / resin trap / empty layer thresholds. Every other detector is switched off by repair_layers.
    // Defaults are the UVtools detection defaults. required_pixels_to_support_multiplier and
    // required_pixel_brightness_to_support of the island config also drive "attach islands below".
    IssueDetectionConfig detection;
};
struct RepairLayersResult {
    LayerListResult list;                // source map (identity unless empty layers were removed)
    size_t islands_removed = 0, islands_attached = 0, resin_traps_filled = 0, suction_cups_drilled = 0, empty_layers_removed = 0;
};
std::string        validate_repair_layers(const RepairLayersParams &);
RepairLayersResult repair_layers(std::vector<Mask> &layers, std::vector<double> &z_mm, const RepairLayersParams &,
                                 const LayerProgress &progress = {});

// The repair as a sequence of passes over a layer source instead of a vector of decoded layers: detection streams
// through LayerIssues, the edits of islands are kept as run-length records per layer (zeroed / attached pixels), the
// per-layer step (single pass island removal, resin traps, gap closing, noise removal) maps layer by layer, the suction
// cup drills are kept as circles per layer on a small LRU of decoded layers and the empty layer removal only needs the
// lit pixel count of each layer. repair_layers is this engine over vectors, so both give the same result.
struct RepairRef { size_t index = 0; bool emitted = false; }; // layer `index` of the source, or computed layer `index`
struct RepairIO {
    size_t layer_count = 0, width = 0, height = 0;
    std::function<Mask(size_t)> load;                  // original layer (thread safe)
    // Stores a computed layer; `source` = the original layer whose payload is reused when the pixels are identical;
    // `scratch` = an intermediate result that is read back (load_ref) and stored again, so it must not be quantized.
    std::function<RepairRef(Mask &&, std::optional<size_t> source, bool scratch)> store; // thread safe
    std::function<Mask(RepairRef)> load_ref;           // an original or stored layer (thread safe)
    // fn(i) for i in [first, last] in parallel chunks; `buffers` = decoded layers one task holds at once.
    std::function<void(size_t first, size_t last, size_t buffers, const std::function<void(size_t)> &fn)> parallel;
};
// `refs` receives one reference per OUTPUT layer; `z_mm` is the Z of the input and, afterwards, the output layers.
RepairLayersResult repair_layers_plan(const RepairIO &, std::vector<double> &z_mm, const RepairLayersParams &,
                                      std::vector<RepairRef> &refs, const LayerProgress &progress = {});


// ---- Calculator -------------------------------------------------------------------------------------
struct PixelsPerMillimeter { double x = 0, y = 0; };
// Pixels = Resolution / Display * Millimeters. Zero display size gives 0.
PixelsPerMillimeter pixels_per_millimeter(size_t resolution_x, size_t resolution_y, double display_width_mm, double display_height_mm);
struct PixelsResult { double x = 0, y = 0; };
PixelsResult        mm_to_pixels(size_t resolution_x, size_t resolution_y, double display_width_mm, double display_height_mm, double millimeters);
// Light-off delay = lift / (lift speed / 60) + lift / (retract speed / 60) + wait, rounded to 2 decimals.
double light_off_delay_seconds(double lift_height, double lift_speed, double retract_speed, double extra_wait_s = 0);
// Two stage variant (UVtools float overload).
double light_off_delay_seconds(double lift_height, double lift_speed, double retract_speed, double extra_wait_s,
                               double lift_height2, double lift_speed2, double retract_height2, double retract_speed2);
double light_off_delay_seconds(const LayerParams &, double extra_wait_s);
double lift_only_seconds(const LayerParams &, double extra_wait_s = 0);
// Angle = atan(layer height / xy resolution) in degrees, xy resolution = max(display / resolution) in mm, 3 decimals.
double xy_resolution_mm(size_t resolution_x, size_t resolution_y, double display_width_mm, double display_height_mm);
double optimal_model_tilt_degrees(size_t resolution_x, size_t resolution_y, double display_width_mm, double display_height_mm, double layer_height_mm);
// Seconds for one layer / the whole table: waits + exposure + motor time.
double layer_print_time_s(const LayerParams &);
double estimate_print_time_s(const std::vector<LayerParams> &);

// ---- Public pixel helpers (used by LayerExports and tests) -----------------------------------------
Mask orient_mask(const Mask &, std::optional<FlipDirection> flip, RotateDirection rotate); // flip first, then rotate (UVtools order)
Mask resize_mask_bilinear(const Mask &, size_t new_width, size_t new_height);              // cv::resize INTER_LINEAR
Mask erode_mask(const Mask &, int iterations);   // 3x3 rectangle kernel, `iterations` times
Mask dilate_mask(const Mask &, int iterations);
Mask crop_mask(const Mask &, const Rect &);      // rect must lie inside the mask

// ---- Per-layer kernels (streaming) ------------------------------------------------------------------
// Every operation whose result for layer i only depends on layer i, a bounded window of neighbour layers and a few
// document-wide numbers (model bounds, a mask found by a scan) is expressed as a StreamKernel. The batch functions
// above (move_layers, light_bleed_compensation_layers, ...) are thin wrappers that run the SAME kernel over a vector
// (run_kernel), and LayerDocument::apply_mask_kernel runs it layer by layer straight from the encoded document, so
// both paths are byte-identical by construction and the streamed one never holds more than a small window decoded.
//
// A kernel sees the ORIGINAL data of its neighbours (the result of layer i never feeds layer i+1), like the batch
// functions, which commit only at the end.
using LayerFetch = std::function<std::shared_ptr<const Mask>(size_t layer)>;
// Raised by a kernel factory when the operation, with these parameters, needs global state; callers fall back to batch.
struct NotStreamable : std::runtime_error { using std::runtime_error::runtime_error; };

// What a kernel factory may ask about the whole document. `scan` and `fetch` may decode layers on demand.
struct StreamLayout {
    size_t              layer_count = 0, width = 0, height = 0;
    std::vector<double> z_mm;           // top Z per layer, empty when the operation does not use it
    LayerFetch          fetch;          // random access to an original layer (outside the window: one decode per call)
    // Visits every original layer once (any order, visitor called concurrently from several threads).
    std::function<void(const std::function<void(size_t, const Mask &)> &)> scan;
};
StreamLayout batch_layout(const std::vector<Mask> &layers, const std::vector<double> *z_mm = nullptr);
Rect         scan_model_bounds(const StreamLayout &);   // union of the non-zero bounds of all layers

struct StreamKernel {
    size_t first = 0, last = 0;         // inclusive, already clamped to the layer count; other layers stay as they are
    size_t before = 0, after = 0;       // neighbour layers the kernel reads through `fetch` on each side of layer i
    // When not empty, `fetch(j)` returns neighbour layer j cropped to this rectangle (a Mask of exactly that size)
    // instead of the whole layer, so a wide window only keeps the part of every layer the kernel looks at.
    Rect fetch_crop;
    // Result for layer i (first <= i <= last). `fetch(j)` is valid for i - before <= j <= i + after (a window kernel may
    // not look further); kernels that read arbitrary layers set `random_access`.
    std::function<Mask(size_t layer, const Mask &in, const LayerFetch &fetch)> run;
    bool random_access = false;
    // Optional: layers of [first, last] for which this returns false are left alone without being decoded.
    std::function<bool(size_t layer)> applies;
};
// Batch path of a kernel: results are committed only when every layer finished (cancel leaves `layers` untouched).
void run_kernel(std::vector<Mask> &layers, const StreamKernel &kernel, const LayerProgress &progress = {});

// Factories validate like the matching batch function (std::invalid_argument with the same text).
StreamKernel move_kernel(const MoveParams &, const StreamLayout &);
StreamKernel resize_kernel(const ResizeParams &, const StreamLayout &);
StreamKernel rotate_kernel(const RotateParams &, const StreamLayout &);
StreamKernel flip_kernel(const FlipParams &, const StreamLayout &);
StreamKernel pattern_kernel(const PatternParams &, const StreamLayout &);
StreamKernel solidify_kernel(const SolidifyParams &, const StreamLayout &);
StreamKernel threshold_kernel(const ThresholdParams &, const StreamLayout &);
StreamKernel pixel_dimming_kernel(const PixelDimmingParams &, const StreamLayout &);
StreamKernel pixel_arithmetic_kernel(const PixelArithmeticParams &, const StreamLayout &);
StreamKernel light_bleed_kernel(const LightBleedParams &, const StreamLayout &);
// Needs layout.z_mm. Window = floor/ceiling thickness in layers (can be large; the document refuses a window that does
// not fit the decoded-pixel budget).
StreamKernel infill_kernel(const InfillParams &, const StreamLayout &);
// nullopt when no raft or support layers were found (nothing to change). Scans at most 1000 layers through `fetch`.
std::optional<StreamKernel> raft_relief_kernel(const RaftReliefParams &, const StreamLayout &);
// Throws NotStreamable when a group reads a layer that an earlier group writes or two groups write the same layer.
StreamKernel layer_arithmetic_kernel(const LayerArithmeticParams &, const StreamLayout &);

} // namespace Slic3r::sla::layerops
