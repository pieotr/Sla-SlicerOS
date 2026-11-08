#include "CalibrationRaster.hpp"
#include "ExposureFinderTable.hpp"
#include "AGGRaster.hpp"
#include "PrinterRaster.hpp"
#include "GeneratedLayerOwnership.hpp"
#include "ProjectedPNGMaskEncoder.hpp"
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <cmath>
namespace Slic3r::sla {
void CalibrationRecipe::validate() const {
  const auto &p = parameters;
  if (int(p.pattern) < 0 || int(p.pattern) > 3 || int(precision) < 0 || int(precision) > 18)
    throw RuntimeError("Invalid calibration recipe pattern or palette");
  if ((p.pattern == CalibrationPattern::ExposureFinder) != bool(exposure_finder))
    throw RuntimeError("ExposureFinder recipe requires matching typed table provenance");
  if (exposure_finder) {
    if (p.lift_taper_step || p.lift_taper_min != 10)
      throw RuntimeError("ExposureFinder recipe cannot contain lift taper settings");
    exposure_finder->validate();
  }
  for (double value : {p.size_x, p.size_y, p.height, p.margin})
    if (!std::isfinite(value) || value < 0 || value > 100000.)
      throw RuntimeError("Invalid calibration recipe dimension");
  if (p.size_x == 0 || p.size_y == 0 || p.height == 0)
    throw RuntimeError("Calibration recipe dimensions must be positive");
  if (p.lift_taper_step > 99 || p.lift_taper_min < 1 || p.lift_taper_min > 99 ||
      (p.pattern != CalibrationPattern::LiftArea && p.lift_taper_step != 0) ||
      (p.lift_taper_step != 0 && 100 - p.lift_taper_step < p.lift_taper_min))
    throw RuntimeError("Invalid lift calibration size range");
  if (p.pattern != CalibrationPattern::GrayscalePatches) {
    if (!requested.empty() || !effective.empty())
      throw RuntimeError("Non-grayscale calibration recipe contains dose levels");
    return;
  }
  if (p.grayscale_steps < 2 || p.grayscale_steps > 256 ||
      p.grayscale_first >= p.grayscale_last || requested.size() != p.grayscale_steps ||
      effective.size() != requested.size())
    throw RuntimeError("Invalid calibration recipe grayscale range");
  std::array<bool, 256> seen{};
  for (unsigned i = 0; i < p.grayscale_steps; ++i) {
    const auto value = uint8_t(std::lround(p.grayscale_first +
        double(p.grayscale_last - p.grayscale_first) * i / (p.grayscale_steps - 1)));
    if (requested[i] != value || effective[i] != project_mask_value(value, precision) || seen[effective[i]])
      throw RuntimeError("Invalid calibration recipe requested/effective levels");
    seen[effective[i]] = true;
  }
}
std::shared_ptr<const CalibrationRecipe> CalibrationRecipe::clone() const {
  auto copy = std::make_shared<CalibrationRecipe>(*this);
  if (exposure_finder)
    copy->exposure_finder = std::make_shared<const ExposureFinderRecipe>(*exposure_finder);
  return copy;
}
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
size_t calibration_layer_count(const DynamicPrintConfig &config, double height) {
  const double layer_height = config.opt_float("layer_height");
  if (!std::isfinite(layer_height) || layer_height <= 0. || !std::isfinite(height) || height <= 0.)
    throw RuntimeError("Invalid calibration layer height or test height");
  const double count = std::round(height / layer_height);
  if (!std::isfinite(count) || count < 1. || count > 100000. ||
      std::abs(count * layer_height - height) > 1e-6)
    throw RuntimeError("Calibration height must be an exact multiple of layer height (maximum 100000 layers)");
  return size_t(count);
}
} // namespace
CalibrationLiftPlan calibration_lift_plan(
    const DynamicPrintConfig &configuration, const CalibrationParameters &parameters) {
  CalibrationRecipe recipe;
  recipe.parameters = parameters;
  recipe.validate();
  if (parameters.pattern != CalibrationPattern::LiftArea)
    throw RuntimeError("Lift size planning requires the Lift area pattern");
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{}); config.apply(configuration);
  const size_t count = calibration_layer_count(config, parameters.height);
  CalibrationLiftPlan plan;
  if (parameters.lift_taper_step) {
    // Signed subtraction avoids unsigned wraparound at the last factor.
    for (int factor = 100 - int(parameters.lift_taper_step);
         factor >= int(parameters.lift_taper_min); factor -= int(parameters.lift_taper_step))
      plan.factors.push_back(unsigned(factor));
    const int bottom = config.opt_int("sla_bottom_layers");
    if (bottom < 0 || count <= plan.factors.size() || count - plan.factors.size() < size_t(bottom))
      throw RuntimeError("Increase test height: decreasing sizes must follow all bottom layers and at least one full-size layer");
  }
  plan.base_layers = count - plan.factors.size();
  return plan;
}
CalibrationGrayscalePlan calibration_grayscale_plan(
    const DynamicPrintConfig &configuration, const CalibrationParameters &parameters) {
  if (parameters.grayscale_steps < 2 || parameters.grayscale_steps > 256 ||
      parameters.grayscale_first >= parameters.grayscale_last)
    throw RuntimeError("Grayscale calibration requires 2–256 ordered values");
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{}); config.apply(configuration);
  CalibrationGrayscalePlan plan;
  const auto format = config.opt_string("sla_archive_format");
  if(const auto precision=independent_generated_mask_precision(format,config.opt_string("sla_anycubic_version")))
    plan.precision=*precision;
  else if (generated_layer_ownership(format, FreshLayerProducer::Calibration,
                                config.opt_string("sla_anycubic_version")))
    plan.precision = generated_mask_precision(format, config.opt_int("sla_cbddlp_aa"),
                                             config.opt_string("sla_anycubic_version"));
  std::array<bool, 256> seen{};
  for (unsigned i = 0; i < parameters.grayscale_steps; ++i) {
    const auto requested = uint8_t(std::lround(parameters.grayscale_first +
        double(parameters.grayscale_last - parameters.grayscale_first) * i /
            (parameters.grayscale_steps - 1)));
    const auto effective = project_mask_value(requested, plan.precision);
    plan.requested.push_back(requested); plan.effective.push_back(effective);
    plan.changed_levels += requested != effective;
    if (!seen[effective]) { seen[effective] = true; ++plan.distinct_levels; }
  }
  return plan;
}
std::unique_ptr<SLAPrint>
create_calibration(const DynamicPrintConfig &configuration,
                   const CalibrationParameters &parameters) {
  if (parameters.pattern == CalibrationPattern::ExposureFinder)
    throw RuntimeError("ExposureFinder requires its dedicated generator; the basic pattern generator cannot create it");
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{});
  config.apply(configuration);
  if (config.option("printer_technology")->getInt() != ptSLA)
    throw RuntimeError("Calibration requires an SLA printer");
  CalibrationLiftPlan lift;
  if (parameters.pattern == CalibrationPattern::LiftArea)
    lift = calibration_lift_plan(config, parameters);
  else if (parameters.lift_taper_step)
    throw RuntimeError("Decreasing sizes require the Lift area pattern");
  CalibrationGrayscalePlan grayscale;
  if (parameters.pattern == CalibrationPattern::GrayscalePatches) {
    grayscale = calibration_grayscale_plan(config, parameters);
    if (grayscale.has_duplicates())
      throw RuntimeError("Grayscale calibration levels repeat in the generated palette; reduce steps or widen the grayscale range");
  }
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
  const size_t count = calibration_layer_count(config, parameters.height);
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
        raster.color(grayscale.effective[i]);
      raster.draw(rectangle(origin_x + (i % cols) * (sx + margin),
                            origin_y + (i / cols) * (sy + margin), sx, sy));
    }
  }
  // Keep the canonical source PNG (including AA edges) equal to its native
  // PW0 decode. Palette support does not manufacture an owned parameter writer.
  const auto independent_precision=independent_generated_mask_precision(
      config.opt_string("sla_archive_format"),config.opt_string("sla_anycubic_version"));
  const RasterEncoder encoder=independent_precision
      ? RasterEncoder(ProjectedPNGMaskEncoder{*independent_precision,{}})
      : RasterEncoder(PNGRasterEncoder{});
  const auto encoded = raster.encode(encoder);
  if (encoded.size() == 0 ||
      uint64_t(encoded.size()) * count > 1024ULL * 1024 * 1024)
    throw RuntimeError("Calibration exceeds the 1 GiB encoded-mask budget");
  std::vector<EncodedRaster> masks(lift.factors.empty() ? count : lift.base_layers, encoded);
  uint64_t mask_bytes = uint64_t(encoded.size()) * masks.size();
  for (unsigned factor : lift.factors) {
    ColoredRaster reduced{resolution, pitch, transform};
    const double reduced_x = sx * factor / 100., reduced_y = sy * factor / 100.;
    reduced.draw(rectangle((width - reduced_x) * .5, (height - reduced_y) * .5, reduced_x, reduced_y));
    auto mask = reduced.encode(encoder);
    mask_bytes += mask.size();
    if (mask.size() == 0 || mask_bytes > 1024ULL * 1024 * 1024)
      throw RuntimeError("Calibration exceeds the 1 GiB encoded-mask budget");
    masks.push_back(std::move(mask));
  }
  std::vector<double> positions;
  positions.reserve(count);
  for (size_t i = 0; i < count; ++i)
    positions.push_back((i + 1) * layer_height);
  config.set("gamma_correction", 1.);
  config.set("initial_layer_height", layer_height);
  config.set("sla_transition_layers", 0);
  auto job = SLAPrint::create_raster_job(
      config, independent_precision ? LayerDocument::from_precision_rasters(std::move(masks),*independent_precision) :
          LayerDocument::from_rasters(std::move(masks)), positions,
      generated_layer_ownership(config.opt_string("sla_archive_format"),
                                FreshLayerProducer::Calibration,
                                config.option("sla_anycubic_version")?config.opt_string("sla_anycubic_version"):"auto"));
  CalibrationRecipe recipe;
  recipe.parameters = parameters;
  if (const auto &fresh = job->layer_document()->fresh_job()) recipe.precision = fresh->mask_precision;
  else if(independent_precision)recipe.precision=*independent_precision;
  recipe.requested = grayscale.requested; recipe.effective = grayscale.effective;
  job->set_calibration_recipe(std::make_shared<const CalibrationRecipe>(std::move(recipe)));
  return job;
}
} // namespace Slic3r::sla
