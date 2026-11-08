#ifndef SLIC3R_SLA_CALIBRATION_RASTER_HPP
#define SLIC3R_SLA_CALIBRATION_RASTER_HPP
#include "libslic3r/SLAPrint.hpp"
#include "CalibrationRecipe.hpp"
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
} // namespace Slic3r::sla
#endif
