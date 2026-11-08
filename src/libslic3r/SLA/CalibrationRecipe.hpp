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
} // namespace Slic3r::sla
