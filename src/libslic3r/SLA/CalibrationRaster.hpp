#ifndef SLIC3R_SLA_CALIBRATION_RASTER_HPP
#define SLIC3R_SLA_CALIBRATION_RASTER_HPP
#include "libslic3r/SLAPrint.hpp"
#include "CalibrationRecipe.hpp"
#include <functional>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace Slic3r::sla {
// Native patterns, independent of a mesh and of the target file container.
// These are the initial calibration set; UVtools' other patterns remain
// separate.
// Describe the actual generated source palette before raster allocation. The
// GUI and generator share this plan so the displayed dose levels cannot drift.
struct CalibrationGrayscalePlan {
  LayerMaskPrecision precision = LayerMaskPrecision::Gray8;
  std::vector<uint8_t> requested, effective;
  size_t distinct_levels = 0, changed_levels = 0;
  bool has_duplicates() const { return distinct_levels != effective.size(); }
};
CalibrationGrayscalePlan calibration_grayscale_plan(
    const DynamicPrintConfig &configuration, const CalibrationParameters &parameters);
// Total height includes the constant base and one layer per decreasing size.
// Factors scale both sides of the rectangle, not its area.
struct CalibrationLiftPlan {
  size_t base_layers = 0;
  std::vector<unsigned> factors;
};
CalibrationLiftPlan calibration_lift_plan(
    const DynamicPrintConfig &configuration, const CalibrationParameters &parameters);
std::unique_ptr<SLAPrint>
create_calibration(const DynamicPrintConfig &configuration,
                   const CalibrationParameters &parameters);

// ---------------------------------------------------------------------------
// UVtools-derived pixel-domain calibrations. Geometry is in raster pixels (as
// in UVtools) so sizes scale with the printer's pixel pitch; exposure, layer
// height and motion come from the active presets. Mirroring follows the
// printer profile. The callbacks may throw to cancel and are invoked once per
// generated mask.
using CalibrationCheck = std::function<void()>;

// Elephant foot: one test part per strategy (original / eroded bottom layer /
// dimmed bottom wall). Bottom layers carry the modified part and the following
// normal layers the original, so the part whose bottom is flush shows the best
// compensation.
struct ElephantFootParameters {
  unsigned bottom_layers = 0, normal_layers = 0;  // 0 = 1.0 mm / 3.5 mm of layers
  double part_scale = 1.;                         // x; rounded to 0.01
  unsigned margin_px = 30;                        // gap between parts
  bool extrude_text = true;                       // labels raised above the part
  double text_height_mm = 1.;
  bool antialiasing = true;
  bool output_original = true;
  bool erode = true;
  unsigned erode_start = 2, erode_end = 6, erode_step = 1;  // 3x3 erode iterations
  bool dimming = true;
  unsigned dimming_wall_px = 20;                  // wall eroded by this many px
  unsigned dimming_start = 140, dimming_end = 200, dimming_step = 20;  // 0-255 wall gray
};
struct ElephantFootCell {
  enum class Kind { Original, Erode, Dimming } kind = Kind::Original;
  int x_px = 0, y_px = 0;  // top-left corner before the final centering move
  unsigned value = 0;      // erode iterations or wall brightness
  unsigned number = 1;     // label number, 1-based
};
struct ElephantFootPlan {
  unsigned bottom_layers = 0, normal_layers = 0, text_layers = 0, total_layers = 0;
  int shape_width_px = 0, shape_height_px = 0;
  size_t requested_objects = 0;  // before the screen-fit cut-off
  std::vector<ElephantFootCell> cells;  // parts that really fit
};
ElephantFootPlan elephant_foot_plan(const DynamicPrintConfig &configuration,
                                    const ElephantFootParameters &parameters);
std::unique_ptr<SLAPrint> create_elephant_foot_calibration(
    const DynamicPrintConfig &configuration, const ElephantFootParameters &parameters,
    const CalibrationCheck &check = {});

