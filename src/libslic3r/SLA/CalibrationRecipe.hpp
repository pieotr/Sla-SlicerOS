// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "LayerMaskPrecision.hpp"
#include "libslic3r/Exception.hpp"
#include <memory>
#include <array>
#include <cmath>
#include <vector>

namespace Slic3r::sla {
struct ExposureFinderRecipe;
enum class CalibrationPattern { XYZBlocks, GrayscalePatches, LiftArea, ExposureFinder };
struct CalibrationParameters {
  CalibrationPattern pattern = CalibrationPattern::XYZBlocks;
  double size_x = 10., size_y = 10., height = 1., margin = 2.;
  unsigned grayscale_steps = 16;
  uint8_t grayscale_first = 32, grayscale_last = 255;
  unsigned lift_taper_step = 0, lift_taper_min = 10;
};
// Immutable provenance of generation. Later mask edits do not change the
// recipe and must never be discarded by automatically regenerating it.
struct CalibrationRecipe {
  CalibrationParameters parameters;
  LayerMaskPrecision precision = LayerMaskPrecision::Gray8;
  std::vector<uint8_t> requested, effective;
  std::shared_ptr<const ExposureFinderRecipe> exposure_finder;
  void validate() const;
  // Freeze nested data as well as the outer provenance object.
  std::shared_ptr<const CalibrationRecipe> clone() const;
};

// Conversions used by the "apply result to profile" step. They are pure so the
// dialog only picks the value and shows old -> new.
//
// A grayscale patch cured with the preset exposure at gray `level` received
// level/255 of the full-white dose. Assuming dose ~ gray x time, the same dose
// at full white needs exposure * level / 255 seconds. This is the cure
// threshold, not a recommendation: the user may edit the result before applying.
inline double grayscale_equivalent_exposure_s(double exposure_s, unsigned level) {
  if (!std::isfinite(exposure_s) || exposure_s <= 0. || level == 0 || level > 255)
    throw RuntimeError("Grayscale level must be 1-255 and exposure positive");
  return exposure_s * double(level) / 255.;
}
// A 3x3 erode removes one pixel per iteration on every side, so N iterations
// shrink the footprint by N pixels; the compensation is that width in mm
// (mean of both axes for non-square pixels).
inline double elephant_foot_compensation_mm(unsigned erode_iterations, double pitch_x_mm, double pitch_y_mm) {
  if (!std::isfinite(pitch_x_mm) || !std::isfinite(pitch_y_mm) || pitch_x_mm <= 0. || pitch_y_mm <= 0.)
    throw RuntimeError("Invalid pixel pitch");
  return double(erode_iterations) * (pitch_x_mm + pitch_y_mm) * .5;
}
} // namespace Slic3r::sla
