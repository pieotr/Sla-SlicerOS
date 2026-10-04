#include "CalibrationRaster.hpp"
#include "AGGRaster.hpp"
#include "PrinterRaster.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>
namespace Slic3r::sla {
namespace {
class ColoredRaster final : public RasterGrayscaleAA {
public:
  ColoredRaster(Resolution resolution, PixelDim pitch, const Trafo &transform)
      : RasterGrayscaleAA(resolution, pitch, transform, agg::gamma_none()) {}
  void color(uint8_t value) {
    m_renderer.color(_RasterGrayscaleAA::TColor{value});
  }
};
ExPolygon rectangle(double x, double y, double width, double height) {
  return ExPolygon{
      Polygon{Points{Point::new_scale(x, y), Point::new_scale(x + width, y),
                     Point::new_scale(x + width, y + height),
                     Point::new_scale(x, y + height)}}};
}
} // namespace
std::unique_ptr<SLAPrint>
create_calibration(const DynamicPrintConfig &configuration,
                   const CalibrationParameters &parameters) {
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{});
  config.apply(configuration);
  if (config.option("printer_technology")->getInt() != ptSLA)
    throw RuntimeError("Calibration requires an SLA printer");
  SLAPrinterConfig printer;
  printer.apply(config, true);
  // Validate resolution/memory limits with the same contract as mesh/PCB masks.
  auto reference = create_printer_raster(printer);
  const auto transform = reference->trafo();
  reference.reset();
  for (double value : {parameters.size_x, parameters.size_y, parameters.height,
                       parameters.margin})
    if (!std::isfinite(value) || value < 0. || value > 100000.)
      throw RuntimeError("Invalid calibration dimension");
  const double width = printer.display_width.value,
               height = printer.display_height.value;
  const double sx = parameters.size_x, sy = parameters.size_y,
               margin = parameters.margin;
  if (sx <= 0. || sy <= 0. || parameters.height <= 0. ||
      parameters.height > printer.max_print_height.value)
    throw RuntimeError(
        "Calibration sizes and height must be positive and fit the printer");
  const double layer_height = config.opt_float("layer_height");
  if (!std::isfinite(layer_height) || layer_height <= 0.)
    throw RuntimeError("Invalid calibration layer height");
  const double rounded_count = std::round(parameters.height / layer_height);
  if (rounded_count < 1. || rounded_count > 100000. ||
      std::abs(rounded_count * layer_height - parameters.height) > 1e-6)
    throw RuntimeError("Calibration height must be an exact multiple of layer "
                       "height (maximum 100000 layers)");
  const size_t count = size_t(rounded_count);
  Resolution resolution{size_t(printer.display_pixels_x.value),
                        size_t(printer.display_pixels_y.value)};
  const PixelDim pitch{width / resolution.width_px,
                       height / resolution.height_px};
  if (printer.display_orientation.value == sladoPortrait)
    std::swap(resolution.width_px, resolution.height_px);
  ColoredRaster raster{resolution, pitch, transform};
  unsigned cols = 1, rows = 1;
  if (parameters.pattern == CalibrationPattern::XYZBlocks) {
    cols = rows = 3;
  } else if (parameters.pattern == CalibrationPattern::GrayscalePatches) {
    if (parameters.grayscale_steps < 2 || parameters.grayscale_steps > 256 ||
        parameters.grayscale_first >= parameters.grayscale_last)
      throw RuntimeError("Grayscale calibration requires 2–256 ordered values");
    cols = unsigned(std::ceil(std::sqrt(double(parameters.grayscale_steps))));
    rows = (parameters.grayscale_steps + cols - 1) / cols;
  }
  if (cols * sx + (cols + 1) * margin > width ||
      rows * sy + (rows + 1) * margin > height)
    throw RuntimeError("Calibration patterns do not fit the selected LCD. "
                       "Reduce their size or margin.");
  if (parameters.pattern == CalibrationPattern::XYZBlocks) {
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned col = 0; col < 3; ++col) {
        const double x = col == 0   ? margin
                         : col == 1 ? (width - sx) * .5
                                    : width - margin - sx;
        const double y = row == 0   ? margin
                         : row == 1 ? (height - sy) * .5
                                    : height - margin - sy;
        raster.draw(rectangle(x, y, sx, sy));
      }
  } else {
    const double origin_x = (width - cols * sx - (cols - 1) * margin) * .5;
    const double origin_y = (height - rows * sy - (rows - 1) * margin) * .5;
    const unsigned patterns =
        parameters.pattern == CalibrationPattern::GrayscalePatches
            ? parameters.grayscale_steps
            : 1;
    for (unsigned i = 0; i < patterns; ++i) {
      if (patterns > 1)
        raster.color(uint8_t(std::lround(
            parameters.grayscale_first +
            double(parameters.grayscale_last - parameters.grayscale_first) * i /
                (patterns - 1))));
      raster.draw(rectangle(origin_x + (i % cols) * (sx + margin),
                            origin_y + (i / cols) * (sy + margin), sx, sy));
    }
  }
  const auto encoded = raster.encode(PNGRasterEncoder{});
  if (encoded.size() == 0 ||
      uint64_t(encoded.size()) * count > 1024ULL * 1024 * 1024)
    throw RuntimeError("Calibration exceeds the 1 GiB encoded-mask budget");
  std::vector<EncodedRaster> masks(count, encoded);
  std::vector<double> positions;
  positions.reserve(count);
  for (size_t i = 0; i < count; ++i)
    positions.push_back((i + 1) * layer_height);
  config.set("gamma_correction", 1.);
  config.set("initial_layer_height", layer_height);
  config.set("sla_transition_layers", 0);
  return SLAPrint::create_raster_job(
      config, LayerDocument::from_rasters(std::move(masks)), positions);
}
} // namespace Slic3r::sla