// Blooming effect: squares cured at the same Z with an increasing wait time
// before cure; compare their final sizes. Needs a format with per-layer
// repeated-height exposures and a per-layer wait / light-off field.
struct BloomingParameters {
  unsigned bottom_layers = 3;
  unsigned left_right_margin_px = 200, top_bottom_margin_px = 200;
  double wait_start_s = 0., wait_increment_s = 1.;
  unsigned object_count = 15;  // squares = object_count + 1, as in UVtools
  unsigned object_diameter_px = 400, object_margin_px = 20;
  double object_height_mm = 5.;
};
struct BloomingObject { int x_px, y_px; double wait_s; };
struct BloomingPlan {
  std::vector<BloomingObject> objects;  // squares that fit the screen
  size_t requested_objects = 0;
  size_t object_layers = 0, text_layers = 0, total_layers = 0;
};
BloomingPlan blooming_plan(const DynamicPrintConfig &configuration, const BloomingParameters &parameters);
std::unique_ptr<SLAPrint> create_blooming_calibration(
    const DynamicPrintConfig &configuration, const BloomingParameters &parameters,
    const CalibrationCheck &check = {});

// Stress tower: a cylinder with spirals of small circles winding around it.
enum class StressTowerSpiral { Clockwise, Alternate, Both };
struct StressTowerParameters {
  double base_diameter_mm = 30., base_height_mm = 3., body_height_mm = 50., ceil_height_mm = 3.;
  unsigned chamfer_layers = 6;
  bool antialiasing = true;
  unsigned spirals = 2;
  double spiral_diameter_mm = 2.;
  StressTowerSpiral spiral_direction = StressTowerSpiral::Both;
  double spiral_angle_step_deg = 1.;  // per layer
};
struct StressTowerPlan {
  unsigned base_layers = 0, body_layers = 0, ceil_layers = 0, total_layers = 0;
  int base_radius_px = 0, spiral_radius_px = 0;
};
StressTowerPlan stress_tower_plan(const DynamicPrintConfig &configuration, const StressTowerParameters &parameters);
std::unique_ptr<SLAPrint> create_stress_tower_calibration(
    const DynamicPrintConfig &configuration, const StressTowerParameters &parameters,
    const CalibrationCheck &check = {});

// ---------------------------------------------------------------------------
// XYZ accuracy (UVtools OperationCalibrateXYZAccuracy): up to nine blocks at the
// corners / edges / centre, optionally hollow with a drain slot and a centre
// relief hole, each labelled with its position and nominal size. The classic
// "XYZ blocks" pattern above stays as the quick solid variant.
struct XYZAccuracyParameters {
  unsigned bottom_layers = 0;  // 0 = material preset
  unsigned top_bottom_margin_px = 100, left_right_margin_px = 100;
  double x_mm = 15., y_mm = 15., z_mm = 15.;
  bool center_hole_relief = true, hollow = true;
  double wall_thickness_mm = 3., drain_hole_mm = 3.;
  bool antialiasing = false;
  // Row-major: top L C R, middle L C R, bottom L C R. Default = centre only.
  std::array<bool, 9> objects{false, false, false, false, true, false, false, false, false};
};
struct XYZAccuracyPlan {
  unsigned total_layers = 0, bottom_layers = 0, vent_layers = 0;
  int block_x_px = 0, block_y_px = 0, wall_x_px = 0, wall_y_px = 0;
  double real_x_mm = 0., real_y_mm = 0., real_z_mm = 0.;  // after rounding down to whole pixels / layers
  unsigned object_count = 0;
};
XYZAccuracyPlan xyz_accuracy_plan(const DynamicPrintConfig &configuration, const XYZAccuracyParameters &parameters);
std::unique_ptr<SLAPrint> create_xyz_accuracy_calibration(
    const DynamicPrintConfig &configuration, const XYZAccuracyParameters &parameters,
    const CalibrationCheck &check = {});

