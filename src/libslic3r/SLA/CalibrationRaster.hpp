#ifndef SLIC3R_SLA_CALIBRATION_RASTER_HPP
#define SLIC3R_SLA_CALIBRATION_RASTER_HPP
#include "libslic3r/SLAPrint.hpp"
namespace Slic3r::sla {
// Native patterns, independent of a mesh and of the target file container.
// These are the initial calibration set; UVtools' other patterns remain
// separate.
enum class CalibrationPattern { XYZBlocks, GrayscalePatches, LiftArea };
struct CalibrationParameters {
  CalibrationPattern pattern = CalibrationPattern::XYZBlocks;
  double size_x = 10., size_y = 10., height = 1., margin = 2.;
  unsigned grayscale_steps = 16;
  uint8_t grayscale_first = 32, grayscale_last = 255;
};
std::unique_ptr<SLAPrint>
create_calibration(const DynamicPrintConfig &configuration,
                   const CalibrationParameters &parameters);
} // namespace Slic3r::sla
#endif