// ---------------------------------------------------------------------------
// Tolerance (UVtools OperationCalibrateTolerance): a female ring with a hole and
// male parts whose size steps around the hole size; the part that fits shows
// the printer's real XY tolerance.
enum class ToleranceShape { Circle, Square };
struct ToleranceParameters {
  unsigned bottom_layers = 0;  // 0 = material preset
  double z_mm = 10.;
  unsigned top_bottom_margin_px = 100, left_right_margin_px = 100;
  unsigned chamfer_layers = 4, erode_bottom_iterations = 4;
  ToleranceShape shape = ToleranceShape::Circle;
  unsigned part_margin_px = 50;
  bool output_same_diameter_part = true;
  bool fuse_parts = false;  // each male part sits inside its own female ring
  bool antialiasing = true;
  double female_diameter_mm = 16., female_hole_diameter_mm = 10.;
  unsigned thinner_models = 5;
  double thinner_offset_mm = 0., thinner_step_mm = -.1;
  unsigned thicker_models = 0;
  double thicker_offset_mm = 0., thicker_step_mm = .1;
};
struct TolerancePart {
  int x_px = 0, y_px = 0, width_px = 0, height_px = 0;
  double step_mm = 0.;      // offset from the hole size, printed as the label
  double size_mm = 0.;      // nominal male size (hole + step)
};
struct TolerancePlan {
  unsigned total_layers = 0, bottom_layers = 0;
  int female_x_px = 0, female_y_px = 0, hole_x_px = 0, hole_y_px = 0;
  size_t requested_objects = 0;
  std::vector<TolerancePart> parts;  // male parts that fit the screen
};
TolerancePlan tolerance_plan(const DynamicPrintConfig &configuration, const ToleranceParameters &parameters);
std::unique_ptr<SLAPrint> create_tolerance_calibration(
    const DynamicPrintConfig &configuration, const ToleranceParameters &parameters,
    const CalibrationCheck &check = {});

// ---------------------------------------------------------------------------
// Lift height (UVtools OperationCalibrateLiftHeight): one large block, optionally
// followed by shrinking layers, printed with explicit lift/retract motion so the
// user can find the lowest lift that still peels the layer. Unset motion and
// exposure values come from the active material preset.
struct LiftHeightParameters {
  unsigned bottom_layers = 3, normal_layers = 2;
  unsigned left_right_margin_px = 200, top_bottom_margin_px = 200;
  bool decrease_image = true;
  unsigned decrease_factor_pct = 10, minimum_factor_pct = 10;
  std::optional<double> bottom_exposure_s, normal_exposure_s;
  std::optional<double> bottom_lift_height_mm, lift_height_mm;
  std::optional<double> bottom_lift_speed_mm_min, lift_speed_mm_min, retract_speed_mm_min;
};
struct LiftHeightPlan {
  unsigned bottom_layers = 0, normal_layers = 0, decrease_layers = 0, total_layers = 0;
  double total_height_mm = 0.;
  int block_x_px = 0, block_y_px = 0, block_width_px = 0, block_height_px = 0;
  std::vector<unsigned> factors_pct;  // sizes of the shrinking layers
};
LiftHeightPlan lift_height_plan(const DynamicPrintConfig &configuration, const LiftHeightParameters &parameters);
std::unique_ptr<SLAPrint> create_lift_height_calibration(
    const DynamicPrintConfig &configuration, const LiftHeightParameters &parameters,
    const CalibrationCheck &check = {});

// ---------------------------------------------------------------------------
// External tests (UVtools "External tests" tab). UVtools only opens web pages;
// offline we keep the reference list as text (names, purpose, address) and tell
// which native generator replaces each test where one exists.
struct ExternalCalibrationTest {
  std::string name, purpose, address, native_alternative;
};
const std::vector<ExternalCalibrationTest> &external_calibration_tests();
} // namespace Slic3r::sla
#endif
